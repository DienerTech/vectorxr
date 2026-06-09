#include "depthxr/openxr_layer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <openxr/openxr_platform.h>

#include "depthxr/config_path.h"
#include "depthxr/effects.h"
#include "depthxr/input_devices.h"
#include "depthxr/process_info.h"
#include "depthxr/seen_apps.h"

namespace depthxr {
namespace {

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

bool NearlyZero(double value) {
    return std::abs(value) < 0.0001;
}

double Clamp(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

double DegreesToRadians(double degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}

bool ExtensionRequested(const XrInstanceCreateInfo* create_info, std::string_view extension_name) {
    if (!create_info || !create_info->enabledExtensionNames) {
        return false;
    }

    for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
        const char* requested = create_info->enabledExtensionNames[i];
        if (requested && std::string_view(requested) == extension_name) {
            return true;
        }
    }
    return false;
}

void* FindMutableStructInChain(void* next, XrStructureType type) {
    auto* header = reinterpret_cast<XrBaseOutStructure*>(next);
    while (header) {
        if (header->type == type) {
            return header;
        }
        header = header->next;
    }
    return nullptr;
}

uint32_t ScaleDimension(uint32_t value, double scale, uint32_t max_value) {
    const double scaled = std::max(1.0, std::round(static_cast<double>(value) * std::max(0.01, scale)));
    const uint32_t rounded = static_cast<uint32_t>(std::min<double>(scaled, std::max<uint32_t>(1, max_value)));
    return std::max<uint32_t>(1, rounded);
}

void SetFoveatedViewActive(XrViewConfigurationView& view, XrBool32 active) {
    void* foveated = FindMutableStructInChain(view.next, XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO);
    if (!foveated) {
        return;
    }
    reinterpret_cast<XrFoveatedViewConfigurationViewVARJO*>(foveated)->foveatedRenderingActive = active;
}

struct AngularWindow {
    double negative;
    double positive;
};

AngularWindow ClampAngularWindow(float base_negative,
                                 float base_positive,
                                 double center_radians,
                                 double size_radians) {
    const double half = std::max(0.001, size_radians * 0.5);
    double negative = center_radians - half;
    double positive = center_radians + half;

    const double base_span = static_cast<double>(base_positive) - static_cast<double>(base_negative);
    if (positive - negative >= base_span) {
        negative = base_negative;
        positive = base_positive;
    } else {
        if (negative < base_negative) {
            positive += base_negative - negative;
            negative = base_negative;
        }
        if (positive > base_positive) {
            negative -= positive - base_positive;
            positive = base_positive;
        }
        negative = Clamp(negative, base_negative, base_positive);
        positive = Clamp(positive, base_negative, base_positive);
    }

    return {negative, positive};
}

XrFovf BuildFocusFov(const XrFovf& base_fov,
                     const QuadViewsResolvedSettings& settings,
                     double focus_yaw_radians,
                     double focus_pitch_radians) {
    const double horizontal_center = DegreesToRadians(settings.horizontal_offset_degrees) + focus_yaw_radians;
    const double vertical_center = DegreesToRadians(settings.vertical_offset_degrees) + focus_pitch_radians;
    const double horizontal_size = DegreesToRadians(settings.focus_horizontal_fov_degrees);
    const double vertical_size = DegreesToRadians(settings.focus_vertical_fov_degrees);

    const AngularWindow horizontal =
        ClampAngularWindow(base_fov.angleLeft, base_fov.angleRight, horizontal_center, horizontal_size);
    const AngularWindow vertical =
        ClampAngularWindow(base_fov.angleDown, base_fov.angleUp, vertical_center, vertical_size);

    return {
        static_cast<float>(horizontal.negative),
        static_cast<float>(horizontal.positive),
        static_cast<float>(vertical.positive),
        static_cast<float>(vertical.negative),
    };
}

template <typename T>
void SafeRelease(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

const void* FindStructInChain(const void* next, XrStructureType type) {
    const auto* header = reinterpret_cast<const XrBaseInStructure*>(next);
    while (header) {
        if (header->type == type) {
            return header;
        }
        header = header->next;
    }
    return nullptr;
}

bool IsD3D11SwapchainImage(const XrSwapchainImageBaseHeader* image) {
    return image && image->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
}

struct FocusRectConstants {
    float focus_rect[4];
    float feather[2];
    float padding[2];
};

FocusRectConstants BuildFocusRectConstants(const XrFovf& full_fov,
                                           const XrFovf& focus_fov,
                                           uint32_t width,
                                           uint32_t height) {
    const double full_left = std::tan(full_fov.angleLeft);
    const double full_right = std::tan(full_fov.angleRight);
    const double full_down = std::tan(full_fov.angleDown);
    const double full_up = std::tan(full_fov.angleUp);
    const double focus_left = std::tan(focus_fov.angleLeft);
    const double focus_right = std::tan(focus_fov.angleRight);
    const double focus_down = std::tan(focus_fov.angleDown);
    const double focus_up = std::tan(focus_fov.angleUp);
    const double full_width = std::max(0.0001, full_right - full_left);
    const double full_height = std::max(0.0001, full_up - full_down);

    FocusRectConstants constants{};
    constants.focus_rect[0] = static_cast<float>(Clamp((focus_left - full_left) / full_width, 0.0, 1.0));
    constants.focus_rect[2] = static_cast<float>(Clamp((focus_right - full_left) / full_width, 0.0, 1.0));
    constants.focus_rect[1] = static_cast<float>(Clamp((full_up - focus_up) / full_height, 0.0, 1.0));
    constants.focus_rect[3] = static_cast<float>(Clamp((full_up - focus_down) / full_height, 0.0, 1.0));

    constexpr double kFeatherPixels = 28.0;
    constants.feather[0] = static_cast<float>(kFeatherPixels / std::max<uint32_t>(1, width));
    constants.feather[1] = static_cast<float>(kFeatherPixels / std::max<uint32_t>(1, height));
    return constants;
}

const char* D3D11QuadViewsShaderSource() {
    return R"(
cbuffer QuadViewsConstants : register(b0) {
    float4 focusRect;
    float2 feather;
    float2 padding;
};

Texture2D peripheralTexture : register(t0);
Texture2D focusTexture : register(t1);
SamplerState linearSampler : register(s0);

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

float4 PSMain(VSOut input) : SV_Target {
    float2 uv = saturate(input.uv);
    float4 peripheral = peripheralTexture.Sample(linearSampler, uv);

    float2 inside = step(focusRect.xy, uv) * step(uv, focusRect.zw);
    float inRect = inside.x * inside.y;
    if (inRect < 0.5) {
        return peripheral;
    }

    float2 focusUv = saturate((uv - focusRect.xy) / max(focusRect.zw - focusRect.xy, float2(0.0001, 0.0001)));
    float4 focus = focusTexture.Sample(linearSampler, focusUv);

    float2 distToEdge = min(uv - focusRect.xy, focusRect.zw - uv);
    float edgeAlpha = saturate(min(distToEdge.x / max(feather.x, 0.0001), distToEdge.y / max(feather.y, 0.0001)));
    edgeAlpha = edgeAlpha * edgeAlpha * (3.0 - 2.0 * edgeAlpha);
    return lerp(peripheral, focus, edgeAlpha);
}
)";
}

std::string FormatDiagnosticDouble(double value) {
    std::ostringstream stream;
    if (value != 0.0 && std::abs(value) < 0.0001) {
        stream << std::scientific << std::setprecision(6) << value;
    } else {
        stream << std::fixed << std::setprecision(6) << value;
    }
    return stream.str();
}

double HorizontalProjectionCenter(const ViewFov& fov) {
    return (std::tan(fov.angle_left) + std::tan(fov.angle_right)) * 0.5;
}

double ExtractYawRadians(const ViewOrientation& orientation) {
    return std::atan2(
        2.0 * (orientation.w * orientation.y + orientation.x * orientation.z),
        1.0 - 2.0 * (orientation.y * orientation.y + orientation.x * orientation.x));
}

double ExtractPitchRadians(const ViewOrientation& orientation) {
    const double sin_pitch =
        Clamp(2.0 * (orientation.w * orientation.x - orientation.z * orientation.y), -1.0, 1.0);
    return std::asin(sin_pitch);
}

XrQuaternionf MultiplyQuaternion(const XrQuaternionf& lhs, const XrQuaternionf& rhs) {
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

XrQuaternionf ConjugateQuaternion(const XrQuaternionf& quaternion) {
    return {-quaternion.x, -quaternion.y, -quaternion.z, quaternion.w};
}

XrQuaternionf NormalizeQuaternion(const XrQuaternionf& quaternion) {
    const double magnitude = std::sqrt(static_cast<double>(quaternion.x) * quaternion.x +
                                       static_cast<double>(quaternion.y) * quaternion.y +
                                       static_cast<double>(quaternion.z) * quaternion.z +
                                       static_cast<double>(quaternion.w) * quaternion.w);
    if (magnitude < 0.000001) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float scale = static_cast<float>(1.0 / magnitude);
    return {quaternion.x * scale, quaternion.y * scale, quaternion.z * scale, quaternion.w * scale};
}

XrVector3f RotateVector(const XrQuaternionf& rotation, const XrVector3f& vector) {
    const XrQuaternionf pure_vector{vector.x, vector.y, vector.z, 0.0f};
    const XrQuaternionf rotated =
        MultiplyQuaternion(MultiplyQuaternion(rotation, pure_vector), ConjugateQuaternion(rotation));
    return {rotated.x, rotated.y, rotated.z};
}

XrQuaternionf YawQuaternion(float yaw_radians) {
    const float half = yaw_radians * 0.5f;
    return {0.0f, std::sin(half), 0.0f, std::cos(half)};
}

XrQuaternionf PitchQuaternion(float pitch_radians) {
    const float half = pitch_radians * 0.5f;
    return {std::sin(half), 0.0f, 0.0f, std::cos(half)};
}

XrPosef MultiplyPoses(const XrPosef& local_pose, const XrPosef& parent_pose) {
    const XrQuaternionf parent_orientation = NormalizeQuaternion(parent_pose.orientation);
    return {
        NormalizeQuaternion(MultiplyQuaternion(parent_orientation, local_pose.orientation)),
        {
            RotateVector(parent_orientation, local_pose.position).x + parent_pose.position.x,
            RotateVector(parent_orientation, local_pose.position).y + parent_pose.position.y,
            RotateVector(parent_orientation, local_pose.position).z + parent_pose.position.z,
        },
    };
}

XrPosef InvertPose(const XrPosef& pose) {
    const XrQuaternionf inverse_orientation = NormalizeQuaternion(ConjugateQuaternion(pose.orientation));
    const XrVector3f inverse_position =
        RotateVector(inverse_orientation, {-pose.position.x, -pose.position.y, -pose.position.z});
    return {inverse_orientation, inverse_position};
}

XrPosef ApplyExtraRotationToPose(const XrPosef& pose, float extra_yaw_radians, float extra_pitch_radians) {
    if (NearlyZero(extra_yaw_radians) && NearlyZero(extra_pitch_radians)) {
        return pose;
    }

    const XrQuaternionf extra_rotation = NormalizeQuaternion(
        MultiplyQuaternion(YawQuaternion(extra_yaw_radians), PitchQuaternion(extra_pitch_radians)));
    return {
        NormalizeQuaternion(MultiplyQuaternion(extra_rotation, pose.orientation)),
        pose.position,
    };
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

void CopyName(char* destination, size_t capacity, std::string_view value) {
    if (!destination || capacity == 0) {
        return;
    }

    const size_t copy_count = std::min(capacity - 1, value.size());
    std::memcpy(destination, value.data(), copy_count);
    destination[copy_count] = '\0';
}

ViewOrientation ApplyExtraRotationToOrientation(const ViewOrientation& orientation,
                                                double extra_yaw_radians,
                                                double extra_pitch_radians) {
    const XrPosef pose{
        {static_cast<float>(orientation.x),
         static_cast<float>(orientation.y),
         static_cast<float>(orientation.z),
         static_cast<float>(orientation.w)},
        {0.0f, 0.0f, 0.0f},
    };
    const XrPosef rotated_pose =
        ApplyExtraRotationToPose(pose,
                                 static_cast<float>(extra_yaw_radians),
                                 static_cast<float>(extra_pitch_radians));
    return {
        rotated_pose.orientation.x,
        rotated_pose.orientation.y,
        rotated_pose.orientation.z,
        rotated_pose.orientation.w,
    };
}

double ComputeTimeBasedBlend(double smoothing, double delta_seconds) {
    const double clamped_smoothing = Clamp(smoothing, 0.0, 0.99);
    const double fallback_blend = Clamp(1.0 - clamped_smoothing, 0.05, 1.0);
    if (delta_seconds <= 0.0) {
        return fallback_blend;
    }

    constexpr double kReferenceFrameSeconds = 1.0 / 90.0;
    const double frame_scale = std::max(delta_seconds / kReferenceFrameSeconds, 0.0);
    return Clamp(1.0 - std::pow(clamped_smoothing, frame_scale), 0.05, 1.0);
}

double ComputePivotExtraAngleRadians(double current_angle_radians,
                                     double rotation_multiplier,
                                     double deadzone_degrees,
                                     double max_extra_degrees,
                                     double smoothing,
                                     double delta_seconds,
                                     double& smoothed_extra_angle_radians,
                                     bool subtract_deadzone_from_gain = true) {
    if (rotation_multiplier <= 1.0) {
        smoothed_extra_angle_radians = 0.0;
        return 0.0;
    }

    const double deadzone_radians = DegreesToRadians(std::max(0.0, deadzone_degrees));
    const double max_extra_radians = DegreesToRadians(std::max(0.0, max_extra_degrees));
    const double abs_angle = std::abs(current_angle_radians);

    double target_extra_angle = 0.0;
    if (abs_angle <= deadzone_radians * 0.5) {
        smoothed_extra_angle_radians = 0.0;
        return 0.0;
    }

    if (abs_angle > deadzone_radians) {
        if (subtract_deadzone_from_gain) {
            const double direction = current_angle_radians >= 0.0 ? 1.0 : -1.0;
            const double pivoted_angle = direction * deadzone_radians +
                                         (current_angle_radians - direction * deadzone_radians) * rotation_multiplier;
            target_extra_angle = pivoted_angle - current_angle_radians;
        } else {
            target_extra_angle = current_angle_radians * (rotation_multiplier - 1.0);
        }
    }

    if (max_extra_radians > 0.0) {
        target_extra_angle = Clamp(target_extra_angle, -max_extra_radians, max_extra_radians);
    }

    const double blend = ComputeTimeBasedBlend(smoothing, delta_seconds);
    smoothed_extra_angle_radians += (target_extra_angle - smoothed_extra_angle_radians) * blend;
    if (NearlyZero(smoothed_extra_angle_radians)) {
        smoothed_extra_angle_radians = 0.0;
    }
    return smoothed_extra_angle_radians;
}

const char* ToString(XrViewConfigurationType type) {
    switch (type) {
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO:
        return "primary_mono";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO:
        return "primary_stereo";
    case XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET:
        return "primary_stereo_with_foveated_inset";
    default:
        return "unknown";
    }
}

std::string FormatViewConfigurationTypes(std::span<const XrViewConfigurationType> types) {
    std::ostringstream stream;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i > 0) {
            stream << ",";
        }
        stream << ToString(types[i]);
    }
    return stream.str();
}

std::string FormatHandle(XrSwapchain swapchain) {
    std::uint64_t value = 0;
    static_assert(sizeof(swapchain) <= sizeof(value));
    std::memcpy(&value, &swapchain, sizeof(swapchain));

    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string FormatUsageFlags(XrSwapchainUsageFlags flags) {
    std::vector<std::string_view> names;
    if ((flags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
        names.push_back("color");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        names.push_back("depth");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0) {
        names.push_back("unorderedAccess");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) != 0) {
        names.push_back("transferSrc");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0) {
        names.push_back("transferDst");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0) {
        names.push_back("sampled");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0) {
        names.push_back("mutableFormat");
    }
    if ((flags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) != 0) {
        names.push_back("inputAttachment");
    }

    if (names.empty()) {
        return "none";
    }

    std::ostringstream stream;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            stream << "|";
        }
        stream << names[i];
    }
    return stream.str();
}

ViewLayout DetermineViewLayout(XrViewConfigurationType type, uint32_t count) {
    if (type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET) {
        return count >= 4 ? ViewLayout::kStereoWithFoveatedInset : ViewLayout::kMono;
    }

    if (count >= 2) {
        return ViewLayout::kStereo;
    }

    return ViewLayout::kMono;
}

bool IsQuadViewConfiguration(XrViewConfigurationType type) {
    return type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
}

constexpr XrTime kMaxPivotPoseDeltaMatchWindow = 5'000'000;

bool SameSettings(const ResolvedRuntimeConfig& lhs, const ResolvedRuntimeConfig& rhs) {
    return lhs.core.enabled == rhs.core.enabled &&
           lhs.core.log_level == rhs.core.log_level &&
           lhs.core.log_retention_files == rhs.core.log_retention_files &&
           lhs.core.track_seen_apps == rhs.core.track_seen_apps &&
           lhs.depthxr.enabled == rhs.depthxr.enabled &&
           lhs.depthxr.stereo_boost_enabled == rhs.depthxr.stereo_boost_enabled &&
           lhs.depthxr.convergence_enabled == rhs.depthxr.convergence_enabled &&
           NearlyEqual(lhs.depthxr.stereo_boost, rhs.depthxr.stereo_boost) &&
           NearlyEqual(lhs.depthxr.convergence, rhs.depthxr.convergence) &&
           lhs.depthxr_bindings.toggle_enabled.type == rhs.depthxr_bindings.toggle_enabled.type &&
           lhs.depthxr_bindings.toggle_enabled.chord == rhs.depthxr_bindings.toggle_enabled.chord &&
           lhs.depthxr_bindings.toggle_enabled.device_guid == rhs.depthxr_bindings.toggle_enabled.device_guid &&
           lhs.depthxr_bindings.toggle_enabled.input_path == rhs.depthxr_bindings.toggle_enabled.input_path &&
           lhs.depthxr_bindings.toggle_enabled.product_guid == rhs.depthxr_bindings.toggle_enabled.product_guid &&
           lhs.depthxr_bindings.toggle_enabled.device_name == rhs.depthxr_bindings.toggle_enabled.device_name &&
           lhs.depthxr_bindings.toggle_enabled.input_label == rhs.depthxr_bindings.toggle_enabled.input_label &&
           lhs.pivotxr.enabled == rhs.pivotxr.enabled &&
           lhs.pivotxr.activation_mode == rhs.pivotxr.activation_mode &&
           lhs.pivotxr.activation_binding.type == rhs.pivotxr.activation_binding.type &&
           lhs.pivotxr.activation_binding.chord == rhs.pivotxr.activation_binding.chord &&
           lhs.pivotxr.activation_binding.device_guid == rhs.pivotxr.activation_binding.device_guid &&
           lhs.pivotxr.activation_binding.input_path == rhs.pivotxr.activation_binding.input_path &&
           lhs.pivotxr.activation_binding.product_guid == rhs.pivotxr.activation_binding.product_guid &&
           lhs.pivotxr.activation_binding.device_name == rhs.pivotxr.activation_binding.device_name &&
           lhs.pivotxr.activation_binding.input_label == rhs.pivotxr.activation_binding.input_label &&
           NearlyEqual(lhs.pivotxr.yaw_rotation_multiplier, rhs.pivotxr.yaw_rotation_multiplier) &&
           NearlyEqual(lhs.pivotxr.yaw_smoothing, rhs.pivotxr.yaw_smoothing) &&
           NearlyEqual(lhs.pivotxr.yaw_deadzone_degrees, rhs.pivotxr.yaw_deadzone_degrees) &&
           NearlyEqual(lhs.pivotxr.yaw_max_extra_degrees, rhs.pivotxr.yaw_max_extra_degrees) &&
           NearlyEqual(lhs.pivotxr.pitch_rotation_multiplier, rhs.pivotxr.pitch_rotation_multiplier) &&
           NearlyEqual(lhs.pivotxr.pitch_smoothing, rhs.pivotxr.pitch_smoothing) &&
           NearlyEqual(lhs.pivotxr.pitch_deadzone_degrees, rhs.pivotxr.pitch_deadzone_degrees) &&
           NearlyEqual(lhs.pivotxr.pitch_max_extra_degrees, rhs.pivotxr.pitch_max_extra_degrees) &&
           lhs.quadviews.enabled == rhs.quadviews.enabled &&
           lhs.quadviews.prefer_eye_tracking == rhs.quadviews.prefer_eye_tracking &&
           lhs.quadviews.tracking_mode == rhs.quadviews.tracking_mode &&
           NearlyEqual(lhs.quadviews.focus_horizontal_fov_degrees, rhs.quadviews.focus_horizontal_fov_degrees) &&
           NearlyEqual(lhs.quadviews.focus_vertical_fov_degrees, rhs.quadviews.focus_vertical_fov_degrees) &&
           NearlyEqual(lhs.quadviews.focus_scale, rhs.quadviews.focus_scale) &&
           NearlyEqual(lhs.quadviews.peripheral_scale, rhs.quadviews.peripheral_scale) &&
           NearlyEqual(lhs.quadviews.horizontal_offset_degrees, rhs.quadviews.horizontal_offset_degrees) &&
           NearlyEqual(lhs.quadviews.vertical_offset_degrees, rhs.quadviews.vertical_offset_degrees) &&
           NearlyEqual(lhs.quadviews.gaze_smoothing, rhs.quadviews.gaze_smoothing) &&
           NearlyEqual(lhs.quadviews.gaze_deadzone_degrees, rhs.quadviews.gaze_deadzone_degrees);
}

bool SameInputBinding(const InputBinding& lhs, const InputBinding& rhs) {
    return lhs.type == rhs.type &&
           lhs.chord == rhs.chord &&
           lhs.device_guid == rhs.device_guid &&
           lhs.input_path == rhs.input_path &&
           lhs.product_guid == rhs.product_guid &&
           lhs.device_name == rhs.device_name &&
           lhs.input_label == rhs.input_label;
}

bool SamePivotActivationBinding(const PivotXrResolvedSettings& lhs, const PivotXrResolvedSettings& rhs) {
    return lhs.enabled == rhs.enabled && lhs.activation_mode == rhs.activation_mode &&
           SameInputBinding(lhs.activation_binding, rhs.activation_binding);
}

std::string BindingLabel(const InputBinding& binding) {
    if (binding.type == InputBindingType::Device) {
        const std::string device = binding.device_name.empty() ? binding.device_guid : binding.device_name;
        const std::string input = binding.input_label.empty() ? binding.input_path : binding.input_label;
        return device + "/" + input;
    }

    std::ostringstream stream;
    for (size_t index = 0; index < binding.chord.size(); ++index) {
        if (index > 0) {
            stream << "+";
        }
        stream << binding.chord[index];
    }
    return stream.str();
}

ConfigDocument DefaultConfig() {
    ConfigDocument document;
    document.version = 3;
    return document;
}

void AppendViewSummary(std::ostringstream& stream, std::span<const ViewAdjustmentData> views) {
    const size_t summary_count = std::min<size_t>(views.size(), 4);
    for (size_t i = 0; i < summary_count; ++i) {
        stream << " view" << i << "Pos=(" << FormatDiagnosticDouble(views[i].position.x) << ", "
               << FormatDiagnosticDouble(views[i].position.y) << ", "
               << FormatDiagnosticDouble(views[i].position.z) << ")"
               << " view" << i << "Fov=(" << FormatDiagnosticDouble(views[i].fov.angle_left) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_right) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_up) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_down) << ")"
               << " view" << i << "ProjCenter=" << FormatDiagnosticDouble(HorizontalProjectionCenter(views[i].fov));
    }
}

} // namespace

