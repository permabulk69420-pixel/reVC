#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>

#include "common.h"
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "main.h"
#include "Camera.h"
#include "Draw.h"
#include "Renderer.h"
#include "JavaWrapper.h"
#include "QuestOpenXR.h"
#ifdef EXTENDED_PIPELINES
#include "custompipes.h"
#endif

extern JavaVM* javaVM;
extern CJavaWrapper* g_pJavaWrapper;
extern char* StorageRootBuffer;

namespace {

const char* const kTag = "reVC-XR";

FILE* gBridgeLog = NULL;
bool gCaptureReady = false;
bool gXrAttempted = false;
bool gXrReady = false;
bool gBridgeFatal = false;
bool gFirstMonoFrameLogged = false;
bool gFirstStereoFrameLogged = false;

GLuint gCaptureFramebuffer = 0;
GLuint gCaptureColorTexture = 0;
GLuint gCaptureDepthStencil = 0;
int gCaptureWidth = 0;
int gCaptureHeight = 0;

QuestOpenXR::EyeView gEyes[2];
uint32_t gEyeCount = 0;
uint32_t gCurrentEye = 0;
bool gStereoFrameActive = false;
bool gStereoShouldRender = false;
bool gReplayingRightEye = false;

CMatrix gSavedCameraMatrix;
RwV2d gSavedViewWindow;
RwV2d gSavedViewOffset;
float gSavedFov = 70.0f;
bool gCameraBaseSaved = false;

enum class StartMode {
    None,
    Plain,
    Horizon,
};

struct CapturedRenderPass {
    bool captureStarted;
    StartMode startMode;
    int16 startArgs[7];
    bool scene;
    bool debug;
    bool effects;
    bool twoD;
    bool menus;
    bool fade;
    bool afterFade;

    CapturedRenderPass()
        : captureStarted(false), startMode(StartMode::None), scene(false),
          debug(false), effects(false), twoD(false), menus(false),
          fade(false), afterFade(false) {
        memset(startArgs, 0, sizeof(startArgs));
    }
};

CapturedRenderPass gPass;

void OpenBridgeLog() {
    if (gBridgeLog != NULL) {
        return;
    }
    const char* root = getenv("STORAGE_ROOT");
    if (root == NULL || root[0] == '\0') {
        return;
    }
    std::string gameRoot(root);
    while (!gameRoot.empty() && gameRoot[gameRoot.size() - 1] == '/') {
        gameRoot.erase(gameRoot.size() - 1);
    }
    const std::string userFiles = gameRoot + "/userfiles";
    if (mkdir(userFiles.c_str(), 0775) != 0 && errno != EEXIST) {
        return;
    }
    gBridgeLog = fopen((userFiles + "/xr_log.txt").c_str(), "a");
}

void BridgeLog(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    __android_log_write(ANDROID_LOG_INFO, kTag, text);
    OpenBridgeLog();
    if (gBridgeLog != NULL) {
        fprintf(gBridgeLog, "HANDOFF %s\n", text);
        fflush(gBridgeLog);
    }
}

void CloseBridgeLog() {
    if (gBridgeLog != NULL) {
        fclose(gBridgeLog);
        gBridgeLog = NULL;
    }
}

void ResetCapturedPass(StartMode mode) {
    gPass.captureStarted = true;
    gPass.startMode = mode;
    memset(gPass.startArgs, 0, sizeof(gPass.startArgs));
    gPass.scene = false;
    gPass.debug = false;
    gPass.effects = false;
    gPass.twoD = false;
    gPass.menus = false;
    gPass.fade = false;
    gPass.afterFade = false;
}

void FinishCapturedPass() {
    gPass.captureStarted = false;
    gPass.startMode = StartMode::None;
}

void ForgetCaptureTarget() {
    gCaptureFramebuffer = 0;
    gCaptureColorTexture = 0;
    gCaptureDepthStencil = 0;
    gCaptureWidth = 0;
    gCaptureHeight = 0;
    gCaptureReady = false;
}

void DestroyCaptureTarget() {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        if (gCaptureFramebuffer != 0) {
            glDeleteFramebuffers(1, &gCaptureFramebuffer);
        }
        if (gCaptureDepthStencil != 0) {
            glDeleteRenderbuffers(1, &gCaptureDepthStencil);
        }
        if (gCaptureColorTexture != 0) {
            glDeleteTextures(1, &gCaptureColorTexture);
        }
    }
    ForgetCaptureTarget();
}

