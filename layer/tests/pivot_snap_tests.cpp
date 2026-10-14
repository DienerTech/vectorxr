#include "depthxr/config_parser.h"
#include "depthxr/openxr_layer.h"
#include "depthxr/settings_resolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Counts this executable's heap allocations while a scenario measures a call.
namespace {
std::atomic<bool> g_counting_allocations{false};
std::atomic<std::size_t> g_allocations{0};
} // namespace

void* operator new(std::size_t size) {
    if (g_counting_allocations.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

// Scenario tests for Snap Views and their interplay with Enhanced Motion.
// Every scenario drives the real xrLocateViews (Pivot drive) and xrLocateSpace
// (non-drive) entry points through a fake runtime, and feeds input through the
// real binding arbitration using cached key states.

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFrameSeconds = 1.0 / 90.0;
constexpr double kAngleToleranceDegrees = 0.02;
constexpr double kPositionToleranceMeters = 0.0005;
// Largest per-frame change an intended transition may produce in these
// scenarios (180 degrees over one second peaks at 3 degrees per frame).
constexpr double kMaxTransitionStepDegrees = 3.5;
constexpr XrTime kFrameNanoseconds = 11'111'111;
constexpr XrSpaceLocationFlags kValidPose =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;

const XrSession kSession = reinterpret_cast<XrSession>(std::uintptr_t{0x51});
const XrSpace kViewSpace = reinterpret_cast<XrSpace>(std::uintptr_t{1});
const XrSpace kLocalSpace = reinterpret_cast<XrSpace>(std::uintptr_t{2});
const XrVector3f kSeatedHead{0.0f, 1.2f, 0.0f};

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double Radians(double degrees) {
    return degrees * kPi / 180.0;
}

double Degrees(double radians) {
    return radians * 180.0 / kPi;
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
            v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

// parent * child: child is expressed in the parent's frame.
XrPosef Compose(const XrPosef& parent, const XrPosef& child) {
    const XrVector3f offset = Rotate(parent.orientation, child.position);
    return {Multiply(parent.orientation, child.orientation),
            {offset.x + parent.position.x, offset.y + parent.position.y, offset.z + parent.position.z}};
}

XrPosef Inverse(const XrPosef& pose) {
    const XrQuaternionf conjugate{-pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w};
    return {conjugate, Rotate(conjugate, {-pose.position.x, -pose.position.y, -pose.position.z})};
}

// Yaw turns left for positive angles; pitch looks up for positive angles.
XrPosef HeadPose(double yaw_degrees, double pitch_degrees) {
    const double half_yaw = Radians(yaw_degrees) / 2.0;
    const double half_pitch = Radians(pitch_degrees) / 2.0;
    const XrQuaternionf yaw{0.0f, static_cast<float>(std::sin(half_yaw)), 0.0f, static_cast<float>(std::cos(half_yaw))};
    const XrQuaternionf pitch{static_cast<float>(std::sin(half_pitch)), 0.0f, 0.0f, static_cast<float>(std::cos(half_pitch))};
    return {Multiply(yaw, pitch), kSeatedHead};
}

double WrapDegrees(double degrees) {
    while (degrees > 180.0) {
        degrees -= 360.0;
    }
    while (degrees < -180.0) {
        degrees += 360.0;
    }
    return degrees;
}

double ViewYaw(const XrPosef& pose) {
    const XrVector3f forward = Rotate(pose.orientation, {0.0f, 0.0f, -1.0f});
    return Degrees(std::atan2(-forward.x, -forward.z));
}

double ViewPitch(const XrPosef& pose) {
    const XrVector3f forward = Rotate(pose.orientation, {0.0f, 0.0f, -1.0f});
    return Degrees(std::asin(std::clamp(static_cast<double>(forward.y), -1.0, 1.0)));
}

// Rotation angle of conjugate(a) * b, computed in double precision with atan2
// so float poses that differ only by round-off compare as equal.
double AngleBetween(const XrPosef& a, const XrPosef& b) {
    const double ax = -a.orientation.x;
    const double ay = -a.orientation.y;
    const double az = -a.orientation.z;
    const double aw = a.orientation.w;
    const double bx = b.orientation.x;
    const double by = b.orientation.y;
    const double bz = b.orientation.z;
    const double bw = b.orientation.w;
    const double x = aw * bx + ax * bw + ay * bz - az * by;
    const double y = aw * by - ax * bz + ay * bw + az * bx;
    const double z = aw * bz + ax * by - ay * bx + az * bw;
    const double w = aw * bw - ax * bx - ay * by - az * bz;
    return Degrees(2.0 * std::atan2(std::sqrt(x * x + y * y + z * z), std::abs(w)));
}

double Distance(const XrVector3f& a, const XrVector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

bool SamePose(const XrPosef& a, const XrPosef& b) {
    return AngleBetween(a, b) < kAngleToleranceDegrees && Distance(a.position, b.position) < kPositionToleranceMeters;
}

std::string Describe(double value) {
    return std::to_string(value);
}

void ExpectViewYaw(const XrPosef& pose, double expected_degrees, const std::string& message) {
    const double actual = ViewYaw(pose);
    Expect(std::abs(WrapDegrees(actual - expected_degrees)) < kAngleToleranceDegrees,
           message + " (expected view yaw " + Describe(expected_degrees) + ", got " + Describe(actual) + ")");
}

void ExpectSamePose(const XrPosef& actual, const XrPosef& expected, const std::string& message) {
    Expect(SamePose(actual, expected),
           message + " (rotation differs by " + Describe(AngleBetween(actual, expected)) + " degrees: yaw/pitch " +
               Describe(ViewYaw(actual)) + "/" + Describe(ViewPitch(actual)) + " vs " + Describe(ViewYaw(expected)) +
               "/" + Describe(ViewPitch(expected)) + "; position differs by " +
               Describe(Distance(actual.position, expected.position)) + " m)");
}

void ExpectOffset(const XrVector3f& actual, const XrVector3f& expected, const std::string& message) {
    Expect(Distance(actual, expected) < kPositionToleranceMeters,
           message + " (expected (" + Describe(expected.x) + ", " + Describe(expected.y) + ", " + Describe(expected.z) +
               "), got (" + Describe(actual.x) + ", " + Describe(actual.y) + ", " + Describe(actual.z) + "))");
}

struct FakeRuntime {
    inline static XrPosef head = HeadPose(0.0, 0.0);
    inline static XrEventDataReferenceSpaceChangePending pending_change{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};

    static XrPosef EyeOffset(uint32_t eye) {
        return {{0.0f, 0.0f, 0.0f, 1.0f}, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
    }

    static XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime, XrSpaceLocation* location) {
        location->locationFlags = kValidPose;
        if (space == kViewSpace && base == kLocalSpace) {
            location->pose = head;
        } else if (space == kLocalSpace && base == kViewSpace) {
            location->pose = Inverse(head);
        } else {
            location->pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        }
        return XR_SUCCESS;
    }

    static XrResult XRAPI_CALL LocateViews(XrSession,
                                           const XrViewLocateInfo* info,
                                           XrViewState* state,
                                           uint32_t capacity,
                                           uint32_t* count,
                                           XrView* views) {
        *count = 2;
        if (state) {
            state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                    XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
        }
        if (!views || capacity < 2) {
            return XR_SUCCESS;
        }
        const XrPosef base = info->space == kViewSpace ? XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}} : head;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            views[eye].pose = Compose(base, EyeOffset(eye));
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        }
        return XR_SUCCESS;
    }

    static XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* buffer) {
        std::memcpy(buffer, &pending_change, sizeof(pending_change));
        return XR_SUCCESS;
    }

    static XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo*) { return XR_SUCCESS; }
};

depthxr::InputBinding Key(const char* name) {
    depthxr::InputBinding binding;
    binding.type = depthxr::InputBindingType::Keyboard;
    binding.chord = {name};
    return binding;
}

depthxr::PivotActivationBinding Toggle(const char* key) {
    return {depthxr::PivotActivationBehavior::Toggle, Key(key)};
}

depthxr::PivotActivationBinding Hold(const char* key) {
    return {depthxr::PivotActivationBehavior::Hold, Key(key)};
}

