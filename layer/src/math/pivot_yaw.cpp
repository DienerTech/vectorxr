#include "depthxr/effects.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace depthxr {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool NearlyZero(double value) {
    return std::abs(value) < 0.0001;
}

double DegreesToRadians(double degrees) {
    return degrees * kPi / 180.0;
}

double Clamp(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
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
    ViewPose pose;
    for (const size_t index : indices) {
        pose.x += views[index].position.x;
        pose.y += views[index].position.y;
        pose.z += views[index].position.z;
    }

    const double scale = 1.0 / static_cast<double>(indices.size());
    pose.x *= scale;
    pose.y *= scale;
    pose.z *= scale;
    return pose;
}

double ExtractYaw(const ViewOrientation& orientation) {
    return std::atan2(
        2.0 * (orientation.w * orientation.y + orientation.x * orientation.z),
        1.0 - 2.0 * (orientation.y * orientation.y + orientation.x * orientation.x));
}

ViewOrientation Multiply(const ViewOrientation& lhs, const ViewOrientation& rhs) {
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

ViewOrientation YawQuaternion(double yaw_radians) {
    const double half = yaw_radians * 0.5;
    return {0.0, std::sin(half), 0.0, std::cos(half)};
}

ViewPose RotateAroundY(const ViewPose& position, const ViewPose& center, double yaw_radians) {
    const double sin_yaw = std::sin(yaw_radians);
    const double cos_yaw = std::cos(yaw_radians);
    const double relative_x = position.x - center.x;
    const double relative_z = position.z - center.z;

    return {
        center.x + relative_x * cos_yaw + relative_z * sin_yaw,
        position.y,
        center.z - relative_x * sin_yaw + relative_z * cos_yaw,
    };
}

} // namespace

void ApplyPivotYaw(std::span<ViewAdjustmentData> views,
                   double rotation_multiplier,
                   double deadzone_degrees,
                   double smoothing,
                   ViewLayout layout,
                   double& smoothed_extra_yaw_radians) {
    if (views.empty() || rotation_multiplier <= 1.0) {
        smoothed_extra_yaw_radians = 0.0;
        return;
    }

    const std::optional<EyeGroups> groups = ResolveEyeGroups(views, layout);
    if (!groups) {
        smoothed_extra_yaw_radians = 0.0;
        return;
    }

    const ViewPose left_center = AveragePosition(views, groups->left);
    const ViewPose right_center = AveragePosition(views, groups->right);
    const ViewPose headset_center{
        (left_center.x + right_center.x) * 0.5,
        (left_center.y + right_center.y) * 0.5,
        (left_center.z + right_center.z) * 0.5,
    };

    const double current_yaw = ExtractYaw(views[groups->left.front()].orientation);
    const double deadzone_radians = DegreesToRadians(std::max(0.0, deadzone_degrees));
    const double abs_yaw = std::abs(current_yaw);

    double target_extra_yaw = 0.0;
    if (abs_yaw > deadzone_radians) {
        const double direction = current_yaw >= 0.0 ? 1.0 : -1.0;
        const double pivoted_yaw =
            direction * deadzone_radians + (current_yaw - direction * deadzone_radians) * rotation_multiplier;
        target_extra_yaw = pivoted_yaw - current_yaw;
    }

    const double blend = Clamp(1.0 - smoothing, 0.05, 1.0);
    smoothed_extra_yaw_radians += (target_extra_yaw - smoothed_extra_yaw_radians) * blend;

    if (NearlyZero(smoothed_extra_yaw_radians)) {
        return;
    }

    const ViewOrientation extra_rotation = YawQuaternion(smoothed_extra_yaw_radians);
    for (ViewAdjustmentData& view : views) {
        view.position = RotateAroundY(view.position, headset_center, smoothed_extra_yaw_radians);
        view.orientation = Multiply(extra_rotation, view.orientation);
    }
}

} // namespace depthxr