void DesiredCaptureSize(RwCamera* camera, int* width, int* height) {
    *width = 0;
    *height = 0;
    if (camera == nil) {
        return;
    }
    RwRaster* raster = RwCameraGetRaster(camera);
    if (raster == nil) {
        return;
    }

    const int rasterWidth = RwRasterGetWidth(raster);
    const int rasterHeight = RwRasterGetHeight(raster);
    if (rasterWidth <= 0 || rasterHeight <= 0) {
        return;
    }

    if (gStereoFrameActive && gStereoShouldRender &&
        gCurrentEye < gEyeCount && gEyes[gCurrentEye].valid &&
        gEyes[gCurrentEye].width > 0 && gEyes[gCurrentEye].height > 0) {
        *width = gEyes[gCurrentEye].width;
        *height = gEyes[gCurrentEye].height;
        return;
    }

    const int recommendedWidth = QuestOpenXR::RecommendedWidth();
    const int recommendedHeight = QuestOpenXR::RecommendedHeight();
    if (gXrReady && recommendedWidth > 0 && recommendedHeight > 0) {
        const float aspect = static_cast<float>(rasterWidth) /
                             static_cast<float>(rasterHeight);
        int targetWidth = recommendedWidth;
        int targetHeight = static_cast<int>(lroundf(
                static_cast<float>(targetWidth) / aspect));
        if (targetHeight > recommendedHeight) {
            targetHeight = recommendedHeight;
            targetWidth = static_cast<int>(lroundf(
                    static_cast<float>(targetHeight) * aspect));
        }
        *width = targetWidth;
        *height = targetHeight;
        return;
    }

    *width = rasterWidth;
    *height = rasterHeight;
}

bool CreateCaptureTarget(RwCamera* camera, int requestedWidth,
                         int requestedHeight) {
    if (camera == nil || requestedWidth <= 0 || requestedHeight <= 0) {
        BridgeLog("capture target creation failed: camera=%p size=%dx%d",
                  camera, requestedWidth, requestedHeight);
        return false;
    }

    if (gCaptureReady && gCaptureWidth == requestedWidth &&
        gCaptureHeight == requestedHeight) {
        return true;
    }

    DestroyCaptureTarget();

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousRenderbuffer = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);

    while (glGetError() != GL_NO_ERROR) {
    }

    glGenTextures(1, &gCaptureColorTexture);
    glBindTexture(GL_TEXTURE_2D, gCaptureColorTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 requestedWidth, requestedHeight, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenRenderbuffers(1, &gCaptureDepthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, gCaptureDepthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          requestedWidth, requestedHeight);

    glGenFramebuffers(1, &gCaptureFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, gCaptureFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, gCaptureColorTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, gCaptureDepthStencil);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum error = glGetError();

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                      static_cast<GLuint>(previousDrawFramebuffer));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,
                      static_cast<GLuint>(previousReadFramebuffer));
    glBindRenderbuffer(GL_RENDERBUFFER,
                       static_cast<GLuint>(previousRenderbuffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (gCaptureFramebuffer == 0 || gCaptureColorTexture == 0 ||
        gCaptureDepthStencil == 0 || status != GL_FRAMEBUFFER_COMPLETE ||
        error != GL_NO_ERROR) {
        BridgeLog("capture target creation failed: fbo=%u status=0x%x GL=0x%x size=%dx%d",
                  gCaptureFramebuffer, status, error,
                  requestedWidth, requestedHeight);
        DestroyCaptureTarget();
        return false;
    }

    gCaptureWidth = requestedWidth;
    gCaptureHeight = requestedHeight;
    gCaptureReady = true;
    BridgeLog("native capture FBO=%u size=%dx%d created without replacing the RenderWare camera raster",
              gCaptureFramebuffer, gCaptureWidth, gCaptureHeight);
    return true;
}

bool BindCaptureTarget(RwCamera* camera) {
    int desiredWidth = 0;
    int desiredHeight = 0;
    DesiredCaptureSize(camera, &desiredWidth, &desiredHeight);
    if (!CreateCaptureTarget(camera, desiredWidth, desiredHeight)) {
        return false;
    }

    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depthMask = GL_TRUE;
    GLint stencilMask = -1;
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);

    while (glGetError() != GL_NO_ERROR) {
    }

    glBindFramebuffer(GL_FRAMEBUFFER, gCaptureFramebuffer);
    glViewport(0, 0, gCaptureWidth, gCaptureHeight);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFFFFFFFFu);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glDepthMask(depthMask);
    glStencilMask(static_cast<GLuint>(stencilMask));
    glScissor(0, 0, gCaptureWidth, gCaptureHeight);
    if (scissorEnabled == GL_TRUE) {
        glEnable(GL_SCISSOR_TEST);
    }

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        BridgeLog("binding capture target failed: GL=0x%x", error);
        return false;
    }
    return true;
}

