#if defined(ANDROID)

#include <android/log.h>

#include <cmath>

#include "common.h"
#include "rwcore.h"
#include "main.h"
#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedBasisRepair = false;

bool NormalizeSafe(CVector* value) {
    if (value == nil) {
        return false;
    }
    const float magnitudeSquared = DotProduct(*value, *value);
    if (!std::isfinite(magnitudeSquared) || magnitudeSquared < 0.000001f) {
        return false;
    }
    *value /= sqrtf(magnitudeSquared);
    return true;
}

float BasisDeterminant(const CVector& right, const CVector& forward,
                       const CVector& up) {
    return DotProduct(right, CrossProduct(forward, up));
}

void RepairCameraBasis(RwFrame* frame) {
    if (frame == nil) {
        return;
    }

    RwMatrix* matrix = RwFrameGetMatrix(frame);
    if (matrix == nil) {
        return;
    }

    const CVector originalRight = *RwMatrixGetRight(matrix);
    const CVector originalForward = *RwMatrixGetAt(matrix);
    const CVector originalUp = *RwMatrixGetUp(matrix);
    const float originalDeterminant = BasisDeterminant(
            originalRight, originalForward, originalUp);

    CVector forward = originalForward;
    CVector up = originalUp;
    if (!NormalizeSafe(&forward)) {
        return;
    }

    // Gram-Schmidt the up vector against forward, then derive the remaining
    // axes in reVC's native right/forward/up convention. The old stereo path
    // calculated forward x up correctly, but then stored up x forward as the
    // camera right vector, reflecting the basis across the camera plane.
    up -= forward * DotProduct(up, forward);
    if (!NormalizeSafe(&up)) {
        return;
    }

    CVector right = CrossProduct(forward, up);
    if (!NormalizeSafe(&right)) {
        return;
    }
    up = CrossProduct(right, forward);
    if (!NormalizeSafe(&up)) {
        return;
    }

    *RwMatrixGetRight(matrix) = right;
    *RwMatrixGetAt(matrix) = forward;
    *RwMatrixGetUp(matrix) = up;
    RwMatrixUpdate(matrix);
    RwFrameUpdateObjects(frame);

    if (!gLoggedBasisRepair) {
        gLoggedBasisRepair = true;
        const float repairedDeterminant = BasisDeterminant(right, forward, up);
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "camera basis repaired after orthonormalize: determinant %.4f -> %.4f, dots RF=%.4f RU=%.4f FU=%.4f",
                originalDeterminant, repairedDeterminant,
                DotProduct(right, forward), DotProduct(right, up),
                DotProduct(forward, up));
    }
}

} // namespace

// librw exposes this compatibility helper as a C++ free function. Wrap the
// exact symbol so the normal reVC camera update remains untouched everywhere
// except the active OpenXR game camera.
extern "C" RwFrame*
__real__Z21RwFrameOrthoNormalizePN2rw5FrameE(RwFrame* frame);

extern "C" RwFrame*
__wrap__Z21RwFrameOrthoNormalizePN2rw5FrameE(RwFrame* frame) {
    RwFrame* result =
            __real__Z21RwFrameOrthoNormalizePN2rw5FrameE(frame);

    if (QuestOpenXR::StereoFrameActive() && Scene.camera != nil &&
        frame == RwCameraGetFrame(Scene.camera)) {
        RepairCameraBasis(frame);
    }
    return result;
}

#endif
