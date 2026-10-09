#include "depthxr/pivot_view.h"

#include "depthxr/settings.h"

#include <algorithm>
#include <cmath>

namespace depthxr {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadiansPerDegree = kPi / 180.0;
constexpr double kMetersPerCentimeter = 0.01;
// Position Assist never moves the view more than one meter on any axis.
constexpr double kMaxPositionAssistMeters = 1.0;

double SmoothStep(double t) {
    return t * t * (3.0 - 2.0 * t);
}

double Interpolate(double start, double target, double t) {
    return start + (target - start) * t;
}

PivotViewOffset InterpolateOffset(const PivotViewOffset& start,
                                  const PivotViewOffset& target,
                                  double t) {
    return {
        Interpolate(start.yaw_radians, target.yaw_radians, t),
        Interpolate(start.pitch_radians, target.pitch_radians, t),
        Interpolate(start.right_meters, target.right_meters, t),
        Interpolate(start.up_meters, target.up_meters, t),
        Interpolate(start.forward_meters, target.forward_meters, t),
    };
}

} // namespace

double ComputePivotContinuousExtra(double angle_radians,
                                   double rotation_multiplier,
                                   double deadzone_degrees,
                                   double transition_degrees,
                                   double max_extra_degrees) {
    const double beyond_deadzone =
        std::max(0.0, std::abs(angle_radians) - std::max(0.0, deadzone_degrees) * kRadiansPerDegree);
    const double transition_width = std::max(0.0, transition_degrees) * kRadiansPerDegree;
    double amplified_distance = beyond_deadzone;
    if (transition_width > 0.0) {
        if (beyond_deadzone < transition_width) {
            // Integral of smoothstep: sensitivity rises from 1 to the
            // multiplier without overshoot or a catch-up step at the far edge.
            const double t = beyond_deadzone / transition_width;
            amplified_distance = transition_width * (t * t * t - 0.5 * t * t * t * t);
        } else {
            amplified_distance = beyond_deadzone - transition_width * 0.5;
        }
    }
    double extra = amplified_distance * std::max(0.0, rotation_multiplier - 1.0);
    if (max_extra_degrees > 0.0) {
        extra = std::min(extra, max_extra_degrees * kRadiansPerDegree);
    }
    return std::copysign(extra, angle_radians);
}

PivotViewOffset ComputePivotPositionAssist(double yaw_radians,
                                           double pitch_radians,
                                           const PivotPositionAssist& settings) {
    PivotViewOffset result;
    if (!settings.enabled) {
        return result;
    }
    constexpr double kMinimumRangeDegrees = 0.001;
    auto add_direction = [&](double angle_radians, const PivotPositionDirection& direction, double sideways_sign) {
        const double angle_degrees = std::abs(angle_radians) / kRadiansPerDegree;
        const double range_degrees =
            std::max(kMinimumRangeDegrees, direction.full_angle_degrees - direction.start_angle_degrees);
        const double progress =
            std::clamp((angle_degrees - direction.start_angle_degrees) / range_degrees, 0.0, 1.0);
        const double scale = SmoothStep(progress) * kMetersPerCentimeter;
        result.right_meters += direction.right_cm * scale * sideways_sign;
        result.up_meters += direction.up_cm * scale;
        result.forward_meters += direction.forward_cm * scale;
    };
    if (settings.advanced_directions) {
        add_direction(yaw_radians, yaw_radians >= 0.0 ? settings.yaw_left : settings.yaw_right, 1.0);
        add_direction(pitch_radians, pitch_radians >= 0.0 ? settings.pitch_up : settings.pitch_down, 1.0);
    } else {
        // Basic sideways lean is "toward the turn": positive yaw turns left,
        // so the configured rightward offset is mirrored to the left.
        add_direction(yaw_radians, settings.yaw, yaw_radians >= 0.0 ? -1.0 : 1.0);
    }
    result.right_meters = std::clamp(result.right_meters, -kMaxPositionAssistMeters, kMaxPositionAssistMeters);
    result.up_meters = std::clamp(result.up_meters, -kMaxPositionAssistMeters, kMaxPositionAssistMeters);
    result.forward_meters = std::clamp(result.forward_meters, -kMaxPositionAssistMeters, kMaxPositionAssistMeters);
    return result;
}

bool PivotViewOffsetNearlyEqual(const PivotViewOffset& lhs,
                                const PivotViewOffset& rhs,
                                double epsilon) {
    return std::abs(lhs.yaw_radians - rhs.yaw_radians) <= epsilon &&
           std::abs(lhs.pitch_radians - rhs.pitch_radians) <= epsilon &&
           std::abs(lhs.right_meters - rhs.right_meters) <= epsilon &&
           std::abs(lhs.up_meters - rhs.up_meters) <= epsilon &&
           std::abs(lhs.forward_meters - rhs.forward_meters) <= epsilon;
}

bool PivotViewOffsetNearlyZero(const PivotViewOffset& offset, double epsilon) {
    return PivotViewOffsetNearlyEqual(offset, {}, epsilon);
}

double AdvancePivotActivationGain(double current_gain,
                                  bool engaged,
                                  bool suspended,
                                  double ramp_seconds,
                                  double delta_seconds) {
    current_gain = std::clamp(current_gain, 0.0, 1.0);
    if (suspended) return current_gain;

    const double target_gain = engaged ? 1.0 : 0.0;
    if (ramp_seconds <= 0.0) return target_gain;

    const double step = std::max(0.0, delta_seconds) / ramp_seconds;
    if (target_gain > current_gain) {
        return std::min(target_gain, current_gain + step);
    }
    return std::max(target_gain, current_gain - step);
}

void RetargetPivotViewTransition(const PivotViewOffset& target,
                                 double duration_seconds,
                                 PivotViewTransitionState& state) {
    state.start = state.current;
    state.target = target;
    state.elapsed_seconds = 0.0;
    state.duration_seconds = std::max(0.0, duration_seconds);
    state.active = !PivotViewOffsetNearlyEqual(state.current, state.target);
    if (!state.active) {
        state.current = state.target;
    }
}

void UpdatePivotViewTransition(double delta_seconds, PivotViewTransitionState& state) {
    if (!state.active) return;
    state.elapsed_seconds += std::max(0.0, delta_seconds);
    const double linear = state.duration_seconds <= 0.0
                              ? 1.0
                              : std::clamp(state.elapsed_seconds / state.duration_seconds, 0.0, 1.0);
    const double eased = SmoothStep(linear);
    state.current = InterpolateOffset(state.start, state.target, eased);
    if (linear >= 1.0) {
        state.current = state.target;
        state.active = false;
    }
}

} // namespace depthxr