QuestOpenXR::FrameResult SubmitCaptureTargetMono() {
    if (!gCaptureReady || gCaptureFramebuffer == 0 ||
        gCaptureWidth <= 0 || gCaptureHeight <= 0) {
        return QuestOpenXR::FrameResult::FatalError;
    }
    return QuestOpenXR::SubmitGameFrame(
            gCaptureFramebuffer, gCaptureWidth, gCaptureHeight);
}

bool EnsureOpenXrStarted() {
    if (gXrReady) {
        return true;
    }
    if (gXrAttempted || gBridgeFatal || !gCaptureReady) {
        return false;
    }

    gXrAttempted = true;
    BridgeLog("starting OpenXR after the normal SDL camera rendered into the independent capture FBO");

    if (!QuestOpenXR::Initialize()) {
        BridgeLog("OpenXR existing-context initialization failed");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return false;
    }
    if (!QuestOpenXR::AwaitSessionReady(5000)) {
        BridgeLog("OpenXR session did not reach READY after the SDL renderer handoff");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return false;
    }

    gXrReady = true;
    BridgeLog("OpenXR adopted the SDL EGL context; runtime recommends %dx%d per eye",
              QuestOpenXR::RecommendedWidth(),
              QuestOpenXR::RecommendedHeight());
    return true;
}

struct Float3 {
    float x;
    float y;
    float z;
};

Float3 Cross(const Float3& a, const Float3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

Float3 RotateByQuaternion(const float qInput[4], const Float3& value) {
    float qx = qInput[0];
    float qy = qInput[1];
    float qz = qInput[2];
    float qw = qInput[3];
    const float length = sqrtf(qx*qx + qy*qy + qz*qz + qw*qw);
    if (length > 0.00001f) {
        qx /= length;
        qy /= length;
        qz /= length;
        qw /= length;
    } else {
        qx = qy = qz = 0.0f;
        qw = 1.0f;
    }

    const Float3 q = {qx, qy, qz};
    Float3 t = Cross(q, value);
    t.x *= 2.0f;
    t.y *= 2.0f;
    t.z *= 2.0f;
    const Float3 qCrossT = Cross(q, t);
    return {
        value.x + qw * t.x + qCrossT.x,
        value.y + qw * t.y + qCrossT.y,
        value.z + qw * t.z + qCrossT.z
    };
}

CVector MapXrVectorToGame(const Float3& xr,
                         const CVector& baseRight,
                         const CVector& baseUp,
                         const CVector& baseForward) {
    return baseRight * xr.x + baseUp * xr.y - baseForward * xr.z;
}

void PushGameCameraToRenderWare() {
    RwCamera* camera = Scene.camera;
    if (camera == nil) {
        return;
    }
    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame == nil) {
        return;
    }
    RwMatrix* matrix = RwFrameGetMatrix(frame);
    *RwMatrixGetPos(matrix) = TheCamera.GetPosition();
    *RwMatrixGetAt(matrix) = TheCamera.GetForward();
    *RwMatrixGetUp(matrix) = TheCamera.GetUp();
    *RwMatrixGetRight(matrix) = TheCamera.GetRight();
    RwMatrixUpdate(matrix);
    RwFrameUpdateObjects(frame);
    RwFrameOrthoNormalize(frame);
    TheCamera.m_vecGameCamPos = TheCamera.GetPosition();
}

