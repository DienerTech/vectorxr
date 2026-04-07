#include "depthxr/effects.h"

#include <cmath>

namespace depthxr {
namespace {

bool IsNeutral(double amount) {
    return std::abs(amount) < 0.0001;
}

double ShiftAngle(double angle, double amount) {
    return std::atan(std::tan(angle) + amount);
}

} // namespace

void ApplyConvergence(std::span<ViewAdjustmentData> views, double amount) {
    if (views.size() < 2 || IsNeutral(amount)) {
        return;
    }

    // Shift the per-eye projection centers inward or outward while preserving
    // each eye's horizontal FoV span. Positive values move the zero-parallax
    // plane closer to the viewer.
    views[0].fov.angle_left = ShiftAngle(views[0].fov.angle_left, amount);
    views[0].fov.angle_right = ShiftAngle(views[0].fov.angle_right, amount);
    views[1].fov.angle_left = ShiftAngle(views[1].fov.angle_left, -amount);
    views[1].fov.angle_right = ShiftAngle(views[1].fov.angle_right, -amount);
}

} // namespace depthxr
