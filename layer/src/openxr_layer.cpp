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
#include "depthxr/runtime_pacing.h"
#include "depthxr/seen_apps.h"
#include "depthxr/sound_player.h"
#include "depthxr/turbo_metrics.h"

// Injected by layer/CMakeLists.txt from the canonical app version in
// app/src-tauri/tauri.conf.json. Falls back when the layer is built without that
// definition (e.g. the test target) so the symbol always resolves.
#ifndef VECTORXR_VERSION
#define VECTORXR_VERSION "unknown"
#endif

namespace depthxr {
namespace {

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

// Seed table for Auto pacing. Known-interlocking runtimes get sequenced
// pacing up front (their xrWaitFrame can stall until the next submit — 0.12
// field reports on Oculus and Varjo; PiOpenXR observed during development);
// runtimes with async pipelining confirmed stable in the field start async.
// Unknown runtimes return nullopt and are probed async-first.
std::optional<TurboPacingMode> SeededTurboPacingMode(const std::string& runtime_name) {
    for (const char* fragment : {"Oculus", "Varjo", "PiOpenXR", "Pimax"}) {
        if (runtime_name.find(fragment) != std::string::npos) {
            return TurboPacingMode::kSequenced;
        }
    }
    if (runtime_name.find("SteamVR") != std::string::npos) {
        return TurboPacingMode::kAsync;
    }
    return std::nullopt;
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

// Density multiplier for the full-FOV composite canvas relative to the runtime's
// stereo-recommended resolution. The focus view is rendered at focus_scale x its
// FOV fraction of stereo; for the focus texture to sample 1:1 into its sub-rectangle
// of the canvas (rather than being downsampled back to stereo-native, which throws
// away the whole point of a high-density inset), the canvas must span the full FOV
// at focus_scale density. Never drop below 1.0 so the submitted layer keeps at least
// stereo-native quality. This mirrors OpenXR-Quad-Views-Foveated's focus_multiplier.
double QuadViewsCanvasDensity(const QuadViewsResolvedSettings& settings) {
    return std::max(1.0, settings.focus_scale);
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

const void* StripVarjoFoveatedViewLocateNextChain(const void* next) {
    const auto* header = reinterpret_cast<const XrBaseInStructure*>(next);
    if (!header) {
        return nullptr;
    }
    if (header->type == XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO) {
        return header->next;
    }
    if (!FindStructInChain(header->next, XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO)) {
        return next;
    }

    // XrViewLocateInfo chains are const app memory. If the Varjo node is not
    // first, drop the chain rather than mutating unknown extension structs to
    // splice it out before forwarding to non-Varjo runtimes.
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
                                           uint32_t focus_width,
                                           uint32_t focus_height,
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
    // Feathering is measured in output pixels, while sharpening samples the focus texture.
    constants.blend_params[0] = static_cast<float>(std::max(1.0 / std::max<uint32_t>(1, width),
                                                           focus_rect_width * transition_fraction));
    constants.blend_params[1] = static_cast<float>(std::max(1.0 / std::max<uint32_t>(1, height),
                                                           focus_rect_height * transition_fraction));
    constants.blend_params[2] = static_cast<float>(Clamp(foveate_sharpness, 0.0, 100.0) / 100.0);
    constants.blend_params[3] = 0.0f;
    constants.focus_texel[0] = 1.0f / static_cast<float>(std::max<uint32_t>(1, focus_width));
    constants.focus_texel[1] = 1.0f / static_cast<float>(std::max<uint32_t>(1, focus_height));
    // One output pixel expressed in focus-UV, so inline sharpening samples at output-pixel
    // spacing (frequencies that survive the focus->output resample) instead of focus-texel scale.
    constants.focus_texel[2] =
        static_cast<float>((1.0 / std::max<uint32_t>(1, width)) / focus_rect_width);
    constants.focus_texel[3] =
        static_cast<float>((1.0 / std::max<uint32_t>(1, height)) / focus_rect_height);
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
    float sharpenAmount = saturate(blendParams.z);
    if (sharpenAmount > 0.001) {
        // Sample neighbors at output-pixel spacing (focusTexel.zw is one output pixel expressed
        // in focus-UV), not focus-texel spacing. The focus texture is minified into the focus
        // rect, so a focus-texel-scale unsharp mask synthesizes detail that the resample averages
        // away; output-space offsets target the frequencies the user actually sees.
        float2 off = focusTexel.zw;
        float3 n = focusTexture.Sample(linearSampler, saturate(focusUv + float2(0.0, -off.y))).rgb;
        float3 s = focusTexture.Sample(linearSampler, saturate(focusUv + float2(0.0,  off.y))).rgb;
        float3 e = focusTexture.Sample(linearSampler, saturate(focusUv + float2( off.x, 0.0))).rgb;
        float3 w = focusTexture.Sample(linearSampler, saturate(focusUv + float2(-off.x, 0.0))).rgb;
        float3 avg = (n + s + e + w) * 0.25;
        float3 sharpened = focus.rgb + (focus.rgb - avg) * sharpenAmount;
        // CAS-style clamp: keep the result inside the local neighborhood range so strong
        // sharpening cannot ring into bright halos on high-contrast edges (cockpit text/MFDs).
        float3 lo = min(focus.rgb, min(min(n, s), min(e, w)));
        float3 hi = max(focus.rgb, max(max(n, s), max(e, w)));
        focus.rgb = clamp(sharpened, lo, hi);
    }

    float2 distToEdge = min(uv - focusRect.xy, focusRect.zw - uv);
    float edgeAlpha = saturate(min(distToEdge.x / max(blendParams.x, 0.0001), distToEdge.y / max(blendParams.y, 0.0001)));
    edgeAlpha = edgeAlpha * edgeAlpha * (3.0 - 2.0 * edgeAlpha);
    return lerp(peripheral, focus, edgeAlpha);
}
)";
}

// Constants for the standalone focus sharpen pass (Varjo compatible quadviews).
struct FocusSharpenConstants {
    float params[4];   // x = sharpen amount [0,1], y/z = neighbor sample offset in UV, w = unused
    float src_rect[4]; // xy = source UV offset, zw = source UV scale (the focus subImage rect)
};

// Neighbor sampling radius for the Varjo focus sharpen, in source texels. Unlike the
// compositor path there is no minification into a smaller output rect here — the focus
// view is forwarded to the runtime near display-native — so a 1-texel unsharp lands at
// the runtime's panel-resample Nyquist and gets averaged away (the same reason the
// compositor path samples at output-pixel spacing; see D3D11QuadViewsShaderSource). A
// few-texel radius targets the frequencies that survive the resample, reproducing the
// visible-but-gentle sharpen of the compositor path while the CAS clamp still prevents
// halos. Tunable; 3.0 matches the effective radius of the compositor path in testing.
constexpr float kVarjoFocusSharpenRadiusTexels = 3.0f;

// Frames of continuous "sharpen requested but nothing sharpened" before the skip
// diagnostic is logged. Filters startup transients (first frames can legitimately
// skip before the focus swapchains have a released image) so the one-shot log
// only fires for a persistent condition.
constexpr uint32_t kFocusSharpenSkipLogFrames = 90;

// Standalone 1:1 CAS sharpen over a focus view. Unlike the compositor shader this
// does not blend a peripheral view or resample into a sub-rect. Neighbours are taken
// at a widened offset (params.yz, a few source texels — see kVarjoFocusSharpenRadiusTexels)
// so the boost survives the runtime's resample of the focus view onto its panel.
const char* D3D11FocusSharpenShaderSource() {
    return R"(
cbuffer FocusSharpenConstants : register(b0) {
    float4 params;
    float4 srcRect;
};

Texture2D focusTexture : register(t0);
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
    float2 srcUv = srcRect.xy + uv * srcRect.zw;
    float4 focus = focusTexture.Sample(linearSampler, srcUv);
    float amount = saturate(params.x);
    if (amount > 0.001) {
        float2 off = params.yz;
        float3 n = focusTexture.Sample(linearSampler, srcUv + float2(0.0, -off.y)).rgb;
        float3 s = focusTexture.Sample(linearSampler, srcUv + float2(0.0,  off.y)).rgb;
        float3 e = focusTexture.Sample(linearSampler, srcUv + float2( off.x, 0.0)).rgb;
        float3 w = focusTexture.Sample(linearSampler, srcUv + float2(-off.x, 0.0)).rgb;
        float3 avg = (n + s + e + w) * 0.25;
        float3 sharpened = focus.rgb + (focus.rgb - avg) * amount;
        // CAS-style clamp to the local neighbourhood so strong sharpening cannot ring
        // into halos on high-contrast edges (cockpit text/MFDs).
        float3 lo = min(focus.rgb, min(min(n, s), min(e, w)));
        float3 hi = max(focus.rgb, max(max(n, s), max(e, w)));
        focus.rgb = clamp(sharpened, lo, hi);
    }
    return focus;
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

ViewOrientation ToViewOrientation(const XrQuaternionf& orientation) {
    return {orientation.x, orientation.y, orientation.z, orientation.w};
}

double ExtractPoseYawRadians(const XrPosef& pose) {
    return ExtractYawRadians(ToViewOrientation(pose.orientation));
}

double ExtractPosePitchRadians(const XrPosef& pose) {
    return ExtractPitchRadians(ToViewOrientation(pose.orientation));
}

// Wraps an angle to [-pi, pi]; origin-relative yaw deltas must not jump when
// the raw yaw crosses the +/-pi seam.
double WrapRadians(double angle) {
    constexpr double kPi = 3.14159265358979323846;
    while (angle > kPi) {
        angle -= 2.0 * kPi;
    }
    while (angle < -kPi) {
        angle += 2.0 * kPi;
    }
    return angle;
}

void AppendPoseSummary(std::ostringstream& stream, std::string_view label, const XrPosef& pose) {
    stream << label << "Pos=(" << FormatDiagnosticDouble(pose.position.x) << ", "
           << FormatDiagnosticDouble(pose.position.y) << ", "
           << FormatDiagnosticDouble(pose.position.z) << ")"
           << " " << label << "Yaw=" << FormatDiagnosticDouble(ExtractPoseYawRadians(pose))
           << " " << label << "Pitch=" << FormatDiagnosticDouble(ExtractPosePitchRadians(pose));
}

void AppendPoseSummary(std::ostringstream& stream, std::string_view label, const ViewAdjustmentData& view) {
    stream << label << "Pos=(" << FormatDiagnosticDouble(view.position.x) << ", "
           << FormatDiagnosticDouble(view.position.y) << ", "
           << FormatDiagnosticDouble(view.position.z) << ")"
           << " " << label << "Yaw=" << FormatDiagnosticDouble(ExtractYawRadians(view.orientation))
           << " " << label << "Pitch=" << FormatDiagnosticDouble(ExtractPitchRadians(view.orientation));
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
    const XrVector3f rotated_position = RotateVector(parent_orientation, local_pose.position);
    return {
        NormalizeQuaternion(MultiplyQuaternion(parent_orientation, local_pose.orientation)),
        {
            rotated_position.x + parent_pose.position.x,
            rotated_position.y + parent_pose.position.y,
            rotated_position.z + parent_pose.position.z,
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

    // Extra pitch must rotate about the view's own right axis, not the
    // reference space's X axis; a fixed world-X pitch inverts (and rolls) for
    // users whose seated forward is yawed away from the space's forward.
    // Yaw(heading + extraYaw) * Pitch(extraPitch) * Yaw(-heading) pitches
    // about the right axis of the final (extra-yawed) view heading, and
    // reduces to the plain Yaw * Pitch composition when heading is zero.
    const float heading_radians = static_cast<float>(ExtractPoseYawRadians(pose));
    const XrQuaternionf extra_rotation = NormalizeQuaternion(
        MultiplyQuaternion(YawQuaternion(heading_radians + extra_yaw_radians),
                           MultiplyQuaternion(PitchQuaternion(extra_pitch_radians),
                                              YawQuaternion(-heading_radians))));
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

// Stepped response: discrete extra rotation added per step_trigger of head
// angle beyond the deadzone, with hysteresis so the view does not oscillate
// when the head rests near a threshold. `current_step` is signed persistent
// state (positive = positive rotation direction); the regular smoothing eases
// the view between step targets.
void ComputePivotSteppedExtraAngleRadians(double current_angle_radians,
                                          double deadzone_degrees,
                                          double step_trigger_degrees,
                                          double step_amount_degrees,
                                          double step_hysteresis_degrees,
                                          double max_extra_degrees,
                                          double smoothing,
                                          double delta_seconds,
                                          int& current_step,
                                          double& smoothed_extra_angle_radians) {
    const double deadzone = DegreesToRadians(std::max(0.0, deadzone_degrees));
    // Clamp the trigger to a sane floor so a mis-set 0 cannot spin the loop.
    const double trigger = DegreesToRadians(std::max(0.5, step_trigger_degrees));
    const double amount = DegreesToRadians(std::max(0.0, step_amount_degrees));
    // Hysteresis must stay below the trigger spacing or a step could re-engage
    // the moment it releases.
    const double hysteresis =
        std::min(DegreesToRadians(std::max(0.0, step_hysteresis_degrees)), trigger * 0.9);
    const double max_extra = DegreesToRadians(std::max(0.0, max_extra_degrees));

    // Walk down while the head has come back inside the release threshold of
    // the current step (crossing zero when the head swings to the other side).
    while (current_step != 0) {
        const int sign = current_step > 0 ? 1 : -1;
        const double release_threshold = deadzone + std::abs(current_step) * trigger - hysteresis;
        if (current_angle_radians * sign < release_threshold) {
            current_step -= sign;
        } else {
            break;
        }
    }

    // Walk up in the direction of the current angle.
    {
        const int sign = current_angle_radians >= 0.0 ? 1 : -1;
        if (current_step == 0 || (current_step > 0) == (sign > 0)) {
            while (current_angle_radians * sign >= deadzone + (std::abs(current_step) + 1) * trigger) {
                current_step += sign;
            }
        }
    }

    double target_extra_angle = current_step * amount;
    if (max_extra > 0.0) {
        target_extra_angle = Clamp(target_extra_angle, -max_extra, max_extra);
    }

    const double blend = ComputeTimeBasedBlend(smoothing, delta_seconds);
    smoothed_extra_angle_radians += (target_extra_angle - smoothed_extra_angle_radians) * blend;
    if (current_step == 0 && NearlyZero(smoothed_extra_angle_radians)) {
        smoothed_extra_angle_radians = 0.0;
    }
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

// Pivot activation envelope easing duration is per-profile
// (PivotXrResolvedSettings::activation_ramp_seconds, default 0.35s). It decouples
// the on/off feel from the per-frame tracking smoothing so enabling pivot while
// the head is already turned never snaps the view.
constexpr double kPivotActivationGainEpsilon = 0.0001;
constexpr XrTime kMaxPivotPoseDeltaMatchWindow = 5'000'000;
constexpr XrTime kMaxQuadViewsFovMatchWindow = 5'000'000;
constexpr size_t kMaxCachedQuadViewsFovFrames = 180;
constexpr uint32_t kPivotDiagnosticBurstCount = 8;
constexpr uint64_t kPivotDiagnosticStride = 120;
// Consecutive unavailable frames before the eye-gaze focus is logged as lost.
// Debounces sub-second dropouts (e.g. blinks, which invalidate the gaze pose for
// ~100-400ms) so transient gaze loss does not churn the log. ~1/3s at 90Hz.
constexpr uint32_t kEyeGazeUnavailableLogThreshold = 30;
constexpr std::chrono::seconds kQuadViewsDebugHeartbeatInterval{2};
// Hot-path throttles. xrLocateSpace/xrLocateViews/xrEndFrame run multiple
// times per frame; filesystem stats, input-device polls, and downstream
// helper calls must not.
constexpr std::chrono::milliseconds kConfigCheckInterval{500};
constexpr std::chrono::milliseconds kInputBindingPollInterval{30};
constexpr std::chrono::milliseconds kAppActionSyncFreshWindow{100};

bool SameInputBinding(const InputBinding& lhs, const InputBinding& rhs);

bool SamePivotAxisTuning(const PivotAxisTuning& lhs, const PivotAxisTuning& rhs) {
    return NearlyEqual(lhs.rotation_multiplier, rhs.rotation_multiplier) &&
           NearlyEqual(lhs.deadzone_degrees, rhs.deadzone_degrees) &&
           NearlyEqual(lhs.max_extra_degrees, rhs.max_extra_degrees);
}

bool SamePivotResolvedProfile(const PivotXrResolvedProfile& lhs, const PivotXrResolvedProfile& rhs) {
    return lhs.name == rhs.name &&
           lhs.activation_mode == rhs.activation_mode &&
           SameInputBinding(lhs.activation_binding, rhs.activation_binding) &&
           SameInputBinding(lhs.set_origin_binding, rhs.set_origin_binding) &&
           SameInputBinding(lhs.release_origin_binding, rhs.release_origin_binding) &&
           NearlyEqual(lhs.smoothing, rhs.smoothing) &&
           NearlyEqual(lhs.activation_ramp_seconds, rhs.activation_ramp_seconds) &&
           NearlyEqual(lhs.yaw_rotation_multiplier, rhs.yaw_rotation_multiplier) &&
           NearlyEqual(lhs.yaw_deadzone_degrees, rhs.yaw_deadzone_degrees) &&
           NearlyEqual(lhs.yaw_max_extra_degrees, rhs.yaw_max_extra_degrees) &&
           NearlyEqual(lhs.pitch_rotation_multiplier, rhs.pitch_rotation_multiplier) &&
           NearlyEqual(lhs.pitch_deadzone_degrees, rhs.pitch_deadzone_degrees) &&
           NearlyEqual(lhs.pitch_max_extra_degrees, rhs.pitch_max_extra_degrees) &&
           lhs.response_mode == rhs.response_mode &&
           NearlyEqual(lhs.step_trigger_degrees, rhs.step_trigger_degrees) &&
           NearlyEqual(lhs.step_amount_degrees, rhs.step_amount_degrees) &&
           NearlyEqual(lhs.step_hysteresis_degrees, rhs.step_hysteresis_degrees) &&
           SamePivotAxisTuning(lhs.yaw_positive, rhs.yaw_positive) &&
           SamePivotAxisTuning(lhs.yaw_negative, rhs.yaw_negative) &&
           SamePivotAxisTuning(lhs.pitch_positive, rhs.pitch_positive) &&
           SamePivotAxisTuning(lhs.pitch_negative, rhs.pitch_negative);
}

bool SamePivotResolvedSettings(const PivotXrResolvedSettings& lhs, const PivotXrResolvedSettings& rhs) {
    if (lhs.enabled != rhs.enabled || lhs.profiles.size() != rhs.profiles.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.profiles.size(); ++i) {
        if (!SamePivotResolvedProfile(lhs.profiles[i], rhs.profiles[i])) {
            return false;
        }
    }
    return true;
}

bool SameSettings(const ResolvedRuntimeConfig& lhs, const ResolvedRuntimeConfig& rhs) {
    return lhs.core.enabled == rhs.core.enabled &&
           lhs.core.log_level == rhs.core.log_level &&
           lhs.core.log_retention_files == rhs.core.log_retention_files &&
           lhs.core.track_seen_apps == rhs.core.track_seen_apps &&
           lhs.depthxr.enabled == rhs.depthxr.enabled &&
           NearlyEqual(lhs.depthxr.stereo_boost, rhs.depthxr.stereo_boost) &&
           NearlyEqual(lhs.depthxr.convergence, rhs.depthxr.convergence) &&
           lhs.depthxr_bindings.toggle_enabled.type == rhs.depthxr_bindings.toggle_enabled.type &&
           lhs.depthxr_bindings.toggle_enabled.chord == rhs.depthxr_bindings.toggle_enabled.chord &&
           lhs.depthxr_bindings.toggle_enabled.device_guid == rhs.depthxr_bindings.toggle_enabled.device_guid &&
           lhs.depthxr_bindings.toggle_enabled.input_path == rhs.depthxr_bindings.toggle_enabled.input_path &&
           lhs.depthxr_bindings.toggle_enabled.product_guid == rhs.depthxr_bindings.toggle_enabled.product_guid &&
           lhs.depthxr_bindings.toggle_enabled.device_name == rhs.depthxr_bindings.toggle_enabled.device_name &&
           lhs.depthxr_bindings.toggle_enabled.input_label == rhs.depthxr_bindings.toggle_enabled.input_label &&
           SamePivotResolvedSettings(lhs.pivotxr, rhs.pivotxr) &&
           lhs.turbo.enabled == rhs.turbo.enabled &&
           SameInputBinding(lhs.turbo.toggle_binding, rhs.turbo.toggle_binding) &&
           lhs.turbo.pacing_mode == rhs.turbo.pacing_mode &&
           lhs.turbo.runtime_pins == rhs.turbo.runtime_pins &&
           lhs.turbo.metrics_mode == rhs.turbo.metrics_mode &&
           SameInputBinding(lhs.turbo.metrics_binding, rhs.turbo.metrics_binding) &&
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
           lhs.input_label == rhs.input_label &&
           lhs.sound.enabled == rhs.sound.enabled &&
           lhs.sound.activate_sound == rhs.sound.activate_sound &&
           lhs.sound.deactivate_sound == rhs.sound.deactivate_sound;
}

// True when the activation surface (candidate count, modes, bindings) is
// unchanged, so runtime toggle/edge state can survive a config hot-reload.
bool SamePivotActivationSet(const PivotXrResolvedSettings& lhs, const PivotXrResolvedSettings& rhs) {
    if (lhs.enabled != rhs.enabled || lhs.profiles.size() != rhs.profiles.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.profiles.size(); ++i) {
        if (lhs.profiles[i].activation_mode != rhs.profiles[i].activation_mode ||
            !SameInputBinding(lhs.profiles[i].activation_binding, rhs.profiles[i].activation_binding) ||
            !SameInputBinding(lhs.profiles[i].set_origin_binding, rhs.profiles[i].set_origin_binding) ||
            !SameInputBinding(lhs.profiles[i].release_origin_binding, rhs.profiles[i].release_origin_binding)) {
            return false;
        }
    }
    return true;
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
        const std::string label = "view" + std::to_string(i);
        stream << " ";
        AppendPoseSummary(stream, label, views[i]);
        stream << " " << label << "Fov=(" << FormatDiagnosticDouble(views[i].fov.angle_left) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_right) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_up) << ", "
               << FormatDiagnosticDouble(views[i].fov.angle_down) << ")"
               << " " << label << "ProjCenter=" << FormatDiagnosticDouble(HorizontalProjectionCenter(views[i].fov));
    }
}

} // namespace

OpenXrLayer& OpenXrLayer::Instance() {
    static OpenXrLayer layer;
    return layer;
}

OpenXrLayer::~OpenXrLayer() {
    // Safety net for apps that exit without xrDestroyInstance: join the watcher
    // before members are destroyed so a still-joinable std::thread can't trip
    // terminate(). The watcher only touches this object's own members, all of
    // which outlive this destructor body.
    StopConfigWatcher();
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
                                        bool eye_gaze_extension_enabled,
                                        const InstanceCreateDiagnostics& diagnostics) {
    std::scoped_lock lock(mutex_);

    instance_ = instance;
    ResetInstanceState();
    eye_gaze_extension_enabled_ = eye_gaze_extension_enabled;
    varjo_compatible_quadviews_active_ = diagnostics.varjo_compatible_quad_forwarded;
    ResetSessionState();
    config_path_ = ResolveConfigPath();
    log_path_ = ResolveLogPath();
    logger_.Initialize(log_path_);

    current_exe_name_ = GetCurrentExecutableName();
    logger_.Info(std::string("VectorXR layer version: ") + VECTORXR_VERSION);
    logger_.Info("VectorXR attached to process: " + current_exe_name_);

    if (create_info) {
        quad_views_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
        varjo_foveated_rendering_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);

        std::ostringstream stream;
        const XrVersion api_version = create_info->applicationInfo.apiVersion;
        stream << "Application=" << create_info->applicationInfo.applicationName << ", Engine="
               << create_info->applicationInfo.engineName
               << ", AppVersion=" << create_info->applicationInfo.applicationVersion
               << ", OpenXRApiVersion=" << XR_VERSION_MAJOR(api_version) << "."
               << XR_VERSION_MINOR(api_version) << "." << XR_VERSION_PATCH(api_version);
        logger_.Info(stream.str());

        std::ostringstream ext_stream;
        ext_stream << "Application enabled OpenXR extensions (" << create_info->enabledExtensionCount << "):";
        for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
            if (create_info->enabledExtensionNames && create_info->enabledExtensionNames[i]) {
                ext_stream << ' ' << create_info->enabledExtensionNames[i];
            }
        }
        logger_.Info(ext_stream.str());
        if (quad_views_extension_requested_ || varjo_foveated_rendering_extension_requested_) {
            logger_.Info(std::string("Application requested layer-owned extensions: quadViews=") +
                         (quad_views_extension_requested_ ? "true" : "false") +
                         ", varjoFoveatedRendering=" +
                         (varjo_foveated_rendering_extension_requested_ ? "true" : "false"));
        }
    }
    if (varjo_compatible_quadviews_active_) {
        logger_.Info(
            "Quadviews Varjo compatible mode ACTIVE: forwarded native quad views to the runtime so the "
            "focus panels are driven directly; stereo composite and quad->stereo synthesis are disabled "
            "for this instance. Applied knobs: focusScale/peripheralScale (view resolution) and "
            "foveateSharpness (focus CAS post-process, runs only when > 0). Runtime-owned in this mode: "
            "focus size, transition, offsets, gaze smoothing/deadzone.");
    } else if (quad_views_extension_requested_ || varjo_foveated_rendering_extension_requested_) {
        logger_.Info(
            "Quadviews mode: stereo-composite emulation (Varjo compatible mode not active — runtime lacks "
            "native quad views or quadviews is disabled).");
    }

