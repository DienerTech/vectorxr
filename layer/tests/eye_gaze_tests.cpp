#include "depthxr/openxr_layer.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
const auto instance = Handle<XrInstance>(1);
const auto session = Handle<XrSession>(2);
const auto app_set = Handle<XrActionSet>(3);
const auto private_set = Handle<XrActionSet>(4);
const auto app_action = Handle<XrAction>(5);
const auto other_action = Handle<XrAction>(6);
const auto private_action = Handle<XrAction>(7);
constexpr XrPath profile = 65, pose = 66;
XrAction app_created_action = app_action;
XrResult suggestion_result = XR_SUCCESS, attachment_result = XR_SUCCESS, sync_result = XR_SUCCESS;
bool reject_private = false;
bool runtime_gaze_active = false;
int suggestions = 0, attachments = 0, syncs = 0, state_queries = 0, source_queries = 0, gaze_locates = 0;
std::vector<XrActionSuggestedBinding> runtime_bindings;
std::vector<XrActionSet> runtime_sets;
std::vector<XrActiveActionSet> runtime_active_sets;
const void* runtime_next = nullptr;
XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
bool event_pending = false;
XrResult XRAPI_CALL StringToPath(XrInstance, const char* text, XrPath* out) {
    *out = std::strcmp(text, "/interaction_profiles/ext/eye_gaze_interaction") == 0 ? profile : pose;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSet(XrInstance, const XrActionSetCreateInfo* info, XrActionSet* out) {
    Expect(info->priority == 0, "private action priority must remain zero");
    *out = private_set;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* out) {
    if (set == private_set) Expect(info->countSubactionPaths == 0, "private gaze has no subactions");
    *out = set == private_set ? private_action : app_created_action;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* out) {
    *out = Handle<XrSpace>(8);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    ++suggestions;
    runtime_next = info->next;
    bool has_private = false;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i)
        has_private |= info->suggestedBindings[i].action == private_action;
    const auto result = reject_private && has_private ? XR_ERROR_PATH_UNSUPPORTED : suggestion_result;
    if (result == XR_SUCCESS)
        runtime_bindings.assign(info->suggestedBindings, info->suggestedBindings + info->countSuggestedBindings);
    return result;
}
XrResult XRAPI_CALL Attach(XrSession, const XrSessionActionSetsAttachInfo* info) {
    ++attachments;
    runtime_next = info->next;
    runtime_sets.clear();
    if (info->countActionSets) runtime_sets.assign(info->actionSets, info->actionSets + info->countActionSets);
    return attachment_result;
}
XrResult XRAPI_CALL Sync(XrSession, const XrActionsSyncInfo* info) {
    ++syncs;
    runtime_next = info->next;
    runtime_active_sets.clear();
    if (info->countActiveActionSets)
        runtime_active_sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    return sync_result;
}
XrResult XRAPI_CALL State(XrSession, const XrActionStateGetInfo*, XrActionStatePose* out) {
    ++state_queries;
    out->isActive = runtime_gaze_active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Sources(XrSession, const XrBoundSourcesForActionEnumerateInfo*, uint32_t, uint32_t* count, XrPath*) {
    ++source_queries; *count = 1; return XR_SUCCESS;
}
XrResult XRAPI_CALL Locate(XrSpace space, XrSpace, XrTime time, XrSpaceLocation* out) {
    ++gaze_locates;
    Expect(space == Handle<XrSpace>(8) && time == 123456, "private gaze space and requested time must reach runtime");
    out->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    out->pose.orientation.w = 1;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroyAction(XrAction) { return XR_SUCCESS; }
XrResult XRAPI_CALL DestroySet(XrActionSet) { return XR_SUCCESS; }
XrResult XRAPI_CALL Poll(XrInstance, XrEventDataBuffer* out) {
    if (!event_pending) return XR_EVENT_UNAVAILABLE;
    *out = event; event_pending = false; return XR_SUCCESS;
}
auto& Layer() { return depthxr::OpenXrLayer::Instance(); }
void MakeAction(XrAction action) {
    app_created_action = action;
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO}; info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    XrAction out{};
    Expect(Layer().CreateAction(app_set, &info, &out) == XR_SUCCESS && out == action, "create app action");
}
XrResult Bind(std::initializer_list<XrAction> actions, const void* next = nullptr, XrPath target = profile) {
    std::vector<XrActionSuggestedBinding> bindings;
    for (auto action : actions) bindings.push_back({action, pose});
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, next};
    info.interactionProfile = target; info.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    info.suggestedBindings = bindings.data();
    return Layer().SuggestInteractionProfileBindings(instance, &info);
}
XrResult AttachApp(const void* next = nullptr) {
    XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, next};
    info.countActionSets = 1; info.actionSets = &app_set;
    return Layer().AttachSessionActionSets(session, &info);
}
XrResult SyncApp(const void* next = nullptr) {
    const XrActiveActionSet active{app_set, 55};
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, next};
    info.countActiveActionSets = 1; info.activeActionSets = &active;
    return Layer().SyncActions(session, &info);
}
void FocusEvent(XrSessionState state, XrSession target = session) {
    event = {XR_TYPE_EVENT_DATA_BUFFER};
    auto& change = reinterpret_cast<XrEventDataSessionStateChanged&>(event);
    change.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED; change.session = target; change.state = state;
    change.time = 123456; event_pending = true;
    XrEventDataBuffer out{XR_TYPE_EVENT_DATA_BUFFER};
    Expect(Layer().PollEvent(instance, &out) == XR_SUCCESS && std::memcmp(&out, &event, sizeof(event)) == 0,
           "session event must reach application unchanged");
}
}