void SaveCameraBase() {
    if (gCameraBaseSaved || Scene.camera == nil) {
        return;
    }
    gSavedCameraMatrix = TheCamera.GetMatrix();
    gSavedFov = CDraw::GetFOV();
    gSavedViewWindow = *RwCameraGetViewWindow(Scene.camera);
    gSavedViewOffset = *RwCameraGetViewOffset(Scene.camera);
    gCameraBaseSaved = true;
}

void RestoreCameraBase() {
    if (!gCameraBaseSaved || Scene.camera == nil) {
        return;
    }
    TheCamera.GetMatrix() = gSavedCameraMatrix;
    CDraw::SetFOV(gSavedFov);
    TheCamera.CalculateDerivedValues();
    PushGameCameraToRenderWare();
    RwCameraSetViewWindow(Scene.camera, &gSavedViewWindow);
    RwCameraSetViewOffset(Scene.camera, &gSavedViewOffset);
}

void ReleaseCameraBase() {
    RestoreCameraBase();
    gCameraBaseSaved = false;
}

void ApplyEyeCamera(uint32_t eyeIndex) {
    if (!gCameraBaseSaved || eyeIndex >= gEyeCount ||
        !gEyes[eyeIndex].valid || Scene.camera == nil) {
        return;
    }

    TheCamera.GetMatrix() = gSavedCameraMatrix;

    CVector baseForward = gSavedCameraMatrix.GetForward();
    CVector baseUp = gSavedCameraMatrix.GetUp();
    baseForward.Normalise();
    baseUp.Normalise();
    CVector baseRight = CrossProduct(baseForward, baseUp);
    baseRight.Normalise();

    const QuestOpenXR::EyeView& eye = gEyes[eyeIndex];
    const Float3 xrForward = RotateByQuaternion(
            eye.orientation, {0.0f, 0.0f, -1.0f});
    const Float3 xrUp = RotateByQuaternion(
            eye.orientation, {0.0f, 1.0f, 0.0f});

    CVector forward = MapXrVectorToGame(
            xrForward, baseRight, baseUp, baseForward);
    CVector up = MapXrVectorToGame(
            xrUp, baseRight, baseUp, baseForward);
    forward.Normalise();
    CVector physicalRight = CrossProduct(forward, up);
    physicalRight.Normalise();
    up = CrossProduct(physicalRight, forward);
    up.Normalise();

    TheCamera.GetMatrix().GetForward() = forward;
    TheCamera.GetMatrix().GetUp() = up;
    TheCamera.GetMatrix().GetRight() = CrossProduct(up, forward);

    const Float3 xrPosition = {
        eye.position[0], eye.position[1], eye.position[2]
    };
    TheCamera.GetMatrix().GetPosition() =
            gSavedCameraMatrix.GetPosition() +
            MapXrVectorToGame(xrPosition, baseRight, baseUp, baseForward);

    const float halfHorizontal = fmaxf(
            fabsf(eye.angleLeft), fabsf(eye.angleRight));
    if (halfHorizontal > 0.01f) {
        CDraw::SetFOV(halfHorizontal * 2.0f * 180.0f / PI);
    }
    TheCamera.CalculateDerivedValues();
    PushGameCameraToRenderWare();
}

void ApplyEyeProjection(RwCamera* camera, uint32_t eyeIndex) {
    if (camera == nil || eyeIndex >= gEyeCount ||
        !gEyes[eyeIndex].valid) {
        return;
    }
    const QuestOpenXR::EyeView& eye = gEyes[eyeIndex];
    const float tanLeft = tanf(eye.angleLeft);
    const float tanRight = tanf(eye.angleRight);
    const float tanUp = tanf(eye.angleUp);
    const float tanDown = tanf(eye.angleDown);
    RwV2d viewWindow = {
        0.5f * (tanRight - tanLeft),
        0.5f * (tanUp - tanDown)
    };
    RwV2d viewOffset = {
        0.5f * (tanRight + tanLeft),
        0.5f * (tanUp + tanDown)
    };
    if (viewWindow.x > 0.01f && viewWindow.y > 0.01f) {
        RwCameraSetViewWindow(camera, &viewWindow);
        RwCameraSetViewOffset(camera, &viewOffset);
    }
    PushGameCameraToRenderWare();
}

