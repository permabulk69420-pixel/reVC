#if defined(ANDROID)

#include <android/log.h>

#include "common.h"
#include "Camera.h"
#include "QuestOpenXR.h"

namespace {

bool gLoggedCameraBasisFix = false;

void RepairCameraBasis(CCamera* camera) {
    if (camera == nil || !QuestOpenXR::OwnsPresentation()) {
        return;
    }

    CVector forward = camera->GetForward();
    CVector up = camera->GetUp();
    if (forward.MagnitudeSqr() < 0.000001f ||
        up.MagnitudeSqr() < 0.000001f) {
        return;
    }

    forward.Normalise();
    up.Normalise();

    // reVC/RenderWare's own camera convention is:
    //   right = CrossProduct(forward, up)
    //   up    = CrossProduct(right, forward)
    //
    // The first native-stereo bridge accidentally stored CrossProduct(up,
    // forward), producing a reflected/left-handed camera matrix. Inverting
    // that matrix for rendering made pitch, scale and stereo projection look
    // wildly wrong even though both eyes were being rendered successfully.
    CVector right = CrossProduct(forward, up);
    if (right.MagnitudeSqr() < 0.000001f) {
        return;
    }
    right.Normalise();
    up = CrossProduct(right, forward);
    up.Normalise();

    camera->GetForward() = forward;
    camera->GetRight() = right;
    camera->GetUp() = up;

    if (!gLoggedCameraBasisFix) {
        gLoggedCameraBasisFix = true;
        __android_log_write(ANDROID_LOG_INFO, "reVC-XR",
                "repaired VR camera basis to RenderWare right=forward x up convention");
    }
}

} // namespace

extern "C" void
__real__ZN7CCamera22CalculateDerivedValuesEv(CCamera* camera);

extern "C" void
__wrap__ZN7CCamera22CalculateDerivedValuesEv(CCamera* camera) {
    RepairCameraBasis(camera);
    __real__ZN7CCamera22CalculateDerivedValuesEv(camera);
}

#endif
