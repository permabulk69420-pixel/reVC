#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>

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
#include "JavaWrapper.h"
#include "QuestOpenXR.h"

extern JavaVM* javaVM;
extern CJavaWrapper* g_pJavaWrapper;
extern char* StorageRootBuffer;

namespace {

const char* const kTag = "reVC-XR";

FILE* gBridgeLog = NULL;
bool gCameraTargetReady = false;
bool gXrAttempted = false;
bool gXrReady = false;
bool gBridgeFatal = false;
bool gFirstFrameLogged = false;

RwMatrix gSavedCameraMatrix;
RwV2d gSavedViewWindow;
RwV2d gSavedViewOffset;
bool gCameraBaseSaved = false;

QuestOpenXR::EyeView gEyes[2];
uint32_t gEyeCount = 0;
uint32_t gCurrentEye = 0;
bool gStereoFrameActive = false;
bool gStereoShouldRender = false;
bool gReplayingRightEye = false;

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

void SaveCameraBase(RwCamera* camera) {
    if (camera == nil || gCameraBaseSaved) {
        return;
    }
    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame == nil) {
        return;
    }
    gSavedCameraMatrix = *RwFrameGetMatrix(frame);
    gSavedViewWindow = *RwCameraGetViewWindow(camera);
    gSavedViewOffset = *RwCameraGetViewOffset(camera);
    gCameraBaseSaved = true;
}

void RestoreCameraBase(RwCamera* camera) {
    if (!gCameraBaseSaved || camera == nil) {
        return;
    }
    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame != nil) {
        *RwFrameGetMatrix(frame) = gSavedCameraMatrix;
        RwFrameUpdateObjects(frame);
    }
    RwCameraSetViewWindow(camera, &gSavedViewWindow);
    RwCameraSetViewOffset(camera, &gSavedViewOffset);
}

void ReleaseCameraBase(RwCamera* camera) {
    RestoreCameraBase(camera);
    gCameraBaseSaved = false;
}

void ApplyEyeToCamera(RwCamera* camera, uint32_t eyeIndex) {
    if (camera == nil || eyeIndex >= gEyeCount || !gEyes[eyeIndex].valid) {
        return;
    }
    SaveCameraBase(camera);
    RestoreCameraBase(camera);

    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame == nil) {
        return;
    }
    const QuestOpenXR::EyeView& eye = gEyes[eyeIndex];

    const rw::Quat orientation = rw::makeQuat(
            eye.orientation[3], eye.orientation[0],
            -eye.orientation[2], eye.orientation[1]);
    RwMatrix localRotation;
    localRotation.setIdentity();
    localRotation.rotate(orientation, rw::COMBINEREPLACE);
    RwFrameTransform(frame, &localRotation, rwCOMBINEPOSTCONCAT);

    // OpenXR is X-right, Y-up, -Z-forward. Convert metres into the game
    // camera's local right/up/forward basis. GTA world units are metres.
    const RwV3d localOffset = {
        eye.position[0], -eye.position[2], eye.position[1]
    };
    RwMatrix* matrix = RwFrameGetMatrix(frame);
    matrix->pos.x = gSavedCameraMatrix.pos.x +
            gSavedCameraMatrix.right.x * localOffset.x +
            gSavedCameraMatrix.up.x * localOffset.y +
            gSavedCameraMatrix.at.x * localOffset.z;
    matrix->pos.y = gSavedCameraMatrix.pos.y +
            gSavedCameraMatrix.right.y * localOffset.x +
            gSavedCameraMatrix.up.y * localOffset.y +
            gSavedCameraMatrix.at.y * localOffset.z;
    matrix->pos.z = gSavedCameraMatrix.pos.z +
            gSavedCameraMatrix.right.z * localOffset.x +
            gSavedCameraMatrix.up.z * localOffset.y +
            gSavedCameraMatrix.at.z * localOffset.z;
    RwFrameUpdateObjects(frame);

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
}

