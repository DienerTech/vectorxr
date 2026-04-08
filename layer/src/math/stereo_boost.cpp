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

double AverageX(std::span<ViewAdjustmentData> views, std::span<const size_t> indices) {
    double sum = 0.0;
    for (const size_t index : indices) {
        sum += views[index].position.x;
    }
    return sum / static_cast<double>(indices.size());
}

void AssignX(std::span<ViewAdjustmentData> views, std::span<const size_t> indices, double value) {
    for (const size_t index : indices) {
        views[index].position.x = value;
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

    const double left_x = AverageX(views, groups->left);
    const double right_x = AverageX(views, groups->right);
    const double midpoint_x = (left_x + right_x) * 0.5;
    const double adjusted_left_x = midpoint_x + (left_x - midpoint_x) * factor;
    const double adjusted_right_x = midpoint_x + (right_x - midpoint_x) * factor;

    AssignX(views, groups->left, adjusted_left_x);
    AssignX(views, groups->right, adjusted_right_x);
}

} // namespace depthxr
