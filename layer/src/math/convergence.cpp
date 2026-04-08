#include "depthxr/effects.h"

#include <cmath>
#include <optional>

namespace depthxr {
namespace {

bool IsNeutral(double amount) {
    return std::abs(amount) < 0.0001;
}

double ShiftAngle(double angle, double amount) {
    return std::atan(std::tan(angle) + amount);
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

void ShiftHorizontalProjection(std::span<ViewAdjustmentData> views, std::span<const size_t> indices, double amount) {
    for (const size_t index : indices) {
        views[index].fov.angle_left = ShiftAngle(views[index].fov.angle_left, amount);
        views[index].fov.angle_right = ShiftAngle(views[index].fov.angle_right, amount);
    }
}

} // namespace

void ApplyConvergence(std::span<ViewAdjustmentData> views, double amount, ViewLayout layout) {
    if (IsNeutral(amount)) {
        return;
    }

    const std::optional<EyeGroups> groups = ResolveEyeGroups(views, layout);
    if (!groups) {
        return;
    }

    // Shift the per-eye projection centers inward or outward while preserving
    // each eye's horizontal FoV span. Positive values move the zero-parallax
    // plane closer to the viewer.
    ShiftHorizontalProjection(views, groups->left, amount);
    ShiftHorizontalProjection(views, groups->right, -amount);
}

} // namespace depthxr