bool InstallOffscreenCameraTarget(RwCamera* camera) {
    if (camera == nil) {
        BridgeLog("camera target install failed: Scene.camera is null");
        return false;
    }

    RwRaster* oldColor = RwCameraGetRaster(camera);
    RwRaster* oldDepth = RwCameraGetZRaster(camera);
    if (oldColor == nil) {
        BridgeLog("camera target install failed: flat camera raster is null");
        return false;
    }

    const int width = RwRasterGetWidth(oldColor);
    const int height = RwRasterGetHeight(oldColor);
    if (width <= 0 || height <= 0) {
        BridgeLog("camera target install failed: invalid flat size=%dx%d", width, height);
        return false;
    }

    RwRaster* color = RwRasterCreate(
            width, height, 32,
            rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT8888);
    RwRaster* depth = RwRasterCreate(
            width, height, 0, rwRASTERTYPEZBUFFER);
    if (color == nil || depth == nil) {
        if (color != nil) {
            RwRasterDestroy(color);
        }
        if (depth != nil) {
            RwRasterDestroy(depth);
        }
        BridgeLog("camera target install failed: RwRasterCreate(%dx%d)", width, height);
        return false;
    }

    RwCameraSetRaster(camera, color);
    RwCameraSetZRaster(camera, depth);

    rw::gl3::Gl3Raster* native = PLUGINOFFSET(
            rw::gl3::Gl3Raster, color, rw::gl3::nativeRasterOffset);
    if (native == nil || native->fbo == 0) {
        RwCameraSetRaster(camera, oldColor);
        RwCameraSetZRaster(camera, oldDepth);
        RwRasterDestroy(color);
        RwRasterDestroy(depth);
        BridgeLog("camera target install failed: GLES FBO was not created");
        return false;
    }

    if (oldColor != nil) {
        RwRasterDestroy(oldColor);
    }
    if (oldDepth != nil) {
        RwRasterDestroy(oldDepth);
    }

    gCameraTargetReady = true;
    BridgeLog("flat RenderWare camera redirected to FBO=%u size=%dx%d before OpenXR",
              native->fbo, width, height);
    return true;
}

