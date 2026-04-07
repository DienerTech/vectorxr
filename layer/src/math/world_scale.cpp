#include "depthxr/effects.h"

#include <cmath>

namespace depthxr {
namespace {

bool IsNeutral(double factor) {
    return std::abs(factor - 1.0) < 0.0001;
}

} // namespace

void ApplyWorldScale(std::span<ViewAdjustmentData> views, double factor) {
    if (views.empty() || IsNeutral(factor)) {
        return;
    }

    // This is the current experimental world-scale model: scale view translations
    // returned by xrLocateViews. It is intentionally isolated so a future
    // implementation can replace it with broader space interception if needed.
    for (ViewAdjustmentData& view : views) {
        view.position.x *= factor;
        view.position.y *= factor;
        view.position.z *= factor;
    }
}

} // namespace depthxr
