#if defined(ANDROID)

#include <android/log.h>

#include "common.h"
#include "rwcore.h"
#include "QuestOpenXR.h"

namespace {

bool gLoggedMatchedProjection = false;

void LogMatchedProjectionOnce() {
    if (gLoggedMatchedProjection) {
        return;
    }
    gLoggedMatchedProjection = true;
    __android_log_write(
            ANDROID_LOG_INFO, "reVC-XR",
            "OpenXR projection enabled: RenderWare viewWindow/viewOffset now match the submitted eye frusta");
}

} // namespace

extern "C" RwCamera*
__real__Z21RwCameraSetViewWindowPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* window);

extern "C" RwCamera*
__wrap__Z21RwCameraSetViewWindowPN2rw6CameraEPKNS_3V2dE(
        RwCamera* camera, const RwV2d* window) {
    if (QuestOpenXR::StereoFrameActive()) {
        LogMatchedProjectionOnce();
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
    return __real__Z21RwCameraSetViewOffsetPN2rw6CameraEPKNS_3V2dE(
            camera, offset);
}

#endif
