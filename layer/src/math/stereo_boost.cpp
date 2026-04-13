#include "depthxr/effects.h"

#include <cmath>
#include <optional>

namespace depthxr {
namespace {

bool IsNeutral(double factor) {
    return std::abs(factor - 1.0) < 0.0001;
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

ViewPose AveragePosition(std::span<ViewAdjustmentData> views, std::span<const size_t> indices) {
    ViewPose position;
    for (const size_t index : indices) {
        position.x += views[index].position.x;
        position.y += views[index].position.y;
        position.z += views[index].position.z;
    }

    const double scale = 1.0 / static_cast<double>(indices.size());
    position.x *= scale;
    position.y *= scale;
    position.z *= scale;
    return position;
}

void AssignPosition(std::span<ViewAdjustmentData> views, std::span<const size_t> indices, const ViewPose& position) {
    for (const size_t index : indices) {
        views[index].position = position;
    }
}

} // namespace

void ApplyStereoBoost(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout) {
    if (IsNeutral(factor)) {
        return;
    }

    const std::optional<EyeGroups> groups = ResolveEyeGroups(views, layout);
    if (!groups) {
        return;
    }

    const ViewPose left = AveragePosition(views, groups->left);
    const ViewPose right = AveragePosition(views, groups->right);
    const ViewPose midpoint{
        (left.x + right.x) * 0.5,
        (left.y + right.y) * 0.5,
        (left.z + right.z) * 0.5,
    };
    const ViewPose adjusted_left{
        midpoint.x + (left.x - midpoint.x) * factor,
        midpoint.y + (left.y - midpoint.y) * factor,
        midpoint.z + (left.z - midpoint.z) * factor,
    };
    const ViewPose adjusted_right{
        midpoint.x + (right.x - midpoint.x) * factor,
        midpoint.y + (right.y - midpoint.y) * factor,
        midpoint.z + (right.z - midpoint.z) * factor,
    };

    AssignPosition(views, groups->left, adjusted_left);
    AssignPosition(views, groups->right, adjusted_right);
}

} // namespace depthxr