OpenXrLayer& OpenXrLayer::Instance() {
    static OpenXrLayer layer;
    return layer;
}

void OpenXrLayer::SetLayerDirectory(std::filesystem::path dll_directory) {
    std::scoped_lock lock(mutex_);
    dll_directory_ = std::move(dll_directory);
}

void OpenXrLayer::SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr) {
    std::scoped_lock lock(mutex_);
    next_get_instance_proc_addr_ = next_get_instance_proc_addr;
}

XrResult OpenXrLayer::OnInstanceCreated(const XrInstanceCreateInfo* create_info,
                                        XrInstance instance,
                                        bool eye_gaze_extension_enabled) {
    std::scoped_lock lock(mutex_);

    instance_ = instance;
    ResetInstanceState();
    eye_gaze_extension_enabled_ = eye_gaze_extension_enabled;
    ResetSessionState();
    config_path_ = ResolveConfigPath();
    log_path_ = ResolveLogPath();
    logger_.Initialize(log_path_);

    current_exe_name_ = GetCurrentExecutableName();
    logger_.Info("VectorXR attached to process: " + current_exe_name_);

    if (create_info) {
        quad_views_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
        varjo_foveated_rendering_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);

        std::ostringstream stream;
        stream << "Application=" << create_info->applicationInfo.applicationName << ", Engine="
               << create_info->applicationInfo.engineName;
        logger_.Info(stream.str());
        if (quad_views_extension_requested_ || varjo_foveated_rendering_extension_requested_) {
            logger_.Info(std::string("Application requested layer-owned extensions: quadViews=") +
                         (quad_views_extension_requested_ ? "true" : "false") +
                         ", varjoFoveatedRendering=" +
                         (varjo_foveated_rendering_extension_requested_ ? "true" : "false"));
        }
    }
    if (eye_gaze_extension_enabled_) {
        logger_.Info("Enabled XR_EXT_eye_gaze_interaction downstream for VectorXR quadviews.");
    }

    CaptureInstanceFunctions();
    if (!next_destroy_instance_ || !next_create_session_ || !next_destroy_session_ || !next_begin_session_ ||
        !next_attach_session_action_sets_ || !next_sync_actions_ ||
        !next_end_frame_ || !next_get_system_properties_ || !next_enumerate_environment_blend_modes_ ||
        !next_enumerate_view_configurations_ || !next_get_view_configuration_properties_ ||
        !next_enumerate_view_configuration_views_ ||
        !next_create_swapchain_ || !next_destroy_swapchain_ || !next_enumerate_swapchain_images_ ||
        !next_acquire_swapchain_image_ || !next_wait_swapchain_image_ || !next_release_swapchain_image_ ||
        !next_create_reference_space_ || !next_destroy_space_ ||
        !next_locate_space_ || !next_locate_views_) {
        logger_.Error("Failed to resolve required OpenXR function pointers");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    ReloadConfigIfNeeded();
    if (config_.core.track_seen_apps) {
        std::string seen_apps_error;
        if (!RecordSeenApp(current_exe_name_, &seen_apps_error)) {
            logger_.Debug("Unable to record seen app: " + seen_apps_error);
        }
    }
    RefreshResolvedSettings();
    logger_.Info("Active log file: " + logger_.ActiveLogPath().string());
    return XR_SUCCESS;
}

XrResult OpenXrLayer::GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function || !name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (std::string_view(name) == "xrGetInstanceProcAddr") {
        return next_get_instance_proc_addr_(instance, name, function);
    }

    return next_get_instance_proc_addr_(instance, name, function);
}

XrResult OpenXrLayer::DestroyInstance(XrInstance instance) {
    std::scoped_lock lock(mutex_);

    DestroyEyeGazeResources();
    DestroyInternalReferenceSpaces();
    ResetSwapchainState();
    const XrResult result = next_destroy_instance_(instance);
    if (XR_SUCCEEDED(result)) {
        instance_ = XR_NULL_HANDLE;
        next_destroy_instance_ = nullptr;
        next_create_session_ = nullptr;
        next_destroy_session_ = nullptr;
        next_begin_session_ = nullptr;
        next_attach_session_action_sets_ = nullptr;
        next_sync_actions_ = nullptr;
        next_get_system_properties_ = nullptr;
        next_enumerate_environment_blend_modes_ = nullptr;
        next_enumerate_view_configurations_ = nullptr;
        next_get_view_configuration_properties_ = nullptr;
        next_enumerate_view_configuration_views_ = nullptr;
        next_create_swapchain_ = nullptr;
        next_destroy_swapchain_ = nullptr;
        next_enumerate_swapchain_images_ = nullptr;
        next_acquire_swapchain_image_ = nullptr;
        next_wait_swapchain_image_ = nullptr;
        next_release_swapchain_image_ = nullptr;
        next_create_reference_space_ = nullptr;
        next_create_action_space_ = nullptr;
        next_destroy_space_ = nullptr;
        next_end_frame_ = nullptr;
        next_locate_space_ = nullptr;
        next_locate_views_ = nullptr;
        next_string_to_path_ = nullptr;
        next_create_action_set_ = nullptr;
        next_destroy_action_set_ = nullptr;
        next_create_action_ = nullptr;
        next_destroy_action_ = nullptr;
        next_suggest_interaction_profile_bindings_ = nullptr;
        next_get_action_state_pose_ = nullptr;
        has_loaded_config_ = false;
        locate_views_call_count_ = 0;
        pending_locate_views_diagnostics_ = 0;
        pending_end_frame_diagnostics_ = 0;
        ResetPivotActivationState();
        ResetDepthToggleState();
        ResetInstanceState();
        ResetSessionState();
    }

    return result;
}

XrResult OpenXrLayer::CreateSession(XrInstance instance,
                                    const XrSessionCreateInfo* create_info,
                                    XrSession* session) {
    const XrResult result = next_create_session_(instance, create_info, session);
    if (XR_FAILED(result) || !session) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ResetSessionState();
    active_session_ = *session;
    const auto* d3d11_binding = create_info
                                    ? static_cast<const XrGraphicsBindingD3D11KHR*>(
                                          FindStructInChain(create_info->next, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR))
                                    : nullptr;
    if (d3d11_binding && d3d11_binding->device) {
        d3d11_quadviews_compositor_.device = d3d11_binding->device;
        d3d11_quadviews_compositor_.device->AddRef();
        d3d11_quadviews_compositor_.device->GetImmediateContext(&d3d11_quadviews_compositor_.context);
        logger_.Info("D3D11 graphics binding detected; native quadviews compositor is available.");
    } else {
        logger_.Info("D3D11 graphics binding not detected; native quadviews compositor will use fallback split path.");
    }
    const XrResult internal_result = CreateInternalReferenceSpaces(*session);
    if (XR_FAILED(internal_result)) {
        logger_.Error("Failed to create one or more internal reference spaces; PivotXR will degrade for this session.");
        DestroyInternalReferenceSpaces();
    }
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (IsQuadViewsActive() && resolved_settings_.quadviews.prefer_eye_tracking) {
        const XrResult eye_gaze_result = CreateEyeGazeResources(*session);
        if (XR_FAILED(eye_gaze_result)) {
            logger_.Info("Eye gaze resources unavailable; quadviews will use head/static focus offsets.");
            DestroyEyeGazeResources();
        }
    }
    return result;
}

