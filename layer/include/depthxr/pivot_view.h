#pragma once

namespace depthxr {

struct PivotPositionAssist;

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

// Continuous extra rotation for one direction. Sensitivity rises from 1:1 to
// the full multiplier over `transition_degrees` beyond the deadzone, so a soft
// deadzone permanently gives up transition/2 x (multiplier - 1) of extra reach.
double ComputePivotContinuousExtra(double angle_radians,
                                   double rotation_multiplier,
                                   double deadzone_degrees,
                                   double transition_degrees,
                                   double max_extra_degrees);
// Position Assist offset (right/up/forward meters; angular fields are zero)
// for origin-relative head yaw and pitch.
PivotViewOffset ComputePivotPositionAssist(double yaw_radians,
                                           double pitch_radians,
                                           const PivotPositionAssist& settings);

bool PivotViewOffsetNearlyEqual(const PivotViewOffset& lhs,
                                const PivotViewOffset& rhs,
                                double epsilon = 1e-7);
bool PivotViewOffsetNearlyZero(const PivotViewOffset& offset, double epsilon = 1e-7);
double AdvancePivotActivationGain(double current_gain,
                                  bool engaged,
                                  bool suspended,
                                  double ramp_seconds,
                                  double delta_seconds);

// Retargeting never moves `current`: every visible change, including a
// zero-duration jump, lands on the next UpdatePivotViewTransition. Pivot only
// updates transitions on its xrLocateViews drive, so input handled during a
// frame cannot change the transform of a displayTime already returned.
void RetargetPivotViewTransition(const PivotViewOffset& target,
                                 double duration_seconds,
                                 PivotViewTransitionState& state);
void UpdatePivotViewTransition(double delta_seconds, PivotViewTransitionState& state);

} // namespace depthxr