    // Full forward-decision trace. This is the record needed to diagnose a Varjo
    // headset that stays in emulation: it captures every input to the forward
    // decision plus the raw pre-instance extension list the probe saw. Emitted at
    // instance creation (before any frame), so a short launch-and-quit capture
    // preserves it under the debug-report size cap.
    if (diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering) {
        std::ostringstream decision;
        decision << "Varjo compatible forwarding decision: appRequestedQuad="
                 << (diagnostics.app_requested_quad_views ? 1 : 0) << ", appRequestedVarjoFoveated="
                 << (diagnostics.app_requested_varjo_foveated_rendering ? 1 : 0)
                 << ", eligible=" << (diagnostics.varjo_compatible_eligible ? 1 : 0)
                 << ", extScanRan=" << (diagnostics.pre_instance_extension_scan_ran ? 1 : 0)
                 << ", extScanResult=" << static_cast<int>(diagnostics.pre_instance_extension_scan_result)
                 << ", extCount=" << diagnostics.pre_instance_extension_count
                 << ", runtimeAdvertisesVarjoQuad=" << (diagnostics.runtime_advertises_varjo_quad ? 1 : 0)
                 << ", runtimeAdvertisesVarjoFoveated=" << (diagnostics.runtime_advertises_varjo_foveated ? 1 : 0)
                 << ", activeRuntimeIsVarjo=" << (diagnostics.active_runtime_is_varjo ? 1 : 0)
                 << ", forwarded=" << (diagnostics.varjo_compatible_quad_forwarded ? 1 : 0);
        logger_.Info(decision.str());
        logger_.Info("Active OpenXR runtime manifest: [" + diagnostics.active_runtime_path + "]");
        logger_.Info("Pre-instance runtime extensions seen by layer: [" + diagnostics.pre_instance_extensions +
                     "]");
        if (quad_views_extension_requested_ && !diagnostics.runtime_advertises_varjo_quad) {
            logger_.Info(
                "DIAGNOSTIC: the application enabled XR_VARJO_quad_views but the layer's pre-instance probe "
                "did not report it — confirming the pre-instance extension probe is unreliable here. Varjo "
                "detection now uses the active OpenXR runtime instead (see activeRuntimeIsVarjo).");
        }
    }

    const bool log_eye_gaze_instance_setup =
        diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering ||
        diagnostics.app_requested_eye_gaze || diagnostics.optimistic_eye_gaze_request ||
        XR_FAILED(diagnostics.first_create_result) || diagnostics.retried_without_eye_gaze;
    if (log_eye_gaze_instance_setup) {
        std::ostringstream stream;
        stream << "Quadviews eye-gaze instance setup: appRequestedQuadViews="
               << (diagnostics.app_requested_quad_views ? 1 : 0)
               << ", appRequestedVarjoFoveatedRendering="
               << (diagnostics.app_requested_varjo_foveated_rendering ? 1 : 0)
               << ", appRequestedEyeGaze=" << (diagnostics.app_requested_eye_gaze ? 1 : 0);
        if (diagnostics.cheap_eye_gaze_probe_ran) {
            stream << ", cheapProbeSupported=" << (diagnostics.cheap_eye_gaze_probe_supported ? 1 : 0);
        } else {
            stream << ", cheapProbeSupported=not-run";
        }
        stream << ", optimisticRequest=" << (diagnostics.optimistic_eye_gaze_request ? 1 : 0)
               << ", firstCreateResult=" << static_cast<int>(diagnostics.first_create_result)
               << ", firstDownstreamExtensionCount=" << diagnostics.first_downstream_extension_count
               << ", retriedWithoutEyeGaze=" << (diagnostics.retried_without_eye_gaze ? 1 : 0);
        if (diagnostics.retried_without_eye_gaze) {
            stream << ", retryCreateResult=" << static_cast<int>(diagnostics.retry_create_result);
        }
        stream << ", finalDownstreamExtensionCount=" << diagnostics.final_downstream_extension_count
               << ", finalEyeGazeEnabled=" << (eye_gaze_extension_enabled ? 1 : 0);
        logger_.Info(stream.str());
    }
    if (eye_gaze_extension_enabled_) {
        logger_.Info("Enabled XR_EXT_eye_gaze_interaction downstream for VectorXR quadviews.");
    } else if (diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering) {
        logger_.Info("XR_EXT_eye_gaze_interaction is not enabled downstream; VectorXR quadviews eye tracking "
                     "will use head/static focus if no other tracker is available.");
    }

