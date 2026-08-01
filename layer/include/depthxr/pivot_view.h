#pragma once

namespace depthxr {

struct PivotViewOffset {
    double yaw_radians{0.0};
    double pitch_radians{0.0};
    double right_meters{0.0};
    double up_meters{0.0};
    double forward_meters{0.0};
};

struct PivotViewTransitionState {
    PivotViewOffset start;
    PivotViewOffset current;
    PivotViewOffset target;
    double elapsed_seconds{0.0};
    double duration_seconds{0.0};
    bool active{false};
};

bool PivotViewOffsetNearlyEqual(const PivotViewOffset& lhs,
                                const PivotViewOffset& rhs,
                                double epsilon = 1e-7);
bool PivotViewOffsetNearlyZero(const PivotViewOffset& offset, double epsilon = 1e-7);

void RetargetPivotViewTransition(const PivotViewOffset& target,
                                 double duration_seconds,
                                 PivotViewTransitionState& state);
void UpdatePivotViewTransition(double delta_seconds, PivotViewTransitionState& state);

} // namespace depthxr