void ResetStereoBridgeState() {
    ReleaseCameraBase();
    gEyeCount = 0;
    gCurrentEye = 0;
    gStereoFrameActive = false;
    gStereoShouldRender = false;
    gReplayingRightEye = false;
    FinishCapturedPass();
}

bool BeginLeftEyeBeforeCulling() {
    if (!gXrReady || !QuestOpenXR::OwnsPresentation() ||
        gStereoFrameActive || gReplayingRightEye) {
        return false;
    }

    gEyeCount = 0;
    const QuestOpenXR::StereoFrameResult frame =
            QuestOpenXR::BeginStereoFrame(gEyes, 2, &gEyeCount);
    if (frame == QuestOpenXR::StereoFrameResult::ExitRequested ||
        frame == QuestOpenXR::StereoFrameResult::FatalError) {
        BridgeLog("OpenXR stereo frame begin requested reVC shutdown");
        RsGlobal.quit = TRUE;
        return false;
    }
    if (frame == QuestOpenXR::StereoFrameResult::WaitingForSession ||
        frame == QuestOpenXR::StereoFrameResult::Inactive) {
        return false;
    }

    gStereoFrameActive = true;
    gStereoShouldRender =
            frame == QuestOpenXR::StereoFrameResult::Render;
    gCurrentEye = 0;
    ResetCapturedPass(StartMode::None);

    if (gStereoShouldRender && gEyeCount >= 2) {
        SaveCameraBase();
        ApplyEyeCamera(0);
        if (!gFirstStereoFrameLogged) {
            BridgeLog("stereo render begins before ConstructRenderList: eyes=%u native=%dx%d; direct OpenXR orientation mapping active",
                      gEyeCount, gEyes[0].width, gEyes[0].height);
        }
    }
    return true;
}

bool SubmitCurrentEye(uint32_t eye) {
    if (!gCaptureReady || gCaptureFramebuffer == 0 ||
        gCaptureWidth <= 0 || gCaptureHeight <= 0) {
        return false;
    }
    return QuestOpenXR::SubmitStereoEye(
            eye, gCaptureFramebuffer, gCaptureWidth, gCaptureHeight);
}

} // namespace

extern "C" bool __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
        int16, int16, int16, int16, int16, int16, int16);
extern "C" bool __real__Z21DoRWStuffStartOfFramesssssss(
        int16, int16, int16, int16, int16, int16, int16);
extern "C" void __real__Z19DoRWStuffEndOfFramev();
extern "C" void __real__Z11RenderScenev();
extern "C" void __real__Z15RenderDebugShitv();
extern "C" void __real__Z13RenderEffectsv();
extern "C" void __real__Z13Render2dStuffv();
extern "C" void __real__Z11RenderMenusv();
extern "C" void __real__Z6DoFadev();
extern "C" void __real__Z22Render2dStuffAfterFadev();
extern "C" void __real__ZN9CRenderer19ConstructRenderListEv();
extern "C" void __real__ZN9CRenderer9PreRenderEv();

extern "C" void __wrap__ZN9CRenderer19ConstructRenderListEv() {
    if (!gReplayingRightEye) {
        BeginLeftEyeBeforeCulling();
    }
    __real__ZN9CRenderer19ConstructRenderListEv();
}

extern "C" void __wrap__ZN9CRenderer9PreRenderEv() {
    __real__ZN9CRenderer9PreRenderEv();
}

extern "C" bool __wrap__Z29DoRWStuffStartOfFrame_Horizonsssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    if (gStereoFrameActive && !gReplayingRightEye) {
        ResetCapturedPass(StartMode::Horizon);
        const int16 values[7] = {a, b, c, d, e, f, g};
        memcpy(gPass.startArgs, values, sizeof(values));
    }
    return __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
            a, b, c, d, e, f, g);
}

extern "C" bool __wrap__Z21DoRWStuffStartOfFramesssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    if (gStereoFrameActive && !gReplayingRightEye) {
        ResetCapturedPass(StartMode::Plain);
        const int16 values[7] = {a, b, c, d, e, f, g};
        memcpy(gPass.startArgs, values, sizeof(values));
    }
    return __real__Z21DoRWStuffStartOfFramesssssss(
            a, b, c, d, e, f, g);
}

