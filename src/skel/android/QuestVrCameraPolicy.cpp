#if defined(ANDROID)

#include <android/log.h>

#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedProjectionMatchedBaseline = false;

void ApplyProjectionMatchedStereoBaseline(QuestOpenXR::EyeView* eyes,
                                          uint32_t eyeCount) {
    if (eyes == NULL || eyeCount == 0) {
        return;
    }

    // Keep the runtime-provided asymmetric FOV and recommended per-eye
    // resolution intact. QuestOpenXR submits these same frusta to the
    // compositor, so RenderWare must render with these exact values or the
    // world appears heavily zoomed and cropped.
    //
    // Head orientation and positional tracking remain disabled for this
    // diagnostic. The only per-eye camera difference is a deliberately mild
    // 24 mm total separation, allowing projection and stereo replay to be
    // validated before tracked head pose is reintroduced.
    const float halfEyeSeparation = 0.012f;
    for (uint32_t eye = 0; eye < eyeCount; ++eye) {
        QuestOpenXR::EyeView& view = eyes[eye];
        view.orientation[0] = 0.0f;
        view.orientation[1] = 0.0f;
        view.orientation[2] = 0.0f;
        view.orientation[3] = 1.0f;
        view.position[0] = eye == 0 ? -halfEyeSeparation : halfEyeSeparation;
        view.position[1] = 0.0f;
        view.position[2] = 0.0f;
    }

    if (!gLoggedProjectionMatchedBaseline) {
        gLoggedProjectionMatchedBaseline = true;
        const QuestOpenXR::EyeView& left = eyes[0];
        const float radiansToDegrees = 57.295779513082320876f;
        const float horizontalDegrees =
                (left.angleRight - left.angleLeft) * radiansToDegrees;
        const float verticalDegrees =
                (left.angleUp - left.angleDown) * radiansToDegrees;
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "projection-matched stereo baseline: XR FOV %.2fx%.2f deg, eye target %dx%d, separation=24mm; head pose disabled",
                horizontalDegrees, verticalDegrees,
                left.width, left.height);
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
        ApplyProjectionMatchedStereoBaseline(eyes, *eyeCount);
    }
    return result;
}

#endif
