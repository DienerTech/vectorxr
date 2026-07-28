#pragma once

#include "depthxr/settings.h"

namespace depthxr {

struct PivotStepGlideState {
    double start_radians{0.0};
    double target_radians{0.0};
    double elapsed_seconds{0.0};
};

void UpdatePivotSteppedExtraAngleRadians(double current_angle_radians,
                                         const PivotStepTuning& positive,
                                         const PivotStepTuning& negative,
                                         PivotStepGlideMode glide_mode,
                                         double glide_seconds,
                                         double delta_seconds,
                                         int& current_step,
                                         double& displayed_extra_angle_radians,
                                         PivotStepGlideState& glide_state);

} // namespace depthxr