// Continuous x2 motion (the extra angle equals the head angle) with no
// deadzone or smoothing, so expected view angles are easy to state.
depthxr::PivotXrResolvedProfile Motion(const char* name,
                                       depthxr::PivotActivationBinding activation,
                                       bool apply_to_snap_views,
                                       double ramp_seconds = 0.5) {
    depthxr::PivotXrResolvedProfile profile;
    profile.id = name;
    profile.name = name;
    profile.behavior = depthxr::PivotProfileBehavior::EnhancedMotion;
    profile.apply_motion_to_snap_views = apply_to_snap_views;
    profile.smoothing = 0.0;
    profile.activation_ramp_seconds = ramp_seconds;
    profile.yaw_positive = profile.yaw_negative = {2.0, 0.0, 180.0, 0.0};
    profile.pitch_positive = profile.pitch_negative = {2.0, 0.0, 180.0, 0.0};
    profile.activation_bindings = {activation};
    return profile;
}

void UseStepped(depthxr::PivotXrResolvedProfile& profile) {
    profile.response_mode = depthxr::PivotResponseMode::Stepped;
    profile.step_glide_mode = depthxr::PivotStepGlideMode::Instant;
    profile.yaw_step_positive = profile.yaw_step_negative = {0.0, 10.0, 10.0, 0.0, 180.0};
    profile.pitch_step_positive = profile.pitch_step_negative = {0.0, 10.0, 10.0, 0.0, 180.0};
}

depthxr::PivotQuickView View(const char* name,
                             double yaw_degrees,
                             depthxr::PivotActivationBinding activation,
                             double transition_seconds = 1.0) {
    depthxr::PivotQuickView view;
    view.id = name;
    view.name = name;
    view.yaw_degrees = yaw_degrees;
    view.transition_seconds = transition_seconds;
    view.activation_bindings = {activation};
    return view;
}

depthxr::PivotXrResolvedProfile Snaps(std::vector<depthxr::PivotQuickView> views) {
    depthxr::PivotXrResolvedProfile profile;
    profile.id = "views";
    profile.name = "Views";
    profile.behavior = depthxr::PivotProfileBehavior::SnapViews;
    profile.view_controls.quick_views = std::move(views);
    return profile;
}

void AddYawLeftNudge(depthxr::PivotXrResolvedProfile& profile, const char* nudge_set_id, const char* key) {
    profile.nudge_set_id = nudge_set_id;
    profile.view_controls.nudges.yaw_step_degrees = 30.0;
    profile.view_controls.nudges.transition_seconds = 0.2;
    profile.view_controls.nudges.yaw_left_bindings = {Key(key)};
}

} // namespace

namespace depthxr {

class PivotSnapTestPeer {
public:
    static OpenXrLayer& Layer() { return OpenXrLayer::Instance(); }

    static void Start(std::vector<PivotXrResolvedProfile> profiles) {
        Reset();
        Layer().resolved_settings_.pivotxr.enabled = true;
        Layer().resolved_settings_.pivotxr.profiles = std::move(profiles);
        PrimeInput();
    }

    // Resolves profiles from a real config document so mid-flight edits run
    // through RefreshResolvedSettings exactly as a config hot-reload does.
    static void StartFromConfig(const std::string& json) {
        Reset();
        const ParseResult parsed = ParseConfig(json);
        Expect(parsed.ok, "Scenario config must parse: " + parsed.error);
        auto& layer = Layer();
        layer.config_ = parsed.document;
        layer.current_exe_name_ = "PivotSnapTests.exe";
        ++layer.config_generation_;
        layer.RefreshResolvedSettings();
        Expect(layer.resolved_settings_.pivotxr.enabled, "Scenario config must resolve Pivot profiles");
        PrimeInput();
    }

    static void EditConfig(const std::function<void(ConfigDocument&)>& edit) {
        edit(Layer().config_);
        ++Layer().config_generation_;
    }

    static PivotXrResolvedProfile& Profile(size_t index) {
        return Layer().resolved_settings_.pivotxr.profiles[index];
    }

    static void Head(double yaw_degrees, double pitch_degrees = 0.0) {
        FakeRuntime::head = HeadPose(yaw_degrees, pitch_degrees);
    }

    static void SetActivation(size_t profile, bool down) {
        SetActivation(profile, 0, down);
    }

    static void SetActivation(size_t profile, size_t binding, bool down) {
        Layer().pivotxr_profile_input_states_[profile].activation[binding].down_cached = down;
    }

    static void SetSnap(size_t profile, size_t view, bool down) {
        Layer().pivotxr_profile_input_states_[profile].quick_views[view].activation[0].down_cached = down;
    }

    static void SetNudgeLeft(size_t profile, bool down) {
        Layer().pivotxr_profile_input_states_[profile].nudge_yaw_left[0].down_cached = down;
    }

    static void SetOriginKey(size_t profile, bool down) {
        Layer().pivotxr_profile_input_states_[profile].set_origin[0].down_cached = down;
    }

    // Press and release on consecutive drives, as a tap would.
    static void TapActivation(size_t profile) {
        SetActivation(profile, true);
        Drive(0.0);
        SetActivation(profile, false);
    }

    static void TapSnap(size_t profile, size_t view) {
        SetSnap(profile, view, true);
        Drive(0.0);
        SetSnap(profile, view, false);
    }

    static XrTime Now() { return display_time; }

    // One xrLocateViews for a new displayTime after `seconds` of wall time.
    static XrPosef Drive(double seconds = kFrameSeconds) {
        auto& layer = Layer();
        if (layer.pivotxr_last_smoothing_wall_time_.has_value()) {
            layer.pivotxr_last_smoothing_wall_time_ =
                std::chrono::steady_clock::now() -
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
        }
        display_time += kFrameNanoseconds;
        return ViewsAt(display_time);
    }

    // Drives frames for `seconds`, failing on any per-frame jump larger than an
    // intended transition could produce while the head is still.
    static XrPosef Settle(double seconds, const std::string& context) {
        XrPosef previous = ViewsAt(display_time);
        const int frames = static_cast<int>(std::ceil(seconds / kFrameSeconds));
        for (int frame = 0; frame < frames; ++frame) {
            const XrPosef next = Drive();
            Expect(AngleBetween(previous, next) < kMaxTransitionStepDegrees,
                   context + ": view jumped " + Describe(AngleBetween(previous, next)) + " degrees in one frame");
            previous = next;
        }
        return previous;
    }

    // The pivoted head pose recovered from the left eye returned by xrLocateViews.
    static XrPosef ViewsAt(XrTime time) {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = time;
        info.space = kLocalSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        Expect(Layer().LocateViews(kSession, &info, &state, 2, &count, views) == XR_SUCCESS && count == 2,
               "xrLocateViews must succeed");
        return Compose(views[0].pose, Inverse(FakeRuntime::EyeOffset(0)));
    }

    // Head-attached geometry: VIEW located in the app's world space.
    static XrPosef SpaceAt(XrTime time) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        Expect(Layer().LocateSpace(kViewSpace, kLocalSpace, time, &location) == XR_SUCCESS, "xrLocateSpace must succeed");
        return location.pose;
    }

    static void Recenter(XrTime change_time) {
        auto& layer = Layer();
        FakeRuntime::pending_change.session = kSession;
        FakeRuntime::pending_change.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        FakeRuntime::pending_change.changeTime = change_time;
        layer.next_poll_event_ = &FakeRuntime::PollEvent;
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        Expect(layer.PollEvent(XR_NULL_HANDLE, &buffer) == XR_SUCCESS, "Recenter event must pass through");
        layer.next_poll_event_ = nullptr;
    }

    template <typename Call>
    static std::size_t CountAllocations(Call&& call) {
        g_allocations.store(0);
        g_counting_allocations.store(true);
        call();
        g_counting_allocations.store(false);
        return g_allocations.load();
    }

    // Depth's stereo boost with Depth Lock, so xrLocateViews also caches
    // the frame's submission geometry.
    static void EnableAnchoredDepth() {
        auto& settings = Layer().resolved_settings_.depthxr;
        settings.enabled = true;
        settings.stereo_boost = 1.2;
        settings.depth_anchor = true;
    }

    struct FrameAllocations {
        std::size_t locate_views{0};
        std::size_t locate_space{0};
        bool depth_cached{false};
    };

