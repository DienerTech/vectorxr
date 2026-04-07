#include "depthxr/effects.h"

#include <cmath>

namespace depthxr {
namespace {

bool IsNeutral(double factor) {
    return std::abs(factor - 1.0) < 0.0001;
}

double ScaleAngle(double angle, double factor) {
    return std::atan(std::tan(angle) * factor);
}

} // namespace

void ApplyFovScale(std::span<ViewAdjustmentData> views, double factor) {
    if (views.empty() || IsNeutral(factor)) {
        return;
    }

    for (ViewAdjustmentData& view : views) {
        view.fov.angle_left = ScaleAngle(view.fov.angle_left, factor);
        view.fov.angle_right = ScaleAngle(view.fov.angle_right, factor);
        view.fov.angle_up = ScaleAngle(view.fov.angle_up, factor);
        view.fov.angle_down = ScaleAngle(view.fov.angle_down, factor);
    }
}

} // namespace depthxr
