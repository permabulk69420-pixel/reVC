#if defined(ANDROID)

#include <cmath>

#include "common.h"
#include "Pad.h"
#include "QuestOpenXR.h"

namespace {

int16 StickValue(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < -1.0f) value = -1.0f;
    return static_cast<int16>(lroundf(value * 128.0f));
}

int16 PressureValue(float value) {
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    return static_cast<int16>(lroundf(value * 255.0f));
}

void MergeAxis(int16* destination, int16 value) {
    if (Abs(value) > Abs(*destination)) {
        *destination = value;
    }
}

void MergeButton(int16* destination, int16 value) {
    if (value > *destination) {
        *destination = value;
    }
}

void InjectTouchState() {
    QuestOpenXR::ControllerState touch;
    if (!QuestOpenXR::GetControllerState(&touch) || !touch.active) {
        return;
    }

    CControllerState& pad = CPad::GetPad(0)->NewState;
    const int16 leftX = StickValue(touch.leftStickX);
    // OpenXR stick Y is positive upward; reVC's PC pad convention is negative
    // upward, matching its existing XInput and SDL controller paths.
    const int16 leftY = StickValue(-touch.leftStickY);
    const int16 rightX = StickValue(touch.rightStickX);
    const int16 rightY = StickValue(-touch.rightStickY);

    MergeAxis(&pad.LeftStickX, leftX);
    MergeAxis(&pad.LeftStickY, leftY);
    MergeAxis(&pad.RightStickX, rightX);
    MergeAxis(&pad.RightStickY, rightY);

    // Also synthesize a D-pad at a deliberate threshold so every frontend can
    // be navigated even if that menu ignores analogue stick events.
    const float dpadThreshold = 0.55f;
    MergeButton(&pad.DPadLeft,
                touch.leftStickX < -dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadRight,
                touch.leftStickX > dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadUp,
                touch.leftStickY > dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadDown,
                touch.leftStickY < -dpadThreshold ? 255 : 0);

    // Xbox-style face layout maps cleanly to the game's DualShock naming.
    MergeButton(&pad.Cross, touch.a ? 255 : 0);
    MergeButton(&pad.Circle, touch.b ? 255 : 0);
    MergeButton(&pad.Square, touch.x ? 255 : 0);
    MergeButton(&pad.Triangle, touch.y ? 255 : 0);
    MergeButton(&pad.Start, touch.menu ? 255 : 0);

    // Triggers are the primary shoulders; grips provide the secondary pair.
    MergeButton(&pad.LeftShoulder1, PressureValue(touch.leftTrigger));
    MergeButton(&pad.RightShoulder1, PressureValue(touch.rightTrigger));
    MergeButton(&pad.LeftShoulder2, PressureValue(touch.leftGrip));
    MergeButton(&pad.RightShoulder2, PressureValue(touch.rightGrip));
    MergeButton(&pad.LeftShock, touch.leftThumb ? 255 : 0);
    MergeButton(&pad.RightShock, touch.rightThumb ? 255 : 0);
}

} // namespace

extern "C" void __real__ZN4CPad10UpdatePadsEv();
extern "C" void __wrap__ZN4CPad10UpdatePadsEv() {
    // Sync before the game's ordinary input tick so these values drive the same
    // simulation frame. The real update still runs first and remains canonical.
    QuestOpenXR::RefreshControllerState();
    __real__ZN4CPad10UpdatePadsEv();
    InjectTouchState();
}

#endif