    CaptureInstanceFunctions();
    if (!next_destroy_instance_ || !next_create_session_ || !next_destroy_session_ || !next_begin_session_ ||
        !next_attach_session_action_sets_ || !next_sync_actions_ ||
        !next_end_frame_ || !next_get_system_properties_ || !next_enumerate_environment_blend_modes_ ||
        !next_enumerate_view_configurations_ || !next_get_view_configuration_properties_ ||
        !next_enumerate_view_configuration_views_ ||
        !next_enumerate_swapchain_formats_ ||
        !next_create_swapchain_ || !next_destroy_swapchain_ || !next_enumerate_swapchain_images_ ||
        !next_acquire_swapchain_image_ || !next_wait_swapchain_image_ || !next_release_swapchain_image_ ||
        !next_enumerate_reference_spaces_ || !next_get_reference_space_bounds_rect_ ||
        !next_create_reference_space_ || !next_destroy_space_ ||
        !next_locate_space_ || !next_locate_views_ ||
        !next_wait_frame_ || !next_begin_frame_) {
        logger_.Error("Failed to resolve required OpenXR function pointers");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    if (next_get_instance_properties_) {
        XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
        const XrResult props_result = next_get_instance_properties_(instance_, &instance_properties);
        if (XR_SUCCEEDED(props_result)) {
            runtime_name_ = instance_properties.runtimeName;
            defer_quadviews_swapchain_releases_ = runtime_name_.find("Varjo") != std::string::npos;
            const XrVersion rt = instance_properties.runtimeVersion;
            runtime_version_ = std::to_string(XR_VERSION_MAJOR(rt)) + "." +
                               std::to_string(XR_VERSION_MINOR(rt)) + "." +
                               std::to_string(XR_VERSION_PATCH(rt));
            logger_.Info(std::string("OpenXR runtime: name=\"") + runtime_name_ +
                         "\", version=" + runtime_version_);
            if (defer_quadviews_swapchain_releases_) {
                logger_.Info("Varjo runtime detected; deferring app quadviews swapchain releases until xrEndFrame.");
            }
        } else {
            logger_.Info("OpenXR runtime identity unavailable: xrGetInstanceProperties returned " +
                         std::to_string(static_cast<int>(props_result)));
        }
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

    // Hand ongoing config hot-reload to the watcher thread now that the initial
    // load and config_path_ are established. From here the render hot path never
    // touches the filesystem for config.
    StartConfigWatcher();
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
    // Join the watcher before taking mutex_: it may be mid-PollConfigFile
    // holding the lock, so stopping it under mutex_ would deadlock.
    StopConfigWatcher();

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
        next_enumerate_swapchain_formats_ = nullptr;
        next_create_swapchain_ = nullptr;
        next_destroy_swapchain_ = nullptr;
        next_enumerate_swapchain_images_ = nullptr;
        next_acquire_swapchain_image_ = nullptr;
        next_wait_swapchain_image_ = nullptr;
        next_release_swapchain_image_ = nullptr;
        next_enumerate_reference_spaces_ = nullptr;
        next_get_reference_space_bounds_rect_ = nullptr;
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
    {
        const void* graphics_binding = create_info ? create_info->next : nullptr;
        const auto* next_struct = static_cast<const XrBaseInStructure*>(graphics_binding);
        logger_.Info(std::string("xrCreateSession requested by application: graphicsBindingType=") +
                     (next_struct ? std::to_string(next_struct->type) : "none"));
    }

    const XrResult result = next_create_session_(instance, create_info, session);
    if (XR_FAILED(result) || !session) {
        if (XR_FAILED(result)) {
            logger_.Error("xrCreateSession failed downstream: result=" + std::to_string(static_cast<int>(result)));
        }
        return result;
    }
    logger_.Info("xrCreateSession succeeded downstream; configuring VectorXR session resources.");

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
    // Join any in-flight turbo async wait before tearing the session down.
    ResetTurboFrameState();
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            DestroyEyeGazeResources();
            DestroyInternalReferenceSpaces();
            FlushDeferredSwapchainReleasesLocked("session teardown");
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
    // In Varjo compatible mode the runtime supports the quad configuration itself, so
    // begin the session on the app's real config and let the runtime drive the
    // focus panels. Only remap to stereo when we are emulating.
    if (begin_info && IsQuadViewConfiguration(begin_info->primaryViewConfigurationType) && quadviews_active &&
        !varjo_compatible_quadviews_active_) {
        runtime_begin_info = *begin_info;
        runtime_begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        downstream_begin_info = &runtime_begin_info;
    }

    logger_.Info(std::string("xrBeginSession requested by application: appViewConfig=") +
                 (begin_info ? ToString(begin_info->primaryViewConfigurationType) : "null") +
                 ", runtimeViewConfig=" +
                 (downstream_begin_info ? ToString(downstream_begin_info->primaryViewConfigurationType) : "null"));

    const XrResult result = next_begin_session_(session, downstream_begin_info);
    if (XR_FAILED(result) || !begin_info) {
        if (XR_FAILED(result)) {
            logger_.Error("xrBeginSession failed downstream: result=" + std::to_string(static_cast<int>(result)));
        }
        return result;
    }

    std::scoped_lock lock(mutex_);
    active_session_ = session;
    session_begin_wall_time_ = std::chrono::steady_clock::now();
    active_primary_view_configuration_type_ = begin_info->primaryViewConfigurationType;
    active_runtime_view_configuration_type_ = downstream_begin_info->primaryViewConfigurationType;
    has_active_primary_view_configuration_ = true;
    has_logged_quad_view_short_count_ = false;

    logger_.Info(std::string("Session began with view configuration: ") +
                 ToString(active_primary_view_configuration_type_) +
                 (active_primary_view_configuration_type_ != active_runtime_view_configuration_type_
                      ? std::string(" (runtime mapped to ") + ToString(active_runtime_view_configuration_type_) + ")"
                      : std::string()));

    // Unmissable, plain-language quadviews state for this session, so it can be
    // confirmed with a single grep instead of inferring it from view-config names
    // or the desktop-mirror appearance.
    if (IsQuadViewsActive()) {
        if (IsQuadViewConfiguration(active_primary_view_configuration_type_)) {
            logger_.Info(std::string("Quadviews ACTIVE (foveated inset, ") +
                         (varjo_compatible_quadviews_active_ ? "Varjo compatible mode)."
                                                             : "stereo-composite emulation)."));
        } else {
            logger_.Info(
                "Quadviews INACTIVE (application began a STEREO session; it did not select the foveated-inset "
                "configuration). If you expected quadviews, this usually means the app must be restarted after "
                "enabling quadviews.");
        }
    }
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

    logger_.Info("xrAttachSessionActionSets requested by application: appActionSets=" +
                 std::to_string(attach_info->countActionSets) +
                 ", appendedEyeGazeSet=" + (appended_eye_gaze_set ? "1" : "0"));

    const XrResult result = next_attach_session_action_sets_(session, &downstream_attach_info);
    if (XR_FAILED(result)) {
        logger_.Error("xrAttachSessionActionSets failed downstream: result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }
    if (appended_eye_gaze_set) {
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

    const bool app_queried_varjo_foveation =
        FindStructInChain(properties->next, XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO) != nullptr;
    if (!has_logged_system_properties_) {
        std::ostringstream stream;
        stream << "OpenXR system: name=\"" << properties->systemName << "\""
               << ", vendorId=" << properties->vendorId
               << ", maxSwapchainImage=" << properties->graphicsProperties.maxSwapchainImageWidth << "x"
               << properties->graphicsProperties.maxSwapchainImageHeight
               << ", maxLayerCount=" << properties->graphicsProperties.maxLayerCount
               << ", orientationTracking=" << (properties->trackingProperties.orientationTracking ? 1 : 0)
               << ", positionTracking=" << (properties->trackingProperties.positionTracking ? 1 : 0)
               << ", appQueriedVarjoFoveation=" << (app_queried_varjo_foveation ? 1 : 0)
               << ", quadViewsActive=" << (IsQuadViewsActive() ? 1 : 0);
        logger_.Info(stream.str());
        has_logged_system_properties_ = true;
    }

    if (!IsQuadViewsActive()) {
        return result;
    }

    void* foveated = FindMutableStructInChain(properties->next, XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO);
    if (foveated) {
        reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(foveated)->supportsFoveatedRendering = XR_TRUE;
        logger_.Info("Reported supportsFoveatedRendering=TRUE to application (Varjo foveated-rendering emulation).");
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
        logger_.Error("xrEnumerateViewConfigurations failed downstream (count query): result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }

    std::vector<XrViewConfigurationType> runtime_types(runtime_count);
    if (runtime_count > 0) {
        result = next_enumerate_view_configurations_(
            instance, system_id, runtime_count, &runtime_count, runtime_types.data());
        if (XR_FAILED(result)) {
            logger_.Error("xrEnumerateViewConfigurations failed downstream (populate): result=" +
                          std::to_string(static_cast<int>(result)));
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
    const bool synthesize_quad = IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsActive() &&
                                 !varjo_compatible_quadviews_active_;
    const XrViewConfigurationType runtime_type =
        synthesize_quad ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO : view_configuration_type;

    const XrResult result =
        next_get_view_configuration_properties_(instance, system_id, runtime_type, configuration_properties);
    if (XR_FAILED(result)) {
        logger_.Error("xrGetViewConfigurationProperties failed downstream: result=" +
                      std::to_string(static_cast<int>(result)) + ", appViewConfig=" +
                      ToString(view_configuration_type) + ", runtimeViewConfig=" + ToString(runtime_type));
        return result;
    }
    logger_.Info(std::string("xrGetViewConfigurationProperties: appViewConfig=") +
                 ToString(view_configuration_type) + ", runtimeViewConfig=" + ToString(runtime_type) +
                 ", synthesizeQuad=" + (synthesize_quad ? "1" : "0"));
    if (synthesize_quad && configuration_properties) {
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

    if (varjo_compatible_quadviews_active_) {
        // Varjo compatible mode: the runtime owns the quad view geometry (FOV, inset
        // size, count). Query it directly and, as a carryover of the emulation
        // knobs, rescale only the per-view recommended resolutions: peripheral
        // views (0/1) by peripheral_scale, focus views (2/3) by focus_scale. Every
        // other quadviews setting is runtime-owned in this mode.
        const XrResult native_result = next_enumerate_view_configuration_views_(
            instance, system_id, view_configuration_type, view_capacity_input, view_count_output, views);
        if (XR_FAILED(native_result) || !views || view_capacity_input == 0) {
            return native_result;
        }
        const double peripheral_scale = resolved_settings_.quadviews.peripheral_scale;
        const double focus_scale = resolved_settings_.quadviews.focus_scale;
        const uint32_t applied = std::min<uint32_t>(view_capacity_input, *view_count_output);
        std::ostringstream native_sizes;
        for (uint32_t i = 0; i < applied; ++i) {
            const bool is_focus = (i >= 2);
            const double scale = is_focus ? focus_scale : peripheral_scale;
            const uint32_t runtime_width = views[i].recommendedImageRectWidth;
            const uint32_t runtime_height = views[i].recommendedImageRectHeight;
            views[i].recommendedImageRectWidth =
                ScaleDimension(runtime_width, scale, views[i].maxImageRectWidth);
            views[i].recommendedImageRectHeight =
                ScaleDimension(runtime_height, scale, views[i].maxImageRectHeight);
            if (!has_logged_varjo_compatible_view_sizes_) {
                native_sizes << " view" << i << "=" << runtime_width << "x" << runtime_height << "->"
                             << views[i].recommendedImageRectWidth << "x"
                             << views[i].recommendedImageRectHeight << "(x" << FormatDiagnosticDouble(scale)
                             << ")";
            }
        }
        if (!has_logged_varjo_compatible_view_sizes_ && applied > 0) {
            logger_.Info("Quadviews Varjo compatible view sizes (runtime->rescaled): count=" +
                         std::to_string(applied) + ", peripheralScale=" +
                         FormatDiagnosticDouble(peripheral_scale) + ", focusScale=" +
                         FormatDiagnosticDouble(focus_scale) + native_sizes.str());
            has_logged_varjo_compatible_view_sizes_ = true;
        }
        return native_result;
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
        logger_.Error("xrEnumerateViewConfigurationViews: underlying stereo query failed while synthesizing "
                      "quad views: result=" + std::to_string(static_cast<int>(result)));
        return result;
    }
    if (stereo_count < 2) {
        logger_.Error("xrEnumerateViewConfigurationViews: runtime reported fewer than 2 stereo views (" +
                      std::to_string(stereo_count) + "); cannot synthesize quad views.");
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
    cached_quadviews_stereo_max_width_ =
        std::max(stereo_views[0].maxImageRectWidth, stereo_views[1].maxImageRectWidth);
    cached_quadviews_stereo_max_height_ =
        std::max(stereo_views[0].maxImageRectHeight, stereo_views[1].maxImageRectHeight);

    for (uint32_t i = 0; i < 2; ++i) {
        void* app_next = views[i].next;
        views[i] = stereo_views[i];
        views[i].next = app_next;
        views[i].recommendedImageRectWidth = ScaleDimension(stereo_views[i].recommendedImageRectWidth,
                                                            resolved_settings_.quadviews.peripheral_scale,
                                                            stereo_views[i].maxImageRectWidth);
        views[i].recommendedImageRectHeight = ScaleDimension(stereo_views[i].recommendedImageRectHeight,
                                                             resolved_settings_.quadviews.peripheral_scale,
                                                             stereo_views[i].maxImageRectHeight);
        SetFoveatedViewActive(views[i], XR_FALSE);
    }

    for (uint32_t i = 0; i < 2; ++i) {
        XrViewConfigurationView& focus_view = views[i + 2];
        void* app_next = focus_view.next;
        focus_view = stereo_views[i];
        focus_view.next = app_next;
        focus_view.recommendedImageRectWidth = ScaleDimension(stereo_views[i].recommendedImageRectWidth,
                                                              focus_width_scale,
                                                              stereo_views[i].maxImageRectWidth);
        focus_view.recommendedImageRectHeight = ScaleDimension(stereo_views[i].recommendedImageRectHeight,
                                                               focus_height_scale,
                                                               stereo_views[i].maxImageRectHeight);
        SetFoveatedViewActive(focus_view, XR_TRUE);
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

XrResult OpenXrLayer::EnumerateSwapchainFormats(XrSession session,
                                                uint32_t format_capacity_input,
                                                uint32_t* format_count_output,
                                                int64_t* formats) {
    const XrResult result = next_enumerate_swapchain_formats_(
        session, format_capacity_input, format_count_output, formats);

    if (XR_FAILED(result)) {
        logger_.Error("xrEnumerateSwapchainFormats failed downstream: result=" +
                      std::to_string(static_cast<int>(result)) +
                      ", capacityInput=" + std::to_string(format_capacity_input));
        return result;
    }

    // Log the returned format list once, on the populating call (capacity > 0).
    if (formats && format_capacity_input > 0 && format_count_output) {
        std::ostringstream stream;
        const uint32_t count = std::min<uint32_t>(*format_count_output, format_capacity_input);
        stream << "xrEnumerateSwapchainFormats returned " << count << " format(s):";
        for (uint32_t i = 0; i < count; ++i) {
            stream << ' ' << formats[i];
        }
        logger_.Info(stream.str());
    }
    return result;
}

XrResult OpenXrLayer::CreateSwapchain(XrSession session,
                                      const XrSwapchainCreateInfo* create_info,
                                      XrSwapchain* swapchain) {
    {
        std::ostringstream stream;
        stream << "xrCreateSwapchain requested by application:";
        if (create_info) {
            stream << " size=" << create_info->width << "x" << create_info->height
                   << ", arraySize=" << create_info->arraySize
                   << ", faceCount=" << create_info->faceCount
                   << ", mipCount=" << create_info->mipCount
                   << ", sampleCount=" << create_info->sampleCount
                   << ", format=" << create_info->format
                   << ", usage=" << FormatUsageFlags(create_info->usageFlags)
                   << ", createFlags=" << create_info->createFlags;
            const auto* next_struct = static_cast<const XrBaseInStructure*>(create_info->next);
            stream << ", nextChainType=" << (next_struct ? std::to_string(next_struct->type) : "none");
        } else {
            stream << " create_info=null";
        }
        logger_.Info(stream.str());
    }

    const XrResult result = next_create_swapchain_(session, create_info, swapchain);
    if (XR_FAILED(result)) {
        std::ostringstream stream;
        stream << "xrCreateSwapchain failed downstream: result=" << static_cast<int>(result);
        if (create_info) {
            stream << ", size=" << create_info->width << "x" << create_info->height
                   << ", arraySize=" << create_info->arraySize
                   << ", faceCount=" << create_info->faceCount
                   << ", mipCount=" << create_info->mipCount
                   << ", sampleCount=" << create_info->sampleCount
                   << ", format=" << create_info->format
                   << ", usage=" << FormatUsageFlags(create_info->usageFlags)
                   << ", createFlags=" << create_info->createFlags;
            const auto* next_struct = static_cast<const XrBaseInStructure*>(create_info->next);
            stream << ", nextChainType=" << (next_struct ? std::to_string(next_struct->type) : "none");
        }
        logger_.Error(stream.str());
        return result;
    }
    if (!create_info || !swapchain || *swapchain == XR_NULL_HANDLE) {
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
    // A pipelined turbo frame may still hold onto this swapchain; make sure the
    // async xrWaitFrame has finished before the swapchain goes away.
    DrainTurboAsyncWait();
    {
        std::scoped_lock lock(mutex_);
        auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            const XrResult flush_result =
                FlushDeferredSwapchainReleaseLocked(swapchain, it->second, "swapchain destroy");
            if (XR_FAILED(flush_result)) {
                return flush_result;
            }
        }
    }

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
    {
        std::scoped_lock lock(mutex_);
        auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            const XrResult flush_result =
                FlushDeferredSwapchainReleaseLocked(swapchain, it->second, "swapchain acquire");
            if (XR_FAILED(flush_result)) {
                return flush_result;
            }
        }
    }

    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: xrAcquireSwapchainImage(" +
                      std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + ") starting.");
    }
    const XrResult result = next_acquire_swapchain_image_(swapchain, acquire_info, index);
    if (diag) {
        logger_.Debug("Turbo-diag: xrAcquireSwapchainImage completed.");
    }
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
    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: xrWaitSwapchainImage(" +
                      std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + ") starting.");
    }
    const auto wait_start = std::chrono::steady_clock::now();
    const XrResult result = next_wait_swapchain_image_(swapchain, wait_info);
    if (diag) {
        logger_.Debug("Turbo-diag: xrWaitSwapchainImage completed in " +
                      std::to_string(std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - wait_start)
                                         .count()) +
                      "ms.");
    }
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
    {
        std::scoped_lock lock(mutex_);
        const auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            // Snapshot the released image index at the app's release moment —
            // the compositor must sample this frame's image even after the
            // (turbo-pipelined) app has acquired the next one.
            if (it->second.has_last_acquired_image_index) {
                it->second.last_released_image_index = it->second.last_acquired_image_index;
                it->second.has_last_released_image_index = true;
            }
            if (ShouldDeferSwapchainRelease(it->second)) {
                it->second.release_deferred = true;
                ++it->second.release_count;
                if (it->second.quadviews_session && it->second.release_count <= 3) {
                    LogSwapchainSummary(swapchain, it->second, "releaseDeferred");
                }
                return XR_SUCCESS;
            }
        }
    }

    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: xrReleaseSwapchainImage(" +
                      std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + ") starting.");
    }
    const XrResult result = next_release_swapchain_image_(swapchain, release_info);
    if (diag) {
        logger_.Debug("Turbo-diag: xrReleaseSwapchainImage completed.");
    }
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

    // Sample the image the app last RELEASED (this frame's content); under
    // turbo pipelining last_acquired may already point at the next frame.
    const auto source_image_index = [](const SwapchainInfo& info) {
        return info.has_last_released_image_index ? info.last_released_image_index
                                                  : info.last_acquired_image_index;
    };

    std::array<SwapchainInfo*, 4> swapchains{};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto it = tracked_swapchains_.find(source_layer->views[i].subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !(it->second.has_last_acquired_image_index || it->second.has_last_released_image_index) ||
            source_image_index(it->second) >= it->second.d3d11_images.size()) {
            return false;
        }
        swapchains[i] = &it->second;
    }

    const double peripheral_scale = resolved_settings_.quadviews.peripheral_scale;
    const double focus_width_scale = QuadViewsFocusWidthScale(resolved_settings_.quadviews);
    const double focus_height_scale = QuadViewsFocusHeightScale(resolved_settings_.quadviews);
    const double canvas_density = QuadViewsCanvasDensity(resolved_settings_.quadviews);

    // Reconstruct the runtime's stereo-native full-FOV resolution (preferring the value
    // cached during xrEnumerateViewConfigurationViews), then scale it by the focus density
    // so the high-density focus view lands 1:1 in its sub-rectangle of the composite.
    uint32_t stereo_full_width = cached_quadviews_stereo_recommended_width_;
    uint32_t stereo_full_height = cached_quadviews_stereo_recommended_height_;
    if (stereo_full_width == 0) {
        stereo_full_width = std::max({
            EstimateFullResolutionDimension(swapchains[0]->width, peripheral_scale),
            EstimateFullResolutionDimension(swapchains[1]->width, peripheral_scale),
            EstimateFullResolutionDimension(swapchains[2]->width, focus_width_scale),
            EstimateFullResolutionDimension(swapchains[3]->width, focus_width_scale),
        });
    }
    if (stereo_full_height == 0) {
        stereo_full_height = std::max({
            EstimateFullResolutionDimension(swapchains[0]->height, peripheral_scale),
            EstimateFullResolutionDimension(swapchains[1]->height, peripheral_scale),
            EstimateFullResolutionDimension(swapchains[2]->height, focus_height_scale),
            EstimateFullResolutionDimension(swapchains[3]->height, focus_height_scale),
        });
    }
    const uint32_t canvas_max_width =
        cached_quadviews_stereo_max_width_ > 0 ? cached_quadviews_stereo_max_width_ : stereo_full_width * 4;
    const uint32_t canvas_max_height =
        cached_quadviews_stereo_max_height_ > 0 ? cached_quadviews_stereo_max_height_ : stereo_full_height * 4;
    const uint32_t output_width = ScaleDimension(stereo_full_width, canvas_density, canvas_max_width);
    const uint32_t output_height = ScaleDimension(stereo_full_height, canvas_density, canvas_max_height);
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
                    d3d11_quadviews_compositor_.has_last_completed_gpu_timing = true;
                    d3d11_quadviews_compositor_.last_completed_gpu_ms = completed_gpu_ms;
                    d3d11_quadviews_compositor_.last_completed_gpu_frame_time = completed_gpu_frame_time;
                }
                query.issued = false;
                break;
            }
        }
    }

    const bool should_log_compositor_diagnostic =
        pending_quadviews_compositor_diagnostics_ > 0 ||
        ShouldLogQuadViewsDebugHeartbeat(last_quadviews_compositor_debug_heartbeat_);
    QuadViewsGpuTimingQuery* active_gpu_query = nullptr;
    if (should_log_compositor_diagnostic && d3d11_quadviews_compositor_.gpu_timing_available) {
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

        const uint32_t peripheral_index = source_image_index(*swapchains[eye]);
        const uint32_t focus_index = source_image_index(*swapchains[eye + 2]);
        ID3D11Texture2D* peripheral_source = swapchains[eye]->d3d11_images[peripheral_index];
        ID3D11Texture2D* focus_source = swapchains[eye + 2]->d3d11_images[focus_index];

        ID3D11ShaderResourceView* peripheral_resource = nullptr;
        ID3D11ShaderResourceView* focus_resource = nullptr;
        SwapchainInfo& peripheral_swapchain = *swapchains[eye];
        SwapchainInfo& focus_swapchain = *swapchains[eye + 2];
        if (EnsureD3D11SwapchainShaderResources(peripheral_swapchain) &&
            peripheral_index < peripheral_swapchain.d3d11_shader_resources.size()) {
            peripheral_resource = peripheral_swapchain.d3d11_shader_resources[peripheral_index];
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
            focus_index < focus_swapchain.d3d11_shader_resources.size()) {
            focus_resource = focus_swapchain.d3d11_shader_resources[focus_index];
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
                                                               focus_swapchain.width,
                                                               focus_swapchain.height,
                                                               resolved_settings_.quadviews.transition_thickness_percent,
                                                               resolved_settings_.quadviews.foveate_sharpness);
        if (should_log_compositor_diagnostic) {
            // Effective focus sampling ratio: focus texture pixels vs. the output pixels its
            // sub-rectangle occupies. ~1.0 = ideal 1:1; >1.0 = focus is being downsampled
            // (blur, the pre-fix behaviour); <1.0 = focus canvas is larger than needed.
            const double focus_rect_output_width =
                std::max(1.0, static_cast<double>(constants.focus_rect[2] - constants.focus_rect[0]) * target.width);
            const double focus_rect_output_height =
                std::max(1.0, static_cast<double>(constants.focus_rect[3] - constants.focus_rect[1]) * target.height);
            const double focus_sampling_ratio_x = focus_swapchain.width / focus_rect_output_width;
            const double focus_sampling_ratio_y = focus_swapchain.height / focus_rect_output_height;
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
                   << ", focusTex=" << focus_swapchain.width << "x" << focus_swapchain.height
                   << ", focusRectOutputPx=" << FormatDiagnosticDouble(focus_rect_output_width) << "x"
                   << FormatDiagnosticDouble(focus_rect_output_height)
                   << ", focusSamplingRatio=(" << FormatDiagnosticDouble(focus_sampling_ratio_x) << ", "
                   << FormatDiagnosticDouble(focus_sampling_ratio_y) << ")"
                   << ", feather=(" << FormatDiagnosticDouble(constants.blend_params[0]) << ", "
                   << FormatDiagnosticDouble(constants.blend_params[1]) << ")"
                   << ", sharpness=" << FormatDiagnosticDouble(constants.blend_params[2])
                   << ", sharpenBackend=inlineAdaptive"
                   << ", outputTexel=(" << FormatDiagnosticDouble(constants.focus_texel[2]) << ", "
                   << FormatDiagnosticDouble(constants.focus_texel[3]) << ")"
                   << ", focusTexel=(" << FormatDiagnosticDouble(constants.focus_texel[0]) << ", "
                   << FormatDiagnosticDouble(constants.focus_texel[1]) << ")"
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
                     ", canvasDensity=" + FormatDiagnosticDouble(canvas_density) +
                     ", focusTex=" + std::to_string(swapchains[2]->width) + "x" +
                     std::to_string(swapchains[2]->height) +
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
    if (should_log_compositor_diagnostic) {
        const auto compose_end = std::chrono::steady_clock::now();
        const double cpu_ms = std::chrono::duration<double, std::milli>(compose_end - compose_start).count();
        const bool has_completed_gpu_ms =
            completed_gpu_ms >= 0.0 || d3d11_quadviews_compositor_.has_last_completed_gpu_timing;
        const double logged_gpu_ms = completed_gpu_ms >= 0.0 ? completed_gpu_ms :
            d3d11_quadviews_compositor_.last_completed_gpu_ms;
        const XrTime logged_gpu_frame_time = completed_gpu_ms >= 0.0 ? completed_gpu_frame_time :
            d3d11_quadviews_compositor_.last_completed_gpu_frame_time;
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
               << ", completedGpuFrameTime=" << logged_gpu_frame_time
               << ", completedGpuMs="
               << (has_completed_gpu_ms ? FormatDiagnosticDouble(logged_gpu_ms) : "pending")
               << ", appPixelBudget=" << FormatDiagnosticDouble(
                      (static_cast<double>(swapchains[0]->width) * swapchains[0]->height +
                       static_cast<double>(swapchains[2]->width) * swapchains[2]->height) /
                      std::max(1.0, static_cast<double>(output_width) * output_height) * 100.0)
               << "%";
        logger_.Debug(stream.str());
        if (pending_quadviews_compositor_diagnostics_ > 0) {
            --pending_quadviews_compositor_diagnostics_;
        }
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

// Turbo mode frame loop. The runtime always sees a conformant
// wait->begin->end sequence; only the application's view of frame timing is
// decoupled: while a pipelined async wait is outstanding, the app's
// xrWaitFrame returns immediately with a fabricated (monotonic)
// predictedDisplayTime and its xrBeginFrame becomes a no-op — the real
// xrBeginFrame is issued inside ForwardEndFrame once the async xrWaitFrame
// resolves. Exactly one frame of pipelining is allowed. These two hooks touch
// only turbo_mutex_ so they never contend with the config/render lock.
XrResult OpenXrLayer::WaitFrame(XrSession session,
                                const XrFrameWaitInfo* frame_wait_info,
                                XrFrameState* frame_state) {
    if (!frame_state) {
        return next_wait_frame_(session, frame_wait_info, frame_state);
    }

    {
        std::unique_lock lock(turbo_mutex_);
        const bool wait_pipelined = turbo_async_wait_.valid();
        // Fabricate while a pipelined wait is outstanding (async pacing) or
        // the sequenced pipeline is established — in both cases the runtime
        // already owns this frame's pacing and this wait must never reach it.
        // The sequenced shield is a persistent state, not a per-frame flag:
        // DCS calls xrWaitFrame concurrently with xrEndFrame, and any gap
        // here would let that wait reach the runtime alongside our own
        // (hardlock).
        if (wait_pipelined || turbo_seq_state_ == TurboSequencedState::kActive) {
            if (!wait_pipelined && !turbo_valve_open_) {
                // Valve closed (turbo toggled off / suspended): re-couple the
                // app to genuine runtime pacing without touching the pipeline
                // topology — block until the next pre-wait posts a pacing
                // token. The bounded timeout falls back to fabrication so an
                // app that stops submitting frames can never deadlock here.
                constexpr std::chrono::milliseconds kValveTimeout{100};
                const auto valve_wait_start = std::chrono::steady_clock::now();
                const auto deadline = valve_wait_start + kValveTimeout;
                while (turbo_pacing_tokens_ == 0 && !turbo_valve_open_ &&
                       turbo_seq_state_ == TurboSequencedState::kActive) {
                    if (turbo_valve_cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                        break;
                    }
                }
                const bool consumed_token = turbo_pacing_tokens_ > 0;
                if (consumed_token) {
                    --turbo_pacing_tokens_;
                }
                // The valve block is app-visible runtime pacing (turbo off).
                const double valve_wait_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              valve_wait_start)
                        .count();
                turbo_metrics_wait_pending_ms_ += valve_wait_ms;
                if (turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_seq_debug_log_budget_;
                    logger_.Debug("Turbo-diag: valve re-coupling wait ended after " +
                                  std::to_string(valve_wait_ms) +
                                  "ms, token=" + std::to_string(consumed_token ? 1 : 0));
                }
            }
            if (wait_pipelined) {
                if (turbo_async_wait_polled_) {
                    // Second poll while pipelined: only one frame of
                    // pipelining is allowed, so now we must wait for the real
                    // frame.
                    lock.unlock();
                    turbo_async_wait_.wait();
                    lock.lock();
                }
                turbo_async_wait_polled_ = true;
            }

            const auto now = std::chrono::steady_clock::now();
            XrTime predicted = turbo_last_predicted_display_time_;
            if (wait_pipelined && !turbo_async_wait_completed_ &&
                turbo_last_wait_frame_wall_time_.has_value()) {
                // Async wait still in flight: extrapolate by the wall-clock
                // delta between the app's waits. (Sequenced pacing never needs
                // this — its wait completed before EndFrame returned, so the
                // recorded timing is already this frame's real timing.)
                predicted += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 now - *turbo_last_wait_frame_wall_time_)
                                 .count();
            }
            turbo_last_wait_frame_wall_time_ = now;

            // The spec requires predictedDisplayTime to increase monotonically.
            // When the app polls twice for one pipelined frame (DCS menu does),
            // step the second return forward by a display period rather than
            // 1ns — two frames "predicted" for the same instant confuse the
            // app's pose prediction and animation timing.
            const XrTime min_step =
                turbo_last_predicted_display_period_ > 0 ? turbo_last_predicted_display_period_ : 1;
            frame_state->predictedDisplayTime =
                std::max(predicted, turbo_max_returned_display_time_ + min_step);
            frame_state->predictedDisplayPeriod = turbo_last_predicted_display_period_;
            frame_state->shouldRender = turbo_last_should_render_ ? XR_TRUE : XR_FALSE;
            turbo_max_returned_display_time_ = frame_state->predictedDisplayTime;
            ++pacing_fabricated_waits_;
            ++turbo_metrics_fabricated_pending_;
            if (turbo_fabricated_wait_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                --turbo_fabricated_wait_log_budget_;
                logger_.Debug("Turbo: fabricated xrWaitFrame return, predictedDisplayTime=" +
                              std::to_string(frame_state->predictedDisplayTime) +
                              ", asyncWaitCompleted=" + (turbo_async_wait_completed_ ? "1" : "0"));
            }
            return XR_SUCCESS;
        }
    }

    // The establishment handshake runs the REAL wait here, on the app's own
    // wait callsite and thread — this wait can never duplicate an app wait
    // because it IS the app's wait. The app's next xrBeginFrame then passes
    // through real (turbo_begin_owed_).
    bool handshake = false;
    {
        std::scoped_lock lock(turbo_mutex_);
        handshake = turbo_seq_state_ == TurboSequencedState::kEngaging;
    }

    const bool diag_wait = TurboSequencedDebugTick();
    if (diag_wait) {
        logger_.Debug(std::string("Turbo-diag: app xrWaitFrame passing through to the runtime") +
                      (handshake ? " (establishment handshake)." : "."));
    }
    const auto wait_start = std::chrono::steady_clock::now();
    XrResult result = XR_SUCCESS;
    {
        std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
        result = next_wait_frame_(session, frame_wait_info, frame_state);
    }
    const double wait_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
    if (diag_wait) {
        logger_.Debug("Turbo-diag: app xrWaitFrame returned in " + std::to_string(wait_ms) +
                      "ms, result=" + std::to_string(static_cast<int>(result)));
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(turbo_mutex_);
        pacing_wait_sum_ms_ += wait_ms;
        pacing_wait_max_ms_ = std::max(pacing_wait_max_ms_, wait_ms);
        ++pacing_wait_samples_;
        turbo_metrics_wait_pending_ms_ += wait_ms;
        // Always record the real pacing so a mid-session switch into turbo has
        // sane values to extrapolate from.
        turbo_last_predicted_display_time_ = frame_state->predictedDisplayTime;
        turbo_last_predicted_display_period_ = frame_state->predictedDisplayPeriod;
        turbo_last_should_render_ = frame_state->shouldRender == XR_TRUE;
        NoteTurboShouldRenderLocked(turbo_last_should_render_);
        turbo_last_wait_frame_wall_time_ = std::chrono::steady_clock::now();
        frame_state->predictedDisplayTime =
            std::max(frame_state->predictedDisplayTime, turbo_max_returned_display_time_ + 1);
        turbo_max_returned_display_time_ = frame_state->predictedDisplayTime;

        if (handshake && turbo_seq_state_ == TurboSequencedState::kEngaging) {
            turbo_seq_state_ = TurboSequencedState::kActive;
            turbo_begin_owed_ = true;
            turbo_frame_begun_ = false;
            if (logger_.IsDebugEnabled()) {
                logger_.Debug("Turbo: sequenced handshake complete (real wait took " +
                              std::to_string(wait_ms) +
                              "ms); app wait/begin fabricated from the next frame on.");
            }
        }
    } else {
        std::scoped_lock lock(turbo_mutex_);
        if (handshake && turbo_seq_state_ == TurboSequencedState::kEngaging) {
            // Session state advanced under us; give up the transition.
            turbo_seq_state_ = TurboSequencedState::kInactive;
        }
    }
    return result;
}

XrResult OpenXrLayer::BeginFrame(XrSession session, const XrFrameBeginInfo* frame_begin_info) {
    {
        std::scoped_lock lock(turbo_mutex_);
        if (turbo_begin_owed_) {
            if (turbo_end_frame_in_flight_) {
                // The previous frame's submit is still inside the runtime on
                // another thread. Forwarding this begin now would deliver
                // Begin(N+1) before End(N) — MSFS2024 orders its frame calls
                // this way and PiOpenXR wedged on it. Swallow it; the frame
                // thread issues it right after the submit returns.
                turbo_begin_deferred_ = true;
                if (turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_seq_debug_log_budget_;
                    logger_.Debug("Turbo-diag: owed xrBeginFrame deferred past in-flight EndFrame.");
                }
                return XR_SUCCESS;
            }
            // The matching wait ran real during the establishment handshake; this
            // begin must reach the runtime.
            turbo_begin_owed_ = false;
            if (turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                --turbo_seq_debug_log_budget_;
                logger_.Debug("Turbo-diag: owed xrBeginFrame passing through to the runtime.");
            }
        } else if (turbo_async_wait_.valid() || turbo_seq_state_ == TurboSequencedState::kActive) {
            // Async pacing: deferred into ForwardEndFrame once the async wait
            // resolves. Sequenced pacing: the frame was pre-begun inside the
            // previous EndFrame (or will be compensated there).
            return XR_SUCCESS;
        } else if (turbo_seq_state_ == TurboSequencedState::kEngaging &&
                   turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
            --turbo_seq_debug_log_budget_;
            logger_.Debug("Turbo-diag: app xrBeginFrame passing through while engaging.");
        }
    }
    const XrResult result = next_begin_frame_(session, frame_begin_info);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(turbo_mutex_);
        turbo_frame_begun_ = true;
    }
    return result;
}

XrResult OpenXrLayer::ForwardEndFrame(XrSession session,
                                      const XrFrameEndInfo* frame_end_info,
                                      std::unique_lock<std::mutex>& config_lock) {
    // Black-screen forensics: an app that keeps its frame loop running but
    // stops submitting layers shows a void with no error anywhere (DCS did
    // exactly this on SteamVR, silently, after one rendered frame). The
    // first observation is skipped — apps commonly start empty while loading.
    const bool submitting_layers = frame_end_info && frame_end_info->layerCount > 0;
    if (!app_submitting_layers_.has_value()) {
        app_submitting_layers_ = submitting_layers;
    } else if (*app_submitting_layers_ != submitting_layers) {
        app_submitting_layers_ = submitting_layers;
        if (submission_transition_log_budget_ > 0) {
            --submission_transition_log_budget_;
            logger_.Info(std::string(submitting_layers
                                         ? "App began submitting composition layers (layerCount=" +
                                               std::to_string(frame_end_info->layerCount) + ")"
                                         : "App stopped submitting composition layers (layerCount=0; "
                                           "the headset shows a void while this holds)") +
                         (submission_transition_log_budget_ == 0
                              ? "; further transitions suppressed this session."
                              : "."));
        }
    }

    bool turbo_engaged = IsTurboActive();

    // Cadence gate: app-only time since the previous EndFrame (our own
    // drain/join blocking subtracted). A stalled runtime wait is only
    // evidence against a pacing mode when the app is pacing normally, so
    // turbo stays passive until the cadence steadies (loading screens) and
    // pauses across mid-session load hitches. First field test: DCS's loading
    // screen ran at 2fps, every wait "stalled", and discovery recorded a
    // poisoned unsupported verdict within 11 seconds of launch.
    constexpr double kHealthyFrameMs = 50.0;
    constexpr double kCadencePauseMs = 150.0;
    constexpr uint32_t kEngageStreakFrames = 90;
    double app_frame_delta_ms = -1.0;
    {
        const auto entry_now = std::chrono::steady_clock::now();
        if (pacing_last_end_time_.has_value()) {
            app_frame_delta_ms =
                std::chrono::duration<double, std::milli>(entry_now - *pacing_last_end_time_).count() -
                turbo_last_frame_blocked_ms_;
        }
    }
    if (app_frame_delta_ms >= 0.0 && app_frame_delta_ms < kHealthyFrameMs) {
        ++turbo_cadence_healthy_streak_;
    } else {
        turbo_cadence_healthy_streak_ = 0;
    }
    if (!turbo_cadence_ready_) {
        // Healthy streak alone is not enough: MSFS2024 renders at full rate
        // through its VR-mode transition, so 90 healthy frames arrive within
        // a second of xrBeginSession while the app's frame threading is still
        // settling — and engaging in that window froze the game on PiOpenXR.
        // Require a few seconds of session age before the first engage.
        constexpr std::chrono::seconds kEngageMinSessionAge{5};
        const bool session_mature =
            session_begin_wall_time_.has_value() &&
            std::chrono::steady_clock::now() - *session_begin_wall_time_ >= kEngageMinSessionAge;
        turbo_cadence_ready_ = session_mature && turbo_cadence_healthy_streak_ >= kEngageStreakFrames;
    } else if (app_frame_delta_ms >= kCadencePauseMs) {
        turbo_cadence_ready_ = false;
    }
    // The gate delays the FIRST engage and (below) gates circuit counting and
    // stability accrual — but it must NOT tear down an established pipeline:
    // every unwind/re-engage transition is a race against the app's loose
    // frame threading (a mission-load pause/resume cycle hardlocked DCS on
    // PiOpenXR), and riding through a hitch is harmless — the pipelined wait
    // simply runs at the app's own loading pace.
    bool pipeline_established = false;
    {
        std::scoped_lock lock(turbo_mutex_);
        pipeline_established = turbo_async_wait_.valid() ||
                               turbo_seq_state_ == TurboSequencedState::kActive ||
                               turbo_seq_state_ == TurboSequencedState::kEngaging;
    }
    const bool cadence_countable = turbo_cadence_ready_;
    if (turbo_engaged && !turbo_cadence_ready_ && !pipeline_established) {
        // Delay the first engage until the app renders steadily.
        turbo_engaged = false;
        if (!turbo_cadence_pause_logged_) {
            logger_.Info("Turbo: waiting for a stable frame cadence before pipelining (app loading or "
                         "hitching); engages automatically.");
            turbo_cadence_pause_logged_ = true;
        }
    } else if (turbo_engaged && turbo_cadence_pause_logged_) {
        logger_.Info("Turbo: frame cadence stabilized; pipelining engages.");
        turbo_cadence_pause_logged_ = false;
    }

    if (turbo_engaged && !turbo_pacing_resolved_) {
        ResolveTurboPacingModeLocked();
        // Resolution can suspend immediately (a recorded "unsupported"
        // verdict) — honor it before any pipelining starts.
        if (turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            turbo_engaged = false;
        }
    }

    // Copied under the lock: the auto-suspend path below runs after the lock
    // is released and may need to play the toggle's deactivate cue.
    SoundFeedback turbo_sound{};
    int turbo_sound_volume = 100;
    if (turbo_engaged) {
        turbo_sound = resolved_settings_.turbo.toggle_binding.sound;
        turbo_sound_volume = resolved_settings_.core.sound_volume;
    }
    // Metrics capture config, also copied under the lock: the recorder runs
    // at the tail of this function, after the lock is released.
    const TurboMetricsMode metrics_mode = resolved_settings_.turbo.metrics_mode;
    const InputBinding metrics_binding = resolved_settings_.turbo.metrics_binding;
    const bool metrics_available = resolved_settings_.core.enabled && resolved_settings_.turbo.enabled;
    const int metrics_sound_volume = resolved_settings_.core.sound_volume;

    // Everything past this point is frame forwarding: turbo drain (which can
    // block a full frame interval), the deferred begin, and the runtime end.
    // Release the config/render lock so locate calls from other app threads
    // keep flowing — holding it here caused visible judder at locked fps.
    config_lock.unlock();

    const auto pacing_start = std::chrono::steady_clock::now();

    bool has_pending_wait = false;
    bool frame_begun = false;
    bool begin_owed = false;
    bool valve_open = false;
    TurboSequencedState seq_state = TurboSequencedState::kInactive;
    {
        std::scoped_lock lock(turbo_mutex_);
        has_pending_wait = turbo_async_wait_.valid();
        // This submit consumes the begun-frame marker (set by our pre-begin,
        // a compensation begin, or the app's own begin passing through).
        frame_begun = turbo_frame_begun_;
        turbo_frame_begun_ = false;
        seq_state = turbo_seq_state_;
        begin_owed = turbo_begin_owed_;
        valve_open = turbo_valve_open_;
        // From here until the runtime xrEndFrame returns, an owed
        // establishment begin arriving on another thread is deferred rather
        // than forwarded (Begin(N+1) must never reach the runtime before
        // End(N) — the MSFS2024 ordering).
        turbo_end_frame_in_flight_ = true;
    }
    if (TurboSequencedDebugTick()) {
        logger_.Debug("Turbo-diag: pre-submit snapshot: engaged=" + std::to_string(turbo_engaged ? 1 : 0) +
                      ", seqState=" + std::to_string(static_cast<int>(seq_state)) +
                      ", pendingAsyncWait=" + std::to_string(has_pending_wait ? 1 : 0) +
                      ", frameBegun=" + std::to_string(frame_begun ? 1 : 0) +
                      ", beginOwed=" + std::to_string(begin_owed ? 1 : 0) +
                      ", valveOpen=" + std::to_string(valve_open ? 1 : 0));
    }
    bool timed_out = false;
    double frame_blocked_ms = 0.0;
    if (has_pending_wait) {
        // This is the latest point the pipelined frame must have been fully
        // waited. Some runtimes interlock the wait with the next submit
        // (PiOpenXR, Oculus, Varjo), stalling it until we proceed anyway —
        // cap the per-frame damage with a short timeout. If it trips, the
        // future stays valid and no second wait is enqueued below; submitting
        // is what unblocks the stalled runtime wait.
        constexpr std::chrono::milliseconds kTurboDrainTimeout{250};
        const bool diag_drain = TurboSequencedDebugTick();
        if (diag_drain) {
            logger_.Debug("Turbo-diag: async drain starting (250ms cap).");
        }
        const auto drain_start = std::chrono::steady_clock::now();
        const bool ready = turbo_async_wait_.wait_for(kTurboDrainTimeout) == std::future_status::ready;
        const double drain_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drain_start)
                .count();
        frame_blocked_ms += drain_ms;
        if (diag_drain) {
            logger_.Debug("Turbo-diag: async drain completed in " + std::to_string(drain_ms) +
                          "ms, ready=" + std::to_string(ready ? 1 : 0));
        }
        {
            std::scoped_lock lock(turbo_mutex_);
            if (ready) {
                turbo_async_wait_ = {};
            }
        }
        // Only count while engaged with a healthy cadence: a drain-out during
        // a suspend, or a stall while the app itself is hitching/loading, is
        // not evidence against the pacing mode.
        if (!ready && turbo_engaged && cadence_countable &&
            !turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            timed_out = true;
            if (HandleTurboDrainTimeout(std::chrono::steady_clock::now())) {
                // Same audible cue as a manual turbo-off, so the user knows the
                // safety net fired. Non-blocking (worker-thread playback).
                SoundPlayer::Instance().PlayTransition(turbo_sound, false, dll_directory_, turbo_sound_volume,
                                                       L"turbo-on.wav", L"turbo-off.wav");
            }
        }

        if (!frame_begun) {
            // Deferred xrBeginFrame for the pipelined frame. Errors pass
            // through (e.g. the session state machine advanced under us).
            const XrResult begin_result = next_begin_frame_(session, nullptr);
            if (XR_FAILED(begin_result)) {
                logger_.Error("Turbo: deferred xrBeginFrame failed with " +
                              std::to_string(static_cast<int>(begin_result)));
                return begin_result;
            }
        }
    } else if (seq_state == TurboSequencedState::kActive && !frame_begun && begin_owed) {
        // Establishment round-trip still in flight: the handshake's real wait
        // has run but the app's owed begin has not arrived at the runtime
        // yet. A compensation wait+begin here would duplicate that sequence
        // against the runtime from the submit thread — the PiOpenXR wedge
        // class — so forward the submit as-is and let the owed begin land.
        if (TurboSequencedDebugTick()) {
            logger_.Debug("Turbo-diag: compensation skipped (establishment begin still owed).");
        }
    } else if (seq_state == TurboSequencedState::kActive && !frame_begun) {
        // Compensation: the app's wait for this frame was fabricated during a
        // state transition and no runtime frame is open — supply the
        // wait+begin pair before the submit so the runtime's sequence stays
        // conformant.
        const bool diag_comp = TurboSequencedDebugTick();
        if (diag_comp) {
            logger_.Debug("Turbo-diag: compensation xrWaitFrame starting (frame thread).");
        }
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const auto comp_start = std::chrono::steady_clock::now();
        XrResult comp_result = XR_SUCCESS;
        {
            std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
            comp_result = next_wait_frame_(session, nullptr, &frame_state);
        }
        const double comp_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - comp_start)
                .count();
        frame_blocked_ms += comp_ms;
        if (diag_comp) {
            logger_.Debug("Turbo-diag: compensation xrWaitFrame completed in " + std::to_string(comp_ms) +
                          "ms, result=" + std::to_string(static_cast<int>(comp_result)));
        }
        if (XR_SUCCEEDED(comp_result)) {
            {
                std::scoped_lock lock(turbo_mutex_);
                turbo_last_predicted_display_time_ = frame_state.predictedDisplayTime;
                turbo_last_predicted_display_period_ = frame_state.predictedDisplayPeriod;
                turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
                NoteTurboShouldRenderLocked(turbo_last_should_render_);
            }
            const XrResult begin_result = next_begin_frame_(session, nullptr);
            if (XR_FAILED(begin_result)) {
                logger_.Error("Turbo: compensation xrBeginFrame failed with " +
                              std::to_string(static_cast<int>(begin_result)));
                return begin_result;
            }
        } else {
            logger_.Error("Turbo: compensation xrWaitFrame failed with " +
                          std::to_string(static_cast<int>(comp_result)) + ".");
        }
    }

    const auto pacing_after_drain = std::chrono::steady_clock::now();
    const bool diag_end = TurboSequencedDebugTick();
    if (diag_end) {
        logger_.Debug("Turbo-diag: runtime xrEndFrame starting.");
    }
    const XrResult result = next_end_frame_(session, frame_end_info);
    const auto pacing_after_end = std::chrono::steady_clock::now();
    if (XR_FAILED(result) && end_frame_error_log_budget_ > 0) {
        // A failing runtime EndFrame shows as a silent black screen if the
        // app ignores the result (DCS does); make it visible at info level.
        --end_frame_error_log_budget_;
        logger_.Error("Runtime xrEndFrame failed with " + std::to_string(static_cast<int>(result)) +
                      " (layerCount=" +
                      std::to_string(frame_end_info ? frame_end_info->layerCount : 0) + ")" +
                      (end_frame_error_log_budget_ == 0 ? "; further failures suppressed this session."
                                                        : "."));
    }
    if (diag_end) {
        logger_.Debug("Turbo-diag: runtime xrEndFrame completed in " +
                      std::to_string(std::chrono::duration<double, std::milli>(pacing_after_end -
                                                                               pacing_after_drain)
                                         .count()) +
                      "ms.");
    }
    RecordFramePacing(pacing_start, pacing_after_drain, pacing_after_end, turbo_engaged);

    if (XR_SUCCEEDED(result)) {
        if (turbo_pacing_mode_ == TurboPacingMode::kSequenced) {
            bool do_steady_wait = false;
            {
                std::scoped_lock lock(turbo_mutex_);
                // The valve is what the turbo toggle operates: the pipeline
                // itself is structural and never reacts to the toggle.
                const bool valve_was_open = turbo_valve_open_;
                turbo_valve_open_ = turbo_engaged;
                if (turbo_valve_open_ != valve_was_open) {
                    turbo_valve_cv_.notify_all();
                }
                if (!turbo_async_wait_.valid()) {
                    switch (turbo_seq_state_) {
                    case TurboSequencedState::kInactive:
                        if (!turbo_engaged) {
                            break;
                        }
                        // Establish via handshake: the app's own next
                        // WaitFrame runs real and flips to kActive. No
                        // runtime call here — the app's wait may already be
                        // in flight concurrently with this EndFrame, and
                        // issuing our own would duplicate it (observed
                        // hardlock on DCS). This happens ONCE per session.
                        turbo_seq_state_ = TurboSequencedState::kEngaging;
                        if (!turbo_pipelining_logged_) {
                            logger_.Info("Turbo: frame pipelining engaged (sequenced pacing); the app's "
                                         "xrWaitFrame is now decoupled from runtime pacing.");
                            turbo_pipelining_logged_ = true;
                            turbo_fabricated_wait_log_budget_ = 5;
                            // Generous: the forensic markers added after the
                            // MSFS2024 freeze consume several per frame during
                            // establishment, and the engage window is exactly
                            // what they exist to capture.
                            turbo_seq_debug_log_budget_ = 160;
                        }
                        break;
                    case TurboSequencedState::kActive:
                        // Steady state runs every frame regardless of the
                        // valve — unless the handshake just completed
                        // concurrently and the app still owes its real begin,
                        // in which case this cycle stays hands-off.
                        do_steady_wait = !turbo_begin_owed_;
                        break;
                    case TurboSequencedState::kEngaging:
                        // Handshake in flight; the app's WaitFrame completes it.
                        break;
                    }
                }
            }

            if (do_steady_wait) {
                // The real wait+begin run synchronously RIGHT HERE, on the
                // app's frame thread. From the runtime's view this is the
                // standard single-threaded frame loop (End -> Wait -> Begin),
                // which every conformant runtime must release — no timeout
                // needed. Thread identity matters: PiOpenXR pins frame pacing
                // to the calling thread, and an off-thread wait stalls until
                // the next submit.
                if (turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    logger_.Debug("Turbo: sequenced wait+begin starting (frame thread).");
                }
                XrFrameState frame_state{XR_TYPE_FRAME_STATE};
                const auto wait_start = std::chrono::steady_clock::now();
                XrResult wait_result = XR_SUCCESS;
                {
                    std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
                    wait_result = next_wait_frame_(session, nullptr, &frame_state);
                }
                const double wait_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              wait_start)
                        .count();
                frame_blocked_ms += wait_ms;
                if (turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_seq_debug_log_budget_;
                    logger_.Debug("Turbo: sequenced wait+begin completed in " +
                                  std::to_string(wait_ms) + "ms.");
                }
                if (XR_SUCCEEDED(wait_result)) {
                    {
                        std::scoped_lock lock(turbo_mutex_);
                        turbo_last_predicted_display_time_ = frame_state.predictedDisplayTime;
                        turbo_last_predicted_display_period_ = frame_state.predictedDisplayPeriod;
                        turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
                        NoteTurboShouldRenderLocked(turbo_last_should_render_);
                        turbo_async_wait_completed_ = true;
                        pacing_wait_sum_ms_ += wait_ms;
                        pacing_wait_max_ms_ = std::max(pacing_wait_max_ms_, wait_ms);
                        ++pacing_wait_samples_;
                        // Post one pacing token (capped): a valve-closed app
                        // wait consumes it, re-coupling the app to this real
                        // pacing event.
                        turbo_pacing_tokens_ = 1;
                        turbo_valve_cv_.notify_all();
                    }
                    const XrResult begin_result = next_begin_frame_(session, nullptr);
                    if (XR_SUCCEEDED(begin_result)) {
                        std::scoped_lock lock(turbo_mutex_);
                        turbo_frame_begun_ = true;
                    } else {
                        // Session state advanced under us; the next EndFrame's
                        // compensation path re-establishes the sequence.
                        logger_.Error("Turbo: sequenced xrBeginFrame failed with " +
                                      std::to_string(static_cast<int>(begin_result)));
                    }
                    // The wait cannot be capped, but it can be judged after
                    // the fact: a post-submit wait that blocks far beyond
                    // pacing is the level-2 signal.
                    constexpr double kSequencedWaitStallMs = 250.0;
                    if (wait_ms >= kSequencedWaitStallMs && turbo_engaged && cadence_countable &&
                        !turbo_auto_suspended_.load(std::memory_order_relaxed)) {
                        timed_out = true;
                        if (HandleTurboDrainTimeout(std::chrono::steady_clock::now())) {
                            SoundPlayer::Instance().PlayTransition(turbo_sound, false, dll_directory_,
                                                                   turbo_sound_volume, L"turbo-on.wav",
                                                                   L"turbo-off.wav");
                        }
                    }
                } else {
                    logger_.Error("Turbo: sequenced xrWaitFrame failed with " +
                                  std::to_string(static_cast<int>(wait_result)) +
                                  "; keeping previous frame timing.");
                }
            }
        } else if (turbo_engaged) {
            // Async pacing: background-thread wait, drained at the next
            // EndFrame.
            std::scoped_lock lock(turbo_mutex_);
            if (!turbo_async_wait_.valid() && turbo_seq_state_ == TurboSequencedState::kInactive) {
                if (!turbo_pipelining_logged_) {
                    logger_.Info("Turbo: frame pipelining engaged (async pacing); the app's xrWaitFrame "
                                 "is now decoupled from runtime pacing.");
                    turbo_pipelining_logged_ = true;
                    turbo_fabricated_wait_log_budget_ = 5;
                }
                turbo_async_wait_polled_ = false;
                turbo_async_wait_completed_ = false;
                turbo_async_wait_ = std::async(std::launch::async, [this, session] {
                    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
                    XrResult wait_result = XR_SUCCESS;
                    {
                        std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
                        wait_result = next_wait_frame_(session, nullptr, &frame_state);
                    }
                    std::scoped_lock state_lock(turbo_mutex_);
                    if (XR_SUCCEEDED(wait_result)) {
                        turbo_last_predicted_display_time_ = frame_state.predictedDisplayTime;
                        turbo_last_predicted_display_period_ = frame_state.predictedDisplayPeriod;
                        turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
                        NoteTurboShouldRenderLocked(turbo_last_should_render_);
                    } else {
                        logger_.Error("Turbo: async xrWaitFrame failed with " +
                                      std::to_string(static_cast<int>(wait_result)) +
                                      "; keeping previous frame timing.");
                    }
                    turbo_async_wait_completed_ = true;
                });
            }
        }

        if (turbo_engaged && !timed_out && cadence_countable &&
            !turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            NoteTurboPacingStableFrame(app_frame_delta_ms);
        }

        if (!turbo_engaged) {
            // Turbo off/suspended. The structural sequenced pipeline stays up
            // (the valve above already closed — tearing the pipeline down and
            // re-establishing it wedges PiOpenXR's per-thread pacing); only a
            // not-yet-established handshake is dropped, and the async
            // drain-out is logged.
            std::scoped_lock lock(turbo_mutex_);
            if (turbo_seq_state_ == TurboSequencedState::kEngaging) {
                // Nothing was pipelined yet; drop the handshake request.
                turbo_seq_state_ = TurboSequencedState::kInactive;
                if (turbo_pipelining_logged_) {
                    logger_.Info("Turbo: frame pipelining released; runtime pacing restored.");
                    turbo_pipelining_logged_ = false;
                }
            } else if (has_pending_wait && !turbo_async_wait_.valid() && turbo_pipelining_logged_ &&
                       turbo_seq_state_ == TurboSequencedState::kInactive) {
                // Async pipeline just drained without relaunching.
                logger_.Info("Turbo: frame pipelining released; runtime pacing restored.");
                turbo_pipelining_logged_ = false;
            }
        }
    }

