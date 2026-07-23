#if defined(ANDROID)

#include <android/log.h>

#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedUntouchedCameraBaseline = false;

void ApplyUntouchedCameraBaseline(QuestOpenXR::EyeView* eyes,
                                  uint32_t eyeCount) {
    if (eyes == NULL || eyeCount == 0) {
        return;
    }

    // Mark the eye views invalid for the game-camera bridge. The OpenXR runtime
    // still owns and submits both eye swapchains, but ApplyEyeCamera(),
    // ApplyEyeProjection() and the stereo capture-size override all return
    // without modifying Vice City's native camera or RenderWare projection.
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        QuestOpenXR::EyeView& view = eyes[eye];
        view.valid = false;
        view.orientation[0] = 0.0f;
        view.orientation[1] = 0.0f;
        view.orientation[2] = 0.0f;
        view.orientation[3] = 1.0f;
        view.position[0] = 0.0f;
        view.position[1] = 0.0f;
        view.position[2] = 0.0f;
    }

    if (!gLoggedUntouchedCameraBaseline) {
        gLoggedUntouchedCameraBaseline = true;
        __android_log_write(
                ANDROID_LOG_INFO, kTag,
                "untouched-camera baseline: game pose, FOV and RenderWare projection injection disabled");
    }
}

} // namespace

extern "C" QuestOpenXR::StereoFrameResult
__real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount);

extern "C" QuestOpenXR::StereoFrameResult
__wrap__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount) {
    const QuestOpenXR::StereoFrameResult result =
            __real__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
                    eyes, eyeCapacity, eyeCount);
    if (result == QuestOpenXR::StereoFrameResult::Render &&
        eyes != NULL && eyeCount != NULL && *eyeCount > 0) {
        ApplyUntouchedCameraBaseline(eyes, *eyeCount);
    }
    return result;
}

#endif
