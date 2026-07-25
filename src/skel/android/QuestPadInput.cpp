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
    if (Abs(value) > Abs(*destination))
        *destination = value;
}

void MergeButton(int16* destination, int16 value) {
    if (value > *destination)
        *destination = value;
}

void InjectTouchState() {
    QuestOpenXR::ControllerState touch;
    if (!QuestOpenXR::GetControllerState(&touch) || !touch.active)
        return;

    CControllerState& pad = CPad::GetPad(0)->NewState;
    MergeAxis(&pad.LeftStickX, StickValue(touch.leftStickX));
    MergeAxis(&pad.LeftStickY, StickValue(-touch.leftStickY));
    MergeAxis(&pad.RightStickX, StickValue(touch.rightStickX));
    MergeAxis(&pad.RightStickY, StickValue(-touch.rightStickY));

    const float threshold = 0.55f;
    MergeButton(&pad.DPadLeft, touch.leftStickX < -threshold ? 255 : 0);
    MergeButton(&pad.DPadRight, touch.leftStickX > threshold ? 255 : 0);
    MergeButton(&pad.DPadUp, touch.leftStickY > threshold ? 255 : 0);
    MergeButton(&pad.DPadDown, touch.leftStickY < -threshold ? 255 : 0);

    MergeButton(&pad.Cross, touch.a ? 255 : 0);
    MergeButton(&pad.Circle, touch.b ? 255 : 0);
    MergeButton(&pad.Square, touch.x ? 255 : 0);
    MergeButton(&pad.Triangle, touch.y ? 255 : 0);
    MergeButton(&pad.Start, touch.menu ? 255 : 0);

    MergeButton(&pad.LeftShoulder1, PressureValue(touch.leftTrigger));
    MergeButton(&pad.RightShoulder1, PressureValue(touch.rightTrigger));
    MergeButton(&pad.LeftShoulder2, PressureValue(touch.leftGrip));
    MergeButton(&pad.RightShoulder2, PressureValue(touch.rightGrip));
    MergeButton(&pad.LeftShock, touch.leftThumb ? 255 : 0);
    MergeButton(&pad.RightShock, touch.rightThumb ? 255 : 0);
}

} // namespace

extern "C" void QuestPadInput_UpdateAfterStockPad() {
    QuestOpenXR::RefreshControllerState();
    InjectTouchState();
}

#endif
