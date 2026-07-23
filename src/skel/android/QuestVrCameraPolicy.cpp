#if defined(ANDROID)

#include <android/log.h>

#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedSinglePassBaseline = false;

} // namespace

// The immersive stereo bridge normally starts an OpenXR frame before reVC
// constructs its render list, saves/restores TheCamera, and manually replays the
// scene for the right eye. That replay is the leading suspect for the cutscene
// blur and persistent camera corruption: cutscene fades, motion blur and other
// render state have already been consumed by the first pass.
//
// For this control build, never enter that path. reVC renders exactly one normal
// frame with its native camera and projection. psCameraShowRaster then submits
// the completed capture through the proven mono OpenXR path, which copies the
// same untouched image to both eye swapchains. QuestOpenXRScreenBaseline rewrites
// those swapchains as eye-specific 16:9 quad layers at xrEndFrame.
extern "C" QuestOpenXR::StereoFrameResult
__wrap__ZN11QuestOpenXR16BeginStereoFrameEPNS_7EyeViewEjPj(
        QuestOpenXR::EyeView* eyes, uint32_t eyeCapacity,
        uint32_t* eyeCount) {
    (void)eyes;
    (void)eyeCapacity;
    if (eyeCount != NULL) {
        *eyeCount = 0;
    }

    if (!gLoggedSinglePassBaseline) {
        gLoggedSinglePassBaseline = true;
        __android_log_write(
                ANDROID_LOG_INFO, kTag,
                "single-pass native-camera baseline: stereo replay disabled; one untouched reVC frame copied to both eyes");
    }
    return QuestOpenXR::StereoFrameResult::Inactive;
}

#endif
