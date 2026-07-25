#if defined(ANDROID)

// Keep the proven immersive/stereo runtime as the implementation underneath
// this thin controller layer. Rename only the lifecycle entry points so this
// file can create and destroy an OpenXR action set around them.
#define Initialize InitializeStereoBase
#define Shutdown ShutdownStereoBase
#include "QuestOpenXRStereo.cpp"
#undef Shutdown
#undef Initialize

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct ControllerRuntime {
    bool ready;
    bool loggedActive;
    bool loggedSyncFailure;
    XrActionSet actionSet;
    XrAction thumbstick;
    XrAction trigger;
    XrAction squeeze;
    XrAction primary;
    XrAction secondary;
    XrAction menu;
    XrAction thumbClick;
    XrPath hands[2];
    QuestOpenXR::ControllerState state;

    ControllerRuntime()
        : ready(false), loggedActive(false), loggedSyncFailure(false),
          actionSet(XR_NULL_HANDLE), thumbstick(XR_NULL_HANDLE),
          trigger(XR_NULL_HANDLE), squeeze(XR_NULL_HANDLE),
          primary(XR_NULL_HANDLE), secondary(XR_NULL_HANDLE),
          menu(XR_NULL_HANDLE), thumbClick(XR_NULL_HANDLE) {
        hands[0] = XR_NULL_PATH;
        hands[1] = XR_NULL_PATH;
        Clear();
    }

    void Clear() {
        memset(&state, 0, sizeof(state));
    }
};

ControllerRuntime controls;

bool ToPath(const char* text, XrPath* path) {
    return Check(xrStringToPath(g.instance, text, path), text);
}

bool CreateAction(XrActionType type, const char* name,
                  const char* localizedName, XrAction* action) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(info.localizedActionName, localizedName,
            XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.countSubactionPaths = 2;
    info.subactionPaths = controls.hands;
    return Check(xrCreateAction(controls.actionSet, &info, action), name);
}

bool AddBinding(std::vector<XrActionSuggestedBinding>* bindings,
                XrAction action, const char* pathText) {
    XrPath path = XR_NULL_PATH;
    if (!ToPath(pathText, &path)) {
        return false;
    }
    XrActionSuggestedBinding binding;
    binding.action = action;
    binding.binding = path;
    bindings->push_back(binding);
    return true;
}

bool SuggestTouchBindings() {
    XrPath profile = XR_NULL_PATH;
    if (!ToPath("/interaction_profiles/oculus/touch_controller", &profile)) {
        return false;
    }

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(16);
    bool valid = true;
    valid = AddBinding(&bindings, controls.thumbstick,
                       "/user/hand/left/input/thumbstick") && valid;
    valid = AddBinding(&bindings, controls.thumbstick,
                       "/user/hand/right/input/thumbstick") && valid;
    valid = AddBinding(&bindings, controls.trigger,
                       "/user/hand/left/input/trigger/value") && valid;
    valid = AddBinding(&bindings, controls.trigger,
                       "/user/hand/right/input/trigger/value") && valid;
    valid = AddBinding(&bindings, controls.squeeze,
                       "/user/hand/left/input/squeeze/value") && valid;
    valid = AddBinding(&bindings, controls.squeeze,
                       "/user/hand/right/input/squeeze/value") && valid;
    valid = AddBinding(&bindings, controls.primary,
                       "/user/hand/left/input/x/click") && valid;
    valid = AddBinding(&bindings, controls.primary,
                       "/user/hand/right/input/a/click") && valid;
    valid = AddBinding(&bindings, controls.secondary,
                       "/user/hand/left/input/y/click") && valid;
    valid = AddBinding(&bindings, controls.secondary,
                       "/user/hand/right/input/b/click") && valid;
    valid = AddBinding(&bindings, controls.menu,
                       "/user/hand/left/input/menu/click") && valid;
    valid = AddBinding(&bindings, controls.thumbClick,
                       "/user/hand/left/input/thumbstick/click") && valid;
    valid = AddBinding(&bindings, controls.thumbClick,
                       "/user/hand/right/input/thumbstick/click") && valid;
    if (!valid) {
        return false;
    }

    XrInteractionProfileSuggestedBinding suggested = {
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING
    };
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings =
            static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    return Check(xrSuggestInteractionProfileBindings(g.instance, &suggested),
                 "xrSuggestInteractionProfileBindings(Touch)");
}

