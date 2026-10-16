#include "depthxr/head_prediction.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Head prediction past the runtime's limit, against a stand-in runtime that
// answers every locate with the head at min(time asked for, now + limit).

namespace {

using namespace depthxr;

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kMs = 1'000'000;

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

HeadQuaternion Multiply(const HeadQuaternion& a, const HeadQuaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

HeadQuaternion Conjugate(const HeadQuaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

HeadQuaternion AxisAngle(HeadVector axis, double degrees) {
    const double n = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const double half = Radians(degrees) / 2.0;
    const double s = std::sin(half) / n;
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

HeadVector Rotate(const HeadQuaternion& q, const HeadVector& v) {
    const HeadQuaternion r = Multiply(Multiply(q, HeadQuaternion{v.x, v.y, v.z, 0.0}), Conjugate(q));
    return {r.x, r.y, r.z};
}

double AngleBetween(const HeadQuaternion& a, const HeadQuaternion& b) {
    const HeadQuaternion d = Multiply(Conjugate(a), b);
    return Degrees(2.0 * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::abs(d.w)));
}

double Distance(const HeadVector& a, const HeadVector& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// A head turning at a constant rate about a fixed axis from a pitched-down,
// seated start.
struct Turn {
    HeadVector axis{0.0, 1.0, 0.0};
    double degrees_per_second{150.0};
    HeadPredictionPose start{AxisAngle({1.0, 0.0, 0.0}, -10.0), {0.0, 1.2, 0.0}};

    HeadPredictionPose At(std::int64_t time) const {
        HeadPredictionPose pose = start;
        pose.orientation = Multiply(AxisAngle(axis, degrees_per_second * static_cast<double>(time) / 1e9),
                                    start.orientation);
        return pose;
    }
};

// What a runtime that predicts `limit` past `now` and no further answers.
struct CappedRuntime {
    Turn turn;
    std::int64_t limit{20 * kMs};

    HeadPredictionPose Locate(std::int64_t now, std::int64_t time) const { return turn.At(std::min(time, now + limit)); }

    HeadPredictionAnchors Anchors(std::int64_t now, std::int64_t target, std::int64_t assumed_limit,
                                  const HeadPredictionTuning& tuning = {}) const {
        HeadPredictionAnchors anchors;
        anchors.target = target;
        anchors.anchor_time = now + assumed_limit - tuning.anchor_margin_ns;
        anchors.at_target = Locate(now, target);
        anchors.anchor = Locate(now, anchors.anchor_time).orientation;
        anchors.before = Locate(now, anchors.anchor_time - tuning.velocity_span_ns).orientation;
        return anchors;
    }

    std::vector<PredictionProbeSample> Probe(std::int64_t now) const {
        std::vector<PredictionProbeSample> samples;
        for (const std::int64_t offset : PredictionProbeOffsets()) {
            samples.push_back({offset, Locate(now, now + offset).orientation});
        }
        return samples;
    }
};

HeadPredictionTuning Undamped() {
    HeadPredictionTuning tuning;
    tuning.damping = 1.0;
    return tuning;
}

void ReachesTheHeadAtTheDisplayTime() {
    const CappedRuntime runtime;
    const std::int64_t now = 500 * kMs;
    const std::int64_t target = now + 70 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, target, runtime.limit, Undamped());
    const HeadExtension extension = ComputeHeadExtension(anchors, Undamped());
    Expect(extension.applied, "a turning head 50 ms past the runtime's limit must be extended");
    const HeadPredictionPose turned = ApplyHeadExtension(extension, anchors.at_target);
    const double error = AngleBetween(turned.orientation, runtime.turn.At(target).orientation);
    Expect(error < 0.01, "undamped, the head must reach where it is at the display time (off by " +
                             std::to_string(error) + " deg)");
    Expect(std::abs(extension.degrees - 150.0 * 0.050) < 0.05,
           "the turn must cover the 50 ms the runtime left out (" + std::to_string(extension.degrees) + " deg)");
    Expect(std::abs(extension.speed_degrees_per_second - 150.0) < 0.5, "the runtime's own speed must be measured");
    Expect(Distance(turned.position, anchors.at_target.position) < 1e-9, "the head itself must not move");
}

void DampingLeavesTheExpectedShare() {
    CappedRuntime runtime;
    runtime.turn.degrees_per_second = 100.0;
    const HeadPredictionTuning tuning;  // damping 0.9
    const std::int64_t now = 500 * kMs;
    const std::int64_t target = now + 100 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, target, runtime.limit, tuning);
    const HeadExtension extension = ComputeHeadExtension(anchors, tuning);
    const HeadPredictionPose turned = ApplyHeadExtension(extension, anchors.at_target);
    const double short_by = AngleBetween(turned.orientation, runtime.turn.At(target).orientation);
    // A tenth of the turn from the anchor (18 ms ahead) to the target.
    const double expected = 0.1 * 100.0 * static_cast<double>(target - anchors.anchor_time) / 1e9;
    Expect(std::abs(short_by - expected) < 0.01,
           "damping 0.9 must stop a tenth short (" + std::to_string(short_by) + " vs " + std::to_string(expected) + ")");
}

void TurnsEyesAboutTheHead() {
    const CappedRuntime runtime;
    const std::int64_t now = 500 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, now + 90 * kMs, runtime.limit);
    const HeadExtension extension = ComputeHeadExtension(anchors, {});
    Expect(extension.applied, "must be extended");
    std::vector<HeadPredictionPose> eyes;
    for (const double side : {-0.032, 0.032}) {
        HeadPredictionPose eye = anchors.at_target;
        const HeadVector offset = Rotate(eye.orientation, {side, 0.0, 0.0});
        eye.position = {eye.position.x + offset.x, eye.position.y + offset.y, eye.position.z + offset.z};
        eyes.push_back(ApplyHeadExtension(extension, eye));
    }
    Expect(std::abs(Distance(eyes[0].position, eyes[1].position) - 0.064) < 1e-6, "the eyes must keep their distance");
    const HeadVector middle{(eyes[0].position.x + eyes[1].position.x) / 2, (eyes[0].position.y + eyes[1].position.y) / 2,
                            (eyes[0].position.z + eyes[1].position.z) / 2};
    Expect(Distance(middle, anchors.at_target.position) < 1e-6, "the eyes must turn about the head");
    const HeadPredictionPose head = ApplyHeadExtension(extension, anchors.at_target);
    for (const HeadPredictionPose& eye : eyes) {
        Expect(AngleBetween(eye.orientation, head.orientation) < 1e-6, "every eye must take the head's turn");
        const HeadVector across = Rotate(head.orientation, {1.0, 0.0, 0.0});
        const double along = (eye.position.x - middle.x) * across.x + (eye.position.y - middle.y) * across.y +
                             (eye.position.z - middle.z) * across.z;
        Expect(std::abs(std::abs(along) - 0.032) < 1e-6, "the eyes must stay level across the turned head");
    }
}

void InverseLocateMatches() {
    const CappedRuntime runtime;
    const std::int64_t now = 500 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, now + 120 * kMs, runtime.limit);
    const HeadExtension extension = ComputeHeadExtension(anchors, {});
    const HeadPredictionPose head = ApplyHeadExtension(extension, anchors.at_target);
    // The world located in the head: the inverse pose.
    HeadPredictionPose world_in_head;
    world_in_head.orientation = Conjugate(anchors.at_target.orientation);
    const HeadVector back = Rotate(world_in_head.orientation, anchors.at_target.position);
    world_in_head.position = {-back.x, -back.y, -back.z};
    const HeadPredictionPose turned = ApplyHeadExtensionInverse(extension, world_in_head);
    Expect(AngleBetween(turned.orientation, Conjugate(head.orientation)) < 1e-6,
           "the inverse locate must turn the other way");
    const HeadVector expected = Rotate(Conjugate(head.orientation), head.position);
    Expect(Distance(turned.position, {-expected.x, -expected.y, -expected.z}) < 1e-6,
           "the inverse locate must be the inverse of the turned head");
}

