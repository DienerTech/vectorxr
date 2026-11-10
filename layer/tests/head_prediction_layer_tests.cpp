#include <windows.h>

#include "depthxr/openxr_layer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// Head prediction past the runtime's limit through the real xrLocateViews and
// xrLocateSpace entry points, against a stand-in runtime whose head turns at a
// constant rate and which predicts it no further than 20 ms past "now".

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr XrTime kMs = 1'000'000;
constexpr XrSpaceLocationFlags kTrackedPose =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

const XrSession kSession = reinterpret_cast<XrSession>(std::uintptr_t{0x61});
const XrSpace kViewSpace = reinterpret_cast<XrSpace>(std::uintptr_t{1});
const XrSpace kLocalSpace = reinterpret_cast<XrSpace>(std::uintptr_t{2});
const XrSpace kControllerSpace = reinterpret_cast<XrSpace>(std::uintptr_t{3});

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

XrPosef Compose(const XrPosef& parent, const XrPosef& child) {
    const XrVector3f offset = Rotate(parent.orientation, child.position);
    return {Multiply(parent.orientation, child.orientation),
            {offset.x + parent.position.x, offset.y + parent.position.y, offset.z + parent.position.z}};
}

XrPosef Inverse(const XrPosef& pose) {
    const XrQuaternionf conjugate{-pose.orientation.x, -pose.orientation.y, -pose.orientation.z, pose.orientation.w};
    return {conjugate, Rotate(conjugate, {-pose.position.x, -pose.position.y, -pose.position.z})};
}

double AngleBetween(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf d = Multiply({-a.orientation.x, -a.orientation.y, -a.orientation.z, a.orientation.w},
                                     b.orientation);
    return Degrees(2.0 * std::atan2(std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y +
                                              static_cast<double>(d.z) * d.z),
                                    std::abs(static_cast<double>(d.w))));
}

double Distance(const XrVector3f& a, const XrVector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

std::string Describe(double value) {
    return std::to_string(value);
}

struct FakeRuntime {
    inline static XrTime now = 0;          // the runtime's clock, as VectorXR reads it
    inline static XrTime limit = 20 * kMs; // how far past now it predicts
    inline static double degrees_per_second = 120.0;
    inline static std::atomic<int> locates = 0;

    // Two threads locating one query at once (RacedLocatesOfOneDisplayTimeAnswerTheSame):
    // both runtime locates return together, and the first thread's head
    // extension holds until the second has missed the kept answers too and
    // is extending as well; the second goes on once the first has answered.
    struct Race {
        std::mutex mutex;
        std::condition_variable changed;
        int arrived{0};
        bool second_extending{false};
        bool first_answered{false};
    };
    inline static Race race;
    inline static thread_local int race_role = 0; // 1: the first thread, 2: the second
    inline static thread_local int race_locates = 0;

    static void RaceStep() {
        if (race_role == 0) {
            return;
        }
        std::unique_lock lock(race.mutex);
        if (++race_locates == 1) {
            ++race.arrived;
            race.changed.notify_all();
            race.changed.wait(lock, [] { return race.arrived == 2; });
        } else if (race_locates == 2 && race_role == 1) {
            race.changed.wait(lock, [] { return race.second_extending; });
        } else if (race_locates == 2) {
            race.second_extending = true;
            race.changed.notify_all();
            race.changed.wait(lock, [] { return race.first_answered; });
        }
    }

    // Yawing at a constant rate from a pitched-down seated start.
    static XrPosef Head(XrTime time) {
        const double yaw = Radians(degrees_per_second * static_cast<double>(time) / 1e9) / 2.0;
        const double pitch = Radians(-10.0) / 2.0;
        const XrQuaternionf q_yaw{0.0f, static_cast<float>(std::sin(yaw)), 0.0f, static_cast<float>(std::cos(yaw))};
        const XrQuaternionf q_pitch{static_cast<float>(std::sin(pitch)), 0.0f, 0.0f, static_cast<float>(std::cos(pitch))};
        return {Multiply(q_yaw, q_pitch), {0.0f, 1.2f, 0.0f}};
    }

    static XrPosef Answer(XrTime time) { return Head(std::min(time, now + limit)); }

    // Each eye turned outward by this much (a canted display: Pimax without
    // parallel projection, as pvr_calcEyePoses gives them); 0 is parallel.
    inline static double cant_degrees = 0.0;
    // False: the runtime cannot locate the views yet (PimaxXR before its first
    // valid head pose): flags 0 and the views left as they were.
    inline static bool views_valid = true;

    static XrPosef EyeOffset(uint32_t eye) {
        const double half = Radians(eye == 0 ? cant_degrees : -cant_degrees) / 2.0;
        return {{0.0f, static_cast<float>(std::sin(half)), 0.0f, static_cast<float>(std::cos(half))},
                {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
    }

    static XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
        RaceStep();
        ++locates;
        location->locationFlags = kTrackedPose;
        if (space == kViewSpace && base == kLocalSpace) {
            location->pose = Answer(time);
        } else if (space == kLocalSpace && base == kViewSpace) {
            location->pose = Inverse(Answer(time));
        } else {
            location->pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.1f, 0.9f, -0.3f}};
        }
        return XR_SUCCESS;
    }

    static XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                                           uint32_t capacity, uint32_t* count, XrView* views) {
        ++locates;
        *count = 2;
        if (state) {
            state->viewStateFlags = views_valid ? XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                                      XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                                                      XR_VIEW_STATE_POSITION_TRACKED_BIT
                                                : 0;
        }
        if (!views || capacity < 2 || !views_valid) {
            return XR_SUCCESS;
        }
        const XrPosef base =
            info->space == kViewSpace ? XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}} : Answer(info->displayTime);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            views[eye].pose = Compose(base, EyeOffset(eye));
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        }
        return XR_SUCCESS;
    }

    static XrResult XRAPI_PTR Convert(XrInstance, const LARGE_INTEGER*, XrTime* time) {
        *time = now;
        return XR_SUCCESS;
    }
};

} // namespace

