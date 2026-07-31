#include "depthxr/pivot_step.h"

#include <algorithm>
#include <cmath>

namespace depthxr {
namespace {

constexpr double kPi = 3.14159265358979323846;

double DegreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

double Clamp(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

double SmoothStep(double value) {
    const double t = Clamp(value, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

const PivotStepTuning& TuningForSign(int sign,
                                     const PivotStepTuning& positive,
                                     const PivotStepTuning& negative) {
    return sign >= 0 ? positive : negative;
}

} // namespace

void UpdatePivotSteppedExtraAngleRadians(double current_angle_radians,
                                         const PivotStepTuning& positive,
                                         const PivotStepTuning& negative,
                                         PivotStepGlideMode glide_mode,
                                         double glide_seconds,
                                         double delta_seconds,
                                         int& current_step,
                                         double& displayed_extra_angle_radians,
                                         PivotStepGlideState& glide_state) {
    while (current_step != 0) {
        const int sign = current_step > 0 ? 1 : -1;
        const PivotStepTuning& tuning = TuningForSign(sign, positive, negative);
        const double deadzone = DegreesToRadians(std::max(0.0, tuning.deadzone_degrees));
        const double trigger = DegreesToRadians(std::max(0.5, tuning.trigger_degrees));
        const double hysteresis = std::min(
            DegreesToRadians(std::max(0.0, tuning.hysteresis_degrees)), trigger * 0.9);
        const double release_threshold = deadzone + std::abs(current_step) * trigger - hysteresis;
        if (current_angle_radians * sign < release_threshold) {
            current_step -= sign;
        } else {
            break;
        }
    }

    const int input_sign = current_angle_radians >= 0.0 ? 1 : -1;
    const PivotStepTuning& input_tuning = TuningForSign(input_sign, positive, negative);
    const double input_deadzone = DegreesToRadians(std::max(0.0, input_tuning.deadzone_degrees));
    const double input_trigger = DegreesToRadians(std::max(0.5, input_tuning.trigger_degrees));
    if (current_step == 0 || (current_step > 0) == (input_sign > 0)) {
        while (current_angle_radians * input_sign >=
               input_deadzone + (std::abs(current_step) + 1) * input_trigger) {
            current_step += input_sign;
        }
    }

    const int target_sign = current_step >= 0 ? 1 : -1;
    const PivotStepTuning& target_tuning = TuningForSign(target_sign, positive, negative);
    double target = current_step * DegreesToRadians(std::max(0.0, target_tuning.amount_degrees));
    const double max_extra = DegreesToRadians(std::max(0.0, target_tuning.max_extra_degrees));
    if (max_extra > 0.0) {
        target = Clamp(target, -max_extra, max_extra);
    }

    const double duration = std::max(0.0, glide_seconds);
    if (glide_mode == PivotStepGlideMode::Instant || duration <= 0.0) {
        displayed_extra_angle_radians = target;
        glide_state = {target, target, duration};
        return;
    }

    if (std::abs(target - glide_state.target_radians) > 1e-9) {
        glide_state.start_radians = displayed_extra_angle_radians;
        glide_state.target_radians = target;
        glide_state.elapsed_seconds = 0.0;
    }

    glide_state.elapsed_seconds = std::min(duration,
                                            glide_state.elapsed_seconds + std::max(0.0, delta_seconds));
    const double progress = SmoothStep(glide_state.elapsed_seconds / duration);
    displayed_extra_angle_radians = glide_state.start_radians +
                                    (glide_state.target_radians - glide_state.start_radians) * progress;
    if (glide_state.elapsed_seconds >= duration) {
        displayed_extra_angle_radians = glide_state.target_radians;
    }
}

} // namespace depthxr