void AnAnchorShortOfTheLimitCostsNothing() {
    // The runtime predicts 25 ms; VectorXR measured 20.
    CappedRuntime runtime;
    runtime.limit = 25 * kMs;
    const std::int64_t now = 500 * kMs;
    const std::int64_t target = now + 70 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, target, 20 * kMs, Undamped());
    const HeadExtension extension = ComputeHeadExtension(anchors, Undamped());
    const HeadPredictionPose turned = ApplyHeadExtension(extension, anchors.at_target);
    Expect(AngleBetween(turned.orientation, runtime.turn.At(target).orientation) < 0.01,
           "a limit measured short must still reach the head at the display time");
}

void ALimitMeasuredLongNeverOvershoots() {
    // The runtime predicts 12 ms; VectorXR assumes 20: the velocity answers
    // straddle the runtime's limit and read slow.
    CappedRuntime runtime;
    runtime.limit = 12 * kMs;
    const std::int64_t now = 500 * kMs;
    const std::int64_t target = now + 100 * kMs;
    const HeadPredictionAnchors anchors = runtime.Anchors(now, target, 20 * kMs, Undamped());
    const HeadExtension extension = ComputeHeadExtension(anchors, Undamped());
    const HeadPredictionPose turned = ApplyHeadExtension(extension, anchors.at_target);
    const double reached = AngleBetween(anchors.at_target.orientation, turned.orientation);
    const double needed = AngleBetween(anchors.at_target.orientation, runtime.turn.At(target).orientation);
    Expect(reached < needed, "a limit measured long must fall short, not overshoot");
}