namespace depthxr {

class HeadPredictionTestPeer {
public:
    static OpenXrLayer& Layer() { return OpenXrLayer::Instance(); }

    static void Reset(bool setting = true, bool framegen = true) {
        auto& layer = Layer();
        layer.ResetInstanceState();
        layer.ResetSessionState();
        layer.config_ = {};
        layer.has_loaded_config_ = true;
        layer.active_session_ = kSession;
        layer.resolved_settings_ = {};
        layer.resolved_settings_.core.enabled = true;
        layer.resolved_settings_.framegen.enabled = framegen;
        layer.resolved_settings_.framegen.extend_head_prediction = setting;
        layer.resolved_settings_generation_ = layer.config_generation_;
        layer.resolved_settings_session_ = layer.active_session_;
        layer.tracked_view_spaces_ = {kViewSpace};
        layer.tracked_local_spaces_ = {kLocalSpace};
        layer.internal_view_space_ = kViewSpace;
        layer.next_locate_space_ = &FakeRuntime::LocateSpace;
        layer.next_locate_views_ = &FakeRuntime::LocateViews;
        layer.turbo_convert_counter_time_ = reinterpret_cast<PFN_xrVoidFunction>(&FakeRuntime::Convert);
        layer.framegen_owns_pacing_.store(framegen, std::memory_order_release);
        FakeRuntime::now = 2'000 * kMs;
        FakeRuntime::limit = 20 * kMs;
        FakeRuntime::degrees_per_second = 120.0;
        FakeRuntime::cant_degrees = 0.0;
        FakeRuntime::views_valid = true;
        FakeRuntime::locates = 0;
    }

    static bool QuadFrameCached(XrTime time) {
        OpenXrLayer::QuadViewsFrameState frame;
        XrTime matched = 0;
        return Layer().FindQuadViewsFrame(time, &frame, &matched) && matched == time;
    }
    static XrTime EyeOffsetsTime() { return Layer().cached_eye_offsets_display_time_; }
    static bool EnsureEyeOffsets(XrTime time) {
        return Layer().EnsureEyeOffsets(kSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, time, 2);
    }