    // One frame as an application makes it: xrLocateViews for a new
    // displayTime, three xrLocateSpace calls for it, then xrEndFrame's
    // pruning of the per-frame caches.
    static FrameAllocations LocateFrame() {
        auto& layer = Layer();
        if (layer.pivotxr_last_smoothing_wall_time_.has_value()) {
            layer.pivotxr_last_smoothing_wall_time_ =
                std::chrono::steady_clock::now() - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                       std::chrono::duration<double>(kFrameSeconds));
        }
        display_time += kFrameNanoseconds;
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = display_time;
        info.space = kLocalSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        XrResult views_result = XR_ERROR_RUNTIME_FAILURE;
        FrameAllocations allocations;
        allocations.locate_views = CountAllocations(
            [&] { views_result = layer.LocateViews(kSession, &info, &state, 2, &count, views); });
        Expect(views_result == XR_SUCCESS && count == 2, "xrLocateViews must succeed");
        allocations.depth_cached = !layer.cached_depth_submission_geometry_.empty();
        for (int call = 0; call < 3; ++call) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            XrResult space_result = XR_ERROR_RUNTIME_FAILURE;
            allocations.locate_space += CountAllocations(
                [&] { space_result = layer.LocateSpace(kViewSpace, kLocalSpace, display_time, &location); });
            Expect(space_result == XR_SUCCESS, "xrLocateSpace must succeed");
        }
        layer.PrunePivotPoseDeltas(display_time);
        layer.PruneDepthSubmissionGeometry(display_time);
        layer.PruneQuadViewsFrames(display_time);
        return allocations;
    }

    // xrEndFrame with no layers for `time`, through a runtime that takes it.
    static void EndFrameAt(XrTime time) {
        auto& layer = Layer();
        layer.next_end_frame_ = &FakeRuntime::EndFrame;
        layer.turbo_frame_interception_required_.store(false, std::memory_order_release);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        info.displayTime = time;
        info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        Expect(layer.EndFrame(kSession, &info) == XR_SUCCESS, "xrEndFrame must succeed");
    }

    // A pose-delta continuity miss was logged and its run has not recovered.
    static bool ContinuityMissReported() { return Layer().pivotxr_pose_delta_miss_logged_; }

    static std::size_t PendingReferenceChanges() { return Layer().pivotxr_reference_changes_.size(); }
    static void DisablePivot() { Layer().resolved_settings_.pivotxr.enabled = false; }

    static bool SnapSelected() { return Layer().pivotxr_quick_view_active_; }
    static bool SnapEffectActive() {
        const auto& layer = Layer();
        return layer.pivotxr_quick_view_active_ || layer.pivotxr_quick_view_transitioning_ ||
               layer.pivotxr_quick_view_transition_.active;
    }
    static bool Stacked() { return Layer().pivotxr_snap_stacked_; }
    static bool Engaged() { return Layer().pivotxr_engaged_; }
    static size_t MotionOwner() { return Layer().pivotxr_active_profile_index_; }
    static size_t SelectedSnapView() { return Layer().pivotxr_quick_view_index_; }
    static double AppliedExtraPitchDegrees() { return Degrees(Layer().pivot_diagnostic_.eased_extra_pitch_radians); }
    static OsdSnapshot Osd() { return Layer().BuildOsdSnapshot(); }

private:
    inline static XrTime display_time = 0;

    static void Reset() {
        auto& layer = Layer();
        layer.ResetInstanceState();
        layer.ResetSessionState();
        layer.ResetPivotActivationState();
        layer.config_ = {};
        layer.has_loaded_config_ = true;
        layer.active_session_ = kSession;
        layer.resolved_settings_ = {};
        layer.resolved_settings_.core.enabled = true;
        layer.resolved_settings_generation_ = layer.config_generation_;
        layer.resolved_settings_session_ = layer.active_session_;
        layer.tracked_view_spaces_ = {kViewSpace};
        layer.tracked_local_spaces_ = {kLocalSpace};
        layer.internal_view_space_ = kViewSpace;
        layer.next_locate_space_ = &FakeRuntime::LocateSpace;
        layer.next_locate_views_ = &FakeRuntime::LocateViews;
        FakeRuntime::head = HeadPose(0.0, 0.0);
        display_time = 1'000'000'000;
    }

    // Primes edge detection, then freezes hardware polling so the cached key
    // states set by each scenario are the only input arbitration sees.
    static void PrimeInput() {
        auto& layer = Layer();
        layer.pivotxr_binding_last_poll_time_.reset();
        layer.IsPivotXrActive();
        layer.pivotxr_binding_last_poll_time_ = std::chrono::steady_clock::now() + std::chrono::hours(1);
    }
};

} // namespace depthxr

namespace {

using Peer = depthxr::PivotSnapTestPeer;

// Profile indices used by the common rigs below.
constexpr size_t kFlight = 0;
constexpr size_t kViews = 1;
constexpr size_t kThird = 2;
constexpr size_t kRight = 0;
constexpr size_t kLeft = 1;

void EngageFlight() {
    Peer::TapActivation(kFlight);
    Peer::Settle(0.6, "Flight engagement ramp");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "Flight must be engaged");
}

void StackedSnapEntryAndReleaseAreContinuous(bool stepped) {
    const std::string mode = stepped ? "Stepped" : "Continuous";
    auto flight = Motion("Flight", Toggle("F1"), true);
    if (stepped) {
        UseStepped(flight);
    }
    Peer::Start({flight, Snaps({View("Right", 90.0, Hold("F2"))})});
    // Head angles chosen so the Continuous extra equals the angle and the
    // Stepped extra is the nearest lower multiple of ten.
    const auto extra = [&](double angle) { return stepped ? 10.0 * std::floor(angle / 10.0) : angle; };
    EngageFlight();
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), 12.0 + extra(12.0), mode + " motion must amplify before the snap");

    const XrPosef before_snap = Peer::Drive(0.0);
    Peer::SetSnap(kViews, kRight, true);
    ExpectSamePose(Peer::Drive(0.0), before_snap, mode + " snap entry must start from the displayed pose");
    ExpectViewYaw(Peer::Settle(1.1, mode + " snap entry"), -90.0, mode + " stacked snap must land on the named pose");
    Expect(Peer::Stacked(), mode + " snap entered with opted-in motion must stack");

    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), -90.0 + 12.0 + extra(12.0), mode + " motion must keep amplifying inside the snap");
    const depthxr::OsdSnapshot stacked_status = Peer::Osd();
    Expect(stacked_status.pivot.find("Active") != std::string::npos &&
               stacked_status.pivot_secondary.find("Stacked") != std::string::npos,
           mode + " OSD must report active motion stacked on the snap");

    const XrPosef before_release = Peer::Drive(0.0);
    Peer::SetSnap(kViews, kRight, false);
    ExpectSamePose(Peer::Drive(0.0), before_release, mode + " snap release must start from the displayed pose");
    ExpectViewYaw(Peer::Settle(1.1, mode + " snap release"), 24.0 + extra(24.0),
                  mode + " release must hand back to origin-relative motion");
    Expect(!Peer::SnapEffectActive() && !Peer::Stacked(), mode + " release must clear the snap and stacking");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, mode + " release must keep the motion profile engaged");
    Expect(Peer::Osd().pivot_secondary.find("Stacked") == std::string::npos,
           mode + " OSD must not report a stale stack after the return");
}

