#include "depthxr/openxr_layer.h"
#include "depthxr/runtime_compatibility.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <Windows.h>

namespace {
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::size_t Occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}
template<class T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
const auto instance = Handle<XrInstance>(1);
const auto session = Handle<XrSession>(2);
const auto app_set = Handle<XrActionSet>(3);
const auto private_set = Handle<XrActionSet>(4);
const auto app_action = Handle<XrAction>(5);
const auto other_action = Handle<XrAction>(6);
const auto private_action = Handle<XrAction>(7);
// An application action set created after attachment; it was never attached.
const auto late_set = Handle<XrActionSet>(10);
const auto late_action = Handle<XrAction>(11);
constexpr XrPath profile = 65, pose = 66;
XrAction app_created_action = app_action;
XrResult suggestion_result = XR_SUCCESS, attachment_result = XR_SUCCESS, sync_result = XR_SUCCESS;
bool reject_private = false;
bool runtime_gaze_active = false;
int suggestions = 0, attachments = 0, syncs = 0, state_queries = 0, source_queries = 0, gaze_locates = 0;
std::vector<XrActionSuggestedBinding> runtime_bindings;
std::vector<XrActionSet> runtime_sets;
std::vector<XrActiveActionSet> runtime_active_sets;
// Spec: a suggestion naming an action whose set was ever attached (in any
// session of the instance) fails with XR_ERROR_ACTIONSETS_ALREADY_ATTACHED.
std::set<XrActionSet> runtime_attached;
std::map<XrAction, XrActionSet> runtime_owner;
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
    runtime_owner[*out] = set;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* out) {
    *out = Handle<XrSpace>(8);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding* info) {
    ++suggestions;
    runtime_next = info->next;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        const auto owner = runtime_owner.find(info->suggestedBindings[i].action);
        if (owner != runtime_owner.end() && runtime_attached.contains(owner->second))
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
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
    if (XR_SUCCEEDED(attachment_result)) runtime_attached.insert(runtime_sets.begin(), runtime_sets.end());
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
// A stand-in runtime for Varjo focus sharpening's output swapchains.
int sharpen_creates = 0, sharpen_destroys = 0, sharpen_acquires = 0, sharpen_waits = 0, sharpen_releases = 0;
uint32_t sharpen_next_image = 0;
XrResult sharpen_wait_result = XR_SUCCESS;
XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* out) {
    ++sharpen_creates; *out = Handle<XrSwapchain>(300); return XR_SUCCESS;
}
// Images with no texture (the layer's zeroed structures): with no device, no
// render target view can be made for them.
XrResult XRAPI_CALL EnumerateImages(XrSwapchain, uint32_t, uint32_t* count, XrSwapchainImageBaseHeader*) {
    *count = 3;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySwapchain(XrSwapchain) { ++sharpen_destroys; return XR_SUCCESS; }
XrResult XRAPI_CALL AcquireImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    ++sharpen_acquires; *index = sharpen_next_image++ % 3; return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitImage(XrSwapchain, const XrSwapchainImageWaitInfo*) { ++sharpen_waits; return sharpen_wait_result; }
XrResult XRAPI_CALL ReleaseImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) { ++sharpen_releases; return XR_SUCCESS; }
auto& Layer() { return depthxr::OpenXrLayer::Instance(); }
void MakeAction(XrAction action, XrActionSet set = app_set) {
    app_created_action = action;
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO}; info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    XrAction out{};
    Expect(Layer().CreateAction(set, &info, &out) == XR_SUCCESS && out == action, "create app action");
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
        runtime_attached.clear(); runtime_owner.clear();
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
    static void NewSession() { Layer().ResetSessionState(); SetupSession(); runtime_attached.erase(private_set); }
    // A live Tracking Mode change in an emulated quadviews session.
    static void Track(QuadViewsTrackingMode mode) {
        auto& l = Layer();
        l.quadviews_session_active_ = true; l.d3d11_graphics_extension_requested_ = true;
        const auto previous = l.resolved_settings_.quadviews.tracking_mode;
        l.resolved_settings_.quadviews.tracking_mode = mode;
        l.UpdateEyeGazeResources(previous);
    }
    static bool Ready() { return Layer().eye_gaze_resources_ready_; }
    // A swapchain of the quad views session with enumerated images, as
    // CreateSwapchain and EnumerateSwapchainImages track it.
    static void QuadSwapchain(uintptr_t handle, XrSwapchainUsageFlags usage) {
        OpenXrLayer::SwapchainInfo info;
        info.session = session;
        info.width = info.height = 2048;
        info.format = 29;
        info.usage_flags = usage;
        info.quadviews_session = true;
        info.d3d11_images.push_back(nullptr);
        Layer().tracked_swapchains_[Handle<XrSwapchain>(handle)] = std::move(info);
    }
    // Varjo focus sharpening's left output target, against the stand-in runtime.
    static void SharpenRuntime() {
        auto& l = Layer();
        l.next_create_swapchain_ = &CreateSwapchain;
        l.next_enumerate_swapchain_images_ = &EnumerateImages;
        l.next_destroy_swapchain_ = &DestroySwapchain;
        l.next_acquire_swapchain_image_ = &AcquireImage;
        l.next_wait_swapchain_image_ = &WaitImage;
        l.next_release_swapchain_image_ = &ReleaseImage;
        sharpen_creates = sharpen_destroys = sharpen_acquires = sharpen_waits = sharpen_releases = 0;
        sharpen_next_image = 0;
        sharpen_wait_result = XR_SUCCESS;
        SharpenReset();
    }
    static void SharpenReset() {
        Layer().d3d11_focus_sharpen_.targets = {};
        Layer().d3d11_focus_sharpen_.failed_shapes = {};
    }
    static bool SharpenTarget(uint32_t width, uint32_t height) {
        return Layer().EnsureFocusSharpenTarget(Layer().d3d11_focus_sharpen_.targets[0], width, height, 29);
    }
    // A built target with three images (render target views stand-ins).
    static void SharpenTargetBuilt() {
        auto& target = Layer().d3d11_focus_sharpen_.targets[0];
        target.swapchain = Handle<XrSwapchain>(300);
        target.image_render_target_views.assign(3, nullptr);
    }
    static bool SharpenImage(uint32_t* index, const char** reason) {
        return Layer().AcquireFocusSharpenImage(Layer().d3d11_focus_sharpen_.targets[0], index, reason);
    }
    // The swapchains the compositor prewarm takes as its four inputs.
    static std::set<uintptr_t> PrewarmInputs() {
        std::array<OpenXrLayer::SwapchainInfo*, 4> inputs{};
        std::set<uintptr_t> handles;
        if (!Layer().SelectQuadViewsPrewarmInputs(&inputs)) return handles;
        for (const auto& [handle, info] : Layer().tracked_swapchains_) {
            if (std::find(inputs.begin(), inputs.end(), &info) != inputs.end())
                handles.insert(reinterpret_cast<uintptr_t>(handle));
        }
        return handles;
    }
    static void WithoutGazeExtension() { Layer().eye_gaze_extension_enabled_ = false; }
    // Logs to a file of its own at `path`; a second call lets the first go.
    static void LogTo(const std::filesystem::path& path) { Layer().logger_.Initialize(path); }
    // The log so far: an error reaches the file at once, with everything
    // logged before it.
    static std::string Log() {
        static int reads = 0;
        Layer().logger_.Error("Eye-gaze tests: log read " + std::to_string(++reads) + ".");
        std::ifstream stream(Layer().logger_.ActiveLogPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }
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
        Peer::NewSession(); Peer::Resources();
        Expect(AttachApp() == XR_SUCCESS && runtime_bindings.size() == 1 &&
                   runtime_bindings[0].action == private_action && runtime_sets.size() == 2,
               "a later session must keep private gaze when attached app actions cannot be re-suggested");
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
        MakeAction(late_action, late_set); Bind({late_action}); const int replaced_suggestions = suggestions;
        Peer::AgeAttempts(); sync_result = XR_SUCCESS; Peer::Gaze();
        Expect(syncs == 4 && suggestions == replaced_suggestions,
               "a late suggestion for an unattached set must neither rebind nor disable this session's gaze");
        MakeAction(app_action);
        Expect(Bind({app_action}) == XR_ERROR_ACTIONSETS_ALREADY_ATTACHED,
               "re-suggesting attached application actions must fail as the runtime reports");
        Peer::NewSession(); Peer::Resources(); AttachApp();
        Expect(!Peer::GoodAppSync() && !Peer::GoodPrivateSync(), "session recreation must reset focus and sync freshness");

        using Mode = depthxr::QuadViewsTrackingMode;
        Peer::Prepare(); Peer::Track(Mode::Head); Peer::Track(Mode::Eye); Peer::Track(Mode::Head);
        Expect(!Peer::Ready(), "a switch to Head before attachment must release the private gaze set");
        Peer::Track(Mode::Eye); MakeAction(app_action); Bind({app_action});
        Expect(AttachApp() == XR_SUCCESS && runtime_sets.size() == 2, "a switch back to Eye before attachment must attach gaze");
        Peer::Track(Mode::Head); Peer::Track(Mode::Eye); runtime_gaze_active = true;
        Expect(Peer::Ready() && Peer::Gaze() && gaze_locates == 1 && attachments == 1 && suggestions == 2,
               "Eye -> Head -> Eye after attachment must keep the attached gaze set");
        Peer::Prepare(); Peer::Track(Mode::Head); MakeAction(app_action); Bind({app_action}); AttachApp();
        Peer::Track(Mode::Eye); Peer::Track(Mode::Head); Peer::Track(Mode::Eye); runtime_gaze_active = true;
        Expect(!Peer::Ready() && !Peer::Gaze() && attachments == 1 && suggestions == 1 && runtime_sets.size() == 1,
               "Head -> Eye after an attachment without gaze must not create an unattachable set");

        // Varjo focus sharpening: an output target that cannot be built
        // (here: no device for its render target views) leaves no runtime
        // swapchain behind and is not rebuilt every frame; a new size is.
        Peer::Prepare(); Peer::SharpenRuntime();
        Expect(!Peer::SharpenTarget(1000, 800) && sharpen_creates == 1 && sharpen_destroys == 1,
               "a sharpen target that cannot be built must leave no swapchain behind");
        Expect(!Peer::SharpenTarget(1000, 800) && sharpen_creates == 1 && sharpen_destroys == 1,
               "a sharpen target must not be rebuilt every frame for the same size and format");
        Expect(!Peer::SharpenTarget(1200, 800) && sharpen_creates == 2, "a new size must be tried again");
        // A timed-out wait keeps its image: not drawn into, not released, and
        // waited for again next frame instead of a second image acquired.
        Peer::SharpenRuntime(); Peer::SharpenTargetBuilt();
        sharpen_wait_result = XR_TIMEOUT_EXPIRED;
        uint32_t timed_out_image = 99, image = 99;
        const char* reason = "ok";
        Expect(!Peer::SharpenImage(&timed_out_image, &reason) && sharpen_acquires == 1 && sharpen_releases == 0 &&
                   std::string(reason) == "sharpen output wait timed out",
               "a timed-out sharpen output wait must not be drawn into or released");
        sharpen_wait_result = XR_SUCCESS;
        Expect(Peer::SharpenImage(&image, &reason) && sharpen_acquires == 1 && sharpen_waits == 2 &&
                   image == timed_out_image,
               "the next frame must wait for the same image, not acquire another");
        Expect(Peer::SharpenImage(&image, &reason) && sharpen_acquires == 2, "then images are acquired as usual");
        Peer::SharpenReset();

        // Without XR_EXT_eye_gaze_interaction downstream, a switch to Eye says
        // once that gaze is unavailable; settings reloads that keep Eye tracked
        // neither retry nor log a tracking-mode change.
        const auto logs = std::filesystem::temp_directory_path() /
                          ("vectorxr-eye-gaze-tests-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(logs);
        Peer::Prepare(); Peer::WithoutGazeExtension(); Peer::LogTo(logs / "eye-gaze.log");
        Peer::Track(Mode::Head); Peer::Track(Mode::Eye); Peer::Track(Mode::Eye); Peer::Track(Mode::Eye);
        const std::string text = Peer::Log();
        Expect(Occurrences(text, "is not enabled downstream") == 1 &&
                   Occurrences(text, "after quadviews tracking-mode change") == 1 && !Peer::Ready(),
               "settings reloads without the eye-gaze extension must not retry gaze or log a mode change");
        Peer::LogTo(logs / "eye-gaze.log");
        std::error_code error;
        std::filesystem::remove_all(logs, error);

        // An instance the runtime refuses over one of the layer's additions is
        // created without that one alone: a refused eye-gaze extension must
        // not take Frame Generation's XR_KHR_D3D12_enable (or the clock) along.
        using Names = std::vector<std::string>;
        const char* const d3d11 = "XR_KHR_D3D11_enable";
        const char* const d3d12 = "XR_KHR_D3D12_enable";
        const char* const gaze = "XR_EXT_eye_gaze_interaction";
        const char* const clock_extension = "XR_KHR_win32_convert_performance_counter_time";
        const std::vector<const char*> first{d3d11, d3d12, gaze, clock_extension};
        const std::vector<const char*> additions{d3d12, gaze, clock_extension};
        // The list the instance is created with when the runtime refuses `refused`.
        const auto created_with = [&](std::initializer_list<const char*> refused) {
            for (const auto& retry : depthxr::InstanceExtensionRetries(first, additions)) {
                const bool accepted = std::none_of(retry.begin(), retry.end(), [&](const char* name) {
                    return std::any_of(refused.begin(), refused.end(),
                                       [&](const char* no) { return std::string(name) == no; });
                });
                if (accepted) return Names(retry.begin(), retry.end());
            }
            return Names{};
        };
        Expect(created_with({gaze}) == Names{d3d11, d3d12, clock_extension},
               "a refused eye-gaze extension must keep XR_KHR_D3D12_enable and the clock");
        Expect(created_with({clock_extension}) == Names{d3d11, d3d12, gaze}, "a refused clock must keep the others");
        const auto retries = depthxr::InstanceExtensionRetries(first, additions);
        Expect(retries.size() == 4 &&
                   Names(retries[0].begin(), retries[0].end()) == Names{d3d11, gaze, clock_extension},
               "XR_KHR_D3D12_enable is dropped alone on the first retry");
        Expect(created_with({d3d12, gaze}) == Names{d3d11},
               "when no single addition explains the refusal, the application's own list is used");
        const std::vector<const char*> without_clock{d3d11, d3d12};
        Expect(depthxr::InstanceExtensionRetries(without_clock, additions).size() == 1,
               "additions the first attempt did not carry are not retried");

        // The compositor prewarm takes the game's four views, not another
        // layer's colour-only swapchain (OpenKneeboard) or a depth swapchain.
        Peer::Prepare();
        constexpr XrSwapchainUsageFlags game_usage =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        Peer::QuadSwapchain(200, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
        Peer::QuadSwapchain(201, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
        Peer::QuadSwapchain(202, game_usage); Peer::QuadSwapchain(203, game_usage); Peer::QuadSwapchain(204, game_usage);
        Expect(Peer::PrewarmInputs().empty(), "three game views and two others must not prewarm the compositor");
        Peer::QuadSwapchain(205, game_usage);
        Expect(Peer::PrewarmInputs() == std::set<uintptr_t>{202, 203, 204, 205},
               "the prewarm must take the game's four views only");
        Peer::Prepare();
        std::cout << "Eye-gaze handshake tests passed.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