    // Emulated quad views from the stand-in's stereo eyes, the focus held
    // `focus_yaw` right of head-forward (as a gaze the layer holds): each
    // focus view's centre, the tangent mid-way across it in its eye's frame.
    static std::array<double, 2> FocusCentres(XrTime time, XrSpace space, double focus_yaw) {
        auto& layer = Layer();
        layer.resolved_settings_.quadviews.enabled = true;
        layer.resolved_settings_.quadviews.tracking_mode = QuadViewsTrackingMode::Head;
        layer.quadviews_session_active_ = true;
        layer.d3d11_graphics_extension_requested_ = true;
        layer.varjo_compatible_quadviews_active_ = false;
        layer.quadviews_smoothed_focus_yaw_radians_ = focus_yaw;
        layer.quadviews_smoothed_focus_pitch_radians_ = 0.0;
        layer.quadviews_last_valid_gaze_wall_time_.reset();
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
        info.displayTime = time;
        info.space = space;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        Expect(layer.LocateViews(kSession, &info, &state, 4, &count, views) == XR_SUCCESS && count == 4,
               "quad xrLocateViews must succeed");
        Expect(std::memcmp(&views[2].pose, &views[0].pose, sizeof(XrPosef)) == 0 &&
                   std::memcmp(&views[3].pose, &views[1].pose, sizeof(XrPosef)) == 0,
               "each focus view must keep its eye's pose");
        std::array<double, 2> centres{};
        for (uint32_t eye = 0; eye < 2; ++eye) {
            centres[eye] = (std::tan(views[2 + eye].fov.angleLeft) + std::tan(views[2 + eye].fov.angleRight)) / 2.0;
        }
        return centres;
    }

    static void Finish() {
        auto& layer = Layer();
        layer.framegen_owns_pacing_.store(false, std::memory_order_release);
        layer.turbo_convert_counter_time_ = nullptr;
    }

    // The head recovered from the eyes xrLocateViews returns, asked at
    // `now` for `time`; also checks the eyes stay a head apart.
    static XrPosef ViewsHead(XrTime time, XrSpace space = kLocalSpace) {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = time;
        info.space = space;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        Expect(Layer().LocateViews(kSession, &info, &state, 2, &count, views) == XR_SUCCESS && count == 2,
               "xrLocateViews must succeed");
        Expect(std::abs(Distance(views[0].pose.position, views[1].pose.position) - 0.064) < 1e-4,
               "the eyes must stay 64 mm apart");
        Expect(AngleBetween(views[0].pose, views[1].pose) < 1e-3, "both eyes must look the same way");
        return Compose(views[0].pose, Inverse(FakeRuntime::EyeOffset(0)));
    }

    // Whether xrLocateViews and xrLocateSpace returned the runtime's answers
    // bit for bit; the angle they differ by otherwise.
    static double UntouchedOrDegrees(XrTime time) {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = time;
        info.space = kLocalSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t count = 0;
        Expect(Layer().LocateViews(kSession, &info, &state, 2, &count, views) == XR_SUCCESS && count == 2,
               "xrLocateViews must succeed");
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        Expect(Layer().LocateSpace(kViewSpace, kLocalSpace, time, &location) == XR_SUCCESS, "xrLocateSpace must succeed");
        const XrPosef answer = FakeRuntime::Answer(time);
        bool same = std::memcmp(&location.pose, &answer, sizeof(answer)) == 0;
        double degrees = AngleBetween(location.pose, answer);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const XrPosef expected = Compose(answer, FakeRuntime::EyeOffset(eye));
            same = same && std::memcmp(&views[eye].pose, &expected, sizeof(expected)) == 0;
            degrees = std::max(degrees, AngleBetween(views[eye].pose, expected));
        }
        return same ? 0.0 : std::max(degrees, 1e-9);
    }

    static XrPosef SpaceHead(XrTime time) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        Expect(Layer().LocateSpace(kViewSpace, kLocalSpace, time, &location) == XR_SUCCESS, "xrLocateSpace must succeed");
        return location.pose;
    }

    static XrPosef InverseSpaceHead(XrTime time) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        Expect(Layer().LocateSpace(kLocalSpace, kViewSpace, time, &location) == XR_SUCCESS, "xrLocateSpace must succeed");
        return Inverse(location.pose);
    }

    // Probes as the game's view locates come, with the probe interval
    // passed each time.
    static void SettleLimit() {
        auto& layer = Layer();
        for (int i = 0; i < 5 && !layer.head_prediction_limit_.Settled(); ++i) {
            layer.head_prediction_last_probe_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            FakeRuntime::now += 11 * kMs;
            ViewsHead(FakeRuntime::now + 100 * kMs);
        }
    }

    // The next view locate may probe, whenever the last one was.
    static void ProbeDue() {
        Layer().head_prediction_last_probe_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }
    static void DropClock() { Layer().turbo_convert_counter_time_ = nullptr; }

    static bool Settled() { return Layer().head_prediction_limit_.Settled(); }
    static XrTime Limit() { return Layer().head_prediction_limit_.LimitNs(); }
    static std::string OsdDetail() { return Layer().HeadPredictionOsdDetail(); }
};

} // namespace depthxr