void SnapPressedDuringLocateSpaceKeepsTheFrameTransform() {
    Peer::Start({Motion("Flight", Toggle("F1"), true), Snaps({View("Right", 90.0, Hold("F2"))})});
    EngageFlight();
    Peer::Head(12.0);
    Peer::Drive();
    const XrTime frame = Peer::Now();
    const XrPosef displayed = Peer::ViewsAt(frame);
    ExpectSamePose(Peer::SpaceAt(frame), displayed, "xrLocateSpace must match the drive for the same displayTime");

    // Controllers locate after the views; the snap press is consumed here.
    Peer::SetSnap(kViews, kRight, true);
    ExpectSamePose(Peer::SpaceAt(frame), displayed,
                   "A snap pressed during xrLocateSpace must not change the transform of the current displayTime");
    Expect(Peer::SnapSelected(), "The snap press must be consumed by the xrLocateSpace input poll");
    ExpectSamePose(Peer::ViewsAt(frame), displayed, "Repeated xrLocateViews for the frame must keep its transform");
    ExpectSamePose(Peer::SpaceAt(frame), displayed, "Later xrLocateSpace calls for the frame must agree with the views");

    const XrPosef next = Peer::Drive(0.0);
    ExpectSamePose(next, displayed, "The next drive must start the snap transition from the displayed pose");
    const XrPosef moving = Peer::Drive();
    Expect(AngleBetween(moving, next) > 0.0001 && AngleBetween(moving, next) < kMaxTransitionStepDegrees,
           "The snap transition must then advance smoothly");
    ExpectSamePose(Peer::SpaceAt(Peer::Now()), moving, "xrLocateSpace must follow the new drive");
}

// Applies `press` while the app locates controllers for an already driven
// frame, then checks that every locate for that displayTime still agrees.
void ExpectInputKeepsTheFrame(const std::function<void()>& press, const std::string& transition) {
    Peer::Drive();
    const XrTime frame = Peer::Now();
    const XrPosef displayed = Peer::ViewsAt(frame);
    press();
    ExpectSamePose(Peer::SpaceAt(frame), displayed, transition + " must not change xrLocateSpace for the current frame");
    ExpectSamePose(Peer::ViewsAt(frame), displayed, transition + " must not change xrLocateViews for the current frame");
    ExpectSamePose(Peer::SpaceAt(frame), displayed, transition + " must keep later locates of the frame consistent");
}

void EveryTransitionKeepsTheFrameTransformDuringLocateSpace() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0),
                 Snaps({View("Right", 90.0, Hold("F2")), View("Left", -90.0, Hold("F3"), 0.0)}),
                 Motion("Exclusive", Toggle("F5"), false, 0.0)});
    EngageFlight();
    Peer::Head(12.0);
    ExpectInputKeepsTheFrame([] { Peer::SetSnap(kViews, kRight, true); }, "Entering a stacked snap");
    Peer::Settle(1.1, "stacked snap entry");
    ExpectInputKeepsTheFrame([] { Peer::SetSnap(kViews, kLeft, true); }, "Switching to an instant snap");
    ExpectInputKeepsTheFrame([] { Peer::SetActivation(kFlight, true); }, "Disengaging stacked motion");
    Peer::SetActivation(kFlight, false);
    Peer::Drive();
    ExpectInputKeepsTheFrame([] { Peer::SetActivation(kFlight, true); }, "Re-engaging stacked motion");
    Peer::SetActivation(kFlight, false);
    Peer::Head(20.0);
    ExpectInputKeepsTheFrame([] { Peer::SetActivation(kThird, true); }, "Selecting a non-opted profile mid-snap");
    Peer::SetActivation(kThird, false);
    Expect(Peer::SnapSelected() && !Peer::Stacked(), "The snap must stay selected and exclusive");
    ExpectInputKeepsTheFrame([] { Peer::SetSnap(kViews, kLeft, false); }, "Releasing an instant exclusive snap");
    Peer::Drive();
    Expect(!Peer::SnapSelected(), "The held snap release must leave the instant snap");
    ExpectInputKeepsTheFrame([] { Peer::SetSnap(kViews, kRight, false); }, "Releasing an inactive held snap");
}

void ZeroDurationReleaseDuringLocateSpaceWaitsForTheNextDrive() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"), 0.0)})});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Drive(0.0);
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -90.0 + 12.0 + 12.0, "Instant stacked snap must apply motion around the snap");

    const XrTime frame = Peer::Now();
    const XrPosef displayed = Peer::ViewsAt(frame);
    Peer::SetSnap(kViews, kRight, false);
    ExpectSamePose(Peer::SpaceAt(frame), displayed,
                   "An instant release consumed by xrLocateSpace must not change the current displayTime");
    ExpectSamePose(Peer::ViewsAt(frame), displayed, "Repeated xrLocateViews must keep the frame transform");
    ExpectViewYaw(Peer::Drive(0.0), 24.0, "The instant release must take effect on the next drive");
    Expect(!Peer::SnapEffectActive() && !Peer::Stacked(), "The instant release must finish the snap");
}

void EngagingOptedInMotionWhileSnappedMeasuresFromThePressPose() {
    Peer::Start({Motion("Flight", Toggle("F1"), true), Snaps({View("Right", 90.0, Toggle("F2"))})});
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "exclusive snap entry"), -90.0, "Snap must land on its pose with motion off");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -78.0, "A snap without motion follows the head 1:1");

    const XrPosef before = Peer::Drive(0.0);
    Peer::TapActivation(kFlight);
    ExpectSamePose(Peer::Drive(0.0), before, "Enabling motion inside a snap must preserve the displayed pose");
    Expect(Peer::Stacked() && Peer::Engaged(), "Opted-in motion engaged inside a snap must stack");
    ExpectViewYaw(Peer::Settle(0.6, "motion ramp inside snap"), -78.0, "Motion must start around the press pose");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), -78.0 + 12.0 + 12.0, "Motion must amplify relative to the press pose");

    const XrPosef before_release = Peer::Drive(0.0);
    Peer::TapSnap(kViews, kRight);
    ExpectSamePose(Peer::Drive(0.0), before_release, "Releasing the snap must preserve the displayed pose");
    ExpectViewYaw(Peer::Settle(1.1, "snap release"), 48.0, "Motion must continue after the snap");
    Expect(Peer::Engaged() && !Peer::Stacked() && !Peer::SnapEffectActive(), "Motion must stay engaged after the snap");
}

void DisengagingMotionWhileSnappedKeepsTheSnap() {
    Peer::Start({Motion("Flight", Toggle("F1"), true), Snaps({View("Right", 90.0, Toggle("F2"))})});
    EngageFlight();
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Stacked motion must amplify inside the snap");

    const XrPosef before = Peer::Drive(0.0);
    Peer::TapActivation(kFlight);
    ExpectSamePose(Peer::Drive(0.0), before, "Disengaging motion must ease out, not cut");
    ExpectViewYaw(Peer::Settle(0.6, "motion release inside snap"), -78.0, "Motion must ease out around the snap");
    Expect(Peer::SnapSelected() && !Peer::Engaged(), "Disengaging stacked motion must leave the snap selected");
    Expect(Peer::Osd().pivot_secondary.find("Stacked") == std::string::npos, "Off motion must not be labelled stacked");

    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "snap release without motion"), 12.0, "Release must return to the raw view");
    Expect(!Peer::SnapEffectActive() && !Peer::Stacked() && !Peer::Engaged(), "No state may linger after release");
}

void SwitchingSnapViewsWhileStackedKeepsMotion() {
    Peer::Start({Motion("Flight", Toggle("F1"), true),
                 Snaps({View("Right", 90.0, Hold("F2")), View("Left", -90.0, Hold("F3"))})});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Settle(1.1, "right snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Right snap must stack motion");

    const XrPosef before = Peer::Drive(0.0);
    Peer::SetSnap(kViews, kLeft, true);
    ExpectSamePose(Peer::Drive(0.0), before, "Switching views must start from the displayed pose");
    ExpectViewYaw(Peer::Settle(1.1, "left snap entry"), 90.0, "Switching views must land on the new pose");
    Expect(Peer::Stacked() && Peer::SelectedSnapView() == kLeft, "The new view must stay stacked");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), 90.0 + 12.0 + 12.0, "Motion must amplify around the new view");

    Peer::SetSnap(kViews, kLeft, false);
    ExpectViewYaw(Peer::Settle(1.1, "left snap release"), 48.0, "Releasing the held view must return to motion");
    Peer::SetSnap(kViews, kRight, false);
    Peer::Drive();
    Expect(!Peer::SnapEffectActive() && Peer::Engaged(), "The earlier held view must not reactivate");
}

