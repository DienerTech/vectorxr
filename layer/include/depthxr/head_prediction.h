#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace depthxr {

// Head prediction past the runtime's limit (Frame Generation's
// extendHeadPrediction).
//
// Frame Generation tells a game to render for a display time well past the
// runtime's own, up to ~150 ms ahead. A runtime may predict the head only part
// of that way: PiOpenXR answers a locate with the head about 20 ms past the
// moment it is asked, whatever display time it is asked for. The rest is lag:
// the game renders where the head was, and the compositor rotates the image to
// where it is, which shows unrendered area at the edge the head turns toward.
//
// This carries the head's rotation over that remainder. Three answers from the
// runtime, asked at once, define it: two inside the runtime's limit (their
// difference is the runtime's own angular velocity) and the one for the
// display time the game asked for (the head the game's poses come from). The
// result rotates those poses about the head; it never moves the head itself.

struct HeadQuaternion {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double w{1.0};
};

struct HeadVector {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct HeadPredictionPose {
    HeadQuaternion orientation;
    HeadVector position;
};

struct HeadPredictionTuning {
    // Share of the remaining horizon the rotation is carried over. Short of 1:
    // the end of a turn is not seen coming, and the frames Frame Generation
    // shows before a real frame's display time carry that frame's orientation.
    double damping{0.9};
    // No extension at or below the low speed, all of it from the high one up.
    double gate_low_degrees_per_second{3.0};
    double gate_high_degrees_per_second{10.0};
    // The most the game's poses are ever rotated.
    double max_extension_degrees{10.0};
    // A faster head reads as a tracking glitch or a recenter: no extension.
    double max_speed_degrees_per_second{720.0};
    // The two velocity answers sit this far inside the runtime's limit, and
    // this far apart.
    std::int64_t anchor_margin_ns{2'000'000};
    std::int64_t velocity_span_ns{8'000'000};
};

// The runtime's answers for the head (VIEW space located in the game's world
// space), all asked at the same moment.
struct HeadPredictionAnchors {
    std::int64_t target{0};       // the display time the game asked for
    std::int64_t anchor_time{0};  // inside the runtime's limit
    HeadQuaternion before;        // the answer for anchor_time - velocity_span_ns
    HeadQuaternion anchor;        // the answer for anchor_time
    HeadPredictionPose at_target;           // the answer for target
};

struct HeadExtension {
    bool applied{false};
    HeadQuaternion rotation;  // in the world space, about `center`
    HeadVector center;        // the head, as the runtime answered for the target
    double degrees{0.0};
    double speed_degrees_per_second{0.0};
    std::int64_t horizon_ns{0};  // target - anchor_time
};

HeadExtension ComputeHeadExtension(const HeadPredictionAnchors& anchors, const HeadPredictionTuning& tuning);
// A head-attached pose (a view, or VIEW space) located in the world space.
HeadPredictionPose ApplyHeadExtension(const HeadExtension& extension, const HeadPredictionPose& pose);
// The world space located in a head-attached space: the inverse of the above.
HeadPredictionPose ApplyHeadExtensionInverse(const HeadExtension& extension, const HeadPredictionPose& pose);

// The runtime's limit, probed: the head located at once at each of these
// offsets past now. During a turn the answers move with the offset up to the
// limit and stop there.
std::span<const std::int64_t> PredictionProbeOffsets();

struct PredictionProbeSample {
    std::int64_t ahead_ns{0};
    HeadQuaternion orientation;
};

struct PredictionLimitEstimate {
    std::int64_t limit_ns{0};
    bool unlimited{false};  // the answers still moved at the last offset
    double speed_degrees_per_second{0.0};
    double fit{0.0};  // share of the motion's variance a capped constant turn explains
};

// Nothing for a probe without a clear turn (slower than ~40 deg/s, or answers
// that a capped constant turn does not explain).
std::optional<PredictionLimitEstimate> EstimatePredictionLimit(std::span<const PredictionProbeSample> samples);

// Settles the limit once enough probes agree: three of the last five within
// 3 ms of each other, or three of the last five unlimited.
class PredictionLimitTracker {
public:
    // True when this estimate settled the limit.
    bool Add(const PredictionLimitEstimate& estimate);
    void Reset();
    bool Settled() const { return settled_; }
    bool Unlimited() const { return settled_ && unlimited_; }
    std::int64_t LimitNs() const { return limit_ns_; }
    int Probes() const { return probes_; }

private:
    std::vector<PredictionLimitEstimate> recent_;
    int probes_{0};
    bool settled_{false};
    bool unlimited_{false};
    std::int64_t limit_ns_{0};
};

} // namespace depthxr