XrResult OpenXrLayer::DestroySession(XrSession session) {
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            DestroyEyeGazeResources();
            DestroyInternalReferenceSpaces();
            ResetD3D11QuadViewsCompositor();
            ResetSwapchainState();
        }
    }

    const XrResult result = next_destroy_session_(session);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            ResetSwapchainState();
            ResetSessionState();
        }
    }

    return result;
}

XrResult OpenXrLayer::BeginSession(XrSession session, const XrSessionBeginInfo* begin_info) {
    bool quadviews_active = false;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        quadviews_active = IsQuadViewsActive();
    }

    XrSessionBeginInfo runtime_begin_info{};
    const XrSessionBeginInfo* downstream_begin_info = begin_info;
    if (begin_info && IsQuadViewConfiguration(begin_info->primaryViewConfigurationType) && quadviews_active) {
        runtime_begin_info = *begin_info;
        runtime_begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        downstream_begin_info = &runtime_begin_info;
    }

    const XrResult result = next_begin_session_(session, downstream_begin_info);
    if (XR_FAILED(result) || !begin_info) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    active_session_ = session;
    active_primary_view_configuration_type_ = begin_info->primaryViewConfigurationType;
    active_runtime_view_configuration_type_ = downstream_begin_info->primaryViewConfigurationType;
    has_active_primary_view_configuration_ = true;
    has_logged_quad_view_short_count_ = false;

    logger_.Info(std::string("Session began with view configuration: ") +
                 ToString(active_primary_view_configuration_type_) +
                 (active_primary_view_configuration_type_ != active_runtime_view_configuration_type_
                      ? std::string(" (runtime mapped to ") + ToString(active_runtime_view_configuration_type_) + ")"
                      : std::string()));
    return result;
}

XrResult OpenXrLayer::AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attach_info) {
    if (!attach_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    XrSessionActionSetsAttachInfo downstream_attach_info = *attach_info;
    std::vector<XrActionSet> action_sets;
    bool appended_eye_gaze_set = false;
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_ && eye_gaze_resources_ready_ &&
            quadviews_action_set_ != XR_NULL_HANDLE &&
            (attach_info->countActionSets == 0 || attach_info->actionSets)) {
            if (attach_info->countActionSets > 0 && attach_info->actionSets) {
                action_sets.assign(attach_info->actionSets,
                                   attach_info->actionSets + attach_info->countActionSets);
            }
            if (std::find(action_sets.begin(), action_sets.end(), quadviews_action_set_) == action_sets.end()) {
                action_sets.push_back(quadviews_action_set_);
                appended_eye_gaze_set = true;
            }
            downstream_attach_info.countActionSets = static_cast<uint32_t>(action_sets.size());
            downstream_attach_info.actionSets = action_sets.data();
        }
    }

    const XrResult result = next_attach_session_action_sets_(session, &downstream_attach_info);
    if (XR_SUCCEEDED(result) && appended_eye_gaze_set) {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            eye_gaze_action_set_attached_ = true;
            logger_.Info("Attached VectorXR eye-gaze action set for quadviews.");
        }
    }
    return result;
}

XrResult OpenXrLayer::SyncActions(XrSession session, const XrActionsSyncInfo* sync_info) {
    if (!sync_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    XrActionsSyncInfo downstream_sync_info = *sync_info;
    std::vector<XrActiveActionSet> active_action_sets;
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_ && eye_gaze_resources_ready_ && eye_gaze_action_set_attached_ &&
            quadviews_action_set_ != XR_NULL_HANDLE &&
            (sync_info->countActiveActionSets == 0 || sync_info->activeActionSets)) {
            if (sync_info->countActiveActionSets > 0 && sync_info->activeActionSets) {
                active_action_sets.assign(sync_info->activeActionSets,
                                          sync_info->activeActionSets + sync_info->countActiveActionSets);
            }
            const auto already_present = std::find_if(active_action_sets.begin(),
                                                      active_action_sets.end(),
                                                      [&](const XrActiveActionSet& active_set) {
                                                          return active_set.actionSet == quadviews_action_set_;
                                                      });
            if (already_present == active_action_sets.end()) {
                active_action_sets.push_back({quadviews_action_set_, XR_NULL_PATH});
                downstream_sync_info.countActiveActionSets = static_cast<uint32_t>(active_action_sets.size());
                downstream_sync_info.activeActionSets = active_action_sets.data();
            }
        }
    }

    return next_sync_actions_(session, &downstream_sync_info);
}

XrResult OpenXrLayer::GetSystemProperties(XrInstance instance,
                                          XrSystemId system_id,
                                          XrSystemProperties* properties) {
    const XrResult result = next_get_system_properties_(instance, system_id, properties);
    if (XR_FAILED(result) || !properties) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (!IsQuadViewsActive()) {
        return result;
    }

    void* foveated = FindMutableStructInChain(properties->next, XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO);
    if (foveated) {
        reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(foveated)->supportsFoveatedRendering = XR_TRUE;
    }
    return result;
}

XrResult OpenXrLayer::EnumerateEnvironmentBlendModes(
    XrInstance instance,
    XrSystemId system_id,
    XrViewConfigurationType view_configuration_type,
    uint32_t environment_blend_mode_capacity_input,
    uint32_t* environment_blend_mode_count_output,
    XrEnvironmentBlendMode* environment_blend_modes) {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    const XrViewConfigurationType runtime_type =
        IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsActive()
            ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
            : view_configuration_type;
    return next_enumerate_environment_blend_modes_(instance,
                                                  system_id,
                                                  runtime_type,
                                                  environment_blend_mode_capacity_input,
                                                  environment_blend_mode_count_output,
                                                  environment_blend_modes);
}

XrResult OpenXrLayer::EnumerateViewConfigurations(XrInstance instance,
                                                  XrSystemId system_id,
                                                  uint32_t view_configuration_type_capacity_input,
                                                  uint32_t* view_configuration_type_count_output,
                                                  XrViewConfigurationType* view_configuration_types) {
    if (!view_configuration_type_count_output) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    uint32_t runtime_count = 0;
    XrResult result = next_enumerate_view_configurations_(instance, system_id, 0, &runtime_count, nullptr);
    if (XR_FAILED(result)) {
        return result;
    }

    std::vector<XrViewConfigurationType> runtime_types(runtime_count);
    if (runtime_count > 0) {
        result = next_enumerate_view_configurations_(
            instance, system_id, runtime_count, &runtime_count, runtime_types.data());
        if (XR_FAILED(result)) {
            return result;
        }
        runtime_types.resize(runtime_count);
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();

    std::vector<XrViewConfigurationType> exposed_types = runtime_types;
    const bool runtime_has_quad =
        std::find(runtime_types.begin(),
                  runtime_types.end(),
                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET) != runtime_types.end();
    const bool runtime_has_stereo =
        std::find(runtime_types.begin(), runtime_types.end(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) != runtime_types.end();
    const bool quadviews_active = IsQuadViewsActive();
    const bool app_requested_quadviews =
        quad_views_extension_requested_ || varjo_foveated_rendering_extension_requested_;
    const bool synthesize_quad = quadviews_active && runtime_has_stereo && !runtime_has_quad;
    const bool prefer_quad_first = quadviews_active && app_requested_quadviews;

    if (synthesize_quad) {
        if (prefer_quad_first) {
            exposed_types.insert(exposed_types.begin(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET);
        } else {
            exposed_types.push_back(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET);
        }
    }

    if (quadviews_active && runtime_has_quad && prefer_quad_first) {
        const auto quad_it = std::find(exposed_types.begin(),
                                       exposed_types.end(),
                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET);
        if (quad_it != exposed_types.end() && quad_it != exposed_types.begin()) {
            const XrViewConfigurationType quad_type = *quad_it;
            exposed_types.erase(quad_it);
            exposed_types.insert(exposed_types.begin(), quad_type);
        }
    }

    if (quadviews_active && !has_logged_quadviews_view_configuration_capabilities_) {
        std::ostringstream stream;
        stream << "Quadviews view configuration capabilities: appRequestedQuadviews=" << app_requested_quadviews
               << ", runtimeStereo=" << runtime_has_stereo
               << ", runtimeQuad=" << runtime_has_quad
               << ", synthesizeQuad=" << synthesize_quad
               << ", preferQuadFirst=" << prefer_quad_first
               << ", runtimeTypes=[" << FormatViewConfigurationTypes(runtime_types) << "]"
               << ", exposedTypes=[" << FormatViewConfigurationTypes(exposed_types) << "]";
        logger_.Info(stream.str());
        has_logged_quadviews_view_configuration_capabilities_ = true;
    }

    *view_configuration_type_count_output = static_cast<uint32_t>(exposed_types.size());
    if (!view_configuration_types || view_configuration_type_capacity_input == 0) {
        return XR_SUCCESS;
    }

    const uint32_t copy_count =
        std::min<uint32_t>(view_configuration_type_capacity_input, static_cast<uint32_t>(exposed_types.size()));
    std::copy_n(exposed_types.begin(), copy_count, view_configuration_types);
    return view_configuration_type_capacity_input < exposed_types.size() ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
}

XrResult OpenXrLayer::GetViewConfigurationProperties(
    XrInstance instance,
    XrSystemId system_id,
    XrViewConfigurationType view_configuration_type,
    XrViewConfigurationProperties* configuration_properties) {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    const bool synthesize_quad = IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsActive();
    const XrViewConfigurationType runtime_type =
        synthesize_quad ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO : view_configuration_type;

    const XrResult result =
        next_get_view_configuration_properties_(instance, system_id, runtime_type, configuration_properties);
    if (XR_SUCCEEDED(result) && synthesize_quad && configuration_properties) {
        configuration_properties->viewConfigurationType =
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
    }
    return result;
}

XrResult OpenXrLayer::EnumerateViewConfigurationViews(XrInstance instance,
                                                      XrSystemId system_id,
                                                      XrViewConfigurationType view_configuration_type,
                                                      uint32_t view_capacity_input,
                                                      uint32_t* view_count_output,
                                                      XrViewConfigurationView* views) {
    if (!view_count_output) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (!IsQuadViewConfiguration(view_configuration_type) || !IsQuadViewsActive()) {
        return next_enumerate_view_configuration_views_(
            instance, system_id, view_configuration_type, view_capacity_input, view_count_output, views);
    }

    std::array<XrViewConfigurationView, 2> stereo_views{};
    for (XrViewConfigurationView& view : stereo_views) {
        view = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    }
    uint32_t stereo_count = 0;
    const XrResult result = next_enumerate_view_configuration_views_(instance,
                                                                    system_id,
                                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                    static_cast<uint32_t>(stereo_views.size()),
                                                                    &stereo_count,
                                                                    stereo_views.data());
    if (XR_FAILED(result)) {
        return result;
    }
    if (stereo_count < 2) {
        *view_count_output = stereo_count;
        return result;
    }

    *view_count_output = 4;
    if (!views || view_capacity_input == 0) {
        return XR_SUCCESS;
    }
    if (view_capacity_input < 4) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    for (uint32_t i = 0; i < 2; ++i) {
        views[i] = stereo_views[i];
        views[i].recommendedImageRectWidth = ScaleDimension(stereo_views[i].recommendedImageRectWidth,
                                                            resolved_settings_.quadviews.peripheral_scale,
                                                            stereo_views[i].maxImageRectWidth);
        views[i].recommendedImageRectHeight = ScaleDimension(stereo_views[i].recommendedImageRectHeight,
                                                             resolved_settings_.quadviews.peripheral_scale,
                                                             stereo_views[i].maxImageRectHeight);
        SetFoveatedViewActive(views[i], XR_FALSE);
    }

    for (uint32_t i = 0; i < 2; ++i) {
        views[i + 2] = stereo_views[i];
        views[i + 2].recommendedImageRectWidth = ScaleDimension(stereo_views[i].recommendedImageRectWidth,
                                                                resolved_settings_.quadviews.focus_scale,
                                                                stereo_views[i].maxImageRectWidth);
        views[i + 2].recommendedImageRectHeight = ScaleDimension(stereo_views[i].recommendedImageRectHeight,
                                                                 resolved_settings_.quadviews.focus_scale,
                                                                 stereo_views[i].maxImageRectHeight);
        SetFoveatedViewActive(views[i + 2], XR_TRUE);
    }

    return XR_SUCCESS;
}

XrResult OpenXrLayer::CreateSwapchain(XrSession session,
                                      const XrSwapchainCreateInfo* create_info,
                                      XrSwapchain* swapchain) {
    const XrResult result = next_create_swapchain_(session, create_info, swapchain);
    if (XR_FAILED(result) || !create_info || !swapchain || *swapchain == XR_NULL_HANDLE) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    SwapchainInfo info;
    info.session = session;
    info.width = create_info->width;
    info.height = create_info->height;
    info.array_size = create_info->arraySize;
    info.mip_count = create_info->mipCount;
    info.sample_count = create_info->sampleCount;
    info.format = create_info->format;
    info.usage_flags = create_info->usageFlags;
    info.create_flags = create_info->createFlags;
    info.quadviews_session = IsQuadViewsActive() && session == active_session_ &&
                             (!has_active_primary_view_configuration_ ||
                              IsQuadViewConfiguration(active_primary_view_configuration_type_));
    tracked_swapchains_[*swapchain] = info;
    LogSwapchainSummary(*swapchain, info, "created");
    return result;
}

XrResult OpenXrLayer::DestroySwapchain(XrSwapchain swapchain) {
    const XrResult result = next_destroy_swapchain_(swapchain);
    if (XR_FAILED(result)) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it == tracked_swapchains_.end()) {
        logger_.Debug("Swapchain destroyed: handle=" + FormatHandle(swapchain) + ", tracked=false");
        return result;
    }

    LogSwapchainSummary(swapchain, it->second, "destroyed");
    tracked_swapchains_.erase(it);
    return result;
}

XrResult OpenXrLayer::EnumerateSwapchainImages(XrSwapchain swapchain,
                                               uint32_t image_capacity_input,
                                               uint32_t* image_count_output,
                                               XrSwapchainImageBaseHeader* images) {
    const XrResult result =
        next_enumerate_swapchain_images_(swapchain, image_capacity_input, image_count_output, images);
    if (XR_FAILED(result) || !image_count_output) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it == tracked_swapchains_.end()) {
        return result;
    }

    const bool first_complete_enumeration = !it->second.images_enumerated && images && image_capacity_input > 0;
    it->second.image_count = *image_count_output;
    if (images && image_capacity_input > 0) {
        it->second.images_enumerated = true;
        it->second.d3d11_images.clear();
        it->second.d3d11_images.reserve(*image_count_output);
        if (IsD3D11SwapchainImage(images)) {
            const auto* d3d11_images = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
            for (uint32_t i = 0; i < *image_count_output && i < image_capacity_input; ++i) {
                if (d3d11_images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                    it->second.d3d11_images.clear();
                    break;
                }
                it->second.d3d11_images.push_back(d3d11_images[i].texture);
            }
        }
    }
    if (first_complete_enumeration || it->second.quadviews_session) {
        LogSwapchainSummary(swapchain, it->second, "imagesEnumerated");
    }
    return result;
}

XrResult OpenXrLayer::AcquireSwapchainImage(XrSwapchain swapchain,
                                            const XrSwapchainImageAcquireInfo* acquire_info,
                                            uint32_t* index) {
    const XrResult result = next_acquire_swapchain_image_(swapchain, acquire_info, index);
    if (XR_FAILED(result)) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it != tracked_swapchains_.end()) {
        ++it->second.acquire_count;
        if (index) {
            it->second.last_acquired_image_index = *index;
            it->second.has_last_acquired_image_index = true;
        }
        if (it->second.quadviews_session && it->second.acquire_count <= 3) {
            LogSwapchainSummary(swapchain, it->second, "acquired");
        }
    }
    return result;
}