void CompatibleProfileSwitchKeepsTheSnapCentre() {
    auto cruise = Motion("Cruise", Toggle("F4"), true, 0.0);
    cruise.yaw_positive = cruise.yaw_negative = {3.0, 0.0, 180.0, 0.0};
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"))}), cruise});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Flight must amplify inside the snap");

    Peer::TapActivation(kThird);
    Expect(Peer::MotionOwner() == kThird && Peer::Stacked() && Peer::SnapSelected(),
           "A compatible profile must take over motion and keep the stack");
    Peer::Head(0.0);
    ExpectViewYaw(Peer::Drive(), -90.0, "A profile switch must not move the snap centre");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -90.0 + 12.0 + 24.0, "The new profile must amplify from the snap's reference");
}

void SameTuningProfileSwitchIsSeamlessAndKeepsTheDeadzone() {
    auto flight = Motion("Flight", Toggle("F1"), true, 0.0);
    flight.yaw_positive = flight.yaw_negative = {2.0, 5.0, 180.0, 0.0};
    auto cruise = flight;
    cruise.id = cruise.name = "Cruise";
    cruise.activation_bindings = {Toggle("F4")};
    Peer::Start({flight, Snaps({View("Right", 90.0, Hold("F2"))}), cruise});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -90.0 + 12.0 + 7.0, "Motion beyond the deadzone must amplify inside the snap");

    const XrPosef before = Peer::Drive(0.0);
    Peer::TapActivation(kThird);
    ExpectSamePose(Peer::Drive(0.0), before, "Switching to identical tuning must not move the view");
    Peer::Head(3.0);
    ExpectViewYaw(Peer::Drive(), -87.0, "The deadzone must stay centred on the snap after a profile switch");
}

void IncompatibleProfileKeepsTheSnapExclusive() {
    Peer::Start({Motion("Flight", Toggle("F1"), true), Snaps({View("Right", 90.0, Toggle("F2"))}),
                 Motion("Exclusive", Toggle("F5"), false)});
    EngageFlight();
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Flight must amplify inside the snap");

    const XrPosef before = Peer::Drive(0.0);
    Peer::TapActivation(kThird);
    ExpectSamePose(Peer::Drive(0.0), before, "Selecting a non-opted profile must not jump");
    Expect(Peer::SnapSelected() && !Peer::Stacked(), "A non-opted profile must keep the snap, exclusively");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kThird, "The selected profile must be engaged");
    ExpectViewYaw(Peer::Settle(1.1, "stacked motion fading out"), -78.0, "Stacked motion must ease out of the snap");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "An exclusive snap follows the head 1:1");

    Peer::Head(0.0);
    Peer::Drive();
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "exclusive snap release"), 0.0, "Release must return to the underlying view");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "Release must restore the profile engaged before the snap");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), 24.0, "The restored profile must amplify again");
}

void NonOptedMotionTogglesLeaveAnExclusiveSnapSelected() {
    Peer::Start({Motion("Flight", Toggle("F1"), false, 0.0), Snaps({View("Right", 90.0, Toggle("F2"))})});
    // Motion off before the snap.
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "snap entry");
    const XrPosef snapped = Peer::Drive(0.0);
    Peer::TapActivation(kFlight);
    Expect(Peer::SnapSelected(), "A non-opted motion toggle must not dismiss the snap");
    ExpectSamePose(Peer::Drive(0.0), snapped, "A non-opted motion toggle must not move the snap");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -78.0, "The snap must stay exclusive");
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "snap release"), 12.0, "Release must restore the state from before the snap");
    Expect(!Peer::Engaged(), "Release must restore the motion state from before the snap");

    // Motion on before the snap.
    Peer::Head(0.0);
    EngageFlight();
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "snap entry with motion");
    Peer::TapActivation(kFlight);
    Expect(Peer::SnapSelected(), "A motion toggle during an exclusive snap must not dismiss it");
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "snap release with motion");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "Release must restore the engaged motion profile");
}

void ZeroDurationStackedReleaseLeavesNoStackForARecenter() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"), 0.0)})});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    ExpectViewYaw(Peer::Drive(0.0), -90.0, "An instant snap must land on its pose");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Instant stacked snap must amplify");
    Peer::SetSnap(kViews, kRight, false);
    ExpectViewYaw(Peer::Drive(0.0), 24.0, "An instant release must return to motion");
    Expect(!Peer::Stacked() && !Peer::SnapEffectActive(), "An instant release must clear stacking");

    Peer::Recenter(Peer::Now() + kFrameNanoseconds);
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), 48.0, "After the snap, a recenter must not re-measure motion from the recenter pose");

    Peer::TapActivation(kFlight);
    Peer::Drive();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Drive(0.0);
    Expect(Peer::SnapSelected() && !Peer::Stacked(), "With motion off the next snap must be exclusive");
}

void ZeroDurationIncompatibleSwitchAndReleaseFinishCleanly() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"), 0.0)}),
                 Motion("Exclusive", Toggle("F5"), false, 0.0)});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Drive(0.0);
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Instant stacked snap must amplify");

    Peer::TapActivation(kThird);
    ExpectViewYaw(Peer::Drive(0.0), -78.0, "An instant snap must drop stacked motion instantly");
    Expect(Peer::SnapSelected() && !Peer::Stacked(), "A non-opted profile must keep the instant snap exclusive");
    Peer::SetSnap(kViews, kRight, false);
    Peer::Drive(0.0);
    Expect(!Peer::SnapEffectActive() && !Peer::Stacked(), "An instant exclusive release must finish the snap");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "Release must restore the profile from before the snap");

    Peer::Recenter(Peer::Now() + kFrameNanoseconds);
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), 48.0, "A recenter after the snap must not affect motion measurement");
}

void RecenterWhileStackedIsContinuous() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"))})});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    const XrPosef before = Peer::Drive();
    ExpectViewYaw(before, -66.0, "Stacked motion must amplify");

    Peer::Recenter(Peer::Now() + kFrameNanoseconds);
    ExpectSamePose(Peer::Drive(0.0), before, "A recenter during a stacked snap must not jump the view");
    ExpectViewYaw(Peer::Settle(1.1, "motion re-measured after recenter"), -78.0,
                  "Motion must be re-measured from the recenter pose");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), -78.0 + 12.0 + 12.0, "Motion must amplify from the recenter pose");
}

void HoldActivatedMotionInsideAStackedSnap() {
    Peer::Start({Motion("Flight", Hold("F1"), true), Snaps({View("Six", 180.0, Toggle("F3"))})});
    Peer::SetActivation(kFlight, true);
    Peer::Settle(0.6, "hold engagement");
    Expect(Peer::Engaged(), "Holding the binding must engage motion");
    Peer::TapSnap(kViews, 0);
    ExpectViewYaw(Peer::Settle(1.1, "check six entry"), 180.0, "Check six must land behind");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), 180.0 + 24.0, "Held motion must stack on check six");

    const XrPosef before_release = Peer::Drive(0.0);
    Peer::SetActivation(kFlight, false);
    ExpectSamePose(Peer::Drive(0.0), before_release, "Releasing the hold must ease motion out");
    ExpectViewYaw(Peer::Settle(0.6, "hold release"), 180.0 + 12.0, "Motion must ease out while snapped");
    Expect(Peer::SnapSelected() && !Peer::Engaged(), "The snap must remain after the hold is released");

    const XrPosef before_hold = Peer::Drive(0.0);
    Peer::SetActivation(kFlight, true);
    ExpectSamePose(Peer::Drive(0.0), before_hold, "Holding again must not jump");
    ExpectViewYaw(Peer::Settle(0.6, "hold re-engagement"), 192.0, "Re-engaged motion starts around the current pose");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), 192.0 + 24.0, "Re-engaged motion must amplify inside the snap");

    const XrPosef before_return = Peer::Drive(0.0);
    Peer::TapSnap(kViews, 0);
    ExpectSamePose(Peer::Drive(0.0), before_return, "Leaving check six must not jump");
    ExpectViewYaw(Peer::Settle(1.1, "check six return"), 48.0, "Held motion must continue after the snap");
    Expect(Peer::Engaged() && !Peer::Stacked(), "Held motion must stay engaged after the snap");
}