    // The submit is down; release the owed-begin deferral window. If the
    // app's establishment begin arrived mid-submit and was swallowed, issue
    // it now on this thread — the runtime sees End(N) then Begin(N+1), the
    // order it requires.
    bool issue_deferred_begin = false;
    {
        std::scoped_lock lock(turbo_mutex_);
        turbo_end_frame_in_flight_ = false;
        if (turbo_begin_deferred_) {
            turbo_begin_deferred_ = false;
            issue_deferred_begin = turbo_begin_owed_;
            turbo_begin_owed_ = false;
        }
    }
    if (issue_deferred_begin) {
        if (TurboSequencedDebugTick()) {
            logger_.Debug("Turbo-diag: issuing deferred establishment xrBeginFrame (frame thread).");
        }
        const XrResult begin_result = next_begin_frame_(session, nullptr);
        if (XR_SUCCEEDED(begin_result)) {
            std::scoped_lock lock(turbo_mutex_);
            turbo_frame_begun_ = true;
        } else {
            // The app already saw XR_SUCCESS for the swallowed begin; log and
            // let the next EndFrame's compensation path re-establish.
            logger_.Error("Turbo: deferred establishment xrBeginFrame failed with " +
                          std::to_string(static_cast<int>(begin_result)));
        }
    }

    // Feeds next frame's cadence gate: subtracting our own blocking from the
    // frame delta keeps a turbo-induced stall from masquerading as an app
    // hitch (which would pause the gate and stop the very counting that
    // should catch it).
    turbo_last_frame_blocked_ms_ = frame_blocked_ms;

    RecordTurboMetricsFrame(turbo_engaged, frame_blocked_ms, timed_out, metrics_mode, metrics_binding,
                            metrics_available, metrics_sound_volume);

