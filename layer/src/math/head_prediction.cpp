#include "depthxr/head_prediction.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace depthxr {
namespace {

constexpr double kDegreesPerRadian = 57.29577951308232;

HeadQuaternion Multiply(const HeadQuaternion& a, const HeadQuaternion& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

HeadQuaternion Conjugate(const HeadQuaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

double Norm(const HeadQuaternion& q) {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

std::optional<HeadQuaternion> Normalized(const HeadQuaternion& q) {
    const double n = Norm(q);
    if (!std::isfinite(n) || n < 1e-6) {
        return std::nullopt;
    }
    return HeadQuaternion{q.x / n, q.y / n, q.z / n, q.w / n};
}

HeadVector Add(const HeadVector& a, const HeadVector& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

HeadVector Subtract(const HeadVector& a, const HeadVector& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

HeadVector Scale(const HeadVector& v, double k) {
    return {v.x * k, v.y * k, v.z * k};
}

double Dot(const HeadVector& a, const HeadVector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double Length(const HeadVector& v) {
    return std::sqrt(Dot(v, v));
}

HeadVector Rotate(const HeadQuaternion& q, const HeadVector& v) {
    const HeadQuaternion r = Multiply(Multiply(q, HeadQuaternion{v.x, v.y, v.z, 0.0}), Conjugate(q));
    return {r.x, r.y, r.z};
}

// Rotation vector (axis times angle, radians) of a unit quaternion, the
// shorter way round.
HeadVector Log(HeadQuaternion q) {
    if (q.w < 0.0) {
        q = {-q.x, -q.y, -q.z, -q.w};
    }
    const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (s < 1e-12) {
        return {2.0 * q.x, 2.0 * q.y, 2.0 * q.z};
    }
    const double k = 2.0 * std::atan2(s, q.w) / s;
    return {q.x * k, q.y * k, q.z * k};
}

HeadQuaternion Exp(const HeadVector& v) {
    const double angle = Length(v);
    if (angle < 1e-12) {
        return {v.x / 2.0, v.y / 2.0, v.z / 2.0, 1.0};
    }
    const double s = std::sin(angle / 2.0) / angle;
    return {v.x * s, v.y * s, v.z * s, std::cos(angle / 2.0)};
}

HeadPredictionPose Invert(const HeadPredictionPose& pose) {
    const HeadQuaternion inverse = Conjugate(pose.orientation);
    return {inverse, Scale(Rotate(inverse, pose.position), -1.0)};
}

bool Finite(const HeadVector& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

constexpr std::array<std::int64_t, 13> kProbeOffsets = {
    0, 5'000'000, 10'000'000, 15'000'000, 20'000'000, 25'000'000, 30'000'000,
    35'000'000, 40'000'000, 50'000'000, 60'000'000, 80'000'000, 100'000'000,
};

// A probe must show a clear turn: the runtime's answers barely move otherwise.
constexpr double kMinProbeSpeedDegreesPerSecond = 40.0;
constexpr double kMinProbeFit = 0.97;
constexpr std::int64_t kSettleAgreementNs = 3'000'000;
constexpr std::size_t kSettleWindow = 5;
constexpr std::size_t kSettleCount = 3;

} // namespace

HeadExtension ComputeHeadExtension(const HeadPredictionAnchors& anchors, const HeadPredictionTuning& tuning) {
    HeadExtension extension;
    extension.center = anchors.at_target.position;
    extension.horizon_ns = anchors.target - anchors.anchor_time;
    const auto before = Normalized(anchors.before);
    const auto anchor = Normalized(anchors.anchor);
    const auto at_target = Normalized(anchors.at_target.orientation);
    if (!before || !anchor || !at_target || !Finite(extension.center) || tuning.velocity_span_ns <= 0) {
        return extension;
    }
    // The runtime's own angular velocity, in the world space.
    const HeadVector velocity =
        Scale(Log(Multiply(*anchor, Conjugate(*before))), 1e9 / static_cast<double>(tuning.velocity_span_ns));
    extension.speed_degrees_per_second = Length(velocity) * kDegreesPerRadian;
    if (extension.horizon_ns <= 0 || !Finite(velocity) ||
        extension.speed_degrees_per_second > tuning.max_speed_degrees_per_second ||
        extension.speed_degrees_per_second <= tuning.gate_low_degrees_per_second) {
        return extension;
    }
    const double gate_span = tuning.gate_high_degrees_per_second - tuning.gate_low_degrees_per_second;
    const double gate = gate_span <= 0.0
        ? 1.0
        : std::clamp((extension.speed_degrees_per_second - tuning.gate_low_degrees_per_second) / gate_span, 0.0, 1.0);

    // The head at the target: the anchor answer turned on at the runtime's
    // velocity. The rotation from the runtime's answer for the target to it
    // also takes in whatever separates that answer from the anchor's, so an
    // anchor a little short of the runtime's true limit costs nothing.
    const HeadVector turn = Scale(velocity, tuning.damping * static_cast<double>(extension.horizon_ns) / 1e9);
    const HeadQuaternion predicted = Multiply(Exp(turn), *anchor);
    HeadVector rotation = Scale(Log(Multiply(predicted, Conjugate(*at_target))), gate);
    const double degrees = Length(rotation) * kDegreesPerRadian;
    if (!std::isfinite(degrees)) {
        return extension;
    }
    if (degrees > tuning.max_extension_degrees) {
        rotation = Scale(rotation, tuning.max_extension_degrees / degrees);
    }
    extension.rotation = Exp(rotation);
    extension.degrees = std::min(degrees, tuning.max_extension_degrees);
    extension.applied = true;
    return extension;
}

HeadPredictionPose ApplyHeadExtension(const HeadExtension& extension, const HeadPredictionPose& pose) {
    if (!extension.applied) {
        return pose;
    }
    HeadPredictionPose out;
    out.orientation = Normalized(Multiply(extension.rotation, pose.orientation)).value_or(pose.orientation);
    out.position = Add(extension.center, Rotate(extension.rotation, Subtract(pose.position, extension.center)));
    return out;
}

HeadPredictionPose ApplyHeadExtensionInverse(const HeadExtension& extension, const HeadPredictionPose& pose) {
    if (!extension.applied) {
        return pose;
    }
    return Invert(ApplyHeadExtension(extension, Invert(pose)));
}

std::span<const std::int64_t> PredictionProbeOffsets() {
    return kProbeOffsets;
}

std::optional<PredictionLimitEstimate> EstimatePredictionLimit(std::span<const PredictionProbeSample> samples) {
    if (samples.size() < 6) {
        return std::nullopt;
    }
    const auto first = Normalized(samples.front().orientation);
    if (!first) {
        return std::nullopt;
    }
    // How far each answer has turned from the first, along the turn.
    std::vector<HeadVector> turned;
    turned.reserve(samples.size());
    HeadVector axis;
    double longest = 0.0;
    for (const PredictionProbeSample& sample : samples) {
        const auto q = Normalized(sample.orientation);
        if (!q) {
            return std::nullopt;
        }
        turned.push_back(Log(Multiply(*q, Conjugate(*first))));
        const double length = Length(turned.back());
        if (length > longest) {
            longest = length;
            axis = Scale(turned.back(), 1.0 / length);
        }
    }
    if (longest <= 0.0) {
        return std::nullopt;
    }
    std::vector<double> x(samples.size());
    std::vector<double> d(samples.size());
    double total = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        x[i] = static_cast<double>(samples[i].ahead_ns - samples.front().ahead_ns) / 1e9;
        d[i] = Dot(turned[i], axis);
        total += d[i] * d[i];
    }
    // A constant turn up to the limit, none after: d = w * min(x, limit).
    // The limit with the least squared error over a 0.5 ms grid.
    const double last = x.back();
    double best_limit = last;
    double best_rate = 0.0;
    double best_error = -1.0;
    for (double limit = 0.0005; limit <= last + 1e-9; limit += 0.0005) {
        double mm = 0.0;
        double dm = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double m = std::min(x[i], limit);
            mm += m * m;
            dm += d[i] * m;
        }
        if (mm <= 0.0) {
            continue;
        }
        const double rate = dm / mm;
        double error = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double r = d[i] - rate * std::min(x[i], limit);
            error += r * r;
        }
        if (best_error < 0.0 || error < best_error) {
            best_error = error;
            best_limit = limit;
            best_rate = rate;
        }
    }
    if (best_error < 0.0 || total <= 0.0) {
        return std::nullopt;
    }
    PredictionLimitEstimate estimate;
    estimate.speed_degrees_per_second = best_rate * kDegreesPerRadian;
    estimate.fit = 1.0 - best_error / total;
    if (estimate.speed_degrees_per_second < kMinProbeSpeedDegreesPerSecond || estimate.fit < kMinProbeFit) {
        return std::nullopt;
    }
    estimate.limit_ns = samples.front().ahead_ns + static_cast<std::int64_t>(std::llround(best_limit * 1e9));
    // No knee before the last offset: the runtime predicts at least that far.
    estimate.unlimited = best_limit >= last - 0.0025;
    return estimate;
}

bool PredictionLimitTracker::Add(const PredictionLimitEstimate& estimate) {
    if (settled_) {
        return false;
    }
    ++probes_;
    recent_.push_back(estimate);
    if (recent_.size() > kSettleWindow) {
        recent_.erase(recent_.begin());
    }
    std::size_t unlimited = 0;
    std::vector<std::int64_t> limits;
    for (const PredictionLimitEstimate& e : recent_) {
        if (e.unlimited) {
            ++unlimited;
        } else {
            limits.push_back(e.limit_ns);
        }
    }
    if (unlimited >= kSettleCount) {
        settled_ = true;
        unlimited_ = true;
        limit_ns_ = 0;
        return true;
    }
    if (limits.size() < kSettleCount) {
        return false;
    }
    std::sort(limits.begin(), limits.end());
    const std::int64_t median = limits[limits.size() / 2];
    std::vector<std::int64_t> agreeing;
    for (const std::int64_t limit : limits) {
        if (limit >= median - kSettleAgreementNs && limit <= median + kSettleAgreementNs) {
            agreeing.push_back(limit);
        }
    }
    if (agreeing.size() < kSettleCount) {
        return false;
    }
    settled_ = true;
    unlimited_ = false;
    limit_ns_ = agreeing[agreeing.size() / 2];
    return true;
}

void PredictionLimitTracker::Reset() {
    recent_.clear();
    probes_ = 0;
    settled_ = false;
    unlimited_ = false;
    limit_ns_ = 0;
}

} // namespace depthxr