void GatesSlowHeads() {
    const std::int64_t now = 500 * kMs;
    const HeadPredictionTuning tuning;
    const auto degrees_at = [&](double speed) {
        CappedRuntime runtime;
        runtime.turn.degrees_per_second = speed;
        const HeadPredictionAnchors anchors = runtime.Anchors(now, now + 100 * kMs, runtime.limit, tuning);
        return ComputeHeadExtension(anchors, tuning);
    };
    Expect(!degrees_at(2.0).applied, "a head turning 2 deg/s must not be extended");
    Expect(!degrees_at(-2.9).applied, "the gate must not depend on the direction");
    const HeadExtension half = degrees_at(6.5);
    const HeadExtension full = degrees_at(20.0);
    Expect(half.applied && full.applied, "faster heads must be extended");
    const double half_expected = 0.5 * 0.9 * 6.5 * 0.082 - 0.5 * 6.5 * 0.002;
    Expect(std::abs(half.degrees - half_expected) < 0.01,
           "half way up the gate, half the turn (" + std::to_string(half.degrees) + ")");
}

void RejectsGlitchesAndCapsTheTurn() {
    const std::int64_t now = 500 * kMs;
    CappedRuntime fast;
    fast.turn.degrees_per_second = 1000.0;
    Expect(!ComputeHeadExtension(fast.Anchors(now, now + 100 * kMs, fast.limit), {}).applied,
           "a head turning 1000 deg/s reads as a glitch");
    CappedRuntime quick;
    quick.turn.degrees_per_second = 500.0;
    const HeadExtension capped = ComputeHeadExtension(quick.Anchors(now, now + 150 * kMs, quick.limit), {});
    Expect(capped.applied && std::abs(capped.degrees - 10.0) < 1e-6, "no extension may exceed 10 degrees");
    const HeadPredictionAnchors anchors = quick.Anchors(now, now + 150 * kMs, quick.limit);
    const HeadPredictionPose turned = ApplyHeadExtension(capped, anchors.at_target);
    Expect(std::abs(AngleBetween(anchors.at_target.orientation, turned.orientation) - 10.0) < 1e-4,
           "the capped turn must be what is applied");
}

void NothingWithinTheLimit() {
    const CappedRuntime runtime;
    const std::int64_t now = 500 * kMs;
    HeadPredictionAnchors anchors = runtime.Anchors(now, now + 15 * kMs, runtime.limit);
    Expect(!ComputeHeadExtension(anchors, {}).applied, "a display time inside the runtime's limit needs nothing");
    anchors.anchor = {0.0, 0.0, 0.0, 0.0};
    Expect(!ComputeHeadExtension(runtime.Anchors(now, now + 15 * kMs, runtime.limit), {}).applied &&
               !ComputeHeadExtension(anchors, {}).applied,
           "an invalid answer must leave the views alone");
}

void ProbeFindsTheLimit() {
    for (const std::int64_t limit : {std::int64_t{8 * kMs}, std::int64_t{20 * kMs}, std::int64_t{35 * kMs},
                                     std::int64_t{55 * kMs}}) {
        CappedRuntime runtime;
        runtime.limit = limit;
        runtime.turn.degrees_per_second = 120.0;
        runtime.turn.axis = {0.2, 1.0, 0.1};
        const auto estimate = EstimatePredictionLimit(runtime.Probe(700 * kMs));
        Expect(estimate.has_value(), "a turning head must give an estimate");
        Expect(!estimate->unlimited, "a capped runtime must not read as unlimited");
        Expect(std::abs(static_cast<double>(estimate->limit_ns - limit)) <= 0.5 * kMs,
               "probe must find the limit " + std::to_string(limit / kMs) + " ms, got " +
                   std::to_string(estimate->limit_ns / 1e6));
        Expect(std::abs(estimate->speed_degrees_per_second - 120.0) < 1.0, "probe must measure the speed");
    }
}

void ProbeReadsAFullPredictionAsUnlimited() {
    CappedRuntime runtime;
    runtime.limit = 1000 * kMs;
    const auto estimate = EstimatePredictionLimit(runtime.Probe(700 * kMs));
    Expect(estimate.has_value() && estimate->unlimited, "a runtime predicting every offset must read as unlimited");
}