XrResult OpenXrLayer::WaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* wait_info) {
    const XrResult result = next_wait_swapchain_image_(swapchain, wait_info);
    if (XR_FAILED(result)) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it != tracked_swapchains_.end()) {
        ++it->second.wait_count;
    }
    return result;
}

XrResult OpenXrLayer::ReleaseSwapchainImage(XrSwapchain swapchain,
                                            const XrSwapchainImageReleaseInfo* release_info) {
    const XrResult result = next_release_swapchain_image_(swapchain, release_info);
    if (XR_FAILED(result)) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it != tracked_swapchains_.end()) {
        ++it->second.release_count;
        if (it->second.quadviews_session && it->second.release_count <= 3) {
            LogSwapchainSummary(swapchain, it->second, "released");
        }
    }
    return result;
}

bool OpenXrLayer::EnsureD3D11QuadViewsCompositor(const XrCompositionLayerProjection* projection_layer,
                                                 uint32_t output_width,
                                                 uint32_t output_height,
                                                 int64_t output_format) {
    if (!projection_layer || d3d11_quadviews_compositor_.failed || !d3d11_quadviews_compositor_.device ||
        !d3d11_quadviews_compositor_.context || active_session_ == XR_NULL_HANDLE) {
        return false;
    }

    if (!d3d11_quadviews_compositor_.initialized) {
        const char* source = D3D11QuadViewsShaderSource();
        ID3DBlob* vertex_blob = nullptr;
        ID3DBlob* pixel_blob = nullptr;
        ID3DBlob* errors = nullptr;
        HRESULT hr = D3DCompile(source,
                                std::strlen(source),
                                nullptr,
                                nullptr,
                                nullptr,
                                "VSMain",
                                "vs_5_0",
                                0,
                                0,
                                &vertex_blob,
                                &errors);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor vertex shader compile failed.");
            SafeRelease(errors);
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }
        SafeRelease(errors);

        hr = D3DCompile(source,
                        std::strlen(source),
                        nullptr,
                        nullptr,
                        nullptr,
                        "PSMain",
                        "ps_5_0",
                        0,
                        0,
                        &pixel_blob,
                        &errors);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor pixel shader compile failed.");
            SafeRelease(vertex_blob);
            SafeRelease(errors);
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }
        SafeRelease(errors);

        hr = d3d11_quadviews_compositor_.device->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                                                    vertex_blob->GetBufferSize(),
                                                                    nullptr,
                                                                    &d3d11_quadviews_compositor_.vertex_shader);
        SafeRelease(vertex_blob);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor vertex shader creation failed.");
            SafeRelease(pixel_blob);
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        hr = d3d11_quadviews_compositor_.device->CreatePixelShader(pixel_blob->GetBufferPointer(),
                                                                   pixel_blob->GetBufferSize(),
                                                                   nullptr,
                                                                   &d3d11_quadviews_compositor_.pixel_shader);
        SafeRelease(pixel_blob);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor pixel shader creation failed.");
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d3d11_quadviews_compositor_.device->CreateSamplerState(&sampler_desc,
                                                                    &d3d11_quadviews_compositor_.sampler);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor sampler creation failed.");
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        D3D11_BUFFER_DESC constants_desc{};
        constants_desc.ByteWidth = sizeof(FocusRectConstants);
        constants_desc.Usage = D3D11_USAGE_DEFAULT;
        constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = d3d11_quadviews_compositor_.device->CreateBuffer(&constants_desc,
                                                              nullptr,
                                                              &d3d11_quadviews_compositor_.constants);
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor constant buffer creation failed.");
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        d3d11_quadviews_compositor_.initialized = true;
        logger_.Info("D3D11 quadviews compositor initialized.");
    }

    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        if (target.swapchain != XR_NULL_HANDLE && target.width == output_width && target.height == output_height &&
            target.format == output_format && !target.d3d11_images.empty()) {
            continue;
        }

        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            next_destroy_swapchain_(target.swapchain);
            target = {};
        }

        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        create_info.format = output_format;
        create_info.sampleCount = 1;
        create_info.width = output_width;
        create_info.height = output_height;
        create_info.faceCount = 1;
        create_info.arraySize = 1;
        create_info.mipCount = 1;

        XrResult result = next_create_swapchain_(active_session_, &create_info, &target.swapchain);
        if (XR_FAILED(result) || target.swapchain == XR_NULL_HANDLE) {
            logger_.Error("D3D11 quadviews compositor output swapchain creation failed.");
            target = {};
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        uint32_t image_count = 0;
        result = next_enumerate_swapchain_images_(target.swapchain, 0, &image_count, nullptr);
        if (XR_FAILED(result) || image_count == 0) {
            logger_.Error("D3D11 quadviews compositor output swapchain image count query failed.");
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        std::vector<XrSwapchainImageD3D11KHR> images(image_count);
        for (XrSwapchainImageD3D11KHR& image : images) {
            image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        }
        result = next_enumerate_swapchain_images_(
            target.swapchain,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result)) {
            logger_.Error("D3D11 quadviews compositor output swapchain image enumeration failed.");
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        target.width = output_width;
        target.height = output_height;
        target.format = output_format;
        target.image_count = image_count;
        target.d3d11_images.clear();
        target.d3d11_images.reserve(image_count);
        for (const XrSwapchainImageD3D11KHR& image : images) {
            target.d3d11_images.push_back(image.texture);
        }

        logger_.Info("D3D11 quadviews compositor output swapchain ready: size=" +
                     std::to_string(target.width) + "x" + std::to_string(target.height) +
                     ", images=" + std::to_string(target.image_count));
    }

    return true;
}

bool OpenXrLayer::ComposeQuadViewsD3D11(const XrCompositionLayerProjection* source_layer,
                                        const XrPosef& reverse_delta,
                                        bool has_non_identity_delta,
                                        XrCompositionLayerProjection* composed_layer,
                                        std::vector<XrCompositionLayerProjectionView>* composed_views) {
    if (!source_layer || source_layer->viewCount < 4 || !source_layer->views || !composed_layer || !composed_views ||
        !d3d11_quadviews_compositor_.device || !d3d11_quadviews_compositor_.context) {
        return false;
    }

    std::array<const SwapchainInfo*, 4> swapchains{};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto it = tracked_swapchains_.find(source_layer->views[i].subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !it->second.has_last_acquired_image_index ||
            it->second.last_acquired_image_index >= it->second.d3d11_images.size()) {
            return false;
        }
        swapchains[i] = &it->second;
    }

    const uint32_t output_width = std::max<uint32_t>(swapchains[2]->width, swapchains[3]->width);
    const uint32_t output_height = std::max<uint32_t>(swapchains[2]->height, swapchains[3]->height);
    const int64_t output_format = swapchains[2]->format;
    if (!EnsureD3D11QuadViewsCompositor(source_layer, output_width, output_height, output_format)) {
        return false;
    }

    struct SavedD3D11State {
        ID3D11RenderTargetView* render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth_stencil{nullptr};
        ID3D11VertexShader* vertex_shader{nullptr};
        ID3D11PixelShader* pixel_shader{nullptr};
        ID3D11InputLayout* input_layout{nullptr};
        ID3D11ShaderResourceView* shader_resources[2]{};
        ID3D11SamplerState* samplers[1]{};
        ID3D11Buffer* constant_buffers[1]{};
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewport_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
        D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
    } saved;

    ID3D11DeviceContext* context = d3d11_quadviews_compositor_.context;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, &saved.depth_stencil);
    context->VSGetShader(&saved.vertex_shader, nullptr, nullptr);
    context->PSGetShader(&saved.pixel_shader, nullptr, nullptr);
    context->IAGetInputLayout(&saved.input_layout);
    context->IAGetPrimitiveTopology(&saved.topology);
    context->PSGetShaderResources(0, 2, saved.shader_resources);
    context->PSGetSamplers(0, 1, saved.samplers);
    context->PSGetConstantBuffers(0, 1, saved.constant_buffers);
    context->RSGetViewports(&saved.viewport_count, saved.viewports);

    auto restore_state = [&]() {
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, saved.depth_stencil);
        context->VSSetShader(saved.vertex_shader, nullptr, 0);
        context->PSSetShader(saved.pixel_shader, nullptr, 0);
        context->IASetInputLayout(saved.input_layout);
        context->IASetPrimitiveTopology(saved.topology);
        context->PSSetShaderResources(0, 2, saved.shader_resources);
        context->PSSetSamplers(0, 1, saved.samplers);
        context->PSSetConstantBuffers(0, 1, saved.constant_buffers);
        context->RSSetViewports(saved.viewport_count, saved.viewports);

        for (ID3D11RenderTargetView*& render_target : saved.render_targets) {
            SafeRelease(render_target);
        }
        SafeRelease(saved.depth_stencil);
        SafeRelease(saved.vertex_shader);
        SafeRelease(saved.pixel_shader);
        SafeRelease(saved.input_layout);
        for (ID3D11ShaderResourceView*& resource : saved.shader_resources) {
            SafeRelease(resource);
        }
        for (ID3D11SamplerState*& sampler : saved.samplers) {
            SafeRelease(sampler);
        }
        for (ID3D11Buffer*& buffer : saved.constant_buffers) {
            SafeRelease(buffer);
        }
    };

    bool rendered = true;
    std::array<uint32_t, 2> output_indices{};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = next_acquire_swapchain_image_(target.swapchain, &acquire_info, &output_indices[eye]);
        if (XR_FAILED(result) || output_indices[eye] >= target.d3d11_images.size()) {
            rendered = false;
            break;
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        result = next_wait_swapchain_image_(target.swapchain, &wait_info);
        if (XR_FAILED(result)) {
            rendered = false;
            break;
        }

        ID3D11RenderTargetView* render_target = nullptr;
        ID3D11ShaderResourceView* peripheral_resource = nullptr;
        ID3D11ShaderResourceView* focus_resource = nullptr;
        HRESULT hr = d3d11_quadviews_compositor_.device->CreateRenderTargetView(
            target.d3d11_images[output_indices[eye]],
            nullptr,
            &render_target);
        if (SUCCEEDED(hr)) {
            hr = d3d11_quadviews_compositor_.device->CreateShaderResourceView(
                swapchains[eye]->d3d11_images[swapchains[eye]->last_acquired_image_index],
                nullptr,
                &peripheral_resource);
        }
        if (SUCCEEDED(hr)) {
            hr = d3d11_quadviews_compositor_.device->CreateShaderResourceView(
                swapchains[eye + 2]->d3d11_images[swapchains[eye + 2]->last_acquired_image_index],
                nullptr,
                &focus_resource);
        }
        if (FAILED(hr)) {
            SafeRelease(render_target);
            SafeRelease(peripheral_resource);
            SafeRelease(focus_resource);
            rendered = false;
            break;
        }

        const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(render_target, clear_color);
        context->OMSetRenderTargets(1, &render_target, nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(target.width);
        viewport.Height = static_cast<float>(target.height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(d3d11_quadviews_compositor_.vertex_shader, nullptr, 0);
        context->PSSetShader(d3d11_quadviews_compositor_.pixel_shader, nullptr, 0);
        ID3D11ShaderResourceView* resources[2]{peripheral_resource, focus_resource};
        context->PSSetShaderResources(0, 2, resources);
        context->PSSetSamplers(0, 1, &d3d11_quadviews_compositor_.sampler);

        FocusRectConstants constants =
            BuildFocusRectConstants(source_layer->views[eye].fov, source_layer->views[eye + 2].fov, target.width, target.height);
        context->UpdateSubresource(d3d11_quadviews_compositor_.constants, 0, nullptr, &constants, 0, 0);
        context->PSSetConstantBuffers(0, 1, &d3d11_quadviews_compositor_.constants);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* null_resources[2]{nullptr, nullptr};
        context->PSSetShaderResources(0, 2, null_resources);
        SafeRelease(render_target);
        SafeRelease(peripheral_resource);
        SafeRelease(focus_resource);

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = next_release_swapchain_image_(target.swapchain, &release_info);
        if (XR_FAILED(result)) {
            rendered = false;
            break;
        }
    }

    restore_state();
    if (!rendered) {
        logger_.Error("D3D11 quadviews composition failed; falling back to projection-layer split.");
        return false;
    }

    composed_views->assign(source_layer->views, source_layer->views + 2);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (has_non_identity_delta) {
            (*composed_views)[eye].pose = MultiplyPoses((*composed_views)[eye].pose, reverse_delta);
        }
        QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
        (*composed_views)[eye].subImage.swapchain = target.swapchain;
        (*composed_views)[eye].subImage.imageRect.offset = {0, 0};
        (*composed_views)[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(target.width),
            static_cast<int32_t>(target.height),
        };
        (*composed_views)[eye].subImage.imageArrayIndex = 0;
    }

    *composed_layer = *source_layer;
    composed_layer->viewCount = static_cast<uint32_t>(composed_views->size());
    composed_layer->views = composed_views->data();
    composed_layer->layerFlags &= ~(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT);
    return true;
}