bool CameraTarget(RwCamera* camera, unsigned int* framebuffer,
                  int* width, int* height) {
    if (camera == nil || framebuffer == NULL || width == NULL || height == NULL) {
        return false;
    }
    RwRaster* raster = RwCameraGetRaster(camera);
    if (raster == nil) {
        return false;
    }
    rw::gl3::Gl3Raster* native = PLUGINOFFSET(
            rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
    if (native == nil || native->fbo == 0) {
        return false;
    }
    *framebuffer = native->fbo;
    *width = RwRasterGetWidth(raster);
    *height = RwRasterGetHeight(raster);
    return *width > 0 && *height > 0;
}

QuestOpenXR::FrameResult SubmitCameraLegacy(RwCamera* camera) {
    unsigned int framebuffer = 0;
    int width = 0;
    int height = 0;
    if (!CameraTarget(camera, &framebuffer, &width, &height)) {
        return QuestOpenXR::FrameResult::FatalError;
    }
    return QuestOpenXR::SubmitGameFrame(framebuffer, width, height);
}

bool SubmitCurrentEye(uint32_t eye) {
    unsigned int framebuffer = 0;
    int width = 0;
    int height = 0;
    if (!CameraTarget(Scene.camera, &framebuffer, &width, &height)) {
        return false;
    }
    return QuestOpenXR::SubmitStereoEye(eye, framebuffer, width, height);
}

bool EnsureOpenXrStarted() {
    if (gXrReady) {
        return true;
    }
    if (gXrAttempted || gBridgeFatal || !gCameraTargetReady) {
        return false;
    }

    gXrAttempted = true;
    BridgeLog("starting OpenXR only after normal SDL device, EGL context, RenderWare and camera FBO succeeded");

    if (!QuestOpenXR::Initialize()) {
        BridgeLog("OpenXR existing-context initialization failed");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return false;
    }
    if (!QuestOpenXR::AwaitSessionReady(5000)) {
        BridgeLog("OpenXR session did not reach READY after the flat renderer handoff");
        gBridgeFatal = true;
        RsGlobal.quit = TRUE;
        return false;
    }

    gXrReady = true;
    BridgeLog("OpenXR adopted the existing SDL EGL context; next frame will render per eye");
    return true;
}

bool StartRightEyePass() {
    bool began = false;
    if (gPass.startMode == StartMode::Horizon) {
        extern bool RealStartHorizon(int16, int16, int16, int16,
                                     int16, int16, int16);
        began = RealStartHorizon(
                gPass.startArgs[0], gPass.startArgs[1], gPass.startArgs[2],
                gPass.startArgs[3], gPass.startArgs[4], gPass.startArgs[5],
                gPass.startArgs[6]);
    } else if (gPass.startMode == StartMode::Plain) {
        extern bool RealStartPlain(int16, int16, int16, int16,
                                   int16, int16, int16);
        began = RealStartPlain(
                gPass.startArgs[0], gPass.startArgs[1], gPass.startArgs[2],
                gPass.startArgs[3], gPass.startArgs[4], gPass.startArgs[5],
                gPass.startArgs[6]);
    } else {
        RwRGBA black = {0, 0, 0, 255};
        RwCameraClear(Scene.camera, &black,
                      rwCAMERACLEARIMAGE | rwCAMERACLEARZ);
        began = RsCameraBeginUpdate(Scene.camera) != FALSE;
    }
    if (began) {
        DefinedState();
    }
    return began;
}

void ResetStereoBridgeState() {
    ReleaseCameraBase(Scene.camera);
    gEyeCount = 0;
    gCurrentEye = 0;
    gStereoFrameActive = false;
    gStereoShouldRender = false;
    gReplayingRightEye = false;
    FinishCapturedPass();
}

} // namespace

// Mangled free-function wrappers let the normal game render once for the left
// eye, record which render stages actually ran, and replay only those stages for
// the right eye. Game simulation, audio, timers, streaming and input still run
// exactly once in Idle().
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

bool RealStartHorizon(int16 a, int16 b, int16 c, int16 d,
                      int16 e, int16 f, int16 g) {
    return __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
            a, b, c, d, e, f, g);
}

bool RealStartPlain(int16 a, int16 b, int16 c, int16 d,
                    int16 e, int16 f, int16 g) {
    return __real__Z21DoRWStuffStartOfFramesssssss(
            a, b, c, d, e, f, g);
}

extern "C" bool __wrap__Z29DoRWStuffStartOfFrame_Horizonsssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    if (!gReplayingRightEye) {
        ResetCapturedPass(StartMode::Horizon);
        const int16 values[7] = {a, b, c, d, e, f, g};
        memcpy(gPass.startArgs, values, sizeof(values));
    }
    return __real__Z29DoRWStuffStartOfFrame_Horizonsssssss(
            a, b, c, d, e, f, g);
}