void ProbeNeedsATurn() {
    CappedRuntime runtime;
    runtime.turn.degrees_per_second = 20.0;
    Expect(!EstimatePredictionLimit(runtime.Probe(700 * kMs)).has_value(), "a slow head must give no estimate");
    runtime.turn.degrees_per_second = 0.0;
    Expect(!EstimatePredictionLimit(runtime.Probe(700 * kMs)).has_value(), "a still head must give no estimate");
}

void ProbeToleratesNoise() {
    CappedRuntime runtime;
    std::vector<PredictionProbeSample> samples = runtime.Probe(700 * kMs);
    // About 0.02 degree of jitter, alternating.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].orientation =
            Multiply(AxisAngle({1.0, 0.3, 0.0}, (i % 2 == 0 ? 0.02 : -0.02)), samples[i].orientation);
    }
    const auto estimate = EstimatePredictionLimit(samples);
    Expect(estimate.has_value() && !estimate->unlimited &&
               std::abs(static_cast<double>(estimate->limit_ns - runtime.limit)) <= 1.5 * kMs,
           "jitter must not move the limit far");
}

void ProbeRejectsAnUnexplainedMotion() {
    CappedRuntime runtime;
    std::vector<PredictionProbeSample> samples = runtime.Probe(700 * kMs);
    // Answers that turn back and forth: no capped constant turn explains them.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].orientation = AxisAngle({0.0, 1.0, 0.0}, i % 2 == 0 ? 0.0 : 3.0);
    }
    Expect(!EstimatePredictionLimit(samples).has_value(), "an erratic probe must give no estimate");
}

PredictionLimitEstimate Limited(double ms) {
    PredictionLimitEstimate e;
    e.limit_ns = static_cast<std::int64_t>(ms * 1e6);
    e.speed_degrees_per_second = 100.0;
    e.fit = 0.99;
    return e;
}

void TrackerSettlesOnAgreement() {
    PredictionLimitTracker tracker;
    Expect(!tracker.Add(Limited(19.5)) && !tracker.Add(Limited(30.0)) && !tracker.Add(Limited(20.5)),
           "two agreeing probes must not settle the limit");
    Expect(tracker.Add(Limited(20.0)), "a third agreeing probe must settle it");
    Expect(tracker.Settled() && !tracker.Unlimited() && tracker.LimitNs() == 20'000'000,
           "the limit must be the agreeing probes' median");
    Expect(!tracker.Add(Limited(50.0)) && tracker.LimitNs() == 20'000'000, "a settled limit stays");
    tracker.Reset();
    Expect(!tracker.Settled() && tracker.Probes() == 0, "reset must forget the limit");
}

void TrackerSettlesUnlimited() {
    PredictionLimitTracker tracker;
    PredictionLimitEstimate unlimited = Limited(100.0);
    unlimited.unlimited = true;
    tracker.Add(unlimited);
    tracker.Add(Limited(20.0));
    tracker.Add(unlimited);
    Expect(tracker.Add(unlimited) && tracker.Unlimited(), "three unlimited probes must settle as unlimited");
}

void TrackerIgnoresScatter() {
    PredictionLimitTracker tracker;
    for (const double ms : {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0}) {
        Expect(!tracker.Add(Limited(ms)), "scattered probes must not settle a limit");
    }
}

struct Scenario {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main() {
    const std::vector<Scenario> scenarios = {
        {"reaches the head at the display time", ReachesTheHeadAtTheDisplayTime},
        {"damping leaves the expected share", DampingLeavesTheExpectedShare},
        {"turns the eyes about the head", TurnsEyesAboutTheHead},
        {"inverse locate matches", InverseLocateMatches},
        {"an anchor short of the limit costs nothing", AnAnchorShortOfTheLimitCostsNothing},
        {"a limit measured long never overshoots", ALimitMeasuredLongNeverOvershoots},
        {"gates slow heads", GatesSlowHeads},
        {"rejects glitches and caps the turn", RejectsGlitchesAndCapsTheTurn},
        {"nothing within the limit", NothingWithinTheLimit},
        {"probe finds the limit", ProbeFindsTheLimit},
        {"probe reads a full prediction as unlimited", ProbeReadsAFullPredictionAsUnlimited},
        {"probe needs a turn", ProbeNeedsATurn},
        {"probe tolerates noise", ProbeToleratesNoise},
        {"probe rejects an unexplained motion", ProbeRejectsAnUnexplainedMotion},
        {"tracker settles on agreement", TrackerSettlesOnAgreement},
        {"tracker settles unlimited", TrackerSettlesUnlimited},
        {"tracker ignores scatter", TrackerIgnoresScatter},
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
    std::cout << (scenarios.size() - failures) << "/" << scenarios.size() << " head prediction scenarios passed\n";
    return failures == 0 ? 0 : 1;
}
