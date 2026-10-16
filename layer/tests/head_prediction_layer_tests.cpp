#include <windows.h>

#include "depthxr/openxr_layer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
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
    inline static int locates = 0;

    // Yawing at a constant rate from a pitched-down seated start.
    static XrPosef Head(XrTime time) {
        const double yaw = Radians(degrees_per_second * static_cast<double>(time) / 1e9) / 2.0;
        const double pitch = Radians(-10.0) / 2.0;
        const XrQuaternionf q_yaw{0.0f, static_cast<float>(std::sin(yaw)), 0.0f, static_cast<float>(std::cos(yaw))};
        const XrQuaternionf q_pitch{static_cast<float>(std::sin(pitch)), 0.0f, 0.0f, static_cast<float>(std::cos(pitch))};
        return {Multiply(q_yaw, q_pitch), {0.0f, 1.2f, 0.0f}};
    }

    static XrPosef Answer(XrTime time) { return Head(std::min(time, now + limit)); }

    static XrPosef EyeOffset(uint32_t eye) {
        return {{0.0f, 0.0f, 0.0f, 1.0f}, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f}};
    }

    static XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
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
            state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                    XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
        }
        if (!views || capacity < 2) {
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
        FakeRuntime::locates = 0;
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