namespace {

using Peer = depthxr::HeadPredictionTestPeer;

// Where the views must turn to: the runtime's answer 18 ms ahead, carried on
// at its speed over nine tenths of the rest (head_prediction.h's tuning).
XrPosef Expected(XrTime time) {
    const XrTime anchor = FakeRuntime::now + 18 * kMs;
    const double carried = 0.9 * static_cast<double>(time - anchor);
    return FakeRuntime::Head(anchor + static_cast<XrTime>(carried));
}

void ProbesSettleTheLimitBeforeAnyTurn() {
    Peer::Reset();
    const XrTime time = FakeRuntime::now + 100 * kMs;
    const XrPosef head = Peer::ViewsHead(time);
    Expect(AngleBetween(head, FakeRuntime::Answer(time)) < 1e-3, "before the limit is known, views stay the runtime's");
    Expect(Peer::OsdDetail() == "Head: measuring", "the OSD must say the limit is being measured");
    Peer::SettleLimit();
    Expect(Peer::Settled(), "probes during a turn must settle the limit");
    Expect(std::abs(static_cast<double>(Peer::Limit() - 20 * kMs)) <= 0.5 * kMs,
           "the settled limit must be the runtime's 20 ms, got " + Describe(Peer::Limit() / 1e6));
    Peer::Finish();
}

void ViewsTurnToTheDisplayTime() {
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 90 * kMs;
    const XrPosef head = Peer::ViewsHead(time);
    const double error = AngleBetween(head, Expected(time));
    Expect(error < 0.02, "views must turn to the predicted head (off by " + Describe(error) + " deg)");
    const double runtime_lag = AngleBetween(FakeRuntime::Answer(time), FakeRuntime::Head(time));
    const double left = AngleBetween(head, FakeRuntime::Head(time));
    Expect(left < 0.15 * runtime_lag, "the lag left must be a small share of the runtime's (" + Describe(left) +
                                          " of " + Describe(runtime_lag) + " deg)");
    Expect(Distance(head.position, FakeRuntime::Head(time).position) < 1e-4, "the head must not move");
    Expect(Peer::OsdDetail().rfind("Head +", 0) == 0, "the OSD must show how far past the limit: " + Peer::OsdDetail());
    Peer::Finish();
}

void SpaceLocatesMatchTheViews() {
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 110 * kMs;
    const XrPosef views = Peer::ViewsHead(time);
    const XrPosef space = Peer::SpaceHead(time);
    const XrPosef inverse = Peer::InverseSpaceHead(time);
    Expect(AngleBetween(views, space) < 1e-3 && Distance(views.position, space.position) < 1e-4,
           "VIEW located in LOCAL must match the views (" + Describe(AngleBetween(views, space)) + " deg)");
    Expect(AngleBetween(views, inverse) < 1e-3 && Distance(views.position, inverse.position) < 1e-4,
           "LOCAL located in VIEW must be its inverse (" + Describe(AngleBetween(views, inverse)) + " deg)");
    Peer::Finish();
}

void LaterLocatesOfOneDisplayTimeAgree() {
    // MSFS locates a display time on its main thread, then ~30 ms later on its
    // render thread: the runtime's own answers differ by that much head motion.
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 100 * kMs;
    const XrPosef first = Peer::ViewsHead(time);
    const XrPosef runtime_first = FakeRuntime::Answer(time);
    FakeRuntime::now += 30 * kMs;
    const XrPosef second = Peer::ViewsHead(time);
    const XrPosef runtime_second = FakeRuntime::Answer(time);
    const double runtime_spread = AngleBetween(runtime_first, runtime_second);
    const double spread = AngleBetween(first, second);
    Expect(runtime_spread > 3.0, "the runtime's answers must move with its clock");
    // Damped to 0.9, the two answers keep a tenth of the runtime's spread.
    Expect(spread < 0.15 * runtime_spread, "both locates must answer nearly the same head (" + Describe(spread) +
                                               " vs the runtime's " + Describe(runtime_spread) + " deg)");
    Peer::Finish();
}

void ViewRelativeAndOtherLocatesAreUntouched() {
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 100 * kMs;
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time;
    info.space = kViewSpace;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    Expect(Peer::Layer().LocateViews(kSession, &info, &state, 2, &count, views) == XR_SUCCESS, "must succeed");
    Expect(AngleBetween(views[0].pose, FakeRuntime::EyeOffset(0)) < 1e-6 &&
               Distance(views[0].pose.position, FakeRuntime::EyeOffset(0).position) < 1e-6,
           "views relative to VIEW must stay the eye offsets");
    XrSpaceLocation controller{XR_TYPE_SPACE_LOCATION};
    Expect(Peer::Layer().LocateSpace(kControllerSpace, kLocalSpace, time, &controller) == XR_SUCCESS, "must succeed");
    Expect(controller.pose.position.x == 0.1f && controller.pose.orientation.w == 1.0f,
           "a controller located in LOCAL must stay the runtime's");
    Peer::Finish();
}

void NothingWithinTheRuntimesLimit() {
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 15 * kMs;
    const double moved = Peer::UntouchedOrDegrees(time);
    Expect(moved == 0.0, "a display time the runtime predicts must stay its answer (moved " + Describe(moved) + " deg)");
    Peer::Finish();
}

void AStillHeadIsLeftAlone() {
    Peer::Reset();
    Peer::SettleLimit();
    FakeRuntime::degrees_per_second = 1.0;
    const XrTime time = FakeRuntime::now + 100 * kMs;
    const double moved = Peer::UntouchedOrDegrees(time);
    Expect(moved == 0.0, "a head turning 1 deg/s must not be extended (moved " + Describe(moved) + " deg)");
    Peer::Finish();
}

void OffByDefaultAndOnlyUnderFrameGeneration() {
    for (const auto& [setting, framegen, label] :
         {std::tuple{false, true, "with the setting off"}, std::tuple{true, false, "without Frame Generation"}}) {
        Peer::Reset(setting, framegen);
        for (int i = 0; i < 4; ++i) {
            Peer::ProbeDue();
            FakeRuntime::now += 11 * kMs;
            const XrTime time = FakeRuntime::now + 100 * kMs;
            const double moved = Peer::UntouchedOrDegrees(time);
            Expect(moved == 0.0, std::string("views must stay the runtime's ") + label + " (moved " + Describe(moved) +
                                     " deg)");
        }
        Expect(FakeRuntime::locates == 8, std::string("no extra runtime locates ") + label + " (" +
                                              std::to_string(FakeRuntime::locates) + ")");
        Expect(!Peer::Settled() && Peer::OsdDetail().empty(), std::string("no probe and no OSD line ") + label);
        Peer::Finish();
    }
}

void ARuntimeThatPredictsIsLeftAlone() {
    Peer::Reset();
    FakeRuntime::limit = 1'000 * kMs;
    Peer::SettleLimit();
    Expect(Peer::Settled() && Peer::OsdDetail() == "Head: runtime predicts",
           "a runtime predicting every probe offset must settle as unlimited: " + Peer::OsdDetail());
    const int before = FakeRuntime::locates;
    const XrTime time = FakeRuntime::now + 100 * kMs;
    const double moved = Peer::UntouchedOrDegrees(time);
    Expect(moved == 0.0, "its answers must stay as they are (moved " + Describe(moved) + " deg)");
    Expect(FakeRuntime::locates == before + 2, "and cost no extra locates");
    Peer::Finish();
}

void NoClockNoExtension() {
    Peer::Reset();
    Peer::DropClock();
    for (int i = 0; i < 3; ++i) {
        Peer::ProbeDue();
        const XrTime time = FakeRuntime::now + 100 * kMs;
        const double moved = Peer::UntouchedOrDegrees(time);
        Expect(moved == 0.0, "without the runtime's clock nothing can be measured (moved " + Describe(moved) + " deg)");
    }
    Expect(Peer::OsdDetail() == "Head: no runtime clock", "the OSD must say why: " + Peer::OsdDetail());
    Peer::Finish();
}

void CantedEyesPlaceTheInsetThroughEachEye() {
    // Head-forward and a gaze are directions in VIEW: in an eye turned c
    // outward they fall c further in (QVF's ProjectPoint), so both insets
    // cover the same direction. Parallel eyes keep the angles as they were.
    Peer::Reset(false, false);
    const double cant = 10.0;
    for (const XrSpace space : {kLocalSpace, kViewSpace}) {
        for (const double gaze : {0.0, 8.0, -15.0}) {
            FakeRuntime::cant_degrees = cant;
            const XrTime time = FakeRuntime::now + static_cast<XrTime>(10 + gaze) * kMs;
            const auto canted = Peer::FocusCentres(time, space, Radians(gaze));
            const double left = std::tan(Radians(gaze + cant));
            const double right = std::tan(Radians(gaze - cant));
            Expect(std::abs(canted[0] - left) < 1e-4 && std::abs(canted[1] - right) < 1e-4,
                   "a canted eye's inset must sit where the direction falls in it: gaze " + Describe(gaze) +
                       " deg, left " + Describe(canted[0]) + " (want " + Describe(left) + "), right " +
                       Describe(canted[1]) + " (want " + Describe(right) + ")");
            FakeRuntime::cant_degrees = 0.0;
            const auto parallel = Peer::FocusCentres(time + 1, space, Radians(gaze));
            Expect(std::abs(parallel[0] - std::tan(Radians(gaze))) < 1e-6 &&
                       std::abs(parallel[1] - std::tan(Radians(gaze))) < 1e-6,
                   "parallel eyes must keep the inset at the gaze's own angle");
        }
    }
    // The eyes' turn from the head is asked for once per display time (the
    // same one Pivot's eye offsets keep), never for parallel eyes.
    FakeRuntime::cant_degrees = cant;
    const XrTime time = FakeRuntime::now + 50 * kMs;
    FakeRuntime::locates = 0;
    Peer::FocusCentres(time, kLocalSpace, 0.0);
    Peer::FocusCentres(time, kLocalSpace, 0.0);
    Expect(FakeRuntime::locates == 3, "canted eyes: one extra locate per display time (" +
                                          std::to_string(FakeRuntime::locates) + ")");
    Peer::FocusCentres(time + kMs, kLocalSpace, 0.0);
    Expect(FakeRuntime::locates == 5, "a new display time must locate the eyes again (" +
                                          std::to_string(FakeRuntime::locates) + ")");
    FakeRuntime::cant_degrees = 0.0;
    FakeRuntime::locates = 0;
    Peer::FocusCentres(time + 2 * kMs, kLocalSpace, 0.0);
    Expect(FakeRuntime::locates == 1, "parallel eyes: no extra locate (" + std::to_string(FakeRuntime::locates) + ")");
    Peer::Finish();
}

void ViewsTheRuntimeDidNotLocateAreLeftAlone() {
    // PimaxXR answers "all or nothing": before its first valid head pose it
    // returns flags 0 and leaves the views untouched. Nothing is cached or
    // derived from such views.
    Peer::Reset(false, false);
    FakeRuntime::cant_degrees = 10.0;
    FakeRuntime::views_valid = false;
    const XrTime time = FakeRuntime::now + 20 * kMs;
    FakeRuntime::locates = 0;
    Peer::FocusCentres(time, kLocalSpace, 0.0);
    Expect(!Peer::QuadFrameCached(time), "a quad frame of views the runtime did not locate must not be cached");
    Expect(FakeRuntime::locates == 1 && Peer::EyeOffsetsTime() != time,
           "no eye offsets may be derived from views the runtime did not locate");
    Expect(!Peer::EnsureEyeOffsets(time + kMs) && Peer::EyeOffsetsTime() != time + kMs,
           "eye offsets the runtime did not locate must not be kept");
    FakeRuntime::views_valid = true;
    Peer::FocusCentres(time + 2 * kMs, kLocalSpace, 0.0);
    Expect(Peer::QuadFrameCached(time + 2 * kMs) && Peer::EyeOffsetsTime() == time + 2 * kMs,
           "located views must be cached as before");
    Peer::Finish();
}

void RepeatedSpaceLocatesOfOneDisplayTimeAnswerTheSame() {
    // DCS locates VIEW in LOCAL several times a frame at one display time.
    // The first locate extends the head (three extra runtime locates); the
    // others give the same answer for one locate each. A new display time is
    // extended afresh: nothing is kept across display times.
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 100 * kMs;
    FakeRuntime::locates = 0;
    const XrPosef first = Peer::SpaceHead(time);
    Expect(FakeRuntime::locates == 4, "the first locate extends the head with three extra locates (" +
                                          std::to_string(FakeRuntime::locates) + ")");
    Expect(AngleBetween(first, FakeRuntime::Answer(time)) > 1.0, "the head must be extended");
    for (int i = 1; i <= 3; ++i) {
        FakeRuntime::now += 4 * kMs;
        const XrPosef again = Peer::SpaceHead(time);
        Expect(std::memcmp(&again, &first, sizeof(first)) == 0,
               "a later locate of the same display time must answer the same (off by " +
                   Describe(AngleBetween(again, first)) + " deg)");
    }
    Expect(FakeRuntime::locates == 7, "later locates of the same display time cost one runtime locate each (" +
                                          std::to_string(FakeRuntime::locates) + ")");
    const XrPosef next = Peer::SpaceHead(time + 11 * kMs);
    Expect(FakeRuntime::locates == 11, "a new display time is extended afresh (" +
                                           std::to_string(FakeRuntime::locates) + ")");
    Expect(AngleBetween(next, first) > 0.5, "a new display time must not take the last one's answer");
    Peer::Finish();
}

void RacedLocatesOfOneDisplayTimeAnswerTheSame() {
    // A game's two threads locate VIEW at one display time together: both miss
    // the kept answers and extend the head, the second against a later "now".
    // Both must answer the same, the first one kept.
    Peer::Reset();
    Peer::SettleLimit();
    const XrTime time = FakeRuntime::now + 100 * kMs;
    FakeRuntime::locates = 0;
    FakeRuntime::race.arrived = 0;
    FakeRuntime::race.second_extending = false;
    FakeRuntime::race.first_answered = false;
    XrPosef first{};
    XrPosef second{};
    std::thread first_thread([&] {
        FakeRuntime::race_role = 1;
        first = Peer::SpaceHead(time);
        std::scoped_lock lock(FakeRuntime::race.mutex);
        FakeRuntime::now += 4 * kMs;
        FakeRuntime::race.first_answered = true;
        FakeRuntime::race.changed.notify_all();
    });
    std::thread second_thread([&] {
        FakeRuntime::race_role = 2;
        second = Peer::SpaceHead(time);
    });
    first_thread.join();
    second_thread.join();
    Expect(FakeRuntime::locates == 8, "both threads must have missed and extended the head (" +
                                          std::to_string(FakeRuntime::locates.load()) + " locates)");
    Expect(AngleBetween(first, FakeRuntime::Answer(time)) > 1.0, "the head must be extended");
    Expect(std::memcmp(&first, &second, sizeof(first)) == 0,
           "two threads' locates of one display time must answer the same (off by " +
               Describe(AngleBetween(first, second)) + " deg)");
    const XrPosef again = Peer::SpaceHead(time);
    Expect(std::memcmp(&again, &first, sizeof(first)) == 0, "a later locate must answer as the first thread did");
    Peer::Finish();
}

struct Scenario {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main() {
    const std::vector<Scenario> scenarios = {
        {"probes settle the limit before any turn", ProbesSettleTheLimitBeforeAnyTurn},
        {"views turn to the display time", ViewsTurnToTheDisplayTime},
        {"space locates match the views", SpaceLocatesMatchTheViews},
        {"later locates of one display time agree", LaterLocatesOfOneDisplayTimeAgree},
        {"VIEW-relative and other locates are untouched", ViewRelativeAndOtherLocatesAreUntouched},
        {"nothing within the runtime's limit", NothingWithinTheRuntimesLimit},
        {"a still head is left alone", AStillHeadIsLeftAlone},
        {"off by default and only under Frame Generation", OffByDefaultAndOnlyUnderFrameGeneration},
        {"a runtime that predicts is left alone", ARuntimeThatPredictsIsLeftAlone},
        {"no runtime clock, no extension", NoClockNoExtension},
        {"repeated space locates of one display time answer the same", RepeatedSpaceLocatesOfOneDisplayTimeAnswerTheSame},
        {"raced locates of one display time answer the same", RacedLocatesOfOneDisplayTimeAnswerTheSame},
        {"canted eyes place the inset through each eye", CantedEyesPlaceTheInsetThroughEachEye},
        {"views the runtime did not locate are left alone", ViewsTheRuntimeDidNotLocateAreLeftAlone},
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
    std::cout << (scenarios.size() - failures) << "/" << scenarios.size()
              << " head prediction layer scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
