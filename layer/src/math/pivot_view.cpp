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
