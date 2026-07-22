#if defined(ANDROID)

#include <android/log.h>

#include "common.h"
#include "rwcore.h"
#include "QuestOpenXR.h"

namespace {

bool gLoggedNormalProjection = false;

void LogNormalProjectionOnce() {
    if (gLoggedNormalProjection) {
        return;
    }
    gLoggedNormalProjection = true;
    __android_log_write(ANDROID_LOG_INFO, "reVC-XR",
            "normal-camera stereo baseline preserves the game's RenderWare viewWindow and viewOffset");
}

} // namespace

extern "C" RwCamera*
__real__Z21RwCameraSetViewWindowPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* window);

extern "C" RwCamera*
__wrap__Z21RwCameraSetViewWindowPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* window) {
    if (QuestOpenXR::StereoFrameActive()) {
        LogNormalProjectionOnce();
        return camera;
    }
    return __real__Z21RwCameraSetViewWindowPN2rw6CameraEPKNS_3V2dE(
            camera, window);
}

extern "C" RwCamera*
__real__Z21RwCameraSetViewOffsetPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* offset);

extern "C" RwCamera*
__wrap__Z21RwCameraSetViewOffsetPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* offset) {
    if (QuestOpenXR::StereoFrameActive()) {
        LogNormalProjectionOnce();
        return camera;
    }
    return __real__Z21RwCameraSetViewOffsetPN2rw6CameraEPKNS_3V2dE(
            camera, offset);
}

#endif