XrResult OpenXrLayer::EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    if (!frame_end_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (!resolved_settings_.core.enabled) {
        cached_pivot_pose_deltas_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        return next_end_frame_(session, frame_end_info);
    }

    const bool quadviews_projection_split_active =
        IsQuadViewsActive() && has_active_primary_view_configuration_ &&
        IsQuadViewConfiguration(active_primary_view_configuration_type_) &&
        active_runtime_view_configuration_type_ == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    if (!resolved_settings_.pivotxr.enabled) {
        cached_pivot_pose_deltas_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
    }

    XrPosef pose_delta = IdentityPose();
    XrTime matched_time = 0;
    const bool has_pose_delta = resolved_settings_.pivotxr.enabled &&
                                FindPivotPoseDelta(frame_end_info->displayTime, &pose_delta, &matched_time);
    const bool has_non_identity_delta =
        has_pose_delta && (!NearlyZero(pose_delta.orientation.x) || !NearlyZero(pose_delta.orientation.y) ||
                           !NearlyZero(pose_delta.orientation.z) || !NearlyEqual(pose_delta.orientation.w, 1.0f) ||
                           !NearlyZero(pose_delta.position.x) || !NearlyZero(pose_delta.position.y) ||
                           !NearlyZero(pose_delta.position.z));

    if (!has_non_identity_delta && !quadviews_projection_split_active) {
        if (pending_end_frame_diagnostics_ > 0) {
            std::ostringstream stream;
            stream << "EndFrame pivot correction skipped: cacheHit=" << has_pose_delta
                   << ", frameTime=" << frame_end_info->displayTime;
            if (has_pose_delta) {
                stream << ", matchedTime=" << matched_time
                       << ", matchedDeltaNs=" << (matched_time - frame_end_info->displayTime);
            }
            stream << ", cachedPivotDeltas=" << cached_pivot_pose_deltas_.size();
            logger_.Debug(stream.str());
            --pending_end_frame_diagnostics_;
        }
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        return next_end_frame_(session, frame_end_info);
    }

    const XrPosef reverse_delta = InvertPose(pose_delta);
    std::vector<std::vector<XrCompositionLayerProjectionView>> adjusted_projection_views;
    std::vector<XrCompositionLayerProjection> adjusted_projection_layers;
    std::vector<const XrCompositionLayerBaseHeader*> adjusted_layers;
    uint32_t corrected_projection_layer_count = 0;
    uint32_t corrected_projection_view_count = 0;
    uint32_t split_quad_projection_layer_count = 0;
    uint32_t d3d11_quad_composition_count = 0;
    uint32_t projection_swapchain_reference_count = 0;
    uint32_t unknown_projection_swapchain_count = 0;
    adjusted_projection_views.reserve(frame_end_info->layerCount * 3);
    adjusted_projection_layers.reserve(frame_end_info->layerCount * 3);
    adjusted_layers.reserve(frame_end_info->layerCount * 3);

    auto append_projection_layer = [&](const XrCompositionLayerProjection* projection_layer,
                                       uint32_t first_view,
                                       uint32_t view_count,
                                       XrCompositionLayerFlags extra_layer_flags = 0) {
        adjusted_projection_views.emplace_back(projection_layer->views + first_view,
                                               projection_layer->views + first_view + view_count);
        for (XrCompositionLayerProjectionView& projection_view : adjusted_projection_views.back()) {
            if (has_non_identity_delta) {
                projection_view.pose = MultiplyPoses(projection_view.pose, reverse_delta);
            }
        }

        adjusted_projection_layers.push_back(*projection_layer);
        adjusted_projection_layers.back().layerFlags |= extra_layer_flags;
        adjusted_projection_layers.back().viewCount = view_count;
        adjusted_projection_layers.back().views = adjusted_projection_views.back().data();
        adjusted_layers.push_back(
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&adjusted_projection_layers.back()));

        ++corrected_projection_layer_count;
        corrected_projection_view_count += view_count;
    };

    for (uint32_t i = 0; i < frame_end_info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base_header = frame_end_info->layers[i];
        if (!base_header || base_header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            adjusted_layers.push_back(base_header);
            continue;
        }

        const XrCompositionLayerProjection* projection_layer =
            reinterpret_cast<const XrCompositionLayerProjection*>(base_header);
        if (!projection_layer->views || projection_layer->viewCount == 0) {
            adjusted_layers.push_back(base_header);
            continue;
        }

        for (uint32_t view_index = 0; view_index < projection_layer->viewCount; ++view_index) {
            const XrSwapchain referenced_swapchain = projection_layer->views[view_index].subImage.swapchain;
            if (referenced_swapchain == XR_NULL_HANDLE) {
                continue;
            }
            ++projection_swapchain_reference_count;
            if (!tracked_swapchains_.contains(referenced_swapchain)) {
                ++unknown_projection_swapchain_count;
            }
        }

        if (quadviews_projection_split_active && projection_layer->viewCount >= 4) {
            adjusted_projection_views.emplace_back();
            adjusted_projection_layers.emplace_back();
            if (ComposeQuadViewsD3D11(projection_layer,
                                      reverse_delta,
                                      has_non_identity_delta,
                                      &adjusted_projection_layers.back(),
                                      &adjusted_projection_views.back())) {
                adjusted_layers.push_back(
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&adjusted_projection_layers.back()));
                ++corrected_projection_layer_count;
                corrected_projection_view_count += 2;
                ++split_quad_projection_layer_count;
                ++d3d11_quad_composition_count;
                continue;
            }
            adjusted_projection_layers.pop_back();
            adjusted_projection_views.pop_back();

            // Stereo runtimes vary in how faithfully they composite multiple
            // projection layers. Submit the inset on both sides of the
            // peripheral layer so the focus view survives both normal and
            // reversed painter ordering while we build the native compositor.
            constexpr XrCompositionLayerFlags kFovealBlendFlags =
                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            append_projection_layer(projection_layer, 2, 2, kFovealBlendFlags);
            append_projection_layer(projection_layer, 0, 2);
            append_projection_layer(projection_layer, 2, 2, kFovealBlendFlags);
            ++split_quad_projection_layer_count;
            continue;
        }

        append_projection_layer(projection_layer, 0, projection_layer->viewCount);
    }

    XrFrameEndInfo adjusted_frame_end_info = *frame_end_info;
    adjusted_frame_end_info.layers = adjusted_layers.data();
    if (pending_end_frame_diagnostics_ > 0) {
        const ViewOrientation delta_orientation{
            pose_delta.orientation.x,
            pose_delta.orientation.y,
            pose_delta.orientation.z,
            pose_delta.orientation.w,
        };
        std::ostringstream stream;
        stream << "EndFrame pivot correction applied: frameTime=" << frame_end_info->displayTime
               << ", matchedTime=" << matched_time
               << ", matchedDeltaNs=" << (matched_time - frame_end_info->displayTime)
               << ", poseDeltaYaw=" << FormatDiagnosticDouble(ExtractYawRadians(delta_orientation))
               << ", poseDeltaPitch=" << FormatDiagnosticDouble(ExtractPitchRadians(delta_orientation))
               << ", projectionLayers=" << corrected_projection_layer_count
               << ", projectionViews=" << corrected_projection_view_count
               << ", splitQuadProjectionLayers=" << split_quad_projection_layer_count
               << ", d3d11QuadCompositions=" << d3d11_quad_composition_count
               << ", projectionSwapchainRefs=" << projection_swapchain_reference_count
               << ", unknownProjectionSwapchains=" << unknown_projection_swapchain_count
               << ", trackedSwapchains=" << tracked_swapchains_.size()
               << ", cachedPivotDeltas=" << cached_pivot_pose_deltas_.size();
        logger_.Debug(stream.str());
        --pending_end_frame_diagnostics_;
    } else if (quadviews_projection_split_active && split_quad_projection_layer_count > 0) {
        logger_.Debug("EndFrame quadviews projection split applied: splitLayers=" +
                      std::to_string(split_quad_projection_layer_count) +
                      ", d3d11QuadCompositions=" + std::to_string(d3d11_quad_composition_count) +
                      ", submittedLayers=" + std::to_string(frame_end_info->layerCount) +
                      ", runtimeLayers=" + std::to_string(adjusted_layers.size()) +
                      ", projectionSwapchainRefs=" + std::to_string(projection_swapchain_reference_count) +
                      ", unknownProjectionSwapchains=" + std::to_string(unknown_projection_swapchain_count) +
                      ", trackedSwapchains=" + std::to_string(tracked_swapchains_.size()));
    }
    const XrResult result = next_end_frame_(session, &adjusted_frame_end_info);
    PrunePivotPoseDeltas(frame_end_info->displayTime);
    return result;
}

XrResult OpenXrLayer::CreateReferenceSpace(XrSession session,
                                           const XrReferenceSpaceCreateInfo* create_info,
                                           XrSpace* space) {
    const XrResult result = next_create_reference_space_(session, create_info, space);
    if (XR_FAILED(result) || !create_info || !space) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    switch (create_info->referenceSpaceType) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        tracked_view_spaces_.insert(*space);
        break;
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        tracked_local_spaces_.insert(*space);
        break;
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        tracked_stage_spaces_.insert(*space);
        break;
    default:
        break;
    }

    return result;
}

XrResult OpenXrLayer::LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location) {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (!resolved_settings_.core.enabled) {
        return next_locate_space_(space, base_space, time, location);
    }

    const bool pivotxr_active = IsPivotXrActive(resolved_settings_.pivotxr);
    if (has_active_primary_view_configuration_ &&
        IsQuadViewConfiguration(active_primary_view_configuration_type_)) {
        return next_locate_space_(space, base_space, time, location);
    }

    return LocateSpaceWithPivot(
        space, base_space, time, resolved_settings_.pivotxr, pivotxr_active, location, nullptr, nullptr, nullptr, false);
}

