#include "depthxr/pivot_view.h"

#include <algorithm>
#include <cmath>

namespace depthxr {
namespace {

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

double ComputePivotContinuousExtra(double angle, double multiplier, double deadzone, double transition, double max_extra) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double x=std::max(0.0, std::abs(angle)-std::max(0.0,deadzone)*radians);
    const double width=std::max(0.0,transition)*radians;
    double distance=x;
    if (width>0) {
        if (x<width) {
            const double t=x/width;
            // Integral of smoothstep: sensitivity rises from 1 to multiplier,
            // without overshoot or a catch-up discontinuity at the far edge.
            distance=width*(t*t*t-0.5*t*t*t*t);
        } else distance=x-width*0.5;
    }
    double extra=distance*std::max(0.0,multiplier-1.0);
    if (max_extra>0) extra=std::min(extra,max_extra*radians);
    return std::copysign(extra,angle);
}
PivotViewOffset ComputePivotPositionAssist(double yaw, double pitch, const PivotPositionAssist& settings) {
    PivotViewOffset result;
    if (!settings.enabled) return result;
    auto add=[&](double angle, const PivotPositionDirection& d, double mirror) {
        const double degrees=std::abs(angle)*180.0/3.14159265358979323846;
        const double t=std::clamp((degrees-d.start_angle_degrees)/std::max(0.001,d.full_angle_degrees-d.start_angle_degrees),0.0,1.0);
        const double gain=t*t*(3-2*t)/100.0;
        result.right_meters+=d.right_cm*gain*mirror;
        result.up_meters+=d.up_cm*gain;
        result.forward_meters+=d.forward_cm*gain;
    };
    if (settings.advanced_directions) {
        add(yaw,yaw>=0?settings.yaw_left:settings.yaw_right,1);
        add(pitch,pitch>=0?settings.pitch_up:settings.pitch_down,1);
    } else add(yaw,settings.yaw,yaw>=0?-1:1);
    result.right_meters=std::clamp(result.right_meters,-1.0,1.0);
    result.up_meters=std::clamp(result.up_meters,-1.0,1.0);
    result.forward_meters=std::clamp(result.forward_meters,-1.0,1.0);
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
    if (!state.active || state.duration_seconds <= 0.0) {
        state.current = state.target;
        state.active = false;
    }
}

void UpdatePivotViewTransition(double delta_seconds, PivotViewTransitionState& state) {
    if (!state.active) return;
    state.elapsed_seconds += std::max(0.0, delta_seconds);
    const double linear = state.duration_seconds <= 0.0
                              ? 1.0
                              : std::clamp(state.elapsed_seconds / state.duration_seconds, 0.0, 1.0);
    const double eased = linear * linear * (3.0 - 2.0 * linear);
    state.current = InterpolateOffset(state.start, state.target, eased);
    if (linear >= 1.0) {
        state.current = state.target;
        state.active = false;
    }
}

} // namespace depthxr