namespace depthxr {
class EyeGazeTestPeer {
public:
    static void Prepare() {
        auto& l = Layer();
        l.ResetInstanceState(); l.ResetSessionState(); SetupSession();
        l.instance_ = instance;
        l.eye_gaze_extension_enabled_ = true;
        l.next_string_to_path_ = &StringToPath;
        l.next_create_action_set_ = &CreateSet;
        l.next_create_action_ = &::CreateAction;
        l.next_create_action_space_ = &CreateSpace;
        l.next_destroy_action_ = &::DestroyAction;
        l.next_destroy_action_set_ = &DestroySet;
        l.next_suggest_interaction_profile_bindings_ = &Suggest;
        l.next_attach_session_action_sets_ = &Attach;
        l.next_sync_actions_ = &Sync;
        l.next_get_action_state_pose_ = &State;
        l.next_enumerate_bound_sources_for_action_ = &Sources;
        l.next_locate_space_ = &Locate;
        l.next_poll_event_ = &Poll;
        suggestions = attachments = syncs = state_queries = source_queries = gaze_locates = 0;
        suggestion_result = attachment_result = sync_result = XR_SUCCESS;
        reject_private = event_pending = runtime_gaze_active = false;
        runtime_bindings.clear(); runtime_sets.clear(); runtime_active_sets.clear(); runtime_next = nullptr;
    }
    static void SetupSession() { Layer().active_session_ = session; }
    static void Resources() { Expect(Layer().CreateEyeGazeResources(session) == XR_SUCCESS, "create private resources"); }
    static void Fallback() { Layer().TryAttachEyeGazeActionSetFallback(session); }
    static void Frames(int count) { for (int i = 0; i < count; ++i) Layer().eye_gaze_action_set_attachment_.NoteCompletedFrame(); }
    static bool Gaze() {
        QuadViewsResolvedSettings settings{}; settings.tracking_mode = QuadViewsTrackingMode::Eye;
        double yaw{}, pitch{};
        return Layer().LocateEyeGazeFocusOffsets(session, Handle<XrSpace>(9), 123456, settings, &yaw, &pitch);
    }
    static bool GoodAppSync() { return Layer().last_app_action_sync_time_.has_value(); }
    static bool GoodPrivateSync() { return Layer().last_eye_gaze_self_sync_time_.has_value(); }
    static void AgeAttempts() {
        const auto old = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        Layer().last_app_action_sync_attempt_time_ = old;
        Layer().last_eye_gaze_self_sync_attempt_time_ = old;
    }
    static void NewSession() { Layer().ResetSessionState(); SetupSession(); }
};
}

