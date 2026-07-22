#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>

#include "common.h"
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "main.h"
#include "JavaWrapper.h"
#include "QuestOpenXR.h"

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
bool gFirstFrameLogged = false;

GLuint gCaptureFramebuffer = 0;
GLuint gCaptureColorTexture = 0;
GLuint gCaptureDepthStencil = 0;
int gCaptureWidth = 0;
int gCaptureHeight = 0;

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

bool CreateCaptureTarget(RwCamera* camera) {
    if (camera == nil) {
        BridgeLog("mono capture target creation failed: Scene.camera is null");
        return false;
    }

    RwRaster* cameraRaster = RwCameraGetRaster(camera);
    if (cameraRaster == nil) {
        BridgeLog("mono capture target creation failed: camera raster is null");
        return false;
    }

    const int width = RwRasterGetWidth(cameraRaster);
    const int height = RwRasterGetHeight(cameraRaster);
    if (width <= 0 || height <= 0) {
        BridgeLog("mono capture target creation failed: invalid camera size=%dx%d",
                  width, height);
        return false;
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenRenderbuffers(1, &gCaptureDepthStencil);
    glBindRenderbuffer(GL_RENDERBUFFER, gCaptureDepthStencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

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
        BridgeLog("mono capture target creation failed: fbo=%u status=0x%x GL=0x%x size=%dx%d",
                  gCaptureFramebuffer, status, error, width, height);
        DestroyCaptureTarget();
        return false;
    }

    gCaptureWidth = width;
    gCaptureHeight = height;
    gCaptureReady = true;
    BridgeLog("mono capture FBO=%u size=%dx%d created without replacing the RenderWare camera raster",
              gCaptureFramebuffer, gCaptureWidth, gCaptureHeight);
    return true;
}

bool BindCaptureTarget(RwCamera* camera) {
    if (!gCaptureReady && !CreateCaptureTarget(camera)) {
        return false;
    }

    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean depthMask = GL_TRUE;
    GLint stencilMask = -1;
    GLint scissor[4] = {0, 0, gCaptureWidth, gCaptureHeight};
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilMask);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);

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
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (scissorEnabled == GL_TRUE) {
        glEnable(GL_SCISSOR_TEST);
    }

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        BridgeLog("binding mono capture target failed: GL=0x%x", error);
        return false;
    }
    return true;
}

QuestOpenXR::FrameResult SubmitCaptureTarget() {
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
    BridgeLog("starting OpenXR after a normal camera/culling pass rendered into the capture FBO");

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
    BridgeLog("OpenXR adopted the SDL EGL context; mono capture remains active with the RenderWare camera untouched");
    return true;
}

} // namespace

// These render-stage wrappers remain pass-throughs only. They are retained so
// the existing branch's link options stay compatible while the broken stereo
// replay path is completely dormant.
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

extern "C" bool __wrap__Z29DoRWStuffStartOfFrame_Horizonsssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    return __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
            a, b, c, d, e, f, g);
}

extern "C" bool __wrap__Z21DoRWStuffStartOfFramesssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    return __real__Z21DoRWStuffStartOfFramesssssss(
            a, b, c, d, e, f, g);
}

extern "C" void __wrap__Z19DoRWStuffEndOfFramev() {
    __real__Z19DoRWStuffEndOfFramev();
}

extern "C" void __wrap__Z11RenderScenev() {
    __real__Z11RenderScenev();
}

extern "C" void __wrap__Z15RenderDebugShitv() {
    __real__Z15RenderDebugShitv();
}

extern "C" void __wrap__Z13RenderEffectsv() {
    __real__Z13RenderEffectsv();
}

extern "C" void __wrap__Z13Render2dStuffv() {
    __real__Z13Render2dStuffv();
}

extern "C" void __wrap__Z11RenderMenusv() {
    __real__Z11RenderMenusv();
}

extern "C" void __wrap__Z6DoFadev() {
    __real__Z6DoFadev();
}

extern "C" void __wrap__Z22Render2dStuffAfterFadev() {
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
        if (gXrAttempted) {
            BridgeLog("shutting OpenXR down before the normal RenderWare device closes");
            QuestOpenXR::Shutdown();
        }
        DestroyCaptureTarget();
    }

    const RsEventStatus result = __real_RsEventHandler(event, param);

    if (event == rsCAMERASIZE && result != rsEVENTERROR) {
        if (!CreateCaptureTarget(Scene.camera)) {
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
    const RwBool result = __real_psCameraBeginUpdate(camera);
    if (!result) {
        return result;
    }

    if (!BindCaptureTarget(camera)) {
        BridgeLog("mono capture could not bind after the untouched camera begin-update");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return FALSE;
    }
    return result;
}

extern "C" void __wrap_psCameraShowRaster(RwCamera* camera) {
    if (!gCaptureReady && !gBridgeFatal) {
        CreateCaptureTarget(camera);
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
            const QuestOpenXR::FrameResult frame = SubmitCaptureTarget();
            if (frame == QuestOpenXR::FrameResult::ExitRequested ||
                frame == QuestOpenXR::FrameResult::FatalError) {
                BridgeLog("OpenXR mono capture submission requested reVC shutdown");
                RsGlobal.quit = TRUE;
            } else if (frame == QuestOpenXR::FrameResult::Presented &&
                       !gFirstFrameLogged) {
                gFirstFrameLogged = true;
                BridgeLog("first mono capture submitted: FBO=%u size=%dx%d; no camera transform or camera-raster replacement is active",
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