    return result;
}

XrTime OpenXrLayer::ClampInternalLocateTime(XrTime app_time) {
    std::scoped_lock lock(turbo_mutex_);
    if (turbo_seq_state_ == TurboSequencedState::kActive && turbo_last_predicted_display_time_ > 0 &&
        app_time > turbo_last_predicted_display_time_) {
        return turbo_last_predicted_display_time_;
    }
    return app_time;
}

bool OpenXrLayer::TurboSequencedDebugTick() {
    if (!logger_.IsDebugEnabled()) {
        return false;
    }
    std::scoped_lock lock(turbo_mutex_);
    if (turbo_seq_debug_log_budget_ <= 0 ||
        (turbo_seq_state_ != TurboSequencedState::kActive &&
         turbo_seq_state_ != TurboSequencedState::kEngaging)) {
        return false;
    }
    --turbo_seq_debug_log_budget_;
    return true;
}

// Chooses the pacing strategy when turbo first engages (config lock held,
// frame thread). Precedence: forced setting > per-runtime pin > recorded
// sidecar verdict > known-runtime seed table > async-first probe.
void OpenXrLayer::ResolveTurboPacingModeLocked() {
    turbo_pacing_resolved_ = true;
    // A structural sequenced pipeline cannot change strategy mid-session:
    // tearing it down wedges thread-affine runtimes (PiOpenXR). Re-resolution
    // (settings edited mid-flight) keeps sequenced until the next session.
    {
        std::scoped_lock lock(turbo_mutex_);
        if (turbo_seq_state_ != TurboSequencedState::kInactive) {
            if (turbo_pacing_mode_ != TurboPacingMode::kSequenced) {
                turbo_pacing_mode_ = TurboPacingMode::kSequenced;
            }
            logger_.Info("Turbo pacing: sequenced pipeline already established; pacing-mode changes "
                         "apply at the next session.");
            return;
        }
    }
    turbo_pacing_verdict_pending_ = false;
    turbo_probe_timeout_total_ = 0;
    turbo_stable_accumulated_ms_ = 0.0;
    turbo_drain_timeout_count_ = 0;
    turbo_timeout_window_start_.reset();

    const TurboPacingSetting setting = resolved_settings_.turbo.pacing_mode;
    if (setting != TurboPacingSetting::kAuto) {
        turbo_pacing_mode_ = setting == TurboPacingSetting::kSequenced ? TurboPacingMode::kSequenced
                                                                       : TurboPacingMode::kAsync;
        turbo_pacing_source_ = TurboPacingSource::kForced;
        logger_.Info(std::string("Turbo pacing: ") + ToString(turbo_pacing_mode_) +
                     " (forced in settings; per-runtime discovery and pins are disabled).");
        return;
    }

    for (const auto& [pinned_runtime, pinned_mode] : resolved_settings_.turbo.runtime_pins) {
        if (pinned_runtime == runtime_name_) {
            turbo_pacing_mode_ = pinned_mode;
            turbo_pacing_source_ = TurboPacingSource::kPinned;
            logger_.Info(std::string("Turbo pacing: ") + ToString(turbo_pacing_mode_) +
                         " (pinned for runtime \"" + runtime_name_ + "\").");
            return;
        }
    }

    if (const auto observation =
            FindRuntimePacingObservation(ResolveRuntimePacingPath(), runtime_name_)) {
        if (observation->mode == TurboPacingMode::kUnsupported) {
            // Verdict from a previous session: even sequenced pacing stalled.
            // Suspend before the first pipelined frame instead of replaying
            // the failure; a toggle press retries (sequenced) and a stable
            // window overwrites the verdict.
            turbo_pacing_mode_ = TurboPacingMode::kSequenced;
            turbo_pacing_source_ = TurboPacingSource::kDiscovered;
            turbo_pacing_verdict_pending_ = true;
            turbo_auto_suspended_.store(true, std::memory_order_relaxed);
            logger_.Info("Turbo pacing: runtime \"" + runtime_name_ +
                         "\" is recorded as not tolerating turbo pipelining; turbo stays suspended. "
                         "Press the turbo toggle binding to retry.");
            return;
        }
        turbo_pacing_mode_ = observation->mode;
        turbo_pacing_source_ = TurboPacingSource::kDiscovered;
        logger_.Info(std::string("Turbo pacing: ") + ToString(turbo_pacing_mode_) +
                     " (recorded verdict for runtime \"" + runtime_name_ + "\", source=" +
                     observation->source + ").");
        RuntimePacingObservation updated = *observation;
        updated.runtime_version = runtime_version_;
        updated.last_used_unix_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        RecordRuntimePacingObservation(ResolveRuntimePacingPath(), updated, nullptr);
        return;
    }

    if (const auto seeded = SeededTurboPacingMode(runtime_name_)) {
        turbo_pacing_mode_ = *seeded;
        turbo_pacing_source_ = TurboPacingSource::kPreset;
        turbo_pacing_verdict_pending_ = true;
        logger_.Info(std::string("Turbo pacing: ") + ToString(turbo_pacing_mode_) +
                     " (preset for runtime \"" + runtime_name_ +
                     "\"); will record the verdict once frame pacing proves stable.");
        return;
    }

    turbo_pacing_mode_ = TurboPacingMode::kAsync;
    turbo_pacing_source_ = TurboPacingSource::kProbing;
    turbo_pacing_verdict_pending_ = true;
    logger_.Info("Turbo pacing: unknown runtime \"" + runtime_name_ +
                 "\"; probing async pacing first (falls back to sequenced on stalls).");
}

void OpenXrLayer::RecordTurboPacingVerdict(TurboPacingMode mode,
                                           const char* source,
                                           std::int64_t stable_seconds) {
    if (runtime_name_.empty()) {
        return;
    }
    RuntimePacingObservation observation;
    observation.runtime_name = runtime_name_;
    observation.runtime_version = runtime_version_;
    observation.mode = mode;
    observation.source = source;
#if defined(VECTORXR_VERSION)
    observation.layer_version = VECTORXR_VERSION;
#endif
    observation.last_used_unix_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    observation.probe_timeouts = turbo_probe_timeout_total_;
    observation.stable_seconds = stable_seconds;

    std::string error;
    if (RecordRuntimePacingObservation(ResolveRuntimePacingPath(), observation, &error)) {
        logger_.Info(std::string("Turbo pacing: recorded ") + ToString(mode) +
                     " verdict for runtime \"" + runtime_name_ + "\" (" + source + ").");
    } else {
        logger_.Info("Turbo pacing: failed to record verdict: " + error);
    }
}

void OpenXrLayer::NoteTurboPacingStableFrame(double app_frame_delta_ms) {
    if (!turbo_pacing_verdict_pending_ || app_frame_delta_ms < 0.0) {
        return;
    }
    // Accumulated healthy engaged time, not wall-clock: loading screens and
    // cadence pauses neither earn nor destroy stability. 60s of real play is
    // long enough that the DCS menu -> mission transition (where load spikes
    // surface interlocks) is usually covered before the verdict lands.
    constexpr double kStableWindowMs = 60000.0;
    turbo_stable_accumulated_ms_ += app_frame_delta_ms;
    if (turbo_stable_accumulated_ms_ < kStableWindowMs) {
        return;
    }
    turbo_pacing_verdict_pending_ = false;
    const char* source = turbo_pacing_source_ == TurboPacingSource::kPreset ? "preset" : "discovered";
    if (turbo_pacing_source_ == TurboPacingSource::kProbing ||
        turbo_pacing_source_ == TurboPacingSource::kFallback) {
        turbo_pacing_source_ = TurboPacingSource::kDiscovered;
    }
    RecordTurboPacingVerdict(turbo_pacing_mode_, source,
                             static_cast<std::int64_t>(turbo_stable_accumulated_ms_ / 1000.0));
}

bool OpenXrLayer::HandleTurboDrainTimeout(std::chrono::steady_clock::time_point now) {
    turbo_stable_accumulated_ms_ = 0.0;
    ++turbo_probe_timeout_total_;

    // Rolling-window count: stalled frames interleave with clean ones on
    // these runtimes, so a consecutive-streak check never fires.
    constexpr std::chrono::seconds kTimeoutWindow{30};
    if (!turbo_timeout_window_start_.has_value() ||
        now - *turbo_timeout_window_start_ > kTimeoutWindow) {
        turbo_timeout_window_start_ = now;
        turbo_drain_timeout_count_ = 0;
    }
    ++turbo_drain_timeout_count_;

    const bool auto_pacing = turbo_pacing_source_ != TurboPacingSource::kForced &&
                             turbo_pacing_source_ != TurboPacingSource::kPinned;

    if (turbo_pacing_mode_ == TurboPacingMode::kAsync && auto_pacing) {
        // Level 1: adapt instead of suspending. Probing gets a hair trigger so
        // an unknown runtime suffers at most a couple of stalled frames before
        // the fallback; a recorded/preset async verdict gets more benefit of
        // the doubt before we conclude conditions changed.
        const int threshold = turbo_pacing_source_ == TurboPacingSource::kProbing ? 2 : 5;
        if (turbo_drain_timeout_count_ < threshold) {
            return false;
        }
        turbo_pacing_mode_ = TurboPacingMode::kSequenced;
        turbo_pacing_source_ = TurboPacingSource::kFallback;
        turbo_pacing_verdict_pending_ = true;
        turbo_drain_timeout_count_ = 0;
        turbo_timeout_window_start_.reset();
        logger_.Info("Turbo: this runtime interlocks xrWaitFrame with frame submission; switching to "
                     "sequenced pacing (the wait now happens right after each submit). No action needed.");
        return false;
    }

    // Level 2 (sequenced pacing still stalling), or a forced/pinned mode the
    // user asked us not to adapt: suspend turbo for the session.
    const int threshold = turbo_pacing_mode_ == TurboPacingMode::kSequenced ? 3 : 5;
    if (turbo_drain_timeout_count_ < threshold) {
        return false;
    }
    turbo_auto_suspended_.store(true, std::memory_order_relaxed);
    logger_.Info(std::string("Turbo: ") +
                 (turbo_pacing_mode_ == TurboPacingMode::kAsync
                      ? "the pipelined xrWaitFrame repeatedly stalled until the next frame submit"
                      : "the sequenced xrWaitFrame stalled even after the frame submit") +
                 "; auto-suspending turbo for this session. Press the turbo toggle binding to re-arm it.");
    if (auto_pacing && turbo_pacing_verdict_pending_ &&
        turbo_pacing_mode_ == TurboPacingMode::kSequenced) {
        // Discovery concluded: neither strategy is tolerated. Record it so the
        // next session suspends up front instead of replaying the hitches.
        turbo_pacing_verdict_pending_ = false;
        RecordTurboPacingVerdict(TurboPacingMode::kUnsupported, "discovered", 0);
    }
    return true;
}

void OpenXrLayer::RecordFramePacing(std::chrono::steady_clock::time_point frame_start,
                                    std::chrono::steady_clock::time_point after_drain,
                                    std::chrono::steady_clock::time_point after_end,
                                    bool turbo_engaged) {
    constexpr std::chrono::seconds kPacingWindow{5};

    if (pacing_last_end_time_.has_value()) {
        const double delta_ms =
            std::chrono::duration<double, std::milli>(after_end - *pacing_last_end_time_).count();
        pacing_delta_sum_ms_ += delta_ms;
        pacing_delta_max_ms_ = std::max(pacing_delta_max_ms_, delta_ms);
    }
    pacing_last_end_time_ = after_end;
    pacing_drain_sum_ms_ +=
        std::chrono::duration<double, std::milli>(after_drain - frame_start).count();
    pacing_drain_max_ms_ = std::max(
        pacing_drain_max_ms_, std::chrono::duration<double, std::milli>(after_drain - frame_start).count());
    pacing_end_sum_ms_ += std::chrono::duration<double, std::milli>(after_end - after_drain).count();
    pacing_end_max_ms_ =
        std::max(pacing_end_max_ms_, std::chrono::duration<double, std::milli>(after_end - after_drain).count());
    ++pacing_frames_;

    if (!pacing_window_start_.has_value()) {
        pacing_window_start_ = after_end;
        return;
    }
    if (after_end - *pacing_window_start_ < kPacingWindow) {
        return;
    }

    if (logger_.IsDebugEnabled() && pacing_frames_ > 0) {
        double wait_sum_ms = 0.0;
        double wait_max_ms = 0.0;
        uint32_t wait_samples = 0;
        uint32_t fabricated_waits = 0;
        {
            std::scoped_lock lock(turbo_mutex_);
            wait_sum_ms = pacing_wait_sum_ms_;
            wait_max_ms = pacing_wait_max_ms_;
            wait_samples = pacing_wait_samples_;
            fabricated_waits = pacing_fabricated_waits_;
            pacing_wait_sum_ms_ = 0.0;
            pacing_wait_max_ms_ = 0.0;
            pacing_wait_samples_ = 0;
            pacing_fabricated_waits_ = 0;
        }

        const double window_seconds =
            std::chrono::duration<double>(after_end - *pacing_window_start_).count();
        std::ostringstream stream;
        stream << "Frame pacing (" << FormatDiagnosticDouble(window_seconds) << "s window): frames="
               << pacing_frames_ << " (" << FormatDiagnosticDouble(pacing_frames_ / window_seconds)
               << " fps), turbo=" << (turbo_engaged ? 1 : 0);
        if (turbo_engaged) {
            stream << ", pacing=" << ToString(turbo_pacing_mode_);
        }
        stream << ", endFrameDelta avg/max=" << FormatDiagnosticDouble(pacing_delta_sum_ms_ / pacing_frames_)
               << "/" << FormatDiagnosticDouble(pacing_delta_max_ms_)
               << "ms, turboDrainAndBegin avg/max="
               << FormatDiagnosticDouble(pacing_drain_sum_ms_ / pacing_frames_) << "/"
               << FormatDiagnosticDouble(pacing_drain_max_ms_)
               << "ms, runtimeEndFrame avg/max=" << FormatDiagnosticDouble(pacing_end_sum_ms_ / pacing_frames_)
               << "/" << FormatDiagnosticDouble(pacing_end_max_ms_) << "ms, runtimeWaitFrame avg/max=";
        if (wait_samples > 0) {
            stream << FormatDiagnosticDouble(wait_sum_ms / wait_samples) << "/"
                   << FormatDiagnosticDouble(wait_max_ms) << "ms over " << wait_samples << " calls";
        } else {
            stream << "n/a";
        }
        stream << ", fabricatedWaits=" << fabricated_waits;
        logger_.Debug(stream.str());
    } else {
        // Keep the WaitFrame-side counters bounded even when debug is off.
        std::scoped_lock lock(turbo_mutex_);
        pacing_wait_sum_ms_ = 0.0;
        pacing_wait_max_ms_ = 0.0;
        pacing_wait_samples_ = 0;
        pacing_fabricated_waits_ = 0;
    }

    pacing_window_start_ = after_end;
    pacing_frames_ = 0;
    pacing_delta_sum_ms_ = 0.0;
    pacing_delta_max_ms_ = 0.0;
    pacing_drain_sum_ms_ = 0.0;
    pacing_drain_max_ms_ = 0.0;
    pacing_end_sum_ms_ = 0.0;
    pacing_end_max_ms_ = 0.0;
}

void OpenXrLayer::RecordTurboMetricsFrame(bool turbo_engaged,
                                          double frame_blocked_ms,
                                          bool timed_out,
                                          TurboMetricsMode metrics_mode,
                                          const InputBinding& metrics_binding,
                                          bool metrics_available,
                                          int sound_volume) {
    // Drain the WaitFrame-side pending counters unconditionally so a later
    // capture start never inherits blocking observed while paused.
    double wait_pending_ms = 0.0;
    std::int64_t fabricated_pending = 0;
    {
        std::scoped_lock lock(turbo_mutex_);
        wait_pending_ms = turbo_metrics_wait_pending_ms_;
        fabricated_pending = turbo_metrics_fabricated_pending_;
        turbo_metrics_wait_pending_ms_ = 0.0;
        turbo_metrics_fabricated_pending_ = 0;
    }

    const bool capturing =
        metrics_available &&
        (metrics_mode == TurboMetricsMode::kAlways ||
         (metrics_mode == TurboMetricsMode::kBinding &&
          IsTurboMetricsCaptureArmed(metrics_binding, sound_volume)));
    if (!capturing) {
        turbo_metrics_was_capturing_ = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (turbo_metrics_session_id_.empty()) {
        const auto system_now = std::chrono::system_clock::now().time_since_epoch();
        turbo_metrics_started_unix_seconds_ =
            std::chrono::duration_cast<std::chrono::seconds>(system_now).count();
        turbo_metrics_session_id_ =
            current_exe_name_ + "-" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(system_now).count());
        logger_.Info("Turbo metrics: capture session started (" +
                     std::string(ToString(metrics_mode)) + " mode).");
    }
    turbo_metrics_collection_mode_ = metrics_mode;

    const size_t bucket_index =
        !turbo_engaged ? 0 : (turbo_pacing_mode_ == TurboPacingMode::kSequenced ? 2 : 1);
    TurboMetricsAccum& accum = turbo_metrics_accum_[bucket_index];
    accum.wait_block_sum_ms += wait_pending_ms + frame_blocked_ms;
    accum.fabricated_waits += fabricated_pending;
    if (timed_out) {
        ++accum.drain_timeouts;
    }

    if (turbo_metrics_was_capturing_ && turbo_metrics_last_end_time_.has_value()) {
        const double delta_ms =
            std::chrono::duration<double, std::milli>(now - *turbo_metrics_last_end_time_).count();
        // Intervals past 1s are load stalls, not pacing — excluding them keeps
        // an always-mode capture from drowning the averages in loading time.
        constexpr double kDiscardThresholdMs = 1000.0;
        if (delta_ms >= kDiscardThresholdMs) {
            ++accum.discarded_frames;
        } else {
            ++accum.frames;
            accum.delta_sum_ms += delta_ms;
            accum.delta_max_ms = std::max(accum.delta_max_ms, delta_ms);
            const size_t bin = std::min(static_cast<size_t>(delta_ms / kTurboMetricsHistogramBinMs),
                                        kTurboMetricsHistogramBins - 1);
            ++accum.histogram[bin];
        }
    }
    turbo_metrics_last_end_time_ = now;
    turbo_metrics_was_capturing_ = true;
    turbo_metrics_dirty_ = true;

    constexpr std::chrono::seconds kMetricsFlushInterval{15};
    if (!turbo_metrics_last_flush_time_.has_value()) {
        turbo_metrics_last_flush_time_ = now;
    } else if (now - *turbo_metrics_last_flush_time_ >= kMetricsFlushInterval) {
        FlushTurboMetrics(false);
        turbo_metrics_last_flush_time_ = now;
    }
}

bool OpenXrLayer::IsTurboMetricsCaptureArmed(const InputBinding& binding, int sound_volume) {
#if defined(_WIN32)
    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detector on the first poll so a button held from the
    // bind gesture does not register as a press (same as the turbo toggle).
    const bool first_poll = !turbo_metrics_binding_last_poll_time_.has_value();
    if (first_poll || now - *turbo_metrics_binding_last_poll_time_ >= kInputBindingPollInterval) {
        turbo_metrics_binding_last_poll_time_ = now;
        turbo_metrics_binding_down_cached_ = IsInputBindingDown(binding);
    }
    const bool binding_down = turbo_metrics_binding_down_cached_;
    if (first_poll) {
        turbo_metrics_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !turbo_metrics_binding_was_down_;
    turbo_metrics_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        turbo_metrics_capture_armed_ = !turbo_metrics_capture_armed_;
        logger_.Info(std::string("Turbo metrics: capture ") +
                     (turbo_metrics_capture_armed_ ? "started" : "paused") + " via " +
                     BindingLabel(binding) + ".");
        SoundPlayer::Instance().PlayTransition(binding.sound, turbo_metrics_capture_armed_,
                                               dll_directory_, sound_volume, L"metrics-on.wav",
                                               L"metrics-off.wav");
    }
#else
    (void)binding;
    (void)sound_volume;
#endif
    return turbo_metrics_capture_armed_;
}

void OpenXrLayer::FlushTurboMetrics(bool final_flush) {
    if (turbo_metrics_session_id_.empty() || (!turbo_metrics_dirty_ && !final_flush)) {
        return;
    }

    TurboMetricsSession session;
    session.session_id = turbo_metrics_session_id_;
    session.app_name = current_exe_name_;
    session.runtime_name = runtime_name_;
    session.layer_version = VECTORXR_VERSION;
    session.collection_mode = ToString(turbo_metrics_collection_mode_);
    session.live = !final_flush;
    session.started_unix_seconds = turbo_metrics_started_unix_seconds_;
    session.updated_unix_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    static constexpr const char* kStateNames[3] = {"off", "async", "sequenced"};
    for (size_t i = 0; i < turbo_metrics_accum_.size(); ++i) {
        const TurboMetricsAccum& accum = turbo_metrics_accum_[i];
        if (accum.frames == 0 && accum.discarded_frames == 0) {
            continue;
        }
        TurboMetricsBucket bucket;
        bucket.state = kStateNames[i];
        bucket.frames = accum.frames;
        bucket.seconds = accum.delta_sum_ms / 1000.0;
        bucket.max_frame_ms = accum.delta_max_ms;
        bucket.fabricated_waits = accum.fabricated_waits;
        bucket.drain_timeouts = accum.drain_timeouts;
        bucket.discarded_frames = accum.discarded_frames;
        if (accum.frames > 0) {
            bucket.avg_frame_ms = accum.delta_sum_ms / accum.frames;
            bucket.avg_fps =
                accum.delta_sum_ms > 0.0 ? accum.frames * 1000.0 / accum.delta_sum_ms : 0.0;
            bucket.avg_wait_block_ms = accum.wait_block_sum_ms / accum.frames;
            // p99 from the histogram: the smallest bin upper edge covering 99%
            // of samples, clamped to the exact observed max.
            const std::int64_t target = accum.frames - accum.frames / 100;
            std::int64_t cumulative = 0;
            double p99_ms = accum.delta_max_ms;
            for (size_t bin = 0; bin < accum.histogram.size(); ++bin) {
                cumulative += accum.histogram[bin];
                if (cumulative >= target) {
                    p99_ms = static_cast<double>(bin + 1) * kTurboMetricsHistogramBinMs;
                    break;
                }
            }
            bucket.p99_frame_ms = std::min(p99_ms, accum.delta_max_ms);
        }
        session.buckets.push_back(std::move(bucket));
    }
    if (session.buckets.empty()) {
        return;
    }

    if (final_flush) {
        std::string error;
        if (!RecordTurboMetricsSession(ResolveTurboMetricsPath(), session, &error)) {
            logger_.Info("Turbo metrics: failed to record session: " + error);
        }
    } else {
        // Never block the frame thread on the filesystem: skip this flush if
        // the previous async write is still in flight (data stays dirty and
        // rides the next interval).
        if (turbo_metrics_write_future_.valid() &&
            turbo_metrics_write_future_.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
            return;
        }
        turbo_metrics_write_future_ =
            std::async(std::launch::async,
                       [path = ResolveTurboMetricsPath(), snapshot = std::move(session)] {
                           std::string error;
                           RecordTurboMetricsSession(path, snapshot, &error);
                       });
    }
    turbo_metrics_dirty_ = false;
}

void OpenXrLayer::NoteTurboShouldRenderLocked(bool should_render) {
    if (last_noted_should_render_.has_value() && *last_noted_should_render_ == should_render) {
        return;
    }
    const bool first_observation = !last_noted_should_render_.has_value();
    last_noted_should_render_ = should_render;
    // TRUE from the first frame is the steady state — not worth a line.
    if ((first_observation && should_render) || should_render_log_budget_ <= 0) {
        return;
    }
    --should_render_log_budget_;
    logger_.Info(std::string("Runtime frame state shouldRender is now ") +
                 (should_render ? "TRUE" : "FALSE (apps submit empty frames while this holds)") +
                 (should_render_log_budget_ == 0 ? "; further changes suppressed this session."
                                                 : "."));
}

void OpenXrLayer::ResetTurboMetricsState() {
    // Serialize against an in-flight periodic write first: the final flush
    // below must be the last write, or a stale "live" snapshot could land
    // after it and stick forever.
    if (turbo_metrics_write_future_.valid()) {
        turbo_metrics_write_future_.wait();
        turbo_metrics_write_future_ = {};
    }
    FlushTurboMetrics(true);
    turbo_metrics_accum_ = {};
    turbo_metrics_session_id_.clear();
    turbo_metrics_started_unix_seconds_ = 0;
    turbo_metrics_last_end_time_.reset();
    turbo_metrics_last_flush_time_.reset();
    turbo_metrics_was_capturing_ = false;
    turbo_metrics_dirty_ = false;
    turbo_metrics_capture_armed_ = false;
    turbo_metrics_binding_was_down_ = false;
    turbo_metrics_binding_last_poll_time_.reset();
    turbo_metrics_binding_down_cached_ = false;
    std::scoped_lock lock(turbo_mutex_);
    turbo_metrics_wait_pending_ms_ = 0.0;
    turbo_metrics_fabricated_pending_ = 0;
}

void OpenXrLayer::DrainTurboAsyncWait() {
    // Wait without consuming the future: the pipelined frame is still pending
    // and will be begun/ended by the normal path — only the blocking runtime
    // call must finish before teardown proceeds.
    std::unique_lock lock(turbo_mutex_);
    if (turbo_async_wait_.valid()) {
        lock.unlock();
        turbo_async_wait_.wait();
    }
}

void OpenXrLayer::ResetTurboFrameState() {
    // Final metrics flush first (it takes turbo_mutex_ itself and joins the
    // async writer); a second call at teardown is a no-op.
    ResetTurboMetricsState();
    end_frame_error_log_budget_ = 5;
    submission_transition_log_budget_ = 8;
    app_submitting_layers_.reset();
    std::scoped_lock lock(turbo_mutex_);
    should_render_log_budget_ = 8;
    last_noted_should_render_.reset();
    // Destroying a std::async future joins its thread, which is the drain we
    // want at session teardown.
    turbo_async_wait_ = {};
    turbo_async_wait_polled_ = false;
    turbo_async_wait_completed_ = false;
    turbo_last_predicted_display_time_ = 0;
    turbo_last_predicted_display_period_ = 0;
    turbo_last_should_render_ = true;
    turbo_max_returned_display_time_ = 0;
    turbo_last_wait_frame_wall_time_.reset();
    turbo_pipelining_logged_ = false;
    turbo_fabricated_wait_log_budget_ = 0;
    turbo_drain_timeout_count_ = 0;
    turbo_timeout_window_start_.reset();
    turbo_auto_suspended_.store(false, std::memory_order_relaxed);
    turbo_seq_state_ = TurboSequencedState::kInactive;
    turbo_begin_owed_ = false;
    turbo_end_frame_in_flight_ = false;
    turbo_begin_deferred_ = false;
    turbo_frame_begun_ = false;
    turbo_seq_debug_log_budget_ = 0;
    turbo_valve_open_ = false;
    turbo_pacing_tokens_ = 0;
    turbo_valve_cv_.notify_all();
    // Pacing resolution survives (it is per instance/runtime); the stability
    // window and cadence gate cannot span sessions.
    turbo_stable_accumulated_ms_ = 0.0;
    turbo_cadence_healthy_streak_ = 0;
    turbo_cadence_ready_ = false;
    turbo_cadence_pause_logged_ = false;
    turbo_last_frame_blocked_ms_ = 0.0;
}

XrResult OpenXrLayer::EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    if (!frame_end_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    // Hang forensics: if a log ends between these two markers, a thread is
    // parked inside a runtime call while holding mutex_.
    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: xrEndFrame entered; acquiring config lock.");
    }
    // unique_lock because ForwardEndFrame releases it before blocking runtime
    // calls (turbo drain + next_end_frame_).
    std::unique_lock lock(mutex_);
    if (diag) {
        logger_.Debug("Turbo-diag: xrEndFrame config lock acquired.");
    }
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    if (!resolved_settings_.core.enabled) {
        cached_pivot_pose_deltas_.clear();
        cached_quadviews_fovs_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        const XrResult release_result = FlushDeferredSwapchainReleasesLocked("end frame");
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        PruneQuadViewsFovs(frame_end_info->displayTime);
        if (XR_FAILED(release_result)) {
            return release_result;
        }
        return ForwardEndFrame(session, frame_end_info, lock);
    }

    const bool quadviews_projection_split_active =
        IsQuadViewsActive() && has_active_primary_view_configuration_ &&
        IsQuadViewConfiguration(active_primary_view_configuration_type_) &&
        active_runtime_view_configuration_type_ == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    // Varjo compatible quadviews: the focus sharpen runs inside the projection
    // rewrite loop below, so a requested sharpen must keep this frame out of the
    // identity-delta fast path — otherwise sharpening only ever ran on frames
    // where PivotXR happened to produce a correction. Threshold matches
    // SharpenNativeFocusViews (amount <= 0.001 is treated as off).
    const bool needs_native_focus_sharpen =
        varjo_compatible_quadviews_active_ && IsQuadViewsActive() &&
        Clamp(resolved_settings_.quadviews.foveate_sharpness, 0.0, 100.0) / 100.0 > 0.001;
    const bool should_log_end_frame_diagnostic =
        pending_end_frame_diagnostics_ > 0 ||
        (quadviews_projection_split_active &&
         ShouldLogQuadViewsDebugHeartbeat(last_quadviews_end_frame_debug_heartbeat_));

    if (!resolved_settings_.pivotxr.enabled) {
        cached_pivot_pose_deltas_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
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
    if (should_log_end_frame_diagnostic) {
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
                const ViewOrientation submitted_orientation = ToViewOrientation(view.pose.orientation);
                stream << " view" << v << "Pos=(" << FormatDiagnosticDouble(view.pose.position.x) << ", "
                       << FormatDiagnosticDouble(view.pose.position.y) << ", "
                       << FormatDiagnosticDouble(view.pose.position.z) << ")"
                       << " view" << v << "Yaw=" << FormatDiagnosticDouble(ExtractYawRadians(submitted_orientation))
                       << " view" << v << "Pitch=" << FormatDiagnosticDouble(ExtractPitchRadians(submitted_orientation))
                       << " view" << v << "Fov=(" << FormatDiagnosticDouble(view.fov.angleLeft) << ", "
                       << FormatDiagnosticDouble(view.fov.angleRight) << ", "
                       << FormatDiagnosticDouble(view.fov.angleUp) << ", "
                       << FormatDiagnosticDouble(view.fov.angleDown) << ")";
            }
            logger_.Debug(stream.str());
            break;
        }
    }

    if (!has_non_identity_delta && !quadviews_projection_split_active && !needs_native_focus_sharpen) {
        if (should_log_end_frame_diagnostic) {
            std::ostringstream stream;
            stream << "EndFrame pivot correction skipped: cacheHit=" << has_pose_delta
                   << ", frameTime=" << frame_end_info->displayTime;
            if (has_pose_delta) {
                stream << ", matchedTime=" << matched_time
                       << ", matchedDeltaNs=" << (matched_time - frame_end_info->displayTime);
            }
            stream << ", cachedPivotDeltas=" << cached_pivot_pose_deltas_.size();
            logger_.Debug(stream.str());
            if (pending_end_frame_diagnostics_ > 0) {
                --pending_end_frame_diagnostics_;
            }
        }
        const XrResult release_result = FlushDeferredSwapchainReleasesLocked("end frame");
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        PruneQuadViewsFovs(frame_end_info->displayTime);
        if (XR_FAILED(release_result)) {
            return release_result;
        }
        return ForwardEndFrame(session, frame_end_info, lock);
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
        // Varjo compatible quadviews: sharpen the runtime's focus views in place when
        // foveate_sharpness > 0. No-op (pure passthrough) when sharpness is 0 or the
        // layer is not a native quad projection.
        if (varjo_compatible_quadviews_active_ && projection_layer->viewCount >= 4 && IsQuadViewsActive()) {
            SharpenNativeFocusViews(adjusted_projection_views.back(), frame_end_info->displayTime);
        }
    }

    XrFrameEndInfo adjusted_frame_end_info = *frame_end_info;
    adjusted_frame_end_info.layerCount = static_cast<uint32_t>(adjusted_layers.size());
    adjusted_frame_end_info.layers = adjusted_layers.data();
    if (should_log_end_frame_diagnostic) {
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
        if (pending_end_frame_diagnostics_ > 0) {
            --pending_end_frame_diagnostics_;
        }
    }
    const XrResult release_result = FlushDeferredSwapchainReleasesLocked("end frame");
    PrunePivotPoseDeltas(frame_end_info->displayTime);
    PruneQuadViewsFovs(frame_end_info->displayTime);
    if (XR_FAILED(release_result)) {
        return release_result;
    }

    // Pruning happens above, while the lock is still held: ForwardEndFrame
    // releases mutex_ before forwarding to the runtime.
    return ForwardEndFrame(session, &adjusted_frame_end_info, lock);
}

XrResult OpenXrLayer::GetReferenceSpaceBoundsRect(XrSession session,
                                                  XrReferenceSpaceType reference_space_type,
                                                  XrExtent2Df* bounds) {
    if (reference_space_type == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO) {
        bool emulate_combined_eye = false;
        {
            std::scoped_lock lock(mutex_);
            ReloadConfigIfNeeded();
            RefreshResolvedSettings();
            // In Varjo compatible mode the runtime provides the real combined-eye space,
            // so only emulate it while synthesizing quad views.
            emulate_combined_eye =
                session == active_session_ && IsQuadViewsActive() && !varjo_compatible_quadviews_active_;
        }

        if (emulate_combined_eye) {
            // The emulated combined-eye (gaze) space has no play-area bounds, and
            // the downstream runtime never created it. Report bounds unavailable
            // instead of forwarding a Varjo enum the runtime would reject.
            if (bounds) {
                bounds->width = 0.0f;
                bounds->height = 0.0f;
            }
            return XR_SPACE_BOUNDS_UNAVAILABLE;
        }
    }

    return next_get_reference_space_bounds_rect_(session, reference_space_type, bounds);
}