int main() {
    using Peer = depthxr::EyeGazeTestPeer;
    try {
        Peer::Prepare(); MakeAction(app_action); Bind({app_action}); Peer::Resources();
        Expect(suggestions == 1, "resource creation must not replace application suggestions");
        Bind({app_action, other_action, other_action});
        Expect(AttachApp() == XR_SUCCESS && runtime_sets.size() == 2, "private set appended at attachment");
        Expect(runtime_bindings.size() == 3 && runtime_bindings[0].action == app_action &&
               runtime_bindings[1].action == other_action && runtime_bindings[2].action == private_action,
               "latest app bindings must be preserved and deduplicated");
        const int attached_suggestions = suggestions;
        attachment_result = XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        Expect(AttachApp() == attachment_result && suggestions == attached_suggestions, "never rebind after attachment");

        Peer::Prepare(); Peer::Resources(); MakeAction(app_action); MakeAction(other_action);
        Bind({app_action}); Bind({other_action}); AttachApp();
        Expect(runtime_bindings.size() == 2 && runtime_bindings[0].action == other_action,
               "replacement suggestions must not accumulate old app bindings");
        Peer::Prepare(); MakeAction(app_action); Bind({app_action});
        suggestion_result = XR_ERROR_PATH_UNSUPPORTED;
        Expect(Bind({other_action}) == suggestion_result, "original suggestion failure must reach app");
        suggestion_result = XR_SUCCESS; Peer::Resources(); AttachApp();
        Expect(runtime_bindings[0].action == app_action, "failed original suggestion must not replace cache");

        Peer::Prepare(); MakeAction(app_action); Bind({app_action}); Peer::Resources(); reject_private = true;
        Expect(AttachApp() == XR_SUCCESS && runtime_sets == std::vector<XrActionSet>{app_set} &&
               runtime_bindings.size() == 1 && runtime_bindings[0].action == app_action,
               "augmentation failure must preserve app bindings and attach");
        const int failure_suggestions = suggestions;
        Peer::Gaze(); Expect(syncs == 0, "disabled private binding must not sync");
        attachment_result = XR_ERROR_ACTIONSETS_ALREADY_ATTACHED; AttachApp();
        Expect(suggestions == failure_suggestions, "optional binding failure must not cause live retry");

        XrBaseInStructure chain{XR_TYPE_UNKNOWN, nullptr};
        Peer::Prepare(); Peer::Resources(); MakeAction(app_action); Bind({app_action}, &chain);
        Expect(runtime_next == &chain, "original binding extension chain forwarded");
        AttachApp(&chain);
        Expect(suggestions == 1 && runtime_sets.size() == 1 && runtime_next == &chain,
               "unsupported binding metadata must not be replayed or stripped");
        Peer::Prepare(); Peer::Resources(); Bind({app_action}, &chain); Bind({other_action}); AttachApp();
        Expect(runtime_sets.size() == 2 && runtime_bindings.size() == 2 && runtime_bindings[0].action == other_action,
               "a later plain suggestion must supersede earlier unsupported metadata");
        Peer::Prepare(); Peer::Resources(); Bind({app_action}, nullptr, 99); AttachApp();
        Expect(runtime_bindings.size() == 1 && runtime_bindings[0].action == private_action,
               "other profiles must not contribute bindings to the gaze profile");

        Peer::Prepare(); MakeAction(app_action); MakeAction(other_action); Bind({app_action, other_action});
        Layer().DestroyAction(app_action); Peer::Resources(); AttachApp();
        Expect(runtime_bindings.size() == 2 && runtime_bindings[0].action == other_action, "no destroyed action replay");
        Peer::Prepare(); MakeAction(app_action); Bind({app_action}); Layer().DestroyActionSet(app_set);
        Peer::Resources(); AttachApp();
        Expect(runtime_bindings.size() == 1 && runtime_bindings[0].action == private_action, "no destroyed set replay");
        Peer::Prepare(); MakeAction(app_action); Bind({app_action}); Peer::Resources();
        attachment_result = XR_ERROR_HANDLE_INVALID;
        Expect(AttachApp() == XR_ERROR_HANDLE_INVALID, "original application attachment failure must reach app");
        Layer().DestroyAction(app_action); attachment_result = XR_SUCCESS; AttachApp();
        Expect(runtime_bindings.size() == 1 && runtime_bindings[0].action == private_action,
               "attachment retry must not replay an action destroyed after finalization");

        Peer::Prepare(); Peer::Resources(); Peer::Frames(100); Peer::Fallback();
        Expect(attachments == 0 && suggestions == 0, "fallback attachment must retain grace period");
        Peer::Frames(1); Peer::Fallback(); Peer::Fallback();
        Expect(attachments == 1 && suggestions == 1 && runtime_sets == std::vector<XrActionSet>{private_set},
               "no-action app finalizes and attaches once after grace period");
        Peer::Prepare(); Peer::Resources(); Peer::Frames(101); attachment_result = XR_ERROR_RUNTIME_FAILURE;
        Peer::Fallback(); Peer::Fallback();
        Expect(attachments == 1 && suggestions == 1, "failed fallback attachment must not retry");

        Peer::Prepare(); MakeAction(app_action); Bind({app_action}); Peer::Resources(); AttachApp();
        Peer::NewSession(); Peer::Resources(); AttachApp();
        Expect(runtime_bindings.size() == 2 && runtime_bindings[0].action == app_action,
               "instance-owned suggestions survive session recreation");
        Peer::Prepare(); Peer::Resources(); AttachApp();
        Expect(runtime_bindings.size() == 1, "instance reset must clear old suggestions");

        Expect(SyncApp(&chain) == XR_SUCCESS && Peer::GoodAppSync() && runtime_active_sets.size() == 2 &&
               runtime_active_sets[0].actionSet == app_set && runtime_active_sets[0].subactionPath == 55 &&
               runtime_active_sets[1].actionSet == private_set && runtime_next == &chain,
               "app sync must preserve active sets, subaction paths, next chain, and result");
        sync_result = XR_SESSION_NOT_FOCUSED;
        Expect(SyncApp() == XR_SESSION_NOT_FOCUSED && !Peer::GoodAppSync(), "positive unfocused status is not focused success");
        Peer::Gaze(); Peer::Gaze();
        Expect(syncs == 2 && state_queries == 0, "fresh unfocused app attempt must throttle private retry/query");
        FocusEvent(XR_SESSION_STATE_VISIBLE); Peer::AgeAttempts(); Peer::Gaze();
        Expect(syncs == 2, "known lost focus must suspend private sync");
        FocusEvent(XR_SESSION_STATE_FOCUSED); sync_result = XR_SUCCESS; Peer::Gaze();
        Expect(syncs == 3 && state_queries == 1 && Peer::GoodPrivateSync(), "focus return must allow current focused input");
        Peer::Gaze(); Expect(syncs == 3 && source_queries <= 1, "fresh sync and source probes must be bounded");
        FocusEvent(XR_SESSION_STATE_VISIBLE, Handle<XrSession>(99)); Peer::Gaze();
        Expect(state_queries == 3, "another session's events must not change focus");
        runtime_gaze_active = true;
        Expect(Peer::Gaze() && gaze_locates == 1, "focused active action must yield tracked gaze");
        FocusEvent(XR_SESSION_STATE_VISIBLE);
        Expect(!Peer::Gaze() && gaze_locates == 1, "focus loss must reject stale active gaze");
        FocusEvent(XR_SESSION_STATE_FOCUSED);
        Expect(Peer::Gaze() && gaze_locates == 2, "focus return must reacquire valid gaze");
        runtime_gaze_active = false; // Model a higher-priority application binding suppressing ours.
        const int before_suppression_suggestions = suggestions;
        Expect(!Peer::Gaze() && suggestions == before_suppression_suggestions && attachments == 1,
               "inactive private action must use fallback without overriding priority, rebind, or reattach");
        sync_result = XR_ERROR_RUNTIME_FAILURE;
        Expect(SyncApp(&chain) == XR_ERROR_RUNTIME_FAILURE && !Peer::GoodAppSync() && runtime_next == &chain,
               "application sync errors and extension metadata must be preserved");

        Peer::Prepare(); Peer::Resources(); AttachApp(); sync_result = XR_SESSION_NOT_FOCUSED;
        Peer::Gaze(); Peer::Gaze();
        Expect(syncs == 1 && state_queries == 0 && !Peer::GoodPrivateSync(), "unfocused private attempt must be throttled");
        Peer::AgeAttempts(); sync_result = XR_SUCCESS; Peer::Gaze();
        Expect(syncs == 2 && state_queries == 1, "unknown focus must recover without app polling");
        Peer::AgeAttempts(); sync_result = XR_ERROR_RUNTIME_FAILURE; Peer::Gaze(); Peer::Gaze();
        Expect(syncs == 3 && state_queries == 1, "failed private attempts must be throttled too");
        Bind({app_action}); const int replaced_suggestions = suggestions; Peer::Gaze();
        Expect(syncs == 3 && suggestions == replaced_suggestions, "late replacement must disable gaze without rebinding");
        Peer::NewSession(); Peer::Resources(); AttachApp();
        Expect(!Peer::GoodAppSync() && !Peer::GoodPrivateSync(), "session recreation must reset focus and sync freshness");
        std::cout << "Eye-gaze handshake tests passed.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