XrResult OpenXrLayer::LocateViews(XrSession session,
                                  const XrViewLocateInfo* view_locate_info,
                                  XrViewState* view_state,
                                  uint32_t view_capacity_input,
                                  uint32_t* view_count_output,
                                  XrView* views) {
    bool synthesized_quad_views = false;
    const XrResult result = LocateRuntimeViews(
        session, view_locate_info, view_state, view_capacity_input, view_count_output, views, &synthesized_quad_views);

    if (XR_FAILED(result) || !views || !view_count_output) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();

    if (!resolved_settings_.core.enabled) {
        ResetPivotActivationState();
        ResetDepthToggleState();
        cached_pivot_pose_deltas_.clear();
        return result;
    }

    const uint32_t count = std::min(view_capacity_input, *view_count_output);
    if (count == 0) {
        return result;
    }

    XrViewConfigurationType view_configuration_type = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (view_locate_info) {
        view_configuration_type = view_locate_info->viewConfigurationType;
    } else if (has_active_primary_view_configuration_ && session == active_session_) {
        view_configuration_type = active_primary_view_configuration_type_;
    }

    const ViewLayout view_layout = DetermineViewLayout(view_configuration_type, count);
    if (IsQuadViewConfiguration(view_configuration_type) &&
        view_layout == ViewLayout::kMono && !has_logged_quad_view_short_count_) {
        std::ostringstream stream;
        stream << "Quad-view session reported only " << count
               << " views to xrLocateViews; skipping DepthXR adjustments for this call.";
        logger_.Info(stream.str());
        has_logged_quad_view_short_count_ = true;
        return result;
    }

    // DepthXR keeps the interception surface minimal and applies all current
    // stereo/depth experiments in xrLocateViews.
    ++locate_views_call_count_;
    std::vector<ViewAdjustmentData> original_views(count);
    std::vector<ViewAdjustmentData> adjusted_views(count);
    for (uint32_t i = 0; i < count; ++i) {
        original_views[i].position.x = views[i].pose.position.x;
        original_views[i].position.y = views[i].pose.position.y;
        original_views[i].position.z = views[i].pose.position.z;
        original_views[i].fov.angle_left = views[i].fov.angleLeft;
        original_views[i].fov.angle_right = views[i].fov.angleRight;
        original_views[i].fov.angle_up = views[i].fov.angleUp;
        original_views[i].fov.angle_down = views[i].fov.angleDown;

        adjusted_views[i].position.x = views[i].pose.position.x;
        adjusted_views[i].position.y = views[i].pose.position.y;
        adjusted_views[i].position.z = views[i].pose.position.z;
        adjusted_views[i].fov.angle_left = views[i].fov.angleLeft;
        adjusted_views[i].fov.angle_right = views[i].fov.angleRight;
        adjusted_views[i].fov.angle_up = views[i].fov.angleUp;
        adjusted_views[i].fov.angle_down = views[i].fov.angleDown;
        original_views[i].orientation.x = views[i].pose.orientation.x;
        original_views[i].orientation.y = views[i].pose.orientation.y;
        original_views[i].orientation.z = views[i].pose.orientation.z;
        original_views[i].orientation.w = views[i].pose.orientation.w;
        adjusted_views[i].orientation.x = views[i].pose.orientation.x;
        adjusted_views[i].orientation.y = views[i].pose.orientation.y;
        adjusted_views[i].orientation.z = views[i].pose.orientation.z;
        adjusted_views[i].orientation.w = views[i].pose.orientation.w;
    }

    const bool pivotxr_active = IsPivotXrActive(resolved_settings_.pivotxr);
    const bool depthxr_active = IsDepthXrActive();
    if (resolved_settings_.pivotxr.enabled && !has_logged_pivotxr_spike_mode_) {
        std::ostringstream stream;
        stream << "PivotXR spike is active; quad-view sessions use orientation-stable mode. Press "
               << BindingLabel(resolved_settings_.pivotxr.activation_binding) << " to "
               << (resolved_settings_.pivotxr.activation_mode == ActivationMode::Toggle ? "toggle" : "hold")
               << " the extra pivot factor.";
        logger_.Info(stream.str());
        has_logged_pivotxr_spike_mode_ = true;
    }

    if (resolved_settings_.pivotxr.enabled && pivotxr_active && IsQuadViewConfiguration(view_configuration_type)) {
        const auto now = std::chrono::steady_clock::now();
        double delta_seconds = 0.0;
        if (pivotxr_last_smoothing_wall_time_.has_value()) {
            delta_seconds =
                std::chrono::duration<double>(now - *pivotxr_last_smoothing_wall_time_).count();
        }
        pivotxr_last_smoothing_wall_time_ = now;

        ViewOrientation pivot_reference_orientation = adjusted_views[0].orientation;
        if (internal_view_space_ != XR_NULL_HANDLE && view_locate_info) {
            XrSpaceLocation runtime_view_location{XR_TYPE_SPACE_LOCATION};
            const XrResult runtime_view_result = next_locate_space_(internal_view_space_,
                                                                    view_locate_info->space,
                                                                    view_locate_info->displayTime,
                                                                    &runtime_view_location);
            if (XR_SUCCEEDED(runtime_view_result)) {
                pivot_reference_orientation.x = runtime_view_location.pose.orientation.x;
                pivot_reference_orientation.y = runtime_view_location.pose.orientation.y;
                pivot_reference_orientation.z = runtime_view_location.pose.orientation.z;
                pivot_reference_orientation.w = runtime_view_location.pose.orientation.w;
            }
        }

        const double current_yaw_radians = ExtractYawRadians(pivot_reference_orientation);
        const double current_pitch_radians = ExtractPitchRadians(pivot_reference_orientation);
        const double extra_yaw_radians = ComputePivotExtraAngleRadians(current_yaw_radians,
                                                                       resolved_settings_.pivotxr.yaw_rotation_multiplier,
                                                                       resolved_settings_.pivotxr.yaw_deadzone_degrees,
                                                                       resolved_settings_.pivotxr.yaw_max_extra_degrees,
                                                                       resolved_settings_.pivotxr.yaw_smoothing,
                                                                       delta_seconds,
                                                                       pivotxr_smoothed_extra_yaw_radians_,
                                                                       false);
        const double extra_pitch_radians =
            ComputePivotExtraAngleRadians(current_pitch_radians,
                                          resolved_settings_.pivotxr.pitch_rotation_multiplier,
                                          resolved_settings_.pivotxr.pitch_deadzone_degrees,
                                          resolved_settings_.pivotxr.pitch_max_extra_degrees,
                                          resolved_settings_.pivotxr.pitch_smoothing,
                                          delta_seconds,
                                          pivotxr_smoothed_extra_pitch_radians_,
                                          false);
        if (!NearlyZero(extra_yaw_radians) || !NearlyZero(extra_pitch_radians)) {
            for (ViewAdjustmentData& view : adjusted_views) {
                view.orientation = ApplyExtraRotationToOrientation(view.orientation, extra_yaw_radians, extra_pitch_radians);
            }
        }
    } else if (resolved_settings_.pivotxr.enabled && pivotxr_active && internal_view_space_ != XR_NULL_HANDLE &&
               view_locate_info) {
        XrSpaceLocation pivot_view_location{XR_TYPE_SPACE_LOCATION};
        double applied_extra_yaw_radians = 0.0;
        double applied_extra_pitch_radians = 0.0;
        XrPosef applied_pose_delta = IdentityPose();
        const XrResult pivot_result = LocateSpaceWithPivot(internal_view_space_,
                                                           view_locate_info->space,
                                                           view_locate_info->displayTime,
                                                           resolved_settings_.pivotxr,
                                                           pivotxr_active,
                                                           &pivot_view_location,
                                                           &applied_extra_yaw_radians,
                                                           &applied_extra_pitch_radians,
                                                           &applied_pose_delta,
                                                           true);
        if (XR_SUCCEEDED(pivot_result)) {
            if (NearlyZero(applied_extra_yaw_radians) && NearlyZero(applied_extra_pitch_radians)) {
                CachePivotPoseDelta(view_locate_info->displayTime, IdentityPose());
            } else if (EnsureEyeOffsets(session, view_configuration_type, view_locate_info->displayTime, count)) {
                CachePivotPoseDelta(view_locate_info->displayTime, applied_pose_delta);
                for (uint32_t i = 0; i < count; ++i) {
                    adjusted_views[i].position.x = cached_eye_offset_poses_[i].position.x;
                    adjusted_views[i].position.y = cached_eye_offset_poses_[i].position.y;
                    adjusted_views[i].position.z = cached_eye_offset_poses_[i].position.z;
                    adjusted_views[i].orientation.x = cached_eye_offset_poses_[i].orientation.x;
                    adjusted_views[i].orientation.y = cached_eye_offset_poses_[i].orientation.y;
                    adjusted_views[i].orientation.z = cached_eye_offset_poses_[i].orientation.z;
                    adjusted_views[i].orientation.w = cached_eye_offset_poses_[i].orientation.w;

                    const XrPosef recomposed_pose = MultiplyPoses(
                        cached_eye_offset_poses_[i], pivot_view_location.pose);
                    adjusted_views[i].position.x = recomposed_pose.position.x;
                    adjusted_views[i].position.y = recomposed_pose.position.y;
                    adjusted_views[i].position.z = recomposed_pose.position.z;
                    adjusted_views[i].orientation.x = recomposed_pose.orientation.x;
                    adjusted_views[i].orientation.y = recomposed_pose.orientation.y;
                    adjusted_views[i].orientation.z = recomposed_pose.orientation.z;
                    adjusted_views[i].orientation.w = recomposed_pose.orientation.w;
                }
            } else {
                CachePivotPoseDelta(view_locate_info->displayTime, IdentityPose());
            }
            pivotxr_smoothed_extra_yaw_radians_ = applied_extra_yaw_radians;
            pivotxr_smoothed_extra_pitch_radians_ = applied_extra_pitch_radians;
        }
    } else if (!pivotxr_active) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
    }

    if (depthxr_active &&
        resolved_settings_.depthxr.stereo_boost_enabled &&
        !NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0)) {
        ApplyStereoBoost(adjusted_views, resolved_settings_.depthxr.stereo_boost, view_layout);
    }
    if (depthxr_active &&
        resolved_settings_.depthxr.convergence_enabled &&
        !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0)) {
        ApplyConvergence(adjusted_views, resolved_settings_.depthxr.convergence, view_layout);
    }

    for (uint32_t i = 0; i < count; ++i) {
        views[i].pose.position.x = static_cast<float>(adjusted_views[i].position.x);
        views[i].pose.position.y = static_cast<float>(adjusted_views[i].position.y);
        views[i].pose.position.z = static_cast<float>(adjusted_views[i].position.z);
        views[i].pose.orientation.x = static_cast<float>(adjusted_views[i].orientation.x);
        views[i].pose.orientation.y = static_cast<float>(adjusted_views[i].orientation.y);
        views[i].pose.orientation.z = static_cast<float>(adjusted_views[i].orientation.z);
        views[i].pose.orientation.w = static_cast<float>(adjusted_views[i].orientation.w);
        views[i].fov.angleLeft = static_cast<float>(adjusted_views[i].fov.angle_left);
        views[i].fov.angleRight = static_cast<float>(adjusted_views[i].fov.angle_right);
        views[i].fov.angleUp = static_cast<float>(adjusted_views[i].fov.angle_up);
        views[i].fov.angleDown = static_cast<float>(adjusted_views[i].fov.angle_down);
    }

    if (pending_locate_views_diagnostics_ > 0) {
        std::ostringstream stream;
        const double left_position_delta =
            adjusted_views[0].position.x - original_views[0].position.x;
        const double right_position_delta =
            count > 1 ? adjusted_views[1].position.x - original_views[1].position.x : 0.0;
        const double left_projection_center_delta =
            HorizontalProjectionCenter(adjusted_views[0].fov) - HorizontalProjectionCenter(original_views[0].fov);
        const double right_projection_center_delta = count > 1
                                                         ? HorizontalProjectionCenter(adjusted_views[1].fov) -
                                                               HorizontalProjectionCenter(original_views[1].fov)
                                                         : 0.0;
        const double left_yaw_delta =
            ExtractYawRadians(adjusted_views[0].orientation) - ExtractYawRadians(original_views[0].orientation);
        const double right_yaw_delta =
            count > 1 ? ExtractYawRadians(adjusted_views[1].orientation) -
                            ExtractYawRadians(original_views[1].orientation)
                      : 0.0;
        const double left_inset_yaw_delta =
            count > 2 ? ExtractYawRadians(adjusted_views[2].orientation) -
                            ExtractYawRadians(original_views[2].orientation)
                      : 0.0;
        const double right_inset_yaw_delta =
            count > 3 ? ExtractYawRadians(adjusted_views[3].orientation) -
                            ExtractYawRadians(original_views[3].orientation)
                      : 0.0;
        const double left_pitch_delta =
            ExtractPitchRadians(adjusted_views[0].orientation) - ExtractPitchRadians(original_views[0].orientation);
        const double right_pitch_delta =
            count > 1 ? ExtractPitchRadians(adjusted_views[1].orientation) -
                            ExtractPitchRadians(original_views[1].orientation)
                      : 0.0;
        stream << "LocateViews call " << locate_views_call_count_ << ": count=" << count
               << ", viewConfig=" << ToString(view_configuration_type)
               << ", pivotExtraYawRadians=" << FormatDiagnosticDouble(pivotxr_smoothed_extra_yaw_radians_)
               << ", pivotExtraPitchRadians=" << FormatDiagnosticDouble(pivotxr_smoothed_extra_pitch_radians_)
               << ", leftYawDelta=" << FormatDiagnosticDouble(left_yaw_delta)
               << ", rightYawDelta=" << FormatDiagnosticDouble(right_yaw_delta)
               << ", leftInsetYawDelta=" << FormatDiagnosticDouble(left_inset_yaw_delta)
               << ", rightInsetYawDelta=" << FormatDiagnosticDouble(right_inset_yaw_delta)
               << ", leftPitchDelta=" << FormatDiagnosticDouble(left_pitch_delta)
               << ", rightPitchDelta=" << FormatDiagnosticDouble(right_pitch_delta)
               << ", depthRuntimeActive=" << depthxr_active
               << ", stereoBoostEnabled=" << resolved_settings_.depthxr.stereo_boost_enabled
               << ", stereoBoost=" << FormatDiagnosticDouble(resolved_settings_.depthxr.stereo_boost)
               << ", convergenceEnabled=" << resolved_settings_.depthxr.convergence_enabled
               << ", convergence=" << FormatDiagnosticDouble(resolved_settings_.depthxr.convergence)
               << ", leftXDelta=" << FormatDiagnosticDouble(left_position_delta)
               << ", rightXDelta=" << FormatDiagnosticDouble(right_position_delta)
               << ", leftProjCenterDelta=" << FormatDiagnosticDouble(left_projection_center_delta)
               << ", rightProjCenterDelta=" << FormatDiagnosticDouble(right_projection_center_delta)
               << ", before:";
        AppendViewSummary(stream, original_views);
        stream << " after:";
        AppendViewSummary(stream, adjusted_views);
        logger_.Debug(stream.str());
        --pending_locate_views_diagnostics_;
    }

    return result;
}

XrResult OpenXrLayer::DestroySpace(XrSpace space) {
    const XrResult result = next_destroy_space_(space);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        tracked_view_spaces_.erase(space);
        tracked_local_spaces_.erase(space);
        tracked_stage_spaces_.erase(space);
        if (space == internal_view_space_) {
            internal_view_space_ = XR_NULL_HANDLE;
        }
        if (space == internal_local_space_) {
            internal_local_space_ = XR_NULL_HANDLE;
        }
        if (space == internal_stage_space_) {
            internal_stage_space_ = XR_NULL_HANDLE;
        }
    }

    return result;
}

void OpenXrLayer::ReloadConfigIfNeeded() {
    if (config_path_.empty()) {
        config_path_ = ResolveConfigPath();
    }

    if (!std::filesystem::exists(config_path_)) {
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
            logger_.Info("No config file found. Using default settings.");
        }
        return;
    }

    const auto timestamp = std::filesystem::last_write_time(config_path_);
    if (has_loaded_config_ && has_config_timestamp_ && timestamp == last_config_write_time_) {
        return;
    }

    const ParseResult loaded = LoadConfigFromFile(config_path_);
    if (!loaded.ok) {
        const bool already_reported_failure =
            has_failed_config_timestamp_ && timestamp == last_failed_config_write_time_ &&
            loaded.error == last_failed_config_error_;
        if (!already_reported_failure) {
            logger_.Error("Failed to parse config: " + loaded.error);
        }
        last_failed_config_write_time_ = timestamp;
        has_failed_config_timestamp_ = true;
        last_failed_config_error_ = loaded.error;
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
        }
        return;
    }

    config_ = loaded.document;
    has_loaded_config_ = true;
    has_config_timestamp_ = true;
    last_config_write_time_ = timestamp;
    has_failed_config_timestamp_ = false;
    last_failed_config_error_.clear();
    logger_.Info("Loaded config from " + config_path_.string());
}

void OpenXrLayer::RefreshResolvedSettings() {
    const ResolvedRuntimeConfig previous = resolved_settings_;
    resolved_settings_ = ResolveRuntimeConfig(config_, current_exe_name_);
    if (!SameInputBinding(previous.depthxr_bindings.toggle_enabled, resolved_settings_.depthxr_bindings.toggle_enabled) ||
        previous.depthxr.enabled != resolved_settings_.depthxr.enabled) {
        ResetDepthToggleState();
    }
    if (!SamePivotActivationBinding(previous.pivotxr, resolved_settings_.pivotxr)) {
        ResetPivotActivationState();
        has_logged_pivotxr_spike_mode_ = false;
    }
    logger_.SetLevel(resolved_settings_.core.log_level);
    logger_.SetRetentionFiles(resolved_settings_.core.log_retention_files);
    if (!last_logged_settings_ || !SameSettings(*last_logged_settings_, resolved_settings_)) {
        LogResolvedSettings(resolved_settings_);
        last_logged_settings_ = resolved_settings_;
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
    }
}

void OpenXrLayer::CaptureInstanceFunctions() {
    PFN_xrVoidFunction function = nullptr;

    if (XR_SUCCEEDED(next_get_instance_proc_addr_(
            instance_, "xrDestroyInstance", &function))) {
        next_destroy_instance_ = reinterpret_cast<PFN_xrDestroyInstance>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateSession", &function))) {
        next_create_session_ = reinterpret_cast<PFN_xrCreateSession>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrDestroySession", &function))) {
        next_destroy_session_ = reinterpret_cast<PFN_xrDestroySession>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrBeginSession", &function))) {
        next_begin_session_ = reinterpret_cast<PFN_xrBeginSession>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrAttachSessionActionSets", &function))) {
        next_attach_session_action_sets_ = reinterpret_cast<PFN_xrAttachSessionActionSets>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrSyncActions", &function))) {
        next_sync_actions_ = reinterpret_cast<PFN_xrSyncActions>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEndFrame", &function))) {
        next_end_frame_ = reinterpret_cast<PFN_xrEndFrame>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetSystemProperties", &function))) {
        next_get_system_properties_ = reinterpret_cast<PFN_xrGetSystemProperties>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateEnvironmentBlendModes", &function))) {
        next_enumerate_environment_blend_modes_ =
            reinterpret_cast<PFN_xrEnumerateEnvironmentBlendModes>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateViewConfigurations", &function))) {
        next_enumerate_view_configurations_ =
            reinterpret_cast<PFN_xrEnumerateViewConfigurations>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetViewConfigurationProperties", &function))) {
        next_get_view_configuration_properties_ =
            reinterpret_cast<PFN_xrGetViewConfigurationProperties>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateViewConfigurationViews", &function))) {
        next_enumerate_view_configuration_views_ =
            reinterpret_cast<PFN_xrEnumerateViewConfigurationViews>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateSwapchain", &function))) {
        next_create_swapchain_ = reinterpret_cast<PFN_xrCreateSwapchain>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrDestroySwapchain", &function))) {
        next_destroy_swapchain_ = reinterpret_cast<PFN_xrDestroySwapchain>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateSwapchainImages", &function))) {
        next_enumerate_swapchain_images_ =
            reinterpret_cast<PFN_xrEnumerateSwapchainImages>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrAcquireSwapchainImage", &function))) {
        next_acquire_swapchain_image_ = reinterpret_cast<PFN_xrAcquireSwapchainImage>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrWaitSwapchainImage", &function))) {
        next_wait_swapchain_image_ = reinterpret_cast<PFN_xrWaitSwapchainImage>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrReleaseSwapchainImage", &function))) {
        next_release_swapchain_image_ = reinterpret_cast<PFN_xrReleaseSwapchainImage>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateReferenceSpace", &function))) {
        next_create_reference_space_ = reinterpret_cast<PFN_xrCreateReferenceSpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateActionSpace", &function))) {
        next_create_action_space_ = reinterpret_cast<PFN_xrCreateActionSpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrDestroySpace", &function))) {
        next_destroy_space_ = reinterpret_cast<PFN_xrDestroySpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrLocateSpace", &function))) {
        next_locate_space_ = reinterpret_cast<PFN_xrLocateSpace>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrLocateViews", &function))) {
        next_locate_views_ = reinterpret_cast<PFN_xrLocateViews>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrStringToPath", &function))) {
        next_string_to_path_ = reinterpret_cast<PFN_xrStringToPath>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateActionSet", &function))) {
        next_create_action_set_ = reinterpret_cast<PFN_xrCreateActionSet>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrDestroyActionSet", &function))) {
        next_destroy_action_set_ = reinterpret_cast<PFN_xrDestroyActionSet>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrCreateAction", &function))) {
        next_create_action_ = reinterpret_cast<PFN_xrCreateAction>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrDestroyAction", &function))) {
        next_destroy_action_ = reinterpret_cast<PFN_xrDestroyAction>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrSuggestInteractionProfileBindings", &function))) {
        next_suggest_interaction_profile_bindings_ =
            reinterpret_cast<PFN_xrSuggestInteractionProfileBindings>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetActionStatePose", &function))) {
        next_get_action_state_pose_ = reinterpret_cast<PFN_xrGetActionStatePose>(function);
    }
}