XrResult OpenXrLayer::EnumerateReferenceSpaces(XrSession session,
                                               uint32_t space_capacity_input,
                                               uint32_t* space_count_output,
                                               XrReferenceSpaceType* spaces) {
    if (!space_count_output) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    uint32_t runtime_count = 0;
    XrResult result = next_enumerate_reference_spaces_(session, 0, &runtime_count, nullptr);
    if (XR_FAILED(result)) {
        logger_.Error("xrEnumerateReferenceSpaces failed downstream (count query): result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }

    std::vector<XrReferenceSpaceType> runtime_spaces(runtime_count);
    if (runtime_count > 0) {
        result = next_enumerate_reference_spaces_(
            session, runtime_count, &runtime_count, runtime_spaces.data());
        if (XR_FAILED(result)) {
            logger_.Error("xrEnumerateReferenceSpaces failed downstream (populate): result=" +
                          std::to_string(static_cast<int>(result)));
            return result;
        }
        runtime_spaces.resize(runtime_count);
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();

    std::vector<XrReferenceSpaceType> exposed_spaces = runtime_spaces;
    if (session == active_session_ && IsQuadViewsActive() &&
        std::find(exposed_spaces.begin(),
                  exposed_spaces.end(),
                  XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO) == exposed_spaces.end()) {
        exposed_spaces.push_back(XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO);
    }

    *space_count_output = static_cast<uint32_t>(exposed_spaces.size());
    if (!spaces || space_capacity_input == 0) {
        return XR_SUCCESS;
    }

    const uint32_t copy_count =
        std::min<uint32_t>(space_capacity_input, static_cast<uint32_t>(exposed_spaces.size()));
    std::copy_n(exposed_spaces.begin(), copy_count, spaces);
    return space_capacity_input < exposed_spaces.size() ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
}

XrResult OpenXrLayer::CreateReferenceSpace(XrSession session,
                                           const XrReferenceSpaceCreateInfo* create_info,
                                           XrSpace* space) {
    logger_.Info(std::string("xrCreateReferenceSpace requested by application: referenceSpaceType=") +
                 (create_info ? std::to_string(static_cast<int>(create_info->referenceSpaceType)) : "null"));

    if (create_info && space &&
        create_info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO) {
        bool emulate_combined_eye = false;
        {
            std::scoped_lock lock(mutex_);
            ReloadConfigIfNeeded();
            RefreshResolvedSettings();
            // In Varjo compatible mode the runtime provides the real combined-eye space,
            // so only emulate it while synthesizing quad views.
            emulate_combined_eye =
                session == active_session_ && IsQuadViewsActive() && !varjo_compatible_quadviews_active_;
        }

        if (emulate_combined_eye) {
            XrReferenceSpaceCreateInfo runtime_create_info = *create_info;
            runtime_create_info.next = nullptr;
            runtime_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            const XrResult result = next_create_reference_space_(session, &runtime_create_info, space);
            if (XR_FAILED(result)) {
                logger_.Error("xrCreateReferenceSpace emulated COMBINED_EYE_VARJO failed downstream VIEW: result=" +
                              std::to_string(static_cast<int>(result)));
                return result;
            }

            std::scoped_lock lock(mutex_);
            tracked_view_spaces_.insert(*space);
            logger_.Info("Emulated XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO with runtime VIEW reference space.");
            return result;
        }
    }

    const XrResult result = next_create_reference_space_(session, create_info, space);
    if (XR_FAILED(result)) {
        logger_.Error("xrCreateReferenceSpace failed downstream: result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }
    if (!create_info || !space) {
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
    bool enabled = false;
    bool pivotxr_active = false;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        enabled = resolved_settings_.core.enabled;
        if (enabled) {
            pivotxr_active = IsPivotXrActive();
        }
    }

    if (!enabled) {
        return next_locate_space_(space, base_space, time, location);
    }

    // Quad-view sessions must keep xrLocateSpace consistent with the pivoted
    // view poses returned from xrLocateViews. Apps such as DCS place
    // head-attached geometry (e.g. the FA18 pilot visor) from VIEW-space
    // locates; skipping pivot here desynchronizes that geometry from the
    // pivoted camera.
    //
    // Lock discipline: the runtime locate must NOT run under mutex_ — a
    // runtime may block a locate (e.g. until frame prediction advances via
    // the next xrWaitFrame, which sequenced turbo performs inside EndFrame
    // behind this same mutex), and holding the config lock across it turns
    // that stall into an app-wide deadlock.
    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: app xrLocateSpace starting (time=" + std::to_string(time) + ").");
    }
    const XrResult result = next_locate_space_(space, base_space, time, location);
    if (diag) {
        logger_.Debug("Turbo-diag: app xrLocateSpace completed.");
    }
    if (XR_FAILED(result)) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    return ApplyPivotToLocatedSpace(space, base_space, time, pivotxr_active, location, nullptr,
                                    nullptr, nullptr, false);
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

    const bool pivotxr_active = IsPivotXrActive();
    // Keep driving pivot while the activation envelope is still easing out so a
    // toggle-off releases the view smoothly instead of snapping back to center.
    const bool pivotxr_envelope_engaged =
        pivotxr_active || pivotxr_activation_gain_ > kPivotActivationGainEpsilon;
    const bool depthxr_active = IsDepthXrActive();
    if (resolved_settings_.pivotxr.enabled) {
        pivot_diagnostic_.recomposition_mode = pivotxr_envelope_engaged ? "pending" : "inactive";
    }
    if (resolved_settings_.pivotxr.enabled && !has_logged_pivotxr_spike_mode_) {
        std::ostringstream stream;
        stream << "PivotXR spike is active; quad-view sessions use stereo eye-pose recomposition.";
        for (const PivotXrResolvedProfile& profile : resolved_settings_.pivotxr.profiles) {
            stream << " Press " << BindingLabel(profile.activation_binding) << " to "
                   << (profile.activation_mode == ActivationMode::Toggle ? "toggle" : "hold") << " '"
                   << profile.name << "'.";
        }
        logger_.Info(stream.str());
        has_logged_pivotxr_spike_mode_ = true;
    }

    // Consume a pending origin capture with this frame's head pose. This runs
    // independently of engagement so the origin can be set before pivot is
    // engaged; the capture uses the same displayTime as the pivot drive below.
    const XrTime internal_locate_time =
        view_locate_info ? ClampInternalLocateTime(view_locate_info->displayTime) : 0;

    if (pivotxr_origin_capture_pending_ && resolved_settings_.pivotxr.enabled &&
        internal_view_space_ != XR_NULL_HANDLE && view_locate_info) {
        XrSpaceLocation origin_location{XR_TYPE_SPACE_LOCATION};
        const XrResult origin_result = next_locate_space_(
            internal_view_space_, view_locate_info->space, internal_locate_time, &origin_location);
        if (XR_SUCCEEDED(origin_result) &&
            (origin_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            PivotOrigin origin;
            origin.yaw_radians = ExtractPoseYawRadians(origin_location.pose);
            origin.pitch_radians = ExtractPosePitchRadians(origin_location.pose);
            pivotxr_origin_ = origin;
            pivotxr_origin_capture_pending_ = false;
            std::ostringstream origin_stream;
            origin_stream << "PivotXR origin captured: yaw=" << FormatDiagnosticDouble(origin.yaw_radians)
                          << " pitch=" << FormatDiagnosticDouble(origin.pitch_radians) << " rad.";
            logger_.Info(origin_stream.str());
        }
    }

    if (resolved_settings_.pivotxr.enabled && pivotxr_envelope_engaged && internal_view_space_ != XR_NULL_HANDLE &&
        view_locate_info) {
        XrSpaceLocation pivot_view_location{XR_TYPE_SPACE_LOCATION};
        double applied_extra_yaw_radians = 0.0;
        double applied_extra_pitch_radians = 0.0;
        XrPosef applied_pose_delta = IdentityPose();
        const XrResult pivot_result = LocateSpaceWithPivot(internal_view_space_,
                                                           view_locate_info->space,
                                                           internal_locate_time,
                                                           pivotxr_active,
                                                           &pivot_view_location,
                                                           &applied_extra_yaw_radians,
                                                           &applied_extra_pitch_radians,
                                                           &applied_pose_delta,
                                                           true);
        if (XR_SUCCEEDED(pivot_result)) {
            if (NearlyZero(applied_extra_yaw_radians) && NearlyZero(applied_extra_pitch_radians)) {
                pivot_diagnostic_.recomposition_mode = "identity";
                pivot_diagnostic_.has_eye_offsets = false;
                CachePivotPoseDelta(view_locate_info->displayTime, IdentityPose());
            } else if (IsQuadViewConfiguration(view_configuration_type) &&
                       EnsureEyeOffsets(session,
                                        // Emulation synthesizes quad from stereo, so it captures 2 stereo eye
                                        // offsets. Varjo compatible mode locates the real quad config (same config as
                                        // the session) to get one offset per view and avoid a cross-config locate.
                                        varjo_compatible_quadviews_active_
                                            ? view_configuration_type
                                            : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                        internal_locate_time,
                                        varjo_compatible_quadviews_active_ ? count : 2)) {
                pivot_diagnostic_.recomposition_mode =
                    varjo_compatible_quadviews_active_ ? "quad_native_eye_offsets" : "quad_from_stereo_eye_offsets";
                CachePivotPoseDelta(view_locate_info->displayTime, applied_pose_delta);
                for (uint32_t i = 0; i < count; ++i) {
                    // Native: one offset per view. Emulation: focus views (2/3) reuse the
                    // same eye offset as their peripheral counterpart (i % 2).
                    const uint32_t eye_index = varjo_compatible_quadviews_active_ ? i : (i % 2);
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
                       EnsureEyeOffsets(session, view_configuration_type, internal_locate_time, count)) {
                pivot_diagnostic_.recomposition_mode = "stereo_eye_offsets";
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
                pivot_diagnostic_.recomposition_mode = "eye_offset_capture_failed";
                pivot_diagnostic_.has_eye_offsets = false;
                CachePivotPoseDelta(view_locate_info->displayTime, IdentityPose());
            }
            // LocateSpaceWithPivot owns the steady-state smoothed angles and the
            // activation envelope; do not write the eased output back onto them.
        } else {
            pivot_diagnostic_.recomposition_mode = "locate_space_failed";
        }
    } else if (!pivotxr_envelope_engaged) {
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
    }

    if (depthxr_active && !NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0)) {
        ApplyStereoBoost(adjusted_views, resolved_settings_.depthxr.stereo_boost, view_layout);
    }
    if (depthxr_active && !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0)) {
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

    const XrTime locate_time = view_locate_info ? view_locate_info->displayTime : 0;
    bool should_log_pivot_diagnostic = false;
    if (resolved_settings_.pivotxr.enabled && logger_.IsDebugEnabled()) {
        const bool has_matching_pivot_pose =
            pivot_diagnostic_.has_view_pose && pivot_diagnostic_.view_time == locate_time;
        const bool has_pivot_context = pivotxr_envelope_engaged || has_matching_pivot_pose;
        if (has_pivot_context) {
            if (pivotxr_envelope_engaged) {
                ++pivot_diagnostic_stride_counter_;
            }
            should_log_pivot_diagnostic = pending_pivot_diagnostics_ > 0 ||
                                          (pivotxr_envelope_engaged &&
                                           pivot_diagnostic_stride_counter_ % kPivotDiagnosticStride == 0);
        }
    }

    const bool should_log_quadviews_locate_heartbeat =
        (IsQuadViewConfiguration(view_configuration_type) || synthesized_quad_views) &&
        ShouldLogQuadViewsDebugHeartbeat(last_quadviews_locate_debug_heartbeat_);
    if (pending_locate_views_diagnostics_ > 0 || should_log_pivot_diagnostic ||
        should_log_quadviews_locate_heartbeat) {
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

        if (pending_locate_views_diagnostics_ > 0 || should_log_quadviews_locate_heartbeat) {
            std::ostringstream stream;
            stream << "LocateViews "
                   << (should_log_quadviews_locate_heartbeat && pending_locate_views_diagnostics_ == 0 ?
                           "quadviews heartbeat " : "call ")
                   << locate_views_call_count_ << ": count=" << count
                   << ", viewConfig=" << ToString(view_configuration_type)
                   << ", synthesizedQuadViews=" << synthesized_quad_views
                   << ", varjoCompatible=" << (varjo_compatible_quadviews_active_ ? 1 : 0)
                   << ", quadviewsTrackingMode=" << ToString(resolved_settings_.quadviews.tracking_mode)
                   << ", quadviewsFocusScale=" << FormatDiagnosticDouble(resolved_settings_.quadviews.focus_scale)
                   << ", quadviewsPeripheralScale="
                   << FormatDiagnosticDouble(resolved_settings_.quadviews.peripheral_scale)
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
                   << ", stereoBoost=" << FormatDiagnosticDouble(resolved_settings_.depthxr.stereo_boost)
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
            if (pending_locate_views_diagnostics_ > 0) {
                --pending_locate_views_diagnostics_;
            }
        }

        if (should_log_pivot_diagnostic) {
            const bool view_pose_fresh = pivot_diagnostic_.has_view_pose && pivot_diagnostic_.view_time == locate_time;
            std::ostringstream stream;
            stream << "Pivot diagnostic: frameTime=" << locate_time
                   << ", locateCall=" << locate_views_call_count_
                   << ", viewConfig=" << ToString(view_configuration_type)
                   << ", recomposition=" << pivot_diagnostic_.recomposition_mode
                   << ", pivotActive=" << pivot_diagnostic_.pivot_active
                   << ", envelopeEngaged=" << pivotxr_envelope_engaged
                   << ", viewPoseFresh=" << view_pose_fresh
                   << ", rawYaw=" << FormatDiagnosticDouble(pivot_diagnostic_.raw_yaw_radians)
                   << ", rawPitch=" << FormatDiagnosticDouble(pivot_diagnostic_.raw_pitch_radians)
                   << ", steadyExtraYaw=" << FormatDiagnosticDouble(pivot_diagnostic_.steady_extra_yaw_radians)
                   << ", steadyExtraPitch=" << FormatDiagnosticDouble(pivot_diagnostic_.steady_extra_pitch_radians)
                   << ", easedExtraYaw=" << FormatDiagnosticDouble(pivot_diagnostic_.eased_extra_yaw_radians)
                   << ", easedExtraPitch=" << FormatDiagnosticDouble(pivot_diagnostic_.eased_extra_pitch_radians)
                   << ", activationGain=" << FormatDiagnosticDouble(pivot_diagnostic_.activation_gain)
                   << ", originActive=" << pivot_diagnostic_.origin_active
                   << ", originYaw=" << FormatDiagnosticDouble(pivot_diagnostic_.origin_yaw_radians)
                   << ", originPitch=" << FormatDiagnosticDouble(pivot_diagnostic_.origin_pitch_radians)
                   << ", yawStep=" << pivot_diagnostic_.yaw_step
                   << ", pitchStep=" << pivot_diagnostic_.pitch_step
                   << ", locationFlags=" << FormatHex(static_cast<uint64_t>(pivot_diagnostic_.view_location_flags))
                   << ", spaceIsView=" << pivot_diagnostic_.space_is_view
                   << ", baseSpaceIsView=" << pivot_diagnostic_.base_space_is_view
                   << ", leftYawDelta=" << FormatDiagnosticDouble(left_yaw_delta)
                   << ", rightYawDelta=" << FormatDiagnosticDouble(right_yaw_delta)
                   << ", leftInsetYawDelta=" << FormatDiagnosticDouble(left_inset_yaw_delta)
                   << ", rightInsetYawDelta=" << FormatDiagnosticDouble(right_inset_yaw_delta)
                   << ", leftPitchDelta=" << FormatDiagnosticDouble(left_pitch_delta)
                   << ", rightPitchDelta=" << FormatDiagnosticDouble(right_pitch_delta)
                   << ", leftXDelta=" << FormatDiagnosticDouble(left_position_delta)
                   << ", rightXDelta=" << FormatDiagnosticDouble(right_position_delta)
                   << ", leftProjCenterDelta=" << FormatDiagnosticDouble(left_projection_center_delta)
                   << ", rightProjCenterDelta=" << FormatDiagnosticDouble(right_projection_center_delta);

            if (pivot_diagnostic_.has_eye_offsets) {
                stream << ", eyeOffsetTime=" << pivot_diagnostic_.eye_offsets_time
                       << ", eyeOffsetConfig=" << ToString(pivot_diagnostic_.eye_offsets_view_configuration)
                       << ", eyeOffsetCount=" << pivot_diagnostic_.eye_offset_count;
                if (pivot_diagnostic_.eye_offset_count >= 2) {
                    const XrPosef& left_offset = pivot_diagnostic_.eye_offsets[0];
                    const XrPosef& right_offset = pivot_diagnostic_.eye_offsets[1];
                    const double dx = static_cast<double>(right_offset.position.x) - left_offset.position.x;
                    const double dy = static_cast<double>(right_offset.position.y) - left_offset.position.y;
                    const double dz = static_cast<double>(right_offset.position.z) - left_offset.position.z;
                    stream << ", eyeOffsetSeparation=" << FormatDiagnosticDouble(std::sqrt(dx * dx + dy * dy + dz * dz))
                           << ", eyeOffsetMidpoint=("
                           << FormatDiagnosticDouble((left_offset.position.x + right_offset.position.x) * 0.5) << ", "
                           << FormatDiagnosticDouble((left_offset.position.y + right_offset.position.y) * 0.5) << ", "
                           << FormatDiagnosticDouble((left_offset.position.z + right_offset.position.z) * 0.5) << ")";
                }
                for (uint32_t i = 0; i < pivot_diagnostic_.eye_offset_count; ++i) {
                    const std::string label = "eyeOffset" + std::to_string(i);
                    stream << ", ";
                    AppendPoseSummary(stream, label, pivot_diagnostic_.eye_offsets[i]);
                }
            } else {
                stream << ", eyeOffsets=unavailable";
            }

            logger_.Debug(stream.str());
            if (pending_pivot_diagnostics_ > 0) {
                --pending_pivot_diagnostics_;
            }
        }
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
    // Steady-state config reloads run off the render thread in the config
    // watcher (StartConfigWatcher / PollConfigFile). This path performs only the
    // one-time initial load at instance creation, before the watcher starts, so
    // the very first frame already sees the user's settings. Once the config is
    // loaded it is a cheap single-bool early-out and never touches the
    // filesystem on a hot path again.
    if (has_loaded_config_) {
        return;
    }

    if (config_path_.empty()) {
        config_path_ = ResolveConfigPath();
    }

    std::error_code ec;
    if (!std::filesystem::exists(config_path_, ec) || ec) {
        config_ = DefaultConfig();
        has_loaded_config_ = true;
        ++config_generation_;
        logger_.Info("No config file found. Using default settings.");
        return;
    }

    const auto timestamp = std::filesystem::last_write_time(config_path_, ec);
    const ParseResult loaded = LoadConfigFromFile(config_path_);
    if (!loaded.ok) {
        logger_.Error("Failed to parse config: " + loaded.error);
        if (!ec) {
            last_failed_config_write_time_ = timestamp;
            has_failed_config_timestamp_ = true;
        }
        last_failed_config_error_ = loaded.error;
        config_ = DefaultConfig();
        has_loaded_config_ = true;
        ++config_generation_;
        return;
    }

    config_ = loaded.document;
    has_loaded_config_ = true;
    has_failed_config_timestamp_ = false;
    last_failed_config_error_.clear();
    if (!ec) {
        has_config_timestamp_ = true;
        last_config_write_time_ = timestamp;
    }
    ++config_generation_;
    logger_.Info("Loaded config from " + config_path_.string());
}

void OpenXrLayer::StartConfigWatcher() {
    if (config_watcher_thread_.joinable()) {
        return;
    }
    {
        std::scoped_lock watcher_lock(config_watcher_mutex_);
        config_watcher_stop_ = false;
    }
    config_watcher_thread_ = std::thread([this] { ConfigWatcherLoop(); });
}

void OpenXrLayer::StopConfigWatcher() {
    // Must be called WITHOUT holding mutex_: the watcher may be inside
    // PollConfigFile waiting on mutex_, and join() would otherwise deadlock.
    {
        std::scoped_lock watcher_lock(config_watcher_mutex_);
        config_watcher_stop_ = true;
    }
    config_watcher_cv_.notify_all();
    if (config_watcher_thread_.joinable()) {
        config_watcher_thread_.join();
    }
}

void OpenXrLayer::ConfigWatcherLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> watcher_lock(config_watcher_mutex_);
            config_watcher_cv_.wait_for(
                watcher_lock, kConfigCheckInterval, [this] { return config_watcher_stop_; });
            if (config_watcher_stop_) {
                return;
            }
        }
        PollConfigFile();
    }
}

void OpenXrLayer::PollConfigFile() {
    // Runs on the watcher thread. Every filesystem operation below executes
    // WITHOUT holding mutex_; the lock is taken only to read the last-known
    // timestamp and to publish a parsed change, so render-thread frames are
    // never stalled by filesystem latency (or antivirus interception of it).
    std::filesystem::path path;
    {
        std::scoped_lock lock(mutex_);
        path = config_path_;
    }
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return;  // File missing or unreadable; keep the last good config.
    }

    const auto timestamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;  // Transient stat failure; retry on the next tick.
    }

    {
        std::scoped_lock lock(mutex_);
        if (has_loaded_config_ && has_config_timestamp_ && timestamp == last_config_write_time_) {
            return;  // Unchanged since the last successful load.
        }
        if (has_failed_config_timestamp_ && timestamp == last_failed_config_write_time_) {
            return;  // Same file we already parsed and reported as invalid.
        }
    }

    // Parse outside the lock: this is the expensive part (file read + JSON parse).
    ParseResult loaded = LoadConfigFromFile(path);

    bool log_parse_failure = false;
    bool log_reload_success = false;
    {
        std::scoped_lock lock(mutex_);
        if (!loaded.ok) {
            const bool already_reported_failure =
                has_failed_config_timestamp_ && timestamp == last_failed_config_write_time_ &&
                loaded.error == last_failed_config_error_;
            log_parse_failure = !already_reported_failure;
            last_failed_config_write_time_ = timestamp;
            has_failed_config_timestamp_ = true;
            last_failed_config_error_ = loaded.error;
        } else {
            config_ = std::move(loaded.document);
            has_loaded_config_ = true;
            has_config_timestamp_ = true;
            last_config_write_time_ = timestamp;
            has_failed_config_timestamp_ = false;
            last_failed_config_error_.clear();
            ++config_generation_;
            log_reload_success = true;
        }
    }

    // Log after releasing mutex_ so a render-thread frame waiting on the lock is
    // never blocked behind the logger's disk flush. Logger has its own mutex, so
    // logging off the layer lock is safe.
    if (log_parse_failure) {
        logger_.Error("Failed to parse config: " + loaded.error);
    } else if (log_reload_success) {
        logger_.Info("Reloaded config from " + path.string());
    }
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
    if (!SamePivotActivationSet(previous.pivotxr, resolved_settings_.pivotxr)) {
        ResetPivotActivationState();
        has_logged_pivotxr_spike_mode_ = false;
    }
    if (!SameInputBinding(previous.turbo.toggle_binding, resolved_settings_.turbo.toggle_binding) ||
        previous.turbo.enabled != resolved_settings_.turbo.enabled) {
        ResetTurboToggleState();
    }
    if (!SameInputBinding(previous.turbo.metrics_binding, resolved_settings_.turbo.metrics_binding) ||
        previous.turbo.metrics_mode != resolved_settings_.turbo.metrics_mode) {
        // Re-prime the capture binding's edge detector; a mode change also
        // disarms binding-gated capture so it always starts paused.
        turbo_metrics_capture_armed_ = false;
        turbo_metrics_binding_was_down_ = false;
        turbo_metrics_binding_last_poll_time_.reset();
        turbo_metrics_binding_down_cached_ = false;
    }
    if (previous.turbo.pacing_mode != resolved_settings_.turbo.pacing_mode ||
        previous.turbo.runtime_pins != resolved_settings_.turbo.runtime_pins) {
        // Re-resolve at the next engaged frame so a UI change (mode, pins,
        // re-discover) applies without restarting the session.
        turbo_pacing_resolved_ = false;
    }
    logger_.SetLevel(resolved_settings_.core.log_level);
    logger_.SetRetentionFiles(resolved_settings_.core.log_retention_files);
    if (!last_logged_settings_ || !SameSettings(*last_logged_settings_, resolved_settings_)) {
        LogResolvedSettings(resolved_settings_);
        last_logged_settings_ = resolved_settings_;
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        pending_pivot_diagnostics_ = kPivotDiagnosticBurstCount;
        pending_eye_gaze_diagnostics_ = 10;
        pending_eye_gaze_sync_diagnostics_ = 10;
        ResetQuadViewsDebugHeartbeatState();
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetInstanceProperties", &function))) {
        next_get_instance_properties_ = reinterpret_cast<PFN_xrGetInstanceProperties>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrWaitFrame", &function))) {
        next_wait_frame_ = reinterpret_cast<PFN_xrWaitFrame>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrBeginFrame", &function))) {
        next_begin_frame_ = reinterpret_cast<PFN_xrBeginFrame>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateSwapchainFormats", &function))) {
        next_enumerate_swapchain_formats_ = reinterpret_cast<PFN_xrEnumerateSwapchainFormats>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateReferenceSpaces", &function))) {
        next_enumerate_reference_spaces_ = reinterpret_cast<PFN_xrEnumerateReferenceSpaces>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetReferenceSpaceBoundsRect", &function))) {
        next_get_reference_space_bounds_rect_ = reinterpret_cast<PFN_xrGetReferenceSpaceBoundsRect>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrPathToString", &function))) {
        next_path_to_string_ = reinterpret_cast<PFN_xrPathToString>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetCurrentInteractionProfile", &function))) {
        next_get_current_interaction_profile_ = reinterpret_cast<PFN_xrGetCurrentInteractionProfile>(function);
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
    session_begin_wall_time_.reset();
    pending_end_frame_diagnostics_ = 0;
    pending_eye_gaze_diagnostics_ = 0;
    pending_eye_gaze_sync_diagnostics_ = 0;
    pending_quadviews_compositor_diagnostics_ = 0;
    eye_gaze_diagnostic_stride_counter_ = 0;
    pending_pivot_diagnostics_ = 0;
    pivot_diagnostic_stride_counter_ = 0;
    ResetQuadViewsDebugHeartbeatState();
    pivot_diagnostic_ = PivotDiagnosticState{};
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_yaw_step_ = 0;
    pivotxr_pitch_step_ = 0;
    pivotxr_activation_gain_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    ResetTurboToggleState();
    ResetTurboFrameState();
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
    has_logged_eye_gaze_focus_active_ = false;
    has_logged_eye_gaze_focus_unavailable_ = false;
    eye_gaze_unavailable_streak_ = 0;
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
    defer_quadviews_swapchain_releases_ = false;
    runtime_name_.clear();
    runtime_version_.clear();
    turbo_pacing_resolved_ = false;
    cached_quadviews_stereo_recommended_width_ = 0;
    cached_quadviews_stereo_recommended_height_ = 0;
    cached_quadviews_stereo_max_width_ = 0;
    cached_quadviews_stereo_max_height_ = 0;
    has_logged_system_properties_ = false;
}

bool OpenXrLayer::IsQuadViewsActive() const {
    return resolved_settings_.core.enabled && resolved_settings_.quadviews.enabled;
}

bool OpenXrLayer::IsVarjoCompatibleQuadviewsEligible() {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    // Varjo compatible quadviews is the default whenever the suite and the quadviews
    // module are enabled; the caller additionally requires that the runtime natively
    // supports Varjo quad views. There is no separate opt-in — a user who does not
    // want VectorXR quadviews simply disables the module (per profile or globally).
    return config_.core.enabled && config_.quadviews.enabled;
}

bool OpenXrLayer::ShouldLogQuadViewsDebugHeartbeat(
    std::optional<std::chrono::steady_clock::time_point>& last_heartbeat) {
    if (!logger_.IsDebugEnabled() || !IsQuadViewsActive()) {
        last_heartbeat.reset();
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_heartbeat.has_value() && now - *last_heartbeat < kQuadViewsDebugHeartbeatInterval) {
        return false;
    }
    last_heartbeat = now;
    return true;
}

void OpenXrLayer::ResetQuadViewsDebugHeartbeatState() {
    last_quadviews_locate_debug_heartbeat_.reset();
    last_quadviews_end_frame_debug_heartbeat_.reset();
    last_quadviews_compositor_debug_heartbeat_.reset();
    last_quadviews_eye_gaze_debug_heartbeat_.reset();
}

XrResult OpenXrLayer::CreateEyeGazeResources(XrSession session) {
    if (eye_gaze_resources_ready_) {
        return XR_SUCCESS;
    }
    if (!eye_gaze_extension_enabled_) {
        logger_.Info("VectorXR quadviews eye-gaze resources skipped: XR_EXT_eye_gaze_interaction "
                     "is not enabled downstream.");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    if (!next_string_to_path_ || !next_create_action_set_ || !next_create_action_ ||
        !next_suggest_interaction_profile_bindings_ || !next_create_action_space_) {
        logger_.Info("VectorXR quadviews eye-gaze resources unavailable: required OpenXR action "
                     "functions are missing.");
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }

    XrResult result = next_string_to_path_(
        instance_, "/interaction_profiles/ext/eye_gaze_interaction", &eye_gaze_interaction_profile_path_);
    if (XR_FAILED(result)) {
        logger_.Info("VectorXR quadviews eye-gaze interaction profile path lookup failed: result=" +
                     std::to_string(static_cast<int>(result)));
        return result;
    }
    result = next_string_to_path_(instance_, "/user/eyes_ext/input/gaze_ext/pose", &eye_gaze_pose_path_);
    if (XR_FAILED(result)) {
        logger_.Info("VectorXR quadviews eye-gaze pose path lookup failed: result=" +
                     std::to_string(static_cast<int>(result)));
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
        logger_.Info("VectorXR quadviews eye-gaze action set creation failed: result=" +
                     std::to_string(static_cast<int>(result)));
        return result;
    }

    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    CopyName(action_info.actionName, sizeof(action_info.actionName), "eye_gaze");
    CopyName(action_info.localizedActionName, sizeof(action_info.localizedActionName), "Eye Gaze");
    action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
    result = next_create_action_(quadviews_action_set_, &action_info, &quadviews_eye_gaze_action_);
    if (XR_FAILED(result)) {
        logger_.Info("VectorXR quadviews eye-gaze action creation failed: result=" +
                     std::to_string(static_cast<int>(result)));
        return result;
    }

    const XrActionSuggestedBinding suggested_binding{quadviews_eye_gaze_action_, eye_gaze_pose_path_};
    XrInteractionProfileSuggestedBinding profile_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    profile_bindings.interactionProfile = eye_gaze_interaction_profile_path_;
    profile_bindings.countSuggestedBindings = 1;
    profile_bindings.suggestedBindings = &suggested_binding;
    result = next_suggest_interaction_profile_bindings_(instance_, &profile_bindings);
    if (XR_FAILED(result)) {
        logger_.Info("VectorXR quadviews eye-gaze interaction profile binding suggestion failed: result=" +
                     std::to_string(static_cast<int>(result)));
        return result;
    }

    XrActionSpaceCreateInfo action_space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    action_space_info.action = quadviews_eye_gaze_action_;
    action_space_info.poseInActionSpace = IdentityPose();
    result = next_create_action_space_(session, &action_space_info, &quadviews_eye_gaze_space_);
    if (XR_FAILED(result)) {
        logger_.Info("VectorXR quadviews eye-gaze action space creation failed: result=" +
                     std::to_string(static_cast<int>(result)));
        return result;
    }

    eye_gaze_resources_ready_ = true;
    eye_gaze_action_set_attached_ = false;
    has_logged_eye_gaze_focus_active_ = false;
    has_logged_eye_gaze_focus_unavailable_ = false;
    eye_gaze_unavailable_streak_ = 0;
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
    has_logged_eye_gaze_focus_active_ = false;
    has_logged_eye_gaze_focus_unavailable_ = false;
    eye_gaze_unavailable_streak_ = 0;
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

    // Probe the runtime's current interaction profile for /user/eyes_ext. When our eye-gaze
    // action reports isActive=0, this reveals *why*: "none" means the runtime never bound the
    // eye-gaze profile (e.g. tracker not enabled/calibrated, or the extension isn't truly
    // active downstream), whereas the eye-gaze profile string means the binding took and the
    // problem is elsewhere. This is the single most useful signal for the Varjo blur report.
    auto describe_eye_profile = [&]() -> std::string {
        if (!next_get_current_interaction_profile_ || !next_string_to_path_ ||
            active_session_ == XR_NULL_HANDLE) {
            return "probe-unavailable";
        }
        XrPath user_path = XR_NULL_PATH;
        if (XR_FAILED(next_string_to_path_(instance_, "/user/eyes_ext", &user_path))) {
            return "no-user-path";
        }
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        const XrResult probe_result =
            next_get_current_interaction_profile_(active_session_, user_path, &state);
        if (XR_FAILED(probe_result)) {
            return "getCurrent-failed:" + FormatHex(static_cast<uint64_t>(probe_result));
        }
        if (state.interactionProfile == XR_NULL_PATH) {
            return "none";
        }
        if (next_path_to_string_) {
            char buffer[XR_MAX_PATH_LENGTH]{};
            uint32_t length = 0;
            if (XR_SUCCEEDED(next_path_to_string_(
                    instance_, state.interactionProfile, sizeof(buffer), &length, buffer))) {
                return std::string(buffer);
            }
        }
        return "path#" + std::to_string(static_cast<uint64_t>(state.interactionProfile));
    };

    auto log_eye_gaze_diagnostic = [&](const std::string& reason) {
        if (settings.tracking_mode == QuadViewsTrackingMode::Eye && !has_logged_eye_gaze_focus_unavailable_) {
            // Debounce: require the gaze to stay unavailable for several frames
            // before declaring it lost, so a single-frame dropout (a blink) does
            // not produce an unavailable/active churn pair in the log.
            ++eye_gaze_unavailable_streak_;
            if (eye_gaze_unavailable_streak_ >= kEyeGazeUnavailableLogThreshold) {
                logger_.Info("Quadviews eye-gaze focus unavailable; using head/static focus offsets. reason=" + reason +
                             ", resourcesReady=" + std::to_string(eye_gaze_resources_ready_) +
                             ", actionSetAttached=" + std::to_string(eye_gaze_action_set_attached_));
                has_logged_eye_gaze_focus_unavailable_ = true;
                has_logged_eye_gaze_focus_active_ = false;
            }
        }
        const bool should_log_unavailable_debug =
            settings.tracking_mode == QuadViewsTrackingMode::Eye &&
            (pending_eye_gaze_diagnostics_ > 0 ||
             ShouldLogQuadViewsDebugHeartbeat(last_quadviews_eye_gaze_debug_heartbeat_));
        if (should_log_unavailable_debug) {
            logger_.Debug("Quadviews eye-gaze focus unavailable: " + reason +
                          ", resourcesReady=" + std::to_string(eye_gaze_resources_ready_) +
                          ", actionSetAttached=" + std::to_string(eye_gaze_action_set_attached_) +
                          ", eyeInteractionProfile=" + describe_eye_profile() +
                          ", eyeGazeExtEnabled=" + std::to_string(eye_gaze_extension_enabled_) +
                          ", unavailableStreak=" + std::to_string(eye_gaze_unavailable_streak_));
            if (pending_eye_gaze_diagnostics_ > 0) {
                --pending_eye_gaze_diagnostics_;
            }
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
    const bool gaze_diag = TurboSequencedDebugTick();
    if (gaze_diag) {
        logger_.Debug("Turbo-diag: eye-gaze xrLocateSpace starting (under config lock).");
    }
    const XrResult locate_result = next_locate_space_(quadviews_eye_gaze_space_, gaze_base_space, time, &gaze_location);
    if (gaze_diag) {
        logger_.Debug("Turbo-diag: eye-gaze xrLocateSpace completed.");
    }
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
    const bool should_log_eye_gaze_burst =
        pending_eye_gaze_diagnostics_ > 0 &&
        (eye_gaze_diagnostic_stride_counter_ < 20 || eye_gaze_diagnostic_stride_counter_ % 45 == 0);
    const bool should_log_eye_gaze_heartbeat =
        !should_log_eye_gaze_burst &&
        ShouldLogQuadViewsDebugHeartbeat(last_quadviews_eye_gaze_debug_heartbeat_);
    ++eye_gaze_diagnostic_stride_counter_;
    if (should_log_eye_gaze_burst || should_log_eye_gaze_heartbeat) {
        logger_.Debug("Quadviews eye-gaze focus active: rawYaw=" + FormatDiagnosticDouble(target_yaw) +
                      ", rawPitch=" + FormatDiagnosticDouble(target_pitch) +
                      ", eulerYaw=" + FormatDiagnosticDouble(euler_yaw) +
                      ", eulerPitch=" + FormatDiagnosticDouble(euler_pitch) +
                      ", smoothedYaw=" + FormatDiagnosticDouble(*yaw_radians) +
                      ", smoothedPitch=" + FormatDiagnosticDouble(*pitch_radians) +
                      ", forward=(" + FormatDiagnosticDouble(gaze_ray_angles.forward.x) + ", " +
                      FormatDiagnosticDouble(gaze_ray_angles.forward.y) + ", " +
                      FormatDiagnosticDouble(gaze_ray_angles.forward.z) + ")" +
                      ", baseSpace=" + std::string(gaze_base_space == internal_view_space_ ? "view" : "app") +
                      ", locationFlags=" + FormatHex(static_cast<uint64_t>(gaze_location.locationFlags)) +
                      ", appSyncFresh=" + std::to_string(app_sync_fresh) +
                      ", heartbeat=" + std::to_string(should_log_eye_gaze_heartbeat));
        if (should_log_eye_gaze_burst && pending_eye_gaze_diagnostics_ > 0) {
            --pending_eye_gaze_diagnostics_;
        }
    }
    // Gaze recovered this frame; clear the unavailable streak so a future
    // dropout must persist past the debounce threshold again before it logs.
    eye_gaze_unavailable_streak_ = 0;
    if (!has_logged_eye_gaze_focus_active_) {
        logger_.Info("Quadviews eye-gaze focus active; using gaze-relative focus offsets. rawYaw=" +
                     FormatDiagnosticDouble(target_yaw) +
                     ", rawPitch=" + FormatDiagnosticDouble(target_pitch) +
                     ", smoothedYaw=" + FormatDiagnosticDouble(*yaw_radians) +
                     ", smoothedPitch=" + FormatDiagnosticDouble(*pitch_radians) +
                     ", baseSpace=" + std::string(gaze_base_space == internal_view_space_ ? "view" : "app"));
        has_logged_eye_gaze_focus_active_ = true;
        has_logged_eye_gaze_focus_unavailable_ = false;
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
        pivot_diagnostic_.has_eye_offsets = false;
        return false;
    }

    auto store_eye_offset_diagnostics = [&]() {
        pivot_diagnostic_.has_eye_offsets = true;
        pivot_diagnostic_.eye_offsets_time = display_time;
        pivot_diagnostic_.eye_offsets_view_configuration = view_configuration_type;
        pivot_diagnostic_.eye_offset_count = std::min<uint32_t>(view_count, static_cast<uint32_t>(pivot_diagnostic_.eye_offsets.size()));
        for (uint32_t i = 0; i < pivot_diagnostic_.eye_offset_count; ++i) {
            pivot_diagnostic_.eye_offsets[i] = cached_eye_offset_poses_[i];
        }
    };

    // Eye offsets are predicted poses. Reuse them only for repeated locates at
    // the same displayTime so recomposition does not inherit stale prediction.
    if (cached_eye_offset_poses_.size() == view_count &&
        cached_eye_offsets_view_configuration_ == view_configuration_type &&
        cached_eye_offsets_display_time_ != 0 &&
        cached_eye_offsets_display_time_ == display_time) {
        store_eye_offset_diagnostics();
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
        pivot_diagnostic_.has_eye_offsets = false;
        return false;
    }

    cached_eye_offset_poses_.resize(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        cached_eye_offset_poses_[i] = eye_views[i].pose;
    }
    cached_eye_offsets_view_configuration_ = view_configuration_type;
    cached_eye_offsets_display_time_ = display_time;
    store_eye_offset_diagnostics();
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
    bool varjo_compatible = false;
    QuadViewsResolvedSettings quadviews_settings;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        quadviews_active = IsQuadViewsActive();
        varjo_compatible = varjo_compatible_quadviews_active_;
        quadviews_settings = resolved_settings_.quadviews;
    }

    // Varjo compatible mode: forward the app's quad locate untouched — including the
    // Varjo foveated-rendering next chain, which the runtime consumes to place the
    // focus inset by gaze — and return the runtime's real 4 views. No stereo remap,
    // no synthesis. Pivot is still applied downstream in LocateViews.
    if (varjo_compatible && view_locate_info && quadviews_active &&
        IsQuadViewConfiguration(view_locate_info->viewConfigurationType)) {
        return next_locate_views_(
            session, view_locate_info, view_state, view_capacity_input, view_count_output, views);
    }

    XrViewLocateInfo downstream_locate_info{};
    const XrViewLocateInfo* downstream_view_locate_info = view_locate_info;
    if (view_locate_info) {
        const void* stripped_next = StripVarjoFoveatedViewLocateNextChain(view_locate_info->next);
        if (stripped_next != view_locate_info->next) {
            downstream_locate_info = *view_locate_info;
            downstream_locate_info.next = stripped_next;
            downstream_view_locate_info = &downstream_locate_info;
        }
    }

    if (!view_locate_info || !IsQuadViewConfiguration(view_locate_info->viewConfigurationType) || !quadviews_active) {
        const bool diag = TurboSequencedDebugTick();
        if (diag) {
            logger_.Debug("Turbo-diag: app xrLocateViews starting (no lock held).");
        }
        const XrResult locate_result = next_locate_views_(
            session, downstream_view_locate_info, view_state, view_capacity_input, view_count_output, views);
        if (diag) {
            logger_.Debug("Turbo-diag: app xrLocateViews completed.");
        }
        return locate_result;
    }

    XrViewLocateInfo runtime_locate_info = *downstream_view_locate_info;
    runtime_locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    std::array<XrView, 2> stereo_views{};
    for (XrView& view : stereo_views) {
        view = {XR_TYPE_VIEW};
    }
    XrViewState runtime_view_state{XR_TYPE_VIEW_STATE};
    XrViewState* downstream_view_state = view_state ? &runtime_view_state : nullptr;
    uint32_t stereo_count = 0;
    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: synthesized-quadviews xrLocateViews starting (no lock held).");
    }
    const XrResult result = next_locate_views_(session,
                                              &runtime_locate_info,
                                              downstream_view_state,
                                              static_cast<uint32_t>(stereo_views.size()),
                                              &stereo_count,
                                              stereo_views.data());
    if (diag) {
        logger_.Debug("Turbo-diag: synthesized-quadviews xrLocateViews completed.");
    }
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
                                                            ClampInternalLocateTime(view_locate_info->displayTime),
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
    if (XR_FAILED(result)) {
        if (applied_pose_delta) {
            *applied_pose_delta = IdentityPose();
        }
        pivotxr_activation_gain_ = 0.0;
        if (applied_extra_yaw_radians) {
            *applied_extra_yaw_radians = 0.0;
        }
        if (applied_extra_pitch_radians) {
            *applied_extra_pitch_radians = 0.0;
        }
        return result;
    }

    return ApplyPivotToLocatedSpace(space, base_space, time, pivotxr_active, location,
                                    applied_extra_yaw_radians, applied_extra_pitch_radians,
                                    applied_pose_delta, update_smoothing);
}

// Post-locate pivot application: everything LocateSpaceWithPivot does after
// the runtime locate. Split out so callers that must not hold mutex_ across
// the (potentially blocking) runtime locate can run it separately.
XrResult OpenXrLayer::ApplyPivotToLocatedSpace(XrSpace space,
                                               XrSpace base_space,
                                               XrTime time,
                                               bool pivotxr_active,
                                               XrSpaceLocation* location,
                                               double* applied_extra_yaw_radians,
                                               double* applied_extra_pitch_radians,
                                               XrPosef* applied_pose_delta,
                                               bool update_smoothing) {
    const XrResult result = XR_SUCCESS;
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

    if (!resolved_settings_.pivotxr.enabled) {
        pivotxr_activation_gain_ = 0.0;
        clear_extra_outputs();
        return result;
    }

    // During the release ramp this is the last-engaged profile, so the easing
    // keeps using the settings the pivot was engaged with.
    const PivotXrResolvedProfile& settings = ActivePivotProfile();

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
        const double ramp_seconds = settings.activation_ramp_seconds;
        if (ramp_seconds <= 0.0) {
            pivotxr_activation_gain_ = target_gain;
        } else if (delta_seconds > 0.0) {
            const double step = delta_seconds / ramp_seconds;
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
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
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

    // Measure head angles against the captured origin when one is set; the
    // default is the reference-space forward (HMD origin).
    double current_yaw_radians = ExtractYawRadians(orientation);
    double current_pitch_radians = ExtractPitchRadians(orientation);
    if (pivotxr_origin_.has_value()) {
        current_yaw_radians = WrapRadians(current_yaw_radians - pivotxr_origin_->yaw_radians);
        current_pitch_radians = current_pitch_radians - pivotxr_origin_->pitch_radians;
    }

    // Track the steady-state pivot amount only while actively engaged. During
    // the release ramp it is frozen and the envelope eases the applied angle to
    // zero, avoiding any snap on toggle-off.
    if (update_smoothing && pivotxr_active) {
        const int previous_yaw_step = pivotxr_yaw_step_;
        const int previous_pitch_step = pivotxr_pitch_step_;
        if (settings.response_mode == PivotResponseMode::Stepped) {
            ComputePivotSteppedExtraAngleRadians(current_yaw_radians,
                                                 settings.yaw_deadzone_degrees,
                                                 settings.step_trigger_degrees,
                                                 settings.step_amount_degrees,
                                                 settings.step_hysteresis_degrees,
                                                 settings.yaw_max_extra_degrees,
                                                 settings.smoothing,
                                                 delta_seconds,
                                                 pivotxr_yaw_step_,
                                                 pivotxr_smoothed_extra_yaw_radians_);
            ComputePivotSteppedExtraAngleRadians(current_pitch_radians,
                                                 settings.pitch_deadzone_degrees,
                                                 settings.step_trigger_degrees,
                                                 settings.step_amount_degrees,
                                                 settings.step_hysteresis_degrees,
                                                 settings.pitch_max_extra_degrees,
                                                 settings.smoothing,
                                                 delta_seconds,
                                                 pivotxr_pitch_step_,
                                                 pivotxr_smoothed_extra_pitch_radians_);
        } else {
            const PivotAxisTuning& yaw_tuning =
                current_yaw_radians >= 0.0 ? settings.yaw_positive : settings.yaw_negative;
            const PivotAxisTuning& pitch_tuning =
                current_pitch_radians >= 0.0 ? settings.pitch_positive : settings.pitch_negative;
            ComputePivotExtraAngleRadians(current_yaw_radians,
                                          yaw_tuning.rotation_multiplier,
                                          yaw_tuning.deadzone_degrees,
                                          yaw_tuning.max_extra_degrees,
                                          settings.smoothing,
                                          delta_seconds,
                                          pivotxr_smoothed_extra_yaw_radians_);
            ComputePivotExtraAngleRadians(current_pitch_radians,
                                          pitch_tuning.rotation_multiplier,
                                          pitch_tuning.deadzone_degrees,
                                          pitch_tuning.max_extra_degrees,
                                          settings.smoothing,
                                          delta_seconds,
                                          pivotxr_smoothed_extra_pitch_radians_);
        }

        if ((pivotxr_yaw_step_ != previous_yaw_step || pivotxr_pitch_step_ != previous_pitch_step) &&
            logger_.IsDebugEnabled()) {
            std::ostringstream step_stream;
            step_stream << "PivotXR step change: yawStep=" << previous_yaw_step << "->" << pivotxr_yaw_step_
                        << ", pitchStep=" << previous_pitch_step << "->" << pivotxr_pitch_step_
                        << ", rawYaw=" << FormatDiagnosticDouble(current_yaw_radians)
                        << ", rawPitch=" << FormatDiagnosticDouble(current_pitch_radians);
            logger_.Debug(step_stream.str());
        }
    }

    const double eased_gain = SmoothStep(pivotxr_activation_gain_);
    const double extra_yaw_radians = pivotxr_smoothed_extra_yaw_radians_ * eased_gain;
    const double extra_pitch_radians = pivotxr_smoothed_extra_pitch_radians_ * eased_gain;
    if (update_smoothing) {
        pivot_diagnostic_.has_view_pose = true;
        pivot_diagnostic_.view_time = time;
        pivot_diagnostic_.view_location_flags = location->locationFlags;
        pivot_diagnostic_.pivot_active = pivotxr_active;
        pivot_diagnostic_.space_is_view = space_is_view;
        pivot_diagnostic_.base_space_is_view = base_space_is_view;
        pivot_diagnostic_.raw_yaw_radians = current_yaw_radians;
        pivot_diagnostic_.raw_pitch_radians = current_pitch_radians;
        pivot_diagnostic_.steady_extra_yaw_radians = pivotxr_smoothed_extra_yaw_radians_;
        pivot_diagnostic_.steady_extra_pitch_radians = pivotxr_smoothed_extra_pitch_radians_;
        pivot_diagnostic_.eased_extra_yaw_radians = extra_yaw_radians;
        pivot_diagnostic_.eased_extra_pitch_radians = extra_pitch_radians;
        pivot_diagnostic_.activation_gain = pivotxr_activation_gain_;
        pivot_diagnostic_.origin_active = pivotxr_origin_.has_value();
        pivot_diagnostic_.origin_yaw_radians = pivotxr_origin_ ? pivotxr_origin_->yaw_radians : 0.0;
        pivot_diagnostic_.origin_pitch_radians = pivotxr_origin_ ? pivotxr_origin_->pitch_radians : 0.0;
        pivot_diagnostic_.yaw_step = pivotxr_yaw_step_;
        pivot_diagnostic_.pitch_step = pivotxr_pitch_step_;
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
           << ", stereoBoost=" << settings.depthxr.stereo_boost
           << ", convergence=" << settings.depthxr.convergence
           << ", pivotxrEnabled=" << settings.pivotxr.enabled
           << ", pivotProfileCount=" << settings.pivotxr.profiles.size();
    for (size_t i = 0; i < settings.pivotxr.profiles.size(); ++i) {
        const PivotXrResolvedProfile& profile = settings.pivotxr.profiles[i];
        stream << ", pivotProfile[" << i << "]={name=" << profile.name
               << ", activation=" << ToString(profile.activation_mode)
               << ", binding=" << BindingLabel(profile.activation_binding)
               << ", setOriginBinding=" << BindingLabel(profile.set_origin_binding)
               << ", releaseOriginBinding=" << BindingLabel(profile.release_origin_binding)
               << ", smoothing=" << profile.smoothing
               << ", activationRamp=" << profile.activation_ramp_seconds
               << ", yawMultiplier=" << profile.yaw_rotation_multiplier
               << ", yawDeadzone=" << profile.yaw_deadzone_degrees
               << ", yawMaxExtra=" << profile.yaw_max_extra_degrees
               << ", pitchMultiplier=" << profile.pitch_rotation_multiplier
               << ", pitchDeadzone=" << profile.pitch_deadzone_degrees
               << ", pitchMaxExtra=" << profile.pitch_max_extra_degrees
               << ", responseMode=" << ToString(profile.response_mode);
        if (profile.response_mode == PivotResponseMode::Stepped) {
            stream << ", stepTrigger=" << profile.step_trigger_degrees
                   << ", stepAmount=" << profile.step_amount_degrees
                   << ", stepHysteresis=" << profile.step_hysteresis_degrees;
        }
        stream << ", yawLeftMultiplier=" << profile.yaw_positive.rotation_multiplier
               << ", yawRightMultiplier=" << profile.yaw_negative.rotation_multiplier
               << ", pitchUpMultiplier=" << profile.pitch_positive.rotation_multiplier
               << ", pitchDownMultiplier=" << profile.pitch_negative.rotation_multiplier << "}";
    }
    stream << ", turboEnabled=" << settings.turbo.enabled
           << ", turboToggleBinding=" << BindingLabel(settings.turbo.toggle_binding)
           << ", turboPacingMode=" << ToString(settings.turbo.pacing_mode)
           << ", turboRuntimePins=" << settings.turbo.runtime_pins.size()
           << ", turboMetricsMode=" << ToString(settings.turbo.metrics_mode)
           << ", turboMetricsBinding=" << BindingLabel(settings.turbo.metrics_binding)
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
    pivotxr_yaw_step_ = 0;
    pivotxr_pitch_step_ = 0;
    pivotxr_activation_gain_ = 0.0;
    pivotxr_last_smoothing_wall_time_.reset();
    pivotxr_engaged_ = false;
    pivotxr_active_profile_index_ = 0;
    pivotxr_profile_input_states_.clear();
    pivotxr_origin_.reset();
    pivotxr_origin_capture_pending_ = false;
    pivotxr_binding_last_poll_time_.reset();
    pivot_diagnostic_stride_counter_ = 0;
    pivot_diagnostic_ = PivotDiagnosticState{};
}

void OpenXrLayer::ResetDepthToggleState() {
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    depthxr_binding_last_poll_time_.reset();
    depthxr_binding_down_cached_ = false;
}

void OpenXrLayer::ResetTurboToggleState() {
    turbo_toggle_enabled_ = true;
    turbo_toggle_binding_was_down_ = false;
    turbo_binding_last_poll_time_.reset();
    turbo_binding_down_cached_ = false;
}

bool OpenXrLayer::IsTurboActive() {
    if (!resolved_settings_.core.enabled || !resolved_settings_.turbo.enabled) {
        ResetTurboToggleState();
        return false;
    }

    // Note on Varjo: QVF denies turbo when its deferred-release quirk is
    // active, but that denial exists because QVF flushes deferred releases at
    // the app's *next acquire* — turbo's pipelining lets the app acquire (and
    // overwrite) an image QVF's EndFrame is still sampling. VectorXR flushes
    // its Varjo-deferred releases synchronously inside EndFrame, before the
    // (possibly turbo-deferred) begin/end forward, so the app cannot run
    // ahead of a pending release and the ordering the deferral protects is
    // preserved. OpenXR Toolkit likewise ships turbo with no Varjo guard.
    // Log once for diagnosability when both paths are live.
    if (defer_quadviews_swapchain_releases_ && !has_logged_turbo_varjo_note_) {
        logger_.Info("Turbo mode active on a Varjo runtime; deferred quadviews swapchain releases "
                     "remain EndFrame-synchronous, so frame ordering is preserved.");
        has_logged_turbo_varjo_note_ = true;
    }

#if defined(_WIN32)
    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detector on the first poll so a button held from the bind
    // gesture does not register as a press.
    const bool first_poll = !turbo_binding_last_poll_time_.has_value();
    if (first_poll || now - *turbo_binding_last_poll_time_ >= kInputBindingPollInterval) {
        turbo_binding_last_poll_time_ = now;
        turbo_binding_down_cached_ = IsInputBindingDown(resolved_settings_.turbo.toggle_binding);
    }
    const bool binding_down = turbo_binding_down_cached_;
    if (first_poll) {
        turbo_toggle_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !turbo_toggle_binding_was_down_;
    turbo_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        if (turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            // A toggle press after an auto-suspend re-arms turbo rather than
            // flipping the enable state.
            turbo_auto_suspended_.store(false, std::memory_order_relaxed);
            turbo_drain_timeout_count_ = 0;
            turbo_timeout_window_start_.reset();
            turbo_stable_accumulated_ms_ = 0.0;
            // Under Auto, a retry that then runs clean should overwrite the
            // verdict that suspended us (e.g. a stale "unsupported").
            if (turbo_pacing_source_ != TurboPacingSource::kForced &&
                turbo_pacing_source_ != TurboPacingSource::kPinned) {
                turbo_pacing_verdict_pending_ = true;
            }
            turbo_toggle_enabled_ = true;
            logger_.Info("Turbo mode re-armed after auto-suspend via " +
                         BindingLabel(resolved_settings_.turbo.toggle_binding) + ".");
        } else {
            turbo_toggle_enabled_ = !turbo_toggle_enabled_;
            logger_.Info(std::string("Turbo mode ") + (turbo_toggle_enabled_ ? "enabled" : "disabled") +
                         " via " + BindingLabel(resolved_settings_.turbo.toggle_binding) + ".");
        }
        SoundPlayer::Instance().PlayTransition(resolved_settings_.turbo.toggle_binding.sound,
                                               turbo_toggle_enabled_, dll_directory_,
                                               resolved_settings_.core.sound_volume,
                                               L"turbo-on.wav", L"turbo-off.wav");
    }

    return turbo_toggle_enabled_ && !turbo_auto_suspended_.load(std::memory_order_relaxed);
#else
    return !turbo_auto_suspended_.load(std::memory_order_relaxed);
#endif
}

void OpenXrLayer::ResetSwapchainState() {
    for (auto& [swapchain, info] : tracked_swapchains_) {
        SafeReleaseVector(info.d3d11_shader_resources);
    }
    tracked_swapchains_.clear();
}

bool OpenXrLayer::ShouldDeferSwapchainRelease(const SwapchainInfo& info) const {
    // Deliberately scoped to every swapchain in the quadviews session, not just the
    // four compositor inputs. The extra swapchains are still flushed before
    // next_end_frame_, so submission ordering stays correct; keying off the session
    // avoids tracking which handles are compositor inputs versus app-owned layers.
    return defer_quadviews_swapchain_releases_ && info.quadviews_session && info.session == active_session_ &&
           IsQuadViewsActive();
}

XrResult OpenXrLayer::FlushDeferredSwapchainReleaseLocked(XrSwapchain swapchain,
                                                          SwapchainInfo& info,
                                                          std::string_view reason) {
    if (!info.release_deferred) {
        return XR_SUCCESS;
    }
    if (!next_release_swapchain_image_) {
        logger_.Error("Deferred swapchain release failed: xrReleaseSwapchainImage is unavailable, reason=" +
                      std::string(reason));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult result = next_release_swapchain_image_(swapchain, &release_info);
    if (XR_FAILED(result)) {
        logger_.Error("Deferred swapchain release failed: handle=" + FormatHandle(swapchain) +
                      ", reason=" + std::string(reason) +
                      ", result=" + FormatHex(static_cast<uint64_t>(result)));
        return result;
    }

    info.release_deferred = false;
    if (info.quadviews_session && info.release_count <= 3) {
        LogSwapchainSummary(swapchain, info, "deferredReleaseFlushed");
    }
    return XR_SUCCESS;
}

XrResult OpenXrLayer::FlushDeferredSwapchainReleasesLocked(std::string_view reason) {
    XrResult first_failure = XR_SUCCESS;
    for (auto& [swapchain, info] : tracked_swapchains_) {
        const XrResult result = FlushDeferredSwapchainReleaseLocked(swapchain, info, reason);
        if (XR_FAILED(result) && XR_SUCCEEDED(first_failure)) {
            first_failure = result;
        }
    }
    return first_failure;
}

void OpenXrLayer::ResetD3D11FocusSharpen() {
    for (QuadViewsCompositionTarget& target : d3d11_focus_sharpen_.targets) {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            next_destroy_swapchain_(target.swapchain);
        }
        target = {};
    }
    SafeRelease(d3d11_focus_sharpen_.constants);
    SafeRelease(d3d11_focus_sharpen_.sampler);
    SafeRelease(d3d11_focus_sharpen_.pixel_shader);
    SafeRelease(d3d11_focus_sharpen_.vertex_shader);
    d3d11_focus_sharpen_.initialized = false;
    d3d11_focus_sharpen_.failed = false;
    d3d11_focus_sharpen_.has_logged_active = false;
    d3d11_focus_sharpen_.has_logged_skipped = false;
    d3d11_focus_sharpen_.consecutive_skipped_frames = 0;
    d3d11_focus_sharpen_.failure_logs_remaining = 8;
}

bool OpenXrLayer::EnsureD3D11FocusSharpen() {
    if (d3d11_focus_sharpen_.failed) {
        return false;
    }
    if (d3d11_focus_sharpen_.initialized) {
        return true;
    }
    ID3D11Device* device = d3d11_quadviews_compositor_.device;
    if (!device || !d3d11_quadviews_compositor_.context) {
        return false;
    }

    const char* source = D3D11FocusSharpenShaderSource();
    ID3DBlob* vertex_blob = nullptr;
    ID3DBlob* pixel_blob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertex_blob, &errors);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen vertex shader compile failed.");
        SafeRelease(errors);
        d3d11_focus_sharpen_.failed = true;
        return false;
    }
    SafeRelease(errors);
    hr = D3DCompile(
        source, std::strlen(source), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pixel_blob, &errors);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen pixel shader compile failed.");
        SafeRelease(vertex_blob);
        SafeRelease(errors);
        d3d11_focus_sharpen_.failed = true;
        return false;
    }
    SafeRelease(errors);

    hr = device->CreateVertexShader(
        vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr, &d3d11_focus_sharpen_.vertex_shader);
    SafeRelease(vertex_blob);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen vertex shader creation failed.");
        SafeRelease(pixel_blob);
        d3d11_focus_sharpen_.failed = true;
        return false;
    }
    hr = device->CreatePixelShader(
        pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr, &d3d11_focus_sharpen_.pixel_shader);
    SafeRelease(pixel_blob);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen pixel shader creation failed.");
        d3d11_focus_sharpen_.failed = true;
        return false;
    }

    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler_desc, &d3d11_focus_sharpen_.sampler);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen sampler creation failed.");
        d3d11_focus_sharpen_.failed = true;
        return false;
    }

    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(FocusSharpenConstants);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(&buffer_desc, nullptr, &d3d11_focus_sharpen_.constants);
    if (FAILED(hr)) {
        logger_.Error("D3D11 focus sharpen constant buffer creation failed.");
        d3d11_focus_sharpen_.failed = true;
        return false;
    }

    d3d11_focus_sharpen_.initialized = true;
    return true;
}

