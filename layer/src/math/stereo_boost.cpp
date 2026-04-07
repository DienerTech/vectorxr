#include "depthxr/effects.h"

#include <cmath>

namespace depthxr {
namespace {

bool IsNeutral(double factor) {
    return std::abs(factor - 1.0) < 0.0001;
}

} // namespace

void ApplyStereoBoost(std::span<ViewAdjustmentData> views, double factor) {
    if (views.size() < 2 || IsNeutral(factor)) {
        return;
    }

    const double midpoint_x = (views[0].position.x + views[1].position.x) * 0.5;
    const double midpoint_y = (views[0].position.y + views[1].position.y) * 0.5;
    const double midpoint_z = (views[0].position.z + views[1].position.z) * 0.5;

    for (size_t i = 0; i < 2; ++i) {
        views[i].position.x = midpoint_x + (views[i].position.x - midpoint_x) * factor;
        views[i].position.y = midpoint_y + (views[i].position.y - midpoint_y) * factor;
        views[i].position.z = midpoint_z + (views[i].position.z - midpoint_z) * factor;
    }
}

} // namespace depthxr