void DestroyControllerActions() {
    controls.Clear();
    controls.ready = false;
    controls.loggedActive = false;
    controls.loggedSyncFailure = false;
    if (controls.actionSet != XR_NULL_HANDLE && g.instance != XR_NULL_HANDLE) {
        xrDestroyActionSet(controls.actionSet);
    }
    controls.actionSet = XR_NULL_HANDLE;
    controls.thumbstick = XR_NULL_HANDLE;
    controls.trigger = XR_NULL_HANDLE;
    controls.squeeze = XR_NULL_HANDLE;
    controls.primary = XR_NULL_HANDLE;
    controls.secondary = XR_NULL_HANDLE;
    controls.menu = XR_NULL_HANDLE;
    controls.thumbClick = XR_NULL_HANDLE;
    controls.hands[0] = XR_NULL_PATH;
    controls.hands[1] = XR_NULL_PATH;
}

bool InitializeControllerActions() {
    if (controls.ready) {
        return true;
    }
    if (g.instance == XR_NULL_HANDLE || g.session == XR_NULL_HANDLE) {
        return false;
    }

    DestroyControllerActions();
    if (!ToPath("/user/hand/left", &controls.hands[0]) ||
        !ToPath("/user/hand/right", &controls.hands[1])) {
        return false;
    }

    XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(setInfo.actionSetName, "revc_gameplay",
            XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "reVC gameplay",
            XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    if (!Check(xrCreateActionSet(g.instance, &setInfo, &controls.actionSet),
               "xrCreateActionSet(reVC gameplay)")) {
        return false;
    }

    bool valid = true;
    valid = CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick",
                         "Thumbsticks", &controls.thumbstick) && valid;
    valid = CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger",
                         "Triggers", &controls.trigger) && valid;
    valid = CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze",
                         "Grips", &controls.squeeze) && valid;
    valid = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary",
                         "A or X", &controls.primary) && valid;
    valid = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary",
                         "B or Y", &controls.secondary) && valid;
    valid = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu_button",
                         "Menu", &controls.menu) && valid;
    valid = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumb_click",
                         "Thumbstick clicks", &controls.thumbClick) && valid;
    if (!valid || !SuggestTouchBindings()) {
        DestroyControllerActions();
        return false;
    }

    XrSessionActionSetsAttachInfo attach = {
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO
    };
    attach.countActionSets = 1;
    attach.actionSets = &controls.actionSet;
    if (!Check(xrAttachSessionActionSets(g.session, &attach),
               "xrAttachSessionActionSets(reVC gameplay)")) {
        DestroyControllerActions();
        return false;
    }

    controls.ready = true;
    Log("OpenXR Touch action set attached: sticks, face buttons, triggers, grips and menu");
    return true;
}

float Deadzone(float value) {
    const float magnitude = fabsf(value);
    const float threshold = 0.18f;
    if (magnitude <= threshold) {
        return 0.0f;
    }
    const float scaled = (magnitude - threshold) / (1.0f - threshold);
    return value < 0.0f ? -scaled : scaled;
}

bool ReadVector2(XrAction action, XrPath hand, float* x, float* y,
                 bool* active) {
    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateVector2f value = {XR_TYPE_ACTION_STATE_VECTOR2F};
    const XrResult result = xrGetActionStateVector2f(g.session, &get, &value);
    if (XR_FAILED(result)) {
        return false;
    }
    *active = value.isActive == XR_TRUE;
    *x = *active ? Deadzone(value.currentState.x) : 0.0f;
    *y = *active ? Deadzone(value.currentState.y) : 0.0f;
    return true;
}

