#if defined(ANDROID)

#include <android/log.h>

#include <cmath>
#include <cstring>

#include "common.h"
#include "ControllerConfig.h"
#include "Pad.h"
#include "QuestOpenXR.h"

// Quest Touch is merged into the pad state that the game actually reads.
//
// Injection happens after the real CPad::UpdatePads has finished, directly into
// NewState. That is the configuration the Quest hardware checkpoint recorded as
// working ("menu navigation and selection work"), and it is deliberately chosen
// over writing into PCTempJoyState earlier in the tick: everything written
// before CPad::Update runs is first passed through ReconcileTwoControllersInput,
// which rebuilds the state field by field and applies contradiction fixes to the
// stick/D-pad pairs. Injecting after that reconcile keeps Touch state whole.
//
// This never replaces SDL input. MergeAxis and MergeButton only ever raise a
// field towards the larger magnitude, so a physically connected controller keeps
// working exactly as it did and Touch is additive on top.
//
// CPad::UpdatePads is defined in core/Pad.cpp and called from core/main.cpp
// (lines 811 and 1740, the main and frontend loops), so this reference genuinely
// crosses object files and --wrap applies to it. Do not move this hook onto a
// function that main.cpp both defines and calls.

namespace {

const char* const kTag = "reVC-XR";

unsigned int gPadTicks = 0;

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

// Reports what OpenXR delivered and what survived into the pad, once a second.
// The previous input hook failed in a way that looked like "only one button
// works", which is impossible to localise without seeing both ends.
void ReportPadState(const QuestOpenXR::ControllerState& touch,
                    const CControllerState& pad) {
    if ((++gPadTicks % 72) != 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
            "touch active=%d a=%d b=%d x=%d y=%d menu=%d lt=%.2f rt=%.2f "
            "lg=%.2f rg=%.2f lthumb=%d rthumb=%d lstick=%.2f,%.2f rstick=%.2f,%.2f",
            touch.active ? 1 : 0, touch.a ? 1 : 0, touch.b ? 1 : 0,
            touch.x ? 1 : 0, touch.y ? 1 : 0, touch.menu ? 1 : 0,
            touch.leftTrigger, touch.rightTrigger, touch.leftGrip,
            touch.rightGrip, touch.leftThumb ? 1 : 0, touch.rightThumb ? 1 : 0,
            touch.leftStickX, touch.leftStickY,
            touch.rightStickX, touch.rightStickY);
    __android_log_print(ANDROID_LOG_INFO, kTag,
            "pad   cross=%d circle=%d square=%d triangle=%d start=%d "
            "dpad=%d,%d,%d,%d l1=%d r1=%d l2=%d r2=%d lstick=%d,%d rstick=%d,%d",
            pad.Cross, pad.Circle, pad.Square, pad.Triangle, pad.Start,
            pad.DPadUp, pad.DPadDown, pad.DPadLeft, pad.DPadRight,
            pad.LeftShoulder1, pad.RightShoulder1,
            pad.LeftShoulder2, pad.RightShoulder2,
            pad.LeftStickX, pad.LeftStickY, pad.RightStickX, pad.RightStickY);
}

void InjectTouchState() {
    QuestOpenXR::ControllerState touch;
    if (!QuestOpenXR::GetControllerState(&touch)) {
        return;
    }

    CControllerState& pad = CPad::GetPad(0)->NewState;
    if (!touch.active) {
        ReportPadState(touch, pad);
        return;
    }

    // OpenXR stick Y is positive upward; reVC's PC pad convention is negative
    // upward, matching its existing XInput and SDL controller paths.
    MergeAxis(&pad.LeftStickX, StickValue(touch.leftStickX));
    MergeAxis(&pad.LeftStickY, StickValue(-touch.leftStickY));
    MergeAxis(&pad.RightStickX, StickValue(touch.rightStickX));
    MergeAxis(&pad.RightStickY, StickValue(-touch.rightStickY));

    // Synthesize a D-pad at a deliberate threshold so menus that ignore the
    // analogue stick can still be navigated.
    const float dpadThreshold = 0.55f;
    MergeButton(&pad.DPadLeft, touch.leftStickX < -dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadRight, touch.leftStickX > dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadUp, touch.leftStickY > dpadThreshold ? 255 : 0);
    MergeButton(&pad.DPadDown, touch.leftStickY < -dpadThreshold ? 255 : 0);

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

    ReportPadState(touch, pad);
}

} // namespace

extern "C" void __real__ZN4CPad10UpdatePadsEv();

extern "C" void __wrap__ZN4CPad10UpdatePadsEv() {
    // Sync first so the values belong to this simulation frame, then let the
    // stock update run untouched and merge Touch into its result.
    QuestOpenXR::RefreshControllerState();
    __real__ZN4CPad10UpdatePadsEv();
    InjectTouchState();
}

#endif
