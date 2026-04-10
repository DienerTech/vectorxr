#pragma once

#include <span>

namespace depthxr {

enum class ViewLayout {
    kMono,
    kStereo,
    kStereoWithFoveatedInset,
};

struct ViewPose {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct ViewFov {
    double angle_left{0.0};
    double angle_right{0.0};
    double angle_up{0.0};
    double angle_down{0.0};
};

struct ViewOrientation {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double w{1.0};
};

struct ViewAdjustmentData {
    ViewPose position;
    ViewFov fov;
    ViewOrientation orientation;
};

void ApplyStereoBoost(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout);
void ApplyConvergence(std::span<ViewAdjustmentData> views, double amount, ViewLayout layout);
void ApplyPivotYaw(std::span<ViewAdjustmentData> views,
                   double rotation_multiplier,
                   double deadzone_degrees,
                   double smoothing,
                   ViewLayout layout,
                   double& smoothed_extra_yaw_radians);

} // namespace depthxr
