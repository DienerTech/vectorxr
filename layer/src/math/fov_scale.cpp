#include "depthxr/effects.h"

#include <cmath>
#include <optional>

namespace depthxr {
namespace {

bool IsNeutral(double factor) {
    return std::abs(factor - 1.0) < 0.0001;
}

double ScaleAngle(double angle, double factor) {
    return std::atan(std::tan(angle) * factor);
}

struct EyeGroups {
    std::span<const size_t> left;
    std::span<const size_t> right;
};

std::optional<EyeGroups> ResolveEyeGroups(std::span<ViewAdjustmentData> views, ViewLayout layout) {
    static constexpr size_t kStereoLeft[] = {0};
    static constexpr size_t kStereoRight[] = {1};
    static constexpr size_t kQuadLeft[] = {0, 2};
    static constexpr size_t kQuadRight[] = {1, 3};

    if (views.size() < 2 || layout == ViewLayout::kMono) {
        return std::nullopt;
    }

    if (layout == ViewLayout::kStereoWithFoveatedInset) {
        if (views.size() < 4) {
            return std::nullopt;
        }
        return EyeGroups{std::span(kQuadLeft), std::span(kQuadRight)};
    }

    return EyeGroups{std::span(kStereoLeft), std::span(kStereoRight)};
}

void ScaleViewFov(ViewAdjustmentData& view, double factor) {
    view.fov.angle_left = ScaleAngle(view.fov.angle_left, factor);
    view.fov.angle_right = ScaleAngle(view.fov.angle_right, factor);
    view.fov.angle_up = ScaleAngle(view.fov.angle_up, factor);
    view.fov.angle_down = ScaleAngle(view.fov.angle_down, factor);
}

} // namespace

void ApplyFovScale(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout) {
    if (views.empty() || IsNeutral(factor)) {
        return;
    }

    const std::optional<EyeGroups> groups = ResolveEyeGroups(views, layout);
    if (!groups) {
        for (ViewAdjustmentData& view : views) {
            ScaleViewFov(view, factor);
        }
        return;
    }

    for (const size_t index : groups->left) {
        ScaleViewFov(views[index], factor);
    }
    for (const size_t index : groups->right) {
        ScaleViewFov(views[index], factor);
    }
}

} // namespace depthxr