void MotionToggledDuringSnapEntryKeepsTheTransition() {
    Peer::Start({Motion("Flight", Toggle("F1"), true, 0.0), Snaps({View("Right", 90.0, Hold("F2"))})});
    EngageFlight();
    Peer::SetSnap(kViews, kRight, true);
    Peer::Settle(0.3, "early snap entry");
    Peer::TapActivation(kFlight);
    Peer::Head(6.0);
    Peer::Drive();
    const XrPosef before = Peer::Drive(0.0);
    Peer::TapActivation(kFlight);
    ExpectSamePose(Peer::Drive(0.0), before, "Re-enabling motion must preserve the in-progress snap pose");
    ExpectViewYaw(Peer::Settle(1.0, "rest of the snap entry"), -84.0,
                  "Re-enabling motion must not cancel the entering snap transition");
}

void AlwaysActiveHoldSuspendInsideAStackedSnap() {
    auto always = Motion("Always", Hold("F1"), true);
    always.always_active = true;
    Peer::Start({always, Snaps({View("Right", 90.0, Toggle("F2"))})});
    Peer::Settle(0.6, "automatic engagement");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "The always-active profile must engage");
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), -66.0, "Always-active motion must stack");

    Peer::SetActivation(kFlight, true);
    ExpectViewYaw(Peer::Settle(0.6, "hold suspend"), -78.0, "Holding the suspend binding must ease motion out");
    Expect(!Peer::Engaged() && Peer::SnapSelected(), "Suspending must keep the snap");
    Peer::SetActivation(kFlight, false);
    Peer::Drive(0.0);
    Expect(Peer::Engaged() && Peer::Stacked(), "Releasing the suspend hold must restore stacked motion");
    Peer::Settle(0.6, "resume ramp");
    Peer::Head(24.0);
    ExpectViewYaw(Peer::Drive(), -78.0 + 12.0 + 12.0, "Resumed motion must amplify inside the snap");
}

void AlwaysActiveOptInDoesNotHijackAnExclusiveSnap() {
    auto always = Motion("Always", Toggle("F1"), true);
    always.always_active = true;
    Peer::Start({always, Snaps({View("Right", 90.0, Toggle("F2"))}), Motion("Cruise", Toggle("F4"), false)});
    Peer::Settle(0.6, "automatic engagement");
    Peer::TapActivation(kThird);
    Expect(Peer::Engaged() && Peer::MotionOwner() == kThird, "Cruise must replace the always-active profile");
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "exclusive snap entry");
    Expect(!Peer::Stacked() && Peer::MotionOwner() != kFlight,
           "An always-active opted-in profile must not engage during another profile's exclusive snap");
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "snap release");
    Expect(Peer::Engaged() && Peer::MotionOwner() == kThird, "Release must restore the manually selected profile");
}

void HoldSuspendRestoresTheAlwaysActiveProfile() {
    auto primary = Motion("Primary", Hold("F1"), false, 0.0);
    primary.always_active = true;
    auto fallback = Motion("Fallback", Toggle("F4"), false, 0.0);
    fallback.always_active = true;
    Peer::Start({primary, Snaps({View("Right", 90.0, Toggle("F2"))}), fallback});
    Peer::Drive();
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "The first always-active profile must engage");
    Peer::SetActivation(kFlight, true);
    Peer::Drive();
    Expect(Peer::Engaged() && Peer::MotionOwner() == kThird, "Suspending must hand over to the next always-active profile");
    Peer::SetActivation(kFlight, false);
    Peer::Drive();
    Expect(Peer::Engaged() && Peer::MotionOwner() == kFlight, "Releasing the suspend hold must restore the profile");
}

void SharedNudgesSurviveAnExclusiveSnap() {
    auto flight = Motion("Flight", Toggle("F1"), false, 0.0);
    AddYawLeftNudge(flight, "shared", "F6");
    auto views = Snaps({View("Right", 90.0, Toggle("F2"))});
    AddYawLeftNudge(views, "shared", "F6");
    Peer::Start({flight, views});
    EngageFlight();
    Peer::SetNudgeLeft(kFlight, true);
    ExpectViewYaw(Peer::Settle(0.3, "nudge"), 30.0, "The nudge must apply while motion is active");
    Peer::SetNudgeLeft(kFlight, false);
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "snap entry"), -60.0, "A shared Nudge Set must stay applied inside the snap");
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "snap release"), 30.0,
                  "Returning to active motion with a shared Nudge Set must keep the nudge");
}

void StackedSnapKeepsItsNudgesWhenMotionChanges() {
    auto flight = Motion("Flight", Toggle("F1"), true, 0.0);
    flight.nudge_set_id = "flight";
    auto views = Snaps({View("Right", 90.0, Toggle("F2"))});
    AddYawLeftNudge(views, "views", "F6");
    auto cruise = Motion("Cruise", Toggle("F4"), true, 0.0);
    cruise.nudge_set_id = "cruise";
    Peer::Start({flight, views, cruise});
    EngageFlight();
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "stacked snap entry");
    Peer::SetNudgeLeft(kViews, true);
    const XrPosef nudged = Peer::Settle(0.3, "snap nudge");
    ExpectViewYaw(nudged, -60.0, "The snap's nudge must apply");
    Peer::SetNudgeLeft(kViews, false);

    Peer::TapActivation(kFlight);
    ExpectSamePose(Peer::Drive(0.0), nudged, "Disengaging motion must not cut the snap's nudges");
    Peer::TapActivation(kThird);
    ExpectSamePose(Peer::Drive(0.0), nudged, "Engaging another motion profile must not cut the snap's nudges");
    Peer::Settle(0.3, "motion change");
    Peer::TapSnap(kViews, kRight);
    ExpectViewYaw(Peer::Settle(1.1, "snap release"), 0.0, "Unshared snap nudges must fade out with the snap");
}

XrVector3f Translation(const XrPosef& pivoted) {
    return {pivoted.position.x - kSeatedHead.x, pivoted.position.y - kSeatedHead.y, pivoted.position.z - kSeatedHead.z};
}

void PositionAssistFollowsTheSnappedHeading() {
    auto flight = Motion("Flight", Toggle("F1"), true, 0.0);
    flight.yaw_positive = flight.yaw_negative = flight.pitch_positive = flight.pitch_negative = {1.0, 0.0, 180.0, 0.0};
    flight.position_assist.enabled = true;
    flight.position_assist.yaw = {10.0, 0.0, 0.0, 0.0, 30.0};
    Peer::Start({flight, Snaps({View("Six", 180.0, Toggle("F3"), 0.0)})});
    EngageFlight();
    Peer::Head(30.0);
    ExpectOffset(Translation(Peer::Settle(1.5, "assist at origin")), {-0.10f, 0.0f, 0.0f},
                 "Turning left must lean left at the origin");
    Peer::Head(0.0);
    ExpectOffset(Translation(Peer::Settle(1.5, "assist neutral")), {0.0f, 0.0f, 0.0f},
                 "Position Assist must return to neutral");

    Peer::TapSnap(kViews, 0);
    Peer::Drive();
    Expect(Peer::Stacked(), "Check six must stack the motion profile");
    Peer::Head(30.0);
    // Looking backwards (+Z), the snapped view's left is +X.
    ExpectOffset(Translation(Peer::Settle(1.5, "assist in check six")), {0.10f, 0.0f, 0.0f},
                 "Leaning toward a turn must follow the snapped view, not the seated origin");
}

void PositionAssistIgnoresOriginPitch() {
    auto flight = Motion("Flight", Toggle("F1"), false, 0.0);
    flight.yaw_positive = flight.yaw_negative = flight.pitch_positive = flight.pitch_negative = {1.0, 0.0, 180.0, 0.0};
    flight.position_assist.enabled = true;
    flight.position_assist.yaw = {0.0, 10.0, 0.0, 0.0, 30.0};
    flight.set_origin_bindings = {Key("F7")};
    Peer::Start({flight});
    Peer::Head(0.0, -30.0);
    Peer::SetOriginKey(kFlight, true);
    Peer::Drive();
    Peer::SetOriginKey(kFlight, false);
    EngageFlight();
    Peer::Head(30.0, -30.0);
    ExpectOffset(Translation(Peer::Settle(1.5, "assist with pitched origin")), {0.0f, 0.10f, 0.0f},
                 "Position Assist must stay gravity-aligned when the origin was captured looking down");
}