bool OpenXrLayer::EnsureFocusSharpenTarget(QuadViewsCompositionTarget& target,
                                           uint32_t width,
                                           uint32_t height,
                                           int64_t format) {
    if (width == 0 || height == 0 || !next_create_swapchain_ || !next_enumerate_swapchain_images_) {
        return false;
    }
    if (target.swapchain != XR_NULL_HANDLE && target.width == width && target.height == height &&
        target.format == format && !target.image_render_target_views.empty()) {
        return true;
    }

    // Recreate on any size/format change.
    SafeReleaseVector(target.image_render_target_views);
    SafeRelease(target.render_target_view);
    SafeRelease(target.render_texture);
    if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
        next_destroy_swapchain_(target.swapchain);
    }
    target = {};

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    create_info.format = format;
    create_info.sampleCount = 1;
    create_info.width = width;
    create_info.height = height;
    create_info.faceCount = 1;
    create_info.arraySize = 1;
    create_info.mipCount = 1;
    XrResult result = next_create_swapchain_(active_session_, &create_info, &target.swapchain);
    if (XR_FAILED(result) || target.swapchain == XR_NULL_HANDLE) {
        logger_.Error("D3D11 focus sharpen output swapchain creation failed: result=" +
                      FormatHex(static_cast<uint64_t>(result)));
        target = {};
        return false;
    }

    uint32_t image_count = 0;
    result = next_enumerate_swapchain_images_(target.swapchain, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        logger_.Error("D3D11 focus sharpen output image count query failed.");
        return false;
    }
    std::vector<XrSwapchainImageD3D11KHR> images(image_count);
    for (XrSwapchainImageD3D11KHR& image : images) {
        image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    }
    result = next_enumerate_swapchain_images_(
        target.swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        logger_.Error("D3D11 focus sharpen output image enumeration failed.");
        return false;
    }

    target.width = width;
    target.height = height;
    target.format = format;
    target.image_count = image_count;
    target.d3d11_images.reserve(image_count);
    for (const XrSwapchainImageD3D11KHR& image : images) {
        target.d3d11_images.push_back(image.texture);
    }
    target.image_render_target_views.reserve(target.d3d11_images.size());
    for (ID3D11Texture2D* texture : target.d3d11_images) {
        ID3D11RenderTargetView* rtv = nullptr;
        HRESULT hr = CreateTextureRenderTargetView(d3d11_quadviews_compositor_.device, texture, format, &rtv);
        if (FAILED(hr)) {
            logger_.Error("D3D11 focus sharpen output RTV creation failed: hr=" +
                          FormatHex(static_cast<uint32_t>(hr)));
            SafeReleaseVector(target.image_render_target_views);
            return false;
        }
        target.image_render_target_views.push_back(rtv);
    }
    return true;
}

