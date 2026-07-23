#if defined(ANDROID)

#include <android/log.h>

#include <cmath>

#include "common.h"
#include "rwcore.h"
#include "main.h"
#include "Camera.h"
#include "QuestOpenXR.h"

namespace {

const char* const kTag = "reVC-XR";
bool gLoggedGameBasisRepair = false;
bool gLoggedRenderWareBasisRepair = false;

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

bool BuildNativeBasis(const CVector& sourceForward, const CVector& sourceUp,
                      CVector* rightOut, CVector* forwardOut,
                      CVector* upOut) {
    CVector forward = sourceForward;
    CVector up = sourceUp;
    if (!NormalizeSafe(&forward)) {
        return false;
    }

    // Remove any accumulated forward component from up, then derive reVC's
    // native right/forward/up basis. For Vice City's default axes,
    // forward x up = right.
    up -= forward * DotProduct(up, forward);
    if (!NormalizeSafe(&up)) {
        return false;
    }

    CVector right = CrossProduct(forward, up);
    if (!NormalizeSafe(&right)) {
        return false;
    }
    up = CrossProduct(right, forward);
    if (!NormalizeSafe(&up)) {
        return false;
    }

    *rightOut = right;
    *forwardOut = forward;
    *upOut = up;
    return true;
}

void RepairGameCameraBasis(CCamera* camera) {
    if (camera == nil) {
        return;
    }

    CMatrix& matrix = camera->GetMatrix();
    const CVector originalRight = matrix.GetRight();
    const CVector originalForward = matrix.GetForward();
    const CVector originalUp = matrix.GetUp();

    CVector right;
    CVector forward;
    CVector up;
    if (!BuildNativeBasis(originalForward, originalUp,
                          &right, &forward, &up)) {
        return;
    }

    matrix.GetRight() = right;
    matrix.GetForward() = forward;
    matrix.GetUp() = up;

    if (!gLoggedGameBasisRepair) {
        gLoggedGameBasisRepair = true;
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "internal GTA camera basis repaired before CalculateDerivedValues: determinant %.4f -> %.4f",
                BasisDeterminant(originalRight, originalForward, originalUp),
                BasisDeterminant(right, forward, up));
    }
}

void RepairRenderWareCameraBasis(RwFrame* frame) {
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

    CVector right;
    CVector forward;
    CVector up;
    if (!BuildNativeBasis(originalForward, originalUp,
                          &right, &forward, &up)) {
        return;
    }

    *RwMatrixGetRight(matrix) = right;
    *RwMatrixGetAt(matrix) = forward;
    *RwMatrixGetUp(matrix) = up;
    RwMatrixUpdate(matrix);
    RwFrameUpdateObjects(frame);

    if (!gLoggedRenderWareBasisRepair) {
        gLoggedRenderWareBasisRepair = true;
        __android_log_print(
                ANDROID_LOG_INFO, kTag,
                "RenderWare camera basis verified after orthonormalize: determinant %.4f -> %.4f, dots RF=%.4f RU=%.4f FU=%.4f",
                BasisDeterminant(originalRight, originalForward, originalUp),
                BasisDeterminant(right, forward, up),
                DotProduct(right, forward), DotProduct(right, up),
                DotProduct(forward, up));
    }
}

} // namespace

// ApplyEyeCamera writes TheCamera's basis and immediately calls this member.
// Repairing only the later RenderWare copy left GTA's own camera reflected and
// allowed the bad orientation to leak into the following frame and cutscenes.
extern "C" void
__real__ZN7CCamera22CalculateDerivedValuesEv(CCamera* camera);

extern "C" void
__wrap__ZN7CCamera22CalculateDerivedValuesEv(CCamera* camera) {
    if (QuestOpenXR::StereoFrameActive() && camera == &TheCamera) {
        RepairGameCameraBasis(camera);
    }
    __real__ZN7CCamera22CalculateDerivedValuesEv(camera);
}

extern "C" RwFrame*
__real__Z21RwFrameOrthoNormalizePN2rw5FrameE(RwFrame* frame);

extern "C" RwFrame*
__wrap__Z21RwFrameOrthoNormalizePN2rw5FrameE(RwFrame* frame) {
    RwFrame* result =
            __real__Z21RwFrameOrthoNormalizePN2rw5FrameE(frame);

    if (QuestOpenXR::StereoFrameActive() && Scene.camera != nil &&
        frame == RwCameraGetFrame(Scene.camera)) {
        RepairRenderWareCameraBasis(frame);
    }
    return result;
}

#endif