extern "C" void __wrap__Z11RenderScenev() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.scene = true;
    __real__Z11RenderScenev();
}

extern "C" void __wrap__Z15RenderDebugShitv() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.debug = true;
    __real__Z15RenderDebugShitv();
}

extern "C" void __wrap__Z13RenderEffectsv() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.effects = true;
    __real__Z13RenderEffectsv();
}

extern "C" void __wrap__Z13Render2dStuffv() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.twoD = true;
    __real__Z13Render2dStuffv();
}

extern "C" void __wrap__Z11RenderMenusv() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.menus = true;
    __real__Z11RenderMenusv();
}

extern "C" void __wrap__Z6DoFadev() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.fade = true;
    __real__Z6DoFadev();
}

extern "C" void __wrap__Z22Render2dStuffAfterFadev() {
    if (gStereoFrameActive && !gReplayingRightEye) gPass.afterFade = true;
    __real__Z22Render2dStuffAfterFadev();
}

extern "C" RsEventStatus __real_RsEventHandler(RsEvent event, void* param);
extern "C" RwBool __real_psCameraBeginUpdate(RwCamera* camera);
extern "C" void __real_psCameraShowRaster(RwCamera* camera);

extern "C" RsEventStatus __wrap_RsEventHandler(RsEvent event, void* param) {
    if (event == rsCAMERASIZE) {
        DestroyCaptureTarget();
    }

    if (event == rsRWTERMINATE) {
        if (gStereoFrameActive) {
            QuestOpenXR::EndStereoFrame();
            ResetStereoBridgeState();
        }
        if (gXrAttempted) {
            BridgeLog("shutting OpenXR down before the normal RenderWare device closes");
            QuestOpenXR::Shutdown();
        }
        DestroyCaptureTarget();
    }

    const RsEventStatus result = __real_RsEventHandler(event, param);

    if (event == rsCAMERASIZE && result != rsEVENTERROR) {
        int width = 0;
        int height = 0;
        DesiredCaptureSize(Scene.camera, &width, &height);
        if (!CreateCaptureTarget(Scene.camera, width, height)) {
            gBridgeFatal = true;
            RsGlobal.quit = TRUE;
        }
    }

    if (event == rsRWTERMINATE && gXrAttempted) {
        QuestOpenXR::FinalizeAfterEngineShutdown();
        gXrReady = false;
    }

    if (event == rsTERMINATE) {
        BridgeLog("normal reVC platform termination completed");
        CloseBridgeLog();
    }

    return result;
}

extern "C" RwBool __wrap_psCameraBeginUpdate(RwCamera* camera) {
    if (gStereoFrameActive && gStereoShouldRender) {
        ApplyEyeProjection(camera, gCurrentEye);
    }

    const RwBool result = __real_psCameraBeginUpdate(camera);
    if (!result) {
        if (gStereoFrameActive) {
            QuestOpenXR::EndStereoFrame();
            ResetStereoBridgeState();
        }
        return result;
    }

    if (!BindCaptureTarget(camera)) {
        BridgeLog("capture FBO could not bind after the RenderWare camera begin-update");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return FALSE;
    }
    return result;
}