void SnapEntryFromSaturatedPitchStartsMovingImmediately() {
    auto flight = Motion("Flight", Toggle("F1"), false, 0.0);
    flight.yaw_positive = flight.yaw_negative = {1.0, 0.0, 180.0, 0.0};
    flight.pitch_positive = flight.pitch_negative = {3.0, 0.0, 180.0, 0.0};
    Peer::Start({flight, Snaps({View("Level", 0.0, Toggle("F2"))})});
    EngageFlight();
    Peer::Head(0.0, 50.0);
    Peer::Drive();
    Expect(std::abs(Peer::AppliedExtraPitchDegrees() - 85.0) < kAngleToleranceDegrees,
           "High pitch gain must saturate at the 85-degree output clamp");
    Peer::TapSnap(kViews, 0);
    Expect(std::abs(Peer::AppliedExtraPitchDegrees() - 85.0) < kAngleToleranceDegrees,
           "Snap entry must start from the clamped pitch");
    Peer::Drive(0.1);
    Expect(Peer::AppliedExtraPitchDegrees() < 84.0,
           "Snap entry must leave the clamp immediately instead of stalling (extra pitch " +
               Describe(Peer::AppliedExtraPitchDegrees()) + ")");
}

void OsdReportsMotionImmediatelyAndOnlyShowsSnapsWhenConfigured() {
    Peer::Start({Motion("Flight", Toggle("F1"), false)});
    Peer::TapActivation(kFlight);
    const depthxr::OsdSnapshot engaged = Peer::Osd();
    Expect(engaged.pivot.find("Motion: Active") == 0,
           "The OSD must show motion as active as soon as it engages, got '" + engaged.pivot + "'");
    Expect(engaged.pivot.find("Continuous") != std::string::npos,
           "The expanded OSD must name the response mode like the app preview, got '" + engaged.pivot + "'");
    Expect(engaged.pivot_secondary.empty() && engaged.compact_pivot_secondary.empty(),
           "Without Snap Views the OSD must not reserve a snap line");

    Peer::Start({Motion("Flight", Toggle("F1"), false), Snaps({View("Right", 90.0, Toggle("F2"))})});
    Expect(Peer::Osd().pivot_secondary == "Snap: Off", "With Snap Views configured the OSD must show the snap line");
}

const char* kMidFlightConfig = R"json(
{
  "version": 3,
  "core": { "enabled": true },
  "applications": [
    { "id": "tests", "name": "Tests", "enabled": true, "match": { "exe": "PivotSnapTests.exe" } }
  ],
  "modules": {
    "depthxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "pivotxr": {
      "enabled": true,
      "defaults": {},
      "nudgeSets": [
        { "id": "a", "name": "A", "settings": { "yawStepDegrees": 30 } },
        { "id": "b", "name": "B", "settings": { "yawStepDegrees": 30 } }
      ],
      "profiles": [
        {
          "id": "flight",
          "name": "Flight",
          "applicationIds": ["tests"],
          "enabled": true,
          "nudgeSetId": "a",
          "activationBindings": [ { "behavior": "toggle", "binding": { "type": "keyboard", "chord": ["F1"] } } ],
          "settings": { "applyMotionToSnapViews": true, "smoothing": 0, "activationRampSeconds": 0 }
        },
        {
          "id": "views",
          "name": "Views",
          "behavior": "snapViews",
          "applicationIds": ["tests"],
          "enabled": true,
          "viewControls": {
            "quickViews": [
              {
                "id": "right",
                "name": "Right",
                "yawDegrees": 90,
                "transitionSeconds": 1.0,
                "activationBindings": [
                  { "behavior": "toggle", "binding": { "type": "keyboard", "chord": ["F2"] } }
                ]
              }
            ]
          }
        }
      ]
    }
  }
}
)json";

void MidFlightEditsKeepTheSnap() {
    Peer::StartFromConfig(kMidFlightConfig);
    EngageFlight();
    Peer::TapSnap(kViews, kRight);
    Peer::Settle(1.1, "stacked snap entry");
    Expect(Peer::Stacked(), "The config-resolved motion profile must stack");
    const XrPosef snapped = Peer::Drive(0.0);

    Peer::EditConfig([](depthxr::ConfigDocument& config) {
        config.pivotxr.profiles[0].settings.apply_motion_to_snap_views = false;
    });
    ExpectSamePose(Peer::Drive(0.0), snapped, "Editing Apply Enhanced Motion to Snap Views must not move the view");
    Expect(Peer::SnapSelected() && Peer::Engaged(), "Editing Apply Enhanced Motion to Snap Views must keep the snap");

    Peer::EditConfig([](depthxr::ConfigDocument& config) { config.pivotxr.profiles[0].nudge_set_id = "b"; });
    ExpectSamePose(Peer::Drive(0.0), snapped, "Switching to an equivalent Nudge Set must not move the view");
    Expect(Peer::SnapSelected() && Peer::Engaged(), "Switching Nudge Sets must keep the snap");

    Peer::EditConfig([](depthxr::ConfigDocument& config) {
        config.pivotxr.profiles[0].activation_bindings[0].binding.chord = {"F8"};
    });
    Peer::Drive(0.0);
    Expect(!Peer::SnapSelected() && !Peer::Engaged(), "Changing activation bindings must still reset input state");
}

const char* kPerDirectionTransitionConfig = R"json(
{
  "version": 3,
  "core": { "enabled": true },
  "applications": [
    { "id": "tests", "name": "Tests", "enabled": true, "match": { "exe": "PivotSnapTests.exe" } }
  ],
  "modules": {
    "depthxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "pivotxr": {
      "enabled": true,
      "defaults": {},
      "profiles": [
        {
          "id": "flight",
          "name": "Flight",
          "applicationIds": ["tests"],
          "enabled": true,
          "activationBindings": [ { "behavior": "toggle", "binding": { "type": "keyboard", "chord": ["F1"] } } ],
          "settings": {
            "smoothing": 0,
            "activationRampSeconds": 0,
            "advancedAxes": true,
            "yawLeft": { "rotationMultiplier": 2, "deadzoneDegrees": 8, "maxExtraDegrees": 180, "deadzoneTransitionDegrees": 4 },
            "yawRight": { "rotationMultiplier": 2, "deadzoneDegrees": 8, "maxExtraDegrees": 180 }
          }
        }
      ]
    }
  }
}
)json";

void PerDirectionDeadzoneTransitionShapesTheExtraAngle() {
    Peer::StartFromConfig(kPerDirectionTransitionConfig);
    EngageFlight();
    // 2 degrees into a 4-degree transition: the integral of smoothstep gives
    // 4 * (0.5^3 - 0.5 * 0.5^4) = 0.375 degrees of extra yaw.
    Peer::Head(10.0);
    ExpectViewYaw(Peer::Drive(), 10.375, "Yaw Left's deadzone transition must soften the extra angle");
    Peer::Head(-10.0);
    ExpectViewYaw(Peer::Drive(), -12.0, "Yaw Right without a transition must keep the hard deadzone");
}

// xrLocateSpace runs several times per frame and xrLocateViews at least
// once, both under the layer's lock. Once frames are steady neither may
// allocate: two motion profiles sharing a Nudge Set, a Snap Views profile
// and anchored depth exercise every per-call list and per-frame cache.
void SteadyLocatesDoNotAllocate() {
    auto flight = Motion("Flight", Toggle("F1"), true);
    auto second = Motion("Second", Hold("F4"), false);
    AddYawLeftNudge(flight, "shared-cockpit-nudge-set", "F6");
    AddYawLeftNudge(second, "shared-cockpit-nudge-set", "F6");
    Peer::Start({flight, Snaps({View("Right", 90.0, Hold("F2"))}), second});
    Peer::EnableAnchoredDepth();
    EngageFlight();
    Peer::Head(12.0);
    for (int frame = 0; frame < 40; ++frame) {
        const Peer::FrameAllocations allocations = Peer::LocateFrame();
        Expect(allocations.depth_cached, "Anchored depth must cache the frame's submission geometry");
        // The first frames size the reused storage.
        if (frame < 5) {
            continue;
        }
        Expect(allocations.locate_views == 0,
               "A steady xrLocateViews made " + std::to_string(allocations.locate_views) + " heap allocations");
        Expect(allocations.locate_space == 0,
               "Three steady xrLocateSpace calls made " + std::to_string(allocations.locate_space) +
                   " heap allocations");
    }
}