void OpenXrLayer::ResetSessionState() {
    ResetD3D11QuadViewsCompositor();
    active_session_ = XR_NULL_HANDLE;
    pending_end_frame_diagnostics_ = 0;
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    internal_local_space_ = XR_NULL_HANDLE;
    internal_view_space_ = XR_NULL_HANDLE;
    internal_stage_space_ = XR_NULL_HANDLE;
    quadviews_action_set_ = XR_NULL_HANDLE;
    quadviews_eye_gaze_action_ = XR_NULL_HANDLE;
    quadviews_eye_gaze_space_ = XR_NULL_HANDLE;
    eye_gaze_interaction_profile_path_ = XR_NULL_PATH;
    eye_gaze_pose_path_ = XR_NULL_PATH;
    eye_gaze_resources_ready_ = false;
    eye_gaze_action_set_attached_ = false;
    quadviews_smoothed_focus_yaw_radians_ = 0.0;
    quadviews_smoothed_focus_pitch_radians_ = 0.0;
    quadviews_last_focus_smoothing_wall_time_.reset();
    active_primary_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    active_runtime_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    has_active_primary_view_configuration_ = false;
    has_logged_quad_view_short_count_ = false;
    has_logged_pivotxr_spike_mode_ = false;
    has_logged_quadviews_view_configuration_capabilities_ = false;
    tracked_view_spaces_.clear();
    tracked_local_spaces_.clear();
    tracked_stage_spaces_.clear();
    cached_eye_offset_poses_.clear();
    cached_pivot_pose_deltas_.clear();
    tracked_swapchains_.clear();
}

void OpenXrLayer::ResetInstanceState() {
    quad_views_extension_requested_ = false;
    varjo_foveated_rendering_extension_requested_ = false;
    eye_gaze_extension_enabled_ = false;
}

bool OpenXrLayer::IsQuadViewsActive() const {
    return resolved_settings_.core.enabled && resolved_settings_.quadviews.enabled;
}

XrResult OpenXrLayer::CreateEyeGazeResources(XrSession session) {
    if (!eye_gaze_extension_enabled_ || !next_string_to_path_ || !next_create_action_set_ || !next_create_action_ ||
        !next_suggest_interaction_profile_bindings_ || !next_create_action_space_) {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    XrResult result = next_string_to_path_(
        instance_, "/interaction_profiles/ext/eye_gaze_interaction", &eye_gaze_interaction_profile_path_);
    if (XR_FAILED(result)) {
        return result;
    }
    result = next_string_to_path_(instance_, "/user/eyes_ext/input/gaze_ext/pose", &eye_gaze_pose_path_);
    if (XR_FAILED(result)) {
        return result;
    }

    XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    CopyName(action_set_info.actionSetName, sizeof(action_set_info.actionSetName), "vectorxr_quadviews");
    CopyName(action_set_info.localizedActionSetName,
             sizeof(action_set_info.localizedActionSetName),
             "VectorXR Quadviews");
    action_set_info.priority = 0;
    result = next_create_action_set_(instance_, &action_set_info, &quadviews_action_set_);
    if (XR_FAILED(result)) {
        return result;
    }

    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    CopyName(action_info.actionName, sizeof(action_info.actionName), "eye_gaze");
    CopyName(action_info.localizedActionName, sizeof(action_info.localizedActionName), "Eye Gaze");
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    result = next_create_action_(quadviews_action_set_, &action_info, &quadviews_eye_gaze_action_);
    if (XR_FAILED(result)) {
        return result;
    }

    const XrActionSuggestedBinding suggested_binding{quadviews_eye_gaze_action_, eye_gaze_pose_path_};
    XrInteractionProfileSuggestedBinding profile_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    profile_bindings.interactionProfile = eye_gaze_interaction_profile_path_;
    profile_bindings.countSuggestedBindings = 1;
    profile_bindings.suggestedBindings = &suggested_binding;
    result = next_suggest_interaction_profile_bindings_(instance_, &profile_bindings);
    if (XR_FAILED(result)) {
        return result;
    }

    XrActionSpaceCreateInfo action_space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    action_space_info.action = quadviews_eye_gaze_action_;
    action_space_info.poseInActionSpace = IdentityPose();
    result = next_create_action_space_(session, &action_space_info, &quadviews_eye_gaze_space_);
    if (XR_FAILED(result)) {
        return result;
    }

    eye_gaze_resources_ready_ = true;
    eye_gaze_action_set_attached_ = false;
    logger_.Info("Created VectorXR eye-gaze action resources for quadviews.");
    return XR_SUCCESS;
}

void OpenXrLayer::DestroyEyeGazeResources() {
    const XrSpace eye_gaze_space = quadviews_eye_gaze_space_;
    const XrAction eye_gaze_action = quadviews_eye_gaze_action_;
    const XrActionSet action_set = quadviews_action_set_;

    quadviews_eye_gaze_space_ = XR_NULL_HANDLE;
    quadviews_eye_gaze_action_ = XR_NULL_HANDLE;
    quadviews_action_set_ = XR_NULL_HANDLE;
    eye_gaze_interaction_profile_path_ = XR_NULL_PATH;
    eye_gaze_pose_path_ = XR_NULL_PATH;
    eye_gaze_resources_ready_ = false;
    eye_gaze_action_set_attached_ = false;
    quadviews_smoothed_focus_yaw_radians_ = 0.0;
    quadviews_smoothed_focus_pitch_radians_ = 0.0;
    quadviews_last_focus_smoothing_wall_time_.reset();

    if (eye_gaze_space != XR_NULL_HANDLE && next_destroy_space_) {
        next_destroy_space_(eye_gaze_space);
    }
    if (eye_gaze_action != XR_NULL_HANDLE && next_destroy_action_) {
        next_destroy_action_(eye_gaze_action);
    }
    if (action_set != XR_NULL_HANDLE && next_destroy_action_set_) {
        next_destroy_action_set_(action_set);
    }
}

bool OpenXrLayer::LocateEyeGazeFocusOffsets(XrSession session,
                                            XrSpace base_space,
                                            XrTime time,
                                            const QuadViewsResolvedSettings& settings,
                                            double* yaw_radians,
                                            double* pitch_radians) {
    if (!yaw_radians || !pitch_radians) {
        return false;
    }

    *yaw_radians = 0.0;
    *pitch_radians = 0.0;
    if (settings.tracking_mode != QuadViewsTrackingMode::Eye || !settings.prefer_eye_tracking ||
        session != active_session_ || base_space == XR_NULL_HANDLE || !eye_gaze_resources_ready_ ||
        !eye_gaze_action_set_attached_ || quadviews_eye_gaze_action_ == XR_NULL_HANDLE ||
        quadviews_eye_gaze_space_ == XR_NULL_HANDLE || !next_get_action_state_pose_ || !next_locate_space_) {
        return false;
    }

    XrActionStateGetInfo action_state_info{XR_TYPE_ACTION_STATE_GET_INFO};
    action_state_info.action = quadviews_eye_gaze_action_;
    XrActionStatePose action_state{XR_TYPE_ACTION_STATE_POSE};
    const XrResult state_result = next_get_action_state_pose_(session, &action_state_info, &action_state);
    if (XR_FAILED(state_result) || !action_state.isActive) {
        return false;
    }

    const XrSpace gaze_base_space = internal_view_space_ != XR_NULL_HANDLE ? internal_view_space_ : base_space;

    XrSpaceLocation gaze_location{XR_TYPE_SPACE_LOCATION};
    const XrResult locate_result = next_locate_space_(quadviews_eye_gaze_space_, gaze_base_space, time, &gaze_location);
    if (XR_FAILED(locate_result) ||
        (gaze_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        return false;
    }

    const ViewOrientation gaze_orientation{
        gaze_location.pose.orientation.x,
        gaze_location.pose.orientation.y,
        gaze_location.pose.orientation.z,
        gaze_location.pose.orientation.w,
    };
    double target_yaw = ExtractYawRadians(gaze_orientation);
    double target_pitch = ExtractPitchRadians(gaze_orientation);
    const double deadzone_radians = DegreesToRadians(std::max(0.0, settings.gaze_deadzone_degrees));
    if (std::abs(target_yaw) < deadzone_radians) {
        target_yaw = 0.0;
    }
    if (std::abs(target_pitch) < deadzone_radians) {
        target_pitch = 0.0;
    }

    const auto now = std::chrono::steady_clock::now();
    double delta_seconds = 0.0;
    if (quadviews_last_focus_smoothing_wall_time_.has_value()) {
        delta_seconds = std::chrono::duration<double>(now - *quadviews_last_focus_smoothing_wall_time_).count();
    }
    quadviews_last_focus_smoothing_wall_time_ = now;
    const double blend = ComputeTimeBasedBlend(settings.gaze_smoothing, delta_seconds);
    quadviews_smoothed_focus_yaw_radians_ += (target_yaw - quadviews_smoothed_focus_yaw_radians_) * blend;
    quadviews_smoothed_focus_pitch_radians_ += (target_pitch - quadviews_smoothed_focus_pitch_radians_) * blend;
    if (NearlyZero(quadviews_smoothed_focus_yaw_radians_)) {
        quadviews_smoothed_focus_yaw_radians_ = 0.0;
    }
    if (NearlyZero(quadviews_smoothed_focus_pitch_radians_)) {
        quadviews_smoothed_focus_pitch_radians_ = 0.0;
    }

    *yaw_radians = quadviews_smoothed_focus_yaw_radians_;
    *pitch_radians = quadviews_smoothed_focus_pitch_radians_;
    return true;
}

XrResult OpenXrLayer::CreateInternalReferenceSpaces(XrSession session) {
    if (!next_create_reference_space_) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    create_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_SUCCEEDED(next_create_reference_space_(session, &create_info, &internal_local_space_))) {
        tracked_local_spaces_.insert(internal_local_space_);
    }

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    const XrResult view_result = next_create_reference_space_(session, &create_info, &internal_view_space_);
    if (XR_FAILED(view_result)) {
        internal_view_space_ = XR_NULL_HANDLE;
        logger_.Error("Failed to create internal VIEW reference space for PivotXR.");
        return view_result;
    }
    tracked_view_spaces_.insert(internal_view_space_);

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_SUCCEEDED(next_create_reference_space_(session, &create_info, &internal_stage_space_))) {
        tracked_stage_spaces_.insert(internal_stage_space_);
    } else {
        internal_stage_space_ = XR_NULL_HANDLE;
    }

    return XR_SUCCESS;
}

void OpenXrLayer::DestroyInternalReferenceSpaces() {
    if (!next_destroy_space_) {
        internal_local_space_ = XR_NULL_HANDLE;
        internal_view_space_ = XR_NULL_HANDLE;
        internal_stage_space_ = XR_NULL_HANDLE;
        tracked_local_spaces_.clear();
        tracked_view_spaces_.clear();
        tracked_stage_spaces_.clear();
        cached_eye_offset_poses_.clear();
        cached_pivot_pose_deltas_.clear();
        return;
    }

    const XrSpace local_space = internal_local_space_;
    const XrSpace view_space = internal_view_space_;
    const XrSpace stage_space = internal_stage_space_;

    internal_local_space_ = XR_NULL_HANDLE;
    internal_view_space_ = XR_NULL_HANDLE;
    internal_stage_space_ = XR_NULL_HANDLE;

    if (local_space != XR_NULL_HANDLE) {
        tracked_local_spaces_.erase(local_space);
        next_destroy_space_(local_space);
    }
    if (view_space != XR_NULL_HANDLE) {
        tracked_view_spaces_.erase(view_space);
        next_destroy_space_(view_space);
    }
    if (stage_space != XR_NULL_HANDLE) {
        tracked_stage_spaces_.erase(stage_space);
        next_destroy_space_(stage_space);
    }

    cached_eye_offset_poses_.clear();
    cached_pivot_pose_deltas_.clear();
}

bool OpenXrLayer::EnsureEyeOffsets(XrSession session,
                                   XrViewConfigurationType view_configuration_type,
                                   XrTime display_time,
                                   uint32_t view_count) {
    if (internal_view_space_ == XR_NULL_HANDLE || !next_locate_views_ || view_count == 0) {
        return false;
    }

    std::vector<XrView> eye_views(view_count);
    for (XrView& eye_view : eye_views) {
        eye_view = {XR_TYPE_VIEW};
    }
    XrViewState eye_view_state{XR_TYPE_VIEW_STATE};
    uint32_t eye_view_count = 0;
    XrViewLocateInfo eye_view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    eye_view_locate_info.viewConfigurationType = view_configuration_type;
    eye_view_locate_info.displayTime = display_time;
    eye_view_locate_info.space = internal_view_space_;

    const XrResult result =
        next_locate_views_(session, &eye_view_locate_info, &eye_view_state, view_count, &eye_view_count, eye_views.data());
    if (XR_FAILED(result) || eye_view_count < view_count) {
        logger_.Error("Failed to capture internal eye offsets for PivotXR view-space recomposition.");
        return false;
    }

    cached_eye_offset_poses_.resize(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        cached_eye_offset_poses_[i] = eye_views[i].pose;
    }
    return true;
}

void OpenXrLayer::CachePivotPoseDelta(XrTime time, const XrPosef& pose_delta) {
    cached_pivot_pose_deltas_[time] = pose_delta;
}

bool OpenXrLayer::FindPivotPoseDelta(XrTime time, XrPosef* pose_delta, XrTime* matched_time) const {
    if (!pose_delta || !matched_time) {
        return false;
    }

    if (cached_pivot_pose_deltas_.empty()) {
        *pose_delta = IdentityPose();
        *matched_time = 0;
        return false;
    }

    auto exact = cached_pivot_pose_deltas_.find(time);
    if (exact != cached_pivot_pose_deltas_.end()) {
        *pose_delta = exact->second;
        *matched_time = exact->first;
        return true;
    }

    auto upper = cached_pivot_pose_deltas_.lower_bound(time);
    std::map<XrTime, XrPosef>::const_iterator best;
    if (upper == cached_pivot_pose_deltas_.begin()) {
        best = upper;
    } else if (upper == cached_pivot_pose_deltas_.end()) {
        best = std::prev(upper);
    } else {
        auto lower = std::prev(upper);
        best = (time - lower->first <= upper->first - time) ? lower : upper;
    }

    const XrTime match_delta = best->first > time ? best->first - time : time - best->first;
    if (match_delta > kMaxPivotPoseDeltaMatchWindow) {
        *pose_delta = IdentityPose();
        *matched_time = best->first;
        return false;
    }

    *pose_delta = best->second;
    *matched_time = best->first;
    return true;
}