extern "C" bool __wrap__Z21DoRWStuffStartOfFramesssssss(
        int16 a, int16 b, int16 c, int16 d, int16 e, int16 f, int16 g) {
    if (!gReplayingRightEye) {
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
    if (event == rsRWTERMINATE && gXrAttempted) {
        ResetStereoBridgeState();
        BridgeLog("shutting OpenXR down before the normal RenderWare device closes");
        QuestOpenXR::Shutdown();
    }

    if (event == rsCAMERASIZE) {
        gCameraTargetReady = false;
    }

    const RsEventStatus result = __real_RsEventHandler(event, param);

    if (event == rsCAMERASIZE && result != rsEVENTERROR) {
        if (!InstallOffscreenCameraTarget(Scene.camera)) {
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
    if (gReplayingRightEye && gStereoFrameActive && gStereoShouldRender) {
        ApplyEyeToCamera(camera, gCurrentEye);
        return __real_psCameraBeginUpdate(camera);
    }

    if (QuestOpenXR::OwnsPresentation() && gXrReady) {
        if (!gPass.captureStarted) {
            ResetCapturedPass(StartMode::None);
        }
        gEyeCount = 0;
        const QuestOpenXR::StereoFrameResult frame =
                QuestOpenXR::BeginStereoFrame(gEyes, 2, &gEyeCount);
        if (frame == QuestOpenXR::StereoFrameResult::ExitRequested ||
            frame == QuestOpenXR::StereoFrameResult::FatalError) {
            BridgeLog("OpenXR stereo frame begin requested reVC shutdown");
            RsGlobal.quit = TRUE;
            return FALSE;
        }
        if (frame == QuestOpenXR::StereoFrameResult::Render) {
            gStereoFrameActive = true;
            gStereoShouldRender = true;
            gCurrentEye = 0;
            SaveCameraBase(camera);
            ApplyEyeToCamera(camera, 0);
        } else if (frame == QuestOpenXR::StereoFrameResult::SkipRender) {
            gStereoFrameActive = true;
            gStereoShouldRender = false;
        }
    }

    const RwBool result = __real_psCameraBeginUpdate(camera);
    if (!result && gStereoFrameActive) {
        ReleaseCameraBase(camera);
        QuestOpenXR::EndStereoFrame();
        ResetStereoBridgeState();
    }
    return result;
}

extern "C" void __wrap__Z19DoRWStuffEndOfFramev() {
    if (!gStereoFrameActive) {
        __real__Z19DoRWStuffEndOfFramev();
        FinishCapturedPass();
        return;
    }

    bool valid = true;
    RwCameraEndUpdate(Scene.camera);

    if (gStereoShouldRender) {
        valid = gEyeCount >= 2 && SubmitCurrentEye(0);
        if (valid) {
            RestoreCameraBase(Scene.camera);
            gCurrentEye = 1;
            gReplayingRightEye = true;
            valid = StartRightEyePass();
            if (valid) {
                if (gPass.scene) __real__Z11RenderScenev();
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

    ReleaseCameraBase(Scene.camera);
    const QuestOpenXR::FrameResult ended = QuestOpenXR::EndStereoFrame();
    if (!valid || ended == QuestOpenXR::FrameResult::FatalError ||
        ended == QuestOpenXR::FrameResult::ExitRequested) {
        BridgeLog("true per-eye render submission failed; requesting reVC shutdown");
        RsGlobal.quit = TRUE;
    }
    ResetStereoBridgeState();
}

extern "C" void __wrap_psCameraShowRaster(RwCamera* camera) {
    if (gStereoFrameActive) {
        return;
    }

    if (!gCameraTargetReady && !gBridgeFatal) {
        InstallOffscreenCameraTarget(camera);
    }

    if (gBridgeFatal) {
        RsGlobal.quit = TRUE;
        return;
    }

    if (gCameraTargetReady) {
        if (!EnsureOpenXrStarted()) {
            if (gXrAttempted || gBridgeFatal) {
                return;
            }
        } else {
            const QuestOpenXR::FrameResult frame = SubmitCameraLegacy(camera);
            if (frame == QuestOpenXR::FrameResult::ExitRequested ||
                frame == QuestOpenXR::FrameResult::FatalError) {
                BridgeLog("OpenXR bootstrap frame submission requested reVC shutdown");
                RsGlobal.quit = TRUE;
            } else if (frame == QuestOpenXR::FrameResult::Presented &&
                       !gFirstFrameLogged) {
                gFirstFrameLogged = true;
                BridgeLog("bootstrap framebuffer submitted; switching to true per-eye rendering");
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
