#pragma once

#include <span>

namespace depthxr {

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

struct ViewAdjustmentData {
    ViewPose position;
    ViewFov fov;
};

void ApplyStereoBoost(std::span<ViewAdjustmentData> views, double factor);
void ApplyConvergence(std::span<ViewAdjustmentData> views, double amount);
void ApplyWorldScale(std::span<ViewAdjustmentData> views, double factor);
void ApplyFovScale(std::span<ViewAdjustmentData> views, double factor);

} // namespace depthxr