void OpenXrLayer::SharpenNativeFocusViews(std::vector<XrCompositionLayerProjectionView>& views,
                                          XrTime display_time) {
    const double sharpen_amount = Clamp(resolved_settings_.quadviews.foveate_sharpness, 0.0, 100.0) / 100.0;
    if (sharpen_amount <= 0.001 || views.size() < 4) {
        return;
    }
    if (!EnsureD3D11FocusSharpen()) {
        if (!d3d11_focus_sharpen_.has_logged_skipped) {
            logger_.Info(std::string("Varjo focus sharpen requested (amount=") +
                         FormatDiagnosticDouble(sharpen_amount) +
                         ") but the D3D11 sharpen pass is unavailable: " +
                         (d3d11_focus_sharpen_.failed
                              ? "shader/resource initialization failed (see earlier errors)."
                              : "no D3D11 graphics binding for this session."));
            d3d11_focus_sharpen_.has_logged_skipped = true;
        }
        return;
    }

    ID3D11DeviceContext* context = d3d11_quadviews_compositor_.context;

    // Save the app's context state so the sharpen draw leaves it untouched.
    struct SavedD3D11State {
        ID3D11RenderTargetView* render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth_stencil{nullptr};
        ID3D11VertexShader* vertex_shader{nullptr};
        ID3D11PixelShader* pixel_shader{nullptr};
        ID3D11InputLayout* input_layout{nullptr};
        ID3D11ShaderResourceView* shader_resources[1]{};
        ID3D11SamplerState* samplers[1]{};
        ID3D11Buffer* constant_buffers[1]{};
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewport_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
        D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
    } saved;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, &saved.depth_stencil);
    context->VSGetShader(&saved.vertex_shader, nullptr, nullptr);
    context->PSGetShader(&saved.pixel_shader, nullptr, nullptr);
    context->IAGetInputLayout(&saved.input_layout);
    context->IAGetPrimitiveTopology(&saved.topology);
    context->PSGetShaderResources(0, 1, saved.shader_resources);
    context->PSGetSamplers(0, 1, saved.samplers);
    context->PSGetConstantBuffers(0, 1, saved.constant_buffers);
    context->RSGetViewports(&saved.viewport_count, saved.viewports);

    uint32_t sharpened_count = 0;
    // Why each focus view was skipped, for the one-shot diagnostic below — the
    // pass otherwise degrades to a silent no-op that looks like a dead slider.
    const char* skip_reason[2] = {"ok", "ok"};
    for (uint32_t i = 2; i < 4; ++i) {
        const char*& reason = skip_reason[i - 2];
        XrCompositionLayerProjectionView& view = views[i];
        // Only single-slice focus swapchains are supported for the sharpen pass; an
        // arrayed subImage falls back to the app's unsharpened focus.
        if (view.subImage.imageArrayIndex != 0 || view.subImage.swapchain == XR_NULL_HANDLE) {
            reason = "arrayed or null subImage";
            continue;
        }
        const auto it = tracked_swapchains_.find(view.subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !(it->second.has_last_acquired_image_index || it->second.has_last_released_image_index)) {
            reason = "swapchain untracked or no acquired image yet";
            continue;
        }
        SwapchainInfo& src = it->second;
        // This frame's content: the last-released image (turbo pipelining can
        // advance last_acquired past it before EndFrame runs).
        const uint32_t src_index = src.has_last_released_image_index ? src.last_released_image_index
                                                                     : src.last_acquired_image_index;
        if (src_index >= src.d3d11_images.size()) {
            reason = "image index out of range";
            continue;
        }
        if (!EnsureD3D11SwapchainShaderResources(src) ||
            src_index >= src.d3d11_shader_resources.size()) {
            reason = "no shader resource views (focus swapchain needs sampled usage)";
            continue;
        }
        ID3D11ShaderResourceView* focus_srv = src.d3d11_shader_resources[src_index];
        if (!focus_srv) {
            reason = "null shader resource view";
            continue;
        }

        const uint32_t out_width = static_cast<uint32_t>(view.subImage.imageRect.extent.width);
        const uint32_t out_height = static_cast<uint32_t>(view.subImage.imageRect.extent.height);
        QuadViewsCompositionTarget& target = d3d11_focus_sharpen_.targets[i - 2];
        if (!EnsureFocusSharpenTarget(target, out_width, out_height, src.format)) {
            reason = "sharpen output swapchain unavailable";
            continue;
        }

        uint32_t output_index = 0;
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result = next_acquire_swapchain_image_(target.swapchain, &acquire_info, &output_index);
        if (XR_FAILED(result) || output_index >= target.image_render_target_views.size()) {
            reason = "sharpen output acquire failed";
            continue;
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        result = next_wait_swapchain_image_(target.swapchain, &wait_info);
        if (XR_FAILED(result)) {
            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next_release_swapchain_image_(target.swapchain, &release_info);
            reason = "sharpen output wait failed";
            continue;
        }

        FocusSharpenConstants constants{};
        constants.params[0] = static_cast<float>(sharpen_amount);
        constants.params[1] =
            kVarjoFocusSharpenRadiusTexels / static_cast<float>(std::max<uint32_t>(1, src.width));
        constants.params[2] =
            kVarjoFocusSharpenRadiusTexels / static_cast<float>(std::max<uint32_t>(1, src.height));
        constants.params[3] = 0.0f;
        constants.src_rect[0] =
            static_cast<float>(view.subImage.imageRect.offset.x) / static_cast<float>(std::max<uint32_t>(1, src.width));
        constants.src_rect[1] =
            static_cast<float>(view.subImage.imageRect.offset.y) / static_cast<float>(std::max<uint32_t>(1, src.height));
        constants.src_rect[2] =
            static_cast<float>(out_width) / static_cast<float>(std::max<uint32_t>(1, src.width));
        constants.src_rect[3] =
            static_cast<float>(out_height) / static_cast<float>(std::max<uint32_t>(1, src.height));

        ID3D11RenderTargetView* render_target = target.image_render_target_views[output_index];
        const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(render_target, clear_color);
        context->OMSetRenderTargets(1, &render_target, nullptr);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(out_width);
        viewport.Height = static_cast<float>(out_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(d3d11_focus_sharpen_.vertex_shader, nullptr, 0);
        context->PSSetShader(d3d11_focus_sharpen_.pixel_shader, nullptr, 0);
        context->PSSetShaderResources(0, 1, &focus_srv);
        context->PSSetSamplers(0, 1, &d3d11_focus_sharpen_.sampler);
        context->UpdateSubresource(d3d11_focus_sharpen_.constants, 0, nullptr, &constants, 0, 0);
        context->PSSetConstantBuffers(0, 1, &d3d11_focus_sharpen_.constants);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* null_srv[1]{nullptr};
        context->PSSetShaderResources(0, 1, null_srv);
        ID3D11RenderTargetView* null_rtv = nullptr;
        context->OMSetRenderTargets(1, &null_rtv, nullptr);

        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        result = next_release_swapchain_image_(target.swapchain, &release_info);
        if (XR_FAILED(result)) {
            reason = "sharpen output release failed";
            continue;
        }

        // Repoint the submitted view at our sharpened swapchain.
        view.subImage.swapchain = target.swapchain;
        view.subImage.imageArrayIndex = 0;
        view.subImage.imageRect.offset = {0, 0};
        view.subImage.imageRect.extent = {static_cast<int32_t>(out_width), static_cast<int32_t>(out_height)};
        ++sharpened_count;
    }

    // Restore app context state.
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, saved.depth_stencil);
    context->VSSetShader(saved.vertex_shader, nullptr, 0);
    context->PSSetShader(saved.pixel_shader, nullptr, 0);
    context->IASetInputLayout(saved.input_layout);
    context->IASetPrimitiveTopology(saved.topology);
    context->PSSetShaderResources(0, 1, saved.shader_resources);
    context->PSSetSamplers(0, 1, saved.samplers);
    context->PSSetConstantBuffers(0, 1, saved.constant_buffers);
    context->RSSetViewports(saved.viewport_count, saved.viewports);
    for (ID3D11RenderTargetView*& rtv : saved.render_targets) {
        SafeRelease(rtv);
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

    if (sharpened_count > 0) {
        d3d11_focus_sharpen_.consecutive_skipped_frames = 0;
        if (!d3d11_focus_sharpen_.has_logged_active) {
            logger_.Info("D3D11 focus sharpen active in Varjo compatible quadviews: sharpened " +
                         std::to_string(sharpened_count) + " focus view(s), amount=" +
                         FormatDiagnosticDouble(sharpen_amount) + ", radiusTexels=" +
                         FormatDiagnosticDouble(kVarjoFocusSharpenRadiusTexels) +
                         " (frameTime=" + std::to_string(display_time) + ").");
            d3d11_focus_sharpen_.has_logged_active = true;
        }
    } else {
        ++d3d11_focus_sharpen_.consecutive_skipped_frames;
        if (!d3d11_focus_sharpen_.has_logged_skipped &&
            d3d11_focus_sharpen_.consecutive_skipped_frames >= kFocusSharpenSkipLogFrames) {
            logger_.Info(std::string("Varjo focus sharpen requested (amount=") +
                         FormatDiagnosticDouble(sharpen_amount) + ") but no focus view was sharpened for " +
                         std::to_string(d3d11_focus_sharpen_.consecutive_skipped_frames) +
                         " consecutive frames: view2=[" + skip_reason[0] + "], view3=[" + skip_reason[1] +
                         "] (frameTime=" + std::to_string(display_time) + ").");
            d3d11_focus_sharpen_.has_logged_skipped = true;
        }
    }
}

void OpenXrLayer::ResetD3D11QuadViewsCompositor() {
    ResetD3D11FocusSharpen();
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
    d3d11_quadviews_compositor_.has_last_completed_gpu_timing = false;
    d3d11_quadviews_compositor_.failure_logs_remaining = 8;
    d3d11_quadviews_compositor_.next_gpu_timing_query = 0;
    d3d11_quadviews_compositor_.last_completed_gpu_ms = 0.0;
    d3d11_quadviews_compositor_.last_completed_gpu_frame_time = 0;
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
           << ", releaseDeferred=" << info.release_deferred
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
        SoundPlayer::Instance().PlayTransition(resolved_settings_.depthxr_bindings.toggle_enabled.sound,
                                               depthxr_toggle_enabled_, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    }

    return depthxr_toggle_enabled_;
#else
    return resolved_settings_.depthxr.enabled;
#endif
}

const PivotXrResolvedProfile& OpenXrLayer::ActivePivotProfile() const {
    static const PivotXrResolvedProfile kDisabledPivotProfile{};
    const auto& profiles = resolved_settings_.pivotxr.profiles;
    if (profiles.empty()) {
        return kDisabledPivotProfile;
    }
    return profiles[std::min(pivotxr_active_profile_index_, profiles.size() - 1)];
}

bool OpenXrLayer::IsPivotXrActive() {
    const PivotXrResolvedSettings& pivot = resolved_settings_.pivotxr;
    if (!pivot.enabled || pivot.profiles.empty()) {
        ResetPivotActivationState();
        return false;
    }

#if defined(_WIN32)
    // RefreshResolvedSettings resets activation state whenever the candidate
    // set changes, so a size mismatch only happens right after such a reset.
    if (pivotxr_profile_input_states_.size() != pivot.profiles.size()) {
        pivotxr_profile_input_states_.assign(pivot.profiles.size(), PivotProfileInputState{});
        pivotxr_active_profile_index_ = std::min(pivotxr_active_profile_index_, pivot.profiles.size() - 1);
    }

    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detectors on the first poll after a reset so a button held
    // from the bind gesture does not register as a spurious activation press.
    const bool first_poll = !pivotxr_binding_last_poll_time_.has_value();
    if (first_poll || now - *pivotxr_binding_last_poll_time_ >= kInputBindingPollInterval) {
        pivotxr_binding_last_poll_time_ = now;
        for (size_t i = 0; i < pivot.profiles.size(); ++i) {
            PivotProfileInputState& input_state = pivotxr_profile_input_states_[i];
            input_state.down_cached = IsInputBindingDown(pivot.profiles[i].activation_binding);
            input_state.set_origin_down_cached = IsInputBindingDown(pivot.profiles[i].set_origin_binding);
            input_state.release_origin_down_cached =
                IsInputBindingDown(pivot.profiles[i].release_origin_binding);
        }
    }

    auto note_transition_diagnostics = [&]() {
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        pending_pivot_diagnostics_ = kPivotDiagnosticBurstCount;
    };
    auto engage = [&](size_t index) {
        const PivotXrResolvedProfile& profile = pivot.profiles[index];
        const bool switching = pivotxr_engaged_ && pivotxr_active_profile_index_ != index;
        pivotxr_active_profile_index_ = index;
        pivotxr_engaged_ = true;
        note_transition_diagnostics();
        logger_.Info("PivotXR profile '" + profile.name + "' engaged via " +
                     BindingLabel(profile.activation_binding) + (switching ? " (switched profile)." : "."));
        SoundPlayer::Instance().PlayTransition(profile.activation_binding.sound, true, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    auto disengage = [&](size_t index) {
        const PivotXrResolvedProfile& profile = pivot.profiles[index];
        pivotxr_engaged_ = false;
        note_transition_diagnostics();
        logger_.Info("PivotXR profile '" + profile.name + "' disengaged via " +
                     BindingLabel(profile.activation_binding) + ".");
        SoundPlayer::Instance().PlayTransition(profile.activation_binding.sound, false, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };

    // Arbitrate across all candidates: last pressed wins. Duplicate bindings
    // were pruned at resolve time, so at most one candidate sees a given edge.
    for (size_t i = 0; i < pivot.profiles.size(); ++i) {
        PivotProfileInputState& input_state = pivotxr_profile_input_states_[i];
        const bool binding_down = input_state.down_cached;
        if (first_poll) {
            input_state.was_down = binding_down;
            input_state.set_origin_was_down = input_state.set_origin_down_cached;
            input_state.release_origin_was_down = input_state.release_origin_down_cached;
            continue;
        }

        // Origin bindings act on press regardless of engagement, so the origin
        // can be aligned (alongside the game's own recenter) before pivot is
        // ever engaged. Capture is deferred to the xrLocateViews drive path,
        // which has the head pose for this frame.
        {
            const bool down = input_state.set_origin_down_cached;
            const bool pressed = down && !input_state.set_origin_was_down;
            input_state.set_origin_was_down = down;
            if (pressed) {
                pivotxr_origin_capture_pending_ = true;
                logger_.Info("PivotXR origin capture requested via " +
                             BindingLabel(pivot.profiles[i].set_origin_binding) + ".");
                SoundPlayer::Instance().PlayTransition(pivot.profiles[i].set_origin_binding.sound, true,
                                                       dll_directory_, resolved_settings_.core.sound_volume,
                                                       L"origin-set.wav", L"origin-set.wav");
            }
        }
        {
            const bool down = input_state.release_origin_down_cached;
            const bool pressed = down && !input_state.release_origin_was_down;
            input_state.release_origin_was_down = down;
            if (pressed && (pivotxr_origin_.has_value() || pivotxr_origin_capture_pending_)) {
                pivotxr_origin_.reset();
                pivotxr_origin_capture_pending_ = false;
                logger_.Info("PivotXR origin released via " +
                             BindingLabel(pivot.profiles[i].release_origin_binding) +
                             "; reverting to the HMD origin.");
                SoundPlayer::Instance().PlayTransition(pivot.profiles[i].release_origin_binding.sound, true,
                                                       dll_directory_, resolved_settings_.core.sound_volume,
                                                       L"origin-release.wav", L"origin-release.wav");
            }
        }

        const bool was_pressed = binding_down && !input_state.was_down;
        const bool was_released = !binding_down && input_state.was_down;
        input_state.was_down = binding_down;

        const bool is_engaged_profile = pivotxr_engaged_ && pivotxr_active_profile_index_ == i;
        if (pivot.profiles[i].activation_mode == ActivationMode::Toggle) {
            if (was_pressed) {
                if (is_engaged_profile) {
                    disengage(i);
                } else {
                    engage(i);
                }
            }
        } else if (pivot.profiles[i].activation_mode == ActivationMode::AlwaysOn) {
            // Always-on profiles engage automatically (below); their binding
            // suspends and resumes the automatic engagement.
            if (was_pressed) {
                if (is_engaged_profile) {
                    input_state.always_on_suspended = true;
                    logger_.Info("PivotXR always-on profile '" + pivot.profiles[i].name +
                                 "' suspended; it will not re-engage until resumed.");
                    disengage(i);
                } else {
                    input_state.always_on_suspended = false;
                    logger_.Info("PivotXR always-on profile '" + pivot.profiles[i].name + "' resumed.");
                    engage(i);
                }
            }
        } else {
            // Hold mode: down engages (taking over from any toggled profile),
            // release disengages only if this profile still owns the pivot.
            if (was_pressed) {
                engage(i);
            } else if (was_released && is_engaged_profile) {
                disengage(i);
            }
        }
    }

    // Automatic engagement: when nothing is engaged, the first non-suspended
    // always-on candidate takes over. Silent (no binding sound) because this is
    // not a user action — it fires at session start and whenever another
    // profile releases the pivot.
    if (!pivotxr_engaged_) {
        for (size_t i = 0; i < pivot.profiles.size(); ++i) {
            if (pivot.profiles[i].activation_mode != ActivationMode::AlwaysOn ||
                pivotxr_profile_input_states_[i].always_on_suspended) {
                continue;
            }
            pivotxr_active_profile_index_ = i;
            pivotxr_engaged_ = true;
            note_transition_diagnostics();
            logger_.Info("PivotXR profile '" + pivot.profiles[i].name + "' engaged (always on).");
            break;
        }
    }

    return pivotxr_engaged_;
#else
    return pivot.enabled;
#endif
}

} // namespace depthxr