bool ReadFloat(XrAction action, XrPath hand, float* output,
               bool* active) {
    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateFloat value = {XR_TYPE_ACTION_STATE_FLOAT};
    const XrResult result = xrGetActionStateFloat(g.session, &get, &value);
    if (XR_FAILED(result)) {
        return false;
    }
    *active = value.isActive == XR_TRUE;
    *output = *active ? value.currentState : 0.0f;
    return true;
}

bool ReadBoolean(XrAction action, XrPath hand, bool* output,
                 bool* active) {
    XrActionStateGetInfo get = {XR_TYPE_ACTION_STATE_GET_INFO};
    get.action = action;
    get.subactionPath = hand;
    XrActionStateBoolean value = {XR_TYPE_ACTION_STATE_BOOLEAN};
    const XrResult result = xrGetActionStateBoolean(g.session, &get, &value);
    if (XR_FAILED(result)) {
        return false;
    }
    *active = value.isActive == XR_TRUE;
    *output = *active && value.currentState == XR_TRUE;
    return true;
}

void SyncControllerActions() {
    controls.Clear();
    if (!controls.ready || g.session == XR_NULL_HANDLE ||
        !g.sessionRunning || g.sessionState != XR_SESSION_STATE_FOCUSED) {
        return;
    }

    XrActiveActionSet activeSet;
    activeSet.actionSet = controls.actionSet;
    activeSet.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &activeSet;
    const XrResult syncResult = xrSyncActions(g.session, &sync);
    if (syncResult == XR_SESSION_NOT_FOCUSED) {
        return;
    }
    if (XR_FAILED(syncResult)) {
        if (!controls.loggedSyncFailure) {
            controls.loggedSyncFailure = true;
            Log("xrSyncActions failed: %s", ResultName(syncResult));
        }
        return;
    }

    bool anyActive = false;
    bool active = false;
    if (!ReadVector2(controls.thumbstick, controls.hands[0],
                     &controls.state.leftStickX,
                     &controls.state.leftStickY, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadVector2(controls.thumbstick, controls.hands[1],
                     &controls.state.rightStickX,
                     &controls.state.rightStickY, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadFloat(controls.trigger, controls.hands[0],
                   &controls.state.leftTrigger, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadFloat(controls.trigger, controls.hands[1],
                   &controls.state.rightTrigger, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadFloat(controls.squeeze, controls.hands[0],
                   &controls.state.leftGrip, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadFloat(controls.squeeze, controls.hands[1],
                   &controls.state.rightGrip, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.primary, controls.hands[0],
                     &controls.state.x, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.primary, controls.hands[1],
                     &controls.state.a, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.secondary, controls.hands[0],
                     &controls.state.y, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.secondary, controls.hands[1],
                     &controls.state.b, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.menu, controls.hands[0],
                     &controls.state.menu, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.thumbClick, controls.hands[0],
                     &controls.state.leftThumb, &active)) {
        return;
    }
    anyActive = anyActive || active;
    if (!ReadBoolean(controls.thumbClick, controls.hands[1],
                     &controls.state.rightThumb, &active)) {
        return;
    }
    anyActive = anyActive || active;

    controls.state.active = anyActive;
    if (anyActive && !controls.loggedActive) {
        controls.loggedActive = true;
        Log("Quest Touch controllers active through OpenXR actions");
    }
}

} // namespace

namespace QuestOpenXR {

bool Initialize() {
    const bool initialized = InitializeStereoBase();
    if (initialized && !InitializeControllerActions()) {
        Log("OpenXR rendering initialized, but Touch action setup failed; continuing without injected controls");
    }
    return initialized;
}

void RefreshControllerState() {
    if (!g.initialized || g.fatalError || !OnOwnerThread("RefreshControllerState")) {
        controls.Clear();
        return;
    }
    if (!PollEventsInternal()) {
        controls.Clear();
        return;
    }
    SyncControllerActions();
}

bool GetControllerState(ControllerState* state) {
    if (state == NULL) {
        return false;
    }
    *state = controls.state;
    return controls.ready;
}

void Shutdown() {
    DestroyControllerActions();
    ShutdownStereoBase();
}

} // namespace QuestOpenXR

#endif
