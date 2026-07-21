#if defined(ANDROID)

#include <jni.h>
#include <android/log.h>

#include <cerrno>
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
bool gCameraPoseApplied = false;
RwMatrix gSavedCameraMatrix;
RwV2d gSavedViewWindow;
RwV2d gSavedViewOffset;

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

void RestoreCameraAfterHeadTracking(RwCamera* camera) {
    if (!gCameraPoseApplied || camera == nil) {
        return;
    }
    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame != nil) {
        *RwFrameGetMatrix(frame) = gSavedCameraMatrix;
        RwFrameUpdateObjects(frame);
    }
    RwCameraSetViewWindow(camera, &gSavedViewWindow);
    RwCameraSetViewOffset(camera, &gSavedViewOffset);
    gCameraPoseApplied = false;
}

void ApplyHeadTrackingToCamera(RwCamera* camera) {
    RestoreCameraAfterHeadTracking(camera);

    QuestOpenXR::HeadState head;
    if (camera == nil || !QuestOpenXR::OwnsPresentation() ||
        !QuestOpenXR::GetHeadState(&head) || !head.valid) {
        return;
    }

    RwFrame* frame = RwCameraGetFrame(camera);
    if (frame == nil) {
        return;
    }

    gSavedCameraMatrix = *RwFrameGetMatrix(frame);
    gSavedViewWindow = *RwCameraGetViewWindow(camera);
    gSavedViewOffset = *RwCameraGetViewOffset(camera);

    const rw::Quat orientation = rw::makeQuat(
            head.orientation[3],
            head.orientation[0],
            -head.orientation[2],
            head.orientation[1]);
    RwMatrix localRotation;
    localRotation.setIdentity();
    localRotation.rotate(orientation, rw::COMBINEREPLACE);

    const RwV3d position = RwFrameGetMatrix(frame)->pos;
    RwFrameTransform(frame, &localRotation, rwCOMBINEPOSTCONCAT);
    RwFrameGetMatrix(frame)->pos = position;
    RwFrameUpdateObjects(frame);

    RwV2d viewWindow;
    viewWindow.x = head.tanHalfFov[0];
    viewWindow.y = head.tanHalfFov[1];
    RwV2d viewOffset = {0.0f, 0.0f};
    RwCameraSetViewWindow(camera, &viewWindow);
    RwCameraSetViewOffset(camera, &viewOffset);
    gCameraPoseApplied = true;
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

QuestOpenXR::FrameResult SubmitCamera(RwCamera* camera) {
    if (camera == nil) {
        return QuestOpenXR::FrameResult::FatalError;
    }
    RwRaster* raster = RwCameraGetRaster(camera);
    if (raster == nil) {
        return QuestOpenXR::FrameResult::FatalError;
    }
    rw::gl3::Gl3Raster* native = PLUGINOFFSET(
            rw::gl3::Gl3Raster, raster, rw::gl3::nativeRasterOffset);
    return QuestOpenXR::SubmitGameFrame(
            native == nil ? 0 : native->fbo,
            RwRasterGetWidth(raster), RwRasterGetHeight(raster));
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
    BridgeLog("OpenXR adopted the existing SDL EGL context; first game frame may submit");
    return true;
}

} // namespace

extern "C" RsEventStatus __real_RsEventHandler(RsEvent event, void* param);
extern "C" RwBool __real_psCameraBeginUpdate(RwCamera* camera);
extern "C" void __real_psCameraShowRaster(RwCamera* camera);

extern "C" RsEventStatus __wrap_RsEventHandler(RsEvent event, void* param) {
    if (event == rsRWTERMINATE && gXrAttempted) {
        RestoreCameraAfterHeadTracking(Scene.camera);
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
    ApplyHeadTrackingToCamera(camera);
    const RwBool result = __real_psCameraBeginUpdate(camera);
    if (!result) {
        RestoreCameraAfterHeadTracking(camera);
    }
    return result;
}

extern "C" void __wrap_psCameraShowRaster(RwCamera* camera) {
    RestoreCameraAfterHeadTracking(camera);

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
            const QuestOpenXR::FrameResult frame = SubmitCamera(camera);
            if (frame == QuestOpenXR::FrameResult::ExitRequested ||
                frame == QuestOpenXR::FrameResult::FatalError) {
                BridgeLog("OpenXR frame submission requested reVC shutdown");
                RsGlobal.quit = TRUE;
            } else if (frame == QuestOpenXR::FrameResult::Presented &&
                       !gFirstFrameLogged) {
                gFirstFrameLogged = true;
                BridgeLog("first completed reVC framebuffer submitted to Quest");
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