void OpenXrLayer::PrunePivotPoseDeltas(XrTime time) {
    auto keep_from = cached_pivot_pose_deltas_.upper_bound(time);
    if (keep_from == cached_pivot_pose_deltas_.begin()) {
        return;
    }

    cached_pivot_pose_deltas_.erase(cached_pivot_pose_deltas_.begin(), keep_from);
}

bool OpenXrLayer::IsTrackedViewSpace(XrSpace space) const {
    return space != XR_NULL_HANDLE && tracked_view_spaces_.contains(space);
}

XrResult OpenXrLayer::LocateRuntimeViews(XrSession session,
                                         const XrViewLocateInfo* view_locate_info,
                                         XrViewState* view_state,
                                         uint32_t view_capacity_input,
                                         uint32_t* view_count_output,
                                         XrView* views,
                                         bool* synthesized_quad_views) {
    if (synthesized_quad_views) {
        *synthesized_quad_views = false;
    }

    bool quadviews_active = false;
    QuadViewsResolvedSettings quadviews_settings;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        quadviews_active = IsQuadViewsActive();
        quadviews_settings = resolved_settings_.quadviews;
    }

    if (!view_locate_info || !IsQuadViewConfiguration(view_locate_info->viewConfigurationType) || !quadviews_active) {
        return next_locate_views_(
            session, view_locate_info, view_state, view_capacity_input, view_count_output, views);
    }

    XrViewLocateInfo runtime_locate_info = *view_locate_info;
    runtime_locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    std::array<XrView, 2> stereo_views{};
    for (XrView& view : stereo_views) {
        view = {XR_TYPE_VIEW};
    }
    XrViewState runtime_view_state{XR_TYPE_VIEW_STATE};
    XrViewState* downstream_view_state = view_state ? &runtime_view_state : nullptr;
    uint32_t stereo_count = 0;
    const XrResult result = next_locate_views_(session,
                                              &runtime_locate_info,
                                              downstream_view_state,
                                              static_cast<uint32_t>(stereo_views.size()),
                                              &stereo_count,
                                              stereo_views.data());
    if (view_state && XR_SUCCEEDED(result)) {
        *view_state = runtime_view_state;
    }
    if (XR_FAILED(result)) {
        return result;
    }
    if (stereo_count < 2) {
        if (view_count_output) {
            *view_count_output = stereo_count;
        }
        return result;
    }

    if (!view_count_output) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *view_count_output = 4;
    if (!views || view_capacity_input == 0) {
        return XR_SUCCESS;
    }
    if (view_capacity_input < 4) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    double focus_yaw_radians = 0.0;
    double focus_pitch_radians = 0.0;
    {
        std::scoped_lock lock(mutex_);
        const bool has_eye_focus = LocateEyeGazeFocusOffsets(session,
                                                            view_locate_info->space,
                                                            view_locate_info->displayTime,
                                                            quadviews_settings,
                                                            &focus_yaw_radians,
                                                            &focus_pitch_radians);
        if (!has_eye_focus) {
            quadviews_smoothed_focus_yaw_radians_ = 0.0;
            quadviews_smoothed_focus_pitch_radians_ = 0.0;
            quadviews_last_focus_smoothing_wall_time_.reset();
        }
    }

    if (!SynthesizeQuadViewsFromStereo(stereo_views,
                                       quadviews_settings,
                                       focus_yaw_radians,
                                       focus_pitch_radians,
                                       view_capacity_input,
                                       view_count_output,
                                       views)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (synthesized_quad_views) {
        *synthesized_quad_views = true;
    }
    return XR_SUCCESS;
}

bool OpenXrLayer::SynthesizeQuadViewsFromStereo(std::span<const XrView> stereo_views,
                                                const QuadViewsResolvedSettings& quadviews_settings,
                                                double focus_yaw_radians,
                                                double focus_pitch_radians,
                                                uint32_t view_capacity_input,
                                                uint32_t* view_count_output,
                                                XrView* views) const {
    if (stereo_views.size() < 2 || !views || !view_count_output || view_capacity_input < 4) {
        return false;
    }

    *view_count_output = 4;
    views[0] = stereo_views[0];
    views[1] = stereo_views[1];
    views[2] = stereo_views[0];
    views[3] = stereo_views[1];

    views[2].fov = BuildFocusFov(stereo_views[0].fov, quadviews_settings, focus_yaw_radians, focus_pitch_radians);
    views[3].fov = BuildFocusFov(stereo_views[1].fov, quadviews_settings, focus_yaw_radians, focus_pitch_radians);
    return true;
}

XrResult OpenXrLayer::LocateSpaceWithPivot(XrSpace space,
                                           XrSpace base_space,
                                           XrTime time,
                                           const PivotXrResolvedSettings& settings,
                                           bool pivotxr_active,
                                           XrSpaceLocation* location,
                                           double* applied_extra_yaw_radians,
                                           double* applied_extra_pitch_radians,
                                           XrPosef* applied_pose_delta,
                                           bool update_smoothing) {
    if (!location) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    const XrResult result = next_locate_space_(space, base_space, time, location);
    if (applied_pose_delta) {
        *applied_pose_delta = IdentityPose();
    }
    if (XR_FAILED(result) || !settings.enabled) {
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    if (!pivotxr_active) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    const bool space_is_view = IsTrackedViewSpace(space);
    const bool base_space_is_view = IsTrackedViewSpace(base_space);
    if (space_is_view == base_space_is_view) {
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    const XrPosef original_relation_pose = location->pose;
    const XrPosef view_pose = space_is_view ? location->pose : InvertPose(location->pose);
    const ViewOrientation orientation{
        view_pose.orientation.x,
        view_pose.orientation.y,
        view_pose.orientation.z,
        view_pose.orientation.w,
    };
    double delta_seconds = 0.0;
    if (update_smoothing) {
        const auto now = std::chrono::steady_clock::now();
        if (pivotxr_last_smoothing_wall_time_.has_value()) {
            delta_seconds =
                std::chrono::duration<double>(now - *pivotxr_last_smoothing_wall_time_).count();
        }
        pivotxr_last_smoothing_wall_time_ = now;
    }

    const double current_yaw_radians = ExtractYawRadians(orientation);
    const double current_pitch_radians = ExtractPitchRadians(orientation);
    double extra_yaw_radians = pivotxr_smoothed_extra_yaw_radians_;
    double extra_pitch_radians = pivotxr_smoothed_extra_pitch_radians_;
    if (update_smoothing) {
        extra_yaw_radians = ComputePivotExtraAngleRadians(current_yaw_radians,
                                                         settings.yaw_rotation_multiplier,
                                                         settings.yaw_deadzone_degrees,
                                                         settings.yaw_max_extra_degrees,
                                                         settings.yaw_smoothing,
                                                         delta_seconds,
                                                         pivotxr_smoothed_extra_yaw_radians_);
        extra_pitch_radians = ComputePivotExtraAngleRadians(current_pitch_radians,
                                                           settings.pitch_rotation_multiplier,
                                                           settings.pitch_deadzone_degrees,
                                                           settings.pitch_max_extra_degrees,
                                                           settings.pitch_smoothing,
                                                           delta_seconds,
                                                           pivotxr_smoothed_extra_pitch_radians_);
    }
    if (applied_extra_yaw_radians) {
        *applied_extra_yaw_radians = extra_yaw_radians;
    }
    if (applied_extra_pitch_radians) {
        *applied_extra_pitch_radians = extra_pitch_radians;
    }
    if (NearlyZero(extra_yaw_radians) && NearlyZero(extra_pitch_radians)) {
        return result;
    }

    const XrPosef manipulated_pose =
        ApplyExtraRotationToPose(view_pose,
                                 static_cast<float>(extra_yaw_radians),
                                 static_cast<float>(extra_pitch_radians));
    location->pose = space_is_view ? manipulated_pose : InvertPose(manipulated_pose);
    if (applied_pose_delta && space_is_view && !base_space_is_view) {
        *applied_pose_delta = MultiplyPoses(InvertPose(original_relation_pose), location->pose);
    }
    return result;
}

void OpenXrLayer::LogResolvedSettings(const ResolvedRuntimeConfig& settings) {
    std::ostringstream stream;
    stream << "Resolved settings for " << current_exe_name_ << ": "
           << "coreEnabled=" << settings.core.enabled
           << ", logLevel=" << ToString(settings.core.log_level)
           << ", depthxrEnabled=" << settings.depthxr.enabled
           << ", depthToggleBinding=" << BindingLabel(settings.depthxr_bindings.toggle_enabled)
           << ", stereoBoostEnabled=" << settings.depthxr.stereo_boost_enabled
           << ", stereoBoost=" << settings.depthxr.stereo_boost
           << ", convergenceEnabled=" << settings.depthxr.convergence_enabled
           << ", convergence=" << settings.depthxr.convergence
           << ", pivotxrEnabled=" << settings.pivotxr.enabled
           << ", pivotActivation=" << ToString(settings.pivotxr.activation_mode)
           << ", pivotActivationBinding=" << BindingLabel(settings.pivotxr.activation_binding)
           << ", pivotYawMultiplier=" << settings.pivotxr.yaw_rotation_multiplier
           << ", pivotYawSmoothing=" << settings.pivotxr.yaw_smoothing
           << ", pivotYawDeadzone=" << settings.pivotxr.yaw_deadzone_degrees
           << ", pivotYawMaxExtra=" << settings.pivotxr.yaw_max_extra_degrees
           << ", pivotPitchMultiplier=" << settings.pivotxr.pitch_rotation_multiplier
           << ", pivotPitchSmoothing=" << settings.pivotxr.pitch_smoothing
           << ", pivotPitchDeadzone=" << settings.pivotxr.pitch_deadzone_degrees
           << ", pivotPitchMaxExtra=" << settings.pivotxr.pitch_max_extra_degrees
           << ", quadviewsEnabled=" << settings.quadviews.enabled
           << ", quadviewsPreferEyeTracking=" << settings.quadviews.prefer_eye_tracking
           << ", quadviewsTrackingMode=" << ToString(settings.quadviews.tracking_mode)
           << ", quadviewsFocusHorizontalFov=" << settings.quadviews.focus_horizontal_fov_degrees
           << ", quadviewsFocusVerticalFov=" << settings.quadviews.focus_vertical_fov_degrees
           << ", quadviewsFocusScale=" << settings.quadviews.focus_scale
           << ", quadviewsPeripheralScale=" << settings.quadviews.peripheral_scale
           << ", quadviewsHorizontalOffset=" << settings.quadviews.horizontal_offset_degrees
           << ", quadviewsVerticalOffset=" << settings.quadviews.vertical_offset_degrees
           << ", quadviewsGazeSmoothing=" << settings.quadviews.gaze_smoothing
           << ", quadviewsGazeDeadzone=" << settings.quadviews.gaze_deadzone_degrees;
    logger_.Debug(stream.str());
}

void OpenXrLayer::ResetPivotActivationState() {
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    pivotxr_toggle_enabled_ = false;
    pivotxr_activation_key_was_down_ = false;
}

void OpenXrLayer::ResetDepthToggleState() {
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
}

void OpenXrLayer::ResetSwapchainState() {
    tracked_swapchains_.clear();
}

void OpenXrLayer::ResetD3D11QuadViewsCompositor() {
    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            next_destroy_swapchain_(target.swapchain);
        }
        target = {};
    }

    SafeRelease(d3d11_quadviews_compositor_.constants);
    SafeRelease(d3d11_quadviews_compositor_.blend_state);
    SafeRelease(d3d11_quadviews_compositor_.sampler);
    SafeRelease(d3d11_quadviews_compositor_.pixel_shader);
    SafeRelease(d3d11_quadviews_compositor_.vertex_shader);
    SafeRelease(d3d11_quadviews_compositor_.context);
    SafeRelease(d3d11_quadviews_compositor_.device);
    d3d11_quadviews_compositor_.initialized = false;
    d3d11_quadviews_compositor_.failed = false;
}

void OpenXrLayer::LogSwapchainSummary(XrSwapchain swapchain,
                                      const SwapchainInfo& info,
                                      std::string_view event_name) {
    std::ostringstream stream;
    stream << "Swapchain " << event_name
           << ": handle=" << FormatHandle(swapchain)
           << ", sessionActive=" << (info.session == active_session_)
           << ", quadviewsSession=" << info.quadviews_session
           << ", activeViewConfig=" << ToString(active_primary_view_configuration_type_)
           << ", size=" << info.width << "x" << info.height
           << ", arraySize=" << info.array_size
           << ", mipCount=" << info.mip_count
           << ", sampleCount=" << info.sample_count
           << ", format=" << info.format
           << ", usage=" << FormatUsageFlags(info.usage_flags)
           << ", createFlags=" << info.create_flags
           << ", imageCount=" << info.image_count
           << ", imagesEnumerated=" << info.images_enumerated
           << ", d3d11Images=" << info.d3d11_images.size()
           << ", acquires=" << info.acquire_count
           << ", waits=" << info.wait_count
           << ", releases=" << info.release_count
           << ", trackedSwapchains=" << tracked_swapchains_.size();
    if (info.quadviews_session) {
        logger_.Info(stream.str());
    } else {
        logger_.Debug(stream.str());
    }
}

bool OpenXrLayer::IsDepthXrActive() {
    if (!resolved_settings_.depthxr.enabled) {
        ResetDepthToggleState();
        return false;
    }

#if defined(_WIN32)
    const bool binding_down = IsInputBindingDown(resolved_settings_.depthxr_bindings.toggle_enabled);
    const bool was_pressed_this_call = binding_down && !depthxr_toggle_binding_was_down_;
    depthxr_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        depthxr_toggle_enabled_ = !depthxr_toggle_enabled_;
        pending_locate_views_diagnostics_ = 5;
        logger_.Info(std::string("Depth effects ") +
                     (depthxr_toggle_enabled_ ? "enabled" : "disabled") + " via " +
                     BindingLabel(resolved_settings_.depthxr_bindings.toggle_enabled) + ".");
    }

    return depthxr_toggle_enabled_;
#else
    return resolved_settings_.depthxr.enabled;
#endif
}

bool OpenXrLayer::IsPivotXrActive(const PivotXrResolvedSettings& settings) {
    if (!settings.enabled) {
        ResetPivotActivationState();
        return false;
    }

#if defined(_WIN32)
    const bool binding_down = IsInputBindingDown(settings.activation_binding);
    const bool was_pressed_this_call = binding_down && !pivotxr_activation_key_was_down_;
    pivotxr_activation_key_was_down_ = binding_down;

    if (settings.activation_mode == ActivationMode::Toggle) {
        if (was_pressed_this_call) {
            pivotxr_toggle_enabled_ = !pivotxr_toggle_enabled_;
            pending_locate_views_diagnostics_ = 5;
            pending_end_frame_diagnostics_ = 5;
            logger_.Info(std::string("PivotXR extra pivot factor ") +
                         (pivotxr_toggle_enabled_ ? "enabled" : "disabled") + " via " +
                         BindingLabel(settings.activation_binding) + ".");
        }
        return pivotxr_toggle_enabled_;
    }

    return binding_down;
#else
    return settings.enabled;
#endif
}

} // namespace depthxr
