#include "depthxr/openxr_layer.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
const auto instance = Handle<XrInstance>(1);
const auto session = Handle<XrSession>(2);
const auto action_set = Handle<XrActionSet>(3);
const auto view = Handle<XrSpace>(4);
const auto local = Handle<XrSpace>(5);
constexpr XrPath eye_profile = 65, eye_pose = 66, eye_user = 67;
XrAction next_action = Handle<XrAction>(10);
XrSpace next_space = Handle<XrSpace>(20);
XrResult mutation_result = XR_SUCCESS, locate_result = XR_SUCCESS;
XrSpaceLocationFlags flags = 15;
int locate_calls = 0;
XrPosef runtime_pose{{0.02f, -0.03f, 0, 0.9993497736f}, {0.001f, 0.002f, -0.003f}};
XrResult XRAPI_CALL CreateAction(XrActionSet, const XrActionCreateInfo*, XrAction* out) {
    if (XR_SUCCEEDED(mutation_result)) *out = next_action;
    return mutation_result;
}
XrResult XRAPI_CALL CreateSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* out) {
    if (XR_SUCCEEDED(mutation_result)) *out = next_space;
    return mutation_result;
}
XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding*) { return mutation_result; }
XrResult XRAPI_CALL DestroyAction(XrAction) { return mutation_result; }
XrResult XRAPI_CALL DestroySet(XrActionSet) { return mutation_result; }
XrResult XRAPI_CALL DestroySpace(XrSpace) { return mutation_result; }
XrResult XRAPI_CALL StringToPath(XrInstance, const char* text, XrPath* out) {
    *out = std::strcmp(text, "/interaction_profiles/ext/eye_gaze_interaction") == 0 ? eye_profile :
           std::strcmp(text, "/user/eyes_ext") == 0 ? eye_user : eye_pose;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL Locate(XrSpace, XrSpace, XrTime time, XrSpaceLocation* out) {
    ++locate_calls;
    Expect(time == 123456, "requested gaze time must reach runtime unchanged");
    if (XR_SUCCEEDED(locate_result)) {
        out->pose = runtime_pose;
        out->locationFlags = flags;
        if (out->next) reinterpret_cast<XrEyeGazeSampleTimeEXT*>(out->next)->time = 123000;
    }
    return locate_result;
}
template<class T> T Dispatch(const char* name) {
    PFN_xrVoidFunction function = nullptr;
    Expect(xrGetInstanceProcAddr(instance, name, &function) == XR_SUCCESS && function,
           "gaze tracking dispatch must be available");
    return reinterpret_cast<T>(function);
}
XrAction MakeAction(uintptr_t handle, const char* name = "user_intent") {
    next_action = Handle<XrAction>(handle);
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(info.actionName, name, sizeof(info.actionName)-1);
    XrAction result = XR_NULL_HANDLE;
    Expect(Dispatch<PFN_xrCreateAction>("xrCreateAction")(action_set, &info, &result) == mutation_result,
           "create action result must be preserved");
    return result;
}
XrSpace MakeSpace(XrAction action, uintptr_t handle, XrPath subaction = XR_NULL_PATH) {
    next_space = Handle<XrSpace>(handle);
    XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action = action;
    info.subactionPath = subaction;
    info.poseInActionSpace.orientation.w = 1;
    XrSpace result = XR_NULL_HANDLE;
    Expect(Dispatch<PFN_xrCreateActionSpace>("xrCreateActionSpace")(session, &info, &result) == mutation_result,
           "create space result must be preserved");
    return result;
}
void Bind(XrAction action, XrPath profile = eye_profile, XrPath path = eye_pose) {
    XrActionSuggestedBinding binding{action, path};
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = profile;
    info.countSuggestedBindings = 1;
    info.suggestedBindings = &binding;
    Expect(Dispatch<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings")(instance, &info)
           == mutation_result, "binding result must be preserved");
}
void CheckPassThrough(XrSpace space, XrSpace base) {
    XrEyeGazeSampleTimeEXT sample{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
    XrSpaceLocation out{XR_TYPE_SPACE_LOCATION, &sample};
    const int before = locate_calls;
    Expect(Dispatch<PFN_xrLocateSpace>("xrLocateSpace")(space, base, 123456, &out) == locate_result,
           "runtime locate result changed");
    Expect(locate_calls == before + 1, "gaze bypass must not issue additional runtime queries");
    if (XR_SUCCEEDED(locate_result)) {
        Expect(std::memcmp(&out.pose, &runtime_pose, sizeof(runtime_pose)) == 0,
               "head-relative gaze must preserve runtime position and orientation exactly");
        Expect(out.locationFlags == flags && out.next == &sample && sample.time == 123000,
               "gaze bypass must preserve flags, next chain, and sample time");
    }
}
}

namespace depthxr {
class PivotGazeTestPeer {
public:
    static void Prepare() {
        auto& l = OpenXrLayer::Instance();
        l.ResetInstanceState();
        l.ResetSessionState();
        l.has_loaded_config_ = true;
        l.instance_ = instance;
        l.resolved_settings_generation_ = l.config_generation_;
        l.resolved_settings_session_ = l.active_session_;
        l.resolved_settings_ = {};
        l.resolved_settings_.core.enabled = true;
        l.resolved_settings_.pivotxr.enabled = true;
        l.resolved_settings_.pivotxr.profiles = {PivotXrResolvedProfile{}};
        l.pivotxr_activation_gain_ = 1;
        l.pivotxr_smoothed_extra_yaw_radians_ = 0.65;
        l.pivotxr_smoothed_extra_pitch_radians_ = 0.2;
        l.tracked_view_spaces_ = {view};
        l.tracked_local_spaces_ = {local};
        l.next_create_action_ = &::CreateAction;
        l.next_destroy_action_ = &::DestroyAction;
        l.next_destroy_action_set_ = &::DestroySet;
        l.next_create_action_space_ = &::CreateSpace;
        l.next_destroy_space_ = &::DestroySpace;
        l.next_suggest_interaction_profile_bindings_ = &::Suggest;
        l.next_string_to_path_ = &::StringToPath;
        l.next_locate_space_ = &::Locate;
    }
    static bool Exempt(XrSpace space, XrSpace base = view) {
        return OpenXrLayer::Instance().IsHeadRelativeEyeGaze(space, base);
    }
    static void ResetSession() { OpenXrLayer::Instance().ResetSessionState(); }
    static void RestoreView() { OpenXrLayer::Instance().tracked_view_spaces_ = {view}; }
    static void ResetInstance() { OpenXrLayer::Instance().ResetInstanceState(); }
    static void CheckOtherTransforms(XrSpace controller) {
        auto& l = OpenXrLayer::Instance();
        // Exercise the actual public locate path, including the inverse VIEW
        // query that previously caught eye gaze, with an ordinary controller.
        XrSpaceLocation out{XR_TYPE_SPACE_LOCATION};
        l.LocateSpace(controller, view, 123456, &out);
        Expect(std::memcmp(&out.pose, &runtime_pose, sizeof(runtime_pose)) != 0,
               "controller-relative VIEW transform must still apply Pivot");
        l.LocateSpace(view, local, 123456, &out);
        Expect(std::memcmp(&out.pose, &runtime_pose, sizeof(runtime_pose)) != 0,
               "world-relative head transform must still apply Pivot");
    }
};
}

int main() {
    using Peer = depthxr::PivotGazeTestPeer;
    try {
        Peer::Prepare();
        const auto eye = MakeAction(10);
        const auto gaze = MakeSpace(eye, 20); // Spaces may precede binding suggestions.
        Expect(!Peer::Exempt(gaze), "unbound action must not be guessed to be gaze");
        Bind(eye);
        Expect(Peer::Exempt(gaze), "standard eye binding must identify existing space");
        Expect(Peer::Exempt(MakeSpace(eye, 30, eye_user)), "explicit eyes subaction must preserve gaze");
        Expect(!Peer::Exempt(MakeSpace(eye, 31, 101)), "hand-filtered space must not inherit an action's eye exemption");
        for (auto f : {XrSpaceLocationFlags{0}, XrSpaceLocationFlags{1}, XrSpaceLocationFlags{15}}) {
            flags = f;
            CheckPassThrough(gaze, view);
            CheckPassThrough(view, gaze);
        }
        flags = 15;
        locate_result = XR_ERROR_RUNTIME_FAILURE;
        CheckPassThrough(gaze, view);
        locate_result = XR_SUCCESS;
        Expect(!Peer::Exempt(gaze, local), "world-relative gaze must not use the VIEW-specific exemption");
        CheckPassThrough(gaze, local);
        const auto controller = MakeAction(11, "eye_gaze"); // Name alone is insufficient.
        const auto controller_space = MakeSpace(controller, 21);
        Bind(controller, 99, 100);
        Expect(!Peer::Exempt(controller_space) && Peer::Exempt(gaze), "other profiles must not replace eye bindings");
        Peer::CheckOtherTransforms(controller_space);

        mutation_result = XR_ERROR_RUNTIME_FAILURE;
        Bind(controller);
        Expect(Peer::Exempt(gaze) && !Peer::Exempt(controller_space), "failed binding must preserve classification");
        mutation_result = XR_SUCCESS;
        Bind(controller);
        Expect(!Peer::Exempt(gaze) && Peer::Exempt(controller_space), "successful binding replacement must update spaces");
        Bind(eye);
        Expect(Peer::Exempt(gaze), "rebinding must restore gaze classification");

        mutation_result = XR_ERROR_RUNTIME_FAILURE;
        Dispatch<PFN_xrDestroyAction>("xrDestroyAction")(eye);
        mutation_result = XR_SUCCESS;
        Expect(Peer::Exempt(MakeSpace(eye, 22)), "failed action destruction must retain live identity");
        Dispatch<PFN_xrDestroyAction>("xrDestroyAction")(eye);
        Expect(Peer::Exempt(gaze), "action destruction must preserve still-locatable gaze spaces");
        const auto reused_action = MakeAction(10, "controller");
        const auto reused_space = MakeSpace(reused_action, 23);
        Expect(!Peer::Exempt(reused_space) && Peer::Exempt(gaze), "reused action handle must not alias old resource");
        mutation_result = XR_ERROR_RUNTIME_FAILURE;
        Dispatch<PFN_xrDestroySpace>("xrDestroySpace")(gaze);
        Expect(Peer::Exempt(gaze), "failed space destruction must preserve identity");
        mutation_result = XR_SUCCESS;
        Dispatch<PFN_xrDestroySpace>("xrDestroySpace")(gaze);
        Expect(!Peer::Exempt(MakeSpace(reused_action, 20)), "reused space handle must not retain gaze identity");

        Bind(reused_action);
        Expect(Peer::Exempt(reused_space), "binding must reach spaces created before it");
        Dispatch<PFN_xrDestroyActionSet>("xrDestroyActionSet")(action_set);
        Expect(Peer::Exempt(reused_space), "action-set destruction must preserve referenced resources");
        const auto recreated = MakeAction(10);
        Expect(!Peer::Exempt(MakeSpace(recreated, 24)), "action-set teardown must remove live action handles");
        Bind(recreated);
        Peer::ResetSession();
        Peer::RestoreView();
        Expect(!Peer::Exempt(Handle<XrSpace>(24)), "session reset must forget destroyed spaces");
        Expect(Peer::Exempt(MakeSpace(recreated, 25)), "instance-owned bindings must survive session recreation");
        Peer::ResetInstance();
        Expect(!Peer::Exempt(Handle<XrSpace>(25)), "instance reset must clear gaze identities");
        MakeAction(10);
        Expect(!Peer::Exempt(MakeSpace(Handle<XrAction>(10), 26)), "new instance must not inherit eye bindings");
        std::cout << "Pivot eye-gaze dispatch, exact pass-through, non-gaze transforms and lifetime regressions passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
