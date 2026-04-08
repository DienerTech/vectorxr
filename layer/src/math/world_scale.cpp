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

ViewPose AveragePose(std::span<ViewAdjustmentData> views, std::span<const size_t> indices) {
    ViewPose average;
    for (const size_t index : indices) {
        average.x += views[index].position.x;
        average.y += views[index].position.y;
        average.z += views[index].position.z;
    }

    const double count = static_cast<double>(indices.size());
    average.x /= count;
    average.y /= count;
    average.z /= count;
    return average;
}

void AssignPose(std::span<ViewAdjustmentData> views, std::span<const size_t> indices, const ViewPose& pose) {
    for (const size_t index : indices) {
        views[index].position = pose;
    }
}

} // namespace

void ApplyWorldScale(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout) {
    if (views.empty() || IsNeutral(factor)) {
        return;
    }

    // This is the current experimental world-scale model: scale view translations
    // returned by xrLocateViews. It is intentionally isolated so a future
    // implementation can replace it with broader space interception if needed.
    const std::optional<EyeGroups> groups = ResolveEyeGroups(views, layout);
    if (!groups) {
        for (ViewAdjustmentData& view : views) {
            view.position.x *= factor;
            view.position.y *= factor;
            view.position.z *= factor;
        }
        return;
    }

    ViewPose left_pose = AveragePose(views, groups->left);
    ViewPose right_pose = AveragePose(views, groups->right);
    left_pose.x *= factor;
    left_pose.y *= factor;
    left_pose.z *= factor;
    right_pose.x *= factor;
    right_pose.y *= factor;
    right_pose.z *= factor;

    AssignPose(views, groups->left, left_pose);
    AssignPose(views, groups->right, right_pose);
}

} // namespace depthxr
