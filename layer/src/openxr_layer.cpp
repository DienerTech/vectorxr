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

double SmoothStep(double t) {
    const double clamped = Clamp(t, 0.0, 1.0);
    return clamped * clamped * (3.0 - 2.0 * clamped);
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

double QuadViewsFocusWidthScale(const QuadViewsResolvedSettings& settings) {
    return settings.focus_scale * Clamp(settings.focus_horizontal_size_percent, 1.0, 100.0) / 100.0;
}

double QuadViewsFocusHeightScale(const QuadViewsResolvedSettings& settings) {
    return settings.focus_scale * Clamp(settings.focus_vertical_size_percent, 1.0, 100.0) / 100.0;
}

uint32_t EstimateFullResolutionDimension(uint32_t rendered_dimension, double render_scale) {
    const double scale = std::max(0.01, render_scale);
    return std::max<uint32_t>(1, static_cast<uint32_t>(std::round(static_cast<double>(rendered_dimension) / scale)));
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

AngularWindow ClampProjectedWindow(float base_negative,
                                   float base_positive,
                                   double center_radians,
                                   double size_percent) {
    const double base_negative_tan = std::tan(base_negative);
    const double base_positive_tan = std::tan(base_positive);
    const double base_size_tan = std::max(0.0001, base_positive_tan - base_negative_tan);
    const double window_size_tan = base_size_tan * Clamp(size_percent, 1.0, 100.0) / 100.0;
    const double half_window_tan = window_size_tan * 0.5;
    double negative_tan = std::tan(center_radians) - half_window_tan;
    double positive_tan = std::tan(center_radians) + half_window_tan;

    if (window_size_tan >= base_size_tan) {
        negative_tan = base_negative_tan;
        positive_tan = base_positive_tan;
    } else {
        if (negative_tan < base_negative_tan) {
            positive_tan += base_negative_tan - negative_tan;
            negative_tan = base_negative_tan;
        }
        if (positive_tan > base_positive_tan) {
            negative_tan -= positive_tan - base_positive_tan;
            positive_tan = base_positive_tan;
        }
        negative_tan = Clamp(negative_tan, base_negative_tan, base_positive_tan);
        positive_tan = Clamp(positive_tan, base_negative_tan, base_positive_tan);
    }

    return {std::atan(negative_tan), std::atan(positive_tan)};
}

XrFovf BuildFocusFov(const XrFovf& base_fov,
                     const QuadViewsResolvedSettings& settings,
                     double focus_yaw_radians,
                     double focus_pitch_radians) {
    const double horizontal_center = DegreesToRadians(settings.horizontal_offset_degrees) + focus_yaw_radians;
    const double vertical_center = DegreesToRadians(settings.vertical_offset_degrees) + focus_pitch_radians;

    const AngularWindow horizontal =
        ClampProjectedWindow(base_fov.angleLeft,
                             base_fov.angleRight,
                             horizontal_center,
                             settings.focus_horizontal_size_percent);
    const AngularWindow vertical =
        ClampProjectedWindow(base_fov.angleDown,
                             base_fov.angleUp,
                             vertical_center,
                             settings.focus_vertical_size_percent);

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

template <typename T>
void SafeReleaseVector(std::vector<T*>& pointers) {
    for (T*& pointer : pointers) {
        SafeRelease(pointer);
    }
    pointers.clear();
}

bool IsTypelessFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT ResolveViewFormat(DXGI_FORMAT texture_format, int64_t fallback_format) {
    const DXGI_FORMAT fallback = static_cast<DXGI_FORMAT>(fallback_format);
    if (!IsTypelessFormat(texture_format)) {
        return texture_format;
    }
    if (fallback != DXGI_FORMAT_UNKNOWN && !IsTypelessFormat(fallback)) {
        return fallback;
    }

    switch (texture_format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return texture_format;
    }
}

std::string FormatTextureDesc(const D3D11_TEXTURE2D_DESC& desc) {
    std::ostringstream stream;
    stream << "desc={size=" << desc.Width << "x" << desc.Height
           << ", mipLevels=" << desc.MipLevels
           << ", arraySize=" << desc.ArraySize
           << ", format=" << static_cast<uint32_t>(desc.Format)
           << ", sampleCount=" << desc.SampleDesc.Count
           << ", sampleQuality=" << desc.SampleDesc.Quality
           << ", usage=" << desc.Usage
           << ", bindFlags=" << desc.BindFlags
           << ", cpuAccessFlags=" << desc.CPUAccessFlags
           << ", miscFlags=" << desc.MiscFlags
           << "}";
    return stream.str();
}

HRESULT CreateTextureShaderResourceView(ID3D11Device* device,
                                        ID3D11Texture2D* texture,
                                        int64_t fallback_format,
                                        ID3D11ShaderResourceView** shader_resource) {
    if (!device || !texture || !shader_resource) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);
    if ((texture_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || texture_desc.ArraySize != 1 ||
        texture_desc.MipLevels == 0) {
        return E_INVALIDARG;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = ResolveViewFormat(texture_desc.Format, fallback_format);
    if (texture_desc.SampleDesc.Count > 1) {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MostDetailedMip = 0;
        view_desc.Texture2D.MipLevels = 1;
    }

    return device->CreateShaderResourceView(texture, &view_desc, shader_resource);
}

HRESULT CreateTextureRenderTargetView(ID3D11Device* device,
                                      ID3D11Texture2D* texture,
                                      int64_t fallback_format,
                                      ID3D11RenderTargetView** render_target_view) {
    if (!device || !texture || !render_target_view) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);
    if ((texture_desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || texture_desc.ArraySize != 1) {
        return E_INVALIDARG;
    }

    D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
    view_desc.Format = ResolveViewFormat(texture_desc.Format, fallback_format);
    if (texture_desc.SampleDesc.Count > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
    }

    return device->CreateRenderTargetView(texture, &view_desc, render_target_view);
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
    float blend_params[4];
    float focus_texel[4];
};

FocusRectConstants BuildFocusRectConstants(const XrFovf& full_fov,
                                           const XrFovf& focus_fov,
                                           uint32_t width,
                                           uint32_t height,
                                           double transition_thickness_percent,
                                           double foveate_sharpness) {
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

    const double focus_rect_width =
        std::max(0.0001, static_cast<double>(constants.focus_rect[2]) - constants.focus_rect[0]);
    const double focus_rect_height =
        std::max(0.0001, static_cast<double>(constants.focus_rect[3]) - constants.focus_rect[1]);
    const double transition_fraction = Clamp(transition_thickness_percent, 0.0, 100.0) / 100.0;
    constants.blend_params[0] = static_cast<float>(std::max(1.0 / std::max<uint32_t>(1, width),
                                                           focus_rect_width * transition_fraction));
    constants.blend_params[1] = static_cast<float>(std::max(1.0 / std::max<uint32_t>(1, height),
                                                           focus_rect_height * transition_fraction));
    constants.blend_params[2] = static_cast<float>(Clamp(foveate_sharpness, 0.0, 100.0) / 100.0);
    constants.blend_params[3] = 0.0f;
    constants.focus_texel[0] = 1.0f / static_cast<float>(std::max<uint32_t>(1, width));
    constants.focus_texel[1] = 1.0f / static_cast<float>(std::max<uint32_t>(1, height));
    return constants;
}

const char* D3D11QuadViewsShaderSource() {
    return R"(
cbuffer QuadViewsConstants : register(b0) {
    float4 focusRect;
    float4 blendParams;
    float4 focusTexel;
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
    float sharpenAmount = saturate(blendParams.z) * 0.35;
    if (sharpenAmount > 0.001) {
        float2 texel = focusTexel.xy;
        float4 blur =
            focusTexture.Sample(linearSampler, saturate(focusUv + float2(texel.x, 0.0))) +
            focusTexture.Sample(linearSampler, saturate(focusUv - float2(texel.x, 0.0))) +
            focusTexture.Sample(linearSampler, saturate(focusUv + float2(0.0, texel.y))) +
            focusTexture.Sample(linearSampler, saturate(focusUv - float2(0.0, texel.y)));
        blur *= 0.25;
        focus.rgb = saturate(focus.rgb + (focus.rgb - blur.rgb) * sharpenAmount);
    }

    float2 distToEdge = min(uv - focusRect.xy, focusRect.zw - uv);
    float edgeAlpha = saturate(min(distToEdge.x / max(blendParams.x, 0.0001), distToEdge.y / max(blendParams.y, 0.0001)));
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

std::string FormatHex(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::string FormatFov(const XrFovf& fov) {
    std::ostringstream stream;
    stream << "(" << FormatDiagnosticDouble(fov.angleLeft) << ", " << FormatDiagnosticDouble(fov.angleRight)
           << ", " << FormatDiagnosticDouble(fov.angleUp) << ", " << FormatDiagnosticDouble(fov.angleDown) << ")";
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

struct GazeRayAngles {
    double yaw_radians{0.0};
    double pitch_radians{0.0};
    XrVector3f forward{0.0f, 0.0f, -1.0f};
};

GazeRayAngles ExtractGazeRayAngles(const XrQuaternionf& orientation) {
    const XrQuaternionf normalized_orientation = NormalizeQuaternion(orientation);
    XrVector3f forward = RotateVector(normalized_orientation, {0.0f, 0.0f, -1.0f});
    const double magnitude = std::sqrt(static_cast<double>(forward.x) * forward.x +
                                       static_cast<double>(forward.y) * forward.y +
                                       static_cast<double>(forward.z) * forward.z);
    if (magnitude > 0.000001) {
        const float scale = static_cast<float>(1.0 / magnitude);
        forward.x *= scale;
        forward.y *= scale;
        forward.z *= scale;
    } else {
        forward = {0.0f, 0.0f, -1.0f};
    }

    const double forward_depth = std::max(0.000001, static_cast<double>(-forward.z));
    return {
        std::atan2(static_cast<double>(forward.x), forward_depth),
        std::atan2(static_cast<double>(forward.y), forward_depth),
        forward,
    };
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

// Duration over which the pivot effect eases in/out when activation toggles.
// Decouples the on/off feel from the per-frame tracking smoothing so enabling
// pivot while the head is already turned never snaps the view.
constexpr double kPivotActivationRampSeconds = 0.35;
constexpr double kPivotActivationGainEpsilon = 0.0001;
constexpr XrTime kMaxPivotPoseDeltaMatchWindow = 5'000'000;
constexpr XrTime kMaxQuadViewsFovMatchWindow = 5'000'000;
constexpr size_t kMaxCachedQuadViewsFovFrames = 180;
// Hot-path throttles. xrLocateSpace/xrLocateViews/xrEndFrame run multiple
// times per frame; filesystem stats, input-device polls, and downstream
// helper calls must not.
constexpr std::chrono::milliseconds kConfigCheckInterval{500};
constexpr std::chrono::milliseconds kInputBindingPollInterval{30};
constexpr std::chrono::milliseconds kAppActionSyncFreshWindow{100};

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
           lhs.quadviews.tracking_mode == rhs.quadviews.tracking_mode &&
           NearlyEqual(lhs.quadviews.focus_horizontal_size_percent, rhs.quadviews.focus_horizontal_size_percent) &&
           NearlyEqual(lhs.quadviews.focus_vertical_size_percent, rhs.quadviews.focus_vertical_size_percent) &&
           NearlyEqual(lhs.quadviews.focus_scale, rhs.quadviews.focus_scale) &&
           NearlyEqual(lhs.quadviews.peripheral_scale, rhs.quadviews.peripheral_scale) &&
           NearlyEqual(lhs.quadviews.foveate_sharpness, rhs.quadviews.foveate_sharpness) &&
           NearlyEqual(lhs.quadviews.transition_thickness_percent, rhs.quadviews.transition_thickness_percent) &&
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
    if (IsQuadViewsActive() && resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye) {
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
            pending_eye_gaze_diagnostics_ = std::max(pending_eye_gaze_diagnostics_, 20u);
            pending_eye_gaze_sync_diagnostics_ = std::max(pending_eye_gaze_sync_diagnostics_, 20u);
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
    bool appended_eye_gaze_active_set = false;
    uint32_t submitted_action_set_count = sync_info->countActiveActionSets;
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
                appended_eye_gaze_active_set = true;
                downstream_sync_info.countActiveActionSets = static_cast<uint32_t>(active_action_sets.size());
                downstream_sync_info.activeActionSets = active_action_sets.data();
            }
            submitted_action_set_count = downstream_sync_info.countActiveActionSets;
        }
    }

    const XrResult result = next_sync_actions_(session, &downstream_sync_info);
    if (XR_SUCCEEDED(result) && appended_eye_gaze_active_set) {
        std::scoped_lock lock(mutex_);
        last_app_action_sync_time_ = std::chrono::steady_clock::now();
    }
    if (pending_eye_gaze_sync_diagnostics_ > 0) {
        std::scoped_lock lock(mutex_);
        if (session == active_session_ && eye_gaze_resources_ready_) {
            logger_.Debug("Quadviews eye-gaze action sync: result=" +
                          FormatHex(static_cast<uint64_t>(result)) +
                          ", appActionSets=" + std::to_string(sync_info->countActiveActionSets) +
                          ", submittedActionSets=" + std::to_string(submitted_action_set_count) +
                          ", appended=" + std::to_string(appended_eye_gaze_active_set) +
                          ", actionSetAttached=" + std::to_string(eye_gaze_action_set_attached_));
            --pending_eye_gaze_sync_diagnostics_;
        }
    }
    return result;
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

    const double focus_width_scale = QuadViewsFocusWidthScale(resolved_settings_.quadviews);
    const double focus_height_scale = QuadViewsFocusHeightScale(resolved_settings_.quadviews);
    cached_quadviews_stereo_recommended_width_ =
        std::max(stereo_views[0].recommendedImageRectWidth, stereo_views[1].recommendedImageRectWidth);
    cached_quadviews_stereo_recommended_height_ =
        std::max(stereo_views[0].recommendedImageRectHeight, stereo_views[1].recommendedImageRectHeight);

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
                                                                focus_width_scale,
                                                                stereo_views[i].maxImageRectWidth);
        views[i + 2].recommendedImageRectHeight = ScaleDimension(stereo_views[i].recommendedImageRectHeight,
                                                                 focus_height_scale,
                                                                 stereo_views[i].maxImageRectHeight);
        SetFoveatedViewActive(views[i + 2], XR_TRUE);
    }

    const double estimated_pixel_budget =
        resolved_settings_.quadviews.peripheral_scale * resolved_settings_.quadviews.peripheral_scale +
        focus_width_scale * focus_height_scale;
    logger_.Info("Quadviews synthesized view sizes: stereoRecommended=" +
                 std::to_string(stereo_views[0].recommendedImageRectWidth) + "x" +
                 std::to_string(stereo_views[0].recommendedImageRectHeight) +
                 ", peripheralRecommended=" + std::to_string(views[0].recommendedImageRectWidth) + "x" +
                 std::to_string(views[0].recommendedImageRectHeight) +
                 ", focusRecommended=" + std::to_string(views[2].recommendedImageRectWidth) + "x" +
                 std::to_string(views[2].recommendedImageRectHeight) +
                 ", focusScaleEffective=(" + FormatDiagnosticDouble(focus_width_scale) + ", " +
                 FormatDiagnosticDouble(focus_height_scale) + ")" +
                 ", estimatedPerEyePixelBudget=" + FormatDiagnosticDouble(estimated_pixel_budget * 100.0) +
                 "% of stereo");

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
    SafeReleaseVector(it->second.d3d11_shader_resources);
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
        SafeReleaseVector(it->second.d3d11_shader_resources);
        it->second.d3d11_shader_resources_attempted = false;
        it->second.d3d11_shader_resources_available = false;
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
    if (it->second.quadviews_session) {
        TryPrewarmD3D11QuadViewsCompositor();
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

bool OpenXrLayer::EnsureD3D11QuadViewsCompositor(const XrCompositionLayerProjection* /*projection_layer*/,
                                                 uint32_t output_width,
                                                 uint32_t output_height,
                                                 int64_t output_format) {
    if (d3d11_quadviews_compositor_.failed || !d3d11_quadviews_compositor_.device ||
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

        bool gpu_timing_ready = true;
        for (QuadViewsGpuTimingQuery& query : d3d11_quadviews_compositor_.gpu_timing_queries) {
            D3D11_QUERY_DESC disjoint_desc{};
            disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            hr = d3d11_quadviews_compositor_.device->CreateQuery(&disjoint_desc, &query.disjoint);
            if (FAILED(hr)) {
                gpu_timing_ready = false;
                break;
            }

            D3D11_QUERY_DESC timestamp_desc{};
            timestamp_desc.Query = D3D11_QUERY_TIMESTAMP;
            hr = d3d11_quadviews_compositor_.device->CreateQuery(&timestamp_desc, &query.start);
            if (FAILED(hr)) {
                gpu_timing_ready = false;
                break;
            }
            hr = d3d11_quadviews_compositor_.device->CreateQuery(&timestamp_desc, &query.end);
            if (FAILED(hr)) {
                gpu_timing_ready = false;
                break;
            }
        }
        if (!gpu_timing_ready) {
            for (QuadViewsGpuTimingQuery& query : d3d11_quadviews_compositor_.gpu_timing_queries) {
                SafeRelease(query.disjoint);
                SafeRelease(query.start);
                SafeRelease(query.end);
                query = {};
            }
            logger_.Info("D3D11 quadviews compositor GPU timing queries unavailable; CPU timing remains active.");
        }
        d3d11_quadviews_compositor_.gpu_timing_available = gpu_timing_ready;

        d3d11_quadviews_compositor_.initialized = true;
        logger_.Info("D3D11 quadviews compositor initialized.");
    }

    auto release_render_resources = [](QuadViewsCompositionTarget& target) {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
    };

    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        if (target.swapchain != XR_NULL_HANDLE && target.width == output_width && target.height == output_height &&
            target.format == output_format && !target.d3d11_images.empty() && target.render_texture &&
            target.render_target_view) {
            continue;
        }

        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            release_render_resources(target);
            next_destroy_swapchain_(target.swapchain);
            target = {};
        }

        XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                 XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create_info.format = output_format;
        create_info.sampleCount = 1;
        create_info.width = output_width;
        create_info.height = output_height;
        create_info.faceCount = 1;
        create_info.arraySize = 1;
        create_info.mipCount = 1;

        XrResult result = next_create_swapchain_(active_session_, &create_info, &target.swapchain);
        if (XR_FAILED(result) || target.swapchain == XR_NULL_HANDLE) {
            const XrResult transfer_result = result;
            create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            result = next_create_swapchain_(active_session_, &create_info, &target.swapchain);
            if (XR_FAILED(result) || target.swapchain == XR_NULL_HANDLE) {
                logger_.Error("D3D11 quadviews compositor output swapchain creation failed. transferDstResult=" +
                              FormatHex(static_cast<uint64_t>(transfer_result)) +
                              ", fallbackResult=" + FormatHex(static_cast<uint64_t>(result)));
                target = {};
                d3d11_quadviews_compositor_.failed = true;
                return false;
            }
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

        target.image_render_target_views.clear();
        target.image_render_target_views.reserve(target.d3d11_images.size());
        bool direct_render_targets_ready = !target.d3d11_images.empty();
        HRESULT direct_render_target_failure = S_OK;
        uint32_t failed_direct_render_target_index = 0;
        D3D11_TEXTURE2D_DESC failed_direct_render_target_desc{};
        for (ID3D11Texture2D* texture : target.d3d11_images) {
            ID3D11RenderTargetView* render_target_view = nullptr;
            HRESULT hr = CreateTextureRenderTargetView(d3d11_quadviews_compositor_.device,
                                                       texture,
                                                       output_format,
                                                       &render_target_view);
            if (FAILED(hr)) {
                direct_render_targets_ready = false;
                direct_render_target_failure = hr;
                failed_direct_render_target_index = static_cast<uint32_t>(target.image_render_target_views.size());
                if (texture) {
                    texture->GetDesc(&failed_direct_render_target_desc);
                }
                SafeRelease(render_target_view);
                break;
            }
            target.image_render_target_views.push_back(render_target_view);
        }
        if (!direct_render_targets_ready || target.image_render_target_views.size() != target.d3d11_images.size()) {
            SafeReleaseVector(target.image_render_target_views);
            logger_.Info("D3D11 quadviews compositor direct output RTVs unavailable; using private render target copy path. "
                         "hr=" + FormatHex(static_cast<uint32_t>(direct_render_target_failure)) +
                         ", failedImage=" + std::to_string(failed_direct_render_target_index) +
                         ", format=" + std::to_string(output_format) +
                         ", size=" + std::to_string(output_width) + "x" + std::to_string(output_height) +
                         ", " + FormatTextureDesc(failed_direct_render_target_desc));
        }

        D3D11_TEXTURE2D_DESC render_desc{};
        render_desc.Width = output_width;
        render_desc.Height = output_height;
        render_desc.MipLevels = 1;
        render_desc.ArraySize = 1;
        render_desc.Format = static_cast<DXGI_FORMAT>(output_format);
        render_desc.SampleDesc.Count = 1;
        render_desc.Usage = D3D11_USAGE_DEFAULT;
        render_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11_quadviews_compositor_.device->CreateTexture2D(&render_desc, nullptr, &target.render_texture);
        std::string d3d_step = "CreatePrivateRenderTexture";
        if (SUCCEEDED(hr)) {
            hr = d3d11_quadviews_compositor_.device->CreateRenderTargetView(
                target.render_texture,
                nullptr,
                &target.render_target_view);
            d3d_step = "CreatePrivateRenderTargetView";
        }
        if (FAILED(hr)) {
            logger_.Error("D3D11 quadviews compositor private render target creation failed. step=" + d3d_step +
                          ", hr=" + FormatHex(static_cast<uint32_t>(hr)) +
                          ", format=" + std::to_string(output_format) +
                          ", size=" + std::to_string(output_width) + "x" + std::to_string(output_height));
            release_render_resources(target);
            if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
                next_destroy_swapchain_(target.swapchain);
            }
            target = {};
            d3d11_quadviews_compositor_.failed = true;
            return false;
        }

        logger_.Debug("D3D11 quadviews compositor output swapchain ready: size=" +
                      std::to_string(target.width) + "x" + std::to_string(target.height) +
                      ", images=" + std::to_string(target.image_count) +
                      ", directOutputRtvs=" + std::to_string(target.image_render_target_views.size()) +
                      ", privateRenderTarget=1");
    }

    return true;
}

bool OpenXrLayer::EnsureD3D11SwapchainShaderResources(SwapchainInfo& swapchain) {
    if (!d3d11_quadviews_compositor_.device ||
        (swapchain.usage_flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) == 0 ||
        swapchain.d3d11_images.empty()) {
        return false;
    }
    if (swapchain.d3d11_shader_resources_attempted) {
        return swapchain.d3d11_shader_resources_available;
    }
    swapchain.d3d11_shader_resources_attempted = true;
    if (swapchain.d3d11_shader_resources.size() == swapchain.d3d11_images.size()) {
        const bool all_ready = std::all_of(swapchain.d3d11_shader_resources.begin(),
                                           swapchain.d3d11_shader_resources.end(),
                                           [](ID3D11ShaderResourceView* view) { return view != nullptr; });
        if (all_ready) {
            swapchain.d3d11_shader_resources_available = true;
            return true;
        }
        SafeReleaseVector(swapchain.d3d11_shader_resources);
    }

    uint32_t image_index = 0;
    swapchain.d3d11_shader_resources.reserve(swapchain.d3d11_images.size());
    for (ID3D11Texture2D* texture : swapchain.d3d11_images) {
        ID3D11ShaderResourceView* shader_resource = nullptr;
        HRESULT hr = CreateTextureShaderResourceView(d3d11_quadviews_compositor_.device,
                                                     texture,
                                                     swapchain.format,
                                                     &shader_resource);
        if (FAILED(hr)) {
            D3D11_TEXTURE2D_DESC texture_desc{};
            if (texture) {
                texture->GetDesc(&texture_desc);
            }
            SafeRelease(shader_resource);
            SafeReleaseVector(swapchain.d3d11_shader_resources);
            logger_.Info("D3D11 quadviews compositor direct input SRVs unavailable; using input copy path. "
                         "hr=" + FormatHex(static_cast<uint32_t>(hr)) +
                         ", failedImage=" + std::to_string(image_index) +
                         ", format=" + std::to_string(swapchain.format) +
                         ", size=" + std::to_string(swapchain.width) + "x" + std::to_string(swapchain.height) +
                         ", usage=" + FormatUsageFlags(swapchain.usage_flags) +
                         ", " + FormatTextureDesc(texture_desc));
            swapchain.d3d11_shader_resources_available = false;
            return false;
        }
        swapchain.d3d11_shader_resources.push_back(shader_resource);
        ++image_index;
    }
    swapchain.d3d11_shader_resources_available =
        swapchain.d3d11_shader_resources.size() == swapchain.d3d11_images.size();
    if (swapchain.d3d11_shader_resources_available) {
        logger_.Debug("D3D11 quadviews compositor direct input SRVs ready: size=" +
                      std::to_string(swapchain.width) + "x" + std::to_string(swapchain.height) +
                      ", images=" + std::to_string(swapchain.d3d11_shader_resources.size()) +
                      ", format=" + std::to_string(swapchain.format));
    }
    return swapchain.d3d11_shader_resources_available;
}

void OpenXrLayer::TryPrewarmD3D11QuadViewsCompositor() {
    if (!d3d11_quadviews_compositor_.device || !d3d11_quadviews_compositor_.context ||
        active_session_ == XR_NULL_HANDLE || cached_quadviews_stereo_recommended_width_ == 0 ||
        cached_quadviews_stereo_recommended_height_ == 0) {
        return;
    }

    std::array<SwapchainInfo*, 4> quad_swapchains{};
    uint32_t count = 0;
    for (auto& [swapchain_handle, swapchain] : tracked_swapchains_) {
        if (!swapchain.quadviews_session || swapchain.d3d11_images.empty()) {
            continue;
        }
        if (count < quad_swapchains.size()) {
            quad_swapchains[count++] = &swapchain;
        }
    }
    if (count < quad_swapchains.size()) {
        return;
    }

    SwapchainInfo* largest_swapchain = quad_swapchains[0];
    for (SwapchainInfo* swapchain : quad_swapchains) {
        if (static_cast<uint64_t>(swapchain->width) * swapchain->height >
            static_cast<uint64_t>(largest_swapchain->width) * largest_swapchain->height) {
            largest_swapchain = swapchain;
        }
    }

    uint32_t direct_input_count = 0;
    for (SwapchainInfo* swapchain : quad_swapchains) {
        if (EnsureD3D11SwapchainShaderResources(*swapchain)) {
            ++direct_input_count;
        }
    }

    const bool output_ready = EnsureD3D11QuadViewsCompositor(nullptr,
                                                            cached_quadviews_stereo_recommended_width_,
                                                            cached_quadviews_stereo_recommended_height_,
                                                            largest_swapchain->format);
    if (output_ready && !d3d11_quadviews_compositor_.has_logged_prewarm) {
        const uint32_t direct_output_count =
            static_cast<uint32_t>(d3d11_quadviews_compositor_.targets[0].image_render_target_views.empty() ? 0 : 1) +
            static_cast<uint32_t>(d3d11_quadviews_compositor_.targets[1].image_render_target_views.empty() ? 0 : 1);
        logger_.Info("D3D11 quadviews compositor prewarmed: outputSize=" +
                     std::to_string(cached_quadviews_stereo_recommended_width_) + "x" +
                     std::to_string(cached_quadviews_stereo_recommended_height_) +
                     ", directInputSwapchains=" + std::to_string(direct_input_count) + "/4" +
                     ", directOutputEyes=" + std::to_string(direct_output_count) + "/2" +
                     ", gpuTiming=" + std::to_string(d3d11_quadviews_compositor_.gpu_timing_available));
        d3d11_quadviews_compositor_.has_logged_prewarm = true;
    }
}

bool OpenXrLayer::ComposeQuadViewsD3D11(const XrCompositionLayerProjection* source_layer,
                                        XrTime display_time,
                                        const XrPosef& reverse_delta,
                                        bool has_non_identity_delta,
                                        XrCompositionLayerProjection* composed_layer,
                                        std::vector<XrCompositionLayerProjectionView>* composed_views) {
    const auto compose_start = std::chrono::steady_clock::now();
    if (!source_layer || source_layer->viewCount < 4 || !source_layer->views || !composed_layer || !composed_views ||
        !d3d11_quadviews_compositor_.device || !d3d11_quadviews_compositor_.context) {
        return false;
    }

    std::array<SwapchainInfo*, 4> swapchains{};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto it = tracked_swapchains_.find(source_layer->views[i].subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !it->second.has_last_acquired_image_index ||
            it->second.last_acquired_image_index >= it->second.d3d11_images.size()) {
            return false;
        }
        swapchains[i] = &it->second;
    }

    const double peripheral_scale = resolved_settings_.quadviews.peripheral_scale;
    const double focus_width_scale = QuadViewsFocusWidthScale(resolved_settings_.quadviews);
    const double focus_height_scale = QuadViewsFocusHeightScale(resolved_settings_.quadviews);
    const uint32_t output_width = cached_quadviews_stereo_recommended_width_ > 0
                                      ? cached_quadviews_stereo_recommended_width_
                                      : std::max({
                                            EstimateFullResolutionDimension(swapchains[0]->width, peripheral_scale),
                                            EstimateFullResolutionDimension(swapchains[1]->width, peripheral_scale),
                                            EstimateFullResolutionDimension(swapchains[2]->width, focus_width_scale),
                                            EstimateFullResolutionDimension(swapchains[3]->width, focus_width_scale),
                                        });
    const uint32_t output_height = cached_quadviews_stereo_recommended_height_ > 0
                                       ? cached_quadviews_stereo_recommended_height_
                                       : std::max({
                                             EstimateFullResolutionDimension(swapchains[0]->height, peripheral_scale),
                                             EstimateFullResolutionDimension(swapchains[1]->height, peripheral_scale),
                                             EstimateFullResolutionDimension(swapchains[2]->height, focus_height_scale),
                                             EstimateFullResolutionDimension(swapchains[3]->height, focus_height_scale),
                                         });
    const int64_t output_format = swapchains[2]->format;
    if (!EnsureD3D11QuadViewsCompositor(source_layer, output_width, output_height, output_format)) {
        return false;
    }

    std::array<XrFovf, 4> cached_fovs{};
    XrTime matched_quadviews_fov_time = 0;
    const bool has_cached_fovs = FindQuadViewsFovs(display_time, &cached_fovs, &matched_quadviews_fov_time);

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
    std::string failure_reason;
    std::array<uint32_t, 2> output_indices{};
    uint32_t input_copy_count = 0;
    uint32_t direct_input_count = 0;
    uint32_t output_copy_count = 0;
    uint32_t direct_output_count = 0;
    double completed_gpu_ms = -1.0;
    XrTime completed_gpu_frame_time = 0;
    if (d3d11_quadviews_compositor_.gpu_timing_available) {
        for (QuadViewsGpuTimingQuery& query : d3d11_quadviews_compositor_.gpu_timing_queries) {
            if (!query.issued || !query.disjoint || !query.start || !query.end) {
                continue;
            }

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 start_timestamp = 0;
            UINT64 end_timestamp = 0;
            const HRESULT disjoint_result =
                context->GetData(query.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT start_result =
                context->GetData(query.start, &start_timestamp, sizeof(start_timestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT end_result =
                context->GetData(query.end, &end_timestamp, sizeof(end_timestamp), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (disjoint_result == S_OK && start_result == S_OK && end_result == S_OK) {
                if (!disjoint.Disjoint && disjoint.Frequency > 0 && end_timestamp >= start_timestamp) {
                    completed_gpu_ms =
                        static_cast<double>(end_timestamp - start_timestamp) /
                        static_cast<double>(disjoint.Frequency) * 1000.0;
                    completed_gpu_frame_time = query.frame_time;
                }
                query.issued = false;
                break;
            }
        }
    }

    QuadViewsGpuTimingQuery* active_gpu_query = nullptr;
    if (pending_quadviews_compositor_diagnostics_ > 0 && d3d11_quadviews_compositor_.gpu_timing_available) {
        QuadViewsGpuTimingQuery& query =
            d3d11_quadviews_compositor_.gpu_timing_queries[d3d11_quadviews_compositor_.next_gpu_timing_query %
                                                           d3d11_quadviews_compositor_.gpu_timing_queries.size()];
        if (!query.issued && query.disjoint && query.start && query.end) {
            active_gpu_query = &query;
            context->Begin(active_gpu_query->disjoint);
            context->End(active_gpu_query->start);
        }
    }
    auto release_input_copy = [](QuadViewsInputCopy& input_copy) {
        SafeRelease(input_copy.shader_resource);
        SafeRelease(input_copy.texture);
        input_copy = {};
    };
    auto ensure_input_copy = [&](uint32_t input_index, const SwapchainInfo& swapchain) -> bool {
        QuadViewsInputCopy& input_copy = d3d11_quadviews_compositor_.input_copies[input_index];
        if (input_copy.texture && input_copy.shader_resource && input_copy.width == swapchain.width &&
            input_copy.height == swapchain.height && input_copy.format == swapchain.format) {
            return true;
        }

        release_input_copy(input_copy);

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = swapchain.width;
        texture_desc.Height = swapchain.height;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = static_cast<DXGI_FORMAT>(swapchain.format);
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11_quadviews_compositor_.device->CreateTexture2D(&texture_desc, nullptr, &input_copy.texture);
        std::string d3d_step = "CreateInputCopyTexture";
        if (SUCCEEDED(hr)) {
            hr = d3d11_quadviews_compositor_.device->CreateShaderResourceView(
                input_copy.texture,
                nullptr,
                &input_copy.shader_resource);
            d3d_step = "CreateInputCopyShaderResourceView";
        }
        if (FAILED(hr)) {
            failure_reason = d3d_step + " input=" + std::to_string(input_index) +
                             ", hr=" + FormatHex(static_cast<uint32_t>(hr)) +
                             ", format=" + std::to_string(swapchain.format) +
                             ", size=" + std::to_string(swapchain.width) + "x" +
                             std::to_string(swapchain.height);
            release_input_copy(input_copy);
            return false;
        }

        input_copy.width = swapchain.width;
        input_copy.height = swapchain.height;
        input_copy.format = swapchain.format;
        logger_.Debug("D3D11 quadviews compositor input copy ready: input=" + std::to_string(input_index) +
                      ", size=" + std::to_string(input_copy.width) + "x" + std::to_string(input_copy.height) +
                      ", format=" + std::to_string(input_copy.format));
        return true;
    };

    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!rendered) {
            break;
        }
        QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = next_acquire_swapchain_image_(target.swapchain, &acquire_info, &output_indices[eye]);
        if (XR_FAILED(result) || output_indices[eye] >= target.d3d11_images.size()) {
            failure_reason = "outputAcquire eye=" + std::to_string(eye) +
                             ", result=" + FormatHex(static_cast<uint64_t>(result)) +
                             ", imageIndex=" + std::to_string(output_indices[eye]) +
                             ", images=" + std::to_string(target.d3d11_images.size());
            rendered = false;
            break;
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        result = next_wait_swapchain_image_(target.swapchain, &wait_info);
        if (XR_FAILED(result)) {
            failure_reason = "outputWait eye=" + std::to_string(eye) +
                             ", result=" + FormatHex(static_cast<uint64_t>(result));
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next_release_swapchain_image_(target.swapchain, &release_info);
            rendered = false;
            break;
        }
        auto release_output_after_failure = [&]() {
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next_release_swapchain_image_(target.swapchain, &release_info);
        };

        ID3D11Texture2D* peripheral_source =
            swapchains[eye]->d3d11_images[swapchains[eye]->last_acquired_image_index];
        ID3D11Texture2D* focus_source =
            swapchains[eye + 2]->d3d11_images[swapchains[eye + 2]->last_acquired_image_index];

        ID3D11ShaderResourceView* peripheral_resource = nullptr;
        ID3D11ShaderResourceView* focus_resource = nullptr;
        SwapchainInfo& peripheral_swapchain = *swapchains[eye];
        SwapchainInfo& focus_swapchain = *swapchains[eye + 2];
        if (EnsureD3D11SwapchainShaderResources(peripheral_swapchain) &&
            peripheral_swapchain.last_acquired_image_index < peripheral_swapchain.d3d11_shader_resources.size()) {
            peripheral_resource =
                peripheral_swapchain.d3d11_shader_resources[peripheral_swapchain.last_acquired_image_index];
            ++direct_input_count;
        } else {
            if (!ensure_input_copy(eye, *swapchains[eye])) {
                release_output_after_failure();
                rendered = false;
                break;
            }
            context->CopyResource(d3d11_quadviews_compositor_.input_copies[eye].texture, peripheral_source);
            peripheral_resource = d3d11_quadviews_compositor_.input_copies[eye].shader_resource;
            ++input_copy_count;
        }
        if (EnsureD3D11SwapchainShaderResources(focus_swapchain) &&
            focus_swapchain.last_acquired_image_index < focus_swapchain.d3d11_shader_resources.size()) {
            focus_resource = focus_swapchain.d3d11_shader_resources[focus_swapchain.last_acquired_image_index];
            ++direct_input_count;
        } else {
            if (!ensure_input_copy(eye + 2, *swapchains[eye + 2])) {
                release_output_after_failure();
                rendered = false;
                break;
            }
            context->CopyResource(d3d11_quadviews_compositor_.input_copies[eye + 2].texture, focus_source);
            focus_resource = d3d11_quadviews_compositor_.input_copies[eye + 2].shader_resource;
            ++input_copy_count;
        }
        if (!peripheral_resource || !focus_resource) {
            failure_reason = "missingInputResource eye=" + std::to_string(eye);
            release_output_after_failure();
            rendered = false;
            break;
        }

        const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
        ID3D11RenderTargetView* render_target =
            output_indices[eye] < target.image_render_target_views.size()
                ? target.image_render_target_views[output_indices[eye]]
                : nullptr;
        const bool direct_output = render_target != nullptr;
        if (!render_target) {
            render_target = target.render_target_view;
        }
        if (!render_target) {
            failure_reason = "missingOutputRenderTarget eye=" + std::to_string(eye);
            release_output_after_failure();
            rendered = false;
            break;
        }
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
        ID3D11ShaderResourceView* resources[2]{
            peripheral_resource,
            focus_resource,
        };
        context->PSSetShaderResources(0, 2, resources);
        context->PSSetSamplers(0, 1, &d3d11_quadviews_compositor_.sampler);

        const XrFovf& full_fov = has_cached_fovs ? cached_fovs[eye] : source_layer->views[eye].fov;
        const XrFovf& focus_fov = has_cached_fovs ? cached_fovs[eye + 2] : source_layer->views[eye + 2].fov;
        FocusRectConstants constants = BuildFocusRectConstants(full_fov,
                                                               focus_fov,
                                                               target.width,
                                                               target.height,
                                                               resolved_settings_.quadviews.transition_thickness_percent,
                                                               resolved_settings_.quadviews.foveate_sharpness);
        if (pending_quadviews_compositor_diagnostics_ > 0) {
            std::ostringstream stream;
            stream << "D3D11 quadviews compositor focus rect: frameTime=" << display_time
                   << ", eye=" << eye
                   << ", cachedFovHit=" << has_cached_fovs
                   << ", matchedTime=" << matched_quadviews_fov_time
                   << ", matchedDeltaNs=" << (matched_quadviews_fov_time - display_time)
                   << ", rect=(" << FormatDiagnosticDouble(constants.focus_rect[0]) << ", "
                   << FormatDiagnosticDouble(constants.focus_rect[1]) << ", "
                   << FormatDiagnosticDouble(constants.focus_rect[2]) << ", "
                   << FormatDiagnosticDouble(constants.focus_rect[3]) << ")"
                   << ", feather=(" << FormatDiagnosticDouble(constants.blend_params[0]) << ", "
                   << FormatDiagnosticDouble(constants.blend_params[1]) << ")"
                   << ", sharpness=" << FormatDiagnosticDouble(constants.blend_params[2])
                   << ", fullFov=" << FormatFov(full_fov)
                   << ", focusFov=" << FormatFov(focus_fov)
                   << ", sourceFullFov=" << FormatFov(source_layer->views[eye].fov)
                   << ", sourceFocusFov=" << FormatFov(source_layer->views[eye + 2].fov);
            logger_.Debug(stream.str());
        }
        context->UpdateSubresource(d3d11_quadviews_compositor_.constants, 0, nullptr, &constants, 0, 0);
        context->PSSetConstantBuffers(0, 1, &d3d11_quadviews_compositor_.constants);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* null_resources[2]{nullptr, nullptr};
        context->PSSetShaderResources(0, 2, null_resources);
        ID3D11RenderTargetView* null_render_target = nullptr;
        context->OMSetRenderTargets(1, &null_render_target, nullptr);
        if (direct_output) {
            ++direct_output_count;
        } else {
            context->CopyResource(target.d3d11_images[output_indices[eye]], target.render_texture);
            ++output_copy_count;
        }

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = next_release_swapchain_image_(target.swapchain, &release_info);
        if (XR_FAILED(result)) {
            failure_reason = "outputRelease eye=" + std::to_string(eye) +
                             ", result=" + FormatHex(static_cast<uint64_t>(result));
            rendered = false;
            break;
        }
    }
    if (active_gpu_query) {
        context->End(active_gpu_query->end);
        context->End(active_gpu_query->disjoint);
        if (rendered) {
            active_gpu_query->issued = true;
            active_gpu_query->frame_time = display_time;
            d3d11_quadviews_compositor_.next_gpu_timing_query =
                (d3d11_quadviews_compositor_.next_gpu_timing_query + 1) %
                static_cast<uint32_t>(d3d11_quadviews_compositor_.gpu_timing_queries.size());
        }
    }

    restore_state();
    if (!rendered) {
        if (d3d11_quadviews_compositor_.failure_logs_remaining > 0) {
            logger_.Error("D3D11 quadviews composition failed; falling back to projection-layer split. reason=" +
                          failure_reason);
            --d3d11_quadviews_compositor_.failure_logs_remaining;
            if (d3d11_quadviews_compositor_.failure_logs_remaining == 0) {
                logger_.Error("D3D11 quadviews composition failures are repeating; suppressing further per-frame "
                              "failure logs for this session.");
            }
        }
        return false;
    }
    d3d11_quadviews_compositor_.failure_logs_remaining = 8;
    if (!d3d11_quadviews_compositor_.has_logged_capabilities) {
        logger_.Info("D3D11 quadviews compositor capabilities: outputSize=" +
                     std::to_string(output_width) + "x" + std::to_string(output_height) +
                     ", cachedStereoSize=" + std::to_string(cached_quadviews_stereo_recommended_width_) + "x" +
                     std::to_string(cached_quadviews_stereo_recommended_height_) +
                     ", directInputs=" + std::to_string(direct_input_count) + "/4" +
                     ", inputCopyFallbacks=" + std::to_string(input_copy_count) +
                     ", directOutputs=" + std::to_string(direct_output_count) + "/2" +
                     ", outputCopyFallbacks=" + std::to_string(output_copy_count) +
                     ", gpuTiming=" + std::to_string(d3d11_quadviews_compositor_.gpu_timing_available) +
                     ", appPixelBudget=" + FormatDiagnosticDouble(
                         (static_cast<double>(swapchains[0]->width) * swapchains[0]->height +
                          static_cast<double>(swapchains[2]->width) * swapchains[2]->height) /
                         std::max(1.0, static_cast<double>(output_width) * output_height) * 100.0) +
                     "%");
        d3d11_quadviews_compositor_.has_logged_capabilities = true;
    }
    if (pending_quadviews_compositor_diagnostics_ > 0) {
        const auto compose_end = std::chrono::steady_clock::now();
        const double cpu_ms = std::chrono::duration<double, std::milli>(compose_end - compose_start).count();
        std::ostringstream stream;
        stream << "D3D11 quadviews compositor frame: frameTime=" << display_time
               << ", cpuMs=" << FormatDiagnosticDouble(cpu_ms)
               << ", outputSize=" << output_width << "x" << output_height
               << ", cachedStereoSize=" << cached_quadviews_stereo_recommended_width_ << "x"
               << cached_quadviews_stereo_recommended_height_
               << ", directInputs=" << direct_input_count
               << ", inputCopies=" << input_copy_count
               << ", directOutputs=" << direct_output_count
               << ", outputCopies=" << output_copy_count
               << ", completedGpuFrameTime=" << completed_gpu_frame_time
               << ", completedGpuMs="
               << (completed_gpu_ms >= 0.0 ? FormatDiagnosticDouble(completed_gpu_ms) : "pending")
               << ", appPixelBudget=" << FormatDiagnosticDouble(
                      (static_cast<double>(swapchains[0]->width) * swapchains[0]->height +
                       static_cast<double>(swapchains[2]->width) * swapchains[2]->height) /
                      std::max(1.0, static_cast<double>(output_width) * output_height) * 100.0)
               << "%";
        logger_.Debug(stream.str());
        --pending_quadviews_compositor_diagnostics_;
    }

    composed_views->assign(source_layer->views, source_layer->views + 2);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (has_non_identity_delta) {
            (*composed_views)[eye].pose = MultiplyPoses((*composed_views)[eye].pose, reverse_delta);
        }
        if (has_cached_fovs) {
            (*composed_views)[eye].fov = cached_fovs[eye];
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
        cached_quadviews_fovs_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        PruneQuadViewsFovs(frame_end_info->displayTime);
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
        pivotxr_activation_gain_ = 0.0;
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

    // Log what the app actually submitted (independent of the pivot/quadviews
    // branch below) so we can confirm whether DepthXR's xrLocateViews per-eye
    // pose/FOV survives into the composition layer the runtime presents, or
    // whether the app re-derives its own. Does not consume the counter; the
    // branch diagnostics below own the decrement.
    if (pending_end_frame_diagnostics_ > 0) {
        for (uint32_t i = 0; i < frame_end_info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* base_header = frame_end_info->layers[i];
            if (!base_header || base_header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                continue;
            }
            const auto* projection_layer =
                reinterpret_cast<const XrCompositionLayerProjection*>(base_header);
            if (!projection_layer->views || projection_layer->viewCount == 0) {
                continue;
            }
            std::ostringstream stream;
            stream << "EndFrame submitted projection: frameTime=" << frame_end_info->displayTime
                   << ", layer=" << i << ", viewCount=" << projection_layer->viewCount;
            const uint32_t logged = std::min<uint32_t>(projection_layer->viewCount, 4);
            for (uint32_t v = 0; v < logged; ++v) {
                const XrCompositionLayerProjectionView& view = projection_layer->views[v];
                stream << " view" << v << "Pos=(" << FormatDiagnosticDouble(view.pose.position.x) << ", "
                       << FormatDiagnosticDouble(view.pose.position.y) << ", "
                       << FormatDiagnosticDouble(view.pose.position.z) << ")"
                       << " view" << v << "Fov=(" << FormatDiagnosticDouble(view.fov.angleLeft) << ", "
                       << FormatDiagnosticDouble(view.fov.angleRight) << ", "
                       << FormatDiagnosticDouble(view.fov.angleUp) << ", "
                       << FormatDiagnosticDouble(view.fov.angleDown) << ")";
            }
            logger_.Debug(stream.str());
            break;
        }
    }

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
        PruneQuadViewsFovs(frame_end_info->displayTime);
        return next_end_frame_(session, frame_end_info);
    }

    const XrPosef reverse_delta = InvertPose(pose_delta);
    std::vector<std::vector<XrCompositionLayerProjectionView>>& adjusted_projection_views =
        end_frame_projection_views_scratch_;
    std::vector<XrCompositionLayerProjection>& adjusted_projection_layers = end_frame_projection_layers_scratch_;
    std::vector<const XrCompositionLayerBaseHeader*>& adjusted_layers = end_frame_layers_scratch_;
    adjusted_projection_views.clear();
    adjusted_projection_layers.clear();
    adjusted_layers.clear();
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
                                      frame_end_info->displayTime,
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
    adjusted_frame_end_info.layerCount = static_cast<uint32_t>(adjusted_layers.size());
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
    } else if (quadviews_projection_split_active && split_quad_projection_layer_count > 0 &&
               logger_.IsDebugEnabled()) {
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
    PruneQuadViewsFovs(frame_end_info->displayTime);
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

    // Quad-view sessions must keep xrLocateSpace consistent with the pivoted
    // view poses returned from xrLocateViews. Apps such as DCS place
    // head-attached geometry (e.g. the FA18 pilot visor) from VIEW-space
    // locates; skipping pivot here desynchronizes that geometry from the
    // pivoted camera.
    const bool pivotxr_active = IsPivotXrActive(resolved_settings_.pivotxr);
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
    std::vector<ViewAdjustmentData>& original_views = locate_views_original_scratch_;
    std::vector<ViewAdjustmentData>& adjusted_views = locate_views_adjusted_scratch_;
    original_views.resize(count);
    adjusted_views.resize(count);
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
    // Keep driving pivot while the activation envelope is still easing out so a
    // toggle-off releases the view smoothly instead of snapping back to center.
    const bool pivotxr_envelope_engaged =
        pivotxr_active || pivotxr_activation_gain_ > kPivotActivationGainEpsilon;
    const bool depthxr_active = IsDepthXrActive();
    if (resolved_settings_.pivotxr.enabled && !has_logged_pivotxr_spike_mode_) {
        std::ostringstream stream;
        stream << "PivotXR spike is active; quad-view sessions use stereo eye-pose recomposition. Press "
               << BindingLabel(resolved_settings_.pivotxr.activation_binding) << " to "
               << (resolved_settings_.pivotxr.activation_mode == ActivationMode::Toggle ? "toggle" : "hold")
               << " the extra pivot factor.";
        logger_.Info(stream.str());
        has_logged_pivotxr_spike_mode_ = true;
    }

    if (resolved_settings_.pivotxr.enabled && pivotxr_envelope_engaged && internal_view_space_ != XR_NULL_HANDLE &&
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
            } else if (IsQuadViewConfiguration(view_configuration_type) &&
                       EnsureEyeOffsets(session,
                                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                        view_locate_info->displayTime,
                                        2)) {
                CachePivotPoseDelta(view_locate_info->displayTime, applied_pose_delta);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t eye_index = i % 2;
                    const XrPosef recomposed_pose =
                        MultiplyPoses(cached_eye_offset_poses_[eye_index], pivot_view_location.pose);
                    adjusted_views[i].position.x = recomposed_pose.position.x;
                    adjusted_views[i].position.y = recomposed_pose.position.y;
                    adjusted_views[i].position.z = recomposed_pose.position.z;
                    adjusted_views[i].orientation.x = recomposed_pose.orientation.x;
                    adjusted_views[i].orientation.y = recomposed_pose.orientation.y;
                    adjusted_views[i].orientation.z = recomposed_pose.orientation.z;
                    adjusted_views[i].orientation.w = recomposed_pose.orientation.w;
                }
            } else if (!IsQuadViewConfiguration(view_configuration_type) &&
                       EnsureEyeOffsets(session, view_configuration_type, view_locate_info->displayTime, count)) {
                CachePivotPoseDelta(view_locate_info->displayTime, applied_pose_delta);
                for (uint32_t i = 0; i < count; ++i) {
                    const XrPosef recomposed_pose =
                        MultiplyPoses(cached_eye_offset_poses_[i], pivot_view_location.pose);
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
            // LocateSpaceWithPivot owns the steady-state smoothed angles and the
            // activation envelope; do not write the eased output back onto them.
        }
    } else if (!pivotxr_envelope_engaged) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_activation_gain_ = 0.0;
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
    if (view_locate_info && IsQuadViewConfiguration(view_configuration_type) && count >= 4) {
        CacheQuadViewsFovs(view_locate_info->displayTime, std::span<const XrView>(views, count));
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
               << ", pivotActivationGain=" << FormatDiagnosticDouble(pivotxr_activation_gain_)
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
    const auto now = std::chrono::steady_clock::now();
    if (has_loaded_config_ && last_config_check_time_.has_value() &&
        now - *last_config_check_time_ < kConfigCheckInterval) {
        return;
    }
    last_config_check_time_ = now;

    if (config_path_.empty()) {
        config_path_ = ResolveConfigPath();
    }

    if (!std::filesystem::exists(config_path_)) {
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
            ++config_generation_;
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
            ++config_generation_;
        }
        return;
    }

    config_ = loaded.document;
    has_loaded_config_ = true;
    has_config_timestamp_ = true;
    last_config_write_time_ = timestamp;
    has_failed_config_timestamp_ = false;
    last_failed_config_error_.clear();
    ++config_generation_;
    logger_.Info("Loaded config from " + config_path_.string());
}

void OpenXrLayer::RefreshResolvedSettings() {
    // Re-resolving settings is expensive (string compares, copies, logging
    // checks); the result only changes when the config document or the active
    // session changes, so skip the work otherwise.
    if (resolved_settings_generation_ == config_generation_ && resolved_settings_session_ == active_session_) {
        return;
    }
    resolved_settings_generation_ = config_generation_;
    resolved_settings_session_ = active_session_;

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
        pending_eye_gaze_diagnostics_ = 10;
        pending_eye_gaze_sync_diagnostics_ = 10;
    }

    if (IsQuadViewsActive() && resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye &&
        active_session_ != XR_NULL_HANDLE && !eye_gaze_resources_ready_) {
        const XrResult eye_gaze_result = CreateEyeGazeResources(active_session_);
        if (XR_FAILED(eye_gaze_result)) {
            logger_.Info("Eye gaze resources unavailable after quadviews tracking-mode change; head/static focus remains active.");
            DestroyEyeGazeResources();
        }
    } else if ((!IsQuadViewsActive() || resolved_settings_.quadviews.tracking_mode != QuadViewsTrackingMode::Eye) &&
               eye_gaze_resources_ready_) {
        DestroyEyeGazeResources();
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
    pending_eye_gaze_diagnostics_ = 0;
    pending_eye_gaze_sync_diagnostics_ = 0;
    pending_quadviews_compositor_diagnostics_ = 0;
    eye_gaze_diagnostic_stride_counter_ = 0;
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_activation_gain_ = 0.0;
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
    cached_eye_offsets_display_time_ = 0;
    cached_pivot_pose_deltas_.clear();
    cached_quadviews_fovs_.clear();
    last_app_action_sync_time_.reset();
    ResetSwapchainState();
}

void OpenXrLayer::ResetInstanceState() {
    quad_views_extension_requested_ = false;
    varjo_foveated_rendering_extension_requested_ = false;
    eye_gaze_extension_enabled_ = false;
    cached_quadviews_stereo_recommended_width_ = 0;
    cached_quadviews_stereo_recommended_height_ = 0;
}

bool OpenXrLayer::IsQuadViewsActive() const {
    return resolved_settings_.core.enabled && resolved_settings_.quadviews.enabled;
}

XrResult OpenXrLayer::CreateEyeGazeResources(XrSession session) {
    if (eye_gaze_resources_ready_) {
        return XR_SUCCESS;
    }
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
    pending_eye_gaze_diagnostics_ = std::max(pending_eye_gaze_diagnostics_, 120u);
    pending_eye_gaze_sync_diagnostics_ = std::max(pending_eye_gaze_sync_diagnostics_, 20u);
    pending_quadviews_compositor_diagnostics_ = std::max(pending_quadviews_compositor_diagnostics_, 30u);
    eye_gaze_diagnostic_stride_counter_ = 0;
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
    auto log_eye_gaze_diagnostic = [&](const std::string& reason) {
        if (settings.tracking_mode == QuadViewsTrackingMode::Eye && pending_eye_gaze_diagnostics_ > 0) {
            logger_.Debug("Quadviews eye-gaze focus unavailable: " + reason +
                          ", resourcesReady=" + std::to_string(eye_gaze_resources_ready_) +
                          ", actionSetAttached=" + std::to_string(eye_gaze_action_set_attached_));
            --pending_eye_gaze_diagnostics_;
        }
    };

    if (settings.tracking_mode != QuadViewsTrackingMode::Eye) {
        return false;
    }
    if (session != active_session_) {
        log_eye_gaze_diagnostic("session mismatch");
        return false;
    }
    if (base_space == XR_NULL_HANDLE) {
        log_eye_gaze_diagnostic("base space is null");
        return false;
    }
    if (!eye_gaze_resources_ready_ || !eye_gaze_action_set_attached_ ||
        quadviews_eye_gaze_action_ == XR_NULL_HANDLE || quadviews_eye_gaze_space_ == XR_NULL_HANDLE) {
        log_eye_gaze_diagnostic("eye-gaze action resources are not ready");
        return false;
    }
    if (!next_get_action_state_pose_ || !next_locate_space_ || !next_sync_actions_) {
        log_eye_gaze_diagnostic("required OpenXR action/space functions are unavailable");
        return false;
    }

    // The layer's action set rides along with the app's xrSyncActions; only
    // issue a downstream self-sync when the app has not synced recently.
    const auto sync_check_now = std::chrono::steady_clock::now();
    const bool app_sync_fresh = last_app_action_sync_time_.has_value() &&
                                sync_check_now - *last_app_action_sync_time_ < kAppActionSyncFreshWindow;
    if (!app_sync_fresh) {
        const XrActiveActionSet active_action_set{quadviews_action_set_, XR_NULL_PATH};
        XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
        sync_info.countActiveActionSets = 1;
        sync_info.activeActionSets = &active_action_set;
        const XrResult sync_result = next_sync_actions_(session, &sync_info);
        if (XR_FAILED(sync_result)) {
            log_eye_gaze_diagnostic("self sync failed, result=" + FormatHex(static_cast<uint64_t>(sync_result)));
            return false;
        }
    }

    XrActionStateGetInfo action_state_info{XR_TYPE_ACTION_STATE_GET_INFO};
    action_state_info.action = quadviews_eye_gaze_action_;
    XrActionStatePose action_state{XR_TYPE_ACTION_STATE_POSE};
    const XrResult state_result = next_get_action_state_pose_(session, &action_state_info, &action_state);
    if (XR_FAILED(state_result) || !action_state.isActive) {
        log_eye_gaze_diagnostic("action state is unavailable, result=" +
                                FormatHex(static_cast<uint64_t>(state_result)) +
                                ", active=" + std::to_string(action_state.isActive));
        return false;
    }

    const XrSpace gaze_base_space = internal_view_space_ != XR_NULL_HANDLE ? internal_view_space_ : base_space;

    XrSpaceLocation gaze_location{XR_TYPE_SPACE_LOCATION};
    const XrResult locate_result = next_locate_space_(quadviews_eye_gaze_space_, gaze_base_space, time, &gaze_location);
    if (XR_FAILED(locate_result) ||
        (gaze_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        log_eye_gaze_diagnostic("gaze space locate failed, result=" +
                                FormatHex(static_cast<uint64_t>(locate_result)) +
                                ", flags=" + FormatHex(static_cast<uint64_t>(gaze_location.locationFlags)));
        return false;
    }

    const XrQuaternionf gaze_orientation = gaze_location.pose.orientation;
    const GazeRayAngles gaze_ray_angles = ExtractGazeRayAngles(gaze_orientation);
    const ViewOrientation diagnostic_orientation{
        gaze_orientation.x,
        gaze_orientation.y,
        gaze_orientation.z,
        gaze_orientation.w,
    };
    const double euler_yaw = ExtractYawRadians(diagnostic_orientation);
    const double euler_pitch = ExtractPitchRadians(diagnostic_orientation);
    double target_yaw = gaze_ray_angles.yaw_radians;
    double target_pitch = gaze_ray_angles.pitch_radians;
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
    const bool should_log_eye_gaze =
        pending_eye_gaze_diagnostics_ > 0 &&
        (eye_gaze_diagnostic_stride_counter_ < 20 || eye_gaze_diagnostic_stride_counter_ % 45 == 0);
    ++eye_gaze_diagnostic_stride_counter_;
    if (should_log_eye_gaze) {
        logger_.Debug("Quadviews eye-gaze focus active: rawYaw=" + FormatDiagnosticDouble(target_yaw) +
                      ", rawPitch=" + FormatDiagnosticDouble(target_pitch) +
                      ", eulerYaw=" + FormatDiagnosticDouble(euler_yaw) +
                      ", eulerPitch=" + FormatDiagnosticDouble(euler_pitch) +
                      ", smoothedYaw=" + FormatDiagnosticDouble(*yaw_radians) +
                      ", smoothedPitch=" + FormatDiagnosticDouble(*pitch_radians) +
                      ", forward=(" + FormatDiagnosticDouble(gaze_ray_angles.forward.x) + ", " +
                      FormatDiagnosticDouble(gaze_ray_angles.forward.y) + ", " +
                      FormatDiagnosticDouble(gaze_ray_angles.forward.z) + ")" +
                      ", baseSpace=" + std::string(gaze_base_space == internal_view_space_ ? "view" : "app"));
        --pending_eye_gaze_diagnostics_;
    }
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
        cached_eye_offsets_display_time_ = 0;
        cached_pivot_pose_deltas_.clear();
        cached_quadviews_fovs_.clear();
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
    cached_eye_offsets_display_time_ = 0;
    cached_pivot_pose_deltas_.clear();
    cached_quadviews_fovs_.clear();
}

bool OpenXrLayer::EnsureEyeOffsets(XrSession session,
                                   XrViewConfigurationType view_configuration_type,
                                   XrTime display_time,
                                   uint32_t view_count) {
    if (internal_view_space_ == XR_NULL_HANDLE || !next_locate_views_ || view_count == 0) {
        return false;
    }

    // Eye offsets are predicted poses. Reuse them only for repeated locates at
    // the same displayTime so recomposition does not inherit stale prediction.
    if (cached_eye_offset_poses_.size() == view_count &&
        cached_eye_offsets_view_configuration_ == view_configuration_type &&
        cached_eye_offsets_display_time_ != 0 &&
        cached_eye_offsets_display_time_ == display_time) {
        return true;
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
    cached_eye_offsets_view_configuration_ = view_configuration_type;
    cached_eye_offsets_display_time_ = display_time;
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

void OpenXrLayer::CacheQuadViewsFovs(XrTime time, std::span<const XrView> views) {
    if (time == 0 || views.size() < 4) {
        return;
    }

    std::array<XrFovf, 4> fovs{};
    for (uint32_t i = 0; i < fovs.size(); ++i) {
        fovs[i] = views[i].fov;
    }
    cached_quadviews_fovs_[time] = fovs;
    while (cached_quadviews_fovs_.size() > kMaxCachedQuadViewsFovFrames) {
        cached_quadviews_fovs_.erase(cached_quadviews_fovs_.begin());
    }
}

bool OpenXrLayer::FindQuadViewsFovs(XrTime time, std::array<XrFovf, 4>* fovs, XrTime* matched_time) const {
    if (!fovs || !matched_time) {
        return false;
    }

    if (cached_quadviews_fovs_.empty()) {
        *matched_time = 0;
        return false;
    }

    auto exact = cached_quadviews_fovs_.find(time);
    if (exact != cached_quadviews_fovs_.end()) {
        *fovs = exact->second;
        *matched_time = exact->first;
        return true;
    }

    auto upper = cached_quadviews_fovs_.lower_bound(time);
    std::map<XrTime, std::array<XrFovf, 4>>::const_iterator best;
    if (upper == cached_quadviews_fovs_.begin()) {
        best = upper;
    } else if (upper == cached_quadviews_fovs_.end()) {
        best = std::prev(upper);
    } else {
        auto lower = std::prev(upper);
        best = (time - lower->first <= upper->first - time) ? lower : upper;
    }

    const XrTime match_delta = best->first > time ? best->first - time : time - best->first;
    if (match_delta > kMaxQuadViewsFovMatchWindow) {
        *matched_time = best->first;
        return false;
    }

    *fovs = best->second;
    *matched_time = best->first;
    return true;
}

void OpenXrLayer::PruneQuadViewsFovs(XrTime time) {
    auto keep_from = cached_quadviews_fovs_.upper_bound(time);
    if (keep_from == cached_quadviews_fovs_.begin()) {
        return;
    }

    cached_quadviews_fovs_.erase(cached_quadviews_fovs_.begin(), keep_from);
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
    auto clear_extra_outputs = [&]() {
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
    };

    if (XR_FAILED(result) || !settings.enabled) {
        pivotxr_activation_gain_ = 0.0;
        clear_extra_outputs();
        return result;
    }

    // Advance the wall-clock delta and ease the activation envelope toward its
    // target. update_smoothing is true only on the xrLocateViews drive call;
    // the xrLocateSpace path (head-attached geometry) reads the same envelope
    // without advancing it so both stay in sync within a frame.
    double delta_seconds = 0.0;
    if (update_smoothing) {
        const auto now = std::chrono::steady_clock::now();
        if (pivotxr_last_smoothing_wall_time_.has_value()) {
            delta_seconds =
                std::chrono::duration<double>(now - *pivotxr_last_smoothing_wall_time_).count();
        }
        pivotxr_last_smoothing_wall_time_ = now;

        const double target_gain = pivotxr_active ? 1.0 : 0.0;
        if (delta_seconds > 0.0 && kPivotActivationRampSeconds > 0.0) {
            const double step = delta_seconds / kPivotActivationRampSeconds;
            if (target_gain > pivotxr_activation_gain_) {
                pivotxr_activation_gain_ = std::min(target_gain, pivotxr_activation_gain_ + step);
            } else {
                pivotxr_activation_gain_ = std::max(target_gain, pivotxr_activation_gain_ - step);
            }
        }
    }

    // Fully release once the envelope has closed and the pivot is no longer
    // engaged. While the envelope is still open we keep applying the (eased)
    // pivot so toggling off releases the view smoothly.
    if (!pivotxr_active && pivotxr_activation_gain_ <= kPivotActivationGainEpsilon) {
        pivotxr_activation_gain_ = 0.0;
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        if (update_smoothing) {
            pivotxr_last_smoothing_wall_time_.reset();
        }
        clear_extra_outputs();
        return result;
    }

    const bool space_is_view = IsTrackedViewSpace(space);
    const bool base_space_is_view = IsTrackedViewSpace(base_space);
    if (space_is_view == base_space_is_view) {
        clear_extra_outputs();
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

    // Track the steady-state pivot amount only while actively engaged. During
    // the release ramp it is frozen and the envelope eases the applied angle to
    // zero, avoiding any snap on toggle-off.
    if (update_smoothing && pivotxr_active) {
        const double current_yaw_radians = ExtractYawRadians(orientation);
        const double current_pitch_radians = ExtractPitchRadians(orientation);
        ComputePivotExtraAngleRadians(current_yaw_radians,
                                      settings.yaw_rotation_multiplier,
                                      settings.yaw_deadzone_degrees,
                                      settings.yaw_max_extra_degrees,
                                      settings.yaw_smoothing,
                                      delta_seconds,
                                      pivotxr_smoothed_extra_yaw_radians_);
        ComputePivotExtraAngleRadians(current_pitch_radians,
                                      settings.pitch_rotation_multiplier,
                                      settings.pitch_deadzone_degrees,
                                      settings.pitch_max_extra_degrees,
                                      settings.pitch_smoothing,
                                      delta_seconds,
                                      pivotxr_smoothed_extra_pitch_radians_);
    }

    const double eased_gain = SmoothStep(pivotxr_activation_gain_);
    const double extra_yaw_radians = pivotxr_smoothed_extra_yaw_radians_ * eased_gain;
    const double extra_pitch_radians = pivotxr_smoothed_extra_pitch_radians_ * eased_gain;
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
           << ", quadviewsTrackingMode=" << ToString(settings.quadviews.tracking_mode)
           << ", quadviewsFocusHorizontalSizePercent=" << settings.quadviews.focus_horizontal_size_percent
           << ", quadviewsFocusVerticalSizePercent=" << settings.quadviews.focus_vertical_size_percent
           << ", quadviewsFocusScale=" << settings.quadviews.focus_scale
           << ", quadviewsPeripheralScale=" << settings.quadviews.peripheral_scale
           << ", quadviewsFoveateSharpness=" << settings.quadviews.foveate_sharpness
           << ", quadviewsTransitionThicknessPercent=" << settings.quadviews.transition_thickness_percent
           << ", quadviewsHorizontalOffset=" << settings.quadviews.horizontal_offset_degrees
           << ", quadviewsVerticalOffset=" << settings.quadviews.vertical_offset_degrees
           << ", quadviewsGazeSmoothing=" << settings.quadviews.gaze_smoothing
           << ", quadviewsGazeDeadzone=" << settings.quadviews.gaze_deadzone_degrees;
    logger_.Debug(stream.str());
}

void OpenXrLayer::ResetPivotActivationState() {
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_activation_gain_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    pivotxr_toggle_enabled_ = false;
    pivotxr_activation_key_was_down_ = false;
    pivotxr_binding_last_poll_time_.reset();
    pivotxr_binding_down_cached_ = false;
}

void OpenXrLayer::ResetDepthToggleState() {
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    depthxr_binding_last_poll_time_.reset();
    depthxr_binding_down_cached_ = false;
}

void OpenXrLayer::ResetSwapchainState() {
    for (auto& [swapchain, info] : tracked_swapchains_) {
        SafeReleaseVector(info.d3d11_shader_resources);
    }
    tracked_swapchains_.clear();
}

void OpenXrLayer::ResetD3D11QuadViewsCompositor() {
    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            next_destroy_swapchain_(target.swapchain);
        }
        target = {};
    }

    for (QuadViewsInputCopy& input_copy : d3d11_quadviews_compositor_.input_copies) {
        SafeRelease(input_copy.shader_resource);
        SafeRelease(input_copy.texture);
        input_copy = {};
    }
    for (QuadViewsGpuTimingQuery& query : d3d11_quadviews_compositor_.gpu_timing_queries) {
        SafeRelease(query.disjoint);
        SafeRelease(query.start);
        SafeRelease(query.end);
        query = {};
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
    d3d11_quadviews_compositor_.gpu_timing_available = false;
    d3d11_quadviews_compositor_.has_logged_capabilities = false;
    d3d11_quadviews_compositor_.has_logged_prewarm = false;
    d3d11_quadviews_compositor_.failure_logs_remaining = 8;
    d3d11_quadviews_compositor_.next_gpu_timing_query = 0;
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
    // Input polls can hit DirectInput device reads; throttle them so per-call
    // OpenXR interception stays cheap.
    const auto now = std::chrono::steady_clock::now();
    // First poll after a reset (session start, or the binding being (re)assigned)
    // primes the edge detector: a button still held from the bind gesture must
    // not register as a press, or it would flip depth off its enabled default.
    const bool first_poll = !depthxr_binding_last_poll_time_.has_value();
    if (first_poll || now - *depthxr_binding_last_poll_time_ >= kInputBindingPollInterval) {
        depthxr_binding_last_poll_time_ = now;
        depthxr_binding_down_cached_ = IsInputBindingDown(resolved_settings_.depthxr_bindings.toggle_enabled);
    }
    const bool binding_down = depthxr_binding_down_cached_;
    if (first_poll) {
        depthxr_toggle_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !depthxr_toggle_binding_was_down_;
    depthxr_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        depthxr_toggle_enabled_ = !depthxr_toggle_enabled_;
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
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
    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detector on the first poll after a reset so a button held
    // from the bind gesture does not register as a spurious activation press.
    const bool first_poll = !pivotxr_binding_last_poll_time_.has_value();
    if (first_poll || now - *pivotxr_binding_last_poll_time_ >= kInputBindingPollInterval) {
        pivotxr_binding_last_poll_time_ = now;
        pivotxr_binding_down_cached_ = IsInputBindingDown(settings.activation_binding);
    }
    const bool binding_down = pivotxr_binding_down_cached_;
    if (first_poll) {
        pivotxr_activation_key_was_down_ = binding_down;
    }
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