extern "C" void __wrap__Z19DoRWStuffEndOfFramev() {
    if (!gStereoFrameActive) {
        __real__Z19DoRWStuffEndOfFramev();
        FinishCapturedPass();
        return;
    }

    __real__Z19DoRWStuffEndOfFramev();

    bool valid = true;
    if (gStereoShouldRender) {
        valid = gEyeCount >= 2 && SubmitCurrentEye(0);
        if (valid) {
            RestoreCameraBase();
            gCurrentEye = 1;
            gReplayingRightEye = true;
            ApplyEyeCamera(1);

            __real__ZN9CRenderer19ConstructRenderListEv();
            __real__ZN9CRenderer9PreRenderEv();

            bool began = false;
            if (gPass.startMode == StartMode::Horizon) {
                began = __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
                        gPass.startArgs[0], gPass.startArgs[1],
                        gPass.startArgs[2], gPass.startArgs[3],
                        gPass.startArgs[4], gPass.startArgs[5],
                        gPass.startArgs[6]);
            } else if (gPass.startMode == StartMode::Plain) {
                began = __real__Z21DoRWStuffStartOfFramesssssss(
                        gPass.startArgs[0], gPass.startArgs[1],
                        gPass.startArgs[2], gPass.startArgs[3],
                        gPass.startArgs[4], gPass.startArgs[5],
                        gPass.startArgs[6]);
            }

            valid = began;
            if (began) {
                DefinedState();
                if (gPass.scene) __real__Z11RenderScenev();
#ifdef EXTENDED_PIPELINES
                if (gPass.scene) CustomPipes::EnvMapRender();
#endif
                if (gPass.debug) __real__Z15RenderDebugShitv();
                if (gPass.effects) __real__Z13RenderEffectsv();
                if (gPass.scene) TheCamera.RenderMotionBlur();
                if (gPass.twoD) __real__Z13Render2dStuffv();
                if (gPass.menus) __real__Z11RenderMenusv();
                if (gPass.fade) __real__Z6DoFadev();
                if (gPass.afterFade) __real__Z22Render2dStuffAfterFadev();
                RwCameraEndUpdate(Scene.camera);
                valid = SubmitCurrentEye(1);
            }
            gReplayingRightEye = false;
        }
    }

    ReleaseCameraBase();
    const QuestOpenXR::FrameResult ended = QuestOpenXR::EndStereoFrame();
    if (!valid || ended == QuestOpenXR::FrameResult::FatalError ||
        ended == QuestOpenXR::FrameResult::ExitRequested) {
        BridgeLog("native per-eye render submission failed; requesting reVC shutdown");
        RsGlobal.quit = TRUE;
    } else if (gStereoShouldRender && !gFirstStereoFrameLogged) {
        gFirstStereoFrameLogged = true;
        BridgeLog("first native stereo frame completed: left and right each culled and rendered at %dx%d before submission",
                  gEyes[0].width, gEyes[0].height);
    }
    ResetStereoBridgeState();
}

extern "C" void __wrap_psCameraShowRaster(RwCamera* camera) {
    if (gStereoFrameActive) {
        return;
    }

    if (!gCaptureReady && !gBridgeFatal) {
        int width = 0;
        int height = 0;
        DesiredCaptureSize(camera, &width, &height);
        CreateCaptureTarget(camera, width, height);
    }

    if (gBridgeFatal) {
        RsGlobal.quit = TRUE;
        return;
    }

    if (gCaptureReady) {
        if (!EnsureOpenXrStarted()) {
            if (gXrAttempted || gBridgeFatal) {
                return;
            }
        } else {
            const QuestOpenXR::FrameResult frame = SubmitCaptureTargetMono();
            if (frame == QuestOpenXR::FrameResult::ExitRequested ||
                frame == QuestOpenXR::FrameResult::FatalError) {
                BridgeLog("OpenXR mono capture submission requested reVC shutdown");
                RsGlobal.quit = TRUE;
            } else if (frame == QuestOpenXR::FrameResult::Presented &&
                       !gFirstMonoFrameLogged) {
                gFirstMonoFrameLogged = true;
                BridgeLog("first mono bootstrap submitted: FBO=%u size=%dx%d; gameplay switches to native per-eye rendering before culling",
                          gCaptureFramebuffer, gCaptureWidth, gCaptureHeight);
            }
            return;
        }
    }

    __real_psCameraShowRaster(camera);
}

extern "C" JNIEXPORT void JNICALL
Java_com_revc_game_core_REVC_initialize(JNIEnv* env, jclass, jobject activity,
                                        jstring value) {
    if (javaVM == NULL) {
        env->GetJavaVM(&javaVM);
    }

    if (g_pJavaWrapper != NULL) {
        delete g_pJavaWrapper;
    }
    g_pJavaWrapper = new CJavaWrapper(env, activity);

    const char* root = env->GetStringUTFChars(value, NULL);
    setenv("STORAGE_ROOT", root, 1);
    StorageRootBuffer = getenv("STORAGE_ROOT");
    env->ReleaseStringUTFChars(value, root);

    OpenBridgeLog();
    BridgeLog("GameActivity configured; preserving normal SDL/RenderWare startup at %s",
              StorageRootBuffer == NULL ? "<missing>" : StorageRootBuffer);
}

#endif
