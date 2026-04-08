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

struct ViewAdjustmentData {
    ViewPose position;
    ViewFov fov;
};

void ApplyStereoBoost(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout);
void ApplyConvergence(std::span<ViewAdjustmentData> views, double amount, ViewLayout layout);
void ApplyWorldScale(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout);
void ApplyFovScale(std::span<ViewAdjustmentData> views, double factor, ViewLayout layout);

} // namespace depthxr
