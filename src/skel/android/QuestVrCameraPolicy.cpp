#if defined(ANDROID)

#include <android/log.h>

#include <cmath>

#include "common.h"
#include "rwcore.h"
#include "main.h"
#include "Draw.h"
#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedNormalCameraBaseline = false;

void ApplyNormalCameraStereoBaseline(QuestOpenXR::EyeView* eyes,
                                     uint32_t eyeCount) {
    if (eyes == NULL || eyeCount == 0) {
        return;
    }

    float horizontalFovDegrees = CDraw::GetFOV();
    if (!std::isfinite(horizontalFovDegrees) ||
        horizontalFovDegrees < 20.0f || horizontalFovDegrees > 140.0f) {
        horizontalFovDegrees = 70.0f;
    }

    float sourceAspect = 16.0f / 9.0f;
    if (Scene.camera != nil) {
        RwRaster* raster = RwCameraGetRaster(Scene.camera);
        if (raster != nil) {
            const int width = RwRasterGetWidth(raster);
            const int height = RwRasterGetHeight(raster);
            if (width > 0 && height > 0) {
                sourceAspect = static_cast<float>(width) /
                               static_cast<float>(height);
            }
        }
    }

    int targetWidth = eyes[0].width;
    int targetHeight = eyes[0].height;
    if (targetWidth > 0 && targetHeight > 0 && sourceAspect > 0.01f) {
        int fittedHeight = static_cast<int>(lroundf(
                static_cast<float>(targetWidth) / sourceAspect));
        if (fittedHeight <= targetHeight) {
            targetHeight = fittedHeight;
        } else {
            targetWidth = static_cast<int>(lroundf(
                    static_cast<float>(targetHeight) * sourceAspect));
        }
    }

    const float radiansPerDegree = 0.01745329251994329577f;
    const float halfHorizontal = 0.5f * horizontalFovDegrees *
                                 radiansPerDegree;
    const float halfVertical = atanf(tanf(halfHorizontal) /
                                     fmaxf(sourceAspect, 0.01f));

    // Deliberately mild stereo for this baseline: 24 mm total camera
    // separation, roughly 37.5% of a typical 64 mm IPD. The only difference
    // between eyes is this horizontal offset. Head rotation, leaning, player
    // head anchoring and OpenXR asymmetric projection are all disabled.
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
        view.angleLeft = -halfHorizontal;
        view.angleRight = halfHorizontal;
        view.angleUp = halfVertical;
        view.angleDown = -halfVertical;
        view.width = targetWidth;
        view.height = targetHeight;
    }

    if (!gLoggedNormalCameraBaseline) {
        gLoggedNormalCameraBaseline = true;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                "normal-camera stereo baseline: game FOV=%.2f aspect=%.4f capture=%dx%d separation=24mm; head pose and XR frustum overrides disabled",
                horizontalFovDegrees, sourceAspect, targetWidth, targetHeight);
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
        ApplyNormalCameraStereoBaseline(eyes, *eyeCount);
    }
    return result;
}

#endif