// Runtime recenters reach Pivot only while it is on, and an idle Pivot
// consumes them as frames pass them instead of queueing them until the next
// engagement (the queue filled to 16 and then dropped every later recenter).
void RecentersDoNotQueueWhileIdleOrOff() {
    Peer::Start({Motion("Flight", Toggle("F1"), true)});
    for (int recenter = 0; recenter < 20; ++recenter) {
        Peer::Recenter(Peer::Now() + kFrameNanoseconds);
        Peer::Drive();
        Peer::Drive();
    }
    Expect(Peer::PendingReferenceChanges() == 0, "An idle Pivot left " +
                                                     std::to_string(Peer::PendingReferenceChanges()) +
                                                     " passed recenters queued");
    EngageFlight();
    Peer::Head(12.0);
    ExpectViewYaw(Peer::Drive(), 24.0, "Motion must work after recenters while idle");

    Peer::Start({Motion("Flight", Toggle("F1"), true)});
    Peer::DisablePivot();
    Peer::Recenter(Peer::Now() + kFrameNanoseconds);
    Expect(Peer::PendingReferenceChanges() == 0, "A recenter was queued for Pivot while it is off");
}

// A Toggle latch belongs to the engaged profile only. Toggling another
// profile on and off again must not leave the first profile latched, or its
// Hold binding would keep it engaged after release.
void HoldAfterAnotherProfileToggledReleases() {
    depthxr::PivotXrResolvedProfile flight = Motion("Flight", Toggle("F1"), false, 0.0);
    flight.activation_bindings.push_back(Hold("F3"));
    Peer::Start({flight, Motion("Second", Toggle("F4"), false, 0.0)});
    const auto tap = [](size_t profile) {
        Peer::TapActivation(profile);
        Peer::Drive(0.0);
    };
    tap(0);
    Expect(Peer::Engaged() && Peer::MotionOwner() == 0, "Flight's Toggle must engage it");
    tap(1);
    Expect(Peer::Engaged() && Peer::MotionOwner() == 1, "Second's Toggle must take over");
    tap(1);
    Expect(!Peer::Engaged(), "Second's Toggle must release it again");

    Peer::SetActivation(0, 1, true);
    Peer::Drive(0.0);
    Expect(Peer::Engaged() && Peer::MotionOwner() == 0, "Flight's Hold must engage it");
    Peer::SetActivation(0, 1, false);
    Peer::Drive(0.0);
    Expect(!Peer::Engaged(), "Releasing Flight's Hold must release it: no stale Toggle latch may keep it engaged");
}

// xrEndFrame lookups that find no Pivot delta are reported once per run. The
// frames an app located before a press took effect miss as expected and stay
// quiet; a miss after frames matched is reported, and so is a session whose
// submitted display times never match the located ones.
void ContinuityMissesAreReportedOnlyWhenUnexpected() {
    Peer::Start({Motion("Flight", Toggle("F1"), false)});
    // A pipelined app located two frames before the press took effect.
    Peer::Drive();
    const XrTime first = Peer::Now();
    Peer::Drive();
    const XrTime second = Peer::Now();
    Peer::TapActivation(kFlight);
    const XrTime engaged = Peer::Now();
    Expect(Peer::Engaged(), "Flight's Toggle must engage it");
    Peer::EndFrameAt(first);
    Peer::EndFrameAt(second);
    Expect(!Peer::ContinuityMissReported(), "Frames located before the press were reported as a continuity miss");
    Peer::EndFrameAt(engaged);
    Peer::Drive();
    Peer::EndFrameAt(Peer::Now() + 20'000'000);
    Expect(Peer::ContinuityMissReported(), "A miss after frames matched was not reported");
    Peer::Drive();
    Peer::EndFrameAt(Peer::Now());
    Expect(!Peer::ContinuityMissReported(), "A matched frame did not end the reported run");

    // Every frame is submitted 20 ms (beyond the match window) after the time
    // it was located for: no frame ever matches.
    Peer::Start({Motion("Flight", Toggle("F1"), false)});
    Peer::TapActivation(kFlight);
    for (int frame = 1; frame <= 3; ++frame) {
        Peer::EndFrameAt(Peer::Now() + 20'000'000);
        Expect(Peer::ContinuityMissReported() == (frame == 3),
               "Unmatched frame " + std::to_string(frame) +
                   (frame == 3 ? " was not reported: located and submitted display times never match"
                               : " was reported, though a press can follow two located frames"));
        Peer::Drive();
    }
}

struct Scenario {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main() {
    const std::vector<Scenario> scenarios = {
        {"stacked snap entry and release (Continuous)", [] { StackedSnapEntryAndReleaseAreContinuous(false); }},
        {"stacked snap entry and release (Stepped)", [] { StackedSnapEntryAndReleaseAreContinuous(true); }},
        {"snap pressed during xrLocateSpace", SnapPressedDuringLocateSpaceKeepsTheFrameTransform},
        {"instant release during xrLocateSpace", ZeroDurationReleaseDuringLocateSpaceWaitsForTheNextDrive},
        {"every transition during xrLocateSpace", EveryTransitionKeepsTheFrameTransformDuringLocateSpace},
        {"engage motion while snapped", EngagingOptedInMotionWhileSnappedMeasuresFromThePressPose},
        {"disengage motion while snapped", DisengagingMotionWhileSnappedKeepsTheSnap},
        {"switch snap views while stacked", SwitchingSnapViewsWhileStackedKeepsMotion},
        {"compatible profile switch mid-snap", CompatibleProfileSwitchKeepsTheSnapCentre},
        {"same-tuning profile switch mid-snap", SameTuningProfileSwitchIsSeamlessAndKeepsTheDeadzone},
        {"incompatible profile mid-snap", IncompatibleProfileKeepsTheSnapExclusive},
        {"non-opted motion toggles during a snap", NonOptedMotionTogglesLeaveAnExclusiveSnapSelected},
        {"instant stacked release then recenter", ZeroDurationStackedReleaseLeavesNoStackForARecenter},
        {"instant incompatible switch and release", ZeroDurationIncompatibleSwitchAndReleaseFinishCleanly},
        {"recenter while stacked", RecenterWhileStackedIsContinuous},
        {"hold-activated motion inside a snap", HoldActivatedMotionInsideAStackedSnap},
        {"motion toggled during snap entry", MotionToggledDuringSnapEntryKeepsTheTransition},
        {"always-active hold suspend inside a snap", AlwaysActiveHoldSuspendInsideAStackedSnap},
        {"always-active opt-in during an exclusive snap", AlwaysActiveOptInDoesNotHijackAnExclusiveSnap},
        {"hold suspend restores the always-active profile", HoldSuspendRestoresTheAlwaysActiveProfile},
        {"shared nudges across an exclusive snap", SharedNudgesSurviveAnExclusiveSnap},
        {"stacked snap keeps its nudges", StackedSnapKeepsItsNudgesWhenMotionChanges},
        {"Position Assist in check six", PositionAssistFollowsTheSnappedHeading},
        {"Position Assist with a pitched origin", PositionAssistIgnoresOriginPitch},
        {"snap entry from saturated pitch", SnapEntryFromSaturatedPitchStartsMovingImmediately},
        {"OSD motion and snap lines", OsdReportsMotionImmediatelyAndOnlyShowsSnapsWhenConfigured},
        {"mid-flight config edits", MidFlightEditsKeepTheSnap},
        {"per-direction deadzone transition", PerDirectionDeadzoneTransitionShapesTheExtraAngle},
        {"steady locates do not allocate", SteadyLocatesDoNotAllocate},
        {"Hold after another profile's Toggle", HoldAfterAnotherProfileToggledReleases},
        {"recenters while idle or off", RecentersDoNotQueueWhileIdleOrOff},
        {"continuity misses reported only when unexpected", ContinuityMissesAreReportedOnlyWhenUnexpected},
    };

    int failures = 0;
    for (const Scenario& scenario : scenarios) {
        try {
            scenario.run();
            std::cout << "PASS " << scenario.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "FAIL " << scenario.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (scenarios.size() - failures) << "/" << scenarios.size() << " Pivot snap scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
