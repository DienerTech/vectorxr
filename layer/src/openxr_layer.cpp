#include "depthxr/openxr_layer.h"
#include "depthxr/pivot_routing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <openxr/openxr_platform.h>

#include "depthxr/config_path.h"
#include "depthxr/crt_failure_report.h"
#include "depthxr/effects.h"
#include "depthxr/framegen_plan.h"
#include "depthxr/input_devices.h"
#include "depthxr/process_exit.h"
#include "depthxr/process_info.h"
#include "depthxr/pivot_step.h"
#include "depthxr/pivot_view.h"
#include "depthxr/quadviews_compose.h"
#include "depthxr/quadviews_sizing.h"
#include "depthxr/runtime_compatibility.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/seen_apps.h"
#include "depthxr/session_graphics.h"
#include "depthxr/sound_player.h"
#include "depthxr/teardown_watch.h"
#include "depthxr/turbo_metrics.h"
#include "depthxr/vulkan_enable_hooks.h"

// Injected by layer/CMakeLists.txt from the canonical app version in
// app/src-tauri/tauri.conf.json. Falls back when the layer is built without that
// definition (e.g. the test target) so the symbol always resolves.
#ifndef VECTORXR_VERSION
#define VECTORXR_VERSION "unknown"
#endif

namespace depthxr {
namespace {

constexpr double kPi = 3.14159265358979323846;
// Composed Pivot pitch stops short of straight up or down.
constexpr double kPivotMaxPitchDegrees = 85.0;
// Position Assist eases toward its target with this time constant, so its
// offsets never step even when rotation smoothing is off.
constexpr double kPivotPositionAssistSmoothingSeconds = 0.12;

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

template <typename Position>
double PositionSeparationMeters(const Position& left, const Position& right) {
    const double x = left.x - right.x;
    const double y = left.y - right.y;
    const double z = left.z - right.z;
    return std::sqrt(x * x + y * y + z * z);
}

double ViewSeparationMeters(std::span<const ViewAdjustmentData> views) {
    if (views.size() < 2) {
        return 0.0;
    }

    return PositionSeparationMeters(views[0].position, views[1].position);
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

uint64_t RequestedScaledDimension(uint32_t value, double scale) {
    return std::max<uint64_t>(
        1, static_cast<uint64_t>(std::round(static_cast<double>(value) * std::max(0.01, scale))));
}

std::string_view VarjoNativeViewRole(uint32_t index) {
    switch (index) {
    case 0:
        return "peripheral-left";
    case 1:
        return "peripheral-right";
    case 2:
        return "focus-left";
    case 3:
        return "focus-right";
    default:
        return "unknown";
    }
}

double QuadViewsFocusWidthScale(const QuadViewsSettings& settings) {
    return settings.focus_scale * Clamp(settings.focus_horizontal_size_percent, 1.0, 100.0) / 100.0;
}

double QuadViewsFocusHeightScale(const QuadViewsSettings& settings) {
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
                     const QuadViewsSettings& settings,
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

std::string Utf8FromWide(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

// The application's D3D11 device as the D3D11 bridge needs to know it: its creation flags (SINGLETHREADED means
// no call into it from any thread but the game's), whether D3D11 serializes calls into it, and its adapter.
std::string DescribeD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context) {
    std::ostringstream stream;
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    stream << "featureLevel=" << ((level >> 12) & 0xF) << "_" << ((level >> 8) & 0xF);
    const UINT flags = device->GetCreationFlags();
    static constexpr std::pair<UINT, const char*> kFlagNames[] = {
        {D3D11_CREATE_DEVICE_SINGLETHREADED, "SINGLETHREADED"},
        {D3D11_CREATE_DEVICE_DEBUG, "DEBUG"},
        {D3D11_CREATE_DEVICE_SWITCH_TO_REF, "SWITCH_TO_REF"},
        {D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS, "PREVENT_INTERNAL_THREADING_OPTIMIZATIONS"},
        {D3D11_CREATE_DEVICE_BGRA_SUPPORT, "BGRA_SUPPORT"},
        {D3D11_CREATE_DEVICE_DEBUGGABLE, "DEBUGGABLE"},
        {D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY,
         "PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY"},
        {D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT, "DISABLE_GPU_TIMEOUT"},
        {D3D11_CREATE_DEVICE_VIDEO_SUPPORT, "VIDEO_SUPPORT"},
    };
    stream << ", creationFlags=0x" << std::hex << flags << std::dec << " (";
    UINT named = 0;
    for (const auto& [bit, name] : kFlagNames) {
        if ((flags & bit) != 0) {
            stream << (named ? "|" : "") << name;
            named |= bit;
        }
    }
    if ((flags & ~named) != 0) {
        stream << (named ? "|" : "") << "0x" << std::hex << (flags & ~named) << std::dec;
    }
    stream << (flags == 0 ? "none)" : ")");
    ID3D11Multithread* multithread = nullptr;
    if (context && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread),
                                                     reinterpret_cast<void**>(&multithread)))) {
        stream << ", multithreadProtected=" << (multithread->GetMultithreadProtected() ? "yes" : "no");
        multithread->Release();
    } else {
        stream << ", multithreadProtected=unknown";
    }
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        stream << ", adapter=\"" << Utf8FromWide(adapter_desc.Description) << "\" luid=0x" << std::hex
               << static_cast<uint32_t>(adapter_desc.AdapterLuid.HighPart) << ":0x"
               << adapter_desc.AdapterLuid.LowPart << std::dec;
    }
    SafeRelease(adapter);
    SafeRelease(dxgi_device);
    return stream.str();
}

// The calling thread's name, if the application set one (SetThreadDescription, Windows 10 1607 and later).
std::string CurrentThreadDescription() {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    PWSTR description = nullptr;
    if (!get_description || FAILED(get_description(GetCurrentThread(), &description)) || !description) {
        return {};
    }
    std::string name = Utf8FromWide(description);
    LocalFree(description);
    return name;
}

// The thread census is logged whole once the frame loop has settled, and again when the session ends.
constexpr std::uint64_t kThreadCensusSummaryFrame = 300;

struct QuadViewsPixelMetrics {
    uint64_t hash{1469598103934665603ull};
    double mean_luma{0.0};
    double standard_deviation{0.0};
    double mean_neighbor_edge{0.0};
    double near_black_fraction{0.0};
};

uint32_t PixelProbeBytesPerPixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return 8;
    default:
        return 0;
    }
}

double HalfToDouble(uint16_t value) {
    const double sign = (value & 0x8000u) != 0 ? -1.0 : 1.0;
    const uint32_t exponent = (value >> 10u) & 0x1fu;
    const uint32_t mantissa = value & 0x3ffu;
    if (exponent == 0) {
        return sign * std::ldexp(static_cast<double>(mantissa), -24);
    }
    if (exponent == 31) {
        return mantissa == 0 ? sign * HUGE_VAL : NAN;
    }
    return sign * std::ldexp(1.0 + static_cast<double>(mantissa) / 1024.0,
                             static_cast<int>(exponent) - 15);
}

bool DecodePixelProbeLuma(const uint8_t* pixel, DXGI_FORMAT format, double* luma) {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        red = pixel[0] / 255.0;
        green = pixel[1] / 255.0;
        blue = pixel[2] / 255.0;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        red = pixel[2] / 255.0;
        green = pixel[1] / 255.0;
        blue = pixel[0] / 255.0;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        uint32_t packed = 0;
        std::memcpy(&packed, pixel, sizeof(packed));
        red = (packed & 0x3ffu) / 1023.0;
        green = ((packed >> 10u) & 0x3ffu) / 1023.0;
        blue = ((packed >> 20u) & 0x3ffu) / 1023.0;
        break;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        std::array<uint16_t, 4> channels{};
        std::memcpy(channels.data(), pixel, sizeof(channels));
        red = HalfToDouble(channels[0]);
        green = HalfToDouble(channels[1]);
        blue = HalfToDouble(channels[2]);
        break;
    }
    case DXGI_FORMAT_R16G16B16A16_UNORM: {
        std::array<uint16_t, 4> channels{};
        std::memcpy(channels.data(), pixel, sizeof(channels));
        red = channels[0] / 65535.0;
        green = channels[1] / 65535.0;
        blue = channels[2] / 65535.0;
        break;
    }
    default:
        return false;
    }
    *luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
    return std::isfinite(*luma);
}

bool AnalyzePixelProbe(const D3D11_MAPPED_SUBRESOURCE& mapped,
                       DXGI_FORMAT format,
                       uint32_t width,
                       uint32_t height,
                       QuadViewsPixelMetrics* metrics) {
    const uint32_t bytes_per_pixel = PixelProbeBytesPerPixel(format);
    if (!mapped.pData || bytes_per_pixel == 0 || !metrics || width == 0 || height == 0 ||
        width * height > 1024) {
        return false;
    }

    std::array<double, 1024> luma{};
    double sum = 0.0;
    uint32_t near_black = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row + x * bytes_per_pixel;
            for (uint32_t byte = 0; byte < bytes_per_pixel; ++byte) {
                metrics->hash ^= pixel[byte];
                metrics->hash *= 1099511628211ull;
            }
            double value = 0.0;
            if (!DecodePixelProbeLuma(pixel, format, &value)) {
                return false;
            }
            luma[y * width + x] = value;
            sum += value;
            near_black += value <= (1.0 / 255.0) ? 1u : 0u;
        }
    }

    const double count = static_cast<double>(width * height);
    metrics->mean_luma = sum / count;
    metrics->near_black_fraction = near_black / count;
    double squared_delta_sum = 0.0;
    double edge_sum = 0.0;
    uint32_t edge_count = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const double value = luma[y * width + x];
            const double delta = value - metrics->mean_luma;
            squared_delta_sum += delta * delta;
            if (x > 0) {
                edge_sum += std::abs(value - luma[y * width + x - 1]);
                ++edge_count;
            }
            if (y > 0) {
                edge_sum += std::abs(value - luma[(y - 1) * width + x]);
                ++edge_count;
            }
        }
    }
    metrics->standard_deviation = std::sqrt(squared_delta_sum / count);
    metrics->mean_neighbor_edge = edge_count > 0 ? edge_sum / edge_count : 0.0;
    return true;
}

HRESULT CreateTextureShaderResourceView(ID3D11Device* device,
                                        ID3D11Texture2D* texture,
                                        int64_t fallback_format,
                                        uint32_t array_slice,
                                        ID3D11ShaderResourceView** shader_resource) {
    if (!device || !texture || !shader_resource) {
        return E_INVALIDARG;
    }

    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);
    if ((texture_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || texture_desc.MipLevels == 0 ||
        array_slice >= texture_desc.ArraySize) {
        return E_INVALIDARG;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = ResolveViewFormat(texture_desc.Format, fallback_format);
    if (texture_desc.SampleDesc.Count > 1 && texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
        view_desc.Texture2DMSArray.FirstArraySlice = array_slice;
        view_desc.Texture2DMSArray.ArraySize = 1;
    } else if (texture_desc.SampleDesc.Count > 1) {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else if (texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view_desc.Texture2DArray.MostDetailedMip = 0;
        view_desc.Texture2DArray.MipLevels = 1;
        view_desc.Texture2DArray.FirstArraySlice = array_slice;
        view_desc.Texture2DArray.ArraySize = 1;
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

// A frame without the depth of some swapchains (a bridged session's depth
// stays the game's, see D3D11Bridge), and the copies it needs.
struct DepthlessFrame {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<XrCompositionLayerProjection> projections;
    std::deque<std::vector<XrCompositionLayerProjectionView>> views;
};

// `frame` itself when no projection view's depth is to be dropped. A view's
// chain loses its depth structure when that comes first, otherwise the whole
// chain (another structure ahead of it cannot be copied without knowing it).
template <typename Drop>
const XrFrameEndInfo* WithoutDepth(const XrFrameEndInfo* frame, DepthlessFrame& storage, const Drop& drop) {
    const auto dropped = [&](const XrCompositionLayerProjectionView& view) {
        for (auto* base = static_cast<const XrBaseInStructure*>(view.next); base; base = base->next) {
            if (base->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR &&
                drop(reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(base)->subImage.swapchain)) {
                return true;
            }
        }
        return false;
    };
    const auto projection_at = [&](std::uint32_t i) -> const XrCompositionLayerProjection* {
        const XrCompositionLayerBaseHeader* base = frame->layers ? frame->layers[i] : nullptr;
        return base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
                   ? reinterpret_cast<const XrCompositionLayerProjection*>(base)
                   : nullptr;
    };
    const auto any_dropped = [&](const XrCompositionLayerProjection* projection) {
        return projection && projection->views &&
               std::any_of(projection->views, projection->views + projection->viewCount, dropped);
    };
    bool any = false;
    for (std::uint32_t i = 0; i < frame->layerCount && !any; ++i) {
        any = any_dropped(projection_at(i));
    }
    if (!any) {
        return frame;
    }
    storage.info = *frame;
    storage.layers.assign(frame->layers, frame->layers + frame->layerCount);
    for (std::uint32_t i = 0; i < frame->layerCount; ++i) {
        const XrCompositionLayerProjection* projection = projection_at(i);
        if (!any_dropped(projection)) {
            continue;
        }
        storage.views.emplace_back(projection->views, projection->views + projection->viewCount);
        for (XrCompositionLayerProjectionView& view : storage.views.back()) {
            if (dropped(view)) {
                const auto* first = static_cast<const XrBaseInStructure*>(view.next);
                view.next = first->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR ? first->next : nullptr;
            }
        }
        storage.projections.push_back(*projection);
        storage.projections.back().views = storage.views.back().data();
        storage.layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&storage.projections.back());
    }
    storage.info.layers = storage.layers.data();
    return &storage.info;
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
    float peripheral_src_rect[4];
    float focus_src_rect[4];
    float diagnostic_params[4];
    float output_texel[4];
    float gaze_markers[4];
    float reference_markers[4];
};

FrameGenFov ToFrameGenFov(const XrFovf& fov) {
    return {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
}

// The compositor's constants for one eye of a quad views compose plan
// (BuildFrameGenComposeEye: one builder for this compositor and Frame
// Generation's quad views compose), the diagnostics its overlay's.
FocusRectConstants FocusRectConstantsFor(const FrameGenComposeEye& eye) {
    FocusRectConstants constants{};
    std::copy(eye.focus_rect.begin(), eye.focus_rect.end(), constants.focus_rect);
    // Feathering is measured in output pixels, while sharpening samples the focus texture.
    constants.blend_params[0] = eye.feather[0];
    constants.blend_params[1] = eye.feather[1];
    constants.blend_params[2] = eye.sharpen;
    constants.blend_params[3] = 0.0f;
    constants.focus_texel[0] = eye.focus_texel[0];
    constants.focus_texel[1] = eye.focus_texel[1];
    // One output pixel expressed in focus-UV, so inline sharpening samples at output-pixel
    // spacing (frequencies that survive the focus->output resample) instead of focus-texel scale.
    constants.focus_texel[2] = eye.output_pixel_in_focus[0];
    constants.focus_texel[3] = eye.output_pixel_in_focus[1];
    constants.peripheral_src_rect[0] = 0.0f;
    constants.peripheral_src_rect[1] = 0.0f;
    constants.peripheral_src_rect[2] = 1.0f;
    constants.peripheral_src_rect[3] = 1.0f;
    constants.focus_src_rect[0] = 0.0f;
    constants.focus_src_rect[1] = 0.0f;
    constants.focus_src_rect[2] = 1.0f;
    constants.focus_src_rect[3] = 1.0f;
    if (eye.overlay.shown) {
        constants.diagnostic_params[0] = 1.0f;
        constants.diagnostic_params[1] = eye.overlay.eye_tracking ? 1.0f : 0.0f;
        constants.diagnostic_params[2] = eye.overlay.gaze ? 1.0f : 0.0f;
        std::copy(eye.overlay.gaze_markers.begin(), eye.overlay.gaze_markers.end(), constants.gaze_markers);
        std::copy(eye.overlay.reference_markers.begin(), eye.overlay.reference_markers.end(),
                  constants.reference_markers);
    }
    return constants;
}

std::array<float, 2> ProjectViewAnglesToUv(const XrFovf& full_fov,
                                           double yaw_radians,
                                           double pitch_radians) {
    const double left = std::tan(full_fov.angleLeft);
    const double right = std::tan(full_fov.angleRight);
    const double down = std::tan(full_fov.angleDown);
    const double up = std::tan(full_fov.angleUp);
    const double width = std::max(0.0001, right - left);
    const double height = std::max(0.0001, up - down);
    return {
        static_cast<float>((std::tan(yaw_radians) - left) / width),
        static_cast<float>((up - std::tan(pitch_radians)) / height),
    };
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
// compositor path samples at output-pixel spacing; see QuadViewsD3D11ShaderSource). A
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
        // Preserve extrema instead of clamping them back to the unmodified center sample.
        // Limit the added detail to half the local contrast range so the slider remains
        // visibly effective without allowing runaway halos.
        float3 lo = min(focus.rgb, min(min(n, s), min(e, w)));
        float3 hi = max(focus.rgb, max(max(n, s), max(e, w)));
        float3 localRange = max(hi - lo, float3(1.0 / 255.0, 1.0 / 255.0, 1.0 / 255.0));
        float3 detail = (focus.rgb - avg) * (amount * 2.0);
        detail = clamp(detail, -localRange * 0.5, localRange * 0.5);
        focus.rgb = saturate(focus.rgb + detail);
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

// The visibility mask a synthesized focus view is given: nothing of it is
// hidden. The hidden mesh is one zero-area triangle; the visible mesh and the
// line loop are a square of tangents +-10 (about 84 degrees), wider than any
// view; counter-clockwise, at z = -1 as XR_KHR_visibility_mask has them.
XrResult FocusViewVisibilityMask(XrVisibilityMaskTypeKHR type, XrVisibilityMaskKHR* mask) {
    if (!mask) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    constexpr float kEdge = 10.0f;
    static constexpr std::array<XrVector2f, 4> kSquare{{{-kEdge, -kEdge}, {kEdge, -kEdge}, {kEdge, kEdge}, {-kEdge, kEdge}}};
    static constexpr std::array<uint32_t, 6> kSquareTriangles{0, 1, 2, 0, 2, 3};
    static constexpr std::array<uint32_t, 4> kSquareLoop{0, 1, 2, 3};
    static constexpr std::array<XrVector2f, 3> kPoint{};
    static constexpr std::array<uint32_t, 3> kPointTriangle{0, 1, 2};
    std::span<const XrVector2f> vertices;
    std::span<const uint32_t> indices;
    switch (type) {
    case XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR:
        vertices = kPoint;
        indices = kPointTriangle;
        break;
    case XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR:
        vertices = kSquare;
        indices = kSquareTriangles;
        break;
    case XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR:
        vertices = kSquare;
        indices = kSquareLoop;
        break;
    default:
        return XR_ERROR_VALIDATION_FAILURE;
    }
    mask->vertexCountOutput = static_cast<uint32_t>(vertices.size());
    mask->indexCountOutput = static_cast<uint32_t>(indices.size());
    if (mask->vertexCapacityInput == 0 || mask->indexCapacityInput == 0) {
        return XR_SUCCESS; // the sizes, as asked
    }
    if (mask->vertexCapacityInput < vertices.size() || mask->indexCapacityInput < indices.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    if (!mask->vertices || !mask->indices) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::copy(vertices.begin(), vertices.end(), mask->vertices);
    std::copy(indices.begin(), indices.end(), mask->indices);
    return XR_SUCCESS;
}

// A cached frame's display time against the frame's own, for diagnostics:
// "none" when nothing matched (a matched time of 0), not minus the time.
std::string FormatMatchedDeltaNs(XrTime matched_time, XrTime display_time) {
    return matched_time == 0 ? std::string("none") : std::to_string(matched_time - display_time);
}

std::string FormatHex(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

const char* CompositionLayerTypeName(XrStructureType type) {
    switch (type) {
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
        return "projection";
    case XR_TYPE_COMPOSITION_LAYER_QUAD:
        return "quad";
    case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
        return "cube";
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
        return "cylinder";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
        return "equirect";
    case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
        return "equirect2";
    case XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB:
        return "passthrough_fb";
    default:
        return "other";
    }
}

const char* TrackedSpaceKindName(TrackedSpaceKind kind) {
    switch (kind) {
    case TrackedSpaceKind::kView:
        return "VIEW";
    case TrackedSpaceKind::kLocal:
        return "LOCAL";
    case TrackedSpaceKind::kStage:
        return "STAGE";
    case TrackedSpaceKind::kOther:
        break;
    }
    return "OTHER";
}

// A composition layer's space by kind, as the caller tracks it (under its
// lock): the topology functions below never see the layer's spaces otherwise.
using SpaceKindOf = std::function<TrackedSpaceKind(XrSpace)>;

// The frame's first quad layer and its index, or nullptr.
const XrCompositionLayerQuad* FirstQuadLayer(const XrFrameEndInfo& info, std::uint32_t* index = nullptr) {
    for (std::uint32_t i = 0; info.layers && i < info.layerCount; ++i) {
        if (info.layers[i] && info.layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            if (index) {
                *index = i;
            }
            return reinterpret_cast<const XrCompositionLayerQuad*>(info.layers[i]);
        }
    }
    return nullptr;
}

// `value` rounded to `decimals` places; one that rounds to zero prints as 0,
// not -0.
std::string FormatFixed(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(decimals) << std::round(value * scale) / scale + 0.0;
    return stream.str();
}

void HashCompositionTopologyValue(std::uint64_t value, std::uint64_t* hash) {
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
        *hash ^= (value >> (byte * 8u)) & 0xffu;
        *hash *= kFnvPrime;
    }
}

std::uint64_t CompositionLayerTopologySignature(const XrFrameEndInfo* frame_end_info, const SpaceKindOf& kind_of) {
    std::uint64_t hash = 1469598103934665603ull;
    HashCompositionTopologyValue(frame_end_info ? frame_end_info->layerCount : 0, &hash);
    HashCompositionTopologyValue(
        frame_end_info ? static_cast<std::uint64_t>(frame_end_info->environmentBlendMode) : 0, &hash);
    if (!frame_end_info || !frame_end_info->layers) {
        return hash;
    }
    for (std::uint32_t index = 0; index < frame_end_info->layerCount; ++index) {
        const XrCompositionLayerBaseHeader* layer = frame_end_info->layers[index];
        HashCompositionTopologyValue(layer ? static_cast<std::uint64_t>(layer->type) : 0, &hash);
        HashCompositionTopologyValue(layer ? static_cast<std::uint64_t>(layer->layerFlags) : 0, &hash);
        if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            HashCompositionTopologyValue(projection->viewCount, &hash);
        }
        // The space's kind: a quad that moves from VIEW to LOCAL space is a
        // change. A quad's size and pose are not hashed (they can move every
        // frame).
        HashCompositionTopologyValue(
            !layer ? 0
            : layer->space == XR_NULL_HANDLE
                ? 1
                : 2 + static_cast<std::uint64_t>(kind_of(layer->space)),
            &hash);
    }
    return hash;
}

std::string FormatCompositionLayerTopology(const XrFrameEndInfo* frame_end_info, const SpaceKindOf& kind_of) {
    std::ostringstream stream;
    stream << "layerCount=" << (frame_end_info ? frame_end_info->layerCount : 0)
           << ", blendMode="
           << (frame_end_info ? static_cast<int>(frame_end_info->environmentBlendMode) : 0)
           << ", layers=[";
    if (frame_end_info && frame_end_info->layers) {
        for (std::uint32_t index = 0; index < frame_end_info->layerCount; ++index) {
            if (index > 0) {
                stream << ", ";
            }
            const XrCompositionLayerBaseHeader* layer = frame_end_info->layers[index];
            stream << index << ":" << (layer ? CompositionLayerTypeName(layer->type) : "null");
            if (layer) {
                stream << "(type=" << static_cast<int>(layer->type)
                       << ",flags=" << FormatHex(static_cast<std::uint64_t>(layer->layerFlags));
                if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                    stream << ",views=" << projection->viewCount;
                }
                stream << ",space="
                       << (layer->space == XR_NULL_HANDLE ? "NULL" : TrackedSpaceKindName(kind_of(layer->space)));
                if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    // Metres in the layer's space.
                    const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                    const XrVector3f& p = quad->pose.position;
                    const XrQuaternionf& q = quad->pose.orientation;
                    stream << ",size=" << FormatFixed(quad->size.width, 2) << "x" << FormatFixed(quad->size.height, 2)
                           << ",pose=(" << FormatFixed(p.x, 3) << "," << FormatFixed(p.y, 3) << ","
                           << FormatFixed(p.z, 3) << "|" << FormatFixed(q.x, 3) << "," << FormatFixed(q.y, 3) << ","
                           << FormatFixed(q.z, 3) << "," << FormatFixed(q.w, 3)
                           << "),eye=" << static_cast<int>(quad->eyeVisibility);
                }
                stream << ")";
            }
        }
    }
    stream << "]";
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

double HorizontalProjectionCenter(const XrFovf& fov) {
    return (std::tan(fov.angleLeft) + std::tan(fov.angleRight)) * 0.5;
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

double ExtractPoseRollRadians(const XrPosef& pose) {
    const XrQuaternionf& orientation = pose.orientation;
    return std::atan2(
        2.0 * (orientation.w * orientation.z + orientation.x * orientation.y),
        1.0 - 2.0 * (orientation.z * orientation.z + orientation.x * orientation.x));
}

// Wraps an angle to [-pi, pi]; origin-relative yaw deltas must not jump when
// the raw yaw crosses the +/-pi seam.
double WrapRadians(double angle) {
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
    bool hemisphere_corrected{false};
};

GazeRayAngles ExtractGazeRayAngles(const XrQuaternionf& orientation, bool flip_z_only) {
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

    // XR_EXT_eye_gaze_interaction defines forward as -Z, but the Pimax aapvr
    // path through SteamVR has been observed returning the otherwise-valid
    // gaze pose in the +Z hemisphere. A human gaze cannot point behind the
    // headset, so fold that driver convention back into the OpenXR hemisphere
    // before deriving offsets. Without this guard atan2 saturates near +/-90
    // degrees and pins the high-resolution focus inset to a canvas corner.
    // Preserve the established full-direction flip unless the user opts
    // into Z-only correction for reversed gaze tracking.
    const bool hemisphere_corrected = forward.z > 0.0f;
    if (hemisphere_corrected) {
        if (!flip_z_only) {
            forward.x = -forward.x;
            forward.y = -forward.y;
        }
        forward.z = -forward.z;
    }

    const double forward_depth = std::max(0.000001, static_cast<double>(-forward.z));
    return {
        std::atan2(static_cast<double>(forward.x), forward_depth),
        std::atan2(static_cast<double>(forward.y), forward_depth),
        forward,
        hemisphere_corrected,
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

XrPosef ApplyExtraRotationToPose(const XrPosef& pose, float extra_yaw_radians, float extra_pitch_radians,
                               const XrVector3f* translation_anchor = nullptr) {
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
    XrVector3f position = pose.position;
    if (translation_anchor) {
        // Turn the whole tracked displacement, not just the orientation. Keep
        // the seated anchor fixed; rotating absolute room coordinates would
        // orbit the camera around the runtime's origin (often the floor).
        const XrVector3f displacement{position.x - translation_anchor->x,
                                     position.y - translation_anchor->y,
                                     position.z - translation_anchor->z};
        const XrVector3f rotated = RotateVector(extra_rotation, displacement);
        position = {translation_anchor->x + rotated.x,
                    translation_anchor->y + rotated.y,
                    translation_anchor->z + rotated.z};
    }
    return {
        NormalizeQuaternion(MultiplyQuaternion(extra_rotation, pose.orientation)),
        position,
    };
}

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

bool IsIdentityPose(const XrPosef& pose) {
    return NearlyZero(pose.orientation.x) && NearlyZero(pose.orientation.y) &&
           NearlyZero(pose.orientation.z) && NearlyEqual(pose.orientation.w, 1.0f) &&
           NearlyZero(pose.position.x) && NearlyZero(pose.position.y) &&
           NearlyZero(pose.position.z);
}

// MultiplyPoses uses (local, parent) argument order. Adapt it to conventional
// lhs * rhs composition for the coordinate-space conjugation helper.
XrPosef ComposePoses(const XrPosef& lhs, const XrPosef& rhs) {
    return MultiplyPoses(rhs, lhs);
}

XrPosef ReexpressPoseDelta(const XrPosef& pose_delta_in_source,
                           const XrPosef& source_in_target) {
    return ReexpressPivotPoseDelta(
        pose_delta_in_source, source_in_target, ComposePoses, InvertPose);
}

// All generated Pivot movement flows through one pose-offset composition
// point. Motion Assist/manual nudges and Quick Views populate mutually
// exclusive components without creating parallel pose rewrite paths. An
// exclusive Quick View replaces motion; a stacked one includes the live motion
// contribution in its selected pose before routing. The Shoulder Assist
// component is applied after the selected high-level behavior.
struct PivotPoseOffsetComponents {
    XrPosef motion_assist{IdentityPose()};
    XrPosef quick_view{IdentityPose()};
    XrPosef shoulder_assist{IdentityPose()};
    bool quick_view_active{false};
};

XrPosef ComposePivotPoseOffset(const PivotPoseOffsetComponents& components) {
    const XrPosef& high_level =
        components.quick_view_active ? components.quick_view : components.motion_assist;
    return MultiplyPoses(high_level, components.shoulder_assist);
}

XrPosef PoseOffsetBetween(const XrPosef& original, const XrPosef& manipulated) {
    return MultiplyPoses(InvertPose(original), manipulated);
}

void AddPivotViewOffset(PivotViewOffset& offset, const PivotViewOffset& delta) {
    offset.yaw_radians += delta.yaw_radians;
    offset.pitch_radians += delta.pitch_radians;
    offset.right_meters += delta.right_meters;
    offset.up_meters += delta.up_meters;
    offset.forward_meters += delta.forward_meters;
}

PivotViewOffset SubtractPivotViewOffsets(const PivotViewOffset& lhs, const PivotViewOffset& rhs) {
    return {lhs.yaw_radians - rhs.yaw_radians,
            lhs.pitch_radians - rhs.pitch_radians,
            lhs.right_meters - rhs.right_meters,
            lhs.up_meters - rhs.up_meters,
            lhs.forward_meters - rhs.forward_meters};
}

// The same pose as `target`, with its yaw chosen so a transition from
// `current` turns the short way round.
PivotViewOffset NearestEquivalentPivotTarget(const PivotViewOffset& current, PivotViewOffset target) {
    target.yaw_radians = current.yaw_radians + WrapRadians(target.yaw_radians - current.yaw_radians);
    return target;
}

// Pivot offsets name right/up/forward; OpenXR views look down -Z.
XrVector3f PivotOffsetTranslation(const PivotViewOffset& offset, double scale) {
    return {static_cast<float>(offset.right_meters * scale),
            static_cast<float>(offset.up_meters * scale),
            static_cast<float>(-offset.forward_meters * scale)};
}

PivotViewOffset PivotOffsetFromTranslation(const XrVector3f& translation) {
    PivotViewOffset offset;
    offset.right_meters = translation.x;
    offset.up_meters = translation.y;
    offset.forward_meters = -translation.z;
    return offset;
}

XrVector3f AddVectors(const XrVector3f& lhs, const XrVector3f& rhs) {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
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

double ComputePivotExtraAngleRadians(double current_angle_radians,
                                     double rotation_multiplier,
                                     double deadzone_degrees,
                                     double max_extra_degrees,
                                     double smoothing,
                                     double delta_seconds,
                                     double& smoothed_extra_angle_radians,
                                     bool subtract_deadzone_from_gain = true,
                                     double transition_degrees = 0.0) {
    if (rotation_multiplier <= 1.0) {
        smoothed_extra_angle_radians = 0.0;
        return 0.0;
    }

    const double deadzone_radians = DegreesToRadians(std::max(0.0, deadzone_degrees));
    const double max_extra_radians = DegreesToRadians(std::max(0.0, max_extra_degrees));
    const double abs_angle = std::abs(current_angle_radians);

    double target_extra_angle = 0.0;
    // A hard deadzone snaps to zero well inside its edge. A soft deadzone
    // already reaches zero smoothly, so the filter eases out instead.
    if (transition_degrees <= 0.0 && abs_angle <= deadzone_radians * 0.5) {
        smoothed_extra_angle_radians = 0.0;
        return 0.0;
    }

    if (abs_angle > deadzone_radians) {
        if (subtract_deadzone_from_gain) {
            target_extra_angle = ComputePivotContinuousExtra(current_angle_radians,
                                                             rotation_multiplier,
                                                             deadzone_degrees,
                                                             transition_degrees,
                                                             max_extra_degrees);
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

std::string SessionStateName(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return "IDLE";
    case XR_SESSION_STATE_READY:
        return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED:
        return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE:
        return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED:
        return "FOCUSED";
    case XR_SESSION_STATE_STOPPING:
        return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING:
        return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING:
        return "EXITING";
    default:
        return std::to_string(static_cast<int>(state));
    }
}

// The space-warp probe's answer, as its lines say it.
const char* EyeShare(MotionVectorSize size) {
    return size == MotionVectorSize::kHalf      ? "half the eye"
           : size == MotionVectorSize::kQuarter ? "a quarter of the eye"
                                                : "an eighth of the eye";
}

template <typename Handle>
std::string FormatHandle(Handle handle) {
    std::uint64_t value = 0;
    static_assert(sizeof(handle) <= sizeof(value));
    std::memcpy(&value, &handle, sizeof(handle));

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
constexpr XrTime kMaxQuadViewsFovMatchWindow = 5'000'000;
constexpr size_t kMaxCachedQuadViewsFovFrames = 180;
constexpr XrTime kMaxDepthSubmissionMatchWindow = 5'000'000;
constexpr size_t kMaxCachedDepthSubmissionFrames = 180;
constexpr XrDuration kInternalSwapchainWaitTimeout = 100'000'000; // 100 ms
constexpr uint32_t kPivotDiagnosticBurstCount = 8;
constexpr uint64_t kPivotDiagnosticStride = 120;
constexpr std::chrono::seconds kInputDeviceFailureLogInterval{10};
// Consecutive unavailable frames before the eye-gaze focus is logged as lost.
// Debounces sub-second dropouts (e.g. blinks, which invalidate the gaze pose for
// ~100-400ms) so transient gaze loss does not churn the log. ~1/3s at 90Hz.
constexpr uint32_t kEyeGazeUnavailableLogThreshold = 30;
// Identical gaze orientations in a row, read while the session is unfocused,
// after which the runtime is taken to be returning a stale pose. Live eye
// tracking jitters every sample. ~1/2s at 90Hz.
constexpr uint32_t kEyeGazeUnfocusedFrozenFrames = 45;
// How long a focused session's bound eye-gaze input may stay inactive before
// the log says the eye tracker is sending no data.
constexpr std::chrono::seconds kEyeGazeNoDataLogDelay{10};
constexpr std::chrono::seconds kQuadViewsDebugHeartbeatInterval{2};
constexpr std::chrono::seconds kQuadViewsInsetMotionSummaryInterval{30};
// Hot-path throttles. xrLocateSpace/xrLocateViews/xrEndFrame run multiple
// times per frame; filesystem stats, input-device polls, and downstream
// helper calls must not.
constexpr std::chrono::milliseconds kConfigCheckInterval{500};
constexpr std::chrono::milliseconds kInputBindingPollInterval{30};
constexpr std::chrono::milliseconds kAppActionSyncFreshWindow{100};

bool SameInputBinding(const InputBinding& lhs, const InputBinding& rhs);

bool SameInputBindings(const std::vector<InputBinding>& lhs, const std::vector<InputBinding>& rhs);
bool SamePivotActivationBindings(const std::vector<PivotActivationBinding>& lhs,
                                 const std::vector<PivotActivationBinding>& rhs);
bool SamePivotAxisTuning(const PivotAxisTuning& lhs, const PivotAxisTuning& rhs) {
    return NearlyEqual(lhs.rotation_multiplier, rhs.rotation_multiplier) &&
           NearlyEqual(lhs.deadzone_degrees, rhs.deadzone_degrees) &&
           NearlyEqual(lhs.max_extra_degrees, rhs.max_extra_degrees) &&
           NearlyEqual(lhs.deadzone_transition_degrees, rhs.deadzone_transition_degrees);
}

bool SamePivotStepTuning(const PivotStepTuning& lhs, const PivotStepTuning& rhs) {
    return NearlyEqual(lhs.deadzone_degrees, rhs.deadzone_degrees) &&
           NearlyEqual(lhs.trigger_degrees, rhs.trigger_degrees) &&
           NearlyEqual(lhs.amount_degrees, rhs.amount_degrees) &&
           NearlyEqual(lhs.hysteresis_degrees, rhs.hysteresis_degrees) &&
           NearlyEqual(lhs.max_extra_degrees, rhs.max_extra_degrees);
}

bool SamePivotViewControls(const PivotViewControls& lhs, const PivotViewControls& rhs) {
    const PivotNudgeSettings& a = lhs.nudges;
    const PivotNudgeSettings& b = rhs.nudges;
    if (!NearlyEqual(a.yaw_step_degrees, b.yaw_step_degrees) ||
        !NearlyEqual(a.pitch_step_degrees, b.pitch_step_degrees) ||
        !NearlyEqual(a.transition_seconds, b.transition_seconds) ||
        !SameInputBindings(a.yaw_left_bindings, b.yaw_left_bindings) ||
        !SameInputBindings(a.yaw_right_bindings, b.yaw_right_bindings) ||
        !SameInputBindings(a.pitch_up_bindings, b.pitch_up_bindings) ||
        !SameInputBindings(a.pitch_down_bindings, b.pitch_down_bindings) ||
        !SameInputBindings(a.center_bindings, b.center_bindings) ||
        lhs.quick_views.size() != rhs.quick_views.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.quick_views.size(); ++i) {
        const PivotQuickView& left = lhs.quick_views[i];
        const PivotQuickView& right = rhs.quick_views[i];
        if (left.id != right.id || left.name != right.name ||
            !NearlyEqual(left.yaw_degrees, right.yaw_degrees) ||
            !NearlyEqual(left.pitch_degrees, right.pitch_degrees) ||
            !NearlyEqual(left.position_right_cm, right.position_right_cm) ||
            !NearlyEqual(left.position_up_cm, right.position_up_cm) ||
            !NearlyEqual(left.position_forward_cm, right.position_forward_cm) ||
            !NearlyEqual(left.transition_seconds, right.transition_seconds) ||
            !SamePivotActivationBindings(left.activation_bindings, right.activation_bindings)) {
            return false;
        }
    }
    return true;
}
bool SamePivotPositionDirection(const PivotPositionDirection& lhs, const PivotPositionDirection& rhs) {
    return NearlyEqual(lhs.right_cm, rhs.right_cm) &&
           NearlyEqual(lhs.up_cm, rhs.up_cm) &&
           NearlyEqual(lhs.forward_cm, rhs.forward_cm) &&
           NearlyEqual(lhs.start_angle_degrees, rhs.start_angle_degrees) &&
           NearlyEqual(lhs.full_angle_degrees, rhs.full_angle_degrees);
}

bool SamePivotPositionAssist(const PivotPositionAssist& lhs, const PivotPositionAssist& rhs) {
    return lhs.enabled == rhs.enabled &&
           lhs.advanced_directions == rhs.advanced_directions &&
           SamePivotPositionDirection(lhs.yaw, rhs.yaw) &&
           SamePivotPositionDirection(lhs.yaw_left, rhs.yaw_left) &&
           SamePivotPositionDirection(lhs.yaw_right, rhs.yaw_right) &&
           SamePivotPositionDirection(lhs.pitch_up, rhs.pitch_up) &&
           SamePivotPositionDirection(lhs.pitch_down, rhs.pitch_down);
}

bool SamePivotResolvedProfile(const PivotXrResolvedProfile& lhs, const PivotXrResolvedProfile& rhs) {

    return lhs.id == rhs.id &&
           lhs.name == rhs.name &&
           lhs.behavior == rhs.behavior &&
           lhs.apply_motion_to_snap_views == rhs.apply_motion_to_snap_views &&
           SamePivotPositionAssist(lhs.position_assist, rhs.position_assist) &&
           NearlyEqual(lhs.yaw_deadzone_transition_degrees, rhs.yaw_deadzone_transition_degrees) &&
           NearlyEqual(lhs.pitch_deadzone_transition_degrees, rhs.pitch_deadzone_transition_degrees) &&
           lhs.nudge_set_id == rhs.nudge_set_id &&
           lhs.allow_inactive_nudges == rhs.allow_inactive_nudges &&
           lhs.always_active == rhs.always_active &&
           SamePivotActivationBindings(lhs.activation_bindings, rhs.activation_bindings) &&
           SameInputBindings(lhs.set_origin_bindings, rhs.set_origin_bindings) &&
           SameInputBindings(lhs.release_origin_bindings, rhs.release_origin_bindings) &&
           SamePivotViewControls(lhs.view_controls, rhs.view_controls) &&
           NearlyEqual(lhs.smoothing, rhs.smoothing) &&
           NearlyEqual(lhs.activation_ramp_seconds, rhs.activation_ramp_seconds) &&
           NearlyEqual(lhs.yaw_rotation_multiplier, rhs.yaw_rotation_multiplier) &&
           NearlyEqual(lhs.yaw_deadzone_degrees, rhs.yaw_deadzone_degrees) &&
           NearlyEqual(lhs.yaw_max_extra_degrees, rhs.yaw_max_extra_degrees) &&
           NearlyEqual(lhs.pitch_rotation_multiplier, rhs.pitch_rotation_multiplier) &&
           NearlyEqual(lhs.pitch_deadzone_degrees, rhs.pitch_deadzone_degrees) &&
           NearlyEqual(lhs.pitch_max_extra_degrees, rhs.pitch_max_extra_degrees) &&
           lhs.response_mode == rhs.response_mode &&
           lhs.step_glide_mode == rhs.step_glide_mode &&
           NearlyEqual(lhs.step_glide_seconds, rhs.step_glide_seconds) &&
           SamePivotAxisTuning(lhs.yaw_positive, rhs.yaw_positive) &&
           SamePivotAxisTuning(lhs.yaw_negative, rhs.yaw_negative) &&
           SamePivotAxisTuning(lhs.pitch_positive, rhs.pitch_positive) &&
           SamePivotAxisTuning(lhs.pitch_negative, rhs.pitch_negative) &&
           SamePivotStepTuning(lhs.yaw_step_positive, rhs.yaw_step_positive) &&
           SamePivotStepTuning(lhs.yaw_step_negative, rhs.yaw_step_negative) &&
           SamePivotStepTuning(lhs.pitch_step_positive, rhs.pitch_step_positive) &&
           SamePivotStepTuning(lhs.pitch_step_negative, rhs.pitch_step_negative);
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
           lhs.depthxr.depth_anchor == rhs.depthxr.depth_anchor &&
           SameInputBindings(lhs.depthxr_bindings.toggle_enabled_bindings, rhs.depthxr_bindings.toggle_enabled_bindings) &&
           SameInputBindings(lhs.depthxr_bindings.toggle_anchor_bindings, rhs.depthxr_bindings.toggle_anchor_bindings) &&
           SamePivotResolvedSettings(lhs.pivotxr, rhs.pivotxr) &&
           lhs.turbo.enabled == rhs.turbo.enabled &&
           SameInputBindings(lhs.turbo.toggle_bindings, rhs.turbo.toggle_bindings) &&
           lhs.turbo.pacing_mode == rhs.turbo.pacing_mode &&
           lhs.turbo.runtime_pins == rhs.turbo.runtime_pins &&
           lhs.turbo.metrics_mode == rhs.turbo.metrics_mode &&
           SameInputBindings(lhs.turbo.metrics_bindings, rhs.turbo.metrics_bindings) &&
           lhs.quadviews.enabled == rhs.quadviews.enabled &&
           SameInputBinding(lhs.quadviews.diagnostic_visualization_binding, rhs.quadviews.diagnostic_visualization_binding) &&
           lhs.quadviews.eye_tracking_correction == rhs.quadviews.eye_tracking_correction &&
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
           NearlyEqual(lhs.quadviews.gaze_deadzone_degrees, rhs.quadviews.gaze_deadzone_degrees) &&
           lhs.framegen.enabled == rhs.framegen.enabled &&
           lhs.framegen.profile_name == rhs.framegen.profile_name &&
           lhs.framegen.synthesis_mode == rhs.framegen.synthesis_mode &&
           lhs.framegen.flow_quality == rhs.framegen.flow_quality &&
           lhs.framegen.motion_estimation == rhs.framegen.motion_estimation &&
           lhs.framegen.interpolate_flow == rhs.framegen.interpolate_flow &&
           lhs.framegen.multiplier == rhs.framegen.multiplier &&
           lhs.framegen.artifact_suppression == rhs.framegen.artifact_suppression &&
           lhs.framegen.motion_limit_percent == rhs.framegen.motion_limit_percent &&
           lhs.framegen.extend_head_prediction == rhs.framegen.extend_head_prediction &&
           lhs.framegen.diagnostic_view == rhs.framegen.diagnostic_view &&
           SameInputBindings(lhs.framegen.toggle_bindings, rhs.framegen.toggle_bindings);
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

bool SameInputBindings(const std::vector<InputBinding>& lhs, const std::vector<InputBinding>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (!SameInputBinding(lhs[index], rhs[index])) {
            return false;
        }
    }
    return true;
}

bool SamePivotActivationBindings(const std::vector<PivotActivationBinding>& lhs,
                                 const std::vector<PivotActivationBinding>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].behavior != rhs[index].behavior || !SameInputBinding(lhs[index].binding, rhs[index].binding)) {
            return false;
        }
    }
    return true;
}

// True when the action surface (candidate identity and order, behaviors,
// modes, bindings, and View Controls) is unchanged, so runtime edge state can
// survive a config hot-reload. Tuning such as Apply Enhanced Motion to Snap
// Views or the linked Nudge Set applies live: it does not invalidate the
// engaged profile or a selected Snap View.
bool SamePivotActivationSet(const PivotXrResolvedSettings& lhs, const PivotXrResolvedSettings& rhs) {
    if (lhs.enabled != rhs.enabled || lhs.profiles.size() != rhs.profiles.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.profiles.size(); ++i) {
        if (lhs.profiles[i].id != rhs.profiles[i].id ||
            lhs.profiles[i].behavior != rhs.profiles[i].behavior ||
            lhs.profiles[i].always_active != rhs.profiles[i].always_active ||
            !SamePivotActivationBindings(lhs.profiles[i].activation_bindings, rhs.profiles[i].activation_bindings) ||
            !SameInputBindings(lhs.profiles[i].set_origin_bindings, rhs.profiles[i].set_origin_bindings) ||
            !SameInputBindings(lhs.profiles[i].release_origin_bindings, rhs.profiles[i].release_origin_bindings) ||
            !SamePivotViewControls(lhs.profiles[i].view_controls, rhs.profiles[i].view_controls)) {
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
std::string BindingListLabel(const std::vector<InputBinding>& bindings) {
    if (bindings.empty()) {
        return "None";
    }
    std::ostringstream stream;
    for (size_t index = 0; index < bindings.size(); ++index) {
        if (index > 0) stream << " or ";
        stream << BindingLabel(bindings[index]);
    }
    return stream.str();
}
// What a toggle's log line and sound name: the binding of its list that last
// went down, else the list's first (a change Turbo itself makes, say).
InputBinding ActingBinding(const std::optional<InputBinding>& pressed, const std::vector<InputBinding>& bindings) {
    return pressed ? *pressed : bindings.empty() ? InputBinding{} : bindings.front();
}

std::string BindingListLabel(const std::vector<PivotActivationBinding>& bindings) {
    if (bindings.empty()) return "None";
    std::ostringstream stream;
    for (size_t index = 0; index < bindings.size(); ++index) {
        if (index > 0) stream << " or ";
        stream << (bindings[index].behavior == PivotActivationBehavior::Toggle ? "Toggle " : "Hold ")
               << BindingLabel(bindings[index].binding);
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

// Bounds on waiting for Turbo's pipelined runtime xrWaitFrame. The drain is
// the first frame-thread wait in xrEndFrame (submit-interlocked runtimes need
// a submit to release it); the late-wait bound is how long a wait may run
// late before the runtime is treated as holding it. Once it has, later polls
// and submits for that wait give up after the drain bound alone.
constexpr std::chrono::milliseconds kTurboDrainTimeout{250};
constexpr std::chrono::milliseconds kTurboLateWaitTimeout{2000};

} // namespace

OpenXrLayer& OpenXrLayer::Instance() {
    struct Owner {
        OpenXrLayer* layer{new OpenXrLayer()};
        ~Owner() {
            // Static destruction. At a process exit (Star Citizen and MSFS
            // skip xrDestroyInstance) every other thread is already gone,
            // perhaps inside this layer holding its locks, and runtime or
            // driver modules loaded after this layer have already run their
            // DLL_PROCESS_DETACH. Tearing down here would call into them (the
            // OSD's xrDestroySwapchain, GPU waits), end the process on a dead
            // thread's lock (0xC000004B) or wait forever for a dead worker.
            // Leave everything to the exit, except the log's pending
            // "repeated N times" line and buffered lines, written without
            // waiting on its lock.
            // An unload after xrDestroyInstance still destroys the layer.
            if (!ProcessExiting()) delete layer;
            else layer->logger_.FlushAtProcessExit();
        }
    };
    static Owner owner;
    return *owner.layer;
}

OpenXrLayer::~OpenXrLayer() {
    // Safety net for an unload without xrDestroyInstance: join the watcher
    // before members are destroyed so a still-joinable std::thread can't trip
    // terminate(). The watcher only touches this object's own members, all of
    // which outlive this destructor body. Never runs at a process exit.
    StopConfigWatcher();
    StopTurboAsyncWorker();
    DrainRuntimePacingWrites();
    // The logger goes with the members; a later fatal runtime error is still reported, unlogged.
    SetCrtFailureLogger(nullptr);
}

void OpenXrLayer::SetLayerDirectory(std::filesystem::path dll_directory) {
    std::scoped_lock lock(mutex_);
    dll_directory_ = std::move(dll_directory);
}

void OpenXrLayer::SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr) {
    std::scoped_lock lock(mutex_);
    next_get_instance_proc_addr_ = next_get_instance_proc_addr;
}

const InputBinding* OpenXrLayer::PollInputBindingsDown(const std::vector<InputBinding>& bindings) {
    const InputBinding* down = nullptr;
    for (const InputBinding& binding : bindings) {
        if (PollInputBindingDown(binding) && !down) {
            down = &binding;
        }
    }
    return down;
}

bool OpenXrLayer::PollInputBindingDown(const InputBinding& binding) {
    const InputBindingPollResult poll = PollInputBinding(binding);
    if (poll.device_retry_deferred || poll.device_connect_pending) {
        // The first real failure already recorded the unavailable-device
        // diagnostic (or background setup has not finished yet). Keep these
        // bindings entirely off Logger's mutex and string-formatting path.
        return false;
    }
    if (!poll.device_poll_attempted) {
        return poll.down;
    }

    // Device bindings are polled from the locate paths: key them by the GUID
    // they already hold rather than copying it on every poll.
    std::string label_key;
    const std::string& key = binding.device_guid.empty() ? (label_key = BindingLabel(binding)) : binding.device_guid;
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock diagnostic_lock(input_binding_diagnostic_mutex_);
    InputBindingDiagnosticLogState& state = input_binding_diagnostic_states_[key];

    const auto append_result = [](std::ostringstream& stream,
                                  std::string_view label,
                                  std::int64_t code) {
        stream << ", " << label << "=" << DirectInputResultName(code)
               << "(" << FormatHex(static_cast<std::uint32_t>(code)) << ")";
    };
    const auto append_context = [&](std::ostringstream& stream) {
        stream << "binding='" << BindingLabel(binding) << "'"
               << ", guid=" << (binding.device_guid.empty() ? "none" : binding.device_guid)
               << ", stage=" << ToString(poll.diagnostic_stage);
        append_result(stream, "result", poll.result_code);
        if (poll.cooperative_window != 0) {
            stream << ", cooperativeWindow=" << FormatHex(poll.cooperative_window);
        }
        if (poll.reacquire_attempted) {
            append_result(stream, "reacquireResult", poll.reacquire_result_code);
        }
        if (poll.retry_attempted) {
            append_result(stream, "retryResult", poll.retry_result_code);
        }
        if (poll.device_retry_delay_ms > 0) {
            stream << ", nextReconnectInMs=" << poll.device_retry_delay_ms;
        }
    };

    if (poll.diagnostic_stage != InputBindingPollStage::None && poll.recovered) {
        std::ostringstream signature_stream;
        signature_stream << ToString(poll.diagnostic_stage) << ":" << poll.result_code << ":"
                         << poll.reacquire_result_code << ":" << poll.retry_result_code << ":recovered";
        const std::string signature = signature_stream.str();
        const bool interval_elapsed = !state.last_log_time.has_value() ||
                                      now - *state.last_log_time >= kInputDeviceFailureLogInterval;
        if (state.signature != signature || interval_elapsed) {
            std::ostringstream stream;
            stream << "Input device polling recovered during retry: ";
            append_context(stream);
            stream << ".";
            logger_.Info(stream.str());
            state.last_log_time = now;
        }
        state.failure_active = false;
        state.signature = signature;
        state.failed_attempts = 0;
        state.suppressed_attempts = 0;
        return poll.down;
    }

    if (poll.diagnostic_stage != InputBindingPollStage::None) {
        std::ostringstream signature_stream;
        signature_stream << ToString(poll.diagnostic_stage) << ":" << poll.result_code << ":"
                         << poll.reacquire_result_code << ":" << poll.retry_result_code
                         << (poll.device_waits_for_rescan ? ":waits" : "");
        const std::string signature = signature_stream.str();
        const bool changed = state.signature != signature;
        // A device waiting to be plugged in makes no attempts: once is enough.
        const bool interval_elapsed = !poll.device_waits_for_rescan &&
                                      (!state.last_log_time.has_value() ||
                                       now - *state.last_log_time >= kInputDeviceFailureLogInterval);
        state.failure_active = true;
        if (!poll.device_retry_deferred) {
            ++state.failed_attempts;
        }
        if (changed || interval_elapsed) {
            std::ostringstream stream;
            stream << (poll.device_waits_for_rescan ? "Input device not connected: " : "Input device polling unavailable: ");
            append_context(stream);
            stream << ", failedAttempts=" << state.failed_attempts;
            if (state.suppressed_attempts > 0) {
                stream << ", suppressedRepeats=" << state.suppressed_attempts;
            }
            if (poll.device_waits_for_rescan) {
                stream << ". Its bindings stay inactive until it is plugged in: VectorXR looks again when "
                       << (poll.device_arrival_notified ? "Windows reports a device connected, " : "")
                       << "the next session starts, or Rescan inputs in the VectorXR app's Settings is pressed.";
            } else {
                stream << ". The binding will remain inactive while VectorXR retries with reconnect backoff.";
            }
            logger_.Info(stream.str());
            state.signature = signature;
            state.last_log_time = now;
            state.suppressed_attempts = 0;
        } else {
            ++state.suppressed_attempts;
        }
        return false;
    }

    if (state.failure_active) {
        logger_.Info("Input device polling restored: binding='" + BindingLabel(binding) +
                     "', failedAttempts=" + std::to_string(state.failed_attempts) + ".");
        state = InputBindingDiagnosticLogState{};
    }
    return poll.down;
}

bool OpenXrLayer::CanCreateInstance() {
    std::scoped_lock lock(mutex_);
    return instance_ == XR_NULL_HANDLE;
}

XrResult OpenXrLayer::OnInstanceCreated(const XrInstanceCreateInfo* create_info,
                                        XrInstance instance,
                                        bool eye_gaze_extension_enabled,
                                        const InstanceCreateDiagnostics& diagnostics) {
    std::scoped_lock lock(mutex_);

    instance_ = instance;
    ResetInstanceState();
    framegen_teardowns_watched_.store(false, std::memory_order_release);
    turbo_clock_enabled_ = diagnostics.turbo_clock_enabled;
    d3d12_extension_enabled_ = diagnostics.d3d12_enabled;
    vulkan_enable_version_ = diagnostics.vulkan_enable_version;
    vulkan_enable1_requested_ = diagnostics.vulkan_enable1_requested;
    // Decided once for the instance, so both calls of an extension list's
    // two-call idiom, the game's Vulkan device and its session agree. The
    // Vulkan bridge's own decision only: an application that also enables
    // XR_KHR_D3D11_enable had XR_KHR_D3D12_enable added for the Direct3D 11
    // bridge, whatever VECTORXR_VULKAN_BRIDGE says.
    vulkan_bridge_extensions_ = vulkan_enable_version_ != 0 && d3d12_extension_enabled_ &&
                                ((diagnostics.d3d12_wanted && !diagnostics.app_requested_d3d11) || WantsVulkanBridge());
    ResetVulkanEnableHooks(instance, next_get_instance_proc_addr_, vulkan_bridge_extensions_,
                           [this](const std::string& message) { logger_.Info(message); });
    eye_gaze_extension_enabled_ = eye_gaze_extension_enabled;
    varjo_compatible_quadviews_active_ = diagnostics.varjo_compatible_quad_forwarded;
    spacewarp_fb_owned_ = diagnostics.fb_space_warp_owned;
    spacewarp_ext_owned_ = diagnostics.frame_synthesis_owned;
    openxr_toolkit_above_ = diagnostics.openxr_toolkit == "above";
    openxr_toolkit_quadviews_logged_ = false;
    resolved_settings_generation_ = ~0ull; // resolve again with the Toolkit's position
    ResetSessionState();
    config_path_ = ResolveConfigPath();
    log_path_ = ResolveLogPath();
    BeginInstanceLogLocked(log_path_);

    current_exe_name_ = GetCurrentExecutableName();
    logger_.Info(std::string("VectorXR layer version: ") + VECTORXR_VERSION);
    logger_.Info(std::string("VectorXR layer build: ") + __DATE__ + " " + __TIME__);
    logger_.Info("VectorXR attached to process: " + current_exe_name_);
    {
        const ProcessInteropSnapshot interop = GetProcessInteropSnapshot();
        std::ostringstream stream;
        stream << "Interop snapshot: inProcessModules=";
        if (!interop.module_scan_succeeded) {
            stream << "unavailable";
        } else {
            stream << interop.modules.size();
            for (const LoadedInteropModule& module : interop.modules) {
                stream << " [" << module.name << "=" << PathToUtf8(module.path) << "]";
            }
        }
        stream << ", knownCompanionProcesses=";
        if (!interop.process_scan_succeeded) {
            stream << "unavailable";
        } else {
            stream << interop.processes.size();
            for (const RunningInteropProcess& process : interop.processes) {
                stream << " [" << process.name << ",pid=" << process.process_id << "]";
            }
        }
        logger_.Info(stream.str());
    }

    if (create_info) {
        quad_views_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_QUAD_VIEWS_EXTENSION_NAME);
        varjo_foveated_rendering_extension_requested_ =
            ExtensionRequested(create_info, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
        d3d11_graphics_extension_requested_ =
            ExtensionRequested(create_info, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);

        std::ostringstream stream;
        const XrVersion api_version = create_info->applicationInfo.apiVersion;
        application_can_select_quad_views_ = quad_views_extension_requested_ ||
                                             varjo_foveated_rendering_extension_requested_ ||
                                             api_version >= XR_MAKE_VERSION(1, 1, 0);
        stream << "Application=" << create_info->applicationInfo.applicationName << ", Engine="
               << create_info->applicationInfo.engineName
               << ", AppVersion=" << create_info->applicationInfo.applicationVersion
               << ", OpenXRApiVersion=" << XR_VERSION_MAJOR(api_version) << "."
               << XR_VERSION_MINOR(api_version) << "." << XR_VERSION_PATCH(api_version);
        logger_.Info(stream.str());

        instance_enabled_extension_count_ = create_info->enabledExtensionCount;
        std::ostringstream ext_stream;
        ext_stream << "Application enabled OpenXR extensions (" << create_info->enabledExtensionCount << "):";
        for (uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
            if (create_info->enabledExtensionNames && create_info->enabledExtensionNames[i]) {
                ext_stream << ' ' << create_info->enabledExtensionNames[i];
            }
        }
        logger_.Info(ext_stream.str());
        logger_.Info("OpenXR API layers below VectorXR: [" + diagnostics.layers_below + "]");
        if (diagnostics.openxr_toolkit != "absent") {
            logger_.Info("OpenXR Toolkit is loaded " +
                         std::string(diagnostics.openxr_toolkit == "below" ? "below VectorXR."
                                     : diagnostics.openxr_toolkit == "above"
                                         ? "above VectorXR."
                                         : "above VectorXR, bypassed for this application."));
        }
        // The bridge the instance's XR_KHR_D3D12_enable is for, and what Frame
        // Generation does without it.
        const bool vulkan_bridge = !diagnostics.app_requested_d3d11 && diagnostics.vulkan_enable_version != 0;
        const std::string bridge_name = vulkan_bridge ? "Vulkan bridge" : "Direct3D 11 bridge";
        const std::string without_bridge = vulkan_bridge ? "Frame Generation is off for Vulkan sessions."
                                                         : "Frame Generation keeps Direct3D 11 sessions as they are.";
        if (diagnostics.layer_injected_d3d12) {
            logger_.Info("Frame Generation's " + bridge_name + ": XR_KHR_D3D12_enable " +
                         (diagnostics.d3d12_enabled
                              ? "added to the instance's extensions" +
                                    std::string(diagnostics.d3d12_scan_trusted
                                                    ? "."
                                                    : " (the runtime's extension list before the instance was not "
                                                      "believable).")
                              : "left out after the runtime refused an extension (instance result " +
                                    std::to_string(static_cast<int>(diagnostics.first_create_result)) +
                                    "; created again without it, result " +
                                    std::to_string(static_cast<int>(diagnostics.d3d12_retry_result)) +
                                    (diagnostics.d3d12_refused_alone
                                         ? "; the instance has VectorXR's other additions"
                                         : "; no single one of VectorXR's additions explained the refusal, so the "
                                           "instance has the application's own extensions") +
                                    "); " + without_bridge));
        } else if (diagnostics.d3d12_wanted) {
            logger_.Info("Frame Generation's " + bridge_name +
                         " is unavailable: the runtime does not offer XR_KHR_D3D12_enable; " + without_bridge);
        }
        if (quad_views_extension_requested_ || varjo_foveated_rendering_extension_requested_) {
            logger_.Info(std::string("Application requested layer-owned extensions: quadViews=") +
                         (quad_views_extension_requested_ ? "true" : "false") +
                         ", varjoFoveatedRendering=" +
                         (varjo_foveated_rendering_extension_requested_ ? "true" : "false"));
        }
    }
    if (varjo_compatible_quadviews_active_) {
        logger_.Info(
            "Native Varjo quadviews forwarding ACTIVE: the app/runtime quad-view contract is preserved "
            "independently of VectorXR profiles. When VectorXR Quadviews resolves enabled, it may apply "
            "focus/peripheral resolution scaling and focus sharpening; otherwise this path is transparent.");
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
                 << ", extScanComplete=" << (diagnostics.pre_instance_extension_scan_complete ? 1 : 0)
                 << ", extScanDetail=" << diagnostics.pre_instance_extension_scan_detail
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
        if (diagnostics.eye_gaze_probe_structurally_unreliable) {
            logger_.Info(
                "DIAGNOSTIC: the completed pre-instance extension probe is structurally unreliable "
                "(empty or missing extensions VectorXR will forward). Eye-gaze absence will be treated "
                "as advisory and tested through the safe instance-create retry. Missing forwarded "
                "extensions: [" +
                diagnostics.pre_instance_missing_forwarded_extensions + "]");
        }
        if (diagnostics.active_runtime_is_varjo && quad_views_extension_requested_ &&
            !diagnostics.runtime_advertises_varjo_quad) {
            logger_.Info(
                "DIAGNOSTIC: the application enabled XR_VARJO_quad_views but the layer's pre-instance probe "
                "did not report it — confirming the pre-instance extension probe is unreliable here. Varjo "
                "detection now uses the active OpenXR runtime instead (see activeRuntimeIsVarjo).");
        }
    }

    const bool spacewarp_probe_set = !diagnostics.spacewarp_probe_value.empty();
    const bool spacewarp_probe_named = diagnostics.spacewarp_probe != "off";
    if (spacewarp_probe_set || diagnostics.app_requested_fb_space_warp || diagnostics.app_requested_frame_synthesis) {
        std::string taken_over;
        if (spacewarp_fb_owned_) taken_over += " " XR_FB_SPACE_WARP_EXTENSION_NAME;
        if (spacewarp_ext_owned_) taken_over += " " XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME;
        std::ostringstream stream;
        stream << "Space-warp probe: VECTORXR_SPACEWARP_PROBE="
               << (spacewarp_probe_named || !spacewarp_probe_set ? diagnostics.spacewarp_probe
                                                                 : "'" + diagnostics.spacewarp_probe_value + "'")
               << ", appEnabledFbSpaceWarp=" << (diagnostics.app_requested_fb_space_warp ? 1 : 0)
               << ", appEnabledFrameSynthesis=" << (diagnostics.app_requested_frame_synthesis ? 1 : 0)
               << ", extScanComplete=" << (diagnostics.spacewarp_scan_complete ? 1 : 0)
               << ", runtimeListsFbSpaceWarp=" << (diagnostics.runtime_lists_fb_space_warp ? 1 : 0)
               << ", runtimeListsFrameSynthesis=" << (diagnostics.runtime_lists_frame_synthesis ? 1 : 0)
               << ", takenOver=" << (taken_over.empty() ? std::string("none") : taken_over.substr(1));
        logger_.Info(stream.str());
        const SpaceWarpProbeSelection selection = ParseSpaceWarpProbe(diagnostics.spacewarp_probe.c_str());
        if (spacewarp_probe_set && !spacewarp_probe_named) {
            logger_.Info("Space-warp probe: WARNING: VECTORXR_SPACEWARP_PROBE='" + diagnostics.spacewarp_probe_value +
                         "' is not fb, ext or both. A probe manifest loads the layer for any value and advertises "
                         "its extensions, so the layer takes over those the application enabled and the runtime "
                         "lacks, as the runtime would refuse them; unset the variable to turn the probe off.");
        } else if ((spacewarp_fb_owned_ && !selection.fb) || (spacewarp_ext_owned_ && !selection.ext)) {
            logger_.Info("Space-warp probe: WARNING: the application enabled an extension "
                         "VECTORXR_SPACEWARP_PROBE=" +
                         diagnostics.spacewarp_probe +
                         " does not name (the other probe manifest is registered?); the layer takes it over too, "
                         "as the runtime would refuse it.");
        }
        if (!spacewarp_probe_set) {
            logger_.Info("Space-warp probe: off; the application's space-warp extensions go to the runtime as "
                         "they are.");
        }
    }
    // The handshake census (handshake_census.h), with the probe's variable set.
    handshake_census_.Reset(spacewarp_probe_set);
    for (std::atomic<PFN_xrVoidFunction>& next : census_next_) {
        next.store(nullptr, std::memory_order_release);
    }
    census_swapchain_formats_.clear();
    census_swapchains_.clear();
    census_roles_.clear();
    for (const std::string& line : diagnostics.handshake_census) {
        logger_.Info("Handshake census: " + line);
    }

    const bool log_eye_gaze_startup =
        diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering ||
        diagnostics.app_requested_eye_gaze || diagnostics.layer_injected_eye_gaze_request ||
        diagnostics.retried_without_eye_gaze;
    if (log_eye_gaze_startup) {
        const char* extension_request = diagnostics.app_requested_eye_gaze
                                            ? "application"
                                            : diagnostics.layer_injected_eye_gaze_request ? "layer-injected"
                                                                                          : "skipped";
        const char* tracking_path = eye_gaze_extension_enabled
                                        ? "ext-eye-gaze-action"
                                        : diagnostics.varjo_compatible_quad_forwarded ? "native-varjo-runtime"
                                                                                       : "head-static";
        std::ostringstream stream;
        stream << "Quadviews eye-gaze startup: probe="
               << EyeGazeProbeStateName(diagnostics.eye_gaze_probe_state)
               << ", probeDetail=" << diagnostics.pre_instance_extension_scan_detail
               << ", probeXrResult=" << static_cast<int>(diagnostics.pre_instance_extension_scan_result)
               << ", probeHeuristic=" << (!diagnostics.pre_instance_extension_scan_complete
                                               ? "not-evaluated"
                                               : diagnostics.eye_gaze_probe_structurally_unreliable
                                                     ? "inconsistent"
                                                     : "credible")
               << ", probeMissingForwardedCount="
               << diagnostics.pre_instance_missing_forwarded_extension_count
               << ", runtimeWorkaround=" << (diagnostics.eye_gaze_probe_known_unreliable ? 1 : 0)
               << ", extensionRequest=" << extension_request
               << ", requestReason=" << EyeGazeRequestReasonName(diagnostics.eye_gaze_request_reason)
               << ", firstCreateResult=" << static_cast<int>(diagnostics.first_create_result)
               << ", firstDownstreamExtensionCount=" << diagnostics.first_downstream_extension_count
               << ", retry=" << (diagnostics.retried_without_eye_gaze ? "without-eye-gaze" : "not-needed");
        if (diagnostics.retried_without_eye_gaze) {
            stream << ", retryCreateResult=" << static_cast<int>(diagnostics.retry_create_result);
        }
        stream << ", finalDownstreamExtensionCount=" << diagnostics.final_downstream_extension_count
               << ", finalExtensionEnabled=" << (eye_gaze_extension_enabled ? 1 : 0)
               << ", trackingPath=" << tracking_path;
        logger_.Info(stream.str());
    }
    if (eye_gaze_extension_enabled_) {
        logger_.Info("Enabled XR_EXT_eye_gaze_interaction downstream for VectorXR quadviews.");
    } else if (varjo_compatible_quadviews_active_ && varjo_foveated_rendering_extension_requested_) {
        logger_.Info("XR_EXT_eye_gaze_interaction is not needed in native Varjo mode; rendering gaze will use "
                     "XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO.");
    } else if (diagnostics.app_requested_quad_views || diagnostics.app_requested_varjo_foveated_rendering) {
        logger_.Info("XR_EXT_eye_gaze_interaction is not enabled downstream; VectorXR quadviews eye tracking "
                     "will use head/static focus if no other tracker is available.");
    }

    CaptureInstanceFunctions();
    if (!next_destroy_instance_ || !next_create_session_ || !next_destroy_session_ || !next_begin_session_ ||
        !next_end_session_ ||
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

    const bool config_was_loaded_before_log_initialization = has_loaded_config_;
    ReloadConfigIfNeeded();
    if (config_was_loaded_before_log_initialization) {
        if (!last_failed_config_error_.empty()) {
            logger_.Error("Failed to parse config before log initialization: " + last_failed_config_error_ +
                          ". Using default settings; VectorXR enhancements are disabled until a valid config is loaded.");
        } else if (has_config_timestamp_) {
            logger_.Info("Loaded config from " + PathToUtf8(config_path_));
            for (const std::string& line : UnloggedConfigWarningsLocked()) {
                logger_.Info(line);
            }
        } else {
            logger_.Info("No config file found. Using default settings.");
        }
    }
    // An instance with no extension cannot create a session: MSFS 2024's
    // probe of the runtime is not a launch (it doubled MSFS's count).
    if (config_.core.track_seen_apps && instance_enabled_extension_count_ > 0) {
        std::string seen_apps_error;
        if (!RecordSeenApp(current_exe_name_, &seen_apps_error)) {
            logger_.Debug("Unable to record seen app: " + seen_apps_error);
        }
    }
    RefreshResolvedSettings();
    logger_.Info("Active log file: " + PathToUtf8(logger_.ActiveLogPath()));

    // Hand ongoing config hot-reload to the watcher thread now that the initial
    // load and config_path_ are established. From here the render hot path never
    // touches the filesystem for config.
    StartConfigWatcher();
    return XR_SUCCESS;
}

void OpenXrLayer::AbandonInstance() {
    // The config watcher is the last thing OnInstanceCreated starts, so it is
    // not running.
    std::scoped_lock lock(mutex_);
    ResetVulkanEnableHooks(XR_NULL_HANDLE, nullptr, false, {});
    instance_ = XR_NULL_HANDLE;
    has_loaded_config_ = false;
    ResetInstanceState();
    ResetSessionState();
    handshake_census_.Reset(false);
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
    logger_.Info("xrDestroyInstance requested by application.");
    if (handshake_census_.Active()) {
        CensusFlushProcAddrs("since the last of them");
        if (const std::string repeats = handshake_census_.ProcAddrRepeats(); !repeats.empty()) {
            logger_.Info("Handshake census: " + repeats);
        }
    }
    TeardownWatch watch(logger_, "xrDestroyInstance", framegen_.TeardownWait(),
                        [this] { return framegen_.PresenterActivity(); },
                        framegen_teardowns_watched_.load(std::memory_order_acquire));
    // Join the watcher before taking mutex_: it may be mid-PollConfigFile
    // holding the lock, so stopping it under mutex_ would deadlock.
    watch.Step("stopping the layer's background work");
    StopConfigWatcher();
    StopTurboAsyncWorker();
    performance_.EndSession(); // stops the sampler (wanted again by PrepareOsd)
    performance_wanted_ = false;
    // The loader may unload the layer after this returns; let in-flight
    // background input-device setup finish first.
    DrainInputDeviceWork(std::chrono::seconds(2));

    // A session still alive goes with the instance: its presenter thread
    // first, before the layer's lock (as at xrDestroySession).
    XrSession session = XR_NULL_HANDLE;
    {
        std::scoped_lock lock(mutex_);
        session = active_session_;
        if (session != XR_NULL_HANDLE && vulkan_bridge_.Active()) {
            watch.Step("letting the Vulkan bridge's last releases through");
            vulkan_bridge_.Escape();
        }
    }
    if (session != XR_NULL_HANDLE) {
        StopFrameGenPresenterForTeardown(session, [&watch](const char* step) { watch.Step(step); });
    }

    watch.Step("stopping Frame Generation and the layer's resources");
    std::scoped_lock lock(mutex_);

    // A bridged Vulkan session's last releases get through the bridge's
    // queue first (or are let through): Frame Generation's waits on it end.
    if (vulkan_bridge_.Active()) {
        if (!framegen_.PresenterAbandoned()) {
            SubmitKeptVulkanBridgeReleasesLocked();
        }
        vulkan_bridge_.Escape();
    }
    framegen_.Stop();
    framegen_owns_pacing_.store(false, std::memory_order_release);
    framegen_session_enabled_ = false;
    turbo_yielded_to_framegen_ = false;
    // A presenter thread left inside the runtime may be submitting the OSD's
    // swapchain or using the layer's spaces: they stay with the session.
    if (!framegen_.PresenterAbandoned()) {
        osd_.Shutdown();
        DestroyEyeGazeResources();
        DestroyVarjoNativeFoveationResources();
        DestroyInternalReferenceSpaces();
        ResetSwapchainState();
        watch.Step("draining the bridges");
        d3d11_bridge_.Drain();
        vulkan_bridge_.Drain();
    }
    if (framegen_.PresenterAbandoned()) {
        // The thread left behind may still be inside the runtime: its
        // instance (and the runtime's module, and this one) stay for the
        // process's exit, with what the layer holds of them.
        PinModulesForAbandonedPresenter();
        logger_.Error("xrDestroyInstance: the runtime's instance is kept for the process's exit (Frame Generation's "
                      "presenter thread never left the runtime).");
        return XR_SUCCESS;
    }
    watch.Step("in the runtime's xrDestroyInstance");
    const XrResult result = next_destroy_instance_(instance);
    watch.Step("ending the bridges");
    framegen_queue_.Reset();
    EndD3D11Bridge();
    EndVulkanBridge();
    if (XR_SUCCEEDED(result)) {
        ResetVulkanEnableHooks(XR_NULL_HANDLE, nullptr, false, {});
        turbo_recovery_.Complete();
        // An instance that enabled no extension cannot create a session:
        // MSFS 2024 makes one to probe the runtime before its real instance
        // (4 of the 7 logs in one tester's export). Its log would take a slot
        // of the log retention and could be picked as one of Export Debug's
        // complete sessions, so it is removed unless it logged an error, or
        // the handshake census took one (that probe is part of the handshake).
        if (!instance_requested_session_ && instance_enabled_extension_count_ == 0 && !handshake_census_.Active()) {
            logger_.DiscardSessionLog();
        }
        instance_ = XR_NULL_HANDLE;
        next_destroy_instance_ = nullptr;
        next_create_session_ = nullptr;
        next_destroy_session_ = nullptr;
        next_begin_session_ = nullptr;
        next_end_session_ = nullptr;
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
        next_wait_frame_ = nullptr;
        next_begin_frame_ = nullptr;
        next_end_frame_ = nullptr;
        next_locate_space_ = nullptr;
        next_poll_event_ = nullptr;
        next_locate_views_ = nullptr;
        next_string_to_path_ = nullptr;
        next_create_action_set_ = nullptr;
        next_destroy_action_set_ = nullptr;
        next_create_action_ = nullptr;
        next_destroy_action_ = nullptr;
        next_suggest_interaction_profile_bindings_ = nullptr;
        next_get_action_state_pose_ = nullptr;
        next_enumerate_bound_sources_for_action_ = nullptr;
        has_loaded_config_ = false;
        locate_views_call_count_ = 0;
        pending_locate_views_diagnostics_ = 0;
        pending_end_frame_diagnostics_ = 0;
        ResetPivotActivationState();
        ResetDepthToggleState();
        ResetInstanceState();
        ResetSessionState();
        handshake_census_.Reset(false);
    }

    return result;
}

XrResult OpenXrLayer::CreateSession(XrInstance instance,
                                    const XrSessionCreateInfo* create_info,
                                    XrSession* session) {
    {
        std::scoped_lock lock(mutex_);
        instance_requested_session_ = true;
        if (active_session_ != XR_NULL_HANDLE) {
            logger_.Error("VectorXR currently supports one OpenXR session per instance; rejecting a second "
                          "session instead of corrupting the active session state.");
            return XR_ERROR_LIMIT_REACHED;
        }
    }
    thread_census_.Reset();
    NoteThread(ThreadCensus::Call::CreateSession);
    {
        const void* graphics_binding = create_info ? create_info->next : nullptr;
        const auto* next_struct = static_cast<const XrBaseInStructure*>(graphics_binding);
        logger_.Info(std::string("xrCreateSession requested by application: graphicsBindingType=") +
                     (next_struct ? std::to_string(next_struct->type) : "none"));
    }

    // Frame Generation running this session gives the runtime a Direct3D 12
    // queue of its own (FrameGenQueueSubstitution), decided before the
    // session exists. A Direct3D 11 game's session becomes Direct3D 12 for
    // it (D3D11Bridge): the bridge's queue then stands for the game's.
    const XrSessionCreateInfo* downstream_info = create_info;
    std::string bridge_description;
    // The session's graphics binding is the first in the chain, as Frame
    // Generation's backend factory finds it: a Vulkan binding after another
    // structure makes a Vulkan session too (one the bridge does not take).
    const auto* first_binding = create_info ? static_cast<const XrBaseInStructure*>(create_info->next) : nullptr;
    const XrBaseInStructure* graphics_binding = first_binding;
    while (graphics_binding && graphics_binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
           graphics_binding->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
           graphics_binding->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        graphics_binding = graphics_binding->next;
    }
    const bool vulkan_session = graphics_binding && graphics_binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    // A Vulkan session runs Frame Generation only through the Vulkan bridge:
    // why one is not bridged.
    std::string vulkan_unbridged;
    bool vulkan_queried_d3d12 = false; // the Vulkan bridge asked for Direct3D 12's graphics requirements
    bool framegen_wanted = false;
    {
        {
            std::scoped_lock lock(mutex_);
            ReloadConfigIfNeeded();
            const ResolvedRuntimeConfig resolved = ResolveRuntimeConfig(config_, current_exe_name_);
            framegen_wanted = resolved.core.enabled && resolved.framegen.enabled;
        }
        framegen_queue_.Reset();
        d3d11_bridge_.End();
        vulkan_bridge_.End();
        if (framegen_wanted) {
            const XrSessionCreateInfo* game_info = create_info;
            const auto* first = create_info ? static_cast<const XrBaseInStructure*>(create_info->next) : nullptr;
            if (first && first->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const char* bridge_switch = std::getenv("VECTORXR_D3D11_BRIDGE");
                if (bridge_switch && std::string(bridge_switch) == "off") {
                    logger_.Info("Frame Generation keeps this Direct3D 11 session as it is: "
                                 "VECTORXR_D3D11_BRIDGE=off.");
                } else if (!d3d12_extension_enabled_) {
                    logger_.Info("Frame Generation keeps this Direct3D 11 session as it is: the instance has no "
                                 "XR_KHR_D3D12_enable for its bridge (Frame Generation was enabled after the game "
                                 "started, or the runtime refused the extension). Restart the game to use it.");
                } else {
                    std::string reason;
                    d3d11_bridge_.SetLog([this](const std::string& message) { logger_.Error(message); });
                    if (const XrSessionCreateInfo* bridged = d3d11_bridge_.Begin(
                            instance, create_info, next_get_instance_proc_addr_, reason, bridge_description)) {
                        game_info = bridged;
                    } else {
                        logger_.Info("Frame Generation keeps this Direct3D 11 session as it is (no bridge): " +
                                     reason);
                    }
                }
            } else if (vulkan_session) {
                if (const std::string hooks = VulkanEnableHookSummary(); !hooks.empty()) {
                    logger_.Info("Frame Generation's Vulkan bridge: what the XR_KHR_vulkan_enable hooks did for the "
                                 "game's Vulkan instance and device: " +
                                 hooks + ".");
                }
                const char* bridge_switch = std::getenv("VECTORXR_VULKAN_BRIDGE");
                const std::uint64_t device = VulkanSessionDevice(create_info);
                const VulkanBridgeGame game = VulkanGameFor(device);
                if (graphics_binding != first_binding) {
                    vulkan_unbridged = "its Vulkan binding is not the first structure of the session's create info";
                } else if (bridge_switch && std::string(bridge_switch) == "off") {
                    vulkan_unbridged = "VECTORXR_VULKAN_BRIDGE=off";
                } else if (!d3d12_extension_enabled_) {
                    vulkan_unbridged = "the instance has no XR_KHR_D3D12_enable for the bridge (Frame Generation was "
                                       "enabled after the game started, or the runtime refused or does not offer the "
                                       "extension); restart the game to use it";
                } else if (!vulkan_bridge_extensions_) {
                    vulkan_unbridged = "Frame Generation was enabled after the game started; restart the game to use it";
                } else if (VulkanDeviceOriginOf(device) == VulkanDeviceOrigin::kRuntimeWithoutLayer) {
                    vulkan_unbridged = "the runtime made the game's Vulkan device without the layer's vkCreateDevice, "
                                       "so it lacks what the bridge needs";
                } else {
                    std::string reason;
                    vulkan_bridge_.SetLog([this](const std::string& message) { logger_.Error(message); });
                    vulkan_bridge_.SetInfoLog([this](const std::string& message) { logger_.Info(message); });
                    const XrSessionCreateInfo* bridged = vulkan_bridge_.Begin(
                        instance, create_info, next_get_instance_proc_addr_, game, reason, bridge_description);
                    vulkan_queried_d3d12 = vulkan_bridge_.QueriedD3D12Requirements();
                    if (bridged) {
                        game_info = bridged;
                    } else {
                        vulkan_unbridged = "no bridge: " + reason;
                    }
                }
            }
            std::string note;
            framegen_queue_.SetLog([this](const std::string& message) { logger_.Error(message); });
            // A layer the runtime shows over the projection every frame (Star
            // Citizen's interface, OpenKneeboard's kneeboard in any game)
            // would otherwise make every frame Frame Generation writes wait
            // for the game's GPU work: on a GPU-bound game, a refresh missed
            // wherever the runtime's xrEndFrame waits for its queue (Varjo).
            downstream_info = framegen_queue_.Prepare(game_info, note, true);
            const char* queue_diagnostics = std::getenv("VECTORXR_FRAMEGEN_QUEUE_DIAGNOSTICS");
            framegen_queue_diagnostics_ = queue_diagnostics && std::string_view(queue_diagnostics) == "1";
            framegen_queue_diagnostics_next_ms_.store(0, std::memory_order_relaxed);
            if (framegen_queue_diagnostics_ && framegen_queue_.Active()) {
                logger_.Info("Frame Generation queue diagnostics enabled "
                             "(VECTORXR_FRAMEGEN_QUEUE_DIAGNOSTICS=1): runtime xrEndFrame calls taking "
                             "at least 250ms record submitted swapchains and queue progress, at most once every 5s.");
            }
            if (!note.empty()) logger_.Info(note);
            if (framegen_queue_.ShownLayersLazy()) {
                logger_.Info("Frame Generation keeps releases of layers it does not capture lazy "
                             "(VECTORXR_FRAMEGEN_SHOWN_LAYERS=off turns this off): the runtime shows each layer's "
                             "newest finished image, at most one game frame old.");
            }
            if (d3d11_bridge_.Active() && !framegen_queue_.Active()) {
                // The runtime must never be given the bridge's queue: it
                // waits for the game's Direct3D 11 work.
                logger_.Info("Frame Generation keeps this Direct3D 11 session as it is (no bridge): the runtime "
                             "would be given the bridge's queue.");
                framegen_queue_.Reset();
                d3d11_bridge_.End();
                downstream_info = create_info;
            }
            if (vulkan_bridge_.Active() && !framegen_queue_.Active()) {
                // Likewise: the runtime's queue must never wait for the game's Vulkan work.
                vulkan_unbridged = "the runtime would be given the bridge's queue";
                framegen_queue_.Reset();
                vulkan_bridge_.End();
                downstream_info = create_info;
            }
        }
    }

    // A runtime may expect the graphics requirements of the API it was
    // asked about last: a session made with Direct3D 11 after the bridge
    // asked for Direct3D 12's asks for Direct3D 11's again.
    const auto requery_d3d11_requirements = [&] {
        if (!d3d11_bridge_.QueriedD3D12Requirements() || !create_info || !next_get_instance_proc_addr_) {
            return;
        }
        PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
        if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                                      reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) &&
            get_requirements) {
            XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
            get_requirements(instance, create_info->systemId, &requirements);
        }
    };
    // Likewise for the Vulkan requirements, of the extension the game made
    // its device with (a game enabling both that made its device itself used
    // XR_KHR_vulkan_enable's).
    const auto requery_vulkan_requirements = [&] {
        if (!vulkan_queried_d3d12 || !create_info || !next_get_instance_proc_addr_) {
            return;
        }
        const bool own_device = VulkanDeviceOriginOf(VulkanSessionDevice(create_info)) == VulkanDeviceOrigin::kGame;
        RequeryVulkanGraphicsRequirements(next_get_instance_proc_addr_, instance, create_info->systemId,
                                          vulkan_enable_version_ == 2 && vulkan_enable1_requested_ && own_device
                                              ? 1
                                              : vulkan_enable_version_);
    };
    if (!d3d11_bridge_.Active()) {
        requery_d3d11_requirements();
    }
    if (!vulkan_bridge_.Active()) {
        requery_vulkan_requirements();
    }
    XrResult result = next_create_session_(instance, downstream_info, session);
    if (XR_FAILED(result) && d3d11_bridge_.Active()) {
        logger_.Error("xrCreateSession refused the bridged Direct3D 12 session (result=" +
                      std::to_string(static_cast<int>(result)) +
                      "); creating it with the game's Direct3D 11 binding instead.");
        framegen_queue_.Reset();
        d3d11_bridge_.End();
        requery_d3d11_requirements();
        result = next_create_session_(instance, create_info, session);
    }
    if (XR_FAILED(result) && vulkan_bridge_.Active()) {
        logger_.Error("xrCreateSession refused the bridged Direct3D 12 session (result=" +
                      std::to_string(static_cast<int>(result)) + "); creating it with the game's Vulkan binding instead.");
        vulkan_unbridged = "the runtime refused the bridged Direct3D 12 session (result=" +
                           std::to_string(static_cast<int>(result)) + ")";
        framegen_queue_.Reset();
        vulkan_bridge_.End();
        requery_vulkan_requirements();
        result = next_create_session_(instance, create_info, session);
    }
    if (XR_FAILED(result) || !session) {
        if (XR_FAILED(result)) {
            logger_.Error("xrCreateSession failed downstream: result=" + std::to_string(static_cast<int>(result)));
        }
        framegen_queue_.Reset();
        d3d11_bridge_.End();
        vulkan_bridge_.End();
        return result;
    }
    if (d3d11_bridge_.Active()) {
        logger_.Info("Frame Generation's Direct3D 11 bridge: the session is Direct3D 12 and the game renders into "
                     "images shared with it (Varjo focus sharpening is off); " +
                     bridge_description);
    }
    if (vulkan_bridge_.Active()) {
        logger_.Info("Frame Generation's Vulkan bridge: the session is Direct3D 12 and the game renders into images "
                     "shared with it; " +
                     bridge_description);
    }
    // Frame Generation is unavailable for a Vulkan session the bridge does not take.
    std::string framegen_unavailable;
    if (vulkan_session && !vulkan_bridge_.Active()) {
        if (vulkan_unbridged.empty()) {
            vulkan_unbridged = framegen_wanted ? "the session is not bridged" : "Frame Generation was off";
        }
        if (framegen_wanted) {
            logger_.Info("Frame Generation is off for this Vulkan session: " + vulkan_unbridged + ".");
        }
        framegen_unavailable = "Vulkan sessions need the Vulkan bridge, and this one has none (" + vulkan_unbridged + ")";
    }
    logger_.Info("xrCreateSession succeeded downstream; configuring VectorXR session resources.");
    // The hardware of every graphics API in play, once per session: the
    // game's, and a bridge's Direct3D 12 device. Looked up before the lock,
    // as Direct3D 12 and Vulkan need a DXGI factory.
    const std::vector<SessionGraphics> session_graphics = QuerySessionGraphics(
        create_info ? create_info->next : nullptr,
        d3d11_bridge_.Active()    ? d3d11_bridge_.Binding()
        : vulkan_bridge_.Active() ? vulkan_bridge_.Binding()
                                  : nullptr,
        d3d11_bridge_.Active() ? "Direct3D 12 (Frame Generation's Direct3D 11 bridge)"
                               : "Direct3D 12 (Frame Generation's Vulkan bridge)");
    // The performance row's sampler reads the game's adapter (the first entry; a bridge's device is on the same
    // one) and Frame Generation's figures. Before the lock: it joins a previous session's sampler thread.
    performance_.BeginSession(
        session_graphics.empty() ? std::nullopt : session_graphics.front().adapter_luid,
        [this]() -> std::optional<PerformanceFrameGen> {
            if (!framegen_owns_pacing_.load(std::memory_order_acquire)) return std::nullopt;
            const FrameGenStatus status = framegen_.Status();
            PerformanceFrameGen figures;
            // Generating: its real frames each fill `multiplier` refreshes. Standing by, at full rate or not
            // engaged, frames pass straight through at the refresh rate.
            figures.generating = status.state == FrameGenState::kActive && !status.standby && !status.full_rate &&
                                 status.multiplier >= 2;
            figures.refreshes = status.multiplier;
            figures.refresh_ns = status.refresh_period_ns;
            figures.grid_ns = status.grid_period_ns;
            figures.app_frames = status.app_frames;
            figures.late_cpu = status.late_cpu;
            figures.late_gpu = status.late_gpu;
            figures.own_bytes = status.working_bytes + status.output_bytes;
            if (status.memory) {
                figures.usage_bytes = status.memory->usage_bytes;
                figures.budget_bytes = status.memory->budget_bytes;
            }
            return figures;
        },
        &logger_);

    std::scoped_lock lock(mutex_);
    for (const SessionGraphics& graphics : session_graphics) {
        logger_.Info(FormatSessionGraphics(graphics, runtime_name_, runtime_version_));
    }
    ResetSessionState();
    active_session_ = *session;
    d3d11_bridge_lost_logged_ = false;
    d3d11_bridge_depth_logged_ = false;
    d3d11_bridge_last_summary_ = std::chrono::steady_clock::now();
    d3d11_bridge_quad_frames_ = {};
    d3d11_bridge_quad_frames_summarized_ = {};
    d3d11_bridge_sharpen_logged_ = false;
    vulkan_bridge_lost_logged_ = false;
    vulkan_bridge_depth_logged_ = false;
    vulkan_bridge_motion_ = false;
    vulkan_bridge_last_summary_ = std::chrono::steady_clock::now();
    const auto* d3d11_binding = create_info
                                    ? static_cast<const XrGraphicsBindingD3D11KHR*>(
                                          FindStructInChain(create_info->next, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR))
                                    : nullptr;
    graphics_api_ = d3d11_binding && d3d11_binding->device ? "D3D11" : "Other";
    if (d3d11_binding && d3d11_binding->device) {
        d3d11_quadviews_compositor_.device = d3d11_binding->device;
        d3d11_quadviews_compositor_.device->AddRef();
        d3d11_quadviews_compositor_.device->GetImmediateContext(&d3d11_quadviews_compositor_.context);
        ID3D11Device1* device1 = nullptr;
        if (d3d11_quadviews_compositor_.context &&
            SUCCEEDED(d3d11_quadviews_compositor_.device->QueryInterface(
                __uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1))) &&
            SUCCEEDED(d3d11_quadviews_compositor_.context->QueryInterface(
                __uuidof(ID3D11DeviceContext1),
                reinterpret_cast<void**>(&d3d11_quadviews_compositor_.context1)))) {
            UINT context_flags = 0;
            if ((d3d11_quadviews_compositor_.device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
                context_flags |= D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
            }
            const D3D_FEATURE_LEVEL feature_level = d3d11_quadviews_compositor_.device->GetFeatureLevel();
            const HRESULT state_result = device1->CreateDeviceContextState(
                context_flags,
                &feature_level,
                1,
                D3D11_SDK_VERSION,
                __uuidof(ID3D11Device),
                nullptr,
                &d3d11_quadviews_compositor_.layer_context_state);
            if (FAILED(state_result)) {
                SafeRelease(d3d11_quadviews_compositor_.context1);
            }
        }
        SafeRelease(device1);
        logger_.Info("D3D11 device: " + DescribeD3D11Device(d3d11_quadviews_compositor_.device,
                                                             d3d11_quadviews_compositor_.context));
        logger_.Info("D3D11 graphics binding detected; native quadviews compositor is available.");
    } else {
        logger_.Info("D3D11 graphics binding not detected; synthesized quadviews is unavailable for this session.");
    }
    XrSystemProperties osd_system{XR_TYPE_SYSTEM_PROPERTIES};
    const bool osd_limits = create_info && next_get_system_properties_ &&
        XR_SUCCEEDED(next_get_system_properties_(instance, create_info->systemId, &osd_system));
    osd_.InitializeGraphics(*session,
        framegen_queue_.Active() ? framegen_queue_.RuntimeChain() : (create_info ? create_info->next : nullptr),
        osd_limits ? osd_system.graphicsProperties.maxLayerCount : 0,
        {next_enumerate_swapchain_formats_, next_create_swapchain_, next_enumerate_swapchain_images_,
         next_acquire_swapchain_image_, next_wait_swapchain_image_, next_release_swapchain_image_,
         next_destroy_swapchain_, next_create_reference_space_, next_destroy_space_, next_locate_space_}, &logger_,
        "application="+current_exe_name_+" runtime="+runtime_name_);
    graphics_api_=osd_.GraphicsApi();
    if (d3d11_bridge_.Active()) graphics_api_ = "D3D11-on-D3D12";
    if (vulkan_bridge_.Active()) graphics_api_ = "Vulkan-on-D3D12";
    osd_vulkan_.store(graphics_api_=="Vulkan");
    const XrResult internal_result = CreateInternalReferenceSpaces(*session);
    if (XR_FAILED(internal_result)) {
        logger_.Error("Failed to create one or more internal reference spaces; PivotXR will degrade for this session.");
        DestroyInternalReferenceSpaces();
    }
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    quadviews_session_active_ = resolved_settings_.core.enabled && resolved_settings_.quadviews.enabled;
    // A bridged session's Frame Generation is Direct3D 12, on the bridge's
    // device, capturing on the bridge's queue (a Vulkan game's is never touched).
    StartFrameGen(*session,
                  framegen_queue_.FrameGenChain(d3d11_bridge_.Active()    ? d3d11_bridge_.Binding()
                                                : vulkan_bridge_.Active() ? vulkan_bridge_.Binding()
                                                                          : (create_info ? create_info->next : nullptr)),
                  framegen_unavailable);
    ConfigureOsdAnchor();
    d3d11_bridge_.SessionCreated();
    vulkan_bridge_.SessionCreated();
    if (runtime_relay_root_.empty()) runtime_relay_root_ = ResolveRuntimeRelayRoot();
    if (external_status_path_.empty()) external_status_path_ = ResolveExternalStatusPath();
    runtime_relay_session_id_ = std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(RuntimeRelayUnixMilliseconds()) + "-" +
        std::to_string(++runtime_relay_session_sequence_);
    runtime_relay_exit_status_.Arm(RuntimeStatusPath(runtime_relay_root_, runtime_relay_session_id_));
    runtime_relay_exit_control_.Arm(RuntimeControlPath(runtime_relay_root_, runtime_relay_session_id_));
    external_status_exit_.Arm(external_status_path_, GetCurrentProcessId());
    quadviews_dimensions_.clear();
    quadviews_dimensions_at_ = 0;
    runtime_relay_last_applied_revision_ = 0;
    runtime_relay_acknowledged_revision_ = 0;
    runtime_relay_status_dirty_.store(true, std::memory_order_release);
    deferred_quadviews_config_active_.reset();
    if (IsQuadViewsEmulationActive() &&
        resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye) {
        const XrResult eye_gaze_result = CreateEyeGazeResources(*session);
        if (XR_FAILED(eye_gaze_result)) {
            logger_.Info("Eye gaze resources unavailable; quadviews will use head/static focus offsets.");
            DestroyEyeGazeResources();
        }
    }
    return result;
}

XrResult OpenXrLayer::DestroySession(XrSession session) {
    logger_.Info("xrDestroySession requested by application.");
    SetInputDeviceSessionActive(false);
    TeardownWatch watch(logger_, "xrDestroySession", framegen_.TeardownWait(),
                        [this] { return framegen_.PresenterActivity(); },
                        framegen_teardowns_watched_.load(std::memory_order_acquire));
    // Join any in-flight turbo async wait before tearing the session down.
    // The flight recorder keeps running until the runtime's session is gone:
    // a hang in Frame Generation's presenter teardown is what it is for.
    watch.Step("joining Turbo's in-flight wait");
    ResetTurboFrameState(false);
    // Its sampler thread reads Frame Generation's status: it stops before Frame Generation does.
    watch.Step("stopping the performance sampler");
    performance_.EndSession(); // stops the sampler (wanted again by PrepareOsd)
    performance_wanted_ = false;
    bool ending_active = false;
    {
        std::scoped_lock lock(mutex_);
        ending_active = session == active_session_;
        if (ending_active && vulkan_bridge_.Active()) {
            // Frame Generation's waits on the bridge's queue end first.
            watch.Step("letting the Vulkan bridge's last releases through");
            vulkan_bridge_.Escape();
        }
    }
    if (ending_active) {
        // Before the layer's lock: the presenter thread is stopped, let go by
        // the runtime, or left behind.
        StopFrameGenPresenterForTeardown(session, [&watch](const char* step) { watch.Step(step); });
    }
    {
        watch.Step("stopping Frame Generation and the layer's session resources");
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            // A bridged Vulkan session's last releases get through the
            // bridge's queue first (or are let through): Frame Generation's
            // waits on it end.
            if (vulkan_bridge_.Active()) {
                if (!framegen_.PresenterAbandoned()) {
                    SubmitKeptVulkanBridgeReleasesLocked();
                }
                vulkan_bridge_.Escape();
            }
            // Output swapchains and GPU work must go before the session does.
            framegen_.Stop();
            framegen_owns_pacing_.store(false, std::memory_order_release);
            framegen_session_enabled_ = false;
            turbo_yielded_to_framegen_ = false;
            // A presenter thread left inside the runtime may be submitting
            // the OSD's swapchain or using the layer's spaces: they stay, with
            // the runtime's session, for the process's exit (below).
            if (!framegen_.PresenterAbandoned()) {
                osd_.Shutdown();
                DestroyEyeGazeResources();
                DestroyVarjoNativeFoveationResources();
                DestroyInternalReferenceSpaces();
                // Releases a frame's end never handed over go on, uncopied:
                // Frame Generation is stopped and nothing shows them.
                HandOverBridgedReleasesLocked("session teardown", false);
                FlushDeferredSwapchainReleasesLocked("session teardown");
                ResetD3D11QuadViewsCompositor();
                ResetSwapchainState();
                // The bridge's copies into the runtime's images finish before
                // the runtime frees them with the session.
                d3d11_bridge_.Drain();
                vulkan_bridge_.Drain();
            }
        }
    }

    bool destroying_active = false;
    {
        std::scoped_lock lock(mutex_);
        destroying_active = session == active_session_;
    }
    if (destroying_active && framegen_.PresenterAbandoned()) {
        // The thread left behind may still be inside the runtime's session:
        // destroying it under the thread could crash the runtime. It is
        // kept, with what the layer holds of it, for the process's exit.
        PinModulesForAbandonedPresenter();
        logger_.Error("xrDestroySession: the runtime's session is kept for the process's exit (Frame Generation's "
                      "presenter thread never left the runtime).");
        turbo_trace_.Stop();
        return XR_SUCCESS;
    }
    watch.Step("in the runtime's xrDestroySession");
    const XrResult result = next_destroy_session_(session);
    turbo_trace_.Stop();
    watch.Step("ending the bridges");
    if (destroying_active && XR_SUCCEEDED(result)) {
        if (const std::string shown = framegen_queue_.ShownLayersSummary(); !shown.empty()) logger_.Info(shown);
        framegen_queue_.Reset();
        EndD3D11Bridge();
    }
    if (destroying_active) {
        // Whatever the runtime returned: the game may destroy its Vulkan
        // device as soon as the session is gone.
        EndVulkanBridge();
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) turbo_recovery_.Complete();
        if (session == active_session_) {
            ResetSwapchainState();
            ResetSessionState();
        }
    }

    return result;
}

XrResult OpenXrLayer::BeginSession(XrSession session, const XrSessionBeginInfo* begin_info) {
    framegen_.ResetFrames();
    bool quadviews_emulation_active = false;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        quadviews_emulation_active = IsQuadViewsEmulationActive();
    }

    XrSessionBeginInfo runtime_begin_info{};
    const XrSessionBeginInfo* downstream_begin_info = begin_info;
    // In Varjo compatible mode the runtime supports the quad configuration itself, so
    // begin the session on the app's real config and let the runtime drive the
    // focus panels. Only remap to stereo when we are emulating.
    if (begin_info && IsQuadViewConfiguration(begin_info->primaryViewConfigurationType) &&
        quadviews_emulation_active) {
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
    // In-game: a bound device that is not connected is tried once now, then
    // only when Windows reports a device's arrival or the app's Rescan
    // inputs asks; a timed retry stalled the game every 2 s.
    SetInputDeviceSessionActive(true);
    // Frame Generation's eye probe (research) locates the runtime's views in
    // the configuration its session was begun on (Varjo's own quad views
    // included).
    framegen_.SetRuntimeViewConfiguration(downstream_begin_info->primaryViewConfigurationType);

    std::scoped_lock lock(mutex_);
    if (varjo_native_foveation_diagnostic_.locate_calls > 0) {
        LogVarjoNativeFoveationSummaryLocked("session-rebegin", true);
        varjo_native_foveation_diagnostic_ = {};
    }
    active_session_ = session;
    session_begin_wall_time_ = std::chrono::steady_clock::now();
    osd_.ResetPresentation();
    osd_last_input_poll_.reset();
    ConfigureTurboExperiment();
    TurboRecoveryRecord recovery_identity;
    turbo_test_started_at_ = RuntimeRelayUnixMilliseconds();
    recovery_identity.application = current_exe_name_;
    recovery_identity.runtime = runtime_name_;
    recovery_identity.runtime_version = runtime_version_;
    recovery_identity.system_name = system_name_;
    recovery_identity.graphics_api = graphics_api_;
    recovery_identity.process_id = GetCurrentProcessId();
    recovery_identity.process_created = TurboProcessCreationTime();
    // Length-prefix fields to avoid ambiguous concatenations. Version the
    // policy separately from the release so updating VectorXR cannot silently
    // erase an interrupted run and replay it.
    recovery_identity.fingerprint = "turbo-recovery-v1";
    for (const auto& field : {current_exe_name_, runtime_name_, runtime_version_, system_name_,
                              std::to_string(system_vendor_id_), graphics_api_}) {
        recovery_identity.fingerprint += "|" + std::to_string(field.size()) + ":" + field;
    }
    turbo_recovery_.Configure(ResolveRuntimeRelayRoot() / "turbo-recovery", std::move(recovery_identity));
    turbo_recovery_blocked_.store(false);
    turbo_effective_active_.store(false);
    turbo_runtime_error_streak_.store(0);
    turbo_recovery_retry_requested_ = false;

    active_primary_view_configuration_type_ = begin_info->primaryViewConfigurationType;
    active_runtime_view_configuration_type_ = downstream_begin_info->primaryViewConfigurationType;
    has_active_primary_view_configuration_ = true;
    has_logged_quad_view_short_count_ = false;
    depth_view_info_pending_ = true;
    depth_submission_info_pending_ = true;
    depth_submission_info_not_before_time_.reset();

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
                std::string("Quadviews INACTIVE (application began a STEREO session; it did not select the "
                            "foveated-inset configuration). ") +
                std::string(QuadViewsInactiveReason(application_can_select_quad_views_,
                                                    quad_view_configuration_offered_)));
        }
    }
    return result;
}

XrResult OpenXrLayer::EndSession(XrSession session) {
    logger_.Info("xrEndSession requested by application.");
    TeardownWatch watch(logger_, "xrEndSession", framegen_.TeardownWait(),
                        [this] { return framegen_.PresenterActivity(); },
                        framegen_teardowns_watched_.load(std::memory_order_acquire));
    // A presenter thread a runtime holds inside a call (its waits once the
    // session is stopping, until it ends) is stopped again once the
    // runtime's session has ended below.
    watch.Step("stopping Frame Generation's presenter thread");
    const bool presenter_stopped = framegen_.ResetFrames();
    // Turbo deliberately pre-waits (and, in sequenced mode, pre-begins) the
    // following frame. Balance that owned frame before ending the session so a
    // later xrBeginSession starts from a clean runtime call-order state.
    watch.Step("releasing Turbo's pending wait");
    ReleaseTurboPendingWaitForSessionEnd(session);

    bool begin_pending_frame = false;
    bool end_pending_frame = false;
    XrTime display_time = 0;
    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    {
        std::scoped_lock lock(turbo_mutex_);
        const bool async_wait_succeeded = turbo_async_wait_.valid() && turbo_async_wait_completed_ &&
                                          XR_SUCCEEDED(turbo_async_wait_result_);
        // A begin probe is harmless when nothing is waited (-37) and closes a
        // waited frame whose earlier begin failed.
        begin_pending_frame = (async_wait_succeeded && !turbo_frame_begun_) || turbo_begin_owed_ ||
                              (turbo_begin_probe_required_ && !turbo_frame_begun_);
        end_pending_frame = turbo_frame_begun_ || begin_pending_frame;
        turbo_begin_probe_required_ = false;
        display_time = turbo_last_predicted_display_time_;
        blend_mode = turbo_last_environment_blend_mode_;
        turbo_begin_owed_ = false;
    }

    if (begin_pending_frame) {
        const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
        if (XR_FAILED(begin_result)) {
            logger_.Info("Session end: unable to balance Turbo's pending xrBeginFrame, result=" +
                         std::to_string(static_cast<int>(begin_result)) + ".");
            end_pending_frame = false;
        }
    }
    if (end_pending_frame) {
        XrFrameEndInfo empty_end{XR_TYPE_FRAME_END_INFO};
        empty_end.displayTime = display_time;
        empty_end.environmentBlendMode = blend_mode;
        empty_end.layerCount = 0;
        empty_end.layers = nullptr;
        const XrResult end_result = TraceRuntimeEndFrame(session, &empty_end);
        if (XR_FAILED(end_result)) {
            logger_.Info("Session end: unable to balance Turbo's pending empty frame, result=" +
                         std::to_string(static_cast<int>(end_result)) + ".");
        } else {
            std::scoped_lock lock(turbo_mutex_);
            turbo_frame_begun_ = false;
            turbo_async_wait_ = {};
            ++turbo_async_wait_generation_;
        }
    }

    watch.Step("in the runtime's xrEndSession");
    const XrResult result = next_end_session_(session);
    bool presenter_left_in_runtime = false;
    if (!presenter_stopped) {
        // The runtime's session has ended (or refused to): a runtime that
        // held the presenter thread lets it go now. One it still holds is
        // left behind at xrDestroySession.
        watch.Step("stopping Frame Generation's presenter thread after the runtime's xrEndSession");
        const bool stopped = framegen_.ResetFrames();
        presenter_left_in_runtime = !stopped;
        logger_.Info(std::string("xrEndSession: Frame Generation's presenter thread was still in the runtime; ") +
                     (stopped ? "it stopped once the runtime's session ended."
                              : "it is still there (" + framegen_.PresenterActivity() + ")."));
    }
    if (XR_FAILED(result)) {
        logger_.Error("xrEndSession failed downstream: result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }
    // Only once it ended: a refused xrEndSession (not stopping yet) leaves
    // the game playing, its absent devices still waiting for a rescan.
    SetInputDeviceSessionActive(false); // devices not connected retry on the reconnect timer again

    watch.Step("resetting Turbo's frame state");
    // A presenter thread still in the runtime is stopped or left behind at
    // xrDestroySession: the flight recorder keeps running until then.
    ResetTurboFrameState(!presenter_left_in_runtime);
    {
        std::scoped_lock lock(mutex_);
        turbo_recovery_.Complete();
        turbo_effective_active_.store(false);
        session_begin_wall_time_.reset();
        active_primary_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        active_runtime_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        has_active_primary_view_configuration_ = false;
        cached_eye_offset_poses_.clear();
        cached_eye_offsets_display_time_ = 0;
        cached_pivot_pose_deltas_.clear();
        logged_pivot_space_conversions_.clear();
        failed_pivot_space_conversions_.clear();
        cached_depth_submission_geometry_.clear();
        cached_quadviews_frames_.Clear();
        ResetEyeGazeInputState();
        quadviews_smoothed_focus_yaw_radians_ = 0.0;
        quadviews_smoothed_focus_pitch_radians_ = 0.0;
        quadviews_last_focus_smoothing_wall_time_.reset();
        quadviews_last_valid_gaze_wall_time_.reset();
        quadviews_eye_gaze_loss_started_wall_time_.reset();
        quadviews_eye_gaze_loss_was_locate_failure_ = false;
        quadviews_has_seen_valid_gaze_ = false;
        quadviews_compositor_recovery_.Reset();
        ResetPivotActivationState();
        ResetDepthToggleState();
        ResetTurboToggleState();
    }
    logger_.Info("OpenXR session ended; VectorXR frame and tracking state reset for restart.");
    return result;
}

XrResult OpenXrLayer::AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attach_info) {
    if (!attach_info) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    XrSessionActionSetsAttachInfo downstream_attach_info = *attach_info;
    DelayedActionSetAttachment<XrActionSet>::Attempt attachment_attempt;
    bool attachment_attempt_prepared = false;
    bool appended_eye_gaze_set = false;
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_ &&
            (attach_info->countActionSets == 0 || attach_info->actionSets)) {
            std::span<const XrActionSet> application_action_sets;
            if (attach_info->countActionSets > 0 && attach_info->actionSets) {
                application_action_sets =
                    std::span<const XrActionSet>(attach_info->actionSets, attach_info->countActionSets);
            }
            if (!eye_gaze_action_set_attachment_.SessionAttachmentCompleted()) {
                FinalizeEyeGazeBindings();
            }
            attachment_attempt =
                eye_gaze_action_set_attachment_.PrepareApplicationAttachment(application_action_sets);
            attachment_attempt_prepared = true;
            appended_eye_gaze_set = attachment_attempt.includes_private_action_set &&
                                    attachment_attempt.action_sets.size() > attach_info->countActionSets;
            downstream_attach_info.countActionSets =
                static_cast<uint32_t>(attachment_attempt.action_sets.size());
            downstream_attach_info.actionSets = attachment_attempt.action_sets.empty()
                                                     ? nullptr
                                                     : attachment_attempt.action_sets.data();
        }
    }

    logger_.Info("xrAttachSessionActionSets requested by application: appActionSets=" +
                 std::to_string(attach_info->countActionSets) +
                 ", appendedEyeGazeSet=" + (appended_eye_gaze_set ? "1" : "0"));

    const XrResult result = next_attach_session_action_sets_(session, &downstream_attach_info);
    {
        std::scoped_lock lock(mutex_);
        if (attachment_attempt_prepared && session == active_session_) {
            eye_gaze_action_set_attachment_.CompleteAttachment(attachment_attempt, XR_SUCCEEDED(result));
        }
        if (XR_SUCCEEDED(result) && attach_info->actionSets) {
            application_attached_action_sets_.insert(attach_info->actionSets,
                                                     attach_info->actionSets + attach_info->countActionSets);
        }
    }
    if (XR_FAILED(result)) {
        logger_.Error("xrAttachSessionActionSets failed downstream: result=" +
                      std::to_string(static_cast<int>(result)));
        return result;
    }
    if (appended_eye_gaze_set) {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            pending_eye_gaze_diagnostics_ = std::max(pending_eye_gaze_diagnostics_, 20u);
            pending_eye_gaze_sync_diagnostics_ = std::max(pending_eye_gaze_sync_diagnostics_, 20u);
            logger_.Info("Attached VectorXR eye-gaze action set for quadviews.");
        }
    }
    return result;
}

void OpenXrLayer::TryAttachEyeGazeActionSetFallback(XrSession session) {
    std::optional<DelayedActionSetAttachment<XrActionSet>::Attempt> attachment_attempt;
    std::uint64_t completed_frames = 0;
    {
        std::scoped_lock lock(mutex_);
        if (session != active_session_ || !eye_gaze_resources_ready_ ||
            !next_attach_session_action_sets_) {
            return;
        }
        attachment_attempt = eye_gaze_action_set_attachment_.PrepareFallbackAttachment();
        if (attachment_attempt && !FinalizeEyeGazeBindings()) {
            return;
        }
        completed_frames = eye_gaze_action_set_attachment_.CompletedFrames();
    }
    if (!attachment_attempt.has_value()) {
        return;
    }

    XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach_info.countActionSets = static_cast<uint32_t>(attachment_attempt->action_sets.size());
    attach_info.actionSets = attachment_attempt->action_sets.data();
    const XrResult result = next_attach_session_action_sets_(session, &attach_info);
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            eye_gaze_action_set_attachment_.CompleteAttachment(*attachment_attempt, XR_SUCCEEDED(result));
            if (XR_SUCCEEDED(result)) {
                pending_eye_gaze_diagnostics_ = std::max(pending_eye_gaze_diagnostics_, 20u);
                pending_eye_gaze_sync_diagnostics_ = std::max(pending_eye_gaze_sync_diagnostics_, 20u);
            }
        }
    }
    if (XR_SUCCEEDED(result)) {
        logger_.Info("Attached VectorXR eye-gaze action set after the application did not use the OpenXR "
                     "action system for " + std::to_string(completed_frames) + " frames.");
    } else {
        logger_.Error("VectorXR eye-gaze fallback action-set attachment failed: result=" +
                      std::to_string(static_cast<int>(result)) +
                      "; quadviews will continue with head/static focus offsets.");
    }
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
        if (session == active_session_ && eye_gaze_resources_ready_ && eye_gaze_bindings_ready_ &&
            eye_gaze_action_set_attachment_.PrivateActionSetAttached() &&
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
    if (appended_eye_gaze_active_set || pending_eye_gaze_sync_diagnostics_ > 0) {
        std::scoped_lock lock(mutex_);
        // Only a sync that carried the private set says anything about gaze input.
        if (appended_eye_gaze_active_set && session == active_session_ && eye_gaze_resources_ready_ &&
            eye_gaze_bindings_ready_ && eye_gaze_action_set_attachment_.PrivateActionSetAttached()) {
            RecordEyeGazeSyncResult(true, result);
        }
        if (pending_eye_gaze_sync_diagnostics_ > 0 && session == active_session_ && eye_gaze_resources_ready_) {
            logger_.Debug("Quadviews eye-gaze action sync: result=" +
                          FormatHex(static_cast<uint64_t>(result)) +
                          ", appActionSets=" + std::to_string(sync_info->countActiveActionSets) +
                          ", submittedActionSets=" + std::to_string(submitted_action_set_count) +
                          ", appended=" + std::to_string(appended_eye_gaze_active_set) +
                          ", actionSetAttached=" +
                              std::to_string(eye_gaze_action_set_attachment_.PrivateActionSetAttached()));
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
    if (spacewarp_fb_owned_) {
        // The runtime left the structure as the application wrote it.
        auto* space_warp = static_cast<XrSystemSpaceWarpPropertiesFB*>(
            FindMutableStructInChain(properties->next, XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB));
        const MotionVectorSize size = SpaceWarpVectorSizeLocked();
        if (space_warp) {
            spacewarp_motion_vector_size_ = SpaceWarpMotionVectorSize(instance, system_id, size);
            space_warp->recommendedMotionVectorImageRectWidth =
                static_cast<uint32_t>(spacewarp_motion_vector_size_.width);
            space_warp->recommendedMotionVectorImageRectHeight =
                static_cast<uint32_t>(spacewarp_motion_vector_size_.height);
        }
        if (!spacewarp_properties_logged_) {
            spacewarp_properties_logged_ = true;
            logger_.Info(space_warp ? "Space-warp probe: XrSystemSpaceWarpPropertiesFB answered: "
                                      "recommendedMotionVectorImageRect=" +
                                          std::to_string(spacewarp_motion_vector_size_.width) + "x" +
                                          std::to_string(spacewarp_motion_vector_size_.height) + " (" +
                                          EyeShare(size) + " each way)."
                                    : std::string("Space-warp probe: xrGetSystemProperties without "
                                                  "XrSystemSpaceWarpPropertiesFB (its first call)."));
        }
    }
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    system_name_ = properties->systemName;
    system_vendor_id_ = properties->vendorId;

    void* foveated_properties =
        FindMutableStructInChain(properties->next, XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO);
    const bool app_queried_varjo_foveation = foveated_properties != nullptr;
    const bool runtime_reported_varjo_foveation =
        foveated_properties &&
        reinterpret_cast<const XrSystemFoveatedRenderingPropertiesVARJO*>(foveated_properties)
                ->supportsFoveatedRendering == XR_TRUE;
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
               << ", runtimeReportedVarjoFoveation=" << (runtime_reported_varjo_foveation ? 1 : 0)
               << ", vectorReturnedVarjoFoveation="
               << ((app_queried_varjo_foveation && IsQuadViewsActive()) || runtime_reported_varjo_foveation ? 1 : 0)
               << ", quadViewsActive=" << (IsQuadViewsActive() ? 1 : 0);
        logger_.Info(stream.str());
        has_logged_system_properties_ = true;
    }

    if (!IsQuadViewsActive()) {
        return result;
    }

    if (foveated_properties) {
        reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(foveated_properties)
            ->supportsFoveatedRendering = XR_TRUE;
        logger_.Info(varjo_compatible_quadviews_active_
                         ? "Native Varjo foveation system property: runtimeReportedSupports=" +
                               std::to_string(runtime_reported_varjo_foveation ? 1 : 0) +
                               ", vectorReturnedSupports=1."
                         : "Reported supportsFoveatedRendering=TRUE to application "
                           "(Varjo foveated-rendering emulation).");
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
        IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsEmulationActive()
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
    const bool synthesize_quad = IsQuadViewsEmulationActive() && runtime_has_stereo && !runtime_has_quad;
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
    quad_view_configuration_offered_ =
        std::find(exposed_types.begin(), exposed_types.end(),
                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET) != exposed_types.end();

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
    const bool synthesize_quad =
        IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsEmulationActive();
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
    const XrResult result = ForwardEnumerateViewConfigurationViews(
        instance, system_id, view_configuration_type, view_capacity_input, view_count_output, views);
    if (XR_FAILED(result) || !views || view_capacity_input == 0) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    if (!spacewarp_ext_owned_) {
        return result;
    }
    // XR_EXT_frame_synthesis recommends a motion vector size for each view.
    std::string answered;
    const MotionVectorSize share = SpaceWarpVectorSizeLocked();
    const uint32_t count = std::min(view_capacity_input, *view_count_output);
    for (uint32_t i = 0; i < count; ++i) {
        auto* config = static_cast<XrFrameSynthesisConfigViewEXT*>(
            FindMutableStructInChain(views[i].next, XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT));
        if (!config) {
            continue;
        }
        const XrExtent2Di size =
            RecommendedMotionVectorSize(views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight, share);
        config->recommendedMotionVectorImageRectWidth = static_cast<uint32_t>(size.width);
        config->recommendedMotionVectorImageRectHeight = static_cast<uint32_t>(size.height);
        spacewarp_motion_vector_size_ = size;
        answered += " view" + std::to_string(i) + "=" + std::to_string(size.width) + "x" + std::to_string(size.height);
    }
    if (!spacewarp_config_views_logged_) {
        spacewarp_config_views_logged_ = true;
        logger_.Info("Space-warp probe: xrEnumerateViewConfigurationViews(" +
                     std::string(ToString(view_configuration_type)) + ") " +
                     (answered.empty() ? std::string("without XrFrameSynthesisConfigViewEXT (its first call).")
                                       : "XrFrameSynthesisConfigViewEXT answered:" + answered + " (" + EyeShare(share) +
                                             " each way)."));
    }
    return result;
}

XrResult OpenXrLayer::ForwardEnumerateViewConfigurationViews(XrInstance instance,
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
        ++varjo_native_view_configuration_calls_;

        // DCS enables XR_VARJO_foveated_rendering but does not attach the
        // per-view request that tells the Varjo runtime to return the texture
        // sizes for dynamic foveation. When VectorXR eye tracking is enabled,
        // supply that missing request downstream without changing the app's
        // next chains. Respect an explicit app request, including inactive.
        const bool request_native_foveated_views =
            varjo_foveated_rendering_extension_requested_ &&
            resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye;
        std::vector<XrViewConfigurationView> downstream_views_storage;
        std::vector<XrFoveatedViewConfigurationViewVARJO> injected_foveated_requests;
        XrViewConfigurationView* downstream_views = views;
        uint64_t vector_foveated_request_injected_mask = 0;
        if (request_native_foveated_views && views && view_capacity_input > 0) {
            downstream_views_storage.assign(views, views + view_capacity_input);
            injected_foveated_requests.resize(view_capacity_input);
            for (uint32_t i = 0; i < view_capacity_input; ++i) {
                if (FindStructInChain(views[i].next,
                                      XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO)) {
                    continue;
                }
                XrFoveatedViewConfigurationViewVARJO& request = injected_foveated_requests[i];
                request = {XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO};
                request.next = downstream_views_storage[i].next;
                request.foveatedRenderingActive = XR_TRUE;
                downstream_views_storage[i].next = &request;
                if (i < 64) {
                    vector_foveated_request_injected_mask |= uint64_t{1} << i;
                }
            }
            downstream_views = downstream_views_storage.data();
        }
        const XrResult native_result = next_enumerate_view_configuration_views_(
            instance, system_id, view_configuration_type, view_capacity_input, view_count_output, downstream_views);
        if (XR_FAILED(native_result)) {
            return native_result;
        }
        if (!views || view_capacity_input == 0) {
            if (!has_logged_varjo_native_view_configuration_count_) {
                logger_.Info(
                    "Native Varjo resolution contract count query: enumerateCall=" +
                    std::to_string(varjo_native_view_configuration_calls_) +
                    ", runtimeViewCount=" + std::to_string(*view_count_output) +
                    ", appCapacity=" + std::to_string(view_capacity_input) +
                    ", appFoveatedTextureRequest=not-applicable(no per-view next chains). "
                    "A populated query will log the raw recommended/max dimensions and request state.");
                has_logged_varjo_native_view_configuration_count_ = true;
            }
            return native_result;
        }

        // Copy only the base structure fields returned by the runtime. The app
        // owns its next chains, so never expose pointers to our temporary
        // foveation request structures.
        if (!downstream_views_storage.empty()) {
            const uint32_t returned = std::min<uint32_t>(view_capacity_input, *view_count_output);
            for (uint32_t i = 0; i < returned; ++i) {
                void* app_next = views[i].next;
                views[i] = downstream_views_storage[i];
                views[i].next = app_next;
            }
        }
        const double peripheral_scale = resolved_settings_.quadviews.peripheral_scale;
        const double focus_scale = resolved_settings_.quadviews.focus_scale;
        const uint32_t applied = std::min<uint32_t>(view_capacity_input, *view_count_output);

        struct NativeViewDiagnostic {
            uint32_t runtime_recommended_width{0};
            uint32_t runtime_recommended_height{0};
            uint32_t runtime_max_width{0};
            uint32_t runtime_max_height{0};
            uint32_t recommended_sample_count{0};
            uint32_t max_sample_count{0};
            uint64_t vector_requested_width{0};
            uint64_t vector_requested_height{0};
            uint32_t final_width{0};
            uint32_t final_height{0};
            double requested_scale{1.0};
            bool foveated_request_present{false};
            bool foveated_request_active{false};
            bool vector_foveated_request_injected{false};
            bool clamped_to_runtime_max{false};
        };
        std::vector<NativeViewDiagnostic> diagnostics;
        diagnostics.reserve(applied);
        uint64_t foveated_request_present_mask = 0;
        uint64_t foveated_request_active_mask = 0;
        uint64_t runtime_max_clamp_mask = 0;
        for (uint32_t i = 0; i < applied; ++i) {
            const bool is_focus = (i >= 2);
            const double scale = is_focus ? focus_scale : peripheral_scale;
            NativeViewDiagnostic diagnostic;
            diagnostic.runtime_recommended_width = views[i].recommendedImageRectWidth;
            diagnostic.runtime_recommended_height = views[i].recommendedImageRectHeight;
            diagnostic.runtime_max_width = views[i].maxImageRectWidth;
            diagnostic.runtime_max_height = views[i].maxImageRectHeight;
            diagnostic.recommended_sample_count = views[i].recommendedSwapchainSampleCount;
            diagnostic.max_sample_count = views[i].maxSwapchainSampleCount;
            diagnostic.requested_scale = scale;
            diagnostic.vector_requested_width = RequestedScaledDimension(diagnostic.runtime_recommended_width, scale);
            diagnostic.vector_requested_height =
                RequestedScaledDimension(diagnostic.runtime_recommended_height, scale);

            const auto* foveated_request = reinterpret_cast<const XrFoveatedViewConfigurationViewVARJO*>(
                FindStructInChain(views[i].next, XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO));
            diagnostic.foveated_request_present = foveated_request != nullptr;
            diagnostic.foveated_request_active =
                foveated_request && foveated_request->foveatedRenderingActive == XR_TRUE;
            diagnostic.vector_foveated_request_injected =
                i < 64 && (vector_foveated_request_injected_mask & (uint64_t{1} << i)) != 0;
            if (i < 64 && diagnostic.foveated_request_present) {
                foveated_request_present_mask |= uint64_t{1} << i;
            }
            if (i < 64 && diagnostic.foveated_request_active) {
                foveated_request_active_mask |= uint64_t{1} << i;
            }

            views[i].recommendedImageRectWidth =
                ScaleDimension(diagnostic.runtime_recommended_width, scale, diagnostic.runtime_max_width);
            views[i].recommendedImageRectHeight =
                ScaleDimension(diagnostic.runtime_recommended_height, scale, diagnostic.runtime_max_height);
            diagnostic.final_width = views[i].recommendedImageRectWidth;
            diagnostic.final_height = views[i].recommendedImageRectHeight;
            diagnostic.clamped_to_runtime_max =
                diagnostic.final_width < diagnostic.vector_requested_width ||
                diagnostic.final_height < diagnostic.vector_requested_height;
            if (i < 64 && diagnostic.clamped_to_runtime_max) {
                runtime_max_clamp_mask |= uint64_t{1} << i;
            }
            diagnostics.push_back(diagnostic);
        }
        std::ostringstream signature;
        signature << applied << ":" << FormatDiagnosticDouble(peripheral_scale) << ":"
                  << FormatDiagnosticDouble(focus_scale) << ":" << foveated_request_present_mask << ":"
                  << foveated_request_active_mask << ":" << vector_foveated_request_injected_mask;
        for (const NativeViewDiagnostic& diagnostic : diagnostics) {
            signature << ":" << diagnostic.runtime_recommended_width << "x"
                      << diagnostic.runtime_recommended_height << "/" << diagnostic.runtime_max_width << "x"
                      << diagnostic.runtime_max_height << "@" << diagnostic.recommended_sample_count << "/"
                      << diagnostic.max_sample_count << "->" << diagnostic.final_width << "x"
                      << diagnostic.final_height;
        }

        constexpr size_t kMaxLoggedVarjoViewConfigurationSignatures = 16;
        bool should_log_contract = false;
        const std::string signature_value = signature.str();
        if (!logged_varjo_native_view_configuration_signatures_.contains(signature_value)) {
            if (logged_varjo_native_view_configuration_signatures_.size() <
                kMaxLoggedVarjoViewConfigurationSignatures) {
                logged_varjo_native_view_configuration_signatures_.insert(signature_value);
                should_log_contract = true;
            } else if (!has_logged_varjo_native_view_configuration_signature_limit_) {
                logger_.Info(
                    "Native Varjo resolution contract logging reached its 16-signature safety limit; "
                    "further unique enumeration results will still be applied but not logged.");
                has_logged_varjo_native_view_configuration_signature_limit_ = true;
            }
        }
        if (should_log_contract && applied > 0) {
            std::ostringstream stream;
            stream << "Native Varjo resolution contract: enumerateCall=" << varjo_native_view_configuration_calls_
                   << ", runtimeViewCount=" << *view_count_output
                   << ", appCapacity=" << view_capacity_input
                   << ", appFoveatedTextureRequestPresentMask=" << FormatHex(foveated_request_present_mask)
                   << ", appFoveatedTextureRequestActiveMask=" << FormatHex(foveated_request_active_mask)
                   << ", vectorFoveatedTextureRequestInjectedMask="
                   << FormatHex(vector_foveated_request_injected_mask)
                   << ", vectorPeripheralScale=" << FormatDiagnosticDouble(peripheral_scale)
                   << ", vectorFocusScale=" << FormatDiagnosticDouble(focus_scale)
                   << ", runtimeMaxClampMask=" << FormatHex(runtime_max_clamp_mask);
            for (uint32_t i = 0; i < diagnostics.size(); ++i) {
                const NativeViewDiagnostic& diagnostic = diagnostics[i];
                const double effective_width_scale =
                    diagnostic.runtime_recommended_width > 0
                        ? static_cast<double>(diagnostic.final_width) / diagnostic.runtime_recommended_width
                        : 0.0;
                const double effective_height_scale =
                    diagnostic.runtime_recommended_height > 0
                        ? static_cast<double>(diagnostic.final_height) / diagnostic.runtime_recommended_height
                        : 0.0;
                const char* foveated_state = diagnostic.foveated_request_active
                                                  ? "active"
                                                  : (diagnostic.foveated_request_present ? "inactive" : "absent");
                stream << ", view" << i << "{role=" << VarjoNativeViewRole(i)
                       << ", runtimeRecommended=" << diagnostic.runtime_recommended_width << "x"
                       << diagnostic.runtime_recommended_height
                       << ", runtimeMax=" << diagnostic.runtime_max_width << "x" << diagnostic.runtime_max_height
                       << ", runtimeSamples=" << diagnostic.recommended_sample_count << "/"
                       << diagnostic.max_sample_count
                       << ", foveatedRequest=" << foveated_state
                       << ", vectorFoveatedRequestInjected="
                       << (diagnostic.vector_foveated_request_injected ? 1 : 0)
                       << ", vectorRequested=" << diagnostic.vector_requested_width << "x"
                       << diagnostic.vector_requested_height << "@x"
                       << FormatDiagnosticDouble(diagnostic.requested_scale)
                       << ", appFinal=" << diagnostic.final_width << "x" << diagnostic.final_height
                       << ", effectiveScale=" << FormatDiagnosticDouble(effective_width_scale) << "x"
                       << FormatDiagnosticDouble(effective_height_scale)
                       << ", recommendedAtRuntimeMax="
                       << ((diagnostic.runtime_recommended_width >= diagnostic.runtime_max_width ||
                            diagnostic.runtime_recommended_height >= diagnostic.runtime_max_height)
                               ? 1
                               : 0)
                       << ", clampedToRuntimeMax=" << (diagnostic.clamped_to_runtime_max ? 1 : 0) << "}";
            }
            stream << ". runtimeMax is the per-view limit advertised by the active Varjo runtime; comparing this "
                      "line after a Varjo Base resolution change and application restart distinguishes a "
                      "setting-controlled cap from a fixed runtime limit.";
            logger_.Info(stream.str());
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

XrResult OpenXrLayer::GetVisibilityMaskKHR(XrSession session,
                                           XrViewConfigurationType view_configuration_type,
                                           uint32_t view_index,
                                           XrVisibilityMaskTypeKHR visibility_mask_type,
                                           XrVisibilityMaskKHR* visibility_mask) {
    std::scoped_lock lock(mutex_);
    if (!next_get_visibility_mask_khr_) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }

    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    const bool synthesize_quad = IsQuadViewConfiguration(view_configuration_type) && IsQuadViewsActive() &&
                                 !varjo_compatible_quadviews_active_;
    const XrViewConfigurationType runtime_type =
        synthesize_quad ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO : view_configuration_type;
    // Synthetic quadviews use [left peripheral, right peripheral, left focus,
    // right focus]. The underlying stereo runtime only accepts view indices 0/1.
    const uint32_t runtime_view_index = synthesize_quad ? view_index % 2 : view_index;

    if (synthesize_quad && !has_logged_visibility_mask_mapping_) {
        logger_.Info("Mapped xrGetVisibilityMaskKHR from synthesized quadviews to the runtime's stereo "
                     "view configuration (appViewIndex=" +
                     std::to_string(view_index) + ", runtimeViewIndex=" +
                     std::to_string(runtime_view_index) + ").");
        has_logged_visibility_mask_mapping_ = true;
    }
    const uint32_t shape_bit = view_index < 4 && visibility_mask_type >= 1 && visibility_mask_type <= 3
                                   ? 1u << (view_index * 4 + static_cast<uint32_t>(visibility_mask_type))
                                   : 0u;
    if (synthesize_quad && view_index >= 2 && view_index < 4) {
        // A focus view lies inside its eye's view. Its eye's mask (the lens
        // edge, in view-space tangents) drawn through the focus view's own
        // projection, as specified, hides nothing of it until the focus view
        // reaches the lens edge, and then only pixels the lens hides anyway.
        // So it is given a mask that hides nothing: the same picture for a
        // game that applies masks as specified, and no never-drawn region for
        // one that applies them otherwise (in the view's own normalized
        // coordinates, say). A precaution: no game is known to do that, and
        // this mapping (the eye's mask, unchanged since 0.17) is not a
        // confirmed cause of artifacts around the inset.
        const XrResult result = FocusViewVisibilityMask(visibility_mask_type, visibility_mask);
        if (XR_SUCCEEDED(result) && visibility_mask && visibility_mask->vertexCapacityInput > 0 && shape_bit != 0 &&
            (logged_visibility_mask_shapes_ & shape_bit) == 0) {
            logged_visibility_mask_shapes_ |= shape_bit;
            logger_.Info("Visibility mask for synthesized quadviews: appViewIndex=" + std::to_string(view_index) +
                         ", type=" + std::to_string(static_cast<int>(visibility_mask_type)) +
                         ", a focus view: given a mask that hides nothing (not its eye's).");
        }
        return result;
    }

    const XrResult result = next_get_visibility_mask_khr_(
        session, runtime_type, runtime_view_index, visibility_mask_type, visibility_mask);
    if (XR_FAILED(result)) {
        logger_.Error("xrGetVisibilityMaskKHR failed downstream: result=" +
                      std::to_string(static_cast<int>(result)) + ", appViewConfig=" +
                      ToString(view_configuration_type) + ", runtimeViewConfig=" + ToString(runtime_type) +
                      ", appViewIndex=" + std::to_string(view_index) +
                      ", runtimeViewIndex=" + std::to_string(runtime_view_index));
    }
    // The eye's mask each synthesized peripheral view is given, once per view
    // and mask type: its bounds (view-space tangents at z=-1) show how far in
    // the lens edge reaches.
    if (synthesize_quad && XR_SUCCEEDED(result) && visibility_mask && visibility_mask->vertexCapacityInput > 0 &&
        visibility_mask->vertices && shape_bit != 0 && (logged_visibility_mask_shapes_ & shape_bit) == 0) {
        logged_visibility_mask_shapes_ |= shape_bit;
        const uint32_t vertex_count =
            std::min(visibility_mask->vertexCountOutput, visibility_mask->vertexCapacityInput);
        float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
        for (uint32_t i = 0; i < vertex_count; ++i) {
            const XrVector2f& vertex = visibility_mask->vertices[i];
            min_x = i == 0 ? vertex.x : std::min(min_x, vertex.x);
            max_x = i == 0 ? vertex.x : std::max(max_x, vertex.x);
            min_y = i == 0 ? vertex.y : std::min(min_y, vertex.y);
            max_y = i == 0 ? vertex.y : std::max(max_y, vertex.y);
        }
        const char* type_name = visibility_mask_type == XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR ? "hidden"
                                : visibility_mask_type == XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR
                                    ? "visible"
                                    : "lineLoop";
        logger_.Info("Visibility mask for synthesized quadviews: appViewIndex=" + std::to_string(view_index) +
                     ", runtimeViewIndex=" + std::to_string(runtime_view_index) + ", type=" + type_name +
                     ", vertices=" + std::to_string(visibility_mask->vertexCountOutput) +
                     ", indices=" + std::to_string(visibility_mask->indexCountOutput) +
                     ", tangentBounds=(" + FormatDiagnosticDouble(min_x) + ", " + FormatDiagnosticDouble(max_x) +
                     ", " + FormatDiagnosticDouble(min_y) + ", " + FormatDiagnosticDouble(max_y) + ")");
    }
    return result;
}

void OpenXrLayer::LogSwapchainFormatsOnChange(const std::string& line) {
    // Star Citizen asks before every swapchain it creates (about 250 times a
    // session): the list is logged when it changes.
    {
        std::scoped_lock lock(mutex_);
        if (line == last_logged_swapchain_formats_) {
            return;
        }
        last_logged_swapchain_formats_ = line;
    }
    logger_.Info(line);
}

XrResult OpenXrLayer::EnumerateSwapchainFormats(XrSession session,
                                                uint32_t format_capacity_input,
                                                uint32_t* format_count_output,
                                                int64_t* formats) {
    if (vulkan_bridge_.Active()) {
        // A bridged Vulkan session's runtime formats are Direct3D 12's: the
        // game is offered their VkFormats, those its device can share.
        std::uint32_t count = 0;
        XrResult runtime_result = next_enumerate_swapchain_formats_(session, 0, &count, nullptr);
        std::vector<int64_t> runtime_formats(count);
        if (XR_SUCCEEDED(runtime_result) && count) {
            runtime_result = next_enumerate_swapchain_formats_(session, count, &count, runtime_formats.data());
        }
        if (XR_FAILED(runtime_result)) {
            logger_.Error("xrEnumerateSwapchainFormats failed downstream: result=" +
                          std::to_string(static_cast<int>(runtime_result)) + " (Vulkan bridge)");
            return runtime_result;
        }
        runtime_formats.resize(std::min<std::size_t>(runtime_formats.size(), count));
        const std::vector<int64_t> game_formats = vulkan_bridge_.GameFormats(runtime_formats);
        if (!format_count_output) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *format_count_output = static_cast<uint32_t>(game_formats.size());
        if (format_capacity_input == 0) {
            return XR_SUCCESS;
        }
        if (format_capacity_input < game_formats.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        if (!formats) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::copy(game_formats.begin(), game_formats.end(), formats);
        std::ostringstream stream;
        stream << "xrEnumerateSwapchainFormats returned " << game_formats.size()
               << " Vulkan format(s) for the runtime's " << runtime_formats.size()
               << " Direct3D 12 one(s) (Vulkan bridge):";
        for (const int64_t format : game_formats) {
            stream << ' ' << format;
        }
        LogSwapchainFormatsOnChange(stream.str());
        return XR_SUCCESS;
    }
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
        LogSwapchainFormatsOnChange(stream.str());
    }
    return result;
}

XrResult OpenXrLayer::CreateSwapchain(XrSession session,
                                      const XrSwapchainCreateInfo* create_info,
                                      XrSwapchain* swapchain) {
    NoteThread(ThreadCensus::Call::CreateSwapchain);
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

    const XrSwapchainCreateInfo* const original_create_info = create_info;
    // A bridged session's runtime swapchains take the bridge's copies of the
    // game's images (D3D11Bridge, VulkanBridge).
    const bool vulkan_bridged = create_info && vulkan_bridge_.Active();
    const bool bridged = create_info && (d3d11_bridge_.Active() || vulkan_bridged);
    XrSwapchainCreateInfo bridged_create_info{};
    if (vulkan_bridged) {
        // What the bridge cannot share is refused before the runtime is asked.
        XrResult refusal = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        std::string reason;
        bool shareable = vulkan_bridge_.CheckSwapchain(*create_info, refusal, reason);
        if (shareable && !VulkanBridge::RuntimeCreateInfo(*create_info, bridged_create_info, reason)) {
            shareable = false;
            refusal = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        if (!shareable) {
            logger_.Error("Frame Generation's Vulkan bridge refuses a swapchain it cannot share (result=" +
                          std::to_string(static_cast<int>(refusal)) + "): " + reason);
            return refusal;
        }
        create_info = &bridged_create_info;
    } else if (bridged) {
        bridged_create_info = D3D11Bridge::RuntimeCreateInfo(*create_info);
        create_info = &bridged_create_info;
    }
    const XrSwapchainCreateInfo* const base_create_info = create_info;
    // Frame Generation copies released color images (Vulkan needs TRANSFER_SRC).
    XrSwapchainCreateInfo adjusted_create_info{};
    if (create_info && framegen_owns_pacing_.load(std::memory_order_acquire)) {
        const XrSwapchainUsageFlags extra = framegen_.SourceUsage(*create_info);
        if ((create_info->usageFlags & extra) != extra) {
            adjusted_create_info = *create_info;
            adjusted_create_info.usageFlags |= extra;
            logger_.Info("Frame Generation added swapchain usage " + FormatUsageFlags(extra) +
                         " so released images can be captured.");
            create_info = &adjusted_create_info;
        }
    }
    XrResult result = next_create_swapchain_(session, create_info, swapchain);
    if (XR_FAILED(result) && create_info == &adjusted_create_info) {
        // Without the copy usage, Frame Generation reports itself unavailable
        // rather than copying from this swapchain.
        create_info = base_create_info;
        logger_.Info("Runtime rejected the Frame Generation capture usage (result=" +
                     std::to_string(static_cast<int>(result)) + "); retrying with the application's usage.");
        result = next_create_swapchain_(session, create_info, swapchain);
    }
    if (XR_FAILED(result) && bridged &&
        ((bridged_create_info.usageFlags & ~original_create_info->usageFlags) & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
        // Direct3D 12 copies into an image need no usage of their own.
        bridged_create_info.usageFlags &= ~XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create_info = &bridged_create_info;
        logger_.Info("Runtime rejected the bridge's copy usage (result=" + std::to_string(static_cast<int>(result)) +
                     "); retrying without it.");
        result = next_create_swapchain_(session, create_info, swapchain);
    }
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
    if (vulkan_bridged) {
        std::string reason;
        if (!vulkan_bridge_.AddSwapchain(*swapchain, *original_create_info, next_enumerate_swapchain_images_,
                                         reason)) {
            logger_.Error("Frame Generation's Vulkan bridge refuses a swapchain it cannot share: " + reason);
            next_destroy_swapchain_(*swapchain);
            *swapchain = XR_NULL_HANDLE;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        logger_.Info("Frame Generation's Vulkan bridge: swapchain " + FormatHandle(*swapchain) + " is shared (" +
                     vulkan_bridge_.Describe(*swapchain) + ").");
    } else if (bridged) {
        std::string reason;
        if (!d3d11_bridge_.AddSwapchain(*swapchain, *original_create_info, next_enumerate_swapchain_images_,
                                        reason)) {
            logger_.Error("Frame Generation's Direct3D 11 bridge refuses a swapchain it cannot share: " + reason);
            next_destroy_swapchain_(*swapchain);
            *swapchain = XR_NULL_HANDLE;
            return XR_ERROR_RUNTIME_FAILURE;
        }
        logger_.Info("Frame Generation's Direct3D 11 bridge: swapchain " + FormatHandle(*swapchain) + " is shared (" +
                     d3d11_bridge_.Describe(*swapchain) + ").");
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
    framegen_.TrackSwapchain(*swapchain, create_info->format, create_info->sampleCount, create_info->usageFlags);
    if (vulkan_bridge_motion_) {
        // A new swapchain restarts the wait for a Vulkan-bridged game's
        // vectors to settle (Star Citizen's world entry): from its frame's
        // releases on, nothing is given back early until a frame's record
        // is read again (EndFrame marks them then).
        for (const auto& tracked : tracked_swapchains_) {
            vulkan_bridge_.UnmarkMotion(tracked.first);
        }
    }
    LogSwapchainSummary(*swapchain, info, "created");
    if (SpaceWarpProbeActive()) {
        NoteSpaceWarpSwapchainLocked(*swapchain, info);
    }
    return result;
}

XrResult OpenXrLayer::DestroySwapchain(XrSwapchain swapchain) {
    TeardownWatch watch(logger_, "xrDestroySwapchain", framegen_.TeardownWait(),
                        [this] { return framegen_.PresenterActivity(); },
                        framegen_teardowns_watched_.load(std::memory_order_acquire));
    // A pipelined turbo frame may still hold onto this swapchain; let the
    // async xrWaitFrame finish before the swapchain goes away. Bounded: an
    // interlocked runtime releases that wait only on the app's next submit,
    // which cannot happen while this call blocks the app's frame thread.
    watch.Step("draining Turbo's pending wait");
    constexpr std::chrono::milliseconds kSwapchainDrainTimeout{250};
    DrainTurboAsyncWait(kSwapchainDrainTimeout);
    {
        watch.Step("releasing the swapchain's deferred releases");
        std::scoped_lock lock(mutex_);
        auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            const XrResult handed = HandOverBridgedReleasesLocked(swapchain, it->second, "swapchain destroy");
            if (XR_FAILED(handed)) {
                return handed;
            }
            const XrResult flush_result =
                FlushDeferredSwapchainReleaseLocked(swapchain, it->second, "swapchain destroy");
            if (XR_FAILED(flush_result)) {
                return flush_result;
            }
        }
    }

    watch.Step("flushing Frame Generation's queued releases");
    framegen_queue_.Flush(swapchain, FrameGenQueueCalls());
    framegen_queue_.Forget(swapchain);
    // Frame Generation's GPU work may still read the swapchain's images (a
    // capture, or generation from them) when the GPU stopped finishing it:
    // then the runtime's swapchain is kept, never freed under that work. The
    // application's handle counts as destroyed; the runtime frees the
    // swapchain with its session.
    XrResult result = XR_SUCCESS;
    // So do the bridge's copies into the runtime's images.
    const bool vulkan_bridged = vulkan_bridge_.Bridged(swapchain);
    watch.Step("Frame Generation and the bridges letting the swapchain go");
    if (framegen_.ForgetSwapchain(swapchain) && d3d11_bridge_.RemoveSwapchain(swapchain) &&
        vulkan_bridge_.RemoveSwapchain(swapchain)) {
        watch.Step("in the runtime's xrDestroySwapchain");
        result = next_destroy_swapchain_(swapchain);
        if (XR_FAILED(result)) {
            return result;
        }
    } else {
        logger_.Error("Swapchain " + FormatHandle(swapchain) + " is kept until its session ends: GPU work on it " +
                      (vulkan_bridged ? "(Frame Generation's, or the Vulkan bridge's)"
                                      : "(Frame Generation's, or the Direct3D 11 bridge's)") +
                      " did not finish, or Frame Generation's presenter thread may still submit it.");
    }

    watch.Step("in the layer's bookkeeping");
    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it == tracked_swapchains_.end()) {
        logger_.Debug("Swapchain destroyed: handle=" + FormatHandle(swapchain) + ", tracked=false");
        return result;
    }

    LogSwapchainSummary(swapchain, it->second, "destroyed");
    for (CensusSwapchain& census : census_swapchains_) {
        if (census.handle == swapchain && !census.destroyed) {
            census.acquires = it->second.acquire_count;
            census.waits = it->second.wait_count;
            census.releases = it->second.release_count;
            census.destroyed = true;
        }
    }
    SafeReleaseVector(it->second.d3d11_shader_resources);
    tracked_swapchains_.erase(it);
    return result;
}

XrResult OpenXrLayer::EnumerateSwapchainImages(XrSwapchain swapchain,
                                               uint32_t image_capacity_input,
                                               uint32_t* image_count_output,
                                               XrSwapchainImageBaseHeader* images) {
    XrResult result = XR_SUCCESS;
    if (d3d11_bridge_.Bridged(swapchain)) {
        // A bridged swapchain's images are the bridge's Direct3D 11 ones; the
        // runtime's are Direct3D 12.
        const std::vector<ID3D11Texture2D*> textures = d3d11_bridge_.Images(swapchain);
        if (!image_count_output) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = static_cast<uint32_t>(textures.size());
        if (image_capacity_input > 0) {
            if (image_capacity_input < textures.size()) {
                return XR_ERROR_SIZE_INSUFFICIENT;
            }
            if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            auto* d3d11_images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (std::size_t i = 0; i < textures.size(); ++i) {
                d3d11_images[i].texture = textures[i];
            }
        }
    } else if (vulkan_bridge_.Bridged(swapchain)) {
        // A bridged Vulkan swapchain's images are the bridge's VkImages; the
        // runtime's are Direct3D 12.
        result = vulkan_bridge_.EnumerateImages(swapchain, image_capacity_input, image_count_output, images);
    } else {
        result = next_enumerate_swapchain_images_(swapchain, image_capacity_input, image_count_output, images);
    }
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
        it->second.d3d11_shader_resource_slices_attempted.clear();
        it->second.d3d11_shader_resource_slices_available.clear();
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
    const auto trace_call = turbo_trace_.Watch("app.acquireImage");
    NoteThread(ThreadCensus::Call::AcquireSwapchainImage);
    std::uint32_t image_count = 0;
    {
        std::scoped_lock lock(mutex_);
        auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            image_count = it->second.image_count;
            // Acquired again before the frame's end: the image released
            // before goes on first (the runtime hands out an image only while
            // the application holds fewer than all, and the bridge copies
            // the image acquired earliest).
            const XrResult handed = HandOverBridgedReleasesLocked(swapchain, it->second, "swapchain acquire");
            if (XR_FAILED(handed)) {
                return handed;
            }
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
    // Calls Frame Generation held back (FrameGenQueueSubstitution) go first,
    // as far as the runtime needs them to hand out an image.
    // A bridged swapchain whose runtime images never get anything (depth)
    // needs no ordering with the game's queue.
    const XrResult result = UnmirroredSwapchain(swapchain)
                                ? next_acquire_swapchain_image_(swapchain, acquire_info, index)
                                : framegen_queue_.Acquire(swapchain, image_count, FrameGenQueueCalls(), acquire_info,
                                                          index);
    if (diag) {
        logger_.Debug("Turbo-diag: xrAcquireSwapchainImage completed.");
    }
    if (XR_FAILED(result)) {
        return result;
    }
    if (index) {
        // A bridged image is rendered again only after the bridge's last copy of it.
        d3d11_bridge_.Acquired(swapchain, *index);
        vulkan_bridge_.Acquired(swapchain, *index);
        if (framegen_.TracesSwapchainCalls()) {
            framegen_.Trace().Record("acq", reinterpret_cast<uint64_t>(swapchain), *index);
        }
    }

    std::scoped_lock lock(mutex_);
    const auto it = tracked_swapchains_.find(swapchain);
    if (it != tracked_swapchains_.end()) {
        ++it->second.acquire_count;
        if (index) {
            it->second.last_acquired_image_index = *index;
            it->second.has_last_acquired_image_index = true;
            it->second.image_states.Acquire(*index);
            // The game's until its release, in a state of its own: Frame
            // Generation never reads a motion vector image it holds. A
            // bridged swapchain's runtime images are the bridge's.
            if (!BridgedSwapchain(swapchain)) {
                framegen_.NoteAcquire(swapchain, *index);
            }
        }
        if (it->second.quadviews_session && it->second.acquire_count <= 3) {
            LogSwapchainSummary(swapchain, it->second, "acquired");
        }
    }
    return result;
}

XrResult OpenXrLayer::WaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* wait_info) {
    const auto trace_call = turbo_trace_.Watch("app.waitImage");
    NoteThread(ThreadCensus::Call::WaitSwapchainImage);
    {
        // An image acquired ahead is waited for once the one before it is
        // released (OpenXR's order): a bridged release kept for the frame's
        // end goes on first, as at the next acquire.
        std::scoped_lock lock(mutex_);
        if (const auto it = tracked_swapchains_.find(swapchain);
            it != tracked_swapchains_.end() && !it->second.bridge_handovers.empty()) {
            if (const XrResult handed = HandOverBridgedReleasesLocked(swapchain, it->second, "swapchain wait");
                XR_FAILED(handed)) {
                return handed;
            }
        }
    }
    const bool diag = TurboSequencedDebugTick();
    if (diag) {
        logger_.Debug("Turbo-diag: xrWaitSwapchainImage(" +
                      std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + ") starting.");
    }
    const auto wait_start = std::chrono::steady_clock::now();
    // Anything the runtime queued for the image runs before the game's
    // rendering into it; held back with a release before it that is.
    const XrResult result = UnmirroredSwapchain(swapchain)
                                ? next_wait_swapchain_image_(swapchain, wait_info)
                                : framegen_queue_.Wait(swapchain, FrameGenLazy(swapchain), FrameGenQueueCalls(),
                                                       wait_info);
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
        it->second.image_states.WaitOldest();
    }
    return result;
}

XrResult OpenXrLayer::ReleaseSwapchainImage(XrSwapchain swapchain,
                                            const XrSwapchainImageReleaseInfo* release_info) {
    const auto trace_call = turbo_trace_.Watch("app.releaseImage");
    NoteThread(ThreadCensus::Call::ReleaseSwapchainImage);
    {
        std::scoped_lock lock(mutex_);
        const auto bridged = tracked_swapchains_.find(swapchain);
        if (bridged != tracked_swapchains_.end() && BridgedSwapchain(swapchain)) {
            // A bridged image is handed over at the frame's end, where a
            // runtime reads it (HandOverBridgedReleasesLocked): Star Citizen
            // releases its images before xrBeginFrame and renders into them
            // after, so a copy now had the image's previous frame (three
            // frames old with three images) shown with this frame's pose.
            if (const std::optional<uint32_t> released_index = bridged->second.image_states.ReleaseOldest()) {
                bridged->second.last_released_image_index = *released_index;
                bridged->second.has_last_released_image_index = true;
                bridged->second.bridge_handovers.push_back(*released_index);
                // The game may release on another thread than it ends frames
                // on: Direct3D 11's immediate context is then shared.
                d3d11_bridge_.ReleaseCalled();
                if (framegen_.TracesSwapchainCalls()) {
                    framegen_.Trace().Record("rel", reinterpret_cast<uint64_t>(swapchain), *released_index, 2);
                }
                return XR_SUCCESS;
            }
        }
        // A bridged image reaches the runtime's image by the bridge's copy,
        // queued ahead of Frame Generation's capture of it, and before the
        // release goes on below (MarkGameWork must come after the copy on the
        // bridge's queue, or the runtime may read the image before it). A
        // copy skipped (a stalled queue) leaves an older frame there: not
        // captured.
        const bool copied = d3d11_bridge_.Released(swapchain) && vulkan_bridge_.Released(swapchain);
        const auto it = tracked_swapchains_.find(swapchain);
        if (it != tracked_swapchains_.end()) {
            // Snapshot the released image index at the app's release moment —
            // the compositor must sample this frame's image even after the
            // (turbo-pipelined) app has acquired the next one.
            if (const std::optional<uint32_t> released_index = it->second.image_states.ReleaseOldest()) {
                it->second.last_released_image_index = *released_index;
                it->second.has_last_released_image_index = true;
                NoteFrameGenRelease(swapchain, *released_index);
                if (framegen_.TracesSwapchainCalls()) {
                    framegen_.Trace().Record("rel", reinterpret_cast<uint64_t>(swapchain), *released_index, copied);
                }
                // Before forwarding (or deferring) the release: the image is
                // still the application's and in its pre-release state.
                if (copied) {
                    framegen_.CaptureOnRelease(swapchain, *released_index);
                }
            }
            if (ShouldDeferSwapchainRelease(it->second)) {
                ++it->second.deferred_release_count;
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
    const XrResult result = ForwardAppRelease(swapchain, release_info);
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
        it->second.image_states.ReleaseDownstreamOldest();
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
        const char* source = QuadViewsD3D11ShaderSource();
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
        if (logger_.IsDebugEnabled()) {
            pending_quadviews_pixel_diagnostics_ =
                std::max<uint32_t>(pending_quadviews_pixel_diagnostics_, 2);
        }
        logger_.Info("D3D11 quadviews compositor initialized.");
    }

    auto release_render_resources = [](QuadViewsCompositionTarget& target) {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
    };

    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        const bool has_direct_targets =
            !target.d3d11_images.empty() &&
            target.image_render_target_views.size() == target.d3d11_images.size();
        const bool has_private_target = target.render_texture && target.render_target_view;
        if (target.swapchain != XR_NULL_HANDLE && target.width == output_width && target.height == output_height &&
            target.format == output_format && (has_direct_targets || has_private_target)) {
            continue;
        }

        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            release_render_resources(target);
            DestroyLayerSwapchain(target.swapchain);
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

        framegen_.TrackSwapchain(target.swapchain, output_format, 1, create_info.usageFlags);
        std::vector<ID3D11Texture2D*> textures;
        if (d3d11_bridge_.Active()) {
            // A bridged session's outputs are drawn into the bridge's images,
            // each copied into the runtime's at release.
            std::string reason;
            if (!d3d11_bridge_.AddSwapchain(target.swapchain, create_info, next_enumerate_swapchain_images_,
                                            reason)) {
                logger_.Error("D3D11 quadviews compositor output swapchain could not be shared by Frame "
                              "Generation's Direct3D 11 bridge: " + reason);
                DestroyLayerSwapchain(target.swapchain);
                target = {};
                d3d11_quadviews_compositor_.failed = true;
                return false;
            }
            textures = d3d11_bridge_.Images(target.swapchain);
        } else {
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
            for (const XrSwapchainImageD3D11KHR& image : images) {
                textures.push_back(image.texture);
            }
        }

        target.width = output_width;
        target.height = output_height;
        target.format = output_format;
        target.image_count = static_cast<uint32_t>(textures.size());
        target.d3d11_images = std::move(textures);

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

        if (!direct_render_targets_ready) {
            D3D11_TEXTURE2D_DESC render_desc{};
            render_desc.Width = output_width;
            render_desc.Height = output_height;
            render_desc.MipLevels = 1;
            render_desc.ArraySize = 1;
            render_desc.Format = static_cast<DXGI_FORMAT>(output_format);
            render_desc.SampleDesc.Count = 1;
            render_desc.Usage = D3D11_USAGE_DEFAULT;
            render_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            HRESULT hr =
                d3d11_quadviews_compositor_.device->CreateTexture2D(&render_desc, nullptr, &target.render_texture);
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
                    DestroyLayerSwapchain(target.swapchain);
                }
                target = {};
                d3d11_quadviews_compositor_.failed = true;
                return false;
            }
        }

        const uint32_t eye = static_cast<uint32_t>(
            &target - d3d11_quadviews_compositor_.targets.data());
        const uint32_t bytes_per_pixel = PixelProbeBytesPerPixel(static_cast<DXGI_FORMAT>(target.format));
        const bool has_private_render_target = target.render_texture != nullptr;
        const double approximate_mebibytes = bytes_per_pixel == 0
                                                ? 0.0
                                                : static_cast<double>(target.width) * target.height *
                                                      (target.image_count + (has_private_render_target ? 1 : 0)) *
                                                      bytes_per_pixel /
                                                      (1024.0 * 1024.0);
        std::ostringstream ready_stream;
        ready_stream << "D3D11 quadviews compositor output swapchain ready: eye=" << eye
                     << ", generation=" << d3d11_quadviews_compositor_.output_target_generation
                     << ", handle=" << FormatHandle(target.swapchain)
                     << ", usage=" << FormatUsageFlags(create_info.usageFlags)
                     << ", size=" << target.width << "x" << target.height
                     << ", format=" << target.format
                     << ", images=" << target.image_count
                     << ", directOutputRtvs=" << target.image_render_target_views.size()
                     << ", privateRenderTarget=" << has_private_render_target
                     << ", approximateRuntimePlusPrivateMiB="
                     << FormatDiagnosticDouble(approximate_mebibytes);
        for (uint32_t image = 0; image < target.d3d11_images.size(); ++image) {
            D3D11_TEXTURE2D_DESC desc{};
            if (target.d3d11_images[image]) {
                target.d3d11_images[image]->GetDesc(&desc);
            }
            ready_stream << ", runtimeTexture" << image << "="
                         << FormatHex(reinterpret_cast<uintptr_t>(target.d3d11_images[image]))
                         << " " << FormatTextureDesc(desc);
        }
        if (target.render_texture) {
            D3D11_TEXTURE2D_DESC private_desc{};
            target.render_texture->GetDesc(&private_desc);
            ready_stream << ", privateTexture="
                         << FormatHex(reinterpret_cast<uintptr_t>(target.render_texture))
                         << " " << FormatTextureDesc(private_desc);
        }
        if (d3d11_quadviews_compositor_.output_target_generation > 0) {
            logger_.Info(ready_stream.str());
        } else {
            logger_.Debug(ready_stream.str());
        }
    }

    return true;
}

bool OpenXrLayer::EnsureD3D11SwapchainShaderResources(SwapchainInfo& swapchain,
                                                       uint32_t array_slice) {
    if (!d3d11_quadviews_compositor_.device ||
        (swapchain.usage_flags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) == 0 ||
        swapchain.d3d11_images.empty() || swapchain.array_size != 1 || array_slice != 0) {
        // The compositor shader consumes Texture2D resources. Array slices use
        // the CopySubresourceRegion fast fallback below, which flattens only
        // the selected slice without copying the rest of the array.
        return false;
    }
    if (swapchain.d3d11_shader_resource_slices_attempted.contains(array_slice)) {
        return swapchain.d3d11_shader_resource_slices_available.contains(array_slice);
    }
    swapchain.d3d11_shader_resource_slices_attempted.insert(array_slice);

    const size_t array_size = std::max<uint32_t>(1, swapchain.array_size);
    const size_t required_slots = swapchain.d3d11_images.size() * array_size;
    if (swapchain.d3d11_shader_resources.size() != required_slots) {
        SafeReleaseVector(swapchain.d3d11_shader_resources);
        swapchain.d3d11_shader_resources.resize(required_slots, nullptr);
        swapchain.d3d11_shader_resource_slices_available.clear();
    }

    for (uint32_t image_index = 0; image_index < swapchain.d3d11_images.size(); ++image_index) {
        ID3D11Texture2D* texture = swapchain.d3d11_images[image_index];
        ID3D11ShaderResourceView* shader_resource = nullptr;
        HRESULT hr = CreateTextureShaderResourceView(d3d11_quadviews_compositor_.device,
                                                     texture,
                                                     swapchain.format,
                                                     array_slice,
                                                     &shader_resource);
        if (FAILED(hr)) {
            D3D11_TEXTURE2D_DESC texture_desc{};
            if (texture) {
                texture->GetDesc(&texture_desc);
            }
            SafeRelease(shader_resource);
            for (uint32_t release_image = 0; release_image < swapchain.d3d11_images.size(); ++release_image) {
                SafeRelease(swapchain.d3d11_shader_resources[release_image * array_size + array_slice]);
            }
            logger_.Info("D3D11 quadviews compositor direct input SRVs unavailable; using input copy path. "
                         "hr=" + FormatHex(static_cast<uint32_t>(hr)) +
                          ", failedImage=" + std::to_string(image_index) +
                          ", arraySlice=" + std::to_string(array_slice) +
                         ", format=" + std::to_string(swapchain.format) +
                         ", size=" + std::to_string(swapchain.width) + "x" + std::to_string(swapchain.height) +
                         ", usage=" + FormatUsageFlags(swapchain.usage_flags) +
                         ", " + FormatTextureDesc(texture_desc));
            return false;
        }
        swapchain.d3d11_shader_resources[image_index * array_size + array_slice] = shader_resource;
    }
    swapchain.d3d11_shader_resource_slices_available.insert(array_slice);
    logger_.Debug("D3D11 quadviews compositor direct input SRVs ready: size=" +
                  std::to_string(swapchain.width) + "x" + std::to_string(swapchain.height) +
                  ", images=" + std::to_string(swapchain.d3d11_images.size()) +
                  ", arraySlice=" + std::to_string(array_slice) +
                  ", format=" + std::to_string(swapchain.format));
    return true;
}

bool OpenXrLayer::SelectQuadViewsPrewarmInputs(std::array<SwapchainInfo*, 4>* inputs) {
    // The game's four views are colour images the compositor samples. Other
    // swapchains made in the quad views session are not among them: another
    // layer's (OpenKneeboard's are colour-only, 2048x2048 in BGRA) would set
    // the outputs' format, and a format the first composed frame does not
    // match reallocates the outputs (hundreds of MiB) at the session's start.
    uint32_t count = 0;
    for (auto& [swapchain_handle, swapchain] : tracked_swapchains_) {
        const XrSwapchainUsageFlags usage = swapchain.usage_flags;
        if (!swapchain.quadviews_session || swapchain.d3d11_images.empty() ||
            (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0 || (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) == 0 ||
            (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            continue;
        }
        if (count < inputs->size()) {
            (*inputs)[count] = &swapchain;
        }
        ++count;
    }
    return count >= inputs->size();
}

void OpenXrLayer::TryPrewarmD3D11QuadViewsCompositor() {
    if (!d3d11_quadviews_compositor_.device || !d3d11_quadviews_compositor_.context ||
        active_session_ == XR_NULL_HANDLE || cached_quadviews_stereo_recommended_width_ == 0 ||
        cached_quadviews_stereo_recommended_height_ == 0) {
        return;
    }

    std::array<SwapchainInfo*, 4> quad_swapchains{};
    if (!SelectQuadViewsPrewarmInputs(&quad_swapchains)) {
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

    const QuadViewsCanvasDimensions canvas_dimensions = ComputeQuadViewsCanvasDimensions(
        cached_quadviews_stereo_recommended_width_,
        cached_quadviews_stereo_recommended_height_,
        cached_quadviews_stereo_max_width_,
        cached_quadviews_stereo_max_height_,
        resolved_settings_.quadviews.focus_scale);
    const bool output_ready = EnsureD3D11QuadViewsCompositor(
        nullptr, canvas_dimensions.width, canvas_dimensions.height, largest_swapchain->format);
    if (output_ready && !d3d11_quadviews_compositor_.has_logged_prewarm) {
        const uint32_t direct_output_count =
            static_cast<uint32_t>(d3d11_quadviews_compositor_.targets[0].image_render_target_views.empty() ? 0 : 1) +
            static_cast<uint32_t>(d3d11_quadviews_compositor_.targets[1].image_render_target_views.empty() ? 0 : 1);
        std::uint64_t output_allocation_bytes = 0;
        std::uint32_t private_output_count = 0;
        for (const QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
            const std::uint32_t bytes_per_pixel =
                PixelProbeBytesPerPixel(static_cast<DXGI_FORMAT>(target.format));
            const std::uint64_t texture_count = target.d3d11_images.size() +
                                                (target.render_texture != nullptr ? 1u : 0u);
            output_allocation_bytes += static_cast<std::uint64_t>(target.width) * target.height *
                                       bytes_per_pixel * texture_count;
            private_output_count += target.render_texture != nullptr ? 1u : 0u;
        }
        logger_.Info("D3D11 quadviews compositor prewarmed: outputSize=" +
                     std::to_string(canvas_dimensions.width) + "x" +
                     std::to_string(canvas_dimensions.height) +
                     ", canvasDensity=" + FormatDiagnosticDouble(canvas_dimensions.density) +
                     ", directInputSwapchains=" + std::to_string(direct_input_count) + "/4" +
                     ", directOutputEyes=" + std::to_string(direct_output_count) + "/2" +
                     ", privateOutputEyes=" + std::to_string(private_output_count) + "/2" +
                     ", outputAllocationApproxMiB=" +
                     FormatDiagnosticDouble(static_cast<double>(output_allocation_bytes) /
                                            (1024.0 * 1024.0)) +
                     ", gpuTiming=" + std::to_string(d3d11_quadviews_compositor_.gpu_timing_available));
        d3d11_quadviews_compositor_.has_logged_prewarm = true;
    }
}

uint32_t OpenXrLayer::QuadViewsSourceImageIndex(const SwapchainInfo& swapchain) {
    // Sample the image the app last RELEASED (this frame's content); under
    // turbo pipelining last_acquired may already point at the next frame.
    return swapchain.has_last_released_image_index ? swapchain.last_released_image_index
                                                   : swapchain.last_acquired_image_index;
}

bool OpenXrLayer::FindQuadViewsSourceSwapchains(const XrCompositionLayerProjection* source_layer,
                                                std::array<SwapchainInfo*, 4>* swapchains) {
    if (!source_layer || source_layer->viewCount < 4 || !source_layer->views || !swapchains) {
        return false;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const auto it = tracked_swapchains_.find(source_layer->views[i].subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !(it->second.has_last_acquired_image_index || it->second.has_last_released_image_index) ||
            QuadViewsSourceImageIndex(it->second) >= it->second.d3d11_images.size() ||
            source_layer->views[i].subImage.imageArrayIndex >= std::max<uint32_t>(1, it->second.array_size)) {
            return false;
        }
        (*swapchains)[i] = &it->second;
    }
    return true;
}

bool OpenXrLayer::BuildQuadViewsComposePlan(const XrCompositionLayerProjection* source_layer,
                                            XrTime display_time,
                                            const XrPosef& reverse_delta,
                                            bool has_non_identity_delta,
                                            const DepthSubmissionGeometry* depth_geometry,
                                            QuadViewsComposePlan* plan) {
    std::array<SwapchainInfo*, 4> swapchains{};
    if (!plan || !FindQuadViewsSourceSwapchains(source_layer, &swapchains)) {
        return false;
    }

    const double peripheral_scale = resolved_settings_.quadviews.peripheral_scale;
    const double focus_width_scale = QuadViewsFocusWidthScale(resolved_settings_.quadviews);
    const double focus_height_scale = QuadViewsFocusHeightScale(resolved_settings_.quadviews);

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
    const QuadViewsCanvasDimensions canvas_dimensions = ComputeQuadViewsCanvasDimensions(
        stereo_full_width,
        stereo_full_height,
        cached_quadviews_stereo_max_width_,
        cached_quadviews_stereo_max_height_,
        resolved_settings_.quadviews.focus_scale);
    plan->width = canvas_dimensions.width;
    plan->height = canvas_dimensions.height;
    plan->density = canvas_dimensions.density;
    plan->format = swapchains[2]->format;

    QuadViewsFrameState cached_frame{};
    plan->matched_time = 0;
    plan->has_cached_fovs = FindQuadViewsFrame(display_time, &cached_frame, &plan->matched_time);
    plan->gaze = {};
    if (plan->has_cached_fovs) {
        plan->gaze = cached_frame.gaze;
    } else if (quadviews_raw_focus_valid_ && quadviews_raw_focus_time_ == display_time) {
        // A missing FOV cache is unusual, but retain an exact-time fallback for
        // startup frames instead of pairing the submission with a newer locate.
        plan->gaze.valid = true;
        plan->gaze.raw_yaw_radians = quadviews_raw_focus_yaw_radians_;
        plan->gaze.raw_pitch_radians = quadviews_raw_focus_pitch_radians_;
        plan->gaze.smoothed_yaw_radians = quadviews_smoothed_focus_yaw_radians_;
        plan->gaze.smoothed_pitch_radians = quadviews_smoothed_focus_pitch_radians_;
    }

    for (uint32_t view = 0; view < 4; ++view) {
        plan->fovs[view] = plan->has_cached_fovs ? cached_frame.fovs[view] : source_layer->views[view].fov;
        XrCompositionLayerProjectionView& source = plan->source_views[view];
        source = source_layer->views[view];
        if (has_non_identity_delta) {
            source.pose = MultiplyPoses(source.pose, reverse_delta);
        }
        source.fov = plan->fovs[view];
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        plan->eye_views[eye] = plan->source_views[eye];
        plan->eye_views[eye].subImage = {};
    }
    // Use the final submitted eye geometry, including Pivot/depth restoration,
    // so the diagnostic panel occupies the same rays as the independent quad.
    plan->restored = depth_geometry ? RestoreDepthSubmissionGeometry(
        std::span<XrCompositionLayerProjectionView>(plan->eye_views), 0, *depth_geometry,
        reverse_delta, has_non_identity_delta) : 0;

    // A diagnostic frame logs the focus rectangles below, and the compositor
    // its GPU timing and frame. Pending diagnostics count down per planned
    // frame, whoever composes it.
    plan->log_diagnostic = pending_quadviews_compositor_diagnostics_ > 0 ||
                           ShouldLogQuadViewsDebugHeartbeat(last_quadviews_compositor_debug_heartbeat_);
    if (plan->log_diagnostic && pending_quadviews_compositor_diagnostics_ > 0) {
        --pending_quadviews_compositor_diagnostics_;
    }

    plan->composition.output = FrameGenExtent{plan->width, plan->height};
    plan->composition.eyes.resize(2);
    std::array<double, 2> frame_step_px{};
    bool has_frame_step = false;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const XrSwapchainSubImage& focus_sub_image = source_layer->views[eye + 2].subImage;
        const uint32_t focus_content_width =
            static_cast<uint32_t>(std::max<int32_t>(1, focus_sub_image.imageRect.extent.width));
        const uint32_t focus_content_height =
            static_cast<uint32_t>(std::max<int32_t>(1, focus_sub_image.imageRect.extent.height));
        FrameGenComposeEye& composed = plan->composition.eyes[eye];
        composed = BuildFrameGenComposeEye(ToFrameGenFov(plan->fovs[eye]),
                                           ToFrameGenFov(plan->fovs[eye + 2]),
                                           plan->composition.output,
                                           FrameGenExtent{focus_content_width, focus_content_height},
                                           resolved_settings_.quadviews.transition_thickness_percent,
                                           resolved_settings_.quadviews.foveate_sharpness);
        composed.peripheral = eye;
        composed.focus = eye + 2;
        // The diagnostic overlay over the eye, drawn by whichever composes
        // it: the Direct3D 11 compositor, or Frame Generation for the frame
        // and every frame it generates from it.
        if (quadviews_diagnostic_visualization_enabled_) {
            const XrFovf& full_fov = plan->fovs[eye];
            const QuadViewsGazeDiagnostic& gaze = plan->gaze;
            FrameGenComposeOverlay& overlay = composed.overlay;
            overlay.shown = true;
            overlay.eye_tracking = resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye;
            overlay.gaze = overlay.eye_tracking && gaze.valid;
            const std::array<float, 2> raw_gaze_uv =
                ProjectViewAnglesToUv(full_fov, gaze.raw_yaw_radians, gaze.raw_pitch_radians);
            const std::array<float, 2> smoothed_gaze_uv =
                ProjectViewAnglesToUv(full_fov, gaze.smoothed_yaw_radians, gaze.smoothed_pitch_radians);
            const std::array<float, 2> head_center_uv = ProjectViewAnglesToUv(full_fov, 0.0, 0.0);
            const std::array<float, 2> configured_offset_uv = ProjectViewAnglesToUv(
                full_fov,
                DegreesToRadians(resolved_settings_.quadviews.horizontal_offset_degrees),
                DegreesToRadians(resolved_settings_.quadviews.vertical_offset_degrees));
            overlay.gaze_markers = {raw_gaze_uv[0], raw_gaze_uv[1], smoothed_gaze_uv[0], smoothed_gaze_uv[1]};
            overlay.reference_markers = {head_center_uv[0], head_center_uv[1], configured_offset_uv[0],
                                         configured_offset_uv[1]};
        }

        // Where the game's own submission would put the focus view, against
        // where the located FOVs put it: they differ only when the game drew
        // with FOVs other than the last ones located for the frame.
        const FrameGenComposeEye submitted =
            BuildFrameGenComposeEye(ToFrameGenFov(source_layer->views[eye].fov),
                                    ToFrameGenFov(source_layer->views[eye + 2].fov),
                                    plan->composition.output,
                                    FrameGenExtent{focus_content_width, focus_content_height},
                                    resolved_settings_.quadviews.transition_thickness_percent,
                                    resolved_settings_.quadviews.foveate_sharpness);
        double placement_delta_px = 0.0;
        for (uint32_t edge = 0; edge < 4; ++edge) {
            const double size = edge % 2 == 0 ? plan->width : plan->height;
            const double delta = static_cast<double>(submitted.focus_rect[edge]) - composed.focus_rect[edge];
            placement_delta_px = std::max(placement_delta_px, std::abs(delta) * size);
        }
        if (placement_delta_px > 1.0) {
            ++quadviews_placement_mismatch_frames_;
            if (quadviews_placement_mismatch_logs_remaining_ > 0) {
                --quadviews_placement_mismatch_logs_remaining_;
                logger_.Info("Quadviews focus placement differs from the game's submission: frameTime=" +
                             std::to_string(display_time) + ", eye=" + std::to_string(eye) +
                             ", deltaPx=" + FormatDiagnosticDouble(placement_delta_px) +
                             ", mismatchedFrames=" + std::to_string(quadviews_placement_mismatch_frames_) +
                             ", locatedFocusFov=" + FormatFov(plan->fovs[eye + 2]) +
                             ", submittedFocusFov=" + FormatFov(source_layer->views[eye + 2].fov) +
                             ", cachedFovHit=" + std::to_string(plan->has_cached_fovs));
            }
        }
        // The inset's frame-to-frame move, in output pixels: a moving inset
        // (eye tracking) shows here, the largest since the last diagnostic
        // line, and in the Info-level motion summary.
        if (quadviews_has_previous_focus_rect_[eye]) {
            for (uint32_t edge = 0; edge < 4; ++edge) {
                const double size = edge % 2 == 0 ? plan->width : plan->height;
                const double step =
                    static_cast<double>(composed.focus_rect[edge]) - quadviews_previous_focus_rect_[eye][edge];
                frame_step_px[eye] = std::max(frame_step_px[eye], std::abs(step) * size);
            }
            quadviews_focus_max_step_px_[eye] = std::max(quadviews_focus_max_step_px_[eye], frame_step_px[eye]);
            has_frame_step = true;
        }
        quadviews_previous_focus_rect_[eye] = composed.focus_rect;
        quadviews_has_previous_focus_rect_[eye] = true;
        if (!plan->log_diagnostic) {
            continue;
        }
        // Effective focus sampling ratio: focus texture pixels vs. the output pixels its
        // sub-rectangle occupies. ~1.0 = ideal 1:1; >1.0 = focus is being downsampled
        // (blur, the pre-fix behaviour); <1.0 = focus canvas is larger than needed.
        const double focus_rect_output_width =
            std::max(1.0, static_cast<double>(composed.focus_rect[2] - composed.focus_rect[0]) * plan->width);
        const double focus_rect_output_height =
            std::max(1.0, static_cast<double>(composed.focus_rect[3] - composed.focus_rect[1]) * plan->height);
        const double focus_sampling_ratio_x = focus_content_width / focus_rect_output_width;
        const double focus_sampling_ratio_y = focus_content_height / focus_rect_output_height;
        std::ostringstream stream;
        stream << "D3D11 quadviews compositor focus rect: frameTime=" << display_time
               << ", eye=" << eye
               << ", cachedFovHit=" << plan->has_cached_fovs
               << ", matchedTime=" << plan->matched_time
               << ", matchedDeltaNs=" << FormatMatchedDeltaNs(plan->matched_time, display_time)
               << ", rect=(" << FormatDiagnosticDouble(composed.focus_rect[0]) << ", "
               << FormatDiagnosticDouble(composed.focus_rect[1]) << ", "
               << FormatDiagnosticDouble(composed.focus_rect[2]) << ", "
               << FormatDiagnosticDouble(composed.focus_rect[3]) << ")"
               << ", focusTex=" << focus_content_width << "x" << focus_content_height
               << ", focusRectOutputPx=" << FormatDiagnosticDouble(focus_rect_output_width) << "x"
               << FormatDiagnosticDouble(focus_rect_output_height)
               << ", focusSamplingRatio=(" << FormatDiagnosticDouble(focus_sampling_ratio_x) << ", "
               << FormatDiagnosticDouble(focus_sampling_ratio_y) << ")"
               << ", feather=(" << FormatDiagnosticDouble(composed.feather[0]) << ", "
               << FormatDiagnosticDouble(composed.feather[1]) << ")"
               << ", sharpness=" << FormatDiagnosticDouble(composed.sharpen)
               << ", sharpenBackend=inlineAdaptive"
               << ", outputTexel=(" << FormatDiagnosticDouble(composed.output_pixel_in_focus[0]) << ", "
               << FormatDiagnosticDouble(composed.output_pixel_in_focus[1]) << ")"
               << ", focusTexel=(" << FormatDiagnosticDouble(composed.focus_texel[0]) << ", "
               << FormatDiagnosticDouble(composed.focus_texel[1]) << ")"
               << ", fullFov=" << FormatFov(plan->fovs[eye])
               << ", focusFov=" << FormatFov(plan->fovs[eye + 2])
               << ", sourceFullFov=" << FormatFov(source_layer->views[eye].fov)
               << ", sourceFocusFov=" << FormatFov(source_layer->views[eye + 2].fov)
               << ", maxStepPx=" << FormatDiagnosticDouble(quadviews_focus_max_step_px_[eye])
               << ", placementMismatchFrames=" << quadviews_placement_mismatch_frames_;
        logger_.Debug(stream.str());
        quadviews_focus_max_step_px_[eye] = 0.0;
    }

    QuadViewsInsetMotion& motion = quadviews_inset_motion_;
    if (has_frame_step) {
        const double step_px = std::max(frame_step_px[0], frame_step_px[1]);
        ++motion.frames;
        motion.moved_frames += step_px >= 1.0 ? 1 : 0;
        motion.gaze_frames += plan->gaze.valid ? 1 : 0;
        motion.step_px_sum += step_px;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            motion.max_step_px[eye] = std::max(motion.max_step_px[eye], frame_step_px[eye]);
        }
    }
    motion.output_width = plan->width;
    motion.output_height = plan->height;
    const auto now = std::chrono::steady_clock::now();
    if (!quadviews_inset_motion_since_.has_value()) {
        quadviews_inset_motion_since_ = now;
    } else if (now - *quadviews_inset_motion_since_ >= kQuadViewsInsetMotionSummaryInterval) {
        LogQuadViewsInsetMotion("periodic-30s", resolved_settings_.quadviews.tracking_mode);
    }
    return true;
}

void OpenXrLayer::LogQuadViewsInsetMotion(std::string_view reason, QuadViewsTrackingMode tracking_mode) {
    // Whether the inset moves every frame (Eye tracking mode, and how much
    // its gaze smoothing and deadzone hold it), at Info: the focus-rect
    // debug lines carry the same moves only with debug logging.
    const QuadViewsInsetMotion& motion = quadviews_inset_motion_;
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        quadviews_inset_motion_since_.has_value()
            ? std::chrono::duration<double>(now - *quadviews_inset_motion_since_).count()
            : 0.0;
    const double mean_step_px = motion.frames > 0 ? motion.step_px_sum / static_cast<double>(motion.frames) : 0.0;
    std::ostringstream stream;
    stream << "Quadviews focus inset motion: reason=" << reason
           << ", seconds=" << FormatDiagnosticDouble(seconds)
           << ", trackingMode=" << ToString(tracking_mode)
           << ", frames=" << motion.frames
           << ", gazeFrames=" << motion.gaze_frames
           << ", movedFrames=" << motion.moved_frames
           << ", meanStepPx=" << FormatDiagnosticDouble(mean_step_px)
           << ", maxStepPx=(" << FormatDiagnosticDouble(motion.max_step_px[0]) << ", "
           << FormatDiagnosticDouble(motion.max_step_px[1]) << ")"
           << ", outputSize=" << motion.output_width << "x" << motion.output_height
           << ", placementMismatchFrames=" << quadviews_placement_mismatch_frames_
           << ". A step is the inset's largest edge move from one frame to the next, in output pixels.";
    logger_.Info(stream.str());
    quadviews_inset_motion_ = {};
    quadviews_inset_motion_since_ = now;
}

bool OpenXrLayer::ComposeQuadViewsD3D11(const XrCompositionLayerProjection* source_layer,
                                        XrTime display_time,
                                        const QuadViewsComposePlan& plan,
                                        uint32_t* restored_view_count,
                                        XrCompositionLayerProjection* composed_layer,
                                        std::vector<XrCompositionLayerProjectionView>* composed_views) {
    const auto compose_start = std::chrono::steady_clock::now();
    std::array<SwapchainInfo*, 4> swapchains{};
    if (!composed_layer || !composed_views || !d3d11_quadviews_compositor_.device ||
        !d3d11_quadviews_compositor_.context || !FindQuadViewsSourceSwapchains(source_layer, &swapchains)) {
        return false;
    }

    const double canvas_density = plan.density;
    const uint32_t output_width = plan.width;
    const uint32_t output_height = plan.height;
    const int64_t output_format = plan.format;
    if (!EnsureD3D11QuadViewsCompositor(source_layer, output_width, output_height, output_format)) {
        return false;
    }

    composed_views->assign(plan.eye_views.begin(), plan.eye_views.end());
    for (uint32_t eye = 0; eye < 2; ++eye) {
        QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
        (*composed_views)[eye].subImage.swapchain = target.swapchain;
        (*composed_views)[eye].subImage.imageRect.offset = {0, 0};
        (*composed_views)[eye].subImage.imageRect.extent = {
            static_cast<int32_t>(target.width),
            static_cast<int32_t>(target.height),
        };
        (*composed_views)[eye].subImage.imageArrayIndex = 0;
    }
    // The plan's diagnostic overlay, unless Frame Generation captures the
    // composed images to generate frames from: it would measure the
    // overlay's motion.
    const bool overlay_withheld = std::any_of(plan.composition.eyes.begin(), plan.composition.eyes.end(),
                                              [](const FrameGenComposeEye& eye) { return eye.overlay.shown; }) &&
                                  QuadViewsOverlayWithheld();

    const uint32_t restored = plan.restored;
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
    ID3DDeviceContextState* application_context_state = nullptr;
    const bool use_context_state = d3d11_quadviews_compositor_.context1 &&
                                   d3d11_quadviews_compositor_.layer_context_state;
    if (use_context_state) {
        d3d11_quadviews_compositor_.context1->SwapDeviceContextState(
            d3d11_quadviews_compositor_.layer_context_state, &application_context_state);
        context->ClearState();
    } else {
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, &saved.depth_stencil);
        context->VSGetShader(&saved.vertex_shader, nullptr, nullptr);
        context->PSGetShader(&saved.pixel_shader, nullptr, nullptr);
        context->IAGetInputLayout(&saved.input_layout);
        context->IAGetPrimitiveTopology(&saved.topology);
        context->PSGetShaderResources(0, 2, saved.shader_resources);
        context->PSGetSamplers(0, 1, saved.samplers);
        context->PSGetConstantBuffers(0, 1, saved.constant_buffers);
        context->RSGetViewports(&saved.viewport_count, saved.viewports);
    }

    auto restore_state = [&]() {
        if (use_context_state) {
            d3d11_quadviews_compositor_.context1->SwapDeviceContextState(application_context_state, nullptr);
            SafeRelease(application_context_state);
            return;
        }
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
    std::array<uint32_t, 4> selected_source_indices{};
    std::array<ID3D11Texture2D*, 4> selected_source_textures{};
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

    PollD3D11QuadViewsPixelProbe();
    const bool should_issue_pixel_probe =
        pending_quadviews_pixel_diagnostics_ > 0 && logger_.IsDebugEnabled() &&
        output_width >= kQuadViewsPixelProbeSize && output_height >= kQuadViewsPixelProbeSize &&
        !d3d11_quadviews_compositor_.pixel_probe.issued &&
        EnsureD3D11QuadViewsPixelProbeResources(output_format);
    const bool should_log_compositor_diagnostic = plan.log_diagnostic;
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
        XrResult result = XR_SUCCESS;
        if (target.awaiting_wait) {
            // Acquired before, its wait timed out: waited for again.
            output_indices[eye] = target.awaiting_index;
        } else {
            result = AcquireLayerSwapchainImage(target.swapchain, target.image_count, &output_indices[eye]);
        }
        if (XR_FAILED(result) || output_indices[eye] >= target.d3d11_images.size()) {
            failure_reason = "outputAcquire eye=" + std::to_string(eye) +
                             ", result=" + FormatHex(static_cast<uint64_t>(result)) +
                             ", imageIndex=" + std::to_string(output_indices[eye]) +
                             ", images=" + std::to_string(target.d3d11_images.size());
            rendered = false;
            break;
        }
        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = kInternalSwapchainWaitTimeout;
        result = WaitLayerSwapchainImage(target.swapchain, &wait_info, plan.shown_later);
        if (result == XR_TIMEOUT_EXPIRED) {
            // The runtime has not handed the image back: it is neither drawn
            // into nor released, and is waited for again next frame.
            target.awaiting_wait = true;
            target.awaiting_index = output_indices[eye];
            failure_reason = "outputWait eye=" + std::to_string(eye) + " timed out";
            rendered = false;
            break;
        }
        target.awaiting_wait = false;
        if (XR_FAILED(result)) {
            failure_reason = "outputWait eye=" + std::to_string(eye) +
                             ", result=" + FormatHex(static_cast<uint64_t>(result));
            ReleaseLayerSwapchainImage(target.swapchain, output_indices[eye], false, plan.shown_later);
            rendered = false;
            break;
        }
        auto release_output_after_failure = [&]() {
            ReleaseLayerSwapchainImage(target.swapchain, output_indices[eye], false, plan.shown_later);
        };

        const uint32_t peripheral_index = QuadViewsSourceImageIndex(*swapchains[eye]);
        const uint32_t focus_index = QuadViewsSourceImageIndex(*swapchains[eye + 2]);
        ID3D11Texture2D* peripheral_source = swapchains[eye]->d3d11_images[peripheral_index];
        ID3D11Texture2D* focus_source = swapchains[eye + 2]->d3d11_images[focus_index];
        selected_source_indices[eye] = peripheral_index;
        selected_source_indices[eye + 2] = focus_index;
        selected_source_textures[eye] = peripheral_source;
        selected_source_textures[eye + 2] = focus_source;

        ID3D11ShaderResourceView* peripheral_resource = nullptr;
        ID3D11ShaderResourceView* focus_resource = nullptr;
        SwapchainInfo& peripheral_swapchain = *swapchains[eye];
        SwapchainInfo& focus_swapchain = *swapchains[eye + 2];
        const uint32_t peripheral_slice = source_layer->views[eye].subImage.imageArrayIndex;
        const uint32_t focus_slice = source_layer->views[eye + 2].subImage.imageArrayIndex;
        const size_t peripheral_slot =
            peripheral_index * std::max<uint32_t>(1, peripheral_swapchain.array_size) + peripheral_slice;
        const size_t focus_slot =
            focus_index * std::max<uint32_t>(1, focus_swapchain.array_size) + focus_slice;
        if (EnsureD3D11SwapchainShaderResources(peripheral_swapchain, peripheral_slice) &&
            peripheral_slot < peripheral_swapchain.d3d11_shader_resources.size()) {
            peripheral_resource = peripheral_swapchain.d3d11_shader_resources[peripheral_slot];
            ++direct_input_count;
        } else {
            if (!ensure_input_copy(eye, *swapchains[eye])) {
                release_output_after_failure();
                rendered = false;
                break;
            }
            const UINT source_subresource =
                D3D11CalcSubresource(0, peripheral_slice, std::max<uint32_t>(1, peripheral_swapchain.mip_count));
            context->CopySubresourceRegion(d3d11_quadviews_compositor_.input_copies[eye].texture,
                                           0,
                                           0,
                                           0,
                                           0,
                                           peripheral_source,
                                           source_subresource,
                                           nullptr);
            peripheral_resource = d3d11_quadviews_compositor_.input_copies[eye].shader_resource;
            ++input_copy_count;
        }
        if (EnsureD3D11SwapchainShaderResources(focus_swapchain, focus_slice) &&
            focus_slot < focus_swapchain.d3d11_shader_resources.size()) {
            focus_resource = focus_swapchain.d3d11_shader_resources[focus_slot];
            ++direct_input_count;
        } else {
            if (!ensure_input_copy(eye + 2, *swapchains[eye + 2])) {
                release_output_after_failure();
                rendered = false;
                break;
            }
            const UINT source_subresource =
                D3D11CalcSubresource(0, focus_slice, std::max<uint32_t>(1, focus_swapchain.mip_count));
            context->CopySubresourceRegion(d3d11_quadviews_compositor_.input_copies[eye + 2].texture,
                                           0,
                                           0,
                                           0,
                                           0,
                                           focus_source,
                                           source_subresource,
                                           nullptr);
            focus_resource = d3d11_quadviews_compositor_.input_copies[eye + 2].shader_resource;
            ++input_copy_count;
        }
        if (!peripheral_resource || !focus_resource) {
            failure_reason = "missingInputResource eye=" + std::to_string(eye);
            release_output_after_failure();
            rendered = false;
            break;
        }

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
        // ClearState gives the layer a known rasterizer/blend configuration, and the fullscreen
        // triangle then overwrites every output pixel. Avoid a redundant full-target clear on
        // that normal path; retain it on the legacy manual-state fallback where app rasterizer
        // or blend state may still be active.
        if (!use_context_state) {
            const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(render_target, clear_color);
        }
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

        const XrSwapchainSubImage& peripheral_sub_image = source_layer->views[eye].subImage;
        const XrSwapchainSubImage& focus_sub_image = source_layer->views[eye + 2].subImage;
        FocusRectConstants constants = FocusRectConstantsFor(plan.composition.eyes[eye]);
        if (overlay_withheld) {
            constants.diagnostic_params[0] = 0.0f;
        }
        constants.output_texel[0] = 1.0f / static_cast<float>(std::max<uint32_t>(1, target.width));
        constants.output_texel[1] = 1.0f / static_cast<float>(std::max<uint32_t>(1, target.height));

        const auto set_source_rect = [](float (&destination)[4],
                                        const XrSwapchainSubImage& sub_image,
                                        const SwapchainInfo& swapchain) {
            const float width = static_cast<float>(std::max<uint32_t>(1, swapchain.width));
            const float height = static_cast<float>(std::max<uint32_t>(1, swapchain.height));
            destination[0] = static_cast<float>(sub_image.imageRect.offset.x) / width;
            destination[1] = static_cast<float>(sub_image.imageRect.offset.y) / height;
            destination[2] = static_cast<float>(sub_image.imageRect.extent.width) / width;
            destination[3] = static_cast<float>(sub_image.imageRect.extent.height) / height;
        };
        set_source_rect(constants.peripheral_src_rect, peripheral_sub_image, peripheral_swapchain);
        set_source_rect(constants.focus_src_rect, focus_sub_image, focus_swapchain);
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

        if (should_issue_pixel_probe) {
            QuadViewsPixelProbe& probe = d3d11_quadviews_compositor_.pixel_probe;
            const uint32_t half_probe = kQuadViewsPixelProbeSize / 2;
            const uint32_t left = std::min(
                target.width - kQuadViewsPixelProbeSize,
                target.width / 2 > half_probe ? target.width / 2 - half_probe : 0u);
            const std::array<uint32_t, kQuadViewsPixelProbeBandsPerEye> center_y{
                target.height / 5,
                target.height / 2,
                target.height * 4 / 5,
            };
            for (uint32_t band = 0; band < kQuadViewsPixelProbeBandsPerEye; ++band) {
                const uint32_t top = std::min(
                    target.height - kQuadViewsPixelProbeSize,
                    center_y[band] > half_probe ? center_y[band] - half_probe : 0u);
                const D3D11_BOX source_box{
                    left,
                    top,
                    0,
                    left + kQuadViewsPixelProbeSize,
                    top + kQuadViewsPixelProbeSize,
                    1,
                };
                const uint32_t probe_index = eye * kQuadViewsPixelProbeBandsPerEye + band;
                context->CopySubresourceRegion(probe.staging_textures[probe_index], 0, 0, 0, 0,
                                               target.d3d11_images[output_indices[eye]], 0, &source_box);
            }
        }

        result = ReleaseLayerSwapchainImage(target.swapchain, output_indices[eye], true, plan.shown_later);
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
    if (should_issue_pixel_probe && rendered) {
        QuadViewsPixelProbe& probe = d3d11_quadviews_compositor_.pixel_probe;
        context->End(probe.completion);
        probe.issued = true;
        probe.frame_time = display_time;
        probe.target_generation = d3d11_quadviews_compositor_.output_target_generation;
        probe.output_image_indices = output_indices;
        --pending_quadviews_pixel_diagnostics_;
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
        // What the game's four views are drawn into, as the device sees them,
        // and the part of each the compositor samples.
        std::ostringstream sources;
        for (uint32_t view = 0; view < 4; ++view) {
            const XrSwapchainSubImage& sub_image = source_layer->views[view].subImage;
            D3D11_TEXTURE2D_DESC desc{};
            if (selected_source_textures[view]) {
                selected_source_textures[view]->GetDesc(&desc);
            }
            sources << ", source" << view << "{created=" << swapchains[view]->width << "x" << swapchains[view]->height
                    << ",format=" << swapchains[view]->format << ",rect=(" << sub_image.imageRect.offset.x << ","
                    << sub_image.imageRect.offset.y << "," << sub_image.imageRect.extent.width << "x"
                    << sub_image.imageRect.extent.height << "),slice=" << sub_image.imageArrayIndex << ","
                    << FormatTextureDesc(desc) << "}";
        }
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
                     ", bridged=" + std::to_string(d3d11_bridge_.Active()) +
                     ", appPixelBudget=" + FormatDiagnosticDouble(
                         (static_cast<double>(swapchains[0]->width) * swapchains[0]->height +
                          static_cast<double>(swapchains[2]->width) * swapchains[2]->height) /
                         std::max(1.0, static_cast<double>(output_width) * output_height) * 100.0) +
                     "%" + sources.str());
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
               << "%"
               << ", targetGeneration=" << d3d11_quadviews_compositor_.output_target_generation
               << ", deviceRemovedReason="
               << FormatHex(static_cast<uint32_t>(d3d11_quadviews_compositor_.device->GetDeviceRemovedReason()));
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
            stream << ", output" << eye
                   << "{swapchain=" << FormatHandle(target.swapchain)
                   << ",image=" << output_indices[eye]
                   << ",texture="
                   << FormatHex(reinterpret_cast<uintptr_t>(target.d3d11_images[output_indices[eye]]))
                   << "}";
        }
        for (uint32_t view = 0; view < 4; ++view) {
            const XrSwapchainSubImage& sub_image = source_layer->views[view].subImage;
            stream << ", source" << view
                   << "{swapchain=" << FormatHandle(sub_image.swapchain)
                   << ",image=" << selected_source_indices[view]
                   << ",texture="
                   << FormatHex(reinterpret_cast<uintptr_t>(selected_source_textures[view]))
                   << ",arraySlice=" << sub_image.imageArrayIndex
                   << ",rect=(" << sub_image.imageRect.offset.x << ","
                   << sub_image.imageRect.offset.y << ","
                   << sub_image.imageRect.extent.width << "x"
                   << sub_image.imageRect.extent.height << ")"
                   << ",acquires=" << swapchains[view]->acquire_count
                   << ",waits=" << swapchains[view]->wait_count
                   << ",releases=" << swapchains[view]->release_count
                   << "}";
        }
        logger_.Debug(stream.str());
    }

    if(restored_view_count)*restored_view_count+=restored;
    *composed_layer = *source_layer;
    composed_layer->viewCount = static_cast<uint32_t>(composed_views->size());
    composed_layer->views = composed_views->data();
    composed_layer->layerFlags &= ~(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT |
                                    XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT);
    return true;
}

XrResult OpenXrLayer::TraceRuntimeWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state) {
    const auto trace_call = turbo_trace_.Watch("runtime.wait");
    // Every runtime wait (the game's, Turbo's, Frame Generation's) tells the performance row the refresh period.
    const auto note_period = [this](XrResult result, const XrFrameState* state) {
        if (XR_SUCCEEDED(result) && state) {
            runtime_latest_wait_prediction_.store(state->predictedDisplayTime, std::memory_order_release);
            if (performance_.Wanted())
                performance_.Frames().RuntimePeriod(state->predictedDisplayPeriod, std::chrono::steady_clock::now());
        }
    };
    if (!turbo_trace_.Enabled()) {
        const auto result=next_wait_frame_(session, info, state);
        if (XR_SUCCEEDED(result) && state)
            osd_should_render_.store(state->shouldRender == XR_TRUE, std::memory_order_relaxed);
        note_period(result, state);
        return result;
    }
    // The watched call records the start in the recorder files. Watched
    // records never reach the ordinary log, so its sampled bursts keep an
    // enter of their own. The return carries the prediction.
    const auto id = turbo_trace_.NextId();
    if (turbo_trace_.Capturing()) turbo_trace_.Record("runtime.wait.enter", id);
    const auto result = next_wait_frame_(session, info, state);
    if (XR_FAILED(result)) turbo_trace_.Burst();
    turbo_trace_.Record("runtime.wait.return", id, XR_SUCCEEDED(result) ? state->predictedDisplayTime : 0,
                        XR_SUCCEEDED(result) ? state->predictedDisplayPeriod : 0, result);
    if (XR_SUCCEEDED(result) && state) osd_should_render_.store(state->shouldRender == XR_TRUE, std::memory_order_relaxed);
    if (XR_SUCCEEDED(result) && turbo_trace_.Capturing()) turbo_trace_.Record("runtime.wait.state", id, state->shouldRender, TurboClockNow());
    note_period(result, state);
    return result;
}

XrResult OpenXrLayer::TraceRuntimeBeginFrame(XrSession session, const XrFrameBeginInfo* info) {
    const auto trace_call = turbo_trace_.Watch("runtime.begin");
    std::unique_lock queue_lock(osd_vulkan_queue_mutex_,std::defer_lock);
    if(osd_vulkan_.load())queue_lock.lock();
    // The watched call records the start and duration in the recorder files.
    // Watched records never reach the ordinary log, so its sampled bursts keep
    // an enter and a return of their own; outside them only a failure adds one.
    const bool sampled = turbo_trace_.Capturing();
    const auto id = turbo_trace_.NextId();
    if (sampled) turbo_trace_.Record("runtime.begin.enter", id);
    const auto result = next_begin_frame_(session, info);
    if (XR_FAILED(result)) turbo_trace_.Burst();
    if (sampled || XR_FAILED(result)) turbo_trace_.Record("runtime.begin.return", id, result);
    if (XR_SUCCEEDED(result)) {
        // A begin opens the frame of the latest wait (a second wait cannot
        // return before it); the frame begun before bounds this one's end.
        runtime_end_floor_.store(runtime_begun_prediction_.load(std::memory_order_acquire), std::memory_order_release);
        runtime_begun_prediction_.store(runtime_latest_wait_prediction_.load(std::memory_order_acquire),
                                        std::memory_order_release);
    }
    return result;
}

void OpenXrLayer::DropPendingFrameGenQuad(bool abandon) {
    const bool claimed =
        pending_framegen_quad_ && pending_framegen_quad_->route == FrameGenQuadRoute::kClaimed;
    pending_framegen_quad_.reset();
    if (abandon && claimed) {
        // Its end never reaches Frame Generation: the claim goes.
        framegen_.AbandonFrame(0);
    }
}

XrResult OpenXrLayer::TraceRuntimeEndFrame(XrSession session, const XrFrameEndInfo* info) {
    const auto trace_call = turbo_trace_.Watch("forward.end");
    // This frame's route at the quad views compose site, if it came from it.
    std::optional<FrameGenQuadFrame> quad = std::move(pending_framegen_quad_);
    pending_framegen_quad_.reset();
    const std::optional<TraceQuadSpace> quad_space = pending_trace_quad_space_;
    pending_trace_quad_space_.reset();
    // Frame Generation turns one app frame into two runtime frames, each of
    // which goes through SubmitRuntimeFrame (and so carries the OSD).
    if (framegen_owns_pacing_.load(std::memory_order_acquire) && info) {
        if (FrameGenTrace& trace = framegen_.Trace(); trace.Enabled()) {
            const XrCompositionLayerProjection* projection = nullptr;
            for (uint32_t i = 0; info->layers && i < info->layerCount && !projection; ++i) {
                if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
                }
            }
            const bool views = projection && projection->views && projection->viewCount > 0;
            trace.Record("end", 0, info->displayTime, info->layerCount, views ? projection->viewCount : 0,
                         views ? reinterpret_cast<int64_t>(projection->views[0].subImage.swapchain) : 0,
                         views && projection->viewCount > 1
                             ? reinterpret_cast<int64_t>(projection->views[1].subImage.swapchain)
                             : 0);
            if (views) {
                trace.RecordPose("end.v0", 0, projection->views[0].pose);
                if (projection->viewCount > 1) {
                    trace.RecordPose("end.v1", 0, projection->views[1].pose);
                }
            }
            // The game's HUD quad, if it has one. Not on the frame the trace
            // began in: its space was not classified.
            std::uint32_t quad_index = 0;
            const XrCompositionLayerQuad* quad_layer = FirstQuadLayer(*info, &quad_index);
            if (quad_layer && quad_space && quad_space->space == quad_layer->space) {
                const auto micro = [](float value) { return static_cast<int64_t>(std::llround(value * 1e6)); };
                trace.Record("end.q", quad_index, static_cast<int64_t>(quad_space->kind), micro(quad_layer->size.width),
                             micro(quad_layer->size.height), reinterpret_cast<int64_t>(quad_layer->subImage.swapchain),
                             quad_layer->eyeVisibility);
                trace.RecordPose("end.q0", static_cast<std::uint64_t>(quad_space->kind), quad_layer->pose);
            }
        }
        return framegen_.EndFrame(session, info, framegen_calls_, quad ? &*quad : nullptr);
    }
    if (quad && quad->route == FrameGenQuadRoute::kClaimed) {
        // Its primary projection holds four views: a stereo runtime never
        // gets it (only Frame Generation composes it).
        if (!framegen_quad_unforwarded_logged_) {
            framegen_quad_unforwarded_logged_ = true;
            logger_.Error("A quad views frame Frame Generation claimed ended after it stopped owning frame pacing; "
                          "it was not shown.");
        }
        return XR_SUCCESS;
    }
    return SubmitRuntimeFrame(session, info);
}

XrResult OpenXrLayer::SubmitRuntimeFrame(XrSession session, const XrFrameEndInfo* info) {
    const bool queue_diagnostics = framegen_queue_diagnostics_ && framegen_queue_.Active();
    std::vector<XrSwapchain> submitted_swapchains;
    if (framegen_queue_.Active() && info) {
        // Releases Frame Generation deferred go before a frame showing their
        // images (passing through, or with Frame Generation stopped).
        for (std::uint32_t i = 0; i < info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* header = info->layers ? info->layers[i] : nullptr;
            if (!header) continue;
            const bool layer = header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            auto flush = [&](XrSwapchain swapchain) {
                if (queue_diagnostics && std::find(submitted_swapchains.begin(), submitted_swapchains.end(),
                                                   swapchain) == submitted_swapchains.end())
                    submitted_swapchains.push_back(swapchain);
                framegen_queue_.Flush(swapchain, FrameGenQueueCalls(), layer && FrameGenShownLazily(swapchain));
            };
            switch (header->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
                for (std::uint32_t v = 0; v < projection->viewCount && projection->views; ++v) {
                    flush(projection->views[v].subImage.swapchain);
                    for (auto* next = static_cast<const XrBaseInStructure*>(projection->views[v].next); next; next = next->next) {
                        if (next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                            flush(reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next)->subImage.swapchain);
                        }
                    }
                }
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD:
                flush(reinterpret_cast<const XrCompositionLayerQuad*>(header)->subImage.swapchain);
                break;
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
                flush(reinterpret_cast<const XrCompositionLayerCylinderKHR*>(header)->subImage.swapchain);
                break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
                flush(reinterpret_cast<const XrCompositionLayerEquirectKHR*>(header)->subImage.swapchain);
                break;
            case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
                flush(reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(header)->subImage.swapchain);
                break;
            case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
                flush(reinterpret_cast<const XrCompositionLayerCubeKHR*>(header)->swapchain);
                break;
            default:
                break;
            }
        }
    }
    std::unique_lock queue_lock(osd_vulkan_queue_mutex_,std::defer_lock);
    if(osd_vulkan_.load())queue_lock.lock();
    XrFrameEndInfo with_osd{};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    if (info && osd_monitoring_.load(std::memory_order_relaxed)) {
        if (const auto* overlay=osd_.Append(*info, osd_should_render_.load(std::memory_order_relaxed))) {
            with_osd=*info;
            layers.assign(info->layers, info->layers+info->layerCount);
            layers.push_back(overlay);
            with_osd.layerCount=static_cast<std::uint32_t>(layers.size()); with_osd.layers=layers.data();
            info=&with_osd;
        }
    }
    // SteamVR rejects an end whose displayTime is older than the prediction of
    // the frame begun before this one, and the rejected frame is lost (measured
    // with notes/steamvr-time-probe). A Turbo app frame can end a runtime frame
    // newer than the one it was predicted for: raise it to the oldest time the
    // runtime accepts. Pass-through frames are never below it.
    XrFrameEndInfo raised{};
    const XrTime end_floor = runtime_end_floor_.load(std::memory_order_acquire);
    if (info && end_floor > 0 && info->displayTime < end_floor) {
        raised = *info;
        raised.displayTime = end_floor;
        const std::uint64_t raises = runtime_end_raises_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (raises <= 3 || raises % 1000 == 0) {
            logger_.Info("Raised an xrEndFrame displayTime to the runtime's previous frame prediction: submitted=" +
                         std::to_string(info->displayTime) + ", floor=" + std::to_string(end_floor) +
                         ", raisedByNs=" + std::to_string(end_floor - info->displayTime) +
                         ", raisesThisSession=" + std::to_string(raises));
        }
        turbo_trace_.Record("runtime.end.raise", 0, info->displayTime, end_floor);
        info = &raised;
    }
    const auto submit=[&] {
        const auto submit_start=std::chrono::steady_clock::now();
        XrResult result;
        {
            const auto runtime_call = turbo_trace_.Watch("runtime.end");
            result = next_end_frame_(session, info);
        }
        const auto submit_end = std::chrono::steady_clock::now();
        const double submit_ms = std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
        osd_.RecordSubmit(submit_ms);
        if (queue_diagnostics && submit_ms >= 250.0) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                submit_end.time_since_epoch()).count();
            auto next_ms = framegen_queue_diagnostics_next_ms_.load(std::memory_order_relaxed);
            if (now_ms >= next_ms && framegen_queue_diagnostics_next_ms_.compare_exchange_strong(
                    next_ms, now_ms + 5000, std::memory_order_relaxed)) {
                std::ostringstream message;
                message << "Frame Generation slow runtime xrEndFrame: ms=" << submit_ms
                        << ", result=" << static_cast<int>(result) << ", submittedSwapchains=[";
                for (std::size_t i = 0; i < submitted_swapchains.size(); ++i) {
                    if (i) message << ',';
                    message << reinterpret_cast<std::uintptr_t>(submitted_swapchains[i]);
                }
                message << "]; " << framegen_queue_.Describe();
                logger_.Info(message.str());
            }
        }
        if (!layers.empty() && XR_FAILED(result)) {
            // Never retry xrEndFrame: a failing runtime may already consume it.
            // Let the renderer distinguish a frame-order/timestamp rejection
            // from an actual overlay failure.
            osd_.SubmissionFailed(result);
        }
        return result;
    };
    if (!turbo_trace_.Enabled()) return submit();
    const auto id = turbo_trace_.NextId();
    turbo_trace_.Record("submit.enter", id, info->displayTime, info->layerCount);
    const bool sampled = turbo_trace_.Capturing();
    if (sampled) {
        const XrTime clock_now = TurboClockNow();
        turbo_trace_.Record("submit.clock", id, clock_now, clock_now > 0 ? info->displayTime - clock_now : 0);
    }
    const auto result = submit();
    if (XR_FAILED(result)) turbo_trace_.Burst();
    // The watched runtime.end records the duration in the recorder files; the
    // ordinary log's sampled bursts, which never get watched records, keep the
    // return. Outside them only a failure adds one.
    if (sampled || XR_FAILED(result)) turbo_trace_.Record("submit.return", id, result);
    return result;
}

bool OpenXrLayer::WantsTurboClockConversion() {
    const auto parsed = LoadConfigFromFile(ResolveConfigPath());
    if (!parsed.ok) return false;
    const auto settings = ResolveTurboSettings(parsed.document, GetCurrentExecutableName());
    // Head prediction past the runtime's limit measures display times
    // against the runtime's clock too.
    const auto framegen = ResolveFrameGenSettings(parsed.document, GetCurrentExecutableName());
    return parsed.document.core.enabled &&
        (parsed.document.core.log_level == LogLevel::Debug ||
         (settings.experimental.enabled && settings.experimental.prediction_percent != 100) ||
         (framegen.enabled && framegen.extend_head_prediction));
}

bool OpenXrLayer::WantsD3D11Bridge() {
    if (const char* bridge = std::getenv("VECTORXR_D3D11_BRIDGE"); bridge && std::string(bridge) == "off") {
        return false;
    }
    const auto parsed = LoadConfigFromFile(ResolveConfigPath());
    if (!parsed.ok) return false;
    return parsed.document.core.enabled &&
           ResolveFrameGenSettings(parsed.document, GetCurrentExecutableName()).enabled;
}

bool OpenXrLayer::WantsVulkanBridge() {
    if (const char* bridge = std::getenv("VECTORXR_VULKAN_BRIDGE"); bridge && std::string(bridge) == "off") {
        return false;
    }
    const auto parsed = LoadConfigFromFile(ResolveConfigPath());
    if (!parsed.ok) return false;
    return parsed.document.core.enabled &&
           ResolveFrameGenSettings(parsed.document, GetCurrentExecutableName()).enabled;
}

XrTime OpenXrLayer::TurboClockNow() {
    if (!turbo_convert_counter_time_) return 0;
    using Convert = XrResult (XRAPI_PTR *)(XrInstance, const LARGE_INTEGER*, XrTime*);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    XrTime time = 0;
    return XR_SUCCEEDED(reinterpret_cast<Convert>(turbo_convert_counter_time_)(instance_, &counter, &time)) ? time : 0;
}

std::string OpenXrLayer::TurboTimingConfiguration() const {
    return (turbo_experiment_.enabled
        ? "Experimental Async; submit=" + std::string(turbo_experiment_.wait_for_submit ? "wait" : "overlap") +
          "; sample=" + (turbo_experiment_.sample_at_entry ? "entry" : "return") +
          "; prediction=" + std::to_string(turbo_experiment_.prediction_percent) + "%" +
          "; clock=" + (turbo_convert_counter_time_ ? "available" : "unavailable") +
          "; cap=" + std::to_string(turbo_experiment_.frame_limit)
        : std::string("Normal timing")) + "; trace=controlled by live Debug log level";
}

void OpenXrLayer::ConfigureTurboExperiment() {
    turbo_trace_.Stop();
    turbo_experiment_ = resolved_settings_.core.enabled ? resolved_settings_.turbo.experimental : TurboExperimentalSettings{};
    turbo_limit_last_wait_.reset();
    turbo_trace_last_engaged_ = false;
    turbo_convert_counter_time_ = nullptr;
    if (turbo_clock_enabled_ && next_get_instance_proc_addr_) {
        next_get_instance_proc_addr_(instance_, "xrConvertWin32PerformanceCounterToTimeKHR", &turbo_convert_counter_time_);
    }
    // Pacing decisions may survive sessions, but an experimental session must
    // neither inherit nor teach a production verdict.
    if (turbo_experiment_.enabled) {
        turbo_pacing_resolved_ = false;
        turbo_pacing_verdict_pending_ = false;
    }
    // After "Turbo yields to Frame Generation" (xrCreateSession), these are
    // settings the session does not use; say so rather than list them as live.
    logger_.Info(std::string(turbo_yielded_to_framegen_
                                 ? "Turbo session settings (unused: Frame Generation paces this session): "
                                 : "Turbo session settings: ") +
        "application=" + current_exe_name_ +
        ", profile=" + resolved_settings_.turbo.profile_name +
        ", experimental=" + std::to_string(turbo_experiment_.enabled) +
        ", waitForSubmit=" + std::to_string(turbo_experiment_.wait_for_submit) +
        ", sampleAtEntry=" + std::to_string(turbo_experiment_.sample_at_entry) +
        ", predictionPercent=" + std::to_string(turbo_experiment_.prediction_percent) +
        ", frameLimit=" + std::to_string(turbo_experiment_.frame_limit) +
        ", runtimeClock=" + std::to_string(turbo_convert_counter_time_ != nullptr) +
        ", pacing=" + (turbo_experiment_.enabled ? std::string("experimental-async") : ToString(resolved_settings_.turbo.pacing_mode)) +
        ", metrics=" + ToString(resolved_settings_.turbo.metrics_mode) + "; changes require a new session.");
    if (turbo_experiment_.enabled && turbo_experiment_.prediction_percent != 100 && !turbo_convert_counter_time_) {
        logger_.Info("Turbo experiment: runtime clock conversion unavailable; prediction dampening will be bypassed.");
    }
    turbo_trace_.Start(logger_);
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
    bool fabricated = false;
    const XrResult result = PaceWaitFrame(session, frame_wait_info, frame_state, fabricated);
    // The OSD's performance row times the game's frames from here: its CPU time starts as its wait returns.
    if (XR_SUCCEEDED(result) && performance_.Wanted()) {
        performance_.Frames().Wait(std::chrono::steady_clock::now(), fabricated);
    }
    return result;
}

XrResult OpenXrLayer::PaceWaitFrame(XrSession session,
                                    const XrFrameWaitInfo* frame_wait_info,
                                    XrFrameState* frame_state,
                                    bool& fabricated) {
    const auto trace_call = turbo_trace_.Watch("app.wait");
    if (!frame_state || (frame_wait_info && frame_wait_info->type != XR_TYPE_FRAME_WAIT_INFO) ||
        frame_state->type != XR_TYPE_FRAME_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    NoteThread(ThreadCensus::Call::WaitFrame);
    if (framegen_owns_pacing_.load(std::memory_order_acquire)) {
        const XrResult result = framegen_.WaitFrame(session, frame_wait_info, frame_state, framegen_calls_);
        framegen_.Trace().Record("wait", 0, frame_state->predictedDisplayTime, frame_state->predictedDisplayPeriod,
                                 frame_state->shouldRender, result);
        return result;
    }
    const auto trace_id = turbo_trace_.NextId();
    turbo_trace_.Record("wait.enter", trace_id);
    const bool experimental_active = turbo_experiment_.enabled &&
        turbo_effective_active_.load(std::memory_order_relaxed) && turbo_effective_async_.load(std::memory_order_relaxed);
    if (experimental_active && turbo_experiment_.frame_limit > 0) {
        const auto limit_start = std::chrono::steady_clock::now();
        turbo_trace_.Record("limit.enter", trace_id, turbo_experiment_.frame_limit);
        if (turbo_limit_last_wait_) {
            std::this_thread::sleep_until(*turbo_limit_last_wait_ +
                std::chrono::nanoseconds(1'000'000'000 / turbo_experiment_.frame_limit));
        }
        turbo_limit_last_wait_ = std::chrono::steady_clock::now();
        turbo_trace_.Record("limit.return", trace_id);
        std::scoped_lock lock(turbo_mutex_);
        turbo_metrics_wait_pending_ms_ += std::chrono::duration<double, std::milli>(
            *turbo_limit_last_wait_ - limit_start).count();
    } else {
        turbo_limit_last_wait_.reset();
    }
    const auto entry_time = experimental_active && turbo_experiment_.sample_at_entry
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!turbo_frame_interception_required_.load(std::memory_order_acquire)) {
        if (!frame_pacing_debug_enabled_.load(std::memory_order_relaxed)) {
            const auto result = TraceRuntimeWaitFrame(session, frame_wait_info, frame_state);
            turbo_trace_.Record("wait.return", trace_id, XR_SUCCEEDED(result) ? frame_state->predictedDisplayTime : 0,
                XR_SUCCEEDED(result) ? frame_state->predictedDisplayPeriod : 0, result);
            return result;
        }
        const auto wait_start = std::chrono::steady_clock::now();
        const XrResult result = TraceRuntimeWaitFrame(session, frame_wait_info, frame_state);
        const auto wait_end = std::chrono::steady_clock::now();
        if (XR_SUCCEEDED(result) && frame_state) {
            const double wait_ms =
                std::chrono::duration<double, std::milli>(wait_end - wait_start).count();
            std::scoped_lock lock(turbo_mutex_);
            pacing_wait_sum_ms_ += wait_ms;
            pacing_wait_max_ms_ = std::max(pacing_wait_max_ms_, wait_ms);
            ++pacing_wait_samples_;
            // Preserve the runtime timing horizon so enabling Turbo later in
            // the same session begins from the last real app-visible frame.
            turbo_last_predicted_display_time_ = frame_state->predictedDisplayTime;
            turbo_last_predicted_display_period_ = frame_state->predictedDisplayPeriod;
            turbo_prediction_history_.NativeWait(frame_state->predictedDisplayTime,
                                                   frame_state->predictedDisplayPeriod);
            turbo_last_should_render_ = frame_state->shouldRender == XR_TRUE;
            turbo_max_returned_display_time_ =
                std::max(turbo_max_returned_display_time_, frame_state->predictedDisplayTime);
            NoteTurboShouldRenderLocked(turbo_last_should_render_);
            turbo_last_wait_frame_wall_time_ = wait_end;
        }
        turbo_trace_.Record("wait.return", trace_id, XR_SUCCEEDED(result) ? frame_state->predictedDisplayTime : 0,
                            XR_SUCCEEDED(result) ? frame_state->predictedDisplayPeriod : 0, result);
        return result;
    }
    {
        std::unique_lock lock(turbo_mutex_);
        if (experimental_active && turbo_experiment_.wait_for_submit && turbo_end_frame_in_flight_) {
            const auto gate_start = std::chrono::steady_clock::now();
            turbo_trace_.Record("gate.enter", trace_id);
            const bool complete = turbo_async_handoff_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !turbo_end_frame_in_flight_ && !turbo_async_handoff_active_;
            });
            turbo_metrics_wait_pending_ms_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - gate_start).count();
            turbo_trace_.Record("gate.return", trace_id, complete);
        }
        bool wait_pipelined = turbo_async_wait_.valid();
        bool async_handoff = turbo_async_handoff_active_;
        bool async_interception_cancelled = false;
        // A second poll that gave up on the runtime's pipelined wait.
        bool poll_without_wait = false;
        // Fabricate while a pipelined wait is outstanding (async pacing) or
        // the sequenced pipeline is established — in both cases the runtime
        // already owns this frame's pacing and this wait must never reach it.
        // The sequenced shield is a persistent state, not a per-frame flag:
        // DCS calls xrWaitFrame concurrently with xrEndFrame, and any gap
        // here would let that wait reach the runtime alongside our own
        // (hardlock).
        if (wait_pipelined || async_handoff || turbo_seq_state_ == TurboSequencedState::kActive) {
            if (async_handoff) {
                ++turbo_async_handoff_wait_intercepts_;
                if (turbo_async_handoff_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_async_handoff_debug_log_budget_;
                    logger_.Debug("Turbo-diag: app xrWaitFrame intercepted by async handoff shield; "
                                  "generation=" + std::to_string(turbo_async_wait_generation_) +
                                  ", futurePublished=" + (wait_pipelined ? "1" : "0") +
                                  ", alreadyPolled=" + (turbo_async_wait_polled_ ? "1" : "0") + ".");
                }
            }
            if (!wait_pipelined && !async_handoff && !turbo_valve_open_) {
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
            if (wait_pipelined || async_handoff) {
                bool mark_wait_polled = true;
                if (turbo_async_wait_polled_ || !turbo_valve_open_) {
                    // Second poll while pipelined: only one frame of
                    // pipelining is allowed, so now we must wait for the real
                    // frame. If EndFrame has pre-armed the handoff but not yet
                    // published the worker future, first wait for publication
                    // (or cancellation after a failed submit).
                    if (!wait_pipelined && async_handoff) {
                        ++turbo_async_handoff_second_poll_blocks_;
                        if (turbo_async_handoff_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                            --turbo_async_handoff_debug_log_budget_;
                            logger_.Debug("Turbo-diag: second app xrWaitFrame waiting for async handoff "
                                          "publication; generation=" +
                                          std::to_string(turbo_async_wait_generation_) + ".");
                        }
                        turbo_trace_.Record("poll.handoff.enter", trace_id, turbo_async_wait_generation_);
                        turbo_async_handoff_cv_.wait(lock, [this] {
                            return !turbo_async_handoff_active_ || turbo_async_wait_.valid();
                        });
                        turbo_trace_.Record("poll.handoff.return", trace_id, turbo_async_wait_generation_);
                        wait_pipelined = turbo_async_wait_.valid();
                        async_handoff = turbo_async_handoff_active_;
                        // A handover to frame-thread pacing replaces the shield
                        // without a gap: this poll's frame stays fabricated, and
                        // EndFrame opens the runtime frame itself. Only a
                        // cancelled shield returns this call to the runtime.
                        if (!wait_pipelined && !async_handoff &&
                            turbo_seq_state_ != TurboSequencedState::kActive) {
                            async_interception_cancelled = true;
                            mark_wait_polled = false;
                            if (turbo_async_handoff_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                                --turbo_async_handoff_debug_log_budget_;
                                logger_.Debug("Turbo-diag: async handoff cancelled while a second app "
                                              "xrWaitFrame waited; returning to runtime pass-through.");
                            }
                        }
                    }

                    if (wait_pipelined) {
                        // Keep a shared snapshot: EndFrame may retire the
                        // member while this app thread is blocked.
                        const std::shared_future<void> pending_wait = turbo_async_wait_;
                        const std::uint64_t pending_generation = turbo_async_wait_generation_;
                        const bool recouple = !turbo_valve_open_;
                        // Bounded like EndFrame's late wait, and only by the
                        // drain once the runtime has already held this wait
                        // past that bound.
                        const std::chrono::milliseconds poll_bound =
                            turbo_async_held_generation_ == pending_generation ? kTurboDrainTimeout
                                                                                : kTurboLateWaitTimeout;
                        const auto poll_start = std::chrono::steady_clock::now();
                        bool poll_timed_out = false;
                        lock.unlock();
                        turbo_trace_.Record("poll.worker.enter", trace_id, pending_generation);
                        if (!recouple) {
                            // A safety stop must also release app polls already
                            // waiting on this snapshot. The real runtime wait
                            // itself is retained and cannot be cancelled. Nor
                            // may a runtime that holds it hold the app's frame
                            // loop: in issue #88, DCS on Windows Mixed Reality
                            // froze after shouldRender turned FALSE and never
                            // reached xrEndSession.
                            const std::chrono::steady_clock::time_point deadline = poll_start + poll_bound;
                            for (;;) {
                                const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                                if (now >= deadline) {
                                    poll_timed_out = true;
                                    break;
                                }
                                const std::chrono::steady_clock::time_point slice =
                                    now + std::chrono::milliseconds(100);
                                if (pending_wait.wait_until(std::min(deadline, slice)) ==
                                    std::future_status::ready) {
                                    break;
                                }
                                if (turbo_auto_suspended_.load()) break;
                            }
                            poll_without_wait = pending_wait.wait_for(std::chrono::milliseconds(0)) !=
                                                std::future_status::ready;
                        } else {
                            // Keep ownership of waits/begins stable when Turbo
                            // is switched off, but re-couple the app to runtime
                            // pacing. Bound this for submit-interlocked runtimes.
                            pending_wait.wait_for(std::chrono::milliseconds(100));
                        }
                        turbo_trace_.Record("poll.worker.return", trace_id, pending_generation);
                        lock.lock();
                        if (recouple) {
                            turbo_metrics_wait_pending_ms_ += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - poll_start).count();
                        }
                        if (pending_generation != turbo_async_wait_generation_) {
                            // EndFrame already retired this wait and may have
                            // launched the following one. Do not mark that newer
                            // wait as having been polled by this older call.
                            mark_wait_polled = false;
                        } else if (poll_timed_out && poll_without_wait) {
                            // The runtime is holding this wait. EndFrame drops
                            // app frames (each a stall) until it completes;
                            // later polls give up sooner.
                            turbo_async_held_generation_ = pending_generation;
                            turbo_trace_.Burst();
                            turbo_trace_.Record("poll.stuck", trace_id, pending_generation);
                        }
                    }
                }
                if (!async_interception_cancelled && mark_wait_polled) {
                    turbo_async_wait_polled_ = true;
                }
            }

            if (async_interception_cancelled) {
                // The downstream submit failed and EndFrame cancelled the
                // pre-publication shield. This call has not fabricated a frame,
                // so it can safely resume normal runtime pacing below.
            } else {
                const auto now = experimental_active && turbo_experiment_.sample_at_entry
                    ? entry_time : std::chrono::steady_clock::now();
                XrTime predicted = turbo_last_predicted_display_time_;
                bool extrapolated = false;
                if ((wait_pipelined || async_handoff) && !turbo_async_wait_completed_ &&
                    turbo_last_wait_frame_wall_time_.has_value()) {
                    // Async wait still in flight: extrapolate by the wall-clock
                    // delta between the app's waits.
                    predicted += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     now - *turbo_last_wait_frame_wall_time_)
                                     .count();
                    extrapolated = true;
                } else if (turbo_pacing_mode_ == TurboPacingMode::kSequenced && turbo_seq_wait_in_flight_ &&
                           turbo_last_predicted_display_period_ > 0) {
                    // This app wait overlaps the post-submit runtime wait, so its
                    // frame ends a runtime frame after that wait's: hand it the
                    // prediction that wait is about to return, not the cached
                    // one a frame older (SteamVR rejects a time older than the
                    // previous frame's prediction).
                    predicted += turbo_last_predicted_display_period_;
                }
                turbo_last_wait_frame_wall_time_ = now;

                const XrTime extrapolated_prediction = predicted;
                if (experimental_active && turbo_experiment_.prediction_percent != 100) {
                    const XrTime clock_now = TurboClockNow();
                    if (clock_now > 0 && predicted > clock_now) {
                        predicted = clock_now + static_cast<XrTime>(
                            static_cast<double>(predicted - clock_now) * turbo_experiment_.prediction_percent / 100.0);
                    }
                }
                // Preserve the runtime prediction whenever possible. A full-period
                // minimum step accumulates artificial lead when predictions repeat
                // or the app outruns refresh. Only enforce strict monotonicity,
                // as Toolkit does; keep predictedDisplayPeriod runtime-owned.
                frame_state->predictedDisplayTime =
                    std::max(predicted, turbo_max_returned_display_time_ + 1);
                frame_state->predictedDisplayPeriod = turbo_last_predicted_display_period_;
                // A poll that gave up on the runtime's wait returns a frame the
                // runtime has not granted: the app skips rendering it, and
                // EndFrame drops it unless the wait completes first.
                frame_state->shouldRender = turbo_last_should_render_ && !poll_without_wait ? XR_TRUE : XR_FALSE;
                turbo_max_returned_display_time_ = frame_state->predictedDisplayTime;
                turbo_prediction_history_.AppWait(frame_state->predictedDisplayTime,
                    extrapolated ? TurboPredictionHistory::Origin::kAsyncExtrapolated :
                    turbo_pacing_mode_ == TurboPacingMode::kSequenced
                        ? TurboPredictionHistory::Origin::kSequencedCache
                        : TurboPredictionHistory::Origin::kAsyncCache,
                    trace_id);
                turbo_trace_.Record("predict.adjust", trace_id, extrapolated_prediction, predicted,
                                    frame_state->predictedDisplayTime - predicted);
                turbo_trace_.Record("predict.state", trace_id, turbo_async_wait_completed_, async_handoff,
                                    static_cast<int>(turbo_pacing_mode_));
                turbo_trace_.Record("predict", trace_id, turbo_last_predicted_display_time_,
                                    frame_state->predictedDisplayTime, turbo_async_wait_generation_);
                turbo_trace_.Record("wait.return", trace_id, frame_state->predictedDisplayTime,
                                    frame_state->predictedDisplayPeriod, XR_SUCCESS);
                if (turbo_trace_.Capturing()) turbo_trace_.Record("clock", trace_id, TurboClockNow());
                ++pacing_fabricated_waits_;
                ++turbo_metrics_fabricated_pending_;
                fabricated = true;
                if (turbo_fabricated_wait_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_fabricated_wait_log_budget_;
                    logger_.Debug("Turbo: fabricated xrWaitFrame return, predictedDisplayTime=" +
                                  std::to_string(frame_state->predictedDisplayTime) +
                                  ", asyncWaitCompleted=" + (turbo_async_wait_completed_ ? "1" : "0") +
                                  ", asyncHandoff=" + (async_handoff ? "1" : "0") +
                                  ", runtimeWaitPending=" + (poll_without_wait ? "1" : "0"));
                }
                return XR_SUCCESS;
            }
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
        result = TraceRuntimeWaitFrame(session, frame_wait_info, frame_state);
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
        turbo_prediction_history_.NativeWait(frame_state->predictedDisplayTime,
                                               frame_state->predictedDisplayPeriod);
        turbo_last_should_render_ = frame_state->shouldRender == XR_TRUE;
        NoteTurboShouldRenderLocked(turbo_last_should_render_);
        turbo_last_wait_frame_wall_time_ = experimental_active && turbo_experiment_.sample_at_entry
            ? entry_time : std::chrono::steady_clock::now();
        frame_state->predictedDisplayTime =
            std::max(frame_state->predictedDisplayTime, turbo_max_returned_display_time_ + 1);
        turbo_max_returned_display_time_ = frame_state->predictedDisplayTime;
        turbo_prediction_history_.AppWait(frame_state->predictedDisplayTime,
                                            TurboPredictionHistory::Origin::kRuntime, trace_id);

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
    if (XR_FAILED(result) && (handshake || turbo_effective_active_.load())) {
        NoteTurboRuntimeFailure(result, 0, TurboFrameOperation::kWait);
    }
    turbo_trace_.Record("wait.return", trace_id, XR_SUCCEEDED(result) ? frame_state->predictedDisplayTime : 0,
                        XR_SUCCEEDED(result) ? frame_state->predictedDisplayPeriod : 0, result);
    return result;
}

XrResult OpenXrLayer::BeginFrame(XrSession session, const XrFrameBeginInfo* frame_begin_info) {
    const auto trace_call = turbo_trace_.Watch("app.begin");
    // The ordinary log's sampled bursts never get watched records: they keep
    // the app's begin as a record of its own.
    if (turbo_trace_.Capturing()) turbo_trace_.Record("begin.enter", turbo_trace_.NextId());
    NoteThread(ThreadCensus::Call::BeginFrame);
    if (frame_begin_info && frame_begin_info->type != XR_TYPE_FRAME_BEGIN_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // A wait that returns from here until this frame's end is pipelined (the performance row).
    if (performance_.Wanted()) {
        performance_.Frames().Begin(static_cast<std::uint32_t>(GetCurrentThreadId()));
    }
    {
        std::scoped_lock lock(mutex_);
        if (session == active_session_) {
            eye_gaze_action_set_attachment_.NoteFrameBoundary();
        }
    }
    TryAttachEyeGazeActionSetFallback(session);

    if (framegen_owns_pacing_.load(std::memory_order_acquire)) {
        const XrResult result = framegen_.BeginFrame(session, frame_begin_info, framegen_calls_);
        framegen_.Trace().Record("begin", 0, result);
        return result;
    }
    if (!turbo_frame_interception_required_.load(std::memory_order_acquire)) {
        return TraceRuntimeBeginFrame(session, frame_begin_info);
    }
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
        } else if (turbo_async_wait_.valid() || turbo_async_handoff_active_ ||
                   turbo_seq_state_ == TurboSequencedState::kActive) {
            // Async pacing: deferred into ForwardEndFrame once the async wait
            // resolves. Sequenced pacing: the frame was pre-begun inside the
            // previous EndFrame (or will be compensated there).
            if (turbo_async_handoff_active_) {
                ++turbo_async_handoff_begin_intercepts_;
                if (turbo_async_handoff_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
                    --turbo_async_handoff_debug_log_budget_;
                    logger_.Debug("Turbo-diag: app xrBeginFrame intercepted by async handoff shield; "
                                  "generation=" + std::to_string(turbo_async_wait_generation_) +
                                  ", futurePublished=" + (turbo_async_wait_.valid() ? "1" : "0") + ".");
                }
            }
            return XR_SUCCESS;
        } else if (turbo_seq_state_ == TurboSequencedState::kEngaging &&
                   turbo_seq_debug_log_budget_ > 0 && logger_.IsDebugEnabled()) {
            --turbo_seq_debug_log_budget_;
            logger_.Debug("Turbo-diag: app xrBeginFrame passing through while engaging.");
        }
    }
    const XrResult result = TraceRuntimeBeginFrame(session, frame_begin_info);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(turbo_mutex_);
        turbo_frame_begun_ = true;
    } else if (turbo_effective_active_.load()) {
        NoteTurboRuntimeFailure(result, 0, TurboFrameOperation::kBegin);
    }
    return result;
}

void OpenXrLayer::ObserveCompositionLayerTopology(const XrFrameEndInfo* frame_end_info) {
    if (composition_topology_log_budget_ <= 0) {
        return;
    }
    // A quad views frame Frame Generation claimed goes to it, not to the
    // runtime: it composes the four views into the stereo frames the runtime
    // gets. That route's topology is followed apart from the frames forwarded
    // as they are, so frames moving between the two routes are not logged as
    // topology changes (each move spent the session's budget).
    const bool claimed = pending_framegen_quad_ && pending_framegen_quad_->route == FrameGenQuadRoute::kClaimed;
    std::optional<std::uint64_t>& last_signature =
        claimed ? last_claimed_composition_topology_signature_ : last_composition_topology_signature_;
    const SpaceKindOf kind_of = [this](XrSpace space) { return ClassifySpace(space); };
    const std::uint64_t signature = CompositionLayerTopologySignature(frame_end_info, kind_of);
    if (last_signature == signature) {
        return;
    }
    last_signature = signature;
    --composition_topology_log_budget_;
    logger_.Info(std::string(claimed ? "Composition layer topology given to Frame Generation, which composes its "
                                       "quad views into the stereo frames the runtime gets: "
                                     : "Composition layer topology forwarded downstream by VectorXR: ") +
                 FormatCompositionLayerTopology(frame_end_info, kind_of) +
                 (composition_topology_log_budget_ == 0
                      ? "; further topology changes suppressed this session."
                      : "."));
}

XrResult OpenXrLayer::ForwardEndFrame(XrSession session,
                                      const XrFrameEndInfo* frame_end_info,
                                      std::unique_lock<std::mutex>& config_lock) {
    // Runtime wait/begin can fail before TraceRuntimeEndFrame is reached: a
    // quad views frame Frame Generation claimed that never reached
    // TraceRuntimeEndFrame is abandoned.
    struct PendingQuadGuard {
        OpenXrLayer& layer;
        ~PendingQuadGuard() {
            layer.DropPendingFrameGenQuad(true);
        }
    } pending_guard{*this};
    ObserveCompositionLayerTopology(frame_end_info);
    // Frame Generation's trace records the frame's first quad layer with its
    // space's kind (TraceRuntimeEndFrame, which runs without mutex_): it is
    // classified here, under config_lock, for either call below.
    pending_trace_quad_space_.reset();
    if (frame_end_info && framegen_owns_pacing_.load(std::memory_order_acquire) && framegen_.Trace().Enabled()) {
        if (const XrCompositionLayerQuad* quad = FirstQuadLayer(*frame_end_info)) {
            pending_trace_quad_space_ = TraceQuadSpace{quad->space, ClassifySpace(quad->space)};
        }
    }
    if (frame_pacing_debug_enabled_.load(std::memory_order_relaxed) && frame_end_info) {
        std::scoped_lock lock(turbo_mutex_);
        if (turbo_last_predicted_display_period_ > 0 && turbo_max_returned_display_time_ > 0) {
            const double delta_periods =
                static_cast<double>(frame_end_info->displayTime - turbo_max_returned_display_time_) /
                static_cast<double>(turbo_last_predicted_display_period_);
            pacing_submit_delta_sum_periods_ += delta_periods;
            if (pacing_submit_delta_samples_ == 0) {
                pacing_submit_delta_min_periods_ = delta_periods;
                pacing_submit_delta_max_periods_ = delta_periods;
            } else {
                pacing_submit_delta_min_periods_ =
                    std::min(pacing_submit_delta_min_periods_, delta_periods);
                pacing_submit_delta_max_periods_ =
                    std::max(pacing_submit_delta_max_periods_, delta_periods);
            }
            ++pacing_submit_delta_samples_;
        }
    }
    if (!turbo_frame_interception_required_.load(std::memory_order_acquire)) {
        const bool timing_enabled =
            frame_pacing_debug_enabled_.load(std::memory_order_relaxed);
        std::optional<std::chrono::steady_clock::time_point> pacing_start;
        if (timing_enabled) {
            pacing_start = std::chrono::steady_clock::now();
        }
        config_lock.unlock();
        const XrResult result = TraceRuntimeEndFrame(session, frame_end_info);
        if (XR_FAILED(result) && end_frame_error_log_budget_ > 0) {
            --end_frame_error_log_budget_;
            logger_.Error("Runtime xrEndFrame failed with " +
                          std::to_string(static_cast<int>(result)) + " (layerCount=" +
                          std::to_string(frame_end_info ? frame_end_info->layerCount : 0) + ")" +
                          (end_frame_error_log_budget_ == 0
                               ? "; further failures suppressed this session."
                               : "."));
        }
        if (timing_enabled) {
            const auto pacing_after_end = std::chrono::steady_clock::now();
            RecordFramePacing(*pacing_start, *pacing_start, pacing_after_end, false);
        }
        return result;
    }

    // Black-screen forensics: an app that keeps its frame loop running but
    // stops submitting layers shows a void with no error anywhere (DCS did
    // exactly this on SteamVR, silently, after one rendered frame). The
    // first observation is skipped — apps commonly start empty while loading.
    const bool submitting_layers = frame_end_info && frame_end_info->layerCount > 0;
    if (frame_end_info) {
        std::scoped_lock lock(turbo_mutex_);
        turbo_last_environment_blend_mode_ = frame_end_info->environmentBlendMode;
    }
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
    // Recovery is independent of Auto/forced/pinned pacing. Disabling the
    // setting bypasses only interrupted-session protection, not live stalls.
    if (!resolved_settings_.turbo.interrupted_session_recovery) {
        turbo_recovery_.Prepare(false, false, ToString(turbo_pacing_mode_));
        turbo_recovery_blocked_.store(false);
    }

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
    bool runtime_should_render = true;
    {
        const auto entry_now = std::chrono::steady_clock::now();
        if (pacing_last_end_time_.has_value()) {
            app_frame_delta_ms =
                std::chrono::duration<double, std::milli>(entry_now - *pacing_last_end_time_).count() -
                turbo_last_frame_blocked_ms_;
        }
        std::scoped_lock lock(turbo_mutex_);
        runtime_should_render = turbo_last_should_render_;
    }
    // A steady empty-frame loop while the session is merely SYNCHRONIZED is
    // not a usable cadence. Engaging there lets fabricated display times run
    // ahead while SteamVR is not presenting; when visibility returns, the
    // runtime rejects every submit with XR_ERROR_TIME_INVALID.
    const bool renderable_frame = runtime_should_render && submitting_layers;
    if (renderable_frame && app_frame_delta_ms >= 0.0 && app_frame_delta_ms < kHealthyFrameMs) {
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
        pipeline_established = turbo_async_wait_.valid() || turbo_async_handoff_active_ ||
                               turbo_seq_state_ == TurboSequencedState::kActive ||
                               turbo_seq_state_ == TurboSequencedState::kEngaging;
    }
    const bool cadence_countable = turbo_cadence_ready_;
    // While a recovery block holds, Turbo engages on nothing but a retry, so
    // the cadence lines below would only promise an engagement.
    const bool recovery_blocked_before = turbo_recovery_blocked_.load();
    bool cadence_stabilized = false;
    if (turbo_engaged && !turbo_cadence_ready_ && !pipeline_established) {
        // Delay the first engage until the app renders steadily.
        turbo_engaged = false;
        if (!turbo_cadence_pause_logged_) {
            if (!recovery_blocked_before) {
                logger_.Info("Turbo: waiting for a stable renderable frame cadence before pipelining "
                             "(app loading, hidden, or hitching); engages automatically.");
            }
            turbo_cadence_pause_logged_ = true;
        }
    } else if (turbo_engaged && turbo_cadence_pause_logged_) {
        cadence_stabilized = true;
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

    if (turbo_engaged) {
        const bool allowed = turbo_recovery_.Prepare(resolved_settings_.turbo.interrupted_session_recovery,
            turbo_recovery_retry_requested_, ToString(turbo_pacing_mode_));
        turbo_recovery_retry_requested_ = false;
        const bool was_blocked = turbo_recovery_blocked_.exchange(!allowed);
        if (!allowed) {
            turbo_engaged = false;
            if (!was_blocked) logger_.Info("Turbo recovery: " + turbo_recovery_.Reason());
        }
    }
    // Only now is it known whether pipelining really engages: a recorded
    // verdict can suspend Turbo and a recovery block keep it off, and each
    // logs its own reason.
    if (cadence_stabilized && turbo_engaged) {
        logger_.Info("Turbo: frame cadence stabilized; pipelining engages.");
    }
    turbo_effective_async_.store(turbo_pacing_mode_ == TurboPacingMode::kAsync);
    const bool was_effective = turbo_effective_active_.exchange(turbo_engaged);
    if (turbo_engaged && !was_effective) {
        SoundPlayer::Instance().PlayTransition(ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings).sound, true,
            dll_directory_, resolved_settings_.core.sound_volume, L"turbo-on.wav", L"turbo-off.wav");
    }

    // Copied under the lock: the auto-suspend path below runs after the lock
    // is released and may need to play the toggle's deactivate cue.
    SoundFeedback turbo_sound{};
    int turbo_sound_volume = 100;
    if (turbo_engaged) {
        turbo_sound = ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings).sound;
        turbo_sound_volume = resolved_settings_.core.sound_volume;
    }
    // Metrics capture config, also copied under the lock: the recorder runs
    // at the tail of this function, after the lock is released.
    const TurboMetricsMode metrics_mode = resolved_settings_.turbo.metrics_mode;
    const std::vector<InputBinding> metrics_bindings = resolved_settings_.turbo.metrics_bindings;
    const bool metrics_available = resolved_settings_.core.enabled && resolved_settings_.turbo.enabled;
    const int metrics_sound_volume = resolved_settings_.core.sound_volume;
    // Why Turbo is off decides whether an established async pipeline may stay
    // re-coupled. A manual toggle-off keeps it (toggling back on must resume
    // the same queued frame). Every other reason releases the worker wait.
    const bool suspended_now = turbo_auto_suspended_.load(std::memory_order_relaxed);
    const bool recovery_blocked_now = turbo_recovery_blocked_.load();
    const char* async_release_reason =
        turbo_engaged ? nullptr :
        !metrics_available ? "Turbo disabled in settings" :
        suspended_now ? "Turbo safety suspension" :
        recovery_blocked_now ? "Turbo recovery block" :
        turbo_async_recouple_stalled_ ? "runtime wait stalled while Turbo was off" : nullptr;

    // Everything past this point is frame forwarding: turbo drain (which can
    // block a full frame interval), the deferred begin, and the runtime end.
    // Release the config/render lock so locate calls from other app threads
    // keep flowing — holding it here caused visible judder at locked fps.
    config_lock.unlock();

    const auto pacing_start = std::chrono::steady_clock::now();

    bool has_pending_wait = false;
    bool keep_async_pipeline = false;
    bool frame_begun = false;
    bool begin_owed = false;
    bool valve_open = false;
    std::shared_future<void> pending_async_wait;
    std::uint64_t pending_async_generation = 0;
    bool pending_wait_held = false;
    TurboSequencedState seq_state = TurboSequencedState::kInactive;
    {
        std::scoped_lock lock(turbo_mutex_);
        has_pending_wait = turbo_async_wait_.valid();
        if (has_pending_wait) {
            pending_async_wait = turbo_async_wait_;
            pending_async_generation = turbo_async_wait_generation_;
            pending_wait_held = turbo_async_held_generation_ == pending_async_generation;
        }
        // This submit consumes the begun-frame marker (set by our pre-begin,
        // a compensation begin, or the app's own begin passing through).
        frame_begun = turbo_frame_begun_;
        turbo_frame_begun_ = false;
        seq_state = turbo_seq_state_;
        keep_async_pipeline = turbo_pacing_mode_ == TurboPacingMode::kAsync &&
            seq_state == TurboSequencedState::kInactive &&
            (turbo_engaged || has_pending_wait || turbo_async_handoff_active_);
        if (keep_async_pipeline) {
            if (turbo_valve_open_ != turbo_engaged) {
                logger_.Info(turbo_engaged
                    ? "Turbo: async pacing decoupled; frame ownership preserved."
                    : "Turbo: async pacing re-coupled; frame ownership preserved.");
            }
            turbo_valve_open_ = turbo_engaged;
        }
        begin_owed = turbo_begin_owed_;
        valve_open = turbo_valve_open_;
        if (!has_pending_wait && turbo_engaged && turbo_pacing_mode_ == TurboPacingMode::kAsync &&
            seq_state == TurboSequencedState::kInactive) {
            // Pre-arm before the runtime submit. DCS may issue its next app
            // wait concurrently with this EndFrame; it must see interception
            // even though the worker's real wait cannot start until EndFrame
            // has returned downstream.
            ArmTurboAsyncHandoffLocked("pre-submit async establishment");
        }
        // From here until the runtime xrEndFrame returns, an owed
        // establishment begin arriving on another thread is deferred rather
        // than forwarded (Begin(N+1) must never reach the runtime before
        // End(N) — the MSFS2024 ordering).
        turbo_end_frame_in_flight_ = true;
    }
    const auto abandon_end_frame_window = [this] {
        std::scoped_lock lock(turbo_mutex_);
        turbo_end_frame_in_flight_ = false;
        turbo_begin_deferred_ = false;
        CancelTurboAsyncHandoffLocked("frame forwarding aborted before submit");
    };
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
    const XrTime submitted_display_time = frame_end_info ? frame_end_info->displayTime : 0;
    if (has_pending_wait) {
        // This is the latest point the pipelined frame must have been fully
        // waited. Some runtimes interlock an off-thread wait with the next
        // submit (PiOpenXR, Oculus, Varjo): it stalls until we begin and submit
        // anyway, and they accept that begin. A conformant runtime instead
        // rejects a begin without a completed wait (XR_ERROR_CALL_ORDER_INVALID,
        // no state change) and completes the wait on its own. Cap the first
        // frame-thread wait for the former; wait longer, but still bounded,
        // for the latter (kTurboDrainTimeout, kTurboLateWaitTimeout).
        const bool diag_drain = TurboSequencedDebugTick();
        if (diag_drain) {
            logger_.Debug("Turbo-diag: async drain starting (250ms cap).");
        }
        const auto drain_start = std::chrono::steady_clock::now();
        turbo_trace_.Record("drain.enter", pending_async_generation);
        bool ready =
            pending_async_wait.wait_for(kTurboDrainTimeout) == std::future_status::ready;
        if (!ready) turbo_trace_.Burst();
        turbo_trace_.Record("drain.return", pending_async_generation, ready);
        const double drain_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drain_start)
                .count();
        frame_blocked_ms += drain_ms;
        if (diag_drain) {
            logger_.Debug("Turbo-diag: async drain completed in " + std::to_string(drain_ms) +
                          "ms, ready=" + std::to_string(ready ? 1 : 0));
        }
        const bool drain_timed_out = !ready;
        if (drain_timed_out && !frame_begun && !turbo_async_waits_before_begin_) {
            const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
            if (XR_SUCCEEDED(begin_result)) {
                // Interlocked runtime: this submit releases the pending wait.
                frame_begun = true;
            } else if (begin_result == XR_ERROR_CALL_ORDER_INVALID) {
                turbo_async_waits_before_begin_ = true;
                turbo_trace_.Record("drain.strict", pending_async_generation);
                logger_.Info("Turbo: the runtime requires the pipelined xrWaitFrame to complete before "
                             "xrBeginFrame; late waits are now waited for instead of submitted early.");
            } else {
                logger_.Error("Turbo: xrBeginFrame for a late async wait failed with " +
                              std::to_string(static_cast<int>(begin_result)));
                NoteTurboRuntimeFailure(begin_result, submitted_display_time, TurboFrameOperation::kBegin);
                turbo_last_frame_blocked_ms_ = frame_blocked_ms;
                abandon_end_frame_window();
                return begin_result;
            }
        }
        if (drain_timed_out && !frame_begun) {
            // A wait the runtime already held past the late bound (here or in
            // a bounded app poll) is not waited for again: each frame it stays
            // held costs the app only the drain.
            if (!pending_wait_held) {
                const auto late_start = std::chrono::steady_clock::now();
                ready = pending_async_wait.wait_for(kTurboLateWaitTimeout) == std::future_status::ready;
                const double late_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - late_start)
                        .count();
                frame_blocked_ms += late_ms;
                turbo_trace_.Record("drain.late", pending_async_generation, ready);
            }
            if (!ready) {
                // The runtime still owns this frame. Submitting without a begun
                // runtime frame is invalid, so drop the application's frame;
                // the retained wait keeps app frame calls intercepted and
                // forwarding resumes as soon as the runtime completes it.
                {
                    std::scoped_lock lock(turbo_mutex_);
                    turbo_async_held_generation_ = pending_async_generation;
                }
                // Every frame the runtime holds the wait is a stall, whatever
                // the app's cadence: the bounded waits themselves break that
                // cadence (a held wait costs the app whole seconds), so gating
                // on it let a held wait never fall back or suspend.
                if (turbo_engaged && !turbo_auto_suspended_.load(std::memory_order_relaxed) &&
                    HandleTurboDrainTimeout(std::chrono::steady_clock::now())) {
                    SoundPlayer::Instance().PlayTransition(turbo_sound, false, dll_directory_, turbo_sound_volume,
                                                           L"turbo-on.wav", L"turbo-off.wav");
                }
                if (turbo_dropped_frame_log_budget_ > 0) {
                    --turbo_dropped_frame_log_budget_;
                    logger_.Info("Turbo: the runtime's pipelined xrWaitFrame is still pending after " +
                                 std::to_string(static_cast<int>(frame_blocked_ms)) +
                                 "ms" + (pending_wait_held ? " more" : "") +
                                 "; this application frame was not submitted. Until the runtime completes "
                                 "the wait, application frames are dropped after a short bound instead of "
                                 "blocking; frame forwarding then resumes.");
                }
                turbo_trace_.Record("frame.dropped", pending_async_generation);
                turbo_last_frame_blocked_ms_ = frame_blocked_ms;
                abandon_end_frame_window();
                return XR_SUCCESS;
            }
        }
        XrResult async_wait_result = XR_SUCCESS;
        bool handed_over = false;
        {
            std::scoped_lock lock(turbo_mutex_);
            if (ready && pending_async_generation == turbo_async_wait_generation_) {
                async_wait_result = turbo_async_wait_result_;
                turbo_async_wait_ = {};
                if (turbo_seq_state_ == TurboSequencedState::kInactive) {
                    // Retire and shield atomically under turbo_mutex_: an app
                    // WaitFrame can observe the old future or the next shield,
                    // but never a pass-through gap between them.
                    if (async_release_reason || turbo_pacing_mode_ != TurboPacingMode::kAsync) {
                        // Releasing (or an Auto fallback to sequenced) must not
                        // forward a queued app Begin without its runtime wait.
                        // Hand the owned frame to the sequenced shield instead:
                        // this submit is followed by a frame-thread wait+begin,
                        // and app calls stay fabricated/swallowed exactly as in
                        // sequenced pacing.
                        turbo_seq_state_ = TurboSequencedState::kActive;
                        handed_over = true;
                    } else {
                        ArmTurboAsyncHandoffLocked("completed async wait retired before submit");
                    }
                }
            }
        }
        if (handed_over) {
            const char* reason = async_release_reason ? async_release_reason : "Auto switched to sequenced pacing";
            {
                std::scoped_lock lock(mutex_);
                turbo_pacing_mode_ = TurboPacingMode::kSequenced;
            }
            turbo_async_handed_over_ = true;
            turbo_async_recouple_stalled_ = false;
            logger_.Info(std::string("Turbo: async worker wait released (") + reason +
                         "); frame pacing continues on the application's frame thread for this session.");
        }
        if (ready && XR_FAILED(async_wait_result)) {
            // The worker's runtime wait failed, so the runtime holds no waited
            // frame. Keep app frames shielded and open this one on the frame
            // thread rather than returning to pass-through mid-frame.
            NoteTurboRuntimeFailure(async_wait_result, 0, TurboFrameOperation::kWait);
            if (!handed_over) {
                HandOverTurboToFrameThread("runtime xrWaitFrame failed");
                handed_over = true;
            }
            const XrResult opened = OpenTurboRuntimeFrame(session, submitted_display_time, frame_blocked_ms);
            if (XR_FAILED(opened)) {
                turbo_last_frame_blocked_ms_ = frame_blocked_ms;
                abandon_end_frame_window();
                return opened;
            }
            frame_begun = true;
        }
        // Only count while engaged with a healthy cadence: a drain-out during
        // a suspend, or a stall while the app itself is hitching/loading, is
        // not evidence against the pacing mode.
        if (drain_timed_out && turbo_engaged && cadence_countable &&
            !turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            timed_out = true;
            if (HandleTurboDrainTimeout(std::chrono::steady_clock::now())) {
                // Same audible cue as a manual turbo-off, so the user knows the
                // safety net fired. Non-blocking (worker-thread playback).
                SoundPlayer::Instance().PlayTransition(turbo_sound, false, dll_directory_, turbo_sound_volume,
                                                       L"turbo-on.wav", L"turbo-off.wav");
            }
        }
        // A re-coupled pipeline still waits off-thread. If that wait interlocks
        // with submission, Turbo being off must not keep the stall: release it
        // once this wait completes (the next EndFrame).
        if (drain_timed_out && !turbo_engaged && keep_async_pipeline && cadence_countable &&
            !turbo_async_recouple_stalled_) {
            turbo_async_recouple_stalled_ = true;
            logger_.Info("Turbo: async wait stalled while Turbo was off; releasing the async pipeline.");
        }

        if (!frame_begun) {
            // Deferred xrBeginFrame for the pipelined frame.
            const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
            if (XR_FAILED(begin_result)) {
                logger_.Error("Turbo: deferred xrBeginFrame failed with " +
                              std::to_string(static_cast<int>(begin_result)));
                NoteTurboRuntimeFailure(begin_result, submitted_display_time, TurboFrameOperation::kBegin);
                if (!handed_over) {
                    HandOverTurboToFrameThread("deferred xrBeginFrame failed");
                    handed_over = true;
                }
                if (begin_result != XR_ERROR_CALL_ORDER_INVALID) {
                    // The completed wait may still be unconsumed; probe it with
                    // a begin before any further wait.
                    std::scoped_lock lock(turbo_mutex_);
                    turbo_begin_probe_required_ = true;
                }
                // XR_ERROR_CALL_ORDER_INVALID means the runtime holds no waited
                // frame (its ownership view diverged from ours), so a fresh
                // frame-thread wait cannot stack on an unbegun frame.
                const XrResult opened = OpenTurboRuntimeFrame(session, submitted_display_time, frame_blocked_ms);
                if (XR_FAILED(opened)) {
                    turbo_last_frame_blocked_ms_ = frame_blocked_ms;
                    abandon_end_frame_window();
                    return opened;
                }
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
        // state transition (or a failed call left no runtime frame open) —
        // supply the runtime frame before the submit so the runtime's
        // sequence stays conformant.
        const XrResult opened = OpenTurboRuntimeFrame(session, submitted_display_time, frame_blocked_ms);
        if (XR_FAILED(opened)) {
            turbo_last_frame_blocked_ms_ = frame_blocked_ms;
            abandon_end_frame_window();
            return opened;
        }
    }

    const auto pacing_after_drain = std::chrono::steady_clock::now();
    const bool diag_end = TurboSequencedDebugTick();
    if (diag_end) {
        logger_.Debug("Turbo-diag: runtime xrEndFrame starting.");
    }
    if (turbo_engaged != turbo_trace_last_engaged_) {
        turbo_trace_.Burst();
        turbo_trace_.Record("turbo.state", 0, turbo_engaged);
        turbo_trace_last_engaged_ = turbo_engaged;
    }
    const XrResult result = TraceRuntimeEndFrame(session, frame_end_info);
    // Interception is armed by configuration before the first Turbo frame.
    // Only blame Turbo once it is engaged or has an established pipeline.
    if (turbo_engaged || pipeline_established) {
        if (XR_FAILED(result)) NoteTurboRuntimeFailure(result, frame_end_info ? frame_end_info->displayTime : 0);
        else turbo_runtime_error_streak_.store(0);
    }
    if (turbo_auto_suspended_.load()) turbo_engaged = false;
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
                            logger_.Info("Turbo compatibility notice: frame pipelining can prevent runtime "
                                         "reprojection or frame synthesis (including SteamVR Motion Smoothing). "
                                         "Disable Turbo first if presentation becomes unstable.");
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
                turbo_seq_wait_in_flight_ = do_steady_wait;
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
                const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
                const auto wait_start = std::chrono::steady_clock::now();
                XrResult wait_result = XR_SUCCESS;
                {
                    std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
                    wait_result = TraceRuntimeWaitFrame(session, &wait_info, &frame_state);
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
                        turbo_prediction_history_.NativeWait(frame_state.predictedDisplayTime,
                                                               frame_state.predictedDisplayPeriod);
                        turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
                        NoteTurboShouldRenderLocked(turbo_last_should_render_);
                        turbo_async_wait_completed_ = true;
                        turbo_seq_wait_in_flight_ = false;
                        pacing_wait_sum_ms_ += wait_ms;
                        pacing_wait_max_ms_ = std::max(pacing_wait_max_ms_, wait_ms);
                        ++pacing_wait_samples_;
                        // Post one pacing token (capped): a valve-closed app
                        // wait consumes it, re-coupling the app to this real
                        // pacing event.
                        turbo_pacing_tokens_ = 1;
                        turbo_valve_cv_.notify_all();
                    }
                    const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
                    const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
                    if (XR_SUCCEEDED(begin_result)) {
                        std::scoped_lock lock(turbo_mutex_);
                        turbo_frame_begun_ = true;
                    } else {
                        // The next EndFrame opens the frame (compensation). A
                        // rejection other than "no waited frame" may leave this
                        // wait unconsumed, so that path probes with a begin first.
                        logger_.Error("Turbo: sequenced xrBeginFrame failed with " +
                                      std::to_string(static_cast<int>(begin_result)));
                        if (begin_result != XR_ERROR_CALL_ORDER_INVALID) {
                            std::scoped_lock lock(turbo_mutex_);
                            turbo_begin_probe_required_ = true;
                        }
                        NoteTurboRuntimeFailure(begin_result, 0, TurboFrameOperation::kBegin);
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
                    {
                        std::scoped_lock lock(turbo_mutex_);
                        turbo_seq_wait_in_flight_ = false;
                    }
                    logger_.Error("Turbo: sequenced xrWaitFrame failed with " +
                                  std::to_string(static_cast<int>(wait_result)) +
                                  "; keeping previous frame timing.");
                    NoteTurboRuntimeFailure(wait_result, 0, TurboFrameOperation::kWait);
                }
            }
        } else if (turbo_engaged || keep_async_pipeline) {
            // Async pacing: background-thread wait, drained at the next
            // EndFrame.
            std::scoped_lock lock(turbo_mutex_);
            if (!turbo_async_wait_.valid() && turbo_seq_state_ == TurboSequencedState::kInactive) {
                if (!turbo_pipelining_logged_) {
                    logger_.Info("Turbo: frame pipelining engaged (async pacing); the app's xrWaitFrame "
                                 "is now decoupled from runtime pacing.");
                    logger_.Info("Turbo: async frame handoffs are gap-shielded for applications that overlap "
                                 "xrWaitFrame with xrEndFrame.");
                    logger_.Info("Turbo compatibility notice: frame pipelining can prevent runtime "
                                 "reprojection or frame synthesis (including SteamVR Motion Smoothing). "
                                 "Disable Turbo first if presentation becomes unstable.");
                    turbo_pipelining_logged_ = true;
                    turbo_fabricated_wait_log_budget_ = 5;
                }
                if (!turbo_async_handoff_active_) {
                    // Defensive recovery only: the normal path arms before
                    // xrEndFrame, where an overlapping app wait can see it.
                    ArmTurboAsyncHandoffLocked("post-submit async recovery");
                }
                turbo_async_wait_result_ = XR_SUCCESS;
                EnsureTurboAsyncWorkerLocked();
                turbo_async_job_completion_ = std::make_shared<std::promise<void>>();
                turbo_async_wait_ = turbo_async_job_completion_->get_future().share();
                turbo_async_worker_job_ = turbo_async_wait_;
                PublishTurboAsyncHandoffLocked();
                turbo_async_job_session_ = session;
                turbo_async_job_pending_ = true;
                turbo_trace_.Record("worker.queued", turbo_async_wait_generation_);
                turbo_async_worker_cv_.notify_one();
            }
        }

        if (turbo_engaged && !timed_out && cadence_countable &&
            !turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            NoteTurboPacingStableFrame(app_frame_delta_ms);
        }

        if (!turbo_engaged) {
            // Both established pipelines retain runtime frame ownership while
            // their valve restores app-visible pacing. A fabricated app wait
            // can already be queued on another thread when the toggle changes;
            // removing interception here would forward its unmatched begin.
            // Only an unestablished handshake can be discarded safely. An
            // async pipeline that must release (see async_release_reason)
            // hands over to sequenced ownership at its next retired wait.
            std::scoped_lock lock(turbo_mutex_);
            if (!keep_async_pipeline) {
                CancelTurboAsyncHandoffLocked("turbo disengaged before async publication");
            }
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
    } else {
        std::scoped_lock lock(turbo_mutex_);
        CancelTurboAsyncHandoffLocked("runtime xrEndFrame failed");
        if (turbo_auto_suspended_.load()) {
            turbo_valve_open_ = false;
            turbo_valve_cv_.notify_all();
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
        turbo_async_handoff_cv_.notify_all();
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
        const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
        if (XR_SUCCEEDED(begin_result)) {
            std::scoped_lock lock(turbo_mutex_);
            turbo_frame_begun_ = true;
        } else {
            // The app already saw XR_SUCCESS for the swallowed begin. The shield
            // is active, so the next EndFrame opens the frame itself; probe
            // first unless the runtime proved the handshake wait consumed.
            logger_.Error("Turbo: deferred establishment xrBeginFrame failed with " +
                          std::to_string(static_cast<int>(begin_result)));
            if (begin_result != XR_ERROR_CALL_ORDER_INVALID) {
                std::scoped_lock lock(turbo_mutex_);
                turbo_begin_probe_required_ = true;
            }
            NoteTurboRuntimeFailure(begin_result, 0, TurboFrameOperation::kBegin);
        }
    }

    // Feeds next frame's cadence gate: subtracting our own blocking from the
    // frame delta keeps a turbo-induced stall from masquerading as an app
    // hitch (which would pause the gate and stop the very counting that
    // should catch it).
    turbo_last_frame_blocked_ms_ = frame_blocked_ms;

    RecordTurboMetricsFrame(turbo_engaged, frame_blocked_ms, timed_out, metrics_mode, metrics_bindings,
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

namespace {

HeadPredictionPose ToHeadPose(const XrPosef& pose) {
    return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
            {pose.position.x, pose.position.y, pose.position.z}};
}

XrPosef ToXrPose(const HeadPredictionPose& pose) {
    return {{static_cast<float>(pose.orientation.x), static_cast<float>(pose.orientation.y),
             static_cast<float>(pose.orientation.z), static_cast<float>(pose.orientation.w)},
            {static_cast<float>(pose.position.x), static_cast<float>(pose.position.y),
             static_cast<float>(pose.position.z)}};
}

constexpr XrSpaceLocationFlags kHeadPredictionPoseFlags =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
// A probe is due at most this often until the runtime's limit settles.
constexpr auto kHeadPredictionProbeInterval = std::chrono::milliseconds(250);
constexpr auto kHeadPredictionSummaryInterval = std::chrono::seconds(30);

} // namespace

XrSpace OpenXrLayer::HeadPredictionWorldSpace(XrSpace space, XrSpace base_space) const {
    const TrackedSpaceKind space_kind = ClassifySpace(space);
    const TrackedSpaceKind base_kind = ClassifySpace(base_space);
    const auto world = [](TrackedSpaceKind kind) {
        return kind == TrackedSpaceKind::kLocal || kind == TrackedSpaceKind::kStage;
    };
    if (space_kind == TrackedSpaceKind::kView && world(base_kind)) {
        return base_space;
    }
    if (base_kind == TrackedSpaceKind::kView && world(space_kind)) {
        return space;
    }
    return XR_NULL_HANDLE;
}

OpenXrLayer::HeadPredictionPlan OpenXrLayer::PlanHeadPredictionLocked(bool allow_probe) {
    HeadPredictionPlan plan;
    if (!resolved_settings_.core.enabled || !resolved_settings_.framegen.extend_head_prediction ||
        !framegen_owns_pacing_.load(std::memory_order_acquire)) {
        return plan;
    }
    if (!turbo_convert_counter_time_ || internal_view_space_ == XR_NULL_HANDLE) {
        if (!head_prediction_logged_unavailable_) {
            head_prediction_logged_unavailable_ = true;
            logger_.Info(std::string("Head prediction past the runtime's limit is on, but unavailable: ") +
                         (!turbo_convert_counter_time_ ? "the runtime gives no clock to measure it against "
                                                         "(XR_KHR_win32_convert_performance_counter_time)."
                                                       : "no internal VIEW space."));
        }
        return plan;
    }
    plan.enabled = true;
    plan.view_space = internal_view_space_;
    if (head_prediction_limit_.Settled()) {
        plan.limited = !head_prediction_limit_.Unlimited();
        plan.limit = head_prediction_limit_.LimitNs();
        return plan;
    }
    const auto now = std::chrono::steady_clock::now();
    if (allow_probe && now - head_prediction_last_probe_ >= kHeadPredictionProbeInterval) {
        head_prediction_last_probe_ = now;
        plan.probe = true;
    }
    return plan;
}

std::optional<PredictionLimitEstimate> OpenXrLayer::ProbeRuntimePredictionLimit(const HeadPredictionPlan& plan,
                                                                               XrSpace world_space) {
    // The head, asked for at once at each offset past now. Never further
    // than Frame Generation already has the game ask for.
    const std::span<const std::int64_t> offsets = PredictionProbeOffsets();
    std::array<PredictionProbeSample, 13> samples{};
    if (offsets.size() != samples.size()) {
        return std::nullopt;
    }
    const XrTime now = TurboClockNow();
    if (now <= 0) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(next_locate_space_(plan.view_space, world_space, now + offsets[i], &location)) ||
            (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
            return std::nullopt;
        }
        samples[i].ahead_ns = offsets[i];
        samples[i].orientation = ToHeadPose(location.pose).orientation;
    }
    return EstimatePredictionLimit(samples);
}

void OpenXrLayer::NotePredictionProbeLocked(const std::optional<PredictionLimitEstimate>& estimate) {
    if (!estimate) {
        return;
    }
    if (logger_.IsDebugEnabled()) {
        logger_.Debug("Head prediction probe: " +
                      (estimate->unlimited ? std::string("still turning at 100 ms")
                                           : "stops at " + FormatDiagnosticDouble(estimate->limit_ns / 1e6) + " ms") +
                      ", turning " + FormatDiagnosticDouble(estimate->speed_degrees_per_second) +
                      " deg/s, fit " + FormatDiagnosticDouble(estimate->fit) + ".");
    }
    if (!head_prediction_limit_.Add(*estimate)) {
        return;
    }
    if (head_prediction_limit_.Unlimited()) {
        logger_.Info("Head prediction: " + runtime_name_ +
                     " predicts the head at least 100 ms ahead; nothing to extend (" +
                     std::to_string(head_prediction_limit_.Probes()) + " probes).");
    } else {
        logger_.Info("Head prediction: " + runtime_name_ + " predicts the head " +
                     FormatDiagnosticDouble(head_prediction_limit_.LimitNs() / 1e6) +
                     " ms past the moment it is asked and no further (" +
                     std::to_string(head_prediction_limit_.Probes()) +
                     " probes); the rest of each told display time is extended.");
    }
}

HeadExtension OpenXrLayer::ExtendHead(const HeadPredictionPlan& plan, XrSpace world_space, XrTime target,
                                      XrTime now) {
    const HeadPredictionTuning tuning{};
    if (!plan.limited || now <= 0 || target - now <= plan.limit) {
        return {};
    }
    // The runtime's answer for the target first, nearest the game's own;
    // then two inside its limit.
    HeadPredictionAnchors anchors;
    anchors.target = target;
    anchors.anchor_time = now + plan.limit - tuning.anchor_margin_ns;
    const XrTime times[3] = {target, anchors.anchor_time, anchors.anchor_time - tuning.velocity_span_ns};
    XrSpaceLocation located[3] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    for (int i = 0; i < 3; ++i) {
        if (XR_FAILED(next_locate_space_(plan.view_space, world_space, times[i], &located[i])) ||
            (located[i].locationFlags & kHeadPredictionPoseFlags) != kHeadPredictionPoseFlags) {
            return {};
        }
    }
    anchors.at_target = ToHeadPose(located[0].pose);
    anchors.anchor = ToHeadPose(located[1].pose).orientation;
    anchors.before = ToHeadPose(located[2].pose).orientation;
    return ComputeHeadExtension(anchors, tuning);
}

void OpenXrLayer::NoteHeadExtensionLocked(const HeadExtension& extension, XrTime target, XrTime now) {
    if (!head_prediction_limit_.Settled() || head_prediction_limit_.Unlimited() || now <= 0) {
        return;
    }
    const double beyond_ms = static_cast<double>(target - now - head_prediction_limit_.LimitNs()) / 1e6;
    if (beyond_ms <= 0.0) {
        return;
    }
    const auto wall = std::chrono::steady_clock::now();
    if (head_prediction_stats_since_ == std::chrono::steady_clock::time_point{}) {
        head_prediction_stats_since_ = wall;
    }
    HeadPredictionStats& stats = head_prediction_stats_;
    ++stats.locates;
    stats.beyond_sum_ms += beyond_ms;
    stats.beyond_max_ms = std::max(stats.beyond_max_ms, beyond_ms);
    if (extension.applied) {
        ++stats.extended;
        stats.degrees_sum += extension.degrees;
        stats.degrees_max = std::max(stats.degrees_max, extension.degrees);
    }
    head_prediction_beyond_ms_ = head_prediction_beyond_ms_ <= 0.0
        ? beyond_ms
        : head_prediction_beyond_ms_ + 0.05 * (beyond_ms - head_prediction_beyond_ms_);
    if (wall - head_prediction_stats_since_ < kHeadPredictionSummaryInterval) {
        return;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << "Head prediction: " << stats.locates
           << " view locates asked " << stats.beyond_sum_ms / static_cast<double>(stats.locates) << " ms (max "
           << stats.beyond_max_ms << ") past the runtime's " << head_prediction_limit_.LimitNs() / 1e6
           << " ms; " << stats.extended << " turned by "
           << (stats.extended > 0 ? stats.degrees_sum / static_cast<double>(stats.extended) : 0.0)
           << " deg on average (max " << stats.degrees_max << "); the others found the head (nearly) still.";
    logger_.Info(stream.str());
    head_prediction_stats_ = {};
    head_prediction_stats_since_ = wall;
}

std::string OpenXrLayer::HeadPredictionOsdDetail() const {
    if (!resolved_settings_.core.enabled || !resolved_settings_.framegen.extend_head_prediction ||
        !framegen_owns_pacing_.load(std::memory_order_acquire)) {
        return {};
    }
    if (!turbo_convert_counter_time_) {
        return "Head: no runtime clock";
    }
    if (!head_prediction_limit_.Settled()) {
        return "Head: measuring";
    }
    if (head_prediction_limit_.Unlimited()) {
        return "Head: runtime predicts";
    }
    std::ostringstream stream;
    stream << "Head +" << static_cast<int>(std::lround(head_prediction_beyond_ms_)) << " ms";
    return stream.str();
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
// empirically tested sidecar verdict > async-first probe.
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

    const TurboPacingSetting setting = turbo_experiment_.enabled ? TurboPacingSetting::kAsync
                                                               : resolved_settings_.turbo.pacing_mode;
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

    if (const auto observation = FindRuntimePacingObservation(ResolveRuntimePacingPath(),
                                                              runtime_name_,
                                                              system_name_,
                                                              system_vendor_id_,
                                                              graphics_api_)) {
        turbo_pacing_mode_ = observation->mode;
        turbo_pacing_source_ = TurboPacingSource::kDiscovered;
        logger_.Info(std::string("Turbo pacing: ") + ToString(turbo_pacing_mode_) +
                     " (recorded verdict for runtime \"" + runtime_name_ + "\", source=" +
                     observation->source + ").");
        RuntimePacingObservation updated = *observation;
        updated.runtime_version = runtime_version_;
        updated.system_name = system_name_;
        updated.vendor_id = system_vendor_id_;
        updated.graphics_api = graphics_api_;
        updated.last_used_unix_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        QueueRuntimePacingWrite(std::move(updated));
        return;
    }

    turbo_pacing_mode_ = TurboPacingMode::kAsync;
    turbo_pacing_source_ = TurboPacingSource::kProbing;
    turbo_pacing_verdict_pending_ = true;
    logger_.Info("Turbo pacing: no tested decision for runtime \"" + runtime_name_ +
                 "\"; probing async pacing first (falls back to sequenced on stalls or submission errors).");
}

void OpenXrLayer::RecordTurboPacingVerdict(TurboPacingMode mode,
                                           const char* source,
                                           std::int64_t stable_seconds) {
    if (runtime_name_.empty() || turbo_experiment_.enabled) {
        return;
    }
    RuntimePacingObservation observation;
    observation.test_started_at = turbo_test_started_at_;
    observation.runtime_name = runtime_name_;
    observation.runtime_version = runtime_version_;
    observation.system_name = system_name_;
    observation.vendor_id = system_vendor_id_;
    observation.graphics_api = graphics_api_;
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

    QueueRuntimePacingWrite(std::move(observation));
    logger_.Info(std::string("Turbo pacing: queued ") + ToString(mode) +
                 " verdict for runtime \"" + runtime_name_ + "\" (" + source + ").");
}

void OpenXrLayer::QueueRuntimePacingWrite(RuntimePacingObservation observation) {
    if (runtime_pacing_write_future_.valid()) {
        if (runtime_pacing_write_future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            pending_runtime_pacing_write_ = std::move(observation);
            return;
        }
        runtime_pacing_write_future_.get();
        runtime_pacing_write_future_ = {};
    }

    runtime_pacing_write_future_ =
        std::async(std::launch::async, [this, path = ResolveRuntimePacingPath(), observation = std::move(observation)] {
            std::string error;
            if (!RecordRuntimePacingObservation(path, observation, &error)) {
                logger_.Info("Turbo pacing: background verdict write failed: " + error);
            }
        });
}

void OpenXrLayer::DrainRuntimePacingWrites() {
    if (runtime_pacing_write_future_.valid()) {
        runtime_pacing_write_future_.wait();
        runtime_pacing_write_future_ = {};
    }
    if (pending_runtime_pacing_write_.has_value()) {
        RuntimePacingObservation pending = std::move(*pending_runtime_pacing_write_);
        pending_runtime_pacing_write_.reset();
        std::string error;
        if (!RecordRuntimePacingObservation(ResolveRuntimePacingPath(), pending, &error)) {
            logger_.Info("Turbo pacing: final verdict write failed: " + error);
        }
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
    const char* source = "discovered";
    if (turbo_pacing_source_ == TurboPacingSource::kProbing ||
        turbo_pacing_source_ == TurboPacingSource::kFallback) {
        turbo_pacing_source_ = TurboPacingSource::kDiscovered;
    }
    RecordTurboPacingVerdict(turbo_pacing_mode_, source,
                             static_cast<std::int64_t>(turbo_stable_accumulated_ms_ / 1000.0));
}

bool OpenXrLayer::TryTurboAutoFallbackLocked(const std::string& reason) {
    if (turbo_pacing_mode_ != TurboPacingMode::kAsync ||
        turbo_pacing_source_ == TurboPacingSource::kForced || turbo_pacing_source_ == TurboPacingSource::kPinned) return false;
    turbo_recovery_.Fail(reason + " Auto is trying sequenced pacing next.");
    if (!turbo_recovery_.Prepare(resolved_settings_.turbo.interrupted_session_recovery, true, "sequenced")) return false;
    turbo_pacing_mode_ = TurboPacingMode::kSequenced;
    turbo_pacing_source_ = TurboPacingSource::kFallback;
    turbo_pacing_verdict_pending_ = true;
    turbo_stable_accumulated_ms_ = 0.0;
    turbo_runtime_error_streak_.store(0);
    turbo_drain_timeout_count_ = 0;
    turbo_timeout_window_start_.reset();
    logger_.Info("Turbo Auto: async failed; trying sequenced. reason=" + reason);
    return true;
}

void OpenXrLayer::NoteTurboRuntimeFailure(XrResult result, XrTime submitted_display_time,
                                          TurboFrameOperation operation) {
    std::scoped_lock lock(mutex_);
    // A failure breaks the stability window even below the suspension threshold.
    turbo_stable_accumulated_ms_ = 0.0;
    if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST ||
        result == XR_ERROR_SESSION_NOT_RUNNING) return;
    const char* operation_name = operation == TurboFrameOperation::kWait ? "xrWaitFrame" :
                                 operation == TurboFrameOperation::kBegin ? "xrBeginFrame" : "xrEndFrame";
    const char* fault_event = operation == TurboFrameOperation::kWait ? "fault.wait" :
                              operation == TurboFrameOperation::kBegin ? "fault.begin" : "fault.end";
    // Every failure reaches the flight recorder (which freezes its history on
    // fault events), even below the suspension threshold.
    turbo_trace_.Burst();
    if (operation == TurboFrameOperation::kEnd && result == XR_ERROR_TIME_INVALID) {
        std::optional<TurboPredictionHistory::Prediction> prediction;
        XrTime native_snapshot = 0;
        std::uint64_t native_generation_snapshot = 0;
        bool log_prediction = false;
        {
            std::scoped_lock timing_lock(turbo_mutex_);
            if (turbo_prediction_fault_log_budget_ > 0) {
                --turbo_prediction_fault_log_budget_;
                log_prediction = true;
                prediction = turbo_prediction_history_.Find(submitted_display_time);
                native_snapshot = turbo_last_predicted_display_time_;
                native_generation_snapshot = turbo_prediction_history_.NativeGeneration();
            }
        }
        if (log_prediction) {
            std::string detail = "Turbo rejected display time: submitted=" + std::to_string(submitted_display_time) +
                ", nativeAtFailureSnapshot=" + std::to_string(native_snapshot) +
                ", nativeGenerationAtFailureSnapshot=" + std::to_string(native_generation_snapshot);
            if (prediction) {
                detail += ", appWait=" + std::to_string(prediction->app_wait) +
                    ", origin=" + TurboPredictionHistory::Name(prediction->origin) +
                    ", nativeAtWait=" + std::to_string(prediction->native_display_time) +
                    ", nativeGenerationAtWait=" + std::to_string(prediction->native_generation) +
                    ", nativePeriodAtWait=" + std::to_string(prediction->native_period);
                turbo_trace_.Record("fault.prediction", prediction->trace_id, prediction->display_time,
                                    prediction->native_display_time, prediction->native_generation);
            } else {
                detail += ", origin=not-in-recent-app-waits";
            }
            // This correlation is diagnostic, not a runtime validity rule:
            // accepted app predictions can differ from the latest native wait.
            logger_.Info(detail + ".");
        }
    }
    turbo_trace_.Record(fault_event, 0, result, submitted_display_time);
    // Ownership is re-established structurally by ForwardEndFrame; frame
    // forwarding never stops. Repeated failures still end Turbo's pacing.
    if (++turbo_runtime_error_streak_ < 3 || turbo_auto_suspended_.load()) return;
    const std::string error = result == XR_ERROR_TIME_INVALID
        ? "XR_ERROR_TIME_INVALID (-30): runtime rejected the submitted display time"
        : result == XR_ERROR_CALL_ORDER_INVALID
        ? "XR_ERROR_CALL_ORDER_INVALID (-37): runtime frame call order rejected"
        : "runtime error " + std::to_string(static_cast<int>(result));
    const std::string reason = "Three consecutive runtime frame-call failures in " +
        std::string(ToString(turbo_pacing_mode_)) + " pacing; the last was " + operation_name + ": " +
        error + ".";
    {
        std::scoped_lock timing_lock(turbo_mutex_);
        logger_.Info("Turbo failure timing: submittedDisplayTime=" + std::to_string(submitted_display_time) +
            ", latestRuntimeDisplayTime=" + std::to_string(turbo_last_predicted_display_time_) +
            ", displayPeriod=" + std::to_string(turbo_last_predicted_display_period_) + ".");
    }
    if (TryTurboAutoFallbackLocked(reason)) {
        // A rejected submit cannot take the normal success-only establishment
        // path. Let the application's next WaitFrame establish sequenced mode.
        std::scoped_lock timing_lock(turbo_mutex_);
        if (!turbo_async_wait_.valid() && turbo_seq_state_ == TurboSequencedState::kInactive)
            turbo_seq_state_ = TurboSequencedState::kEngaging;
        return;
    }
    turbo_auto_suspended_.store(true);
    turbo_effective_active_.store(false);
    turbo_pacing_verdict_pending_ = false;
    turbo_recovery_.Fail(reason + " Turbo suspended for this session; frames continue at runtime pacing.");
    {
        std::scoped_lock timing_lock(turbo_mutex_);
        turbo_valve_open_ = false;
        turbo_async_handoff_cv_.notify_all();
        turbo_valve_cv_.notify_all();
    }
    logger_.Error("Turbo: " + reason + " Turbo suspended.");
    SoundPlayer::Instance().PlayTransition(ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings).sound, false,
        dll_directory_, resolved_settings_.core.sound_volume, L"turbo-on.wav", L"turbo-off.wav");
}

XrResult OpenXrLayer::OpenTurboRuntimeFrame(XrSession session, XrTime display_time, double& frame_blocked_ms) {
    const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    bool probe = false;
    {
        std::scoped_lock lock(turbo_mutex_);
        probe = turbo_begin_probe_required_;
    }
    if (probe) {
        // xrBeginFrame never blocks and changes nothing when no waited frame
        // exists, so it settles the uncertainty before any further wait.
        const XrResult probe_result = TraceRuntimeBeginFrame(session, &begin_info);
        turbo_trace_.Record("recover.probe", 0, probe_result);
        if (XR_SUCCEEDED(probe_result) || probe_result == XR_ERROR_CALL_ORDER_INVALID) {
            std::scoped_lock lock(turbo_mutex_);
            turbo_begin_probe_required_ = false;
        }
        if (XR_SUCCEEDED(probe_result)) {
            logger_.Info("Turbo: resumed the runtime's pending frame after a failed xrBeginFrame.");
            return XR_SUCCESS;
        }
        if (probe_result != XR_ERROR_CALL_ORDER_INVALID) {
            logger_.Error("Turbo: xrBeginFrame probe failed with " +
                          std::to_string(static_cast<int>(probe_result)));
            NoteTurboRuntimeFailure(probe_result, display_time, TurboFrameOperation::kBegin);
            return probe_result;
        }
    }

    const bool diag_comp = TurboSequencedDebugTick();
    if (diag_comp) {
        logger_.Debug("Turbo-diag: compensation xrWaitFrame starting (frame thread).");
    }
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    const auto comp_start = std::chrono::steady_clock::now();
    XrResult wait_result = XR_SUCCESS;
    {
        std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
        wait_result = TraceRuntimeWaitFrame(session, &wait_info, &frame_state);
    }
    const double comp_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - comp_start).count();
    frame_blocked_ms += comp_ms;
    if (diag_comp) {
        logger_.Debug("Turbo-diag: compensation xrWaitFrame completed in " + std::to_string(comp_ms) +
                      "ms, result=" + std::to_string(static_cast<int>(wait_result)));
    }
    if (XR_FAILED(wait_result)) {
        logger_.Error("Turbo: compensation xrWaitFrame failed with " +
                      std::to_string(static_cast<int>(wait_result)) + ".");
        NoteTurboRuntimeFailure(wait_result, 0, TurboFrameOperation::kWait);
        return wait_result;
    }
    {
        std::scoped_lock lock(turbo_mutex_);
        turbo_last_predicted_display_time_ = frame_state.predictedDisplayTime;
        turbo_last_predicted_display_period_ = frame_state.predictedDisplayPeriod;
        turbo_prediction_history_.NativeWait(frame_state.predictedDisplayTime,
                                               frame_state.predictedDisplayPeriod);
        turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
        NoteTurboShouldRenderLocked(turbo_last_should_render_);
    }
    const XrResult begin_result = TraceRuntimeBeginFrame(session, &begin_info);
    if (XR_SUCCEEDED(begin_result)) {
        return XR_SUCCESS;
    }
    logger_.Error("Turbo: compensation xrBeginFrame failed with " +
                  std::to_string(static_cast<int>(begin_result)));
    if (begin_result != XR_ERROR_CALL_ORDER_INVALID) {
        std::scoped_lock lock(turbo_mutex_);
        turbo_begin_probe_required_ = true;
    }
    NoteTurboRuntimeFailure(begin_result, display_time, TurboFrameOperation::kBegin);
    return begin_result;
}

void OpenXrLayer::HandOverTurboToFrameThread(const char* reason) {
    {
        std::scoped_lock lock(turbo_mutex_);
        // The shield never lapses: the sequenced state takes over before the
        // async handoff is released, and a second app poll waiting on that
        // handoff stays fabricated (see WaitFrame).
        turbo_seq_state_ = TurboSequencedState::kActive;
        turbo_begin_owed_ = false;
        turbo_async_handoff_active_ = false;
        turbo_async_handoff_cv_.notify_all();
    }
    {
        std::scoped_lock lock(mutex_);
        turbo_pacing_mode_ = TurboPacingMode::kSequenced;
        turbo_recovery_.NoteRecovered(std::string("Async pacing recovered from a runtime frame-call failure (") +
            reason + "); the session continued with sequenced pacing.");
    }
    turbo_async_handed_over_ = true;
    turbo_async_recouple_stalled_ = false;
    turbo_trace_.Record("recover.handover");
    logger_.Info(std::string("Turbo: async pacing handed to the application's frame thread (") + reason +
                 "); frames continue with sequenced pacing for this session.");
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
        // the fallback; a recorded async verdict gets more benefit of
        // the doubt before we conclude conditions changed.
        const int threshold = turbo_pacing_source_ == TurboPacingSource::kProbing ? 2 : 5;
        if (turbo_drain_timeout_count_ < threshold) {
            return false;
        }
        std::scoped_lock lock(mutex_);
        if (TryTurboAutoFallbackLocked("Repeated xrWaitFrame stalls in async pacing.")) return false;
    }

    // Level 2 (sequenced pacing still stalling), or a forced/pinned mode the
    // user asked us not to adapt: suspend turbo for the session.
    const int threshold = turbo_pacing_mode_ == TurboPacingMode::kSequenced ? 3 : 5;
    if (turbo_drain_timeout_count_ < threshold) {
        return false;
    }
    turbo_auto_suspended_.store(true, std::memory_order_relaxed);
    {
        std::scoped_lock lock(mutex_);
        turbo_recovery_.Fail("Repeated xrWaitFrame stalls in " + std::string(ToString(turbo_pacing_mode_)) +
            " pacing (" + std::to_string(turbo_drain_timeout_count_) + " stalls within 30 seconds).");
    }
    logger_.Info(std::string("Turbo: ") +
                 (turbo_pacing_mode_ == TurboPacingMode::kAsync
                      ? "the pipelined xrWaitFrame repeatedly stalled until the next frame submit"
                      : "the sequenced xrWaitFrame stalled even after the frame submit") +
                 "; auto-suspending turbo for this session. Press the turbo toggle binding to re-arm it.");
    turbo_pacing_verdict_pending_ = false;
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
        double submit_delta_sum_periods = 0.0;
        double submit_delta_min_periods = 0.0;
        double submit_delta_max_periods = 0.0;
        uint32_t submit_delta_samples = 0;
        std::uint64_t async_handoff_armed = 0;
        std::uint64_t async_handoff_waits = 0;
        std::uint64_t async_handoff_begins = 0;
        std::uint64_t async_handoff_second_poll_blocks = 0;
        std::uint64_t async_handoff_cancellations = 0;
        bool async_handoff_active = false;
        {
            std::scoped_lock lock(turbo_mutex_);
            wait_sum_ms = pacing_wait_sum_ms_;
            wait_max_ms = pacing_wait_max_ms_;
            wait_samples = pacing_wait_samples_;
            fabricated_waits = pacing_fabricated_waits_;
            submit_delta_sum_periods = pacing_submit_delta_sum_periods_;
            submit_delta_min_periods = pacing_submit_delta_min_periods_;
            submit_delta_max_periods = pacing_submit_delta_max_periods_;
            submit_delta_samples = pacing_submit_delta_samples_;
            async_handoff_armed = turbo_async_handoff_armed_total_;
            async_handoff_waits = turbo_async_handoff_wait_intercepts_;
            async_handoff_begins = turbo_async_handoff_begin_intercepts_;
            async_handoff_second_poll_blocks = turbo_async_handoff_second_poll_blocks_;
            async_handoff_cancellations = turbo_async_handoff_cancellations_;
            async_handoff_active = turbo_async_handoff_active_;
            pacing_submit_delta_sum_periods_ = 0.0;
            pacing_submit_delta_min_periods_ = 0.0;
            pacing_submit_delta_max_periods_ = 0.0;
            pacing_submit_delta_samples_ = 0;
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
               << "/" << FormatDiagnosticDouble(pacing_delta_max_ms_) << "ms";
        if (framegen_owns_pacing_.load(std::memory_order_acquire)) {
            // The game's xrEndFrame ends in Frame Generation, which paces it:
            // that time is FG holding the game to its rate, not a runtime
            // stall, and Turbo's waits, handoffs and display-time checks do
            // not run.
            stream << ", framegen=1, frameGenEndFrame avg/max="
                   << FormatDiagnosticDouble(pacing_end_sum_ms_ / pacing_frames_) << "/"
                   << FormatDiagnosticDouble(pacing_end_max_ms_) << "ms";
        } else {
            stream << ", preRuntimeEndFrame avg/max="
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
            // Only where a handoff can arm: an engaged async pipeline.
            if ((turbo_engaged && turbo_pacing_mode_ == TurboPacingMode::kAsync) || async_handoff_armed > 0) {
                stream << ", asyncHandoff active/armed/waits/begins/secondPollBlocks/cancels="
                       << (async_handoff_active ? 1 : 0) << "/" << async_handoff_armed << "/"
                       << async_handoff_waits << "/" << async_handoff_begins << "/"
                       << async_handoff_second_poll_blocks << "/" << async_handoff_cancellations;
            }
            if (submit_delta_samples > 0) {
                stream << ", submittedDisplayTimeVsLatestWait avg/min/max="
                       << FormatDiagnosticDouble(submit_delta_sum_periods / submit_delta_samples) << "/"
                       << FormatDiagnosticDouble(submit_delta_min_periods) << "/"
                       << FormatDiagnosticDouble(submit_delta_max_periods) << " periods";
            }
        }
        logger_.Debug(stream.str());
    } else {
        // Keep the WaitFrame-side counters bounded even when debug is off.
        std::scoped_lock lock(turbo_mutex_);
        pacing_wait_sum_ms_ = 0.0;
        pacing_wait_max_ms_ = 0.0;
        pacing_wait_samples_ = 0;
        pacing_fabricated_waits_ = 0;
        pacing_submit_delta_sum_periods_ = 0.0;
        pacing_submit_delta_min_periods_ = 0.0;
        pacing_submit_delta_max_periods_ = 0.0;
        pacing_submit_delta_samples_ = 0;
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
                                          const std::vector<InputBinding>& metrics_bindings,
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
          IsTurboMetricsCaptureArmed(metrics_bindings, sound_volume)));
    turbo_metrics_active_.store(capturing);
    if (!capturing) {
        if (turbo_metrics_was_capturing_) {
            // Queue the entire tail even if a periodic write is still busy.
            // No later EndFrame or clean application shutdown is required.
            turbo_metrics_dirty_ = true;
            FlushTurboMetrics(false, true);
            logger_.Info("Turbo metrics: paused capture snapshot queued (live=false).");
        }
        turbo_metrics_was_capturing_ = false;
        return;
    }

    const bool resuming = !turbo_metrics_was_capturing_ && !turbo_metrics_session_id_.empty();
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
    if (resuming) {
        FlushTurboMetrics(false, true);
        turbo_metrics_last_flush_time_ = now;
    } else if (!turbo_metrics_last_flush_time_.has_value()) {
        turbo_metrics_last_flush_time_ = now;
    } else if (now - *turbo_metrics_last_flush_time_ >= kMetricsFlushInterval) {
        FlushTurboMetrics(false);
        turbo_metrics_last_flush_time_ = now;
    }
}

bool OpenXrLayer::IsTurboMetricsCaptureArmed(const std::vector<InputBinding>& bindings, int sound_volume) {
    if (turbo_metrics_binding_reset_pending_.exchange(false, std::memory_order_acq_rel)) {
        turbo_metrics_capture_armed_ = false;
        turbo_metrics_binding_was_down_ = false;
        turbo_metrics_binding_last_poll_time_.reset();
        turbo_metrics_binding_down_cached_ = false;
        turbo_metrics_pressed_.reset();
    }
#if defined(_WIN32)
    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detector on the first poll so a button held from the
    // bind gesture does not register as a press (same as the turbo toggle).
    const bool first_poll = !turbo_metrics_binding_last_poll_time_.has_value();
    if (first_poll || now - *turbo_metrics_binding_last_poll_time_ >= kInputBindingPollInterval) {
        turbo_metrics_binding_last_poll_time_ = now;
        const InputBinding* down = PollInputBindingsDown(bindings);
        turbo_metrics_binding_down_cached_ = down != nullptr;
        if (down) {
            turbo_metrics_pressed_ = *down;
        }
    }
    const bool binding_down = turbo_metrics_binding_down_cached_;
    if (first_poll) {
        turbo_metrics_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !turbo_metrics_binding_was_down_;
    turbo_metrics_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        const InputBinding binding = ActingBinding(turbo_metrics_pressed_, bindings);
        turbo_metrics_capture_armed_ = !turbo_metrics_capture_armed_;
        logger_.Info(std::string("Turbo metrics: capture ") +
                     (turbo_metrics_capture_armed_ ? "started" : "paused") + " via " +
                     BindingLabel(binding) + ".");
        SoundPlayer::Instance().PlayTransition(binding.sound, turbo_metrics_capture_armed_,
                                               dll_directory_, sound_volume, L"metrics-on.wav",
                                               L"metrics-off.wav");
    }
#else
    (void)bindings;
    (void)sound_volume;
#endif
    return turbo_metrics_capture_armed_;
}

void OpenXrLayer::FlushTurboMetrics(bool final_flush, bool queue_after_pending) {
    if (turbo_metrics_session_id_.empty() || (!turbo_metrics_dirty_ && !final_flush)) {
        return;
    }

    TurboMetricsSession session;
    session.session_id = turbo_metrics_session_id_;
    session.app_name = current_exe_name_;
    session.runtime_name = runtime_name_;
    session.layer_version = VECTORXR_VERSION;
    session.collection_mode = ToString(turbo_metrics_collection_mode_);
    session.timing_configuration = TurboTimingConfiguration();
    session.live = !final_flush && turbo_metrics_active_.load();
    session.process_id = GetCurrentProcessId();
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
        if (!queue_after_pending && turbo_metrics_write_future_.valid() &&
            turbo_metrics_write_future_.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
            return;
        }
        // Move the old future into the new worker before replacing it: destroying
        // an unfinished std::async future on the frame thread would block it.
        auto previous = std::move(turbo_metrics_write_future_);
        turbo_metrics_write_future_ =
            std::async(std::launch::async,
                       [this, path = ResolveTurboMetricsPath(), snapshot = std::move(session),
                        previous = std::move(previous)]() mutable {
                           if (previous.valid()) previous.wait();
                           std::string error;
                           if (!RecordTurboMetricsSession(path, snapshot, &error)) {
                               logger_.Info("Turbo metrics: background session write failed: " + error);
                           }
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

void OpenXrLayer::ArmTurboAsyncHandoffLocked(const char* reason) {
    if (turbo_async_handoff_active_) {
        return;
    }
    turbo_async_handoff_active_ = true;
    turbo_async_wait_polled_ = false;
    turbo_async_wait_completed_ = false;
    ++turbo_async_wait_generation_;
    ++turbo_async_handoff_armed_total_;
    turbo_trace_.Record("handoff.arm", turbo_async_wait_generation_);
    if (turbo_async_handoff_armed_total_ == 1) {
        // Capture establishment and the first several steady-state handoffs,
        // then rely on the five-second aggregate so debug logging cannot turn
        // into a per-frame pacing cost.
        turbo_async_handoff_debug_log_budget_ = 64;
    }
    if (logger_.IsDebugEnabled() && turbo_async_handoff_debug_log_budget_ > 0) {
        --turbo_async_handoff_debug_log_budget_;
        logger_.Debug("Turbo-diag: async handoff shield armed; reason=" +
                      std::string(reason ? reason : "unknown") +
                      ", generation=" + std::to_string(turbo_async_wait_generation_) +
                      ", armedTotal=" + std::to_string(turbo_async_handoff_armed_total_) + ".");
    }
}

void OpenXrLayer::PublishTurboAsyncHandoffLocked() {
    if (!turbo_async_handoff_active_) {
        return;
    }
    if (!turbo_async_wait_.valid()) {
        CancelTurboAsyncHandoffLocked("publication missing worker future");
        return;
    }
    turbo_trace_.Record("handoff.publish", turbo_async_wait_generation_);
    turbo_async_handoff_active_ = false;
    turbo_async_handoff_cv_.notify_all();
    if (logger_.IsDebugEnabled() && turbo_async_handoff_debug_log_budget_ > 0) {
        --turbo_async_handoff_debug_log_budget_;
        logger_.Debug("Turbo-diag: async handoff published runtime wait; generation=" +
                      std::to_string(turbo_async_wait_generation_) +
                      ", interceptedWaits=" + std::to_string(turbo_async_handoff_wait_intercepts_) +
                      ", interceptedBegins=" + std::to_string(turbo_async_handoff_begin_intercepts_) + ".");
    }
}

void OpenXrLayer::CancelTurboAsyncHandoffLocked(const char* reason) {
    if (!turbo_async_handoff_active_) {
        return;
    }
    turbo_async_handoff_active_ = false;
    ++turbo_async_handoff_cancellations_;
    turbo_trace_.Burst();
    turbo_trace_.Record("handoff.cancel", turbo_async_wait_generation_);
    turbo_async_handoff_cv_.notify_all();
    logger_.Info("Turbo: async handoff shield cancelled; reason=" +
                 std::string(reason ? reason : "unknown") +
                 ", generation=" + std::to_string(turbo_async_wait_generation_) +
                 ", cancellations=" + std::to_string(turbo_async_handoff_cancellations_) + ".");
}

void OpenXrLayer::EnsureTurboAsyncWorkerLocked() {
    if (turbo_async_worker_.joinable()) {
        return;
    }
    turbo_async_worker_stop_ = false;
    const std::uint64_t epoch = turbo_async_worker_epoch_;
    turbo_async_worker_ = std::thread([this, epoch] { TurboAsyncWorkerLoop(epoch); });
}

void OpenXrLayer::TurboAsyncWorkerLoop(std::uint64_t epoch) {
    for (;;) {
        XrSession session = XR_NULL_HANDLE;
        std::uint64_t generation = 0;
        std::shared_ptr<std::promise<void>> completion;
        {
            std::unique_lock lock(turbo_mutex_);
            turbo_async_worker_cv_.wait(lock, [this, epoch] {
                return turbo_async_worker_stop_ || epoch != turbo_async_worker_epoch_ ||
                       turbo_async_job_pending_;
            });
            if (turbo_async_worker_stop_ || epoch != turbo_async_worker_epoch_) {
                return;
            }
            session = turbo_async_job_session_;
            generation = turbo_async_wait_generation_;
            completion = std::move(turbo_async_job_completion_);
            turbo_async_job_pending_ = false;
        }

        turbo_trace_.Record("worker.job", generation);
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrResult wait_result = XR_SUCCESS;
        {
            std::scoped_lock wait_lock(turbo_runtime_wait_mutex_);
            wait_result = TraceRuntimeWaitFrame(session, &wait_info, &frame_state);
        }
        {
            std::scoped_lock state_lock(turbo_mutex_);
            // A wait that outlived its session's teardown (generation bumped)
            // must not publish into a later frame's state.
            if (generation == turbo_async_wait_generation_) {
                if (XR_SUCCEEDED(wait_result)) {
                    turbo_last_predicted_display_time_ = frame_state.predictedDisplayTime;
                    turbo_last_predicted_display_period_ = frame_state.predictedDisplayPeriod;
                    turbo_prediction_history_.NativeWait(frame_state.predictedDisplayTime,
                                                           frame_state.predictedDisplayPeriod);
                    turbo_last_should_render_ = frame_state.shouldRender == XR_TRUE;
                    NoteTurboShouldRenderLocked(turbo_last_should_render_);
                } else {
                    logger_.Error("Turbo: async xrWaitFrame failed with " +
                                  std::to_string(static_cast<int>(wait_result)) +
                                  "; keeping previous frame timing.");
                }
                turbo_async_wait_result_ = wait_result;
                turbo_async_wait_completed_ = true;
            }
        }
        turbo_trace_.Record("worker.ready", generation, wait_result);
        if (completion) {
            completion->set_value();
        }
    }
}

void OpenXrLayer::StopTurboAsyncWorker() {
    constexpr std::chrono::milliseconds kWorkerDrainTimeout{2000};
    // The worker runs one job at a time, so its latest job completing means
    // it is idle. Not turbo_async_wait_: session teardown clears that after
    // its own bounded drain even while the wait is still in the runtime, and
    // joining such a worker would hang xrDestroyInstance.
    std::shared_future<void> last_job;
    {
        std::scoped_lock lock(turbo_mutex_);
        last_job = turbo_async_worker_job_;
    }
    const bool drained = !last_job.valid() || last_job.wait_for(kWorkerDrainTimeout) == std::future_status::ready;
    {
        std::scoped_lock lock(turbo_mutex_);
        turbo_async_worker_stop_ = true;
        ++turbo_async_worker_epoch_;
        turbo_async_worker_cv_.notify_all();
    }
    if (turbo_async_worker_.joinable()) {
        if (drained) {
            turbo_async_worker_.join();
        } else {
            // The worker is blocked inside the runtime and joining could hang
            // application exit. Its epoch is stale, so it exits on return.
            logger_.Error("Turbo: the async worker is still inside the runtime's xrWaitFrame at teardown; "
                          "releasing it without joining.");
            // It may return after the loader unloads this layer and the
            // runtime: keep both loaded for the process's exit.
            PinModulesForAbandonedPresenter();
            turbo_async_worker_.detach();
        }
    }
    {
        std::scoped_lock lock(turbo_mutex_);
        turbo_async_worker_stop_ = false;
        turbo_async_job_pending_ = false;
        turbo_async_job_session_ = XR_NULL_HANDLE;
        turbo_async_job_completion_.reset();
        turbo_async_worker_job_ = {};
    }
}

bool OpenXrLayer::DrainTurboAsyncWait(std::chrono::milliseconds timeout) {
    // Wait without consuming the future: the pipelined frame is still pending
    // and will be begun/ended by the normal path — only the blocking runtime
    // call must finish before teardown proceeds.
    std::shared_future<void> pending_wait;
    {
        std::scoped_lock lock(turbo_mutex_);
        if (turbo_async_wait_.valid()) {
            pending_wait = turbo_async_wait_;
        }
    }
    return !pending_wait.valid() || pending_wait.wait_for(timeout) == std::future_status::ready;
}

void OpenXrLayer::ReleaseTurboPendingWaitForSessionEnd(XrSession session) {
    constexpr std::chrono::milliseconds kSettleTimeout{250};
    constexpr std::chrono::milliseconds kReleaseTimeout{2000};
    if (DrainTurboAsyncWait(kSettleTimeout)) return;
    // An interlocked runtime completes an off-thread wait only after a submit;
    // a conformant one rejects this begin (-37) and completes the wait itself.
    bool begun = false;
    XrTime display_time = 0;
    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    {
        std::scoped_lock lock(turbo_mutex_);
        begun = turbo_frame_begun_;
        display_time = turbo_last_predicted_display_time_;
        blend_mode = turbo_last_environment_blend_mode_;
    }
    if (!begun) {
        const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        begun = XR_SUCCEEDED(TraceRuntimeBeginFrame(session, &begin_info));
    }
    if (begun) {
        XrFrameEndInfo empty_end{XR_TYPE_FRAME_END_INFO};
        empty_end.displayTime = display_time;
        empty_end.environmentBlendMode = blend_mode;
        if (XR_SUCCEEDED(TraceRuntimeEndFrame(session, &empty_end))) {
            std::scoped_lock lock(turbo_mutex_);
            turbo_frame_begun_ = false;
        }
    }
    if (!DrainTurboAsyncWait(kReleaseTimeout)) {
        logger_.Error("Session end: Turbo's pipelined runtime xrWaitFrame is still pending; "
                      "ending the session without waiting for it.");
    }
}

void OpenXrLayer::ResetTurboFrameState(bool stop_recorder) {
    // Final metrics flush first (it takes turbo_mutex_ itself and joins the
    // async writer); a second call at teardown is a no-op.
    ResetTurboMetricsState();
    // A released async pipeline switched this session to frame-thread pacing;
    // the next session starts from the configured strategy again.
    if (turbo_experiment_.enabled || turbo_async_handed_over_) turbo_pacing_resolved_ = false;
    turbo_async_handed_over_ = false;
    turbo_async_recouple_stalled_ = false;
    turbo_experiment_ = {};
    turbo_limit_last_wait_.reset();
    turbo_trace_last_engaged_ = false;
    DrainRuntimePacingWrites();
    // The async wait worker publishes its result while taking turbo_mutex_.
    // Wait for it before taking that mutex ourselves; destroying the last
    // async shared state under the lock would otherwise deadlock teardown.
    // Bounded: a wait that never returns is abandoned (its generation goes
    // stale below) rather than hanging the application's session teardown.
    constexpr std::chrono::milliseconds kResetDrainTimeout{2000};
    if (!DrainTurboAsyncWait(kResetDrainTimeout)) {
        logger_.Error("Turbo: a pipelined runtime xrWaitFrame did not complete during session teardown; "
                      "continuing without it.");
    }
    // Stop the recorder only after the drain so a teardown stall is captured.
    if (stop_recorder) turbo_trace_.Stop();
    turbo_async_waits_before_begin_ = false;
    turbo_dropped_frame_log_budget_ = 5;
    end_frame_error_log_budget_ = 5;
    submission_transition_log_budget_ = 8;
    app_submitting_layers_.reset();
    composition_topology_log_budget_ = 12;
    last_composition_topology_signature_.reset();
    last_claimed_composition_topology_signature_.reset();
    pacing_last_end_time_.reset();
    pacing_window_start_.reset();
    pacing_frames_ = 0;
    pacing_delta_sum_ms_ = 0.0;
    pacing_delta_max_ms_ = 0.0;
    pacing_drain_sum_ms_ = 0.0;
    pacing_drain_max_ms_ = 0.0;
    pacing_end_sum_ms_ = 0.0;
    pacing_end_max_ms_ = 0.0;
    std::scoped_lock lock(turbo_mutex_);
    should_render_log_budget_ = 8;
    last_noted_should_render_.reset();
    pacing_wait_sum_ms_ = 0.0;
    pacing_wait_max_ms_ = 0.0;
    pacing_wait_samples_ = 0;
    pacing_fabricated_waits_ = 0;
    pacing_submit_delta_sum_periods_ = 0.0;
    pacing_submit_delta_min_periods_ = 0.0;
    pacing_submit_delta_max_periods_ = 0.0;
    pacing_submit_delta_samples_ = 0;
    turbo_async_handoff_active_ = false;
    turbo_async_handoff_armed_total_ = 0;
    turbo_async_handoff_wait_intercepts_ = 0;
    turbo_async_handoff_begin_intercepts_ = 0;
    turbo_async_handoff_second_poll_blocks_ = 0;
    turbo_async_handoff_cancellations_ = 0;
    turbo_async_handoff_debug_log_budget_ = 0;
    turbo_async_handoff_cv_.notify_all();
    turbo_async_wait_ = {};
    ++turbo_async_wait_generation_;
    turbo_async_wait_polled_ = false;
    turbo_async_wait_completed_ = false;
    turbo_async_wait_result_ = XR_SUCCESS;
    turbo_last_predicted_display_time_ = 0;
    turbo_last_predicted_display_period_ = 0;
    turbo_last_should_render_ = true;
    turbo_last_environment_blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    turbo_max_returned_display_time_ = 0;
    turbo_seq_wait_in_flight_ = false;
    runtime_latest_wait_prediction_.store(0);
    runtime_begun_prediction_.store(0);
    runtime_end_floor_.store(0);
    runtime_end_raises_.store(0);
    turbo_prediction_history_.Reset();
    turbo_prediction_fault_log_budget_ = 3;
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
    turbo_begin_probe_required_ = false;
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
    turbo_frame_interception_required_.store(false, std::memory_order_release);
}

XrResult OpenXrLayer::EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
    const auto trace_call = turbo_trace_.Watch("app.end");
    if (!frame_end_info || frame_end_info->type != XR_TYPE_FRAME_END_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // The game's CPU time for the performance row ends at the entry: what the layer, Frame Generation and the
    // runtime do from here is not the game's.
    if (performance_.Wanted()) {
        performance_.Frames().End(std::chrono::steady_clock::now(), frame_end_info->layerCount > 0,
                                  static_cast<std::uint32_t>(GetCurrentThreadId()));
    }
    NoteThread(ThreadCensus::Call::EndFrame);

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
    // Each frame's own quad route only (set at the compose site below).
    DropPendingFrameGenQuad(true);
    // The space-warp probe's structures never reach the runtime (nor Frame
    // Generation, nor the bridges' copies of the frame).
    std::optional<SpaceWarpFrame> spacewarp_frame;
    std::optional<FrameGenEngineMotionFrame> bridged_record;
    if (SpaceWarpProbeActive()) {
        frame_end_info = ProbeSpaceWarpFrameLocked(frame_end_info, spacewarp_frame.emplace());
        // Frame Generation's record of what the structures carried, with the
        // game's own poses (Pivot changes a copy later): on every frame, one
        // without structures too, so its frame before is always the last. A
        // Vulkan-bridged frame's goes once its releases are handed over
        // below, as a game's own images' releases come before its xrEndFrame.
        FrameGenEngineMotionFrame record = EngineMotionFrameLocked(*frame_end_info, *spacewarp_frame);
        if (vulkan_bridge_.Active()) {
            bridged_record = std::move(record);
        } else {
            framegen_.NoteEngineMotion(std::move(record), d3d11_bridge_.Active() ? FrameGenEngineBridge::kD3D11
                                                                                  : FrameGenEngineBridge::kNone);
        }
    }
    // The frame's bridged images reach the runtime's images here, where a
    // runtime reads them: everything the game submitted before this call is
    // in them, its rendering after a release included. Frame Generation
    // captures them here too, before anything below reads or composes them.
    if (const XrResult handed = HandOverBridgedReleasesLocked("end frame"); XR_FAILED(handed)) {
        DropPendingFrameGenQuad(false);
        framegen_.AbandonFrame(frame_end_info->displayTime);
        return handed;
    }
    // ...and the game's motion vector images, which it may never release:
    // as they are now (through the Vulkan bridge, as a hand-over copies them
    // into the runtime's images, with VECTORXR_FRAMEGEN_ENGINE_BRIDGE=end).
    if (SpaceWarpProbeActive()) {
        if (bridged_record) {
            std::vector<XrSwapchain> vectors;
            for (const FrameGenEngineMotionView& view : bridged_record->views) {
                vectors.push_back(view.vectors.swapchain);
            }
            // Read from now on: their images go back to the game's queue
            // behind each copy, so its next frame's vectors never overwrite
            // the ones being copied. Not read (the game made a swapchain
            // within the settle time, the validator disabled them, the
            // session reads none): nothing is given back early, and nothing
            // new is asked of the game's queue.
            if (framegen_.NoteEngineMotion(std::move(*bridged_record), FrameGenEngineBridge::kVulkan)) {
                for (const XrSwapchain swapchain : vectors) {
                    vulkan_bridge_motion_ = vulkan_bridge_.MarkMotion(swapchain) || vulkan_bridge_motion_;
                }
            } else {
                for (const XrSwapchain swapchain : vectors) {
                    vulkan_bridge_.UnmarkMotion(swapchain);
                }
            }
        }
        FrameGenMotionHandOver hand_over;
        if (vulkan_bridge_.Active()) {
            hand_over = [this](const std::vector<std::pair<XrSwapchain, std::uint32_t>>& images) {
                return vulkan_bridge_.EndFrameMotion(images);
            };
        }
        framegen_.CaptureEngineMotion(hand_over);
    }
    // A bridged session lives on the bridge's device; depth that stays the
    // game's is left out of its frames.
    std::optional<DepthlessFrame> depthless_frame;
    if (d3d11_bridge_.Active()) {
        std::string lost;
        d3d11_bridge_.FrameEnded();
        const auto now = std::chrono::steady_clock::now();
        if (now - d3d11_bridge_last_summary_ >= std::chrono::seconds(30)) {
            d3d11_bridge_last_summary_ = now;
            std::string quad_frames;
            if (has_active_primary_view_configuration_ &&
                IsQuadViewConfiguration(active_primary_view_configuration_type_)) {
                // This window's quad views frames: composed by Frame Generation,
                // by Direct3D 11, or split into layers.
                quad_frames = ", quadFrames=fg:" +
                              std::to_string(d3d11_bridge_quad_frames_.fg - d3d11_bridge_quad_frames_summarized_.fg) +
                              ",d3d11:" +
                              std::to_string(d3d11_bridge_quad_frames_.d3d11 -
                                             d3d11_bridge_quad_frames_summarized_.d3d11) +
                              ",split:" +
                              std::to_string(d3d11_bridge_quad_frames_.split -
                                             d3d11_bridge_quad_frames_summarized_.split);
            }
            d3d11_bridge_quad_frames_summarized_ = d3d11_bridge_quad_frames_;
            logger_.Info("Frame Generation's Direct3D 11 bridge: " + d3d11_bridge_.Summary() + quad_frames);
        }
        if (d3d11_bridge_.Lost(lost)) {
            if (!d3d11_bridge_lost_logged_) {
                d3d11_bridge_lost_logged_ = true;
                logger_.Error("Frame Generation's Direct3D 11 bridge: " + lost + "; the session is lost.");
            }
            DropPendingFrameGenQuad(false);
            framegen_.AbandonFrame(frame_end_info->displayTime);
            return XR_ERROR_SESSION_LOST;
        }
        if (d3d11_bridge_.HasUnmirrored()) {
            const XrFrameEndInfo* without_depth = WithoutDepth(
                frame_end_info, depthless_frame.emplace(),
                [this](XrSwapchain swapchain) { return d3d11_bridge_.Unmirrored(swapchain); });
            if (without_depth != frame_end_info && !d3d11_bridge_depth_logged_) {
                d3d11_bridge_depth_logged_ = true;
                logger_.Info("Frame Generation's Direct3D 11 bridge keeps the game's depth to itself: it is left "
                             "out of the frames the runtime and Frame Generation get.");
            }
            frame_end_info = without_depth;
        }
    }
    if (vulkan_bridge_.Active()) {
        std::string lost;
        vulkan_bridge_.FrameEnded();
        const auto now = std::chrono::steady_clock::now();
        if (now - vulkan_bridge_last_summary_ >= std::chrono::seconds(30)) {
            vulkan_bridge_last_summary_ = now;
            logger_.Info("Frame Generation's Vulkan bridge: " + vulkan_bridge_.Summary());
        }
        if (vulkan_bridge_.Lost(lost)) {
            if (!vulkan_bridge_lost_logged_) {
                vulkan_bridge_lost_logged_ = true;
                logger_.Error("Frame Generation's Vulkan bridge: " + lost + "; the session is lost.");
            }
            DropPendingFrameGenQuad(false);
            framegen_.AbandonFrame(frame_end_info->displayTime);
            return XR_ERROR_SESSION_LOST;
        }
        if (vulkan_bridge_.HasUnmirrored()) {
            const XrFrameEndInfo* without_depth = WithoutDepth(
                frame_end_info, depthless_frame.emplace(),
                [this](XrSwapchain swapchain) { return vulkan_bridge_.Unmirrored(swapchain); });
            if (without_depth != frame_end_info && !vulkan_bridge_depth_logged_) {
                vulkan_bridge_depth_logged_ = true;
                logger_.Info("Frame Generation's Vulkan bridge keeps the game's depth to itself (multisampled, or the "
                             "runtime's depth images are not depth targets: its swapchain line says which): it is "
                             "left out of the frames the runtime and Frame Generation get.");
            }
            frame_end_info = without_depth;
        }
    }
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    PollFrameGenToggle();
    PrepareOsd();
    // Capture application-submitted rectangles before synthesized Quadviews
    // rewrites them into two runtime views. Report allocated sizes separately.
    // Native Varjo focus views are checked here too, once per frame: most of
    // those frames take the pass-through path below and never reach the
    // projection rewrite.
    if (frame_end_info && frame_end_info->layers) {
        for (std::uint32_t i = 0; i < frame_end_info->layerCount; ++i) {
            const auto* base = frame_end_info->layers[i];
            if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (projection->viewCount != 4 || !projection->views) continue;
            quadviews_dimensions_.resize(4);
            for (std::uint32_t view = 0; view < 4; ++view) {
                const auto& image = projection->views[view].subImage;
                auto& size = quadviews_dimensions_[view];
                size = {};
                size.width = static_cast<std::uint32_t>(std::max(0, image.imageRect.extent.width));
                size.height = static_cast<std::uint32_t>(std::max(0, image.imageRect.extent.height));
                if (const auto found = tracked_swapchains_.find(image.swapchain); found != tracked_swapchains_.end()) {
                    size.allocated_width = found->second.width;
                    size.allocated_height = found->second.height;
                }
            }
            quadviews_dimensions_at_ = RuntimeRelayUnixMilliseconds();
            if (varjo_compatible_quadviews_active_) {
                NoteVarjoNativeSubmittedFocusLocked(frame_end_info->displayTime, projection->views,
                                                    projection->viewCount);
            }
            break;
        }
    }
    // The suite switch hides the diagnostic visualization of a Quadviews
    // session that keeps running (RefreshResolvedSettings).
    if (IsQuadViewsEmulationActive() && config_.core.enabled) {
        PollQuadViewsDiagnosticVisualizationToggle();
    } else {
        ResetQuadViewsDiagnosticVisualizationState();
    }

    if (!resolved_settings_.core.enabled) {
        cached_pivot_pose_deltas_.clear();
        cached_depth_submission_geometry_.clear();
        cached_quadviews_frames_.Clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_position_offset_ = {};
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
        pivotxr_yaw_step_glide_ = {};
        pivotxr_pitch_step_glide_ = {};
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        ResetPivotPoseDeltaContinuityState();
        const XrResult release_result = FlushDeferredSwapchainReleasesLocked("end frame");
        PrunePivotPoseDeltas(frame_end_info->displayTime);
        PruneDepthSubmissionGeometry(frame_end_info->displayTime);
        PruneQuadViewsFrames(frame_end_info->displayTime);
        if (XR_FAILED(release_result)) {
            DropPendingFrameGenQuad(false);
            framegen_.AbandonFrame(frame_end_info->displayTime);
            return release_result;
        }
        return ForwardEndFrame(session, frame_end_info, lock);
    }

    const bool quadviews_projection_split_active =
        IsQuadViewsEmulationActive() && has_active_primary_view_configuration_ &&
        IsQuadViewConfiguration(active_primary_view_configuration_type_) &&
        active_runtime_view_configuration_type_ == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (quadviews_compositor_recovery_.Pending() &&
        quadviews_projection_split_active &&
        quadviews_compositor_recovery_.Ready(std::chrono::steady_clock::now())) {
        RecycleD3D11QuadViewsCompositionTargets();
        quadviews_compositor_recovery_.Reset();
        pending_quadviews_compositor_diagnostics_ =
            std::max<uint32_t>(pending_quadviews_compositor_diagnostics_, 16);
        if (logger_.IsDebugEnabled()) {
            pending_quadviews_pixel_diagnostics_ =
                std::max<uint32_t>(pending_quadviews_pixel_diagnostics_, 12);
        }
        logger_.Info("Quadviews compositor output targets recycled after eye-gaze tracking recovered.");
    }
    // Varjo compatible quadviews: the focus sharpen runs inside the projection
    // rewrite loop below, so a requested sharpen must keep this frame out of the
    // identity-delta fast path — otherwise sharpening only ever ran on frames
    // where PivotXR happened to produce a correction. Threshold matches
    // SharpenNativeFocusViews (amount <= 0.001 is treated as off). The
    // runtime composes native focus views, so the suite switch stops the
    // sharpen at once, though the session keeps Quadviews.
    const bool needs_native_focus_sharpen =
        varjo_compatible_quadviews_active_ && IsQuadViewsActive() && config_.core.enabled && !d3d11_bridge_.Active() &&
        Clamp(resolved_settings_.quadviews.foveate_sharpness, 0.0, 100.0) / 100.0 > 0.001;
    if (d3d11_bridge_.Active() && !d3d11_bridge_sharpen_logged_ && varjo_compatible_quadviews_active_ &&
        IsQuadViewsActive() && Clamp(resolved_settings_.quadviews.foveate_sharpness, 0.0, 100.0) / 100.0 > 0.001) {
        d3d11_bridge_sharpen_logged_ = true;
        logger_.Info("Varjo focus sharpening is off while Frame Generation's Direct3D 11 bridge runs.");
    }
    const bool should_log_end_frame_diagnostic =
        pending_end_frame_diagnostics_ > 0 ||
        (quadviews_projection_split_active &&
         ShouldLogQuadViewsDebugHeartbeat(last_quadviews_end_frame_debug_heartbeat_));

    if (!resolved_settings_.pivotxr.enabled) {
        cached_pivot_pose_deltas_.clear();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_position_offset_ = {};
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
        pivotxr_yaw_step_glide_ = {};
        pivotxr_pitch_step_glide_ = {};
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        ResetPivotPoseDeltaContinuityState();
    }
    if (!resolved_settings_.depthxr.enabled || !depthxr_toggle_enabled_ ||
        !depth_anchor_active_) {
        cached_depth_submission_geometry_.clear();
    }

    PivotPoseDeltaFrame pose_delta_frame;
    XrTime matched_time = 0;

    const bool pivot_pose_continuity_active =
        resolved_settings_.pivotxr.enabled &&
        (pivotxr_engaged_ ||
         pivotxr_activation_gain_ > kPivotActivationGainEpsilon ||
         pivotxr_quick_view_active_ ||
         pivotxr_quick_view_transitioning_ ||
         pivotxr_quick_view_transition_.active);
    const std::size_t misses_before_lookup = pivotxr_consecutive_pose_delta_misses_;
    // A frame located before Pivot engaged has no delta (and needs none: the
    // activation ramp starts from zero). A frame that misses after one matched
    // breaks continuity; before any match, so does a run longer than the frames
    // located before the press took effect (located and submitted display
    // times never match).
    const bool pose_delta_continuity_established = pivotxr_last_matched_pose_delta_.has_value();
    const PivotPoseDeltaSelection pose_delta_selection = resolved_settings_.pivotxr.enabled
        ? ResolvePivotPoseDeltaValue(cached_pivot_pose_deltas_,
                                     frame_end_info->displayTime,
                                     PivotPoseDeltaFrame{},
                                     pivot_pose_continuity_active,
                                     &pivotxr_last_matched_pose_delta_,
                                     &pivotxr_consecutive_pose_delta_misses_,
                                     &pose_delta_frame,
                                     &matched_time)
        : PivotPoseDeltaSelection::None;
    const bool has_pose_delta = pose_delta_selection != PivotPoseDeltaSelection::None;
    const XrPosef& canonical_pose_delta = pose_delta_frame.canonical_view_pose_delta;

    if (pivot_pose_continuity_active) {
        const XrTime nearest_delta_ns = matched_time == 0
            ? 0
            : (matched_time > frame_end_info->displayTime
                   ? matched_time - frame_end_info->displayTime
                   : frame_end_info->displayTime - matched_time);
        // The first miss of a run is logged; the recovery line counts the rest.
        const bool missed = pose_delta_selection == PivotPoseDeltaSelection::HeldPrevious ||
                            pose_delta_selection == PivotPoseDeltaSelection::None;
        const bool miss_unexpected = pose_delta_continuity_established ||
                                     pivotxr_consecutive_pose_delta_misses_ > kPivotPoseDeltaEngagementMisses;
        if (missed && miss_unexpected && !pivotxr_pose_delta_miss_logged_) {
            pivotxr_pose_delta_miss_logged_ = true;
            pending_locate_views_diagnostics_ =
                std::max<uint32_t>(pending_locate_views_diagnostics_, 5);
            pending_end_frame_diagnostics_ =
                std::max<uint32_t>(pending_end_frame_diagnostics_, 5);
            pending_pivot_diagnostics_ =
                std::max<uint32_t>(pending_pivot_diagnostics_, kPivotDiagnosticBurstCount);
            std::ostringstream stream;
            stream << "PivotXR pose-delta continuity miss: frameTime="
                   << frame_end_info->displayTime
                   << ", nearestCachedTime=" << matched_time
                   << ", nearestDeltaMs="
                   << FormatDiagnosticDouble(static_cast<double>(nearest_delta_ns) / 1'000'000.0)
                   << ", matchWindowMs="
                   << FormatDiagnosticDouble(
                          static_cast<double>(kPivotPoseDeltaMatchWindow) / 1'000'000.0)
                   << ", cachedDeltas=" << cached_pivot_pose_deltas_.size()
                   << ", consecutiveMisses=" << pivotxr_consecutive_pose_delta_misses_
                   << ", fallback="
                   << (pose_delta_selection == PivotPoseDeltaSelection::HeldPrevious
                           ? "held_previous"
                           : "identity")
                   << ", pivotEngaged=" << (pivotxr_engaged_ ? "true" : "false")
                   << ", activationGain=" << FormatDiagnosticDouble(pivotxr_activation_gain_)
                   << ", selectedYaw="
                   << FormatDiagnosticDouble(ExtractPoseYawRadians(canonical_pose_delta))
                   << ", selectedPitch="
                   << FormatDiagnosticDouble(ExtractPosePitchRadians(canonical_pose_delta))
                   << ", selectedRoll="
                   << FormatDiagnosticDouble(ExtractPoseRollRadians(canonical_pose_delta))
                   << ", sourceSpace=" << DescribeSpace(pose_delta_frame.source_space)
                   << ", cachedSpaceExpressions="
                   << pose_delta_frame.space_pose_delta_count;
            logger_.Info(stream.str());
        } else if (!missed && misses_before_lookup > 0 && pivotxr_pose_delta_miss_logged_) {
            pivotxr_pose_delta_miss_logged_ = false;
            std::ostringstream stream;
            stream << "PivotXR pose-delta continuity recovered: frameTime="
                   << frame_end_info->displayTime
                   << ", matchedTime=" << matched_time
                   << ", matchedDeltaMs="
                   << FormatDiagnosticDouble(static_cast<double>(nearest_delta_ns) / 1'000'000.0)
                   << ", priorConsecutiveMisses=" << misses_before_lookup
                   << ", cachedDeltas=" << cached_pivot_pose_deltas_.size();
            logger_.Info(stream.str());
        }
    } else {
        pivotxr_pose_delta_miss_logged_ = false;
    }
    const bool has_non_identity_delta =
        has_pose_delta && !IsIdentityPose(canonical_pose_delta);

    auto find_depth_submission_geometry_for_layer =
        [&](const XrCompositionLayerProjection* projection_layer,
            XrTime* matched_time) -> const DepthSubmissionGeometry* {
            if (!projection_layer ||
                !resolved_settings_.depthxr.enabled ||
                !depthxr_toggle_enabled_ ||
                !depth_anchor_active_) {
                return nullptr;
            }

            const DepthSubmissionGeometry* geometry = nullptr;
            XrTime candidate_time = 0;
            if (!FindDepthSubmissionGeometry(frame_end_info->displayTime,
                                             projection_layer->space,
                                             active_primary_view_configuration_type_,
                                             projection_layer->viewCount,
                                             &geometry,
                                             &candidate_time)) {
                return nullptr;
            }
            if (matched_time) {
                *matched_time = candidate_time;
            }
            return geometry;
        };

    const DepthSubmissionGeometry* depth_submission_geometry = nullptr;
    XrTime matched_depth_submission_time = 0;
    if (frame_end_info->layers) {
        for (uint32_t i = 0; i < frame_end_info->layerCount; ++i) {
            const XrCompositionLayerBaseHeader* base_header = frame_end_info->layers[i];
            if (!base_header || base_header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                continue;
            }
            const auto* projection_layer =
                reinterpret_cast<const XrCompositionLayerProjection*>(base_header);
            depth_submission_geometry =
                find_depth_submission_geometry_for_layer(projection_layer,
                                                         &matched_depth_submission_time);
            if (depth_submission_geometry) {
                break;
            }
        }
    }
    const bool has_depth_submission_geometry = depth_submission_geometry != nullptr;

    const bool depth_adjustment_active_for_submission =
        resolved_settings_.depthxr.enabled && depthxr_toggle_enabled_ &&
        (!NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0) ||
         !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0));
    auto log_depth_runtime_submission =
        [&](const XrFrameEndInfo& submitted_frame, std::string_view submission_path) {
            // An application can locate a newer frame before ending the previous
            // one. Do not let that older in-flight submission consume the
            // breadcrumb armed by the newly adjusted locate call.
            if (!depth_submission_info_pending_ ||
                !depth_adjustment_active_for_submission ||
                !depth_submission_info_not_before_time_.has_value() ||
                submitted_frame.displayTime < *depth_submission_info_not_before_time_ ||
                !submitted_frame.layers) {
                return;
            }

            for (uint32_t i = 0; i < submitted_frame.layerCount; ++i) {
                const XrCompositionLayerBaseHeader* base_header = submitted_frame.layers[i];
                if (!base_header || base_header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    continue;
                }
                const auto* projection_layer =
                    reinterpret_cast<const XrCompositionLayerProjection*>(base_header);
                if (!projection_layer->views || projection_layer->viewCount == 0) {
                    continue;
                }

                double eye_separation_mm = 0.0;
                if (projection_layer->viewCount >= 2) {
                    eye_separation_mm =
                        PositionSeparationMeters(projection_layer->views[0].pose.position,
                                                 projection_layer->views[1].pose.position) *
                        1000.0;
                }
                std::ostringstream stream;
                stream << "Depth runtime submission: frameTime=" << submitted_frame.displayTime
                       << ", submissionPath=" << submission_path
                       << ", layer=" << i
                       << ", layerCount=" << submitted_frame.layerCount
                       << ", viewCount=" << projection_layer->viewCount
                       << ", eyeSeparationMm=" << FormatDiagnosticDouble(eye_separation_mm)
                       << ", view0ProjCenter="
                       << FormatDiagnosticDouble(HorizontalProjectionCenter(projection_layer->views[0].fov));
                if (projection_layer->viewCount >= 2) {
                    stream << ", view1ProjCenter="
                           << FormatDiagnosticDouble(HorizontalProjectionCenter(projection_layer->views[1].fov));
                }
                logger_.Info(stream.str());
                depth_submission_info_pending_ = false;
                depth_submission_info_not_before_time_.reset();
                break;
            }
        };

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

    if (!has_non_identity_delta && !has_depth_submission_geometry &&
        !quadviews_projection_split_active && !needs_native_focus_sharpen) {
        if (should_log_end_frame_diagnostic) {
            std::ostringstream stream;
            stream << "EndFrame projection rewrite skipped: pivotCacheHit=" << has_pose_delta
                   << ", depthAnchorCacheHit=" << has_depth_submission_geometry
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
        PruneDepthSubmissionGeometry(frame_end_info->displayTime);
        PruneQuadViewsFrames(frame_end_info->displayTime);
        if (XR_FAILED(release_result)) {
            DropPendingFrameGenQuad(false);
            framegen_.AbandonFrame(frame_end_info->displayTime);
            return release_result;
        }
        log_depth_runtime_submission(*frame_end_info, "passthrough");
        return ForwardEndFrame(session, frame_end_info, lock);
    }

    std::vector<ResolvedPivotSpaceDelta>& resolved_pivot_space_deltas =
        end_frame_pivot_space_deltas_scratch_;
    resolved_pivot_space_deltas.clear();
    resolved_pivot_space_deltas.reserve(frame_end_info->layerCount);
    uint32_t pivot_space_relation_query_count = 0;
    uint32_t pivot_space_conversion_count = 0;
    uint32_t pivot_space_conversion_failure_count = 0;
    const XrSpace canonical_view_space = internal_view_space_;
    const XrTime pivot_conversion_time = ClampInternalLocateTime(frame_end_info->displayTime);

    auto resolve_pivot_delta_for_space = [&](XrSpace target_space) -> const ResolvedPivotSpaceDelta& {
        const auto existing = std::find_if(
            resolved_pivot_space_deltas.begin(), resolved_pivot_space_deltas.end(),
            [target_space](const ResolvedPivotSpaceDelta& candidate) {
                return candidate.space == target_space;
            });
        if (existing != resolved_pivot_space_deltas.end()) {
            return *existing;
        }

        ResolvedPivotSpaceDelta resolved;
        resolved.space = target_space;
        if (!has_pose_delta || !has_non_identity_delta) {
            resolved.available = has_pose_delta;
            resolved.mode = has_pose_delta ? "canonical_identity" : "none";
        } else if (FindPivotPoseDeltaForSpace(pose_delta_frame, target_space, &resolved.forward)) {
            resolved.available = true;
            resolved.mode = target_space == pose_delta_frame.source_space
                ? "source_exact"
                : "located_views_cache";
        } else if (target_space == canonical_view_space &&
                   canonical_view_space != XR_NULL_HANDLE) {
            resolved.forward = canonical_pose_delta;
            resolved.available = true;
            resolved.mode = "canonical_view_exact";
        } else if (target_space != XR_NULL_HANDLE &&
                   canonical_view_space != XR_NULL_HANDLE && next_locate_space_) {
            XrSpaceLocation canonical_in_target{XR_TYPE_SPACE_LOCATION};
            const auto locate_started = std::chrono::steady_clock::now();
            // Runtimes may block xrLocateSpace while prediction advances. Do
            // not hold the layer configuration lock across that downstream
            // call (notably important for DCS and sequenced Turbo pacing).
            lock.unlock();
            resolved.locate_result = next_locate_space_(
                canonical_view_space, target_space, pivot_conversion_time, &canonical_in_target);
            lock.lock();
            resolved.locate_duration_us = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - locate_started).count();
            resolved.location_flags = canonical_in_target.locationFlags;
            ++pivot_space_relation_query_count;
            constexpr XrSpaceLocationFlags kPoseValid =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (XR_SUCCEEDED(resolved.locate_result) &&
                (canonical_in_target.locationFlags & kPoseValid) == kPoseValid) {
                resolved.forward = ReexpressPoseDelta(
                    canonical_pose_delta, canonical_in_target.pose);
                resolved.available = true;
                resolved.mode = "canonical_view_locate";
                CachePivotPoseDeltaForSpace(
                    pose_delta_frame, target_space, resolved.forward);
                ++pivot_space_conversion_count;
            } else {
                resolved.mode = "conversion_failed";
                ++pivot_space_conversion_failure_count;
            }
        } else {
            resolved.mode = "conversion_unavailable";
            ++pivot_space_conversion_failure_count;
        }

        resolved.non_identity = resolved.available && !IsIdentityPose(resolved.forward);
        if (resolved.non_identity) {
            resolved.reverse = InvertPose(resolved.forward);
        }

        const bool cross_space = target_space != pose_delta_frame.source_space;
        if (has_non_identity_delta && cross_space && resolved.available) {
            const bool recovered = failed_pivot_space_conversions_.erase(target_space) > 0;
            const bool first_observation =
                logged_pivot_space_conversions_.insert(target_space).second;
            if (recovered || first_observation) {
                std::ostringstream stream;
                stream << "PivotXR space-aware projection correction "
                       << (recovered ? "recovered" : "active")
                       << ": frameTime=" << frame_end_info->displayTime
                       << ", matchedTime=" << matched_time
                       << ", sourceSpace=" << DescribeSpace(pose_delta_frame.source_space)
                       << ", projectionSpace=" << DescribeSpace(target_space)
                       << ", canonicalSpace=" << DescribeSpace(canonical_view_space)
                       << ", resolution=" << resolved.mode
                       << ", deltaYaw=" << FormatDiagnosticDouble(ExtractPoseYawRadians(resolved.forward))
                       << ", deltaPitch=" << FormatDiagnosticDouble(ExtractPosePitchRadians(resolved.forward))
                       << ", deltaRoll=" << FormatDiagnosticDouble(ExtractPoseRollRadians(resolved.forward))
                       << ", deltaPosition=(" << FormatDiagnosticDouble(resolved.forward.position.x) << ", "
                       << FormatDiagnosticDouble(resolved.forward.position.y) << ", "
                       << FormatDiagnosticDouble(resolved.forward.position.z) << ").";
                logger_.Info(stream.str());
            }
        } else if (has_non_identity_delta && !resolved.available) {
            const bool first_failure =
                failed_pivot_space_conversions_.insert(target_space).second;
            pending_locate_views_diagnostics_ =
                std::max<uint32_t>(pending_locate_views_diagnostics_, 5);
            pending_end_frame_diagnostics_ =
                std::max<uint32_t>(pending_end_frame_diagnostics_, 5);
            if (first_failure) {
                std::ostringstream stream;
                stream << "PivotXR projection-space conversion failed; leaving this projection layer's pose unchanged: "
                       << "frameTime=" << frame_end_info->displayTime
                       << ", matchedTime=" << matched_time
                       << ", sourceSpace=" << DescribeSpace(pose_delta_frame.source_space)
                       << ", projectionSpace=" << DescribeSpace(target_space)
                       << ", canonicalSpace=" << DescribeSpace(canonical_view_space)
                       << ", resolution=" << resolved.mode
                       << ", locateResult=" << static_cast<int>(resolved.locate_result)
                       << ", locationFlags="
                       << FormatHex(static_cast<uint64_t>(resolved.location_flags)) << ".";
                logger_.Info(stream.str());
            }
        }

        if (should_log_end_frame_diagnostic && has_pose_delta) {
            std::ostringstream stream;
            stream << "PivotXR projection-space diagnostic: frameTime="
                   << frame_end_info->displayTime
                   << ", matchedTime=" << matched_time
                   << ", sourceSpace=" << DescribeSpace(pose_delta_frame.source_space)
                   << ", projectionSpace=" << DescribeSpace(target_space)
                   << ", canonicalSpace=" << DescribeSpace(canonical_view_space)
                   << ", resolution=" << resolved.mode
                   << ", available=" << resolved.available
                   << ", nonIdentity=" << resolved.non_identity
                   << ", locateResult=" << static_cast<int>(resolved.locate_result)
                   << ", locationFlags=" << FormatHex(static_cast<uint64_t>(resolved.location_flags))
                   << ", locateDurationUs=" << FormatDiagnosticDouble(resolved.locate_duration_us)
                   << ", forwardQuat=(" << FormatDiagnosticDouble(resolved.forward.orientation.x) << ", "
                   << FormatDiagnosticDouble(resolved.forward.orientation.y) << ", "
                   << FormatDiagnosticDouble(resolved.forward.orientation.z) << ", "
                   << FormatDiagnosticDouble(resolved.forward.orientation.w) << ")"
                   << ", forwardYaw=" << FormatDiagnosticDouble(ExtractPoseYawRadians(resolved.forward))
                   << ", forwardPitch=" << FormatDiagnosticDouble(ExtractPosePitchRadians(resolved.forward))
                   << ", forwardRoll=" << FormatDiagnosticDouble(ExtractPoseRollRadians(resolved.forward))
                   << ", forwardPosition=(" << FormatDiagnosticDouble(resolved.forward.position.x) << ", "
                   << FormatDiagnosticDouble(resolved.forward.position.y) << ", "
                   << FormatDiagnosticDouble(resolved.forward.position.z) << ")";
            logger_.Debug(stream.str());
        }

        resolved_pivot_space_deltas.push_back(std::move(resolved));
        return resolved_pivot_space_deltas.back();
    };

    std::vector<std::vector<XrCompositionLayerProjectionView>>& adjusted_projection_views =
        end_frame_projection_views_scratch_;
    std::vector<XrCompositionLayerProjection>& adjusted_projection_layers = end_frame_projection_layers_scratch_;
    std::vector<const XrCompositionLayerBaseHeader*>& adjusted_layers = end_frame_layers_scratch_;
    adjusted_projection_views.clear();
    adjusted_projection_layers.clear();
    adjusted_layers.clear();
    uint32_t corrected_projection_layer_count = 0;
    uint32_t corrected_projection_view_count = 0;
    uint32_t depth_anchor_restored_view_count = 0;
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
                                       const XrPosef& reverse_delta,
                                       bool has_layer_pose_delta,
                                       XrCompositionLayerFlags extra_layer_flags = 0) {
        adjusted_projection_views.emplace_back(projection_layer->views + first_view,
                                               projection_layer->views + first_view + view_count);
        for (XrCompositionLayerProjectionView& projection_view : adjusted_projection_views.back()) {
            if (has_layer_pose_delta) {
                projection_view.pose = MultiplyPoses(projection_view.pose, reverse_delta);
            }
        }
        if (const DepthSubmissionGeometry* layer_geometry =
                find_depth_submission_geometry_for_layer(projection_layer, nullptr)) {
            depth_anchor_restored_view_count += RestoreDepthSubmissionGeometry(
                std::span<XrCompositionLayerProjectionView>(adjusted_projection_views.back()),
                first_view,
                *layer_geometry,
                reverse_delta,
                has_layer_pose_delta);
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

    bool primary_projection_seen = false;
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
        const ResolvedPivotSpaceDelta& layer_pivot_delta =
            resolve_pivot_delta_for_space(projection_layer->space);

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

        // The frame's primary projection (its first with views) is the one
        // Frame Generation generates from.
        const bool primary_projection = !primary_projection_seen;
        primary_projection_seen = true;
        if (quadviews_projection_split_active && projection_layer->viewCount >= 4) {
            QuadViewsComposePlan plan;
            const bool planned = BuildQuadViewsComposePlan(
                projection_layer, frame_end_info->displayTime, layer_pivot_delta.reverse,
                layer_pivot_delta.non_identity, find_depth_submission_geometry_for_layer(projection_layer, nullptr),
                &plan);
            // Frame Generation may compose the frame itself (just in time, on
            // a bridged session whose pacing it owns), from the four views as
            // the layer would submit them, with the alpha bits the composed
            // layer clears, and draws the plan's diagnostic overlay over it
            // (never declined for the overlay: that restarted Frame
            // Generation on the composed full-size views).
            const bool from_site = primary_projection && d3d11_bridge_.Active() &&
                                   framegen_owns_pacing_.load(std::memory_order_acquire);
            const bool offer = planned && from_site;
            FrameGenQuadRoute route = FrameGenQuadRoute::kDeclined;
            XrCompositionLayerProjection source = *projection_layer;
            if (offer) {
                source.viewCount = static_cast<uint32_t>(plan.source_views.size());
                source.views = plan.source_views.data();
                source.layerFlags &= ~(XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT);
                route = framegen_.ClaimQuadFrame(source);
            }
            if (route == FrameGenQuadRoute::kClaimed) {
                // Frame Generation composes it (from the views' captures) for
                // every refresh: the four views go to it, never to the
                // runtime, and the Direct3D 11 compositor has nothing to do.
                adjusted_projection_views.emplace_back(plan.source_views.begin(), plan.source_views.end());
                adjusted_projection_layers.push_back(source);
                adjusted_projection_layers.back().views = adjusted_projection_views.back().data();
                adjusted_layers.push_back(
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&adjusted_projection_layers.back()));
                for (uint32_t view = 0; view < 4; ++view) {
                    d3d11_bridge_.SetComposed(projection_layer->views[view].subImage.swapchain, true);
                }
                ++d3d11_bridge_quad_frames_.fg;
                FrameGenQuadFrame& pending = pending_framegen_quad_.emplace();
                pending.route = route;
                pending.composition = plan.composition;
                pending.eyes = plan.eye_views;
                pending.source_views = plan.source_views;
                pending.source_space = projection_layer->space;
                depth_anchor_restored_view_count += plan.restored;
                ++corrected_projection_layer_count;
                corrected_projection_view_count += 4;
                ++split_quad_projection_layer_count;
                continue;
            }
            adjusted_projection_views.emplace_back();
            adjusted_projection_layers.emplace_back();
            // Left to the layer while Frame Generation's thread presents just
            // in time (composing the quad views, or not for now): the thread
            // shows the frame as it is, at the frame's refresh.
            plan.shown_later = route == FrameGenQuadRoute::kUncaptured || route == FrameGenQuadRoute::kBusy ||
                               (route == FrameGenQuadRoute::kNotPresenting && framegen_.ShowsOwnImagesOnly());
            const bool composed = planned && ComposeQuadViewsD3D11(
                projection_layer, frame_end_info->displayTime, plan, &depth_anchor_restored_view_count,
                &adjusted_projection_layers.back(), &adjusted_projection_views.back());
            // Frame Generation shows it as the layer composed it (or split
            // it), and sets the views up from it when their releases were not
            // captured. A frame that could not be planned is the layer's too.
            if (from_site) {
                FrameGenQuadFrame& pending = pending_framegen_quad_.emplace();
                pending.route = offer ? route : FrameGenQuadRoute::kNotPresenting;
                if (route == FrameGenQuadRoute::kUncaptured) {
                    pending.composition = plan.composition;
                    pending.source_views = plan.source_views;
                    pending.source_space = projection_layer->space;
                }
            }
            // The views of a composed layer reach the runtime only when
            // composing fails: their next releases need not wait on the
            // runtime's queue (FrameGenLazy).
            if (d3d11_bridge_.Active()) {
                for (uint32_t view = 0; view < 4; ++view) {
                    d3d11_bridge_.SetComposed(projection_layer->views[view].subImage.swapchain, composed);
                }
                ++(composed ? d3d11_bridge_quad_frames_.d3d11 : d3d11_bridge_quad_frames_.split);
            }
            if (composed) {
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
            append_projection_layer(projection_layer, 2, 2,
                                    layer_pivot_delta.reverse,
                                    layer_pivot_delta.non_identity,
                                    kFovealBlendFlags);
            append_projection_layer(projection_layer, 0, 2,
                                    layer_pivot_delta.reverse,
                                    layer_pivot_delta.non_identity);
            append_projection_layer(projection_layer, 2, 2,
                                    layer_pivot_delta.reverse,
                                    layer_pivot_delta.non_identity,
                                    kFovealBlendFlags);
            ++split_quad_projection_layer_count;
            continue;
        }

        append_projection_layer(projection_layer, 0, projection_layer->viewCount,
                                layer_pivot_delta.reverse,
                                layer_pivot_delta.non_identity);
        // Varjo compatible quadviews: sharpen the runtime's focus views in place when
        // foveate_sharpness > 0. No-op (pure passthrough) when sharpness is 0 or the
        // layer is not a native quad projection.
        // Not on a bridged session: the sharpen's own swapchains are not bridged.
        if (varjo_compatible_quadviews_active_ && projection_layer->viewCount >= 4 && IsQuadViewsActive() &&
            !d3d11_bridge_.Active()) {
            SharpenNativeFocusViews(adjusted_projection_views.back(), frame_end_info->displayTime);
        }
    }

    XrFrameEndInfo adjusted_frame_end_info = *frame_end_info;
    adjusted_frame_end_info.layerCount = static_cast<uint32_t>(adjusted_layers.size());
    adjusted_frame_end_info.layers = adjusted_layers.data();
    if (should_log_end_frame_diagnostic) {
        std::ostringstream stream;
        stream << "EndFrame projection rewrite applied: frameTime=" << frame_end_info->displayTime
               << ", matchedTime=" << matched_time
               << ", matchedDeltaNs=" << FormatMatchedDeltaNs(matched_time, frame_end_info->displayTime)
               << ", depthAnchorMatchedTime=" << matched_depth_submission_time
               << ", depthAnchorMatchedDeltaNs="
               << FormatMatchedDeltaNs(matched_depth_submission_time, frame_end_info->displayTime)
               << ", canonicalSpace=" << DescribeSpace(canonical_view_space)
               << ", sourceSpace=" << DescribeSpace(pose_delta_frame.source_space)
               << ", canonicalDeltaYaw="
               << FormatDiagnosticDouble(ExtractPoseYawRadians(canonical_pose_delta))
               << ", canonicalDeltaPitch="
               << FormatDiagnosticDouble(ExtractPosePitchRadians(canonical_pose_delta))
               << ", canonicalDeltaRoll="
               << FormatDiagnosticDouble(ExtractPoseRollRadians(canonical_pose_delta))
               << ", cachedSpaceExpressions=" << pose_delta_frame.space_pose_delta_count
               << ", resolvedProjectionSpaces=" << resolved_pivot_space_deltas.size()
               << ", spaceRelationQueries=" << pivot_space_relation_query_count
               << ", spaceConversions=" << pivot_space_conversion_count
               << ", spaceConversionFailures=" << pivot_space_conversion_failure_count
               << ", projectionLayers=" << corrected_projection_layer_count
               << ", projectionViews=" << corrected_projection_view_count
               << ", depthAnchorRestoredViews=" << depth_anchor_restored_view_count
               << ", splitQuadProjectionLayers=" << split_quad_projection_layer_count
               << ", d3d11QuadCompositions=" << d3d11_quad_composition_count
               << ", projectionSwapchainRefs=" << projection_swapchain_reference_count
               << ", unknownProjectionSwapchains=" << unknown_projection_swapchain_count
               << ", trackedSwapchains=" << tracked_swapchains_.size()
               << ", cachedPivotDeltas=" << cached_pivot_pose_deltas_.size()
               << ", cachedDepthAnchorFrames=" << cached_depth_submission_geometry_.size();
        logger_.Debug(stream.str());
        if (pending_end_frame_diagnostics_ > 0) {
            --pending_end_frame_diagnostics_;
        }
    }
    const XrResult release_result = FlushDeferredSwapchainReleasesLocked("end frame");
    PrunePivotPoseDeltas(frame_end_info->displayTime);
    PruneDepthSubmissionGeometry(frame_end_info->displayTime);
    PruneQuadViewsFrames(frame_end_info->displayTime);
    if (XR_FAILED(release_result)) {
        // A frame Frame Generation claimed is abandoned with it.
        DropPendingFrameGenQuad(false);
        framegen_.AbandonFrame(frame_end_info->displayTime);
        return release_result;
    }

    // Pruning happens above, while the lock is still held: ForwardEndFrame
    // releases mutex_ before forwarding to the runtime.
    if (depth_submission_info_pending_ && pending_framegen_quad_ &&
        pending_framegen_quad_->route == FrameGenQuadRoute::kClaimed) {
        // A quad views frame Frame Generation claimed reaches the runtime as
        // the stereo frames it composes, with these eye views (Depth's
        // geometry restored), not as the four source views it was given.
        XrCompositionLayerProjection composed{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        composed.viewCount = static_cast<uint32_t>(pending_framegen_quad_->eyes.size());
        composed.views = pending_framegen_quad_->eyes.data();
        std::vector<const XrCompositionLayerBaseHeader*> composed_layers(adjusted_layers);
        for (const XrCompositionLayerBaseHeader*& layer : composed_layers) {
            if (layer && layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
                reinterpret_cast<const XrCompositionLayerProjection*>(layer)->viewCount > 0) {
                layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&composed);
                break;
            }
        }
        XrFrameEndInfo composed_frame = adjusted_frame_end_info;
        composed_frame.layers = composed_layers.data();
        log_depth_runtime_submission(composed_frame, "framegen-composed");
    } else {
        log_depth_runtime_submission(adjusted_frame_end_info, "rewritten");
    }
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
            emulate_combined_eye = session == active_session_ && IsQuadViewsEmulationActive();
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
    if (session == active_session_ && IsQuadViewsEmulationActive() &&
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
            emulate_combined_eye = session == active_session_ && IsQuadViewsEmulationActive();
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

XrResult OpenXrLayer::CreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    const auto result = next_create_action_(set, info, action);
    if (XR_SUCCEEDED(result) && info && action) {
        std::scoped_lock lock(mutex_);
        application_pose_actions_.erase(*action);
        application_action_owners_[*action] = set;
        if (info->actionType == XR_ACTION_TYPE_POSE_INPUT) {
            application_pose_actions_[*action] = std::make_shared<ApplicationPoseAction>(ApplicationPoseAction{set});
        }
    }
    return result;
}

XrResult OpenXrLayer::DestroyAction(XrAction action) {
    const auto result = next_destroy_action_(action);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        application_pose_actions_.erase(action);
        application_action_owners_.erase(action);
        const auto removed = std::erase_if(application_eye_gaze_bindings_, [action](const auto& binding) {
            return binding.action == action;
        });
        if (removed && !eye_gaze_action_set_attachment_.SessionAttachmentCompleted()) {
            eye_gaze_bindings_ready_ = false;
        }
        // Existing spaces still locate the old action resource, per OpenXR.
    }
    return result;
}

XrResult OpenXrLayer::DestroyActionSet(XrActionSet set) {
    const auto result = next_destroy_action_set_(set);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        const auto removed = std::erase_if(application_eye_gaze_bindings_, [&](const auto& binding) {
            const auto found = application_action_owners_.find(binding.action);
            return found != application_action_owners_.end() && found->second == set;
        });
        if (removed && !eye_gaze_action_set_attachment_.SessionAttachmentCompleted()) {
            eye_gaze_bindings_ready_ = false;
        }
        std::erase_if(application_action_owners_, [set](const auto& entry) { return entry.second == set; });
        application_attached_action_sets_.erase(set);
        std::erase_if(application_pose_actions_, [set](const auto& entry) { return entry.second->owner == set; });
    }
    return result;
}

XrResult OpenXrLayer::SuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* bindings) {
    const auto result = next_suggest_interaction_profile_bindings_(instance, bindings);
    if (XR_SUCCEEDED(result) && bindings) {
        XrPath profile = XR_NULL_PATH, pose = XR_NULL_PATH;
        // Resolve the standard paths downstream, independently of our private
        // Quadviews gaze action and without holding the layer's state lock.
        if (XR_SUCCEEDED(next_string_to_path_(instance, "/interaction_profiles/ext/eye_gaze_interaction", &profile)) &&
            bindings->interactionProfile == profile &&
            XR_SUCCEEDED(next_string_to_path_(instance, "/user/eyes_ext/input/gaze_ext/pose", &pose))) {
            std::scoped_lock lock(mutex_);
            application_eye_gaze_bindings_.clear();
            application_eye_gaze_binding_chain_unsupported_ = bindings->next != nullptr;
            if (!application_eye_gaze_binding_chain_unsupported_ && bindings->suggestedBindings) {
                application_eye_gaze_bindings_.assign(bindings->suggestedBindings,
                    bindings->suggestedBindings + bindings->countSuggestedBindings);
            }
            logger_.Info("Application eye-gaze binding suggestions accepted: count=" +
                         std::to_string(bindings->countSuggestedBindings) +
                         ", extensionChain=" + std::to_string(bindings->next != nullptr) +
                         ", privateSetAttached=" +
                         std::to_string(eye_gaze_action_set_attachment_.PrivateActionSetAttached()));
            if (eye_gaze_action_set_attachment_.SessionAttachmentCompleted()) {
                // An accepted suggestion after attachment can only name
                // unattached action sets; it cannot change this session's
                // bindings and is merged again at the next session's attach.
                logger_.Info("Application eye-gaze bindings suggested after attachment apply to later sessions.");
            } else {
                eye_gaze_bindings_ready_ = false;
            }
            // A successful re-suggestion replaces this profile's whole list.
            for (const auto& [handle, action] : application_pose_actions_) action->eye_gaze = false;
            for (const auto& [handle, space] : application_action_spaces_) space.action->eye_gaze = false;
            for (uint32_t i = 0; bindings->suggestedBindings && i < bindings->countSuggestedBindings; ++i) {
                const auto& binding = bindings->suggestedBindings[i];
                const auto found = application_pose_actions_.find(binding.action);
                if (binding.binding == pose && found != application_pose_actions_.end()) found->second->eye_gaze = true;
            }
        }
    }
    return result;
}

XrResult OpenXrLayer::CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    const auto result = next_create_action_space_(session, info, space);
    if (XR_SUCCEEDED(result) && info && space) {
        XrPath eyes = XR_NULL_PATH;
        const bool includes_eyes = info->subactionPath == XR_NULL_PATH ||
            (XR_SUCCEEDED(next_string_to_path_(instance_, "/user/eyes_ext", &eyes)) && info->subactionPath == eyes);
        std::scoped_lock lock(mutex_);
        application_action_spaces_.erase(*space);
        const auto action = application_pose_actions_.find(info->action);
        if (action != application_pose_actions_.end())
            application_action_spaces_[*space] = {action->second, includes_eyes};
    }
    return result;
}

bool OpenXrLayer::IsHeadRelativeEyeGaze(XrSpace space, XrSpace base_space) const {
    const auto is_gaze = [&](XrSpace candidate) {
        const auto found = application_action_spaces_.find(candidate);
        return found != application_action_spaces_.end() && found->second.includes_eyes && found->second.action->eye_gaze;
    };
    return (IsTrackedViewSpace(base_space) && is_gaze(space)) ||
           (IsTrackedViewSpace(space) && is_gaze(base_space));
}

XrResult OpenXrLayer::PollEvent(XrInstance instance, XrEventDataBuffer* event_data) {
    if (!next_poll_event_) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = next_poll_event_(instance, event_data);
    if (result != XR_SUCCESS || !event_data) {
        return result;
    }
    if (event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto& change = *reinterpret_cast<const XrEventDataSessionStateChanged*>(event_data);
        // Rare, and what a teardown follows: the runtime asks the session to
        // stop, or it is lost. Logged for any session, before the layer's
        // lock, so a teardown that holds the lock cannot hide it.
        logger_.Info("Runtime session state changed: state=" + std::to_string(static_cast<int>(change.state)) +
                     " (" + SessionStateName(change.state) + "), time=" + std::to_string(change.time));
        std::scoped_lock lock(mutex_);
        if (change.session == active_session_) {
            observed_session_state_ = change.state;
            if (change.state == XR_SESSION_STATE_FOCUSED) {
                eye_gaze_sync_not_focused_ = false;
                // Input sampled before focus returned is not current focused input.
                last_app_action_sync_time_.reset();
                last_eye_gaze_self_sync_time_.reset();
                last_app_action_sync_attempt_time_.reset();
                last_eye_gaze_self_sync_attempt_time_.reset();
            }
        }
        return result;
    }
    if (event_data->type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
        const auto& change = *reinterpret_cast<const XrEventDataInteractionProfileChanged*>(event_data);
        std::scoped_lock lock(mutex_);
        if (change.session == active_session_) {
            logger_.Info("Runtime interaction profile changed; eye-gaze binding/source diagnostics will refresh.");
            last_eye_gaze_bound_sources_probe_.reset();
            pending_eye_gaze_diagnostics_ = std::max(pending_eye_gaze_diagnostics_, 20u);
        }
        return result;
    }
    if (event_data->type != XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
        return result;
    }
    const auto& change = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(event_data);
    if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    // Only Pivot keeps a seated position to re-anchor.
    if (change.session == active_session_ && resolved_settings_.core.enabled && resolved_settings_.pivotxr.enabled) {
        // A bounded queue keeps the newest changes: a later recenter must
        // not be dropped for older ones.
        if (pivotxr_reference_changes_.size() >= 16) {
            pivotxr_reference_changes_.erase(pivotxr_reference_changes_.begin());
        }
        pivotxr_reference_changes_.push_back({change.referenceSpaceType, change.changeTime});
        logger_.Info("Runtime reference space change pending: type=" +
                     std::to_string(static_cast<int>(change.referenceSpaceType)) +
                     ", changeTime=" + std::to_string(change.changeTime) +
                     "; Pivot re-anchors its seated position at the change.");
    }
    return result;
}

bool OpenXrLayer::PivotAnchorAffectedBy(XrReferenceSpaceType type) const {
    if (!pivotxr_translation_anchor_) {
        return false;
    }
    const XrSpace space = pivotxr_translation_anchor_->space;
    if (tracked_local_spaces_.contains(space)) {
        return type == XR_REFERENCE_SPACE_TYPE_LOCAL || type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    }
    if (tracked_stage_spaces_.contains(space)) {
        return type == XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    // Unknown app space type (e.g. LOCAL_FLOOR or a custom origin): any
    // tracking-origin change may move it.
    return true;
}

XrResult OpenXrLayer::LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location) {
    bool pivotxr_active = false;
    bool pivot_processing_required = false;
    XrSpace anchor_space = XR_NULL_HANDLE;
    XrSpace reference_space = XR_NULL_HANDLE;
    XrTime anchor_locate_time = time;
    HeadPredictionPlan head_plan;
    XrSpace head_world_space = XR_NULL_HANDLE;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        // VIEW located in a world space, or the reverse: head prediction
        // turns it as it turns the views for the same display time.
        head_world_space = HeadPredictionWorldSpace(space, base_space);
        if (head_world_space != XR_NULL_HANDLE) {
            head_plan = PlanHeadPredictionLocked(false);
        }
        // The flight recorder's view of where the app places its camera: the
        // time it locates VIEW space at, against its views' (`locate`). Once
        // per time: DCS locates VIEW space several times a frame.
        if (turbo_trace_.Enabled() && time != last_locspace_time_ &&
            (IsTrackedViewSpace(space) || IsTrackedViewSpace(base_space))) {
            last_locspace_time_ = time;
            turbo_trace_.Record("locspace", turbo_trace_.NextId(), time, static_cast<std::int64_t>(ClassifySpace(space)),
                                static_cast<std::int64_t>(ClassifySpace(base_space)));
        }
        // Head-relative eye direction must stay attached to the virtual head.
        // Applying the inverse camera rotation here moves MSFS's focus region
        // away from the user's eyes. Preserve the complete runtime result,
        // including sample time/validity, and avoid extra anchor-space queries.
        if (resolved_settings_.core.enabled && resolved_settings_.pivotxr.enabled &&
            !IsHeadRelativeEyeGaze(space, base_space)) {
            pivotxr_active = IsPivotXrActive();
            pivot_processing_required =
                pivotxr_active || pivotxr_activation_gain_ > kPivotActivationGainEpsilon;
            if (pivot_processing_required && pivotxr_translation_anchor_ &&
                IsTrackedViewSpace(space) != IsTrackedViewSpace(base_space)) {
                reference_space = IsTrackedViewSpace(space) ? base_space : space;
                anchor_space = pivotxr_translation_anchor_->space;
                anchor_locate_time = ClampInternalLocateTime(time);
            }
        }
    }

    const auto extend_head = [&](XrResult located) {
        if (XR_FAILED(located) || !location || !head_plan.limited ||
            (location->locationFlags & kHeadPredictionPoseFlags) != kHeadPredictionPoseFlags) {
            return;
        }
        const HeadExtension extension = ExtendHead(head_plan, head_world_space, time, TurboClockNow());
        const HeadPredictionPose pose = ToHeadPose(location->pose);
        location->pose = ToXrPose(space == head_world_space ? ApplyHeadExtensionInverse(extension, pose)
                                                            : ApplyHeadExtension(extension, pose));
    };

    if (!pivot_processing_required) {
        const XrResult result = next_locate_space_(space, base_space, time, location);
        extend_head(result);
        return result;
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
    extend_head(result);

    XrSpaceLocation anchor_relation{XR_TYPE_SPACE_LOCATION};
    if (anchor_space != XR_NULL_HANDLE && anchor_space != reference_space) {
        if (XR_FAILED(next_locate_space_(anchor_space, reference_space, anchor_locate_time, &anchor_relation))) {
            anchor_relation.locationFlags = 0;
        }
    }
    std::scoped_lock lock(mutex_);
    const bool same_anchor_space = pivotxr_translation_anchor_ &&
        pivotxr_translation_anchor_->space == anchor_space;
    return ApplyPivotToLocatedSpace(space, base_space, time, pivotxr_active, location, nullptr,
                                    nullptr, nullptr, false, same_anchor_space ? &anchor_relation : nullptr);
}

XrResult OpenXrLayer::LocateViews(XrSession session,
                                  const XrViewLocateInfo* view_locate_info,
                                  XrViewState* view_state,
                                  uint32_t view_capacity_input,
                                  uint32_t* view_count_output,
                                  XrView* views) {
    const auto trace_call = turbo_trace_.Watch("app.locateViews");
    bool synthesized_quad_views = false;
    QuadViewsGazeDiagnostic gaze_diagnostic{};
    HeadExtension head_extension;
    const XrResult result = LocateRuntimeViews(
        session,
        view_locate_info,
        view_state,
        view_capacity_input,
        view_count_output,
        views,
        &synthesized_quad_views,
        &gaze_diagnostic,
        &head_extension);

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
        cached_depth_submission_geometry_.clear();
        return result;
    }

    const uint32_t count = std::min(view_capacity_input, *view_count_output);
    if (count == 0) {
        return result;
    }
    if (!resolved_settings_.pivotxr.enabled && !resolved_settings_.depthxr.enabled &&
        !IsQuadViewsActive()) {
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
        stream << "PivotXR enabled; activationState=" << (pivotxr_active ? "engaged" : "idle")
               << "; canonicalFrame=VIEW(internal); spaceAwareProjectionCorrection=enabled; "
                  "quad-view sessions use stereo eye-pose recomposition.";
        for (const PivotXrResolvedProfile& profile : resolved_settings_.pivotxr.profiles) {
            if (profile.always_active) {
                stream << " '" << profile.name << "' engages automatically";
                if (!profile.activation_bindings.empty()) {
                    stream << "; press " << BindingListLabel(profile.activation_bindings) << " to suspend/resume";
                }
                stream << ".";
            } else {
                stream << " Press " << BindingListLabel(profile.activation_bindings) << " to "
                       << "engage '" << profile.name << "'.";
            }
        }
        logger_.Info(stream.str());
        has_logged_pivotxr_spike_mode_ = true;
    }

    const bool pivot_drive_query = ShouldDrivePivotFromLocateViews(
        view_locate_info != nullptr,
        view_locate_info != nullptr && IsTrackedViewSpace(view_locate_info->space));

    // Consume a pending origin capture with this frame's head pose. This runs
    // independently of engagement so the origin can be set before pivot is
    // engaged; the capture uses the same displayTime as the pivot drive below.
    // VIEW-relative calls are pass-through queries and wait for a world-space
    // call, otherwise they would capture an identity origin.
    const XrTime internal_locate_time =
        view_locate_info ? ClampInternalLocateTime(view_locate_info->displayTime) : 0;
    bool pivot_frame_reused = false;
    bool pivot_frame_cached = false;
    XrSpace pivot_frame_source_space = XR_NULL_HANDLE;

    if (pivotxr_origin_capture_pending_ && resolved_settings_.pivotxr.enabled &&
        internal_view_space_ != XR_NULL_HANDLE && pivot_drive_query) {
        XrSpaceLocation origin_location{XR_TYPE_SPACE_LOCATION};
        const XrResult origin_result = next_locate_space_(
            internal_view_space_, view_locate_info->space, internal_locate_time, &origin_location);
        // The head the views were turned to (head prediction), as below.
        origin_location.pose = ToXrPose(ApplyHeadExtension(head_extension, ToHeadPose(origin_location.pose)));
        if (XR_SUCCEEDED(origin_result) &&
            (origin_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
            PivotOrigin origin;
            origin.pose = origin_location.pose;
            origin.yaw_radians = ExtractPoseYawRadians(origin_location.pose);
            origin.pitch_radians = ExtractPosePitchRadians(origin_location.pose);
            origin.capture_time = view_locate_info->displayTime;
            origin.session = session;
            origin.location_flags = origin_location.locationFlags;
            pivotxr_origin_ = origin;
            pivotxr_translation_anchor_.reset();
            if ((origin_location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
                pivotxr_translation_anchor_ = PivotTranslationAnchor{
                    origin.pose.position, view_locate_info->space};
            }
            pivotxr_origin_capture_pending_ = false;
            std::ostringstream origin_stream;
            origin_stream << "PivotXR origin captured: yaw=" << FormatDiagnosticDouble(origin.yaw_radians)
                          << " pitch=" << FormatDiagnosticDouble(origin.pitch_radians)
                          << " rad, position=(" << FormatDiagnosticDouble(origin.pose.position.x) << ", "
                          << FormatDiagnosticDouble(origin.pose.position.y) << ", "
                          << FormatDiagnosticDouble(origin.pose.position.z) << ")"
                          << ", frameTime=" << origin.capture_time
                          << ", baseSpace=" << DescribeSpace(view_locate_info->space)
                          << ", locationFlags="
                          << FormatHex(static_cast<uint64_t>(origin.location_flags)) << ".";
            logger_.Info(origin_stream.str());
        }
    }

    if (resolved_settings_.pivotxr.enabled && pivotxr_envelope_engaged && internal_view_space_ != XR_NULL_HANDLE &&
        pivot_drive_query) {
        XrSpaceLocation pivot_view_location{XR_TYPE_SPACE_LOCATION};
        double applied_extra_yaw_radians = 0.0;
        double applied_extra_pitch_radians = 0.0;
        XrPosef applied_pose_delta = IdentityPose();
        XrPosef runtime_view_pose = IdentityPose();
        XrResult pivot_result = next_locate_space_(
            internal_view_space_, view_locate_info->space, internal_locate_time, &pivot_view_location);
        if (XR_SUCCEEDED(pivot_result)) {
            // Head prediction turned this call's views; the head they are
            // recomposed on takes the same turn about the same point, so the
            // eye offsets derived from them stay the runtime's own.
            pivot_view_location.pose =
                ToXrPose(ApplyHeadExtension(head_extension, ToHeadPose(pivot_view_location.pose)));
            runtime_view_pose = pivot_view_location.pose;
            const auto existing_frame =
                cached_pivot_pose_deltas_.find(view_locate_info->displayTime);
            if (existing_frame != cached_pivot_pose_deltas_.end()) {
                // Repeated world-space queries for one predicted display time
                // reuse the first logical Pivot update. VIEW-in-base is already
                // available from the runtime locate above, so the canonical
                // VIEW-space delta can be expressed in this base without an
                // additional runtime call.
                applied_pose_delta = ReexpressPoseDelta(
                    existing_frame->second.canonical_view_pose_delta, runtime_view_pose);
                pivot_view_location.pose = MultiplyPoses(runtime_view_pose, applied_pose_delta);
                applied_extra_yaw_radians = pivot_diagnostic_.eased_extra_yaw_radians;
                applied_extra_pitch_radians = pivot_diagnostic_.eased_extra_pitch_radians;
                pivot_frame_reused = true;
                pivot_frame_source_space = existing_frame->second.source_space;
            } else {
                pivot_result = ApplyPivotToLocatedSpace(internal_view_space_,
                                                         view_locate_info->space,
                                                         internal_locate_time,
                                                         pivotxr_active,
                                                         &pivot_view_location,
                                                         &applied_extra_yaw_radians,
                                                         &applied_extra_pitch_radians,
                                                         &applied_pose_delta,
                                                         true);
            }
        }
        if (XR_SUCCEEDED(pivot_result)) {
            const auto ensure_eye_offsets = [&](XrViewConfigurationType offset_view_configuration,
                                                uint32_t offset_count) {
                constexpr XrSpaceLocationFlags kPoseValid =
                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                const bool derived =
                    (pivot_view_location.locationFlags & kPoseValid) == kPoseValid && views && count >= offset_count &&
                    CacheEyeOffsetsFromLocatedViews(offset_view_configuration,
                                                    internal_locate_time,
                                                    runtime_view_pose,
                                                    std::span<const XrView>(views, count),
                                                    offset_count);
                return derived || EnsureEyeOffsets(
                                      session, offset_view_configuration, internal_locate_time, offset_count);
            };
            auto cache_applied_pose_delta = [&](const XrPosef& pose_delta) {
                if (pivot_frame_reused) {
                    auto frame = cached_pivot_pose_deltas_.find(view_locate_info->displayTime);
                    if (frame != cached_pivot_pose_deltas_.end()) {
                        CachePivotPoseDeltaForSpace(
                            frame->second, view_locate_info->space, pose_delta);
                    }
                    return;
                }
                pivot_frame_cached = CachePivotPoseDelta(view_locate_info->displayTime,
                                                          view_locate_info->space,
                                                          pose_delta,
                                                          runtime_view_pose);
                pivot_frame_source_space = view_locate_info->space;
            };
            const bool identity_pose_delta = IsIdentityPose(applied_pose_delta);
            if (identity_pose_delta) {
                pivot_diagnostic_.recomposition_mode = "identity";
                pivot_diagnostic_.has_eye_offsets = false;
                cache_applied_pose_delta(IdentityPose());
            } else if (IsQuadViewConfiguration(view_configuration_type) &&
                       ensure_eye_offsets(varjo_compatible_quadviews_active_
                                              ? view_configuration_type
                                              : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          varjo_compatible_quadviews_active_ ? count : 2)) {
                pivot_diagnostic_.recomposition_mode =
                    varjo_compatible_quadviews_active_ ? "quad_native_eye_offsets" : "quad_from_stereo_eye_offsets";
                cache_applied_pose_delta(applied_pose_delta);
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
                       ensure_eye_offsets(view_configuration_type, count)) {
                pivot_diagnostic_.recomposition_mode = "stereo_eye_offsets";
                cache_applied_pose_delta(applied_pose_delta);
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
                cache_applied_pose_delta(IdentityPose());
            }
            // LocateSpaceWithPivot owns the steady-state smoothed angles and the
            // activation envelope; do not write the eased output back onto them.
        } else {
            pivot_diagnostic_.recomposition_mode = "locate_space_failed";
        }
    } else if (!pivotxr_envelope_engaged) {
        // Idle, Pivot applies no change: one that has reached this frame
        // drops the anchor (kept with a captured origin), which the next
        // engagement takes anew, instead of queueing until then.
        if (view_locate_info && !pivotxr_reference_changes_.empty()) {
            std::erase_if(pivotxr_reference_changes_, [&](const PivotReferenceChange& change) {
                if (view_locate_info->displayTime < change.change_time) {
                    return false;
                }
                if (PivotAnchorAffectedBy(change.type)) {
                    pivotxr_translation_anchor_.reset();
                }
                return true;
            });
        }
        if (!pivotxr_origin_) pivotxr_translation_anchor_.reset();
        pivotxr_smoothed_extra_yaw_radians_ = 0.0;
        pivotxr_smoothed_extra_pitch_radians_ = 0.0;
        pivotxr_position_offset_ = {};
        pivotxr_yaw_step_ = 0;
        pivotxr_pitch_step_ = 0;
        pivotxr_yaw_step_glide_ = {};
        pivotxr_pitch_step_glide_ = {};
        pivotxr_activation_gain_ = 0.0;
        pivotxr_last_smoothing_wall_time_.reset();
        ResetPivotPoseDeltaContinuityState();
    }

    const bool depth_geometry_adjusted =
        depthxr_active &&
        (!NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0) ||
         !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0));
    const bool cache_depth_submission_geometry =
        depth_geometry_adjusted && depth_anchor_active_ &&
        view_locate_info && view_locate_info->displayTime != 0;
    std::vector<ViewAdjustmentData>& depth_native_views = locate_views_depth_native_scratch_;
    if (cache_depth_submission_geometry) {
        depth_native_views = adjusted_views;
    }

    if (depthxr_active && !NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0)) {
        ApplyStereoBoost(adjusted_views, resolved_settings_.depthxr.stereo_boost, view_layout);
    }
    if (depthxr_active && !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0)) {
        ApplyConvergence(adjusted_views, resolved_settings_.depthxr.convergence, view_layout);
    }
    if (cache_depth_submission_geometry) {
        CacheDepthSubmissionGeometry(view_locate_info->displayTime,
                                     view_locate_info->space,
                                     view_configuration_type,
                                     depth_native_views,
                                     adjusted_views);
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
        CacheQuadViewsFrame(
            view_locate_info->displayTime, std::span<const XrView>(views, count), gaze_diagnostic);
        if (varjo_compatible_quadviews_active_) {
            NoteVarjoNativeLocatedFocusLocked(view_locate_info->displayTime, views);
        }
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
    const bool depth_adjustment_active_for_info =
        depthxr_active &&
        (!NearlyEqual(resolved_settings_.depthxr.stereo_boost, 1.0) ||
         !NearlyEqual(resolved_settings_.depthxr.convergence, 0.0));
    const bool should_log_depth_view_info = depth_view_info_pending_ && depth_adjustment_active_for_info;
    if (pending_locate_views_diagnostics_ > 0 || should_log_depth_view_info ||
        should_log_pivot_diagnostic || should_log_quadviews_locate_heartbeat) {
        const double left_position_delta =
            adjusted_views[0].position.x - original_views[0].position.x;
        const double right_position_delta =
            count > 1 ? adjusted_views[1].position.x - original_views[1].position.x : 0.0;
        const double eye_separation_before_mm = ViewSeparationMeters(original_views) * 1000.0;
        const double eye_separation_after_mm = ViewSeparationMeters(adjusted_views) * 1000.0;
        const double effective_stereo_factor =
            eye_separation_before_mm > 0.0001
                ? eye_separation_after_mm / eye_separation_before_mm
                : 0.0;
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

        if (pending_locate_views_diagnostics_ > 0 || should_log_depth_view_info ||
            should_log_quadviews_locate_heartbeat) {
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
                   << ", pivotDriveQuery=" << pivot_drive_query
                   << ", pivotFrameReused=" << pivot_frame_reused
                   << ", pivotFrameCached=" << pivot_frame_cached
                   << ", pivotBaseSpace="
                   << DescribeSpace(view_locate_info ? view_locate_info->space : XR_NULL_HANDLE)
                   << ", pivotFrameSourceSpace=" << DescribeSpace(pivot_frame_source_space)
                   << ", leftYawDelta=" << FormatDiagnosticDouble(left_yaw_delta)
                   << ", rightYawDelta=" << FormatDiagnosticDouble(right_yaw_delta)
                   << ", leftInsetYawDelta=" << FormatDiagnosticDouble(left_inset_yaw_delta)
                   << ", rightInsetYawDelta=" << FormatDiagnosticDouble(right_inset_yaw_delta)
                   << ", leftPitchDelta=" << FormatDiagnosticDouble(left_pitch_delta)
                   << ", rightPitchDelta=" << FormatDiagnosticDouble(right_pitch_delta)
                   << ", depthRuntimeActive=" << depthxr_active
                   << ", stereoBoost=" << FormatDiagnosticDouble(resolved_settings_.depthxr.stereo_boost)
                   << ", convergence=" << FormatDiagnosticDouble(resolved_settings_.depthxr.convergence)
                   << ", depthAnchor=" << depth_anchor_active_
                   << ", eyeSeparationBeforeMm=" << FormatDiagnosticDouble(eye_separation_before_mm)
                   << ", eyeSeparationAfterMm=" << FormatDiagnosticDouble(eye_separation_after_mm)
                   << ", effectiveStereoFactor=" << FormatDiagnosticDouble(effective_stereo_factor)
                   << ", leftXDelta=" << FormatDiagnosticDouble(left_position_delta)
                   << ", rightXDelta=" << FormatDiagnosticDouble(right_position_delta)
                   << ", leftProjCenterDelta=" << FormatDiagnosticDouble(left_projection_center_delta)
                   << ", rightProjCenterDelta=" << FormatDiagnosticDouble(right_projection_center_delta)
                   << ", before:";
            AppendViewSummary(stream, original_views);
            stream << " after:";
            AppendViewSummary(stream, adjusted_views);
            logger_.Debug(stream.str());
            if (should_log_depth_view_info) {
                std::ostringstream depth_stream;
                depth_stream << "Depth view geometry applied: frameTime=" << locate_time
                             << ", locateCall=" << locate_views_call_count_
                             << ", viewConfig=" << ToString(view_configuration_type)
                             << ", viewCount=" << count
                             << ", stereoBoost=" << FormatDiagnosticDouble(resolved_settings_.depthxr.stereo_boost)
                             << ", convergence=" << FormatDiagnosticDouble(resolved_settings_.depthxr.convergence)
                             << ", depthAnchor=" << depth_anchor_active_
                             << ", eyeSeparationBeforeMm=" << FormatDiagnosticDouble(eye_separation_before_mm)
                             << ", eyeSeparationAfterMm=" << FormatDiagnosticDouble(eye_separation_after_mm)
                             << ", effectiveStereoFactor=" << FormatDiagnosticDouble(effective_stereo_factor)
                             << ", leftProjCenterDelta=" << FormatDiagnosticDouble(left_projection_center_delta)
                             << ", rightProjCenterDelta=" << FormatDiagnosticDouble(right_projection_center_delta);
                logger_.Info(depth_stream.str());
                depth_submission_info_not_before_time_ = locate_time;
                depth_view_info_pending_ = false;
            }
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
                   << ", driveQuery=" << pivot_drive_query
                   << ", logicalFrameReused=" << pivot_frame_reused
                   << ", logicalFrameCached=" << pivot_frame_cached
                   << ", locateBaseSpace="
                   << DescribeSpace(view_locate_info ? view_locate_info->space : XR_NULL_HANDLE)
                   << ", logicalFrameSourceSpace=" << DescribeSpace(pivot_frame_source_space)
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
    TeardownWatch watch(logger_, "xrDestroySpace", framegen_.TeardownWait(),
                        [this] { return framegen_.PresenterActivity(); },
                        framegen_teardowns_watched_.load(std::memory_order_acquire));
    // Frame Generation may still repeat a frame that refers to this space.
    watch.Step("Frame Generation letting the space go");
    if (!framegen_.ForgetSpace(space)) {
        // Its submission in flight (a runtime call past the teardown wait)
        // may name the space: the runtime frees it with the session.
        logger_.Error("Space " + FormatHandle(space) +
                      " is kept until its session ends: Frame Generation's presenter thread may still submit it.");
        return XR_SUCCESS;
    }
    // The layer lets the space go before the runtime does: once the runtime
    // has destroyed it, no other thread may pick it up as Pivot's anchor or
    // classify it, and a space the runtime then creates under the same handle
    // must not lose its tracking to this cleanup. A refused destroy restores
    // what classifies the space; the caches refill on their own.
    bool was_view = false, was_local = false, was_stage = false;
    std::optional<ApplicationActionSpace> action_space;
    {
        std::scoped_lock lock(mutex_);
        const auto remove_space_from_frame = [space](PivotPoseDeltaFrame& frame) {
            for (std::size_t index = 0; index < frame.space_pose_delta_count;) {
                if (frame.space_pose_deltas[index].space != space) {
                    ++index;
                    continue;
                }
                for (std::size_t move = index + 1;
                     move < frame.space_pose_delta_count; ++move) {
                    frame.space_pose_deltas[move - 1] = frame.space_pose_deltas[move];
                }
                --frame.space_pose_delta_count;
            }
            if (frame.source_space == space) {
                frame.source_space = XR_NULL_HANDLE;
            }
        };
        for (auto& [time, frame] : cached_pivot_pose_deltas_) {
            (void)time;
            remove_space_from_frame(frame);
        }
        if (pivotxr_last_matched_pose_delta_) {
            remove_space_from_frame(*pivotxr_last_matched_pose_delta_);
        }
        logged_pivot_space_conversions_.erase(space);
        failed_pivot_space_conversions_.erase(space);
        was_view = tracked_view_spaces_.erase(space) > 0;
        if (auto node = application_action_spaces_.extract(space)) action_space = std::move(node.mapped());
        was_local = tracked_local_spaces_.erase(space) > 0;
        was_stage = tracked_stage_spaces_.erase(space) > 0;
        if (pivotxr_translation_anchor_ && pivotxr_translation_anchor_->space == space) {
            pivotxr_translation_anchor_.reset();
        }
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
    watch.Step("in the runtime's xrDestroySpace");
    const XrResult result = next_destroy_space_(space);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        if (was_view) tracked_view_spaces_.insert(space);
        if (action_space) application_action_spaces_.emplace(space, std::move(*action_space));
        if (was_local) tracked_local_spaces_.insert(space);
        if (was_stage) tracked_stage_spaces_.insert(space);
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
        config_warnings_.clear();
        has_loaded_config_ = true;
        has_config_timestamp_ = false;
        has_failed_config_timestamp_ = false;
        last_failed_config_error_.clear();
        ++config_generation_;
        logger_.Info("No config file found. Using default settings.");
        return;
    }

    const auto timestamp = std::filesystem::last_write_time(config_path_, ec);
    const ParseResult loaded = LoadConfigFromFile(config_path_);
    if (!loaded.ok) {
        logger_.Error("Failed to parse config: " + loaded.error +
                      ". Using default settings; VectorXR enhancements are disabled until a valid config is loaded.");
        has_config_timestamp_ = false;
        if (!ec) {
            last_failed_config_write_time_ = timestamp;
            has_failed_config_timestamp_ = true;
        }
        last_failed_config_error_ = loaded.error;
        config_ = DefaultConfig();
        config_warnings_.clear();
        has_loaded_config_ = true;
        ++config_generation_;
        return;
    }

    config_ = loaded.document;
    config_warnings_ = loaded.warnings;
    has_loaded_config_ = true;
    has_failed_config_timestamp_ = false;
    last_failed_config_error_.clear();
    if (!ec) {
        has_config_timestamp_ = true;
        last_config_write_time_ = timestamp;
    }
    ++config_generation_;
    logger_.Info("Loaded config from " + PathToUtf8(config_path_));
    for (const std::string& line : UnloggedConfigWarningsLocked()) {
        logger_.Info(line);
    }
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

    // A normal instance teardown stops the watcher before ResetSessionState,
    // so remove this process's relay files here as well. Crashes can still
    // leave files behind; the app rejects them once their heartbeat is stale.
    std::filesystem::path relay_root;
    std::filesystem::path external_status_path;
    std::vector<std::string> relay_session_ids;
    {
        std::scoped_lock lock(mutex_);
        relay_root = runtime_relay_root_;
        external_status_path = external_status_path_;
        relay_session_ids.swap(runtime_relay_sessions_to_remove_);
        if (!runtime_relay_session_id_.empty()) {
            relay_session_ids.push_back(std::move(runtime_relay_session_id_));
        }
        runtime_relay_exit_status_.Disarm();
        runtime_relay_exit_control_.Disarm();
        // The public status file stays armed: its exit removal is
        // owner-checked, and covers a removal held up here.
        runtime_relay_last_applied_revision_ = 0;
        runtime_relay_acknowledged_revision_ = 0;
        runtime_relay_status_dirty_.store(false, std::memory_order_release);
    }
    for (const std::string& relay_session_id : relay_session_ids) {
        std::error_code ec;
        std::filesystem::remove(RuntimeStatusPath(relay_root, relay_session_id), ec);
        ec.clear();
        std::filesystem::remove(RuntimeControlPath(relay_root, relay_session_id), ec);
    }
    // The public status file only while it is still this process's: by now
    // another game may have written its own.
    if ((!relay_session_ids.empty() || external_status_remove_pending_) && !external_status_path.empty()) {
        RemoveExternalStatusIfOwned(external_status_path, GetCurrentProcessId());
        external_status_remove_pending_ = false;
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
        PollRuntimeRelay();
        PollFrameGenRecorder();
    }
}

void OpenXrLayer::PollFrameGenRecorder() {
    // Runs on the watcher thread, never on a frame path. The flight recorder
    // keeps only seconds of history, so an engagement's presenter waits
    // (runtime.wait) rotate away unless something freezes them. A Dynamic
    // stand-down (benefit guard, runtime halving or a probe) and a runtime
    // halving report both come with that engagement in recent history.
    // Without Log Level Debug or Frame Generation this is two atomic loads.
    if (!turbo_trace_.Enabled() || !framegen_owns_pacing_.load(std::memory_order_acquire)) {
        framegen_recorder_.Reset();
        return;
    }
    const FrameGenStatus status = framegen_.Status();
    // The trigger asks for nothing at the first poll of a recording or after
    // a new session reset the counts, and asks again at the next poll for a
    // request the recorder could not take (producers held it).
    framegen_recorder_.Poll(
        status.dynamic_stand_downs + status.dynamic_probes, status.runtime_halving,
        [&](FrameGenSnapshotTrigger::Event event) {
            if (event == FrameGenSnapshotTrigger::Event::kStandDown) {
                // a=stand-downs, b=probes so far this session.
                return turbo_trace_.RequestFrameGenSnapshot(
                    "framegen.standdown", static_cast<std::int64_t>(status.dynamic_stand_downs),
                    static_cast<std::int64_t>(status.dynamic_probes));
            }
            // a=frames submitted to the runtime per second, b=the game's
            // frames per second (both x1000).
            return turbo_trace_.RequestFrameGenSnapshot("framegen.halving",
                                                        static_cast<std::int64_t>(status.output_fps * 1000.0),
                                                        static_cast<std::int64_t>(status.app_fps * 1000.0));
        });
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
    std::vector<std::string> warning_lines;
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
            config_warnings_ = std::move(loaded.warnings);
            warning_lines = UnloggedConfigWarningsLocked();
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
        logger_.Info("Reloaded config from " + PathToUtf8(path));
        for (const std::string& line : warning_lines) {
            logger_.Info(line);
        }
    }
}

void OpenXrLayer::BeginInstanceLogLocked(const std::filesystem::path& log_path) {
    logger_.Initialize(log_path);
    // A fatal runtime error in the layer is named in this log before the game's crash reporting sees it.
    SetCrtFailureLogger(&logger_);
    // A new log: it shows the config's warnings again.
    logged_config_warnings_.clear();
}

std::vector<std::string> OpenXrLayer::UnloggedConfigWarningsLocked() {
    std::vector<std::string> lines;
    for (const std::string& warning : config_warnings_) {
        if (logged_config_warnings_.insert(warning).second) {
            lines.push_back("Config warning: " + warning);
        }
    }
    return lines;
}

OsdSnapshot OpenXrLayer::BuildOsdSnapshot() const {
    OsdSnapshot snapshot;
    // The OSD's language (osd_strings.h); Frame Generation's own row text
    // (OsdDescription) is still English.
    const OsdLanguage language=ResolveOsdLanguage(resolved_settings_.core.osd);
    const auto text=[language](OsdText id) { return std::string(OsdString(language,id)); };
    snapshot.application=current_exe_name_; snapshot.runtime=runtime_name_;
    const bool turbo_enabled=resolved_settings_.turbo.enabled && resolved_settings_.core.enabled;
    snapshot.turbo=text(turbo_yielded_to_framegen_?OsdText::TurboPausedForFrameGen:
        !turbo_enabled?OsdText::Disabled:!turbo_toggle_enabled_?OsdText::TurboEnabledOff:
        turbo_recovery_blocked_.load()?OsdText::TurboRecoveryBlocked:turbo_auto_suspended_.load()?OsdText::Suspended:
        turbo_effective_active_.load()?(turbo_effective_async_.load()?OsdText::TurboAsync:OsdText::TurboSequenced):
        OsdText::TurboEnabledWaiting);
    if(turbo_metrics_active_.load())snapshot.turbo+=text(OsdText::TurboAnalyzingSuffix);
    snapshot.experimental=turbo_experiment_.enabled;
    auto add=[&](OsdText name) { if (!snapshot.modules.empty()) snapshot.modules+="  /  "; snapshot.modules+=text(name); };
    if (resolved_settings_.core.enabled && resolved_settings_.depthxr.enabled && depthxr_toggle_enabled_) add(OsdText::ModuleDepth);
    if (resolved_settings_.core.enabled && resolved_settings_.pivotxr.enabled) add(OsdText::ModulePivot);
    if (turbo_enabled) add(OsdText::ModuleTurbo);
    if (framegen_owns_pacing_.load(std::memory_order_acquire)) add(OsdText::ModuleFrameGen);
    // A session that began in stereo renders no quad views, though the module is on.
    if (quadviews_session_active_.value_or(false) &&
        (!has_active_primary_view_configuration_ ||
         IsQuadViewConfiguration(active_primary_view_configuration_type_))) add(OsdText::ModuleQuadviews);
    const FrameGenOsdText framegen=framegen_.OsdDescription();
    snapshot.framegen=framegen.row;
    snapshot.compact_framegen=framegen.compact;
    snapshot.framegen_detail=framegen.detail;
    // What the generated frames use of the game's own data, in the OSD's language.
    for (const auto& [uses, note] : {std::pair{framegen.uses_depth, OsdText::FrameGenUsesDepth},
                                     std::pair{framegen.uses_motion_vectors, OsdText::FrameGenUsesMotionVectors}}) {
        if (uses) snapshot.framegen_detail+=(snapshot.framegen_detail.empty()?"":"  /  ")+text(note);
    }
    if (const std::string head=HeadPredictionOsdDetail(); !head.empty()) {
        snapshot.framegen_detail+=(snapshot.framegen_detail.empty()?"":"  /  ")+head;
    }
    snapshot.framegen_badge=framegen.badge;
    snapshot.framegen_fps=framegen.shown_fps;
    if (snapshot.modules.empty()) snapshot.modules=text(OsdText::Off);
    snapshot.pivot=OsdFormat(language,OsdText::Motion,text(OsdText::Disabled));
    snapshot.compact_pivot=snapshot.pivot;
    if (resolved_settings_.core.enabled && resolved_settings_.pivotxr.enabled) {
        const auto& profiles=resolved_settings_.pivotxr.profiles;
        const bool snap_effect=PivotSnapEffectActive();
        const PivotXrResolvedProfile& motion=ActivePivotProfile();
        // Motion reads as active from the moment it engages, not only once
        // its activation ramp becomes visible.
        const bool motion_applied=motion.behavior==PivotProfileBehavior::EnhancedMotion &&
            (!snap_effect || pivotxr_snap_stacked_) &&
            (pivotxr_engaged_ || pivotxr_activation_gain_>kPivotActivationGainEpsilon);
        const bool nudged=!PivotViewOffsetNearlyZero(pivotxr_manual_view_transition_.current) ||
            !PivotViewOffsetNearlyZero(pivotxr_profile_view_transition_.current);
        const bool position=motion_applied && !PivotViewOffsetNearlyZero(pivotxr_position_offset_);
        const bool continuous=motion.response_mode==PivotResponseMode::Continuous;
        const std::string state=text(motion_applied ? (pivotxr_engaged_ ? OsdText::Active : OsdText::MotionReleasing) :
            (snap_effect && !pivotxr_snap_stacked_ && pivotxr_quick_view_return_engaged_) ? OsdText::Suspended : OsdText::Off);
        // The snap line is only reserved for apps with Snap Views (or while
        // a snap is still on screen); otherwise nudges join the motion line.
        const bool snaps_configured=std::any_of(profiles.begin(),profiles.end(),
            [](const PivotXrResolvedProfile& profile) {
                return profile.behavior==PivotProfileBehavior::SnapViews && !profile.view_controls.quick_views.empty();
            });
        const bool snap_line=snaps_configured || snap_effect;
        std::string modifiers;
        if (position) modifiers+=text(OsdText::PlusPosition);
        if (nudged && !snap_line) modifiers+=text(OsdText::PlusNudge);
        snapshot.pivot=OsdFormat(language,OsdText::Motion,state);
        snapshot.compact_pivot=OsdFormat(language,OsdText::Motion,
            (motion_applied ? text(continuous ? OsdText::MotionContinuousShort : OsdText::MotionSteppedShort)+" " : std::string())+state)+modifiers;
        if (motion_applied) snapshot.pivot+=" / "+text(continuous ? OsdText::MotionContinuous : OsdText::MotionStepped)+modifiers+" / "+motion.name;
        else snapshot.pivot+=modifiers;
        if (snap_line && snap_effect && pivotxr_quick_view_profile_index_<profiles.size()) {
            const auto& owner=profiles[pivotxr_quick_view_profile_index_];
            const bool entering=pivotxr_quick_view_transition_.active || pivotxr_quick_view_retarget_pending_;
            snapshot.pivot_secondary=text(!pivotxr_quick_view_active_ ? OsdText::SnapReturning : entering ? OsdText::SnapEntering : OsdText::SnapActive);
            if (pivotxr_snap_stacked_ && motion_applied) snapshot.pivot_secondary+=text(OsdText::StackedSuffix);
            if (nudged) snapshot.pivot_secondary+=text(OsdText::PlusNudge);
            snapshot.compact_pivot_secondary=snapshot.pivot_secondary;
            if (pivotxr_quick_view_index_<owner.view_controls.quick_views.size())
                snapshot.pivot_secondary+=" / "+owner.view_controls.quick_views[pivotxr_quick_view_index_].name;
            snapshot.pivot_secondary+=" / "+owner.name;
        } else if (snap_line) {
            snapshot.pivot_secondary=text(nudged ? OsdText::SnapOffNudge : OsdText::SnapOff);
            snapshot.compact_pivot_secondary=snapshot.pivot_secondary;
        }
    }
    const auto& e=resolved_settings_.turbo.experimental;
    if (e.enabled!=turbo_experiment_.enabled || (e.enabled &&
        (e.wait_for_submit!=turbo_experiment_.wait_for_submit || e.sample_at_entry!=turbo_experiment_.sample_at_entry ||
         e.prediction_percent!=turbo_experiment_.prediction_percent || e.frame_limit!=turbo_experiment_.frame_limit)))
        snapshot.restart=text(OsdText::RestartTurboExperiments);
    if (deferred_quadviews_config_active_) snapshot.restart+=(snapshot.restart.empty()?"":" + ")+text(OsdText::RestartQuadviews);
    if (active_session_ != XR_NULL_HANDLE && resolved_settings_.core.enabled && resolved_settings_.framegen.enabled &&
        !framegen_session_enabled_)
        snapshot.restart+=(snapshot.restart.empty()?"":" + ")+text(OsdText::RestartFrameGeneration);
    snapshot.framegen_session=framegen_session_enabled_;
    if (resolved_settings_.core.osd.show_performance || resolved_settings_.core.osd.compact_show_performance)
        snapshot.performance=DescribePerformance(performance_.Reading(),language);
    // Diagnostic modes a tester may leave on name themselves after the brand:
    // debug logging (slower, with large traces) and, in a Frame Generation
    // session, a diagnostic view drawn into generated frames.
    if (resolved_settings_.core.log_level==LogLevel::Debug) snapshot.diagnostics=text(OsdText::DiagnosticsDebugLog);
    if (framegen_session_enabled_ && resolved_settings_.framegen.diagnostic_view!=FrameGenDiagnosticView::kOff) {
        const FrameGenDiagnosticView view=resolved_settings_.framegen.diagnostic_view;
        snapshot.diagnostics+=std::string(snapshot.diagnostics.empty()?"":" \xC2\xB7 ")+
            text(view==FrameGenDiagnosticView::kFlow?OsdText::DiagnosticsFlowView:view==FrameGenDiagnosticView::kConfidence?
                 OsdText::DiagnosticsConfidenceView:OsdText::DiagnosticsTintView);
    }
    if (framegen_session_enabled_) {
        const FrameGenStatus flow = framegen_.Status();
        if (!flow.interpolate_flow_active.empty()) {
            snapshot.diagnostics += std::string(snapshot.diagnostics.empty() ? "" : " \xC2\xB7 ") +
                text(flow.interpolate_flow_active == "hardware" ? OsdText::FlowHardware : OsdText::FlowShader);
        }
    }
    // ...and Frame Generation research switches taken from its research
    // file (framegen-research.txt), which outlives the flight it was for.
    if (framegen_session_enabled_ && framegen_.ResearchFileInUse())
        snapshot.diagnostics+=std::string(snapshot.diagnostics.empty()?"":" \xC2\xB7 ")+text(OsdText::DiagnosticsResearch);
    return snapshot;
}

void OpenXrLayer::PrepareOsd() {
    const auto prepare_start=std::chrono::steady_clock::now();
    const auto record=[&]{osd_.RecordPrepare(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-prepare_start).count());};
    const auto& settings=resolved_settings_.core.osd;
    if (!settings.enabled && !osd_was_enabled_) {record();return;}
    osd_was_enabled_=settings.enabled;
    // Off with the suite switch too (RefreshResolvedSettings).
    if (settings.enabled) {
        const auto now=std::chrono::steady_clock::now();
        if (!osd_last_input_poll_ || now-*osd_last_input_poll_>=kInputBindingPollInterval) {
            osd_last_input_poll_=now;
            const InputBinding* toggle=PollInputBindingsDown(settings.toggle_bindings);
            const InputBinding* cycle=PollInputBindingsDown(settings.cycle_bindings);
            osd_toggle_down_=toggle!=nullptr;
            osd_cycle_down_=cycle!=nullptr;
            if (toggle) osd_toggle_sound_=toggle->sound;
            if (cycle) osd_cycle_sound_=cycle->sound;
        }
    }
    // The status strings are only read when the worker rasterizes (at the
    // refresh rate), so build them then instead of on every EndFrame.
    std::optional<OsdSnapshot> snapshot;
    if (settings.enabled && osd_.SnapshotDue()) snapshot=BuildOsdSnapshot();
    const auto [before, after]=osd_.Prepare(settings, std::move(snapshot), osd_toggle_down_, osd_cycle_down_,
                                            osd_settings_revision_);
    record();
    const auto sound=[](const std::optional<SoundFeedback>& pressed, const std::vector<InputBinding>& bindings) {
        return pressed ? *pressed : bindings.empty() ? SoundFeedback{} : bindings.front().sound;
    };
    // The PERFORMANCE row's sampler runs only while the panel is shown in a
    // layout that has the row (SetWanted never waits; called on a change).
    const bool performance_wanted=settings.enabled && after.shown &&
        (after.compact ? settings.compact_show_performance : settings.show_performance);
    if (performance_wanted!=performance_wanted_.load(std::memory_order_relaxed)) {
        performance_wanted_.store(performance_wanted, std::memory_order_relaxed);
        performance_.SetWanted(performance_wanted);
    }
    if (before.compact!=after.compact) SoundPlayer::Instance().PlayTransition(sound(osd_cycle_sound_,settings.cycle_bindings),after.compact,dll_directory_,resolved_settings_.core.sound_volume);
    if (before.shown!=after.shown) {
        // Research flights mark moments by toggling the OSD where something
        // looked wrong: exact in the log, and in the trace (row "osd").
        logger_.Info(std::string("OSD ")+(after.shown?"shown":"hidden")+" (a marker when toggled by its binding).");
        framegen_.Trace().Record("osd", 0, after.shown?1:0);
        SoundPlayer::Instance().PlayTransition(sound(osd_toggle_sound_,settings.toggle_bindings),after.shown,dll_directory_,resolved_settings_.core.sound_volume);
    }
}

void OpenXrLayer::RefreshResolvedSettings() {
    // Re-resolving settings is expensive (string compares, copies, logging
    // checks); the result only changes when the config document or the active
    // session changes, so skip the work otherwise.
    if (resolved_settings_generation_ == config_generation_ && resolved_settings_session_ == active_session_) {
        if (resolved_settings_.core.enabled && resolved_settings_.turbo.enabled) {
            turbo_frame_interception_required_.store(true, std::memory_order_release);
        }
        return;
    }
    resolved_settings_generation_ = config_generation_;
    resolved_settings_session_ = active_session_;

    const ResolvedRuntimeConfig previous = resolved_settings_;
    resolved_settings_ = ResolveRuntimeConfig(config_, current_exe_name_);
    // The suite switch leaves the layer inert: the OSD (and its bindings)
    // turns off with the Enhancements.
    if (!resolved_settings_.core.enabled) {
        resolved_settings_.core.osd.enabled = false;
    }
    framegen_.Configure(resolved_settings_.framegen);
    framegen_.SetConfigured(resolved_settings_.core.enabled && resolved_settings_.framegen.enabled);
    // Turbo decouples the app from runtime pacing; Frame Generation locks it
    // to half the refresh. A session Frame Generation owns keeps Turbo off.
    turbo_yielded_to_framegen_ = false;
    if (framegen_owns_pacing_.load(std::memory_order_acquire)) {
        turbo_yielded_to_framegen_ = resolved_settings_.turbo.enabled;
        resolved_settings_.turbo.enabled = false;
    }
    // OpenXR Toolkit above VectorXR would get the game's four quad views,
    // which it does not support: it passes them on with room for two, and
    // the game crashes (issue 108). The game runs stereo instead, as with
    // Quadviews off.
    if (openxr_toolkit_above_ && !varjo_compatible_quadviews_active_ && resolved_settings_.quadviews.enabled) {
        resolved_settings_.quadviews.enabled = false;
        if (!openxr_toolkit_quadviews_logged_) {
            openxr_toolkit_quadviews_logged_ = true;
            logger_.Error("Quadviews is off for this game: OpenXR Toolkit is loaded above VectorXR, and it does not "
                          "support quad views. Turn OpenXR Toolkit off for this game (or set its per-game bypass) "
                          "to use Quadviews.");
        }
    }

    osd_monitoring_.store(resolved_settings_.core.osd.enabled, std::memory_order_relaxed);
    // The performance row's sampler stops with the OSD; PrepareOsd starts it
    // while a shown layout has the row (SetWanted never waits).
    if (!resolved_settings_.core.osd.enabled && performance_wanted_.load(std::memory_order_relaxed)) {
        performance_wanted_.store(false, std::memory_order_relaxed);
        performance_.SetWanted(false);
    }
    osd_last_input_poll_.reset();
    ++osd_settings_revision_;
    const bool configured_core_active = resolved_settings_.core.enabled;
    const bool configured_quadviews_active =
        configured_core_active && resolved_settings_.quadviews.enabled;
    if (active_session_ != XR_NULL_HANDLE && quadviews_session_active_.has_value() &&
        configured_quadviews_active != *quadviews_session_active_) {
        if (!deferred_quadviews_config_active_.has_value() ||
            *deferred_quadviews_config_active_ != configured_quadviews_active) {
            logger_.Info(std::string("Quadviews ") +
                         (configured_quadviews_active ? "enable" : "disable") +
                         " change deferred until the OpenXR application exits; this session remains " +
                         (*quadviews_session_active_ ? "enabled." : "disabled."));
        }
        deferred_quadviews_config_active_ = configured_quadviews_active;

        if (*quadviews_session_active_) {
            // Keep the topology and the active profile's settings intact. A disabled
            // custom profile can otherwise resolve to unrelated default values while
            // its four-view session is still running.
            resolved_settings_.quadviews = previous.quadviews;
            if (!configured_core_active) {
                // The suite master switch still disables every topology-safe feature.
                // Core remains internally active only so the latched Quadviews frame
                // path can finish the current session safely.
                resolved_settings_.core.enabled = true;
                resolved_settings_.depthxr.enabled = false;
                resolved_settings_.pivotxr.enabled = false;
                resolved_settings_.turbo.enabled = false;
                resolved_settings_.framegen.enabled = false;
                resolved_settings_.framegen.extend_head_prediction = false;
            }
        }
    } else {
        deferred_quadviews_config_active_.reset();
    }

    if (resolved_settings_.core.enabled && resolved_settings_.turbo.enabled) {
        // Do not disarm this flag on a live-session settings change: once a
        // Turbo pipeline has been established, Wait/Begin/End interception is
        // required until ResetTurboFrameState balances and clears it.
        turbo_frame_interception_required_.store(true, std::memory_order_release);
    }
    if (!SameInputBindings(previous.depthxr_bindings.toggle_enabled_bindings,
                           resolved_settings_.depthxr_bindings.toggle_enabled_bindings) ||
        previous.depthxr.enabled != resolved_settings_.depthxr.enabled) {
        ResetDepthToggleState();
    }
    if (!SameInputBindings(previous.depthxr_bindings.toggle_anchor_bindings,
                           resolved_settings_.depthxr_bindings.toggle_anchor_bindings) ||
        previous.depthxr.depth_anchor != resolved_settings_.depthxr.depth_anchor) {
        ResetDepthAnchorToggleState();
    }
    if (!SamePivotActivationSet(previous.pivotxr, resolved_settings_.pivotxr)) {
        ResetPivotInputStateForConfigChange();
        has_logged_pivotxr_spike_mode_ = false;
    }
    if (!SameInputBindings(previous.turbo.toggle_bindings, resolved_settings_.turbo.toggle_bindings) ||
        previous.turbo.enabled != resolved_settings_.turbo.enabled) {
        ResetTurboToggleState();
    }
    // The OSD's transition sounds follow the binding last pressed; a changed
    // list (its sounds included) plays its own again.
    if (!SameInputBindings(previous.core.osd.toggle_bindings, resolved_settings_.core.osd.toggle_bindings)) {
        osd_toggle_sound_.reset();
    }
    if (!SameInputBindings(previous.core.osd.cycle_bindings, resolved_settings_.core.osd.cycle_bindings)) {
        osd_cycle_sound_.reset();
    }
    if (!SameInputBinding(previous.quadviews.diagnostic_visualization_binding,
                          resolved_settings_.quadviews.diagnostic_visualization_binding)) {
        ResetQuadViewsDiagnosticVisualizationState();
    }
    if (!SameInputBindings(previous.turbo.metrics_bindings, resolved_settings_.turbo.metrics_bindings) ||
        previous.turbo.metrics_mode != resolved_settings_.turbo.metrics_mode) {
        // Re-prime the capture binding's edge detector; a mode change also
        // disarms binding-gated capture so it always starts paused. This may
        // run on the config watcher's thread while the frame thread polls the
        // detector without the lock, so the frame thread does the reset.
        turbo_metrics_binding_reset_pending_.store(true, std::memory_order_release);
    }
    if (previous.turbo.pacing_mode != resolved_settings_.turbo.pacing_mode ||
        previous.turbo.runtime_pins != resolved_settings_.turbo.runtime_pins) {
        // Re-resolve at the next engaged frame so a UI change (mode, pins,
        // re-discover) applies without restarting the session.
        turbo_pacing_resolved_ = false;
    }
    frame_pacing_debug_enabled_.store(resolved_settings_.core.log_level == LogLevel::Debug,
                                      std::memory_order_relaxed);
    logger_.SetLevel(resolved_settings_.core.log_level);
    turbo_trace_.SetDebugEnabled(logger_, resolved_settings_.core.log_level == LogLevel::Debug);
    framegen_.SetTraceEnabled(resolved_settings_.core.log_level == LogLevel::Debug);
    logger_.SetRetentionFiles(resolved_settings_.core.log_retention_files);
    if (!last_logged_settings_ || !SameSettings(*last_logged_settings_, resolved_settings_)) {
        // The inset's motion under the settings it moved with (a switch
        // between Eye and Head, or new gaze smoothing, starts a new summary).
        if (quadviews_inset_motion_.frames > 0) {
            LogQuadViewsInsetMotion("settings-change", previous.quadviews.tracking_mode);
        }
        LogResolvedSettings(resolved_settings_);
        last_logged_settings_ = resolved_settings_;
        depth_view_info_pending_ = true;
        depth_submission_info_pending_ = true;
        depth_submission_info_not_before_time_.reset();
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        pending_pivot_diagnostics_ = kPivotDiagnosticBurstCount;
        pending_eye_gaze_diagnostics_ = 10;
        pending_eye_gaze_sync_diagnostics_ = 10;
        ResetQuadViewsDebugHeartbeatState();
    }

    UpdateEyeGazeResources(previous.quadviews.tracking_mode);

    const bool wants_varjo_rendering_gaze =
        varjo_compatible_quadviews_active_ && varjo_foveated_rendering_extension_requested_ &&
        IsQuadViewsActive() && resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye;
    if (wants_varjo_rendering_gaze && active_session_ != XR_NULL_HANDLE &&
        varjo_native_combined_eye_space_ == XR_NULL_HANDLE &&
        !varjo_native_foveation_resources_attempted_) {
        const XrResult varjo_result = CreateVarjoNativeFoveationResources(active_session_);
        if (XR_FAILED(varjo_result)) {
            logger_.Info("Native Varjo rendering-gaze resources unavailable; the runtime will use its fixed-center "
                         "focus fallback for this session. result=" +
                         FormatHex(static_cast<uint64_t>(varjo_result)));
        }
    } else if (!wants_varjo_rendering_gaze && varjo_native_foveation_resources_attempted_) {
        DestroyVarjoNativeFoveationResources();
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEndSession", &function))) {
        next_end_session_ = reinterpret_cast<PFN_xrEndSession>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrGetVisibilityMaskKHR", &function))) {
        next_get_visibility_mask_khr_ = reinterpret_cast<PFN_xrGetVisibilityMaskKHR>(function);
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
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrPollEvent", &function))) {
        next_poll_event_ = reinterpret_cast<PFN_xrPollEvent>(function);
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
    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrEnumerateBoundSourcesForAction", &function))) {
        next_enumerate_bound_sources_for_action_ =
            reinterpret_cast<PFN_xrEnumerateBoundSourcesForAction>(function);
    }
}

void OpenXrLayer::ResetSessionState() {
    last_logged_swapchain_formats_.clear();
    // Both xrDestroySession and an xrDestroyInstance that skipped it end a session here.
    if (active_session_ != XR_NULL_HANDLE) {
        logger_.Info("Thread census at session end: " + thread_census_.Summary());
        // The probe's frames (xrDestroyInstance forgets what it owned first).
        if (spacewarp_counts_.frames > 0) {
            LogSpaceWarpProbeSummaryLocked("session end");
        }
        if (handshake_census_.Active() && !census_swapchains_.empty()) {
            CensusUpdateSwapchainsLocked();
            LogCensusSwapchainCallsLocked("session end");
        }
    }
    census_swapchains_.clear();
    census_roles_.clear();
    census_swapchain_formats_.clear();
    spacewarp_counts_ = {};
    spacewarp_counts_summarized_ = {};
    spacewarp_projection_frames_ = 0;
    spacewarp_first_frame_logged_ = false;
    spacewarp_last_summary_ = {};
    spacewarp_last_view_.clear();
    application_action_spaces_.clear();
    runtime_latest_wait_prediction_.store(0);
    runtime_begun_prediction_.store(0);
    runtime_end_floor_.store(0);
    runtime_end_raises_.store(0);
    osd_.Shutdown();
    osd_monitoring_.store(false);
    osd_should_render_.store(true);
    osd_last_input_poll_.reset();
    osd_toggle_down_=false; osd_cycle_down_=false; osd_was_enabled_=false;osd_vulkan_.store(false);turbo_metrics_active_.store(false);
    // Resolve the settings again for the next session, even one the runtime
    // gives this session's handle (Virtual Desktop under OpenComposite):
    // what the resolve sets, such as osd_monitoring_, starts over here.
    resolved_settings_generation_ = ~0ull;
    if (!runtime_relay_session_id_.empty()) {
        runtime_relay_sessions_to_remove_.push_back(runtime_relay_session_id_);
        runtime_relay_session_id_.clear();
        runtime_relay_exit_status_.Disarm();
        runtime_relay_exit_control_.Disarm();
        // The public status file stays armed (owner-checked at exit): a game
        // that ends its session and exits before the watcher's next poll
        // still has it removed.
        runtime_relay_last_applied_revision_ = 0;
        runtime_relay_acknowledged_revision_ = 0;
        runtime_relay_status_dirty_.store(false, std::memory_order_release);
    }
    {
        std::scoped_lock diagnostic_lock(input_binding_diagnostic_mutex_);
        input_binding_diagnostic_states_.clear();
    }
    if (varjo_native_foveation_diagnostic_.locate_calls > 0) {
        LogVarjoNativeFoveationSummaryLocked("session-teardown", true);
    }
    varjo_native_foveation_diagnostic_ = {};
    if (quadviews_inset_motion_.frames > 0) {
        LogQuadViewsInsetMotion("session-end", resolved_settings_.quadviews.tracking_mode);
    }
    ResetD3D11QuadViewsCompositor();
    active_session_ = XR_NULL_HANDLE;
    quadviews_session_active_.reset();
    deferred_quadviews_config_active_.reset();
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
    pivotxr_yaw_step_glide_ = {};
    pivotxr_pitch_step_glide_ = {};
    pivotxr_activation_gain_ = 0.0;
    pivotxr_position_offset_ = {};
    pivotxr_last_smoothing_wall_time_.reset();
    head_prediction_limit_.Reset();
    head_prediction_last_probe_ = {};
    head_prediction_stats_ = {};
    head_prediction_stats_since_ = {};
    head_prediction_beyond_ms_ = 0.0;
    head_prediction_logged_unavailable_ = false;
    ResetDepthToggleState();
    ResetQuadViewsDiagnosticVisualizationState();
    ResetTurboToggleState();
    ResetTurboFrameState();
    internal_local_space_ = XR_NULL_HANDLE;
    internal_view_space_ = XR_NULL_HANDLE;
    internal_stage_space_ = XR_NULL_HANDLE;
    varjo_native_view_space_ = XR_NULL_HANDLE;
    varjo_native_combined_eye_space_ = XR_NULL_HANDLE;
    varjo_native_rendering_gaze_tracked_ = false;
    has_logged_varjo_native_rendering_gaze_active_ = false;
    has_logged_varjo_native_rendering_gaze_unavailable_ = false;
    varjo_native_foveation_resources_attempted_ = false;
    varjo_native_rendering_gaze_transition_logs_remaining_ = 8;
    quadviews_action_set_ = XR_NULL_HANDLE;
    quadviews_eye_gaze_action_ = XR_NULL_HANDLE;
    quadviews_eye_gaze_space_ = XR_NULL_HANDLE;
    eye_gaze_interaction_profile_path_ = XR_NULL_PATH;
    eye_gaze_pose_path_ = XR_NULL_PATH;
    eye_gaze_resources_ready_ = false;
    eye_gaze_bindings_ready_ = false;
    eye_gaze_binding_augmentation_disabled_ = false;
    eye_gaze_action_set_attachment_.Reset();
    has_logged_eye_gaze_focus_active_ = false;
    has_logged_eye_gaze_focus_unavailable_ = false;
    eye_gaze_unavailable_streak_ = 0;
    quadviews_smoothed_focus_yaw_radians_ = 0.0;
    quadviews_smoothed_focus_pitch_radians_ = 0.0;
    quadviews_raw_focus_yaw_radians_ = 0.0;
    quadviews_raw_focus_pitch_radians_ = 0.0;
    quadviews_raw_focus_time_ = 0;
    quadviews_raw_focus_valid_ = false;
    quadviews_last_focus_smoothing_wall_time_.reset();
    quadviews_last_valid_gaze_wall_time_.reset();
    quadviews_eye_gaze_loss_started_wall_time_.reset();
    quadviews_eye_gaze_loss_was_locate_failure_ = false;
    quadviews_has_seen_valid_gaze_ = false;
    quadviews_compositor_recovery_.Reset();
    pending_quadviews_pixel_diagnostics_ = 0;
    active_primary_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    active_runtime_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    has_active_primary_view_configuration_ = false;
    has_logged_quad_view_short_count_ = false;
    has_logged_pivotxr_spike_mode_ = false;
    has_logged_quadviews_view_configuration_capabilities_ = false;
    has_logged_visibility_mask_mapping_ = false;
    logged_visibility_mask_shapes_ = 0;
    tracked_view_spaces_.clear();
    tracked_local_spaces_.clear();
    tracked_stage_spaces_.clear();
    cached_eye_offset_poses_.clear();
    cached_eye_offsets_display_time_ = 0;
    cached_pivot_pose_deltas_.clear();
    logged_pivot_space_conversions_.clear();
    failed_pivot_space_conversions_.clear();
    cached_depth_submission_geometry_.clear();
    cached_quadviews_frames_.Clear();
    quadviews_placement_mismatch_frames_ = 0;
    quadviews_placement_mismatch_logs_remaining_ = 3;
    quadviews_has_previous_focus_rect_ = {};
    quadviews_focus_max_step_px_ = {};
    quadviews_inset_motion_ = {};
    quadviews_inset_motion_since_.reset();
    ResetEyeGazeInputState();
    ResetSwapchainState();
}

void OpenXrLayer::ResetInstanceState() {
    instance_requested_session_ = false;
    instance_enabled_extension_count_ = 0;
    application_action_spaces_.clear();
    application_pose_actions_.clear();
    application_action_owners_.clear();
    application_attached_action_sets_.clear();
    application_eye_gaze_bindings_.clear();
    application_eye_gaze_binding_chain_unsupported_ = false;
    quad_views_extension_requested_ = false;
    varjo_foveated_rendering_extension_requested_ = false;
    application_can_select_quad_views_ = false;
    quad_view_configuration_offered_ = false;
    d3d11_graphics_extension_requested_ = false;
    d3d12_extension_enabled_ = false;
    vulkan_enable_version_ = 0;
    vulkan_enable1_requested_ = false;
    vulkan_bridge_extensions_ = false;
    eye_gaze_extension_enabled_ = false;
    next_get_visibility_mask_khr_ = nullptr;
    defer_quadviews_swapchain_releases_ = false;
    runtime_name_.clear();
    runtime_version_.clear();
    system_name_.clear();
    system_vendor_id_ = 0;
    graphics_api_.clear();
    turbo_pacing_resolved_ = false;
    cached_quadviews_stereo_recommended_width_ = 0;
    cached_quadviews_stereo_recommended_height_ = 0;
    cached_quadviews_stereo_max_width_ = 0;
    cached_quadviews_stereo_max_height_ = 0;
    varjo_native_view_configuration_calls_ = 0;
    has_logged_varjo_native_view_configuration_count_ = false;
    has_logged_varjo_native_view_configuration_signature_limit_ = false;
    logged_varjo_native_view_configuration_signatures_.clear();
    has_logged_system_properties_ = false;
    spacewarp_fb_owned_ = false;
    spacewarp_ext_owned_ = false;
    spacewarp_motion_vector_size_ = {};
    spacewarp_properties_logged_ = false;
    spacewarp_config_views_logged_ = false;
}

bool OpenXrLayer::IsQuadViewsActive() const {
    const bool configured_active = resolved_settings_.core.enabled && resolved_settings_.quadviews.enabled;
    return ResolveQuadViewsSessionActive(configured_active, quadviews_session_active_);
}

bool OpenXrLayer::IsQuadViewsEmulationActive() const {
    return IsQuadViewsActive() && !varjo_compatible_quadviews_active_ &&
           d3d11_graphics_extension_requested_;
}

bool OpenXrLayer::QuadViewsOverlayWithheld() const {
    // Composing the quad views itself, Frame Generation captures their views
    // instead (ClaimQuadFrame), and draws the overlay into its own images.
    return std::any_of(d3d11_quadviews_compositor_.targets.begin(), d3d11_quadviews_compositor_.targets.end(),
                       [this](const QuadViewsCompositionTarget& target) {
                           return target.swapchain != XR_NULL_HANDLE && framegen_.CapturesFrom(target.swapchain);
                       });
}

bool OpenXrLayer::QuadViewsDiagnosticVisualizationAvailable() const {
    // Not with the suite switch off, though a Quadviews session keeps its
    // views until the game restarts.
    return config_.core.enabled && IsQuadViewsEmulationActive() && d3d11_quadviews_compositor_.context != nullptr &&
           !QuadViewsOverlayWithheld();
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

bool OpenXrLayer::IsSuiteEnabled() {
    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    return config_.core.enabled;
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

bool OpenXrLayer::FinalizeEyeGazeBindings() {
    if (!eye_gaze_resources_ready_ || eye_gaze_binding_augmentation_disabled_) {
        return false;
    }
    if (eye_gaze_bindings_ready_) {
        return true;
    }
    if (eye_gaze_action_set_attachment_.SessionAttachmentCompleted()) {
        return false; // Never rebind attached actions or recover by reattachment.
    }
    if (application_eye_gaze_binding_chain_unsupported_) {
        logger_.Info("Private eye-gaze binding skipped: application extension-chain semantics cannot be replayed; "
                     "preserving application bindings and using head/static focus offsets.");
    } else {
        const auto merge = [&](bool include_attached_application_bindings) {
            std::vector<XrActionSuggestedBinding> merged;
            const auto add_unique = [&](const XrActionSuggestedBinding& binding) {
                if (std::none_of(merged.begin(), merged.end(), [&](const auto& existing) {
                        return existing.action == binding.action && existing.binding == binding.binding;
                    })) {
                    merged.push_back(binding);
                }
            };
            for (const auto& binding : application_eye_gaze_bindings_) {
                const auto owner = application_action_owners_.find(binding.action);
                const bool attached = owner != application_action_owners_.end() &&
                                      application_attached_action_sets_.contains(owner->second);
                if (!attached || include_attached_application_bindings) add_unique(binding);
            }
            add_unique({quadviews_eye_gaze_action_, eye_gaze_pose_path_});
            return merged;
        };
        const auto suggest = [&](const std::vector<XrActionSuggestedBinding>& merged) {
            XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            info.interactionProfile = eye_gaze_interaction_profile_path_;
            info.countSuggestedBindings = static_cast<uint32_t>(merged.size());
            info.suggestedBindings = merged.data();
            const XrResult result = next_suggest_interaction_profile_bindings_(instance_, &info);
            logger_.Info("Finalized eye-gaze binding suggestions before attachment: result=" +
                         std::to_string(static_cast<int>(result)) +
                         ", appBindings=" + std::to_string(application_eye_gaze_bindings_.size()) +
                         ", mergedBindings=" + std::to_string(merged.size()) + ", privatePriority=0");
            return result;
        };
        const auto all_bindings = merge(true);
        XrResult result = suggest(all_bindings);
        if (result == XR_ERROR_ACTIONSETS_ALREADY_ATTACHED) {
            // A later session of the same instance: the application's gaze
            // actions were attached before and can never be suggested again,
            // which rejects the whole list. Suggest without them.
            const auto unattached = merge(false);
            if (unattached.size() < all_bindings.size()) {
                logger_.Info("Application eye-gaze actions were attached in an earlier session and cannot be "
                             "re-suggested; suggesting VectorXR's gaze binding without them.");
                result = suggest(unattached);
            }
        }
        if (result == XR_SUCCESS) {
            eye_gaze_bindings_ready_ = true;
            return true;
        }
        // A rejected suggestion leaves the previous successful application
        // suggestions intact. Do not make optional gaze fail the app's attach.
        logger_.Info("Private eye-gaze binding rejected; preserving application attachment and using "
                     "head/static focus offsets.");
    }
    eye_gaze_binding_augmentation_disabled_ = true;
    eye_gaze_action_set_attachment_.ClearPrivateActionSet();
    return false;
}

void OpenXrLayer::ResetEyeGazeInputState() {
    last_app_action_sync_time_.reset();
    last_eye_gaze_self_sync_time_.reset();
    last_app_action_sync_attempt_time_.reset();
    last_eye_gaze_self_sync_attempt_time_.reset();
    last_app_action_sync_result_.reset();
    last_eye_gaze_self_sync_result_.reset();
    observed_session_state_ = XR_SESSION_STATE_UNKNOWN;
    eye_gaze_sync_not_focused_ = false;
    eye_gaze_sync_transition_logs_remaining_ = 32;
    last_eye_gaze_bound_sources_probe_.reset();
    eye_gaze_bound_sources_diagnostic_ = "not-probed";
    eye_gaze_unfocused_outcome_.clear();
    eye_gaze_unfocused_last_orientation_.reset();
    eye_gaze_unfocused_identical_frames_ = 0;
    eye_gaze_unfocused_logs_remaining_ = 16;
    eye_gaze_inactive_since_.reset();
    eye_gaze_no_data_logged_ = false;
}

void OpenXrLayer::RecordEyeGazeSyncResult(bool application, XrResult result) {
    auto& previous = application ? last_app_action_sync_result_ : last_eye_gaze_self_sync_result_;
    auto& attempt = application ? last_app_action_sync_attempt_time_ : last_eye_gaze_self_sync_attempt_time_;
    auto& success = application ? last_app_action_sync_time_ : last_eye_gaze_self_sync_time_;
    attempt = std::chrono::steady_clock::now();
    if (result == XR_SUCCESS) {
        success = attempt;
        eye_gaze_sync_not_focused_ = false;
    } else {
        success.reset();
        if (result == XR_SESSION_NOT_FOCUSED) {
            eye_gaze_sync_not_focused_ = true;
            last_app_action_sync_time_.reset();
            last_eye_gaze_self_sync_time_.reset();
        }
    }
    if ((!previous || *previous != result) && eye_gaze_sync_transition_logs_remaining_ > 0) {
        --eye_gaze_sync_transition_logs_remaining_;
        logger_.Info(std::string("Eye-gaze ") + (application ? "application" : "private") +
                     " sync result changed: result=" + std::to_string(static_cast<int>(result)) +
                     ", focusedInput=" + std::to_string(result == XR_SUCCESS) +
                     ", observedSessionState=" + std::to_string(static_cast<int>(observed_session_state_)));
    }
    previous = result;
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
    eye_gaze_bindings_ready_ = false;
    if (!eye_gaze_binding_augmentation_disabled_) {
        eye_gaze_action_set_attachment_.SetPrivateActionSet(quadviews_action_set_);
    }
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
    eye_gaze_bindings_ready_ = false;
    eye_gaze_action_set_attachment_.ClearPrivateActionSet();
    has_logged_eye_gaze_focus_active_ = false;
    has_logged_eye_gaze_focus_unavailable_ = false;
    eye_gaze_unavailable_streak_ = 0;
    quadviews_smoothed_focus_yaw_radians_ = 0.0;
    quadviews_smoothed_focus_pitch_radians_ = 0.0;
    quadviews_last_focus_smoothing_wall_time_.reset();
    quadviews_last_valid_gaze_wall_time_.reset();
    quadviews_eye_gaze_loss_started_wall_time_.reset();
    quadviews_eye_gaze_loss_was_locate_failure_ = false;
    quadviews_has_seen_valid_gaze_ = false;
    quadviews_compositor_recovery_.Reset();
    last_eye_gaze_self_sync_time_.reset();
    last_eye_gaze_self_sync_attempt_time_.reset();
    last_eye_gaze_bound_sources_probe_.reset();

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

// Follows a live Tracking Mode change. A session has one
// xrAttachSessionActionSets: a gaze set attached there is kept through Head
// tracked (gaze is not located then) for a switch back to Eye, and a set made
// after an attachment without one could never be attached or bound.
void OpenXrLayer::UpdateEyeGazeResources(QuadViewsTrackingMode previous_mode) {
    const bool emulation_active = IsQuadViewsEmulationActive();
    const bool wants_eye_gaze =
        emulation_active && resolved_settings_.quadviews.tracking_mode == QuadViewsTrackingMode::Eye;
    const bool attachment_completed = eye_gaze_action_set_attachment_.SessionAttachmentCompleted();
    const bool switched_to_eye = previous_mode != QuadViewsTrackingMode::Eye;
    if (wants_eye_gaze && active_session_ != XR_NULL_HANDLE && !eye_gaze_resources_ready_) {
        if (!eye_gaze_extension_enabled_ || !attachment_completed) {
            // Without XR_EXT_eye_gaze_interaction creation cannot succeed, and
            // xrCreateSession or the switch to Eye has said so: a settings
            // reload that keeps Eye tracked neither retries nor logs it again.
            if (eye_gaze_extension_enabled_ || switched_to_eye) {
                const XrResult eye_gaze_result = CreateEyeGazeResources(active_session_);
                if (XR_FAILED(eye_gaze_result)) {
                    if (switched_to_eye) {
                        logger_.Info("Eye gaze resources unavailable after quadviews tracking-mode change; "
                                     "head/static focus remains active.");
                    }
                    DestroyEyeGazeResources();
                }
            }
        } else if (switched_to_eye) {
            logger_.Info("Eye-tracked quadviews focus takes effect after the game restarts: this session attached "
                         "its OpenXR actions without VectorXR's eye-gaze action set; head/static focus remains active.");
        }
    } else if (!wants_eye_gaze && eye_gaze_resources_ready_) {
        if (!emulation_active || !attachment_completed) {
            DestroyEyeGazeResources();
            return;
        }
        // A switch back to Eye reports gaze afresh, as after creation.
        has_logged_eye_gaze_focus_active_ = false;
        has_logged_eye_gaze_focus_unavailable_ = false;
        eye_gaze_unavailable_streak_ = 0;
    }
}

bool OpenXrLayer::LocateEyeGazeFocusOffsets(XrSession session,
                                            XrSpace base_space,
                                            XrTime time,
                                            const QuadViewsSettings& settings,
                                            double* yaw_radians,
                                            double* pitch_radians) {
    if (!yaw_radians || !pitch_radians) {
        return false;
    }

    *yaw_radians = 0.0;
    *pitch_radians = 0.0;
    quadviews_raw_focus_time_ = time;
    quadviews_raw_focus_valid_ = false;

    // Profile selection and bound sources are independent of current activation.
    // Log them with focus/sync state to distinguish missing bindings from input loss.
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

    auto log_eye_gaze_diagnostic = [&](const std::string& reason, bool locate_failure = false) {
        if (settings.tracking_mode == QuadViewsTrackingMode::Eye) {
            const auto unavailable_now = std::chrono::steady_clock::now();
            if (quadviews_has_seen_valid_gaze_ &&
                !quadviews_eye_gaze_loss_started_wall_time_.has_value()) {
                quadviews_eye_gaze_loss_started_wall_time_ = unavailable_now;
            }
            quadviews_compositor_recovery_.NoteUnstable(unavailable_now);
        }
        if (locate_failure && quadviews_eye_gaze_loss_started_wall_time_.has_value()) {
            quadviews_eye_gaze_loss_was_locate_failure_ = true;
        }
        if (settings.tracking_mode == QuadViewsTrackingMode::Eye && !has_logged_eye_gaze_focus_unavailable_) {
            // Debounce: require the gaze to stay unavailable for several frames
            // before declaring it lost, so a single-frame dropout (a blink) does
            // not produce an unavailable/active churn pair in the log.
            ++eye_gaze_unavailable_streak_;
            if (eye_gaze_unavailable_streak_ >= kEyeGazeUnavailableLogThreshold) {
                logger_.Info("Quadviews eye-gaze focus unavailable; using head/static focus offsets. reason=" + reason +
                             ", resourcesReady=" + std::to_string(eye_gaze_resources_ready_) +
                             ", observedSessionState=" + std::to_string(static_cast<int>(observed_session_state_)) +
                             ", syncNotFocused=" + std::to_string(eye_gaze_sync_not_focused_) +
                             ", actionSetAttached=" +
                                 std::to_string(eye_gaze_action_set_attachment_.PrivateActionSetAttached()));
                has_logged_eye_gaze_focus_unavailable_ = true;
                has_logged_eye_gaze_focus_active_ = false;
            }
        }
        const bool should_log_unavailable_debug =
            settings.tracking_mode == QuadViewsTrackingMode::Eye &&
            (pending_eye_gaze_diagnostics_ > 0 ||
             ShouldLogQuadViewsDebugHeartbeat(last_quadviews_eye_gaze_debug_heartbeat_));
        if (should_log_unavailable_debug) {
            const auto now = std::chrono::steady_clock::now();
            if (eye_gaze_bindings_ready_ && eye_gaze_action_set_attachment_.PrivateActionSetAttached() &&
                next_enumerate_bound_sources_for_action_ &&
                (!last_eye_gaze_bound_sources_probe_ || now - *last_eye_gaze_bound_sources_probe_ >= std::chrono::seconds(2))) {
                last_eye_gaze_bound_sources_probe_ = now;
                XrBoundSourcesForActionEnumerateInfo info{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
                info.action = quadviews_eye_gaze_action_;
                uint32_t count = 0;
                const XrResult result = next_enumerate_bound_sources_for_action_(session, &info, 0, &count, nullptr);
                eye_gaze_bound_sources_diagnostic_ = "result=" + std::to_string(static_cast<int>(result)) +
                                                     ", count=" + std::to_string(count);
            }
            const auto sync_age = [&](const auto& timestamp) {
                return timestamp ? std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - *timestamp).count()) : std::string("never");
            };
            const auto sync_result_text = [](const auto& result) {
                return result ? std::to_string(static_cast<int>(*result)) : std::string("never");
            };
            logger_.Debug("Quadviews eye-gaze focus unavailable: " + reason +
                          ", resourcesReady=" + std::to_string(eye_gaze_resources_ready_) +
                          ", actionSetAttached=" +
                              std::to_string(eye_gaze_action_set_attachment_.PrivateActionSetAttached()) +
                          ", eyeInteractionProfile=" + describe_eye_profile() +
                          ", bindingsReady=" + std::to_string(eye_gaze_bindings_ready_) +
                          ", boundSources={" + eye_gaze_bound_sources_diagnostic_ + "}" +
                          ", observedSessionState=" + std::to_string(static_cast<int>(observed_session_state_)) +
                          ", appSyncResult=" + sync_result_text(last_app_action_sync_result_) +
                          ", privateSyncResult=" + sync_result_text(last_eye_gaze_self_sync_result_) +
                          ", appSyncAttemptAgeMs=" + sync_age(last_app_action_sync_attempt_time_) +
                          ", privateSyncAttemptAgeMs=" + sync_age(last_eye_gaze_self_sync_attempt_time_) +
                          ", appFocusedSyncAgeMs=" + sync_age(last_app_action_sync_time_) +
                          ", privateFocusedSyncAgeMs=" + sync_age(last_eye_gaze_self_sync_time_) +
                          ", eyeGazeExtEnabled=" + std::to_string(eye_gaze_extension_enabled_) +
                          ", unavailableStreak=" + std::to_string(eye_gaze_unavailable_streak_));
            if (pending_eye_gaze_diagnostics_ > 0) {
                --pending_eye_gaze_diagnostics_;
            }
        }
    };

    if (settings.tracking_mode != QuadViewsTrackingMode::Eye) {
        quadviews_eye_gaze_loss_started_wall_time_.reset();
        quadviews_eye_gaze_loss_was_locate_failure_ = false;
        quadviews_has_seen_valid_gaze_ = false;
        quadviews_compositor_recovery_.Reset();
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
    if (!eye_gaze_resources_ready_ || !eye_gaze_bindings_ready_ ||
        !eye_gaze_action_set_attachment_.PrivateActionSetAttached() ||
        quadviews_eye_gaze_action_ == XR_NULL_HANDLE || quadviews_eye_gaze_space_ == XR_NULL_HANDLE) {
        log_eye_gaze_diagnostic("eye-gaze action resources are not ready");
        return false;
    }
    if (!next_get_action_state_pose_ || !next_locate_space_ || !next_sync_actions_) {
        log_eye_gaze_diagnostic("required OpenXR action/space functions are unavailable");
        return false;
    }
    // Input is defined only while FOCUSED, but a VISIBLE session is still on
    // screen: SteamVR hands input focus to a floating desktop or overlay window
    // while the game stays in view. Read gaze there without syncing, and use it
    // only while the runtime still reports it live.
    const bool unfocused_visible = observed_session_state_ == XR_SESSION_STATE_VISIBLE;
    if (observed_session_state_ != XR_SESSION_STATE_UNKNOWN && observed_session_state_ != XR_SESSION_STATE_FOCUSED) {
        eye_gaze_inactive_since_.reset();
    }
    if (observed_session_state_ != XR_SESSION_STATE_UNKNOWN &&
        observed_session_state_ != XR_SESSION_STATE_FOCUSED && !unfocused_visible) {
        log_eye_gaze_diagnostic("session is not focused");
        return false;
    }
    if (!unfocused_visible) {
        eye_gaze_unfocused_outcome_.clear();
        eye_gaze_unfocused_last_orientation_.reset();
        eye_gaze_unfocused_identical_frames_ = 0;
    }
    // Logs each change in what the runtime gives while unfocused; the first
    // line of an episode answers whether that runtime keeps gaze live.
    auto note_unfocused_outcome = [&](const char* outcome, XrResult state_result, bool active,
                                      XrResult locate_result, XrSpaceLocationFlags flags) {
        if (eye_gaze_unfocused_outcome_ == outcome) {
            return;
        }
        eye_gaze_unfocused_outcome_ = outcome;
        if (eye_gaze_unfocused_logs_remaining_ == 0) {
            return;
        }
        --eye_gaze_unfocused_logs_remaining_;
        logger_.Info(std::string("Quadviews eye-gaze while the session is visible but not focused: outcome=") +
                     outcome + ", stateResult=" + std::to_string(static_cast<int>(state_result)) +
                     ", active=" + std::to_string(active) +
                     ", locateResult=" + std::to_string(static_cast<int>(locate_result)) +
                     ", locationFlags=" + FormatHex(static_cast<uint64_t>(flags)) +
                     ", appSyncResult=" +
                     (last_app_action_sync_result_ ? std::to_string(static_cast<int>(*last_app_action_sync_result_))
                                                   : std::string("never")));
    };

    // The layer's action set rides along with the app's xrSyncActions; only
    // issue a downstream self-sync when the app has not synced recently.
    const auto sync_check_now = std::chrono::steady_clock::now();
    const auto fresh = [&](const auto& timestamp) {
        return timestamp && sync_check_now - *timestamp < kAppActionSyncFreshWindow;
    };
    const bool app_sync_fresh = fresh(last_app_action_sync_time_);
    const bool self_sync_fresh = fresh(last_eye_gaze_self_sync_time_);
    // Throttle attempts separately from focused successes, including positive
    // XR_SESSION_NOT_FOCUSED. Unknown focus must still allow bounded retries
    // because some applications never poll session events.
    if (unfocused_visible) {
        // A sync here could only return XR_SESSION_NOT_FOCUSED.
    } else if (!fresh(last_app_action_sync_attempt_time_) && !fresh(last_eye_gaze_self_sync_attempt_time_)) {
        const XrActiveActionSet active_action_set{quadviews_action_set_, XR_NULL_PATH};
        XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
        sync_info.countActiveActionSets = 1;
        sync_info.activeActionSets = &active_action_set;
        const XrResult sync_result = next_sync_actions_(session, &sync_info);
        RecordEyeGazeSyncResult(false, sync_result);
        if (sync_result != XR_SUCCESS) {
            log_eye_gaze_diagnostic("self sync did not provide focused input, result=" +
                                    std::to_string(static_cast<int>(sync_result)));
            return false;
        }
    } else if (eye_gaze_sync_not_focused_ || (!app_sync_fresh && !self_sync_fresh)) {
        log_eye_gaze_diagnostic("waiting for a successful focused action sync");
        return false;
    }

    XrActionStateGetInfo action_state_info{XR_TYPE_ACTION_STATE_GET_INFO};
    action_state_info.action = quadviews_eye_gaze_action_;
    XrActionStatePose action_state{XR_TYPE_ACTION_STATE_POSE};
    const XrResult state_result = next_get_action_state_pose_(session, &action_state_info, &action_state);
    if (unfocused_visible && (XR_FAILED(state_result) || !action_state.isActive)) {
        note_unfocused_outcome("inactive", state_result, action_state.isActive, XR_SUCCESS, 0);
        log_eye_gaze_diagnostic("session is not focused");
        return false;
    }
    if (XR_FAILED(state_result) || !action_state.isActive) {
        // Focused, synced and bound, yet inactive: the runtime has wired up
        // eye gaze but its eye tracker sends nothing. Say so once, plainly;
        // the per-frame diagnostics only show the symptom.
        const auto inactive_now = std::chrono::steady_clock::now();
        if (!eye_gaze_inactive_since_) {
            eye_gaze_inactive_since_ = inactive_now;
        } else if (!eye_gaze_no_data_logged_ && inactive_now - *eye_gaze_inactive_since_ >= kEyeGazeNoDataLogDelay) {
            eye_gaze_no_data_logged_ = true;
            logger_.Info("Quadviews eye tracking: the runtime connected eye gaze (interaction profile " +
                         describe_eye_profile() + ") but has reported no gaze for " +
                         std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                             inactive_now - *eye_gaze_inactive_since_).count()) +
                         " s while the game has focus. The headset's eye tracker is not delivering data: check "
                         "that eye tracking is on and calibrated in the headset's software (toggling it off and "
                         "on can restart it). Quadviews uses head focus until gaze arrives.");
        }
        log_eye_gaze_diagnostic("action state is unavailable, result=" +
                                FormatHex(static_cast<uint64_t>(state_result)) +
                                ", active=" + std::to_string(action_state.isActive));
        return false;
    }
    eye_gaze_inactive_since_.reset();

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
    if (unfocused_visible) {
        const bool located = XR_SUCCEEDED(locate_result) &&
                             (gaze_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        const XrQuaternionf& q = gaze_location.pose.orientation;
        const auto& last = eye_gaze_unfocused_last_orientation_;
        const bool identical = located && last && last->x == q.x && last->y == q.y && last->z == q.z &&
                               last->w == q.w;
        eye_gaze_unfocused_identical_frames_ = identical ? eye_gaze_unfocused_identical_frames_ + 1 : 0;
        eye_gaze_unfocused_last_orientation_ = located ? std::optional<XrQuaternionf>(q) : std::nullopt;
        const bool frozen = eye_gaze_unfocused_identical_frames_ >= kEyeGazeUnfocusedFrozenFrames;
        note_unfocused_outcome(!located ? "unlocated" : frozen ? "frozen" : "live", state_result,
                               action_state.isActive, locate_result, gaze_location.locationFlags);
        if (!located || frozen) {
            log_eye_gaze_diagnostic("session is not focused");
            return false;
        }
    }
    if (XR_FAILED(locate_result) ||
        (gaze_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        log_eye_gaze_diagnostic("gaze space locate failed, result=" +
                                FormatHex(static_cast<uint64_t>(locate_result)) +
                                ", flags=" + FormatHex(static_cast<uint64_t>(gaze_location.locationFlags)), true);
        return false;
    }

    const XrQuaternionf gaze_orientation = gaze_location.pose.orientation;
    const GazeRayAngles gaze_ray_angles = ExtractGazeRayAngles(gaze_orientation, settings.eye_tracking_correction == "flip-z");
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
    quadviews_raw_focus_yaw_radians_ = target_yaw;
    quadviews_raw_focus_pitch_radians_ = target_pitch;
    quadviews_raw_focus_valid_ = true;

    const double deadzone_radians = DegreesToRadians(std::max(0.0, settings.gaze_deadzone_degrees));
    if (std::abs(target_yaw) < deadzone_radians) {
        target_yaw = 0.0;
    }
    if (std::abs(target_pitch) < deadzone_radians) {
        target_pitch = 0.0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (quadviews_eye_gaze_loss_started_wall_time_.has_value()) {
        const auto gaze_loss_duration = now - *quadviews_eye_gaze_loss_started_wall_time_;
        if (quadviews_has_seen_valid_gaze_ && quadviews_eye_gaze_loss_was_locate_failure_ &&
            gaze_loss_duration >= std::chrono::seconds(2)) {
            const auto gaze_loss_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(gaze_loss_duration).count();
            const std::chrono::milliseconds recovery_delay =
                QuadViewsRecoveryStabilizationDelay(runtime_name_);
            quadviews_compositor_recovery_.Schedule(now, recovery_delay);
            const std::string recovery_delay_note = recovery_delay.count() > 0
                ? " after " + std::to_string(recovery_delay.count()) + " ms of runtime stabilization"
                : "";
            logger_.Info("Quadviews eye-gaze tracking recovered after " + std::to_string(gaze_loss_ms) +
                         " ms; scheduling compositor output target recycle" + recovery_delay_note + ".");
        }
        quadviews_eye_gaze_loss_started_wall_time_.reset();
        quadviews_eye_gaze_loss_was_locate_failure_ = false;
    }
    quadviews_has_seen_valid_gaze_ = true;
    quadviews_last_valid_gaze_wall_time_ = now;
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
                      ", hemisphereCorrected=" + std::to_string(gaze_ray_angles.hemisphere_corrected) +
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
                     ", hemisphereCorrected=" + std::to_string(gaze_ray_angles.hemisphere_corrected) +
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
        logged_pivot_space_conversions_.clear();
        failed_pivot_space_conversions_.clear();
        cached_depth_submission_geometry_.clear();
        cached_quadviews_frames_.Clear();
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
    logged_pivot_space_conversions_.clear();
    failed_pivot_space_conversions_.clear();
    cached_depth_submission_geometry_.clear();
    cached_quadviews_frames_.Clear();
}

XrResult OpenXrLayer::CreateVarjoNativeFoveationResources(XrSession session) {
    if (varjo_native_view_space_ != XR_NULL_HANDLE &&
        varjo_native_combined_eye_space_ != XR_NULL_HANDLE) {
        return XR_SUCCESS;
    }
    if (varjo_native_foveation_resources_attempted_) {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    varjo_native_foveation_resources_attempted_ = true;

    if (!next_create_reference_space_ || !next_destroy_space_ || !next_locate_space_) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrReferenceSpaceCreateInfo create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create_info.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    create_info.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrResult result = next_create_reference_space_(session, &create_info, &varjo_native_view_space_);
    if (XR_FAILED(result)) {
        varjo_native_view_space_ = XR_NULL_HANDLE;
        logger_.Info("Failed to create the native Varjo VIEW space used as the rendering-gaze base. result=" +
                     FormatHex(static_cast<uint64_t>(result)));
        return result;
    }

    create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
    result = next_create_reference_space_(session, &create_info, &varjo_native_combined_eye_space_);
    if (XR_FAILED(result)) {
        varjo_native_combined_eye_space_ = XR_NULL_HANDLE;
        const XrResult destroy_result = next_destroy_space_(varjo_native_view_space_);
        varjo_native_view_space_ = XR_NULL_HANDLE;
        logger_.Info("Failed to create XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO. createResult=" +
                     FormatHex(static_cast<uint64_t>(result)) +
                     ", viewCleanupResult=" + FormatHex(static_cast<uint64_t>(destroy_result)));
        return result;
    }

    logger_.Info("Native Varjo rendering-gaze resources ready: viewSpace=" +
                 FormatHex(reinterpret_cast<uintptr_t>(varjo_native_view_space_)) +
                 ", combinedEyeSpace=" +
                 FormatHex(reinterpret_cast<uintptr_t>(varjo_native_combined_eye_space_)) +
                 ". Foveated rendering will be activated only while orientation tracking is reported.");
    return XR_SUCCESS;
}

void OpenXrLayer::DestroyVarjoNativeFoveationResources() {
    const XrSpace combined_eye_space = varjo_native_combined_eye_space_;
    const XrSpace view_space = varjo_native_view_space_;
    varjo_native_combined_eye_space_ = XR_NULL_HANDLE;
    varjo_native_view_space_ = XR_NULL_HANDLE;
    varjo_native_rendering_gaze_tracked_ = false;
    has_logged_varjo_native_rendering_gaze_active_ = false;
    has_logged_varjo_native_rendering_gaze_unavailable_ = false;
    varjo_native_foveation_resources_attempted_ = false;
    varjo_native_rendering_gaze_transition_logs_remaining_ = 8;

    if (!next_destroy_space_) {
        return;
    }
    if (combined_eye_space != XR_NULL_HANDLE) {
        const XrResult result = next_destroy_space_(combined_eye_space);
        if (XR_FAILED(result)) {
            logger_.Info("Failed to destroy native Varjo COMBINED_EYE space. result=" +
                         FormatHex(static_cast<uint64_t>(result)));
        }
    }
    if (view_space != XR_NULL_HANDLE) {
        const XrResult result = next_destroy_space_(view_space);
        if (XR_FAILED(result)) {
            logger_.Info("Failed to destroy native Varjo VIEW space. result=" +
                         FormatHex(static_cast<uint64_t>(result)));
        }
    }
}

bool OpenXrLayer::LocateVarjoRenderingGaze(XrTime display_time,
                                           XrResult* locate_result,
                                           XrSpaceLocationFlags* location_flags) {
    XrSpace combined_eye_space = XR_NULL_HANDLE;
    XrSpace view_space = XR_NULL_HANDLE;
    {
        std::scoped_lock lock(mutex_);
        combined_eye_space = varjo_native_combined_eye_space_;
        view_space = varjo_native_view_space_;
    }

    XrResult result = XR_ERROR_HANDLE_INVALID;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (!next_locate_space_) {
        result = XR_ERROR_FUNCTION_UNSUPPORTED;
    } else if (combined_eye_space != XR_NULL_HANDLE && view_space != XR_NULL_HANDLE) {
        result = next_locate_space_(combined_eye_space, view_space, display_time, &location);
    }
    const bool tracked = XR_SUCCEEDED(result) &&
                         (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;

    if (locate_result) {
        *locate_result = result;
    }
    if (location_flags) {
        *location_flags = location.locationFlags;
    }

    std::scoped_lock lock(mutex_);
    const bool was_tracked = varjo_native_rendering_gaze_tracked_;
    varjo_native_rendering_gaze_tracked_ = tracked;
    if (tracked && (!was_tracked || !has_logged_varjo_native_rendering_gaze_active_) &&
        varjo_native_rendering_gaze_transition_logs_remaining_ > 0) {
        logger_.Info("Native Varjo rendering gaze is tracked; gaze-driven focus placement is active. displayTime=" +
                     std::to_string(display_time) +
                     ", locationFlags=" + FormatHex(static_cast<uint64_t>(location.locationFlags)));
        has_logged_varjo_native_rendering_gaze_active_ = true;
        --varjo_native_rendering_gaze_transition_logs_remaining_;
        has_logged_varjo_native_rendering_gaze_unavailable_ = false;
    } else if (!tracked && (was_tracked || !has_logged_varjo_native_rendering_gaze_unavailable_) &&
               varjo_native_rendering_gaze_transition_logs_remaining_ > 0) {
        logger_.Info("Native Varjo rendering gaze is unavailable; requesting the runtime's fixed-center focus "
                     "fallback. displayTime=" + std::to_string(display_time) +
                     ", locateResult=" + FormatHex(static_cast<uint64_t>(result)) +
                     ", locationFlags=" + FormatHex(static_cast<uint64_t>(location.locationFlags)));
        has_logged_varjo_native_rendering_gaze_unavailable_ = true;
        has_logged_varjo_native_rendering_gaze_active_ = false;
        --varjo_native_rendering_gaze_transition_logs_remaining_;
    }
    return tracked;
}

bool OpenXrLayer::CacheEyeOffsetsFromLocatedViews(XrViewConfigurationType view_configuration_type,
                                                   XrTime display_time,
                                                   const XrPosef& runtime_view_pose,
                                                   std::span<const XrView> located_views,
                                                   uint32_t view_count) {
    if (view_count == 0 || located_views.size() < view_count) {
        return false;
    }

    const XrPosef inverse_view_pose = InvertPose(runtime_view_pose);
    cached_eye_offset_poses_.resize(view_count);
    for (uint32_t i = 0; i < view_count; ++i) {
        cached_eye_offset_poses_[i] = MultiplyPoses(located_views[i].pose, inverse_view_pose);
    }
    cached_eye_offsets_view_configuration_ = view_configuration_type;
    cached_eye_offsets_display_time_ = display_time;

    pivot_diagnostic_.has_eye_offsets = true;
    pivot_diagnostic_.eye_offsets_time = display_time;
    pivot_diagnostic_.eye_offsets_view_configuration = view_configuration_type;
    pivot_diagnostic_.eye_offset_count =
        std::min<uint32_t>(view_count, static_cast<uint32_t>(pivot_diagnostic_.eye_offsets.size()));
    for (uint32_t i = 0; i < pivot_diagnostic_.eye_offset_count; ++i) {
        pivot_diagnostic_.eye_offsets[i] = cached_eye_offset_poses_[i];
    }
    return true;
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

    std::vector<XrView>& eye_views = eye_offset_views_scratch_;
    eye_views.assign(view_count, XrView{XR_TYPE_VIEW});
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

bool OpenXrLayer::CachePivotPoseDelta(XrTime time,
                                      XrSpace source_space,
                                      const XrPosef& source_pose_delta,
                                      const XrPosef& view_pose_in_source) {
    if (time == 0 || source_space == XR_NULL_HANDLE) {
        return false;
    }

    PivotPoseDeltaFrame frame;
    frame.source_space = source_space;
    // The runtime has already given us VIEW-in-source for the Pivot drive.
    // Its inverse is source-in-VIEW, so this conjugation produces a durable
    // frame-canonical delta without another runtime locate.
    frame.canonical_view_pose_delta =
        ReexpressPoseDelta(source_pose_delta, InvertPose(view_pose_in_source));
    frame.space_pose_deltas[0] = {source_space, source_pose_delta};
    frame.space_pose_delta_count = 1;
    if (!pivot_pose_delta_spare_.empty() && !cached_pivot_pose_deltas_.contains(time)) {
        // One frame per Pivot drive: reuse a pruned frame's node instead of
        // allocating one under mutex_.
        pivot_pose_delta_spare_.key() = time;
        pivot_pose_delta_spare_.mapped() = frame;
        cached_pivot_pose_deltas_.insert(std::move(pivot_pose_delta_spare_));
        while (cached_pivot_pose_deltas_.size() > kPivotPoseDeltaMaxEntries) {
            cached_pivot_pose_deltas_.erase(cached_pivot_pose_deltas_.begin());
        }
        return true;
    }
    return CachePivotPoseDeltaValue(cached_pivot_pose_deltas_, time, frame);
}

bool OpenXrLayer::FindPivotPoseDeltaForSpace(const PivotPoseDeltaFrame& frame,
                                             XrSpace space,
                                             XrPosef* pose_delta) const {
    if (!pose_delta || space == XR_NULL_HANDLE) {
        return false;
    }
    for (std::size_t index = 0; index < frame.space_pose_delta_count; ++index) {
        if (frame.space_pose_deltas[index].space == space) {
            *pose_delta = frame.space_pose_deltas[index].pose_delta;
            return true;
        }
    }
    return false;
}

void OpenXrLayer::CachePivotPoseDeltaForSpace(PivotPoseDeltaFrame& frame,
                                              XrSpace space,
                                              const XrPosef& pose_delta) const {
    if (space == XR_NULL_HANDLE) {
        return;
    }
    for (std::size_t index = 0; index < frame.space_pose_delta_count; ++index) {
        if (frame.space_pose_deltas[index].space == space) {
            frame.space_pose_deltas[index].pose_delta = pose_delta;
            return;
        }
    }
    if (frame.space_pose_delta_count < frame.space_pose_deltas.size()) {
        frame.space_pose_deltas[frame.space_pose_delta_count++] = {space, pose_delta};
    }
}

void OpenXrLayer::PrunePivotPoseDeltas(XrTime time) {
    const auto keep_from = cached_pivot_pose_deltas_.upper_bound(time);
    if (pivot_pose_delta_spare_.empty() && keep_from != cached_pivot_pose_deltas_.begin()) {
        pivot_pose_delta_spare_ = cached_pivot_pose_deltas_.extract(std::prev(keep_from));
    }
    PrunePivotPoseDeltaValues(cached_pivot_pose_deltas_, time);
}

std::string OpenXrLayer::DescribeSpace(XrSpace space) const {
    std::string label = "UNKNOWN";
    if (space == XR_NULL_HANDLE) {
        return "NULL";
    }
    if (space == internal_view_space_) {
        label = "VIEW(internal)";
    } else if (space == internal_local_space_) {
        label = "LOCAL(internal)";
    } else if (space == internal_stage_space_) {
        label = "STAGE(internal)";
    } else if (tracked_view_spaces_.contains(space)) {
        label = "VIEW";
    } else if (tracked_local_spaces_.contains(space)) {
        label = "LOCAL";
    } else if (tracked_stage_spaces_.contains(space)) {
        label = "STAGE";
    }
    return label + "(" + FormatHex(reinterpret_cast<uintptr_t>(space)) + ")";
}

TrackedSpaceKind OpenXrLayer::ClassifySpace(XrSpace space) const {
    if (space == XR_NULL_HANDLE) {
        return TrackedSpaceKind::kOther;
    }
    if (space == internal_view_space_ || tracked_view_spaces_.contains(space)) {
        return TrackedSpaceKind::kView;
    }
    if (space == internal_local_space_ || tracked_local_spaces_.contains(space)) {
        return TrackedSpaceKind::kLocal;
    }
    if (space == internal_stage_space_ || tracked_stage_spaces_.contains(space)) {
        return TrackedSpaceKind::kStage;
    }
    return TrackedSpaceKind::kOther;
}

void OpenXrLayer::CacheDepthSubmissionGeometry(
    XrTime time,
    XrSpace space,
    XrViewConfigurationType view_configuration_type,
    std::span<const ViewAdjustmentData> native_views,
    std::span<const ViewAdjustmentData> render_views) {
    if (time == 0 || native_views.empty() || native_views.size() != render_views.size()) {
        return;
    }

    // This runs for every located frame while depth is anchored. A frame
    // reuses the node, and the geometry's storage, of one that was pruned.
    auto frame = cached_depth_submission_geometry_.find(time);
    if (frame == cached_depth_submission_geometry_.end()) {
        if (depth_submission_geometry_spare_.empty()) {
            frame = cached_depth_submission_geometry_.try_emplace(time).first;
        } else {
            depth_submission_geometry_spare_.key() = time;
            std::vector<DepthSubmissionGeometry>& reused = depth_submission_geometry_spare_.mapped();
            reused.resize(std::min<std::size_t>(reused.size(), 1));
            if (!reused.empty()) {
                reused.front().space = space;
                reused.front().view_configuration_type = view_configuration_type;
            }
            frame = cached_depth_submission_geometry_.insert(std::move(depth_submission_geometry_spare_)).position;
        }
    }
    std::vector<DepthSubmissionGeometry>& frame_geometry = frame->second;
    auto existing = std::find_if(
        frame_geometry.begin(), frame_geometry.end(),
        [space, view_configuration_type](const DepthSubmissionGeometry& candidate) {
            return candidate.space == space &&
                   candidate.view_configuration_type == view_configuration_type;
        });
    if (existing == frame_geometry.end()) {
        existing = frame_geometry.emplace(frame_geometry.end());
        existing->space = space;
        existing->view_configuration_type = view_configuration_type;
    }
    DepthSubmissionGeometry& geometry = *existing;
    geometry.native_poses.resize(native_views.size());
    geometry.render_poses.resize(render_views.size());
    geometry.native_fovs.resize(native_views.size());
    geometry.render_fovs.resize(render_views.size());
    const auto copy_view = [](const ViewAdjustmentData& source, XrPosef* pose, XrFovf* fov) {
        pose->orientation = {
            static_cast<float>(source.orientation.x),
            static_cast<float>(source.orientation.y),
            static_cast<float>(source.orientation.z),
            static_cast<float>(source.orientation.w),
        };
        pose->position = {
            static_cast<float>(source.position.x),
            static_cast<float>(source.position.y),
            static_cast<float>(source.position.z),
        };
        *fov = {
            static_cast<float>(source.fov.angle_left),
            static_cast<float>(source.fov.angle_right),
            static_cast<float>(source.fov.angle_up),
            static_cast<float>(source.fov.angle_down),
        };
    };
    for (size_t i = 0; i < native_views.size(); ++i) {
        copy_view(native_views[i], &geometry.native_poses[i], &geometry.native_fovs[i]);
        copy_view(render_views[i], &geometry.render_poses[i], &geometry.render_fovs[i]);
    }
    while (cached_depth_submission_geometry_.size() > kMaxCachedDepthSubmissionFrames) {
        cached_depth_submission_geometry_.erase(cached_depth_submission_geometry_.begin());
    }
}

bool OpenXrLayer::FindDepthSubmissionGeometry(
    XrTime time,
    XrSpace space,
    XrViewConfigurationType view_configuration_type,
    uint32_t view_count,
    const DepthSubmissionGeometry** geometry,
    XrTime* matched_time) const {
    if (!geometry || !matched_time) {
        return false;
    }
    *geometry = nullptr;
    *matched_time = 0;
    if (cached_depth_submission_geometry_.empty()) {
        return false;
    }

    auto best = cached_depth_submission_geometry_.find(time);
    if (best == cached_depth_submission_geometry_.end()) {
        const auto upper = cached_depth_submission_geometry_.lower_bound(time);
        if (upper == cached_depth_submission_geometry_.begin()) {
            best = upper;
        } else if (upper == cached_depth_submission_geometry_.end()) {
            best = std::prev(upper);
        } else {
            const auto lower = std::prev(upper);
            best = (time - lower->first <= upper->first - time) ? lower : upper;
        }
    }

    const XrTime match_delta = best->first > time ? best->first - time : time - best->first;
    *matched_time = best->first;
    if (match_delta > kMaxDepthSubmissionMatchWindow) {
        return false;
    }

    const auto matching_geometry = std::find_if(
        best->second.begin(), best->second.end(),
        [space, view_configuration_type, view_count](const DepthSubmissionGeometry& candidate) {
            return candidate.space == space &&
                   candidate.view_configuration_type == view_configuration_type &&
                   candidate.native_poses.size() == view_count &&
                   candidate.render_poses.size() == view_count &&
                   candidate.native_fovs.size() == view_count &&
                   candidate.render_fovs.size() == view_count;
        });
    if (matching_geometry == best->second.end()) {
        return false;
    }

    *geometry = &*matching_geometry;
    return true;
}

void OpenXrLayer::PruneDepthSubmissionGeometry(XrTime time) {
    const auto keep_from = cached_depth_submission_geometry_.upper_bound(time);
    if (keep_from == cached_depth_submission_geometry_.begin()) {
        return;
    }
    if (depth_submission_geometry_spare_.empty()) {
        depth_submission_geometry_spare_ = cached_depth_submission_geometry_.extract(std::prev(keep_from));
    }
    cached_depth_submission_geometry_.erase(cached_depth_submission_geometry_.begin(), keep_from);
}

uint32_t OpenXrLayer::RestoreDepthSubmissionGeometry(
    std::span<XrCompositionLayerProjectionView> views,
    uint32_t first_view,
    const DepthSubmissionGeometry& geometry,
    const XrPosef& reverse_pose_delta,
    bool has_reverse_pose_delta) const {
    uint32_t restored_views = 0;
    for (size_t i = 0; i < views.size(); ++i) {
        const size_t geometry_index = static_cast<size_t>(first_view) + i;
        if (geometry_index >= geometry.native_poses.size() ||
            geometry_index >= geometry.render_poses.size() ||
            geometry_index >= geometry.native_fovs.size() ||
            geometry_index >= geometry.render_fovs.size()) {
            continue;
        }

        XrPosef native_pose = geometry.native_poses[geometry_index];
        XrPosef render_pose = geometry.render_poses[geometry_index];
        if (has_reverse_pose_delta) {
            native_pose = MultiplyPoses(native_pose, reverse_pose_delta);
            render_pose = MultiplyPoses(render_pose, reverse_pose_delta);
        }

        const XrFovf& native_fov = geometry.native_fovs[geometry_index];
        const XrFovf& render_fov = geometry.render_fovs[geometry_index];
        ViewAdjustmentData submitted_view{
            {
                views[i].pose.position.x,
                views[i].pose.position.y,
                views[i].pose.position.z,
            },
            {
                views[i].fov.angleLeft,
                views[i].fov.angleRight,
                views[i].fov.angleUp,
                views[i].fov.angleDown,
            },
        };
        const ViewAdjustmentData native_view{
            {native_pose.position.x, native_pose.position.y, native_pose.position.z},
            {native_fov.angleLeft, native_fov.angleRight, native_fov.angleUp, native_fov.angleDown},
        };
        const ViewAdjustmentData render_view{
            {render_pose.position.x, render_pose.position.y, render_pose.position.z},
            {render_fov.angleLeft, render_fov.angleRight, render_fov.angleUp, render_fov.angleDown},
        };
        RestoreDepthSubmissionView(submitted_view, native_view, render_view);
        views[i].pose.position = {
            static_cast<float>(submitted_view.position.x),
            static_cast<float>(submitted_view.position.y),
            static_cast<float>(submitted_view.position.z),
        };
        views[i].fov = {
            static_cast<float>(submitted_view.fov.angle_left),
            static_cast<float>(submitted_view.fov.angle_right),
            static_cast<float>(submitted_view.fov.angle_up),
            static_cast<float>(submitted_view.fov.angle_down),
        };
        ++restored_views;
    }
    return restored_views;
}

void OpenXrLayer::CacheQuadViewsFrame(XrTime time,
                                      std::span<const XrView> views,
                                      const QuadViewsGazeDiagnostic& gaze) {
    if (time == 0 || views.size() < 4) {
        return;
    }

    QuadViewsFrameState frame;
    for (uint32_t i = 0; i < frame.fovs.size(); ++i) {
        frame.fovs[i] = views[i].fov;
    }
    frame.gaze = gaze;
    cached_quadviews_frames_.Store(time, frame, kMaxCachedQuadViewsFovFrames);
}

bool OpenXrLayer::FindQuadViewsFrame(XrTime time,
                                     QuadViewsFrameState* frame,
                                     XrTime* matched_time) const {
    if (!frame || !matched_time) {
        return false;
    }

    std::int64_t cache_matched_time = 0;
    const bool found = cached_quadviews_frames_.FindNearest(
        time, kMaxQuadViewsFovMatchWindow, frame, &cache_matched_time);
    *matched_time = cache_matched_time;
    return found;
}

void OpenXrLayer::PruneQuadViewsFrames(XrTime time) {
    cached_quadviews_frames_.PruneThrough(time);
}

bool OpenXrLayer::IsTrackedViewSpace(XrSpace space) const {
    return space != XR_NULL_HANDLE && tracked_view_spaces_.contains(space);
}

void OpenXrLayer::RecordVarjoNativeLocateDiagnostics(const XrViewLocateInfo* view_locate_info,
                                                     bool vector_request_injected,
                                                     bool rendering_gaze_queried,
                                                     XrResult rendering_gaze_result,
                                                     XrSpaceLocationFlags rendering_gaze_flags,
                                                     const XrViewState* view_state,
                                                     XrResult result,
                                                     uint32_t view_capacity_input,
                                                     const uint32_t* view_count_output,
                                                     const XrView* views) {
    const auto* foveated_request = view_locate_info
                                        ? reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(
                                              FindStructInChain(view_locate_info->next,
                                                                XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO))
                                        : nullptr;
    const bool request_present = foveated_request != nullptr;
    const bool request_active = request_present && foveated_request->foveatedRenderingActive == XR_TRUE;
    const size_t request_state = request_present ? (request_active ? 2 : 1) : 0;
    const uint32_t returned_views =
        view_count_output ? std::min(view_capacity_input, *view_count_output) : 0;

    std::scoped_lock lock(mutex_);
    VarjoNativeFoveationDiagnosticState& diagnostic = varjo_native_foveation_diagnostic_;
    ++diagnostic.locate_calls;
    ++diagnostic.request_state_counts[request_state];
    if (vector_request_injected) {
        ++diagnostic.vector_injected_locate_requests;
    }
    if (rendering_gaze_queried) {
        ++diagnostic.rendering_gaze_queries;
        if (XR_FAILED(rendering_gaze_result)) {
            ++diagnostic.rendering_gaze_failed_queries;
        }
        if ((rendering_gaze_flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0) {
            ++diagnostic.rendering_gaze_tracked_queries;
        } else {
            ++diagnostic.rendering_gaze_untracked_queries;
        }
    }

    const uint8_t request_state_bit = static_cast<uint8_t>(uint8_t{1} << request_state);
    if ((diagnostic.logged_request_state_mask & request_state_bit) == 0) {
        diagnostic.logged_request_state_mask |= request_state_bit;
        std::ostringstream stream;
        stream << "Native Varjo foveated locate contract: locateCall=" << diagnostic.locate_calls
               << ", requestChainPresent=" << (request_present ? 1 : 0)
               << ", vectorRequestInjected=" << (vector_request_injected ? 1 : 0)
               << ", foveatedRenderingActive=" << (request_active ? 1 : 0)
               << ", displayTime=" << (view_locate_info ? view_locate_info->displayTime : 0)
               << ", renderingGazeQueried=" << (rendering_gaze_queried ? 1 : 0)
               << ", renderingGazeResult="
               << FormatHex(static_cast<uint64_t>(rendering_gaze_result))
               << ", renderingGazeFlags="
               << FormatHex(static_cast<uint64_t>(rendering_gaze_flags))
               << ", result=" << FormatHex(static_cast<uint64_t>(result))
               << ", appCapacity=" << view_capacity_input
               << ", returnedViews=" << returned_views
               << ", viewStateFlags="
               << FormatHex(view_state ? static_cast<uint64_t>(view_state->viewStateFlags) : 0);
        if (views && returned_views >= 4) {
            stream << ", focusView2FovRadians=" << FormatFov(views[2].fov)
                   << ", focusView3FovRadians=" << FormatFov(views[3].fov);
        }
        stream << ". Counts for every native locate call and exact focus-FOV ranges are included in the "
                  "periodic/session summary.";
        logger_.Info(stream.str());
    }

    if (XR_FAILED(result) || !views || returned_views < 4) {
        return;
    }
    ++diagnostic.successful_quad_locates;
    if (view_locate_info) {
        const size_t slot = diagnostic.located_focus_next++ % diagnostic.located_focus_times.size();
        diagnostic.located_focus_times[slot] = view_locate_info->displayTime;
        diagnostic.located_focus_fovs[slot] = {views[2].fov, views[3].fov};
    }

    std::array<double, 2> focus_yaw{};
    std::array<double, 2> focus_pitch{};
    std::array<double, 2> focus_width{};
    std::array<double, 2> focus_height{};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const XrFovf& fov = views[eye + 2].fov;
        focus_yaw[eye] = (static_cast<double>(fov.angleLeft) + fov.angleRight) * 0.5;
        focus_pitch[eye] = (static_cast<double>(fov.angleUp) + fov.angleDown) * 0.5;
        focus_width[eye] = static_cast<double>(fov.angleRight) - fov.angleLeft;
        focus_height[eye] = static_cast<double>(fov.angleUp) - fov.angleDown;
    }

    if (!diagnostic.focus_ranges_initialized) {
        diagnostic.focus_ranges_initialized = true;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            diagnostic.initial_focus_fovs[eye] = views[eye + 2].fov;
            diagnostic.min_focus_yaw[eye] = diagnostic.max_focus_yaw[eye] = focus_yaw[eye];
            diagnostic.min_focus_pitch[eye] = diagnostic.max_focus_pitch[eye] = focus_pitch[eye];
            diagnostic.min_focus_width[eye] = diagnostic.max_focus_width[eye] = focus_width[eye];
            diagnostic.min_focus_height[eye] = diagnostic.max_focus_height[eye] = focus_height[eye];
        }
    } else {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            diagnostic.min_focus_yaw[eye] = std::min(diagnostic.min_focus_yaw[eye], focus_yaw[eye]);
            diagnostic.max_focus_yaw[eye] = std::max(diagnostic.max_focus_yaw[eye], focus_yaw[eye]);
            diagnostic.min_focus_pitch[eye] = std::min(diagnostic.min_focus_pitch[eye], focus_pitch[eye]);
            diagnostic.max_focus_pitch[eye] = std::max(diagnostic.max_focus_pitch[eye], focus_pitch[eye]);
            diagnostic.min_focus_width[eye] = std::min(diagnostic.min_focus_width[eye], focus_width[eye]);
            diagnostic.max_focus_width[eye] = std::max(diagnostic.max_focus_width[eye], focus_width[eye]);
            diagnostic.min_focus_height[eye] = std::min(diagnostic.min_focus_height[eye], focus_height[eye]);
            diagnostic.max_focus_height[eye] = std::max(diagnostic.max_focus_height[eye], focus_height[eye]);
        }
    }

    constexpr double kFocusMotionThresholdRadians = 0.05 * 3.14159265358979323846 / 180.0;
    bool focus_fov_moved = false;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        focus_fov_moved =
            focus_fov_moved ||
            diagnostic.max_focus_yaw[eye] - diagnostic.min_focus_yaw[eye] >= kFocusMotionThresholdRadians ||
            diagnostic.max_focus_pitch[eye] - diagnostic.min_focus_pitch[eye] >= kFocusMotionThresholdRadians ||
            diagnostic.max_focus_width[eye] - diagnostic.min_focus_width[eye] >= kFocusMotionThresholdRadians ||
            diagnostic.max_focus_height[eye] - diagnostic.min_focus_height[eye] >= kFocusMotionThresholdRadians;
    }
    if (focus_fov_moved && !diagnostic.focus_motion_logged) {
        diagnostic.focus_motion_logged = true;
        std::ostringstream stream;
        stream << "Native Varjo focus FOV movement observed after " << diagnostic.successful_quad_locates
               << " successful quad locates: requestChainPresent=" << (request_present ? 1 : 0)
               << ", vectorRequestInjected=" << (vector_request_injected ? 1 : 0)
               << ", foveatedRenderingActive=" << (request_active ? 1 : 0)
               << ", initialFocusView2=" << FormatFov(diagnostic.initial_focus_fovs[0])
               << ", currentFocusView2=" << FormatFov(views[2].fov)
               << ", initialFocusView3=" << FormatFov(diagnostic.initial_focus_fovs[1])
               << ", currentFocusView3=" << FormatFov(views[3].fov)
               << ". This confirms the runtime-returned native focus geometry is changing during the session.";
        logger_.Info(stream.str());
    }

    const auto now = std::chrono::steady_clock::now();
    if (!diagnostic.last_summary_wall_time.has_value()) {
        diagnostic.last_summary_wall_time = now;
    } else if (now - *diagnostic.last_summary_wall_time >= std::chrono::seconds(10)) {
        diagnostic.last_summary_wall_time = now;
        if (logger_.IsDebugEnabled()) {
            LogVarjoNativeFoveationSummaryLocked("periodic-10s", false);
        }
    }
}

void OpenXrLayer::NoteVarjoNativeLocatedFocusLocked(XrTime display_time, const XrView* views) {
    // The focus FOVs the application was given for this display time: the
    // runtime's, after LocateViews' own adjustments (Depth's convergence
    // shifts each eye's projection). The application submits these, so they,
    // not the runtime's, are what its submitted focus views are checked
    // against; the runtime's stay in the summary's ranges.
    VarjoNativeFoveationDiagnosticState& diagnostic = varjo_native_foveation_diagnostic_;
    const size_t kept = diagnostic.located_focus_times.size();
    if (diagnostic.located_focus_next == 0 || display_time == 0) {
        return;
    }
    const size_t slot = (diagnostic.located_focus_next - 1) % kept;
    if (diagnostic.located_focus_times[slot] == display_time) {
        diagnostic.located_focus_fovs[slot] = {views[2].fov, views[3].fov};
    }
}

void OpenXrLayer::NoteVarjoNativeSubmittedFocusLocked(XrTime display_time, const XrCompositionLayerProjectionView* views,
                                                      uint32_t view_count) {
    VarjoNativeFoveationDiagnosticState& diagnostic = varjo_native_foveation_diagnostic_;
    if (!views || view_count < 4 || diagnostic.successful_quad_locates == 0) {
        return;
    }
    // An application may locate the same display time more than once (the
    // focus FOVs following the gaze in between): any of those locates counts,
    // the newest is the one reported.
    constexpr float kTolerance = 1e-4f; // radians
    const auto differs = [&](const XrFovf& a, const XrFovf& b) {
        return std::abs(a.angleLeft - b.angleLeft) > kTolerance || std::abs(a.angleRight - b.angleRight) > kTolerance ||
               std::abs(a.angleUp - b.angleUp) > kTolerance || std::abs(a.angleDown - b.angleDown) > kTolerance;
    };
    const size_t kept = diagnostic.located_focus_times.size();
    const std::array<XrFovf, 2>* located = nullptr;
    bool matched = false;
    for (size_t back = 1; display_time != 0 && back <= kept; ++back) {
        const size_t slot = (diagnostic.located_focus_next + kept - back) % kept;
        if (diagnostic.located_focus_times[slot] != display_time) {
            continue;
        }
        const auto& fovs = diagnostic.located_focus_fovs[slot];
        located = located ? located : &fovs;
        if (!differs(views[2].fov, fovs[0]) && !differs(views[3].fov, fovs[1])) {
            matched = true;
            break;
        }
    }
    if (!located) {
        ++diagnostic.submitted_focus_unlocated;
        return;
    }
    ++diagnostic.submitted_focus_checked;
    if (matched) {
        return;
    }
    ++diagnostic.submitted_focus_mismatched;
    if (!diagnostic.submitted_focus_mismatch_logged) {
        diagnostic.submitted_focus_mismatch_logged = true;
        logger_.Info("Native Varjo focus views submitted with other FOVs than located for their display time: "
                     "displayTime=" + std::to_string(display_time) + ", submittedFocusView2=" +
                     FormatFov(views[2].fov) + ", locatedFocusView2=" + FormatFov((*located)[0]) +
                     ", submittedFocusView3=" + FormatFov(views[3].fov) + ", locatedFocusView3=" +
                     FormatFov((*located)[1]) + ". The runtime places the focus insets by the FOVs submitted; counts "
                     "are in the foveation summary (submittedFocusFov=checked/mismatched/unlocated).");
    }
}

void OpenXrLayer::LogVarjoNativeFoveationSummaryLocked(std::string_view reason, bool info_level) {
    const VarjoNativeFoveationDiagnosticState& diagnostic = varjo_native_foveation_diagnostic_;
    if (diagnostic.locate_calls == 0) {
        return;
    }

    constexpr double kRadiansToDegrees = 180.0 / 3.14159265358979323846;
    std::ostringstream stream;
    stream << "Native Varjo foveation summary: reason=" << reason
           << ", locateCalls=" << diagnostic.locate_calls
           << ", requestAbsent=" << diagnostic.request_state_counts[0]
           << ", requestPresentInactive=" << diagnostic.request_state_counts[1]
           << ", requestPresentActive=" << diagnostic.request_state_counts[2]
           << ", vectorInjectedLocateRequests=" << diagnostic.vector_injected_locate_requests
           << ", renderingGazeQueries=" << diagnostic.rendering_gaze_queries
           << ", renderingGazeTracked=" << diagnostic.rendering_gaze_tracked_queries
           << ", renderingGazeUntracked=" << diagnostic.rendering_gaze_untracked_queries
           << ", renderingGazeFailed=" << diagnostic.rendering_gaze_failed_queries
           << ", successfulQuadLocates=" << diagnostic.successful_quad_locates
           << ", focusFovObserved=" << (diagnostic.focus_ranges_initialized ? 1 : 0)
           << ", focusFovMotionObserved=" << (diagnostic.focus_motion_logged ? 1 : 0)
           << ", submittedFocusFov=" << diagnostic.submitted_focus_checked << "/"
           << diagnostic.submitted_focus_mismatched << "/" << diagnostic.submitted_focus_unlocated;
    if (diagnostic.focus_ranges_initialized) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            stream << ", focusView" << (eye + 2)
                   << "{initialFovRadians=" << FormatFov(diagnostic.initial_focus_fovs[eye])
                   << ", centerYawRangeDegrees=["
                   << FormatDiagnosticDouble(diagnostic.min_focus_yaw[eye] * kRadiansToDegrees) << ","
                   << FormatDiagnosticDouble(diagnostic.max_focus_yaw[eye] * kRadiansToDegrees)
                   << "], centerPitchRangeDegrees=["
                   << FormatDiagnosticDouble(diagnostic.min_focus_pitch[eye] * kRadiansToDegrees) << ","
                   << FormatDiagnosticDouble(diagnostic.max_focus_pitch[eye] * kRadiansToDegrees)
                   << "], widthRangeDegrees=["
                   << FormatDiagnosticDouble(diagnostic.min_focus_width[eye] * kRadiansToDegrees) << ","
                   << FormatDiagnosticDouble(diagnostic.max_focus_width[eye] * kRadiansToDegrees)
                   << "], heightRangeDegrees=["
                   << FormatDiagnosticDouble(diagnostic.min_focus_height[eye] * kRadiansToDegrees) << ","
                   << FormatDiagnosticDouble(diagnostic.max_focus_height[eye] * kRadiansToDegrees) << "]}";
        }
    }

    if (info_level) {
        logger_.Info(stream.str());
    } else {
        logger_.Debug(stream.str());
    }
}

XrResult OpenXrLayer::LocateRuntimeViews(XrSession session,
                                         const XrViewLocateInfo* view_locate_info,
                                         XrViewState* view_state,
                                         uint32_t view_capacity_input,
                                         uint32_t* view_count_output,
                                         XrView* views,
                                         bool* synthesized_quad_views,
                                         QuadViewsGazeDiagnostic* gaze_diagnostic,
                                         HeadExtension* head_extension) {
    if (synthesized_quad_views) {
        *synthesized_quad_views = false;
    }
    if (gaze_diagnostic) {
        *gaze_diagnostic = {};
    }
    if (head_extension) {
        *head_extension = {};
    }

    bool quadviews_emulation_active = false;
    if (view_locate_info) turbo_trace_.Record("locate", turbo_trace_.NextId(), view_locate_info->displayTime);
    bool varjo_compatible = false;
    bool request_native_foveated_locates = false;
    // The tuning only: the resolved settings' diagnostic binding would make
    // this per-call copy allocate.
    QuadViewsSettings quadviews_settings;
    // Frame Generation's trace (debug logging) records the runtime's answer
    // to the game's stereo locate, with the kind of space it was asked in.
    const bool trace_locate = view_locate_info && framegen_.Trace().Enabled();
    TrackedSpaceKind locate_space_kind = TrackedSpaceKind::kOther;
    HeadPredictionPlan head_plan;
    XrSpace head_world_space = XR_NULL_HANDLE;
    {
        std::scoped_lock lock(mutex_);
        ReloadConfigIfNeeded();
        RefreshResolvedSettings();
        if (trace_locate) {
            locate_space_kind = ClassifySpace(view_locate_info->space);
        }
        if (view_locate_info) {
            head_world_space = HeadPredictionWorldSpace(internal_view_space_, view_locate_info->space);
            if (head_world_space != XR_NULL_HANDLE) {
                head_plan = PlanHeadPredictionLocked(true);
            }
        }
        quadviews_emulation_active = IsQuadViewsEmulationActive();
        varjo_compatible = varjo_compatible_quadviews_active_;
        quadviews_settings = resolved_settings_.quadviews;
        request_native_foveated_locates =
            IsQuadViewsActive() && varjo_foveated_rendering_extension_requested_ &&
            quadviews_settings.tracking_mode == QuadViewsTrackingMode::Eye;
    }

    // Head prediction past the runtime's limit: the runtime's views for a
    // world space, turned on over the part of the display time it does not
    // predict, before Pivot, Depth or quad views see them. The same turn is
    // handed back for the Pivot drive's own locates of this call.
    const auto extend_head = [&](XrView* runtime_views, uint32_t runtime_count) {
        if (!head_plan.enabled || !runtime_views || runtime_count == 0) {
            return;
        }
        if (head_plan.probe) {
            const auto estimate = ProbeRuntimePredictionLimit(head_plan, head_world_space);
            std::scoped_lock lock(mutex_);
            NotePredictionProbeLocked(estimate);
            return;
        }
        if (!head_plan.limited) {
            return;
        }
        const XrTime now = TurboClockNow();
        const HeadExtension extension = ExtendHead(head_plan, head_world_space, view_locate_info->displayTime, now);
        const XrPosef runtime_view0 = runtime_views[0].pose;
        if (extension.applied) {
            for (uint32_t i = 0; i < runtime_count; ++i) {
                runtime_views[i].pose = ToXrPose(ApplyHeadExtension(extension, ToHeadPose(runtime_views[i].pose)));
            }
            if (head_extension) {
                *head_extension = extension;
            }
        }
        // Against "end" (what the game submits): the display time, how far
        // past the runtime's limit it is, the turn (micro-degrees), the
        // runtime's speed (milli-deg/s), the limit, and view 0 as the runtime
        // answered (also for quad views, which have no "loc") and as turned.
        if (trace_locate && now > 0) {
            FrameGenTrace& trace = framegen_.Trace();
            trace.Record("xp", 0, view_locate_info->displayTime, view_locate_info->displayTime - now - head_plan.limit,
                         extension.applied ? std::llround(extension.degrees * 1e6) : 0,
                         std::llround(extension.speed_degrees_per_second * 1e3), head_plan.limit,
                         extension.applied ? 1 : 0);
            trace.RecordPose("xp.r0", 0, runtime_view0);
            trace.RecordPose("xp.v0", 0, runtime_views[0].pose);
        }
        std::scoped_lock lock(mutex_);
        NoteHeadExtensionLocked(extension, view_locate_info->displayTime, now);
    };

    // Varjo compatible mode: forward the app's quad locate untouched — including the
    // Varjo foveated-rendering next chain, which the runtime consumes to place the
    // focus inset by gaze — and return the runtime's real 4 views. No stereo remap,
    // no synthesis. Pivot is still applied downstream in LocateViews.
    // The one exception to transparent forwarding is supplying a missing
    // foveated-rendering request while VectorXR eye tracking is active.
    if (varjo_compatible && view_locate_info &&
        IsQuadViewConfiguration(view_locate_info->viewConfigurationType)) {
        XrViewLocateInfo native_locate_info = *view_locate_info;
        XrViewLocateFoveatedRenderingVARJO injected_foveated_request{
            XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO};
        const XrViewLocateInfo* native_view_locate_info = view_locate_info;
        bool vector_request_injected = false;
        bool rendering_gaze_queried = false;
        XrResult rendering_gaze_result = XR_SUCCESS;
        XrSpaceLocationFlags rendering_gaze_flags = 0;
        if (request_native_foveated_locates &&
            !FindStructInChain(view_locate_info->next, XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO)) {
            rendering_gaze_queried = true;
            const bool rendering_gaze_tracked = LocateVarjoRenderingGaze(
                view_locate_info->displayTime, &rendering_gaze_result, &rendering_gaze_flags);
            injected_foveated_request.next = view_locate_info->next;
            injected_foveated_request.foveatedRenderingActive =
                rendering_gaze_tracked ? XR_TRUE : XR_FALSE;
            native_locate_info.next = &injected_foveated_request;
            native_view_locate_info = &native_locate_info;
            vector_request_injected = true;
        }
        const XrResult native_result = next_locate_views_(
            session, native_view_locate_info, view_state, view_capacity_input, view_count_output, views);
        RecordVarjoNativeLocateDiagnostics(
            native_view_locate_info, vector_request_injected, rendering_gaze_queried, rendering_gaze_result,
            rendering_gaze_flags, view_state, native_result, view_capacity_input,
            view_count_output, views);
        if (XR_SUCCEEDED(native_result) && views && view_count_output) {
            extend_head(views, std::min(*view_count_output, view_capacity_input));
        }
        return native_result;
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

    if (!view_locate_info || !IsQuadViewConfiguration(view_locate_info->viewConfigurationType) ||
        !quadviews_emulation_active) {
        const bool diag = TurboSequencedDebugTick();
        if (diag) {
            logger_.Debug("Turbo-diag: app xrLocateViews starting (no lock held).");
        }
        const XrResult locate_result = next_locate_views_(
            session, downstream_view_locate_info, view_state, view_capacity_input, view_count_output, views);
        if (diag) {
            logger_.Debug("Turbo-diag: app xrLocateViews completed.");
        }
        // The runtime's poses, before Pivot or Depth adjust them (LocateViews):
        // against end.v0 they show whether the game submits what it located.
        // Not for a count query, which locates no view.
        if (trace_locate && XR_SUCCEEDED(locate_result) &&
            !IsQuadViewConfiguration(view_locate_info->viewConfigurationType) && views && view_count_output &&
            view_capacity_input > 0) {
            FrameGenTrace& trace = framegen_.Trace();
            const std::uint32_t located = std::min(*view_count_output, view_capacity_input);
            trace.Record("loc", 0, view_locate_info->displayTime, static_cast<int64_t>(locate_space_kind), located,
                         view_state ? static_cast<int64_t>(view_state->viewStateFlags) : 0);
            if (located > 0) {
                trace.RecordPose("loc.v0", 0, views[0].pose);
            }
            if (located > 1) {
                trace.RecordPose("loc.v1", 0, views[1].pose);
            }
        }
        if (XR_SUCCEEDED(locate_result) && views && view_count_output) {
            extend_head(views, std::min(*view_count_output, view_capacity_input));
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
        void* app_next = view_state->next;
        *view_state = runtime_view_state;
        view_state->next = app_next;
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
    if (views && view_capacity_input >= 4) {
        extend_head(stereo_views.data(), 2);
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
            // Blinks and brief tracker dropouts should not snap the focus inset
            // back to center. Hold the last valid gaze briefly, then ease home
            // if tracking remains unavailable.
            constexpr std::chrono::milliseconds kGazeHoldDuration{500};
            const auto now = std::chrono::steady_clock::now();
            const bool hold_last_gaze = quadviews_last_valid_gaze_wall_time_.has_value() &&
                                        now - *quadviews_last_valid_gaze_wall_time_ <= kGazeHoldDuration;
            if (!hold_last_gaze && quadviews_last_valid_gaze_wall_time_.has_value()) {
                double delta_seconds = 0.0;
                if (quadviews_last_focus_smoothing_wall_time_.has_value()) {
                    delta_seconds = std::chrono::duration<double>(
                                        now - *quadviews_last_focus_smoothing_wall_time_)
                                        .count();
                }
                const double release_blend =
                    ComputeTimeBasedBlend(std::max(0.9, quadviews_settings.gaze_smoothing), delta_seconds);
                quadviews_smoothed_focus_yaw_radians_ *= 1.0 - release_blend;
                quadviews_smoothed_focus_pitch_radians_ *= 1.0 - release_blend;
                if (NearlyZero(quadviews_smoothed_focus_yaw_radians_) &&
                    NearlyZero(quadviews_smoothed_focus_pitch_radians_)) {
                    quadviews_smoothed_focus_yaw_radians_ = 0.0;
                    quadviews_smoothed_focus_pitch_radians_ = 0.0;
                    quadviews_last_valid_gaze_wall_time_.reset();
                }
            }
            quadviews_last_focus_smoothing_wall_time_ = now;
            focus_yaw_radians = quadviews_smoothed_focus_yaw_radians_;
            focus_pitch_radians = quadviews_smoothed_focus_pitch_radians_;
        }
        if (gaze_diagnostic) {
            gaze_diagnostic->valid = has_eye_focus && quadviews_raw_focus_valid_;
            gaze_diagnostic->raw_yaw_radians = quadviews_raw_focus_yaw_radians_;
            gaze_diagnostic->raw_pitch_radians = quadviews_raw_focus_pitch_radians_;
            gaze_diagnostic->smoothed_yaw_radians = focus_yaw_radians;
            gaze_diagnostic->smoothed_pitch_radians = focus_pitch_radians;
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
                                                const QuadViewsSettings& quadviews_settings,
                                                double focus_yaw_radians,
                                                double focus_pitch_radians,
                                                uint32_t view_capacity_input,
                                                uint32_t* view_count_output,
                                                XrView* views) const {
    if (stereo_views.size() < 2 || !views || !view_count_output || view_capacity_input < 4) {
        return false;
    }

    *view_count_output = 4;
    std::array<void*, 4> app_next{};
    for (uint32_t i = 0; i < app_next.size(); ++i) {
        app_next[i] = views[i].next;
    }
    views[0] = stereo_views[0];
    views[1] = stereo_views[1];
    views[2] = stereo_views[0];
    views[3] = stereo_views[1];
    for (uint32_t i = 0; i < app_next.size(); ++i) {
        views[i].next = app_next[i];
    }

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
                                               bool update_smoothing,
                                               const XrSpaceLocation* anchor_space_in_reference) {
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
        pivotxr_translation_anchor_.reset();
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

        // A Quick View temporarily replaces the active motion transform. Keep
        // the underlying activation envelope fixed until its return transition
        // hands off at that same transform, avoiding a reverse-then-forward bounce.
        // A stacked snap keeps applying motion, so its envelope keeps moving.
        const bool returns_to_engaged_motion = pivotxr_quick_view_return_engaged_ &&
            pivotxr_quick_view_return_profile_index_ < resolved_settings_.pivotxr.profiles.size() &&
            resolved_settings_.pivotxr.profiles[pivotxr_quick_view_return_profile_index_].behavior ==
                PivotProfileBehavior::EnhancedMotion;
        const bool suspend_activation_gain =
            !pivotxr_snap_stacked_ && returns_to_engaged_motion && PivotSnapEffectActive();
        pivotxr_activation_gain_ = AdvancePivotActivationGain(
            pivotxr_activation_gain_, pivotxr_engaged_, suspend_activation_gain,
            settings.activation_ramp_seconds, delta_seconds);

    }

    // Fully release once the envelope has closed and the pivot is no longer
    // engaged. While the envelope is still open we keep applying the (eased)
    // pivot so toggling off releases the view smoothly.
    if (!pivotxr_active && pivotxr_activation_gain_ <= kPivotActivationGainEpsilon) {
        if (!pivotxr_origin_) pivotxr_translation_anchor_.reset();
        pivotxr_activation_gain_ = 0.0;
        ResetPivotMotionState();
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

    if ((location->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        clear_extra_outputs();
        return result;
    }

    const XrPosef view_pose = space_is_view ? location->pose : InvertPose(location->pose);
    const XrSpace reference_space = space_is_view ? base_space : space;
    const bool position_valid = (location->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    if (update_smoothing && !pivotxr_reference_changes_.empty()) {
        // A recenter reached this frame: the stored anchor coordinates now
        // name a different physical point, which would orbit the camera.
        std::erase_if(pivotxr_reference_changes_, [&](const PivotReferenceChange& change) {
            if (time < change.change_time) {
                return false;
            }
            if (PivotAnchorAffectedBy(change.type)) {
                pivotxr_translation_anchor_.reset();
                if (pivotxr_snap_stacked_ && pivotxr_snap_reference_valid_) {
                    // The stacked snap's head reference was measured in the
                    // old reference space. Re-measure motion from this pose
                    // and ease the motion already on screen back out.
                    FoldPivotMotionIntoSnap(SelectedQuickViewTransitionSeconds());
                    pivotxr_snap_reference_pending_ = true;
                }
                logger_.Info("PivotXR: re-anchoring the seated position after a runtime recenter.");
            }
            return true;
        });
    }
    if (update_smoothing && position_valid && !pivotxr_translation_anchor_) {
        pivotxr_translation_anchor_ = PivotTranslationAnchor{view_pose.position, reference_space};
    }
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

    // Stacked motion is measured from the head pose on the first drive after
    // stacking starts. Its motion state was zeroed when the request was made,
    // so capturing the reference here changes nothing on screen.
    if (update_smoothing && pivotxr_snap_reference_pending_) {
        pivotxr_snap_reference_yaw_radians_ = current_yaw_radians;
        pivotxr_snap_reference_pitch_radians_ = current_pitch_radians;
        pivotxr_snap_reference_valid_ = true;
        pivotxr_snap_reference_pending_ = false;
    }
    // A released stacked snap measures motion from the seated origin again
    // from this drive on. Record what is on screen before the switch.
    const bool snap_return_handoff = update_smoothing && pivotxr_snap_return_pending_;
    PivotViewOffset motion_before_handoff;
    if (snap_return_handoff) {
        motion_before_handoff = ComposedPivotMotionOffset();
        pivotxr_snap_reference_valid_ = false;
    }
    double motion_yaw_radians = current_yaw_radians;
    double motion_pitch_radians = current_pitch_radians;
    if (pivotxr_snap_stacked_ && pivotxr_snap_reference_valid_) {
        motion_yaw_radians = WrapRadians(current_yaw_radians - pivotxr_snap_reference_yaw_radians_);
        motion_pitch_radians = current_pitch_radians - pivotxr_snap_reference_pitch_radians_;
    }
    // A snap ends once it is released and its return transition has landed.
    auto finish_settled_snap = [&]() {
        if (!pivotxr_quick_view_active_ && pivotxr_quick_view_transitioning_ &&
            !pivotxr_quick_view_transition_.active && !pivotxr_quick_view_retarget_pending_ &&
            !pivotxr_snap_return_pending_) {
            pivotxr_quick_view_transitioning_ = false;
            ClearPivotSnapState();
        }
    };
    if (update_smoothing) {
        UpdatePivotViewTransition(delta_seconds, pivotxr_manual_view_transition_);
        UpdatePivotViewTransition(delta_seconds, pivotxr_profile_view_transition_);
        if (pivotxr_quick_view_retarget_pending_) {
            PivotViewOffset target = pivotxr_quick_view_pending_pose_;
            const double canonical_yaw_offset =
                target.yaw_radians - current_yaw_radians;
            double yaw_delta = WrapRadians(
                canonical_yaw_offset - pivotxr_quick_view_transition_.current.yaw_radians);
            target.yaw_radians =
                pivotxr_quick_view_transition_.current.yaw_radians + yaw_delta;
            target.pitch_radians = std::clamp(target.pitch_radians - current_pitch_radians,
                                              DegreesToRadians(-kPivotMaxPitchDegrees),
                                              DegreesToRadians(kPivotMaxPitchDegrees));
            RetargetPivotViewTransition(
                target, pivotxr_quick_view_pending_duration_seconds_,
                pivotxr_quick_view_transition_);
            pivotxr_quick_view_retarget_pending_ = false;
        }
        UpdatePivotViewTransition(delta_seconds, pivotxr_quick_view_transition_);
        finish_settled_snap();
    }
    bool quick_view_override = PivotSnapEffectActive();

    // Track the steady-state pivot amount only while actively engaged. During
    // the release ramp it is frozen and the envelope eases the applied angle to
    // zero, avoiding any snap on toggle-off. An exclusive snap freezes it for
    // the handoff when the snap returns; a stacked snap keeps it live.
    if (update_smoothing && pivotxr_engaged_ && (!quick_view_override || pivotxr_snap_stacked_) &&
        settings.behavior != PivotProfileBehavior::SnapViews) {
        const int previous_yaw_step = pivotxr_yaw_step_;
        const int previous_pitch_step = pivotxr_pitch_step_;
        if (settings.response_mode == PivotResponseMode::Stepped) {
            UpdatePivotSteppedExtraAngleRadians(motion_yaw_radians,
                                                settings.yaw_step_positive,
                                                settings.yaw_step_negative,
                                                settings.step_glide_mode,
                                                settings.step_glide_seconds,
                                                delta_seconds,
                                                pivotxr_yaw_step_,
                                                pivotxr_smoothed_extra_yaw_radians_,
                                                pivotxr_yaw_step_glide_);
            UpdatePivotSteppedExtraAngleRadians(motion_pitch_radians,
                                                settings.pitch_step_positive,
                                                settings.pitch_step_negative,
                                                settings.step_glide_mode,
                                                settings.step_glide_seconds,
                                                delta_seconds,
                                                pivotxr_pitch_step_,
                                                pivotxr_smoothed_extra_pitch_radians_,
                                                pivotxr_pitch_step_glide_);
        } else {
            const PivotAxisTuning& yaw_tuning =
                motion_yaw_radians >= 0.0 ? settings.yaw_positive : settings.yaw_negative;
            const PivotAxisTuning& pitch_tuning =
                motion_pitch_radians >= 0.0 ? settings.pitch_positive : settings.pitch_negative;
            ComputePivotExtraAngleRadians(motion_yaw_radians,
                                          yaw_tuning.rotation_multiplier,
                                          yaw_tuning.deadzone_degrees,
                                          yaw_tuning.max_extra_degrees,
                                          settings.smoothing,
                                          delta_seconds,
                                          pivotxr_smoothed_extra_yaw_radians_,
                                          true,
                                          yaw_tuning.deadzone_transition_degrees);
            ComputePivotExtraAngleRadians(motion_pitch_radians,
                                          pitch_tuning.rotation_multiplier,
                                          pitch_tuning.deadzone_degrees,
                                          pitch_tuning.max_extra_degrees,
                                          settings.smoothing,
                                          delta_seconds,
                                          pivotxr_smoothed_extra_pitch_radians_,
                                          true,
                                          pitch_tuning.deadzone_transition_degrees);
            pivotxr_yaw_step_ = 0;
            pivotxr_pitch_step_ = 0;
            pivotxr_yaw_step_glide_ = {pivotxr_smoothed_extra_yaw_radians_,
                                       pivotxr_smoothed_extra_yaw_radians_, 0.0};
            pivotxr_pitch_step_glide_ = {pivotxr_smoothed_extra_pitch_radians_,
                                         pivotxr_smoothed_extra_pitch_radians_, 0.0};
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

        const PivotViewOffset position_target =
            ComputePivotPositionAssist(motion_yaw_radians, motion_pitch_radians, settings.position_assist);
        const double position_blend =
            1.0 - std::exp(-std::max(0.0, delta_seconds) / kPivotPositionAssistSmoothingSeconds);
        pivotxr_position_offset_.right_meters +=
            (position_target.right_meters - pivotxr_position_offset_.right_meters) * position_blend;
        pivotxr_position_offset_.up_meters +=
            (position_target.up_meters - pivotxr_position_offset_.up_meters) * position_blend;
        pivotxr_position_offset_.forward_meters +=
            (position_target.forward_meters - pivotxr_position_offset_.forward_meters) * position_blend;
    }

    if (snap_return_handoff) {
        // Motion now follows the seated origin. Fold the change into the
        // return so the view stays put, then ease the whole offset home over
        // what remains of the release transition.
        auto& transition = pivotxr_quick_view_transition_;
        AddPivotViewOffset(transition.current,
                           SubtractPivotViewOffsets(motion_before_handoff, ComposedPivotMotionOffset()));
        RetargetPivotViewTransition(NearestEquivalentPivotTarget(transition.current, transition.target),
                                    std::max(0.0, transition.duration_seconds - transition.elapsed_seconds),
                                    transition);
        UpdatePivotViewTransition(0.0, transition);
        pivotxr_snap_return_pending_ = false;
        finish_settled_snap();
        quick_view_override = PivotSnapEffectActive();
    }

    // Nudges and an active snap set the rotation; motion adds to it unless an
    // exclusive snap replaces it. Snap position offsets keep the seated
    // origin's full orientation (their tuning predates Position Assist), while
    // Position Assist uses its gravity-aligned heading.
    const double eased_gain = SmoothStep(pivotxr_activation_gain_);
    const bool motion_visible = settings.behavior == PivotProfileBehavior::EnhancedMotion &&
                                (!quick_view_override || pivotxr_snap_stacked_);
    PivotViewOffset applied_view_offset;
    applied_view_offset.yaw_radians = pivotxr_manual_view_transition_.current.yaw_radians +
                                      pivotxr_profile_view_transition_.current.yaw_radians;
    applied_view_offset.pitch_radians = pivotxr_manual_view_transition_.current.pitch_radians +
                                        pivotxr_profile_view_transition_.current.pitch_radians;
    XrVector3f translation{0.0f, 0.0f, 0.0f};
    if (quick_view_override) {
        AddPivotViewOffset(applied_view_offset, pivotxr_quick_view_transition_.current);
        translation = RotateVector(pivotxr_origin_ ? pivotxr_origin_->pose.orientation : IdentityPose().orientation,
                                   PivotOffsetTranslation(pivotxr_quick_view_transition_.current, 1.0));
    }
    if (motion_visible) {
        applied_view_offset.yaw_radians += pivotxr_smoothed_extra_yaw_radians_ * eased_gain;
        applied_view_offset.pitch_radians += pivotxr_smoothed_extra_pitch_radians_ * eased_gain;
        const XrQuaternionf assist_heading = YawQuaternion(static_cast<float>(PivotPositionAssistYawRadians()));
        translation = AddVectors(
            translation, RotateVector(assist_heading, PivotOffsetTranslation(pivotxr_position_offset_, eased_gain)));
    }
    const double extra_yaw_radians =
        quick_view_override ? WrapRadians(applied_view_offset.yaw_radians)
                            : std::clamp(applied_view_offset.yaw_radians, -kPi, kPi);
    const double extra_pitch_radians =
        std::clamp(applied_view_offset.pitch_radians,
                   DegreesToRadians(-kPivotMaxPitchDegrees), DegreesToRadians(kPivotMaxPitchDegrees));
    if (update_smoothing) {
        pivot_diagnostic_.has_view_pose = true;
        pivot_diagnostic_.view_time = time;
        pivot_diagnostic_.view_location_flags = location->locationFlags;
        pivot_diagnostic_.pivot_active = pivotxr_engaged_;
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
    std::optional<XrVector3f> translation_anchor;
    if (position_valid && pivotxr_translation_anchor_) {
        const auto& anchor = *pivotxr_translation_anchor_;
        if (anchor.space == reference_space) {
            translation_anchor = anchor.position;
        } else {
            // Like the submission-space conversion, this internal reference
            // relation must use the known runtime horizon, never a fabricated
            // future Turbo time that could block while holding mutex_.
            XrSpaceLocation located_relation{XR_TYPE_SPACE_LOCATION};
            // xrLocateSpace supplies this relation from outside mutex_. Only
            // the xrLocateViews drive path may perform the internal lookup.
            if (!anchor_space_in_reference && update_smoothing && next_locate_space_) {
                if (XR_SUCCEEDED(next_locate_space_(anchor.space, reference_space,
                    ClampInternalLocateTime(time), &located_relation))) {
                    anchor_space_in_reference = &located_relation;
                }
            }
            constexpr XrSpaceLocationFlags valid_pose =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            if (anchor_space_in_reference &&
                (anchor_space_in_reference->locationFlags & valid_pose) == valid_pose) {
                XrPosef anchor_pose = IdentityPose();
                anchor_pose.position = anchor.position;
                translation_anchor = MultiplyPoses(anchor_pose, anchor_space_in_reference->pose).position;
            }
        }
    }
    XrPosef selected_pose = ApplyExtraRotationToPose(
        view_pose, static_cast<float>(extra_yaw_radians), static_cast<float>(extra_pitch_radians),
        translation_anchor ? &*translation_anchor : nullptr);
    selected_pose.position = AddVectors(selected_pose.position, translation);
    PivotPoseOffsetComponents components;
    const XrPosef selected_offset = PoseOffsetBetween(view_pose, selected_pose);
    if (quick_view_override) {
        components.quick_view = selected_offset;
        components.quick_view_active = true;
    } else {
        components.motion_assist = selected_offset;
    }
    const XrPosef composed_pose_offset = ComposePivotPoseOffset(components);
    const XrPosef manipulated_pose = MultiplyPoses(view_pose, composed_pose_offset);
    location->pose = space_is_view ? manipulated_pose : InvertPose(manipulated_pose);
    if (applied_pose_delta && space_is_view && !base_space_is_view) {
        *applied_pose_delta = composed_pose_offset;
    }
    return result;
}

void OpenXrLayer::LogResolvedSettings(const ResolvedRuntimeConfig& settings) {
    std::ostringstream stream;
    stream << "Resolved settings for " << current_exe_name_ << ": "
           << "coreEnabled=" << settings.core.enabled
           << ", logLevel=" << ToString(settings.core.log_level)
           << ", depthxrEnabled=" << settings.depthxr.enabled
           << ", depthToggleBindings=" << BindingListLabel(settings.depthxr_bindings.toggle_enabled_bindings)
           << ", depthAnchorToggleBindings=" << BindingListLabel(settings.depthxr_bindings.toggle_anchor_bindings)
           << ", stereoBoost=" << settings.depthxr.stereo_boost
           << ", convergence=" << settings.depthxr.convergence
           << ", depthAnchor=" << settings.depthxr.depth_anchor
           << ", pivotxrEnabled=" << settings.pivotxr.enabled
           << ", pivotProfileCount=" << settings.pivotxr.profiles.size();
    for (size_t i = 0; i < settings.pivotxr.profiles.size(); ++i) {
        const PivotXrResolvedProfile& profile = settings.pivotxr.profiles[i];
        stream << ", pivotProfile[" << i << "]={name=" << profile.name
               << ", baseline=" << (profile.always_active ? "alwaysActive" : "manual")
               << ", bindings=" << BindingListLabel(profile.activation_bindings)
               << ", setOriginBindings=" << BindingListLabel(profile.set_origin_bindings)
               << ", releaseOriginBindings=" << BindingListLabel(profile.release_origin_bindings)
               << ", nudgeBindingCount=" << (profile.view_controls.nudges.yaw_left_bindings.size() +
                                                profile.view_controls.nudges.yaw_right_bindings.size() + profile.view_controls.nudges.pitch_up_bindings.size() +
                                                profile.view_controls.nudges.pitch_down_bindings.size() + profile.view_controls.nudges.center_bindings.size())
               << ", quickViewCount=" << profile.view_controls.quick_views.size()
               << ", motionInSnaps=" << profile.apply_motion_to_snap_views
               << ", positionAssist=" << profile.position_assist.enabled
               << ", deadzoneTransition=" << profile.yaw_deadzone_transition_degrees
               << ", pitchDeadzoneTransition=" << profile.pitch_deadzone_transition_degrees
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
            stream << ", stepGlide=" << ToString(profile.step_glide_mode)
                   << ", stepGlideSeconds=" << profile.step_glide_seconds
                   << ", yawLeftStep={trigger=" << profile.yaw_step_positive.trigger_degrees
                   << ", amount=" << profile.yaw_step_positive.amount_degrees << "}"
                   << ", yawRightStep={trigger=" << profile.yaw_step_negative.trigger_degrees
                   << ", amount=" << profile.yaw_step_negative.amount_degrees << "}"
                   << ", pitchUpStep={trigger=" << profile.pitch_step_positive.trigger_degrees
                   << ", amount=" << profile.pitch_step_positive.amount_degrees << "}"
                   << ", pitchDownStep={trigger=" << profile.pitch_step_negative.trigger_degrees
                   << ", amount=" << profile.pitch_step_negative.amount_degrees << "}";
        }
        stream << ", yawLeftMultiplier=" << profile.yaw_positive.rotation_multiplier
               << ", yawRightMultiplier=" << profile.yaw_negative.rotation_multiplier
               << ", pitchUpMultiplier=" << profile.pitch_positive.rotation_multiplier
               << ", pitchDownMultiplier=" << profile.pitch_negative.rotation_multiplier << "}";
    }
    stream << ", turboEnabled=" << settings.turbo.enabled
           << ", turboToggleBindings=" << BindingListLabel(settings.turbo.toggle_bindings)
           << ", turboPacingMode=" << ToString(settings.turbo.pacing_mode)
           << ", turboRuntimePins=" << settings.turbo.runtime_pins.size()
           << ", turboMetricsMode=" << ToString(settings.turbo.metrics_mode)
           << ", turboMetricsBindings=" << BindingListLabel(settings.turbo.metrics_bindings)
           << ", quadviewsEnabled=" << settings.quadviews.enabled
           << ", quadviewsDiagnosticVisualizationBinding=" << BindingLabel(settings.quadviews.diagnostic_visualization_binding)
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
           << ", quadviewsGazeDeadzone=" << settings.quadviews.gaze_deadzone_degrees
           << ", framegenEnabled=" << settings.framegen.enabled
           << ", framegenProfile=" << settings.framegen.profile_name
           << ", framegenMode=" << ToString(settings.framegen.synthesis_mode)
           << ", framegenFlowQuality=" << ToString(settings.framegen.flow_quality)
           << ", framegenMotionEstimation=" << ToString(settings.framegen.motion_estimation)
           << ", framegenMultiplier=" << ToString(settings.framegen.multiplier)
           << ", framegenArtifactSuppression=" << settings.framegen.artifact_suppression
           << ", framegenMotionLimitPercent=" << settings.framegen.motion_limit_percent
           << ", framegenExtendHeadPrediction=" << settings.framegen.extend_head_prediction
           << ", framegenDiagnosticView=" << ToString(settings.framegen.diagnostic_view)
           << ", framegenToggleBindings=" << BindingListLabel(settings.framegen.toggle_bindings);
    logger_.Debug(stream.str());
}

void OpenXrLayer::ResetPivotPoseDeltaContinuityState() {
    pivotxr_last_matched_pose_delta_.reset();
    pivotxr_consecutive_pose_delta_misses_ = 0;
    pivotxr_pose_delta_miss_logged_ = false;
}

void OpenXrLayer::ResetPivotActivationState() {
    ResetPivotPoseDeltaContinuityState();
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_yaw_step_ = 0;
    pivotxr_pitch_step_ = 0;
    pivotxr_yaw_step_glide_ = {};
    pivotxr_pitch_step_glide_ = {};
    pivotxr_activation_gain_ = 0.0;
    pivotxr_position_offset_ = {};
    pivotxr_last_smoothing_wall_time_.reset();
    pivotxr_engaged_ = false;
    pivotxr_active_profile_index_ = 0;
    pivotxr_profile_input_states_.clear();
    pivotxr_manual_view_transition_ = {};
    pivotxr_profile_view_transition_ = {};
    pivotxr_quick_view_transition_ = {};
    ClearPivotSnapState();
    pivotxr_quick_view_retarget_pending_ = false;
    pivotxr_quick_view_active_ = false;
    pivotxr_quick_view_transitioning_ = false;
    pivotxr_quick_view_profile_index_ = 0;
    pivotxr_quick_view_index_ = 0;
    pivotxr_origin_.reset();
    pivotxr_origin_capture_pending_ = false;
    pivotxr_translation_anchor_.reset();
    pivotxr_reference_changes_.clear();
    pivotxr_binding_last_poll_time_.reset();
    pivot_diagnostic_stride_counter_ = 0;
    pivot_diagnostic_ = PivotDiagnosticState{};
}

void OpenXrLayer::ResetPivotInputStateForConfigChange() {
    // An incompatible binding/profile edit invalidates edge state, but the
    // displayed pose must still ease back through the existing activation
    // envelope. Preserve the generated angles, gain, smoothing clock, and
    // seated origin; only disarm the action and prime the new controls.
    pivotxr_engaged_ = false;
    pivotxr_active_profile_index_ = resolved_settings_.pivotxr.profiles.empty()
                                        ? 0
                                        : std::min(pivotxr_active_profile_index_,
                                                   resolved_settings_.pivotxr.profiles.size() - 1);
    pivotxr_profile_input_states_.clear();
    pivotxr_manual_view_transition_ = {};
    pivotxr_profile_view_transition_ = {};
    pivotxr_quick_view_transition_ = {};
    ClearPivotSnapState();
    pivotxr_quick_view_retarget_pending_ = false;
    pivotxr_quick_view_active_ = false;
    pivotxr_quick_view_transitioning_ = false;
    pivotxr_quick_view_profile_index_ = 0;
    pivotxr_quick_view_index_ = 0;
    pivotxr_origin_capture_pending_ = false;
    pivotxr_binding_last_poll_time_.reset();
    pending_locate_views_diagnostics_ = 5;
    pending_end_frame_diagnostics_ = 5;
    pending_pivot_diagnostics_ = kPivotDiagnosticBurstCount;
}

void OpenXrLayer::ResetQuadViewsDiagnosticVisualizationState() {
    quadviews_diagnostic_visualization_enabled_ = false;
    quadviews_diagnostic_visualization_binding_was_down_ = false;
    quadviews_diagnostic_visualization_binding_last_poll_time_.reset();
    quadviews_diagnostic_visualization_binding_down_cached_ = false;
}

void OpenXrLayer::PollQuadViewsDiagnosticVisualizationToggle() {
#if defined(_WIN32)
    const InputBinding& binding = resolved_settings_.quadviews.diagnostic_visualization_binding;
    if (binding.type == InputBindingType::None) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool first_poll = !quadviews_diagnostic_visualization_binding_last_poll_time_.has_value();
    if (first_poll ||
        now - *quadviews_diagnostic_visualization_binding_last_poll_time_ >= kInputBindingPollInterval) {
        quadviews_diagnostic_visualization_binding_last_poll_time_ = now;
        quadviews_diagnostic_visualization_binding_down_cached_ = PollInputBindingDown(binding);
    }

    const bool binding_down = quadviews_diagnostic_visualization_binding_down_cached_;
    if (first_poll) {
        // Prime the edge detector so the key/button used while assigning the
        // binding cannot make the visualization appear immediately.
        quadviews_diagnostic_visualization_binding_was_down_ = binding_down;
        return;
    }

    const bool was_pressed = binding_down && !quadviews_diagnostic_visualization_binding_was_down_;
    quadviews_diagnostic_visualization_binding_was_down_ = binding_down;
    if (!was_pressed) {
        return;
    }

    // Refused while Frame Generation generates from the composed images (it
    // would measure the overlay as motion), as the app's control is; hiding
    // it always works.
    if (!quadviews_diagnostic_visualization_enabled_ && QuadViewsOverlayWithheld()) {
        logger_.Info("Quadviews diagnostic visualization is unavailable (" + BindingLabel(binding) +
                     "): Frame Generation generates frames from the composed images in this session. Turn Frame "
                     "Generation off for this game and restart it to use the visualization.");
        return;
    }
    quadviews_diagnostic_visualization_enabled_ = !quadviews_diagnostic_visualization_enabled_;
    runtime_relay_status_dirty_.store(true, std::memory_order_release);
    logger_.Info(std::string("Quadviews diagnostic visualization ") +
                 (quadviews_diagnostic_visualization_enabled_ ? "shown" : "hidden") + " via " +
                 BindingLabel(binding) + ".");
    SoundPlayer::Instance().PlayTransition(binding.sound,
                                           quadviews_diagnostic_visualization_enabled_,
                                           dll_directory_,
                                           resolved_settings_.core.sound_volume);
#endif
}

void OpenXrLayer::ResetDepthToggleState() {
    depthxr_toggle_enabled_ = true;
    depthxr_toggle_binding_was_down_ = false;
    depthxr_binding_last_poll_time_.reset();
    depthxr_toggle_pressed_.reset();
    depthxr_binding_down_cached_ = false;
    ResetDepthAnchorToggleState();
}

void OpenXrLayer::ResetDepthAnchorToggleState() {
    depth_anchor_toggle_inverted_ = false;
    depth_anchor_active_ = resolved_settings_.depthxr.depth_anchor;
    depth_anchor_toggle_binding_was_down_ = false;
    depth_anchor_binding_last_poll_time_.reset();
    depth_anchor_toggle_pressed_.reset();
    depth_anchor_binding_down_cached_ = false;
}

void OpenXrLayer::PollDepthAnchorToggle() {
    depth_anchor_active_ = resolved_settings_.depthxr.depth_anchor != depth_anchor_toggle_inverted_;

#if defined(_WIN32)
    const auto now = std::chrono::steady_clock::now();
    const bool first_poll = !depth_anchor_binding_last_poll_time_.has_value();
    if (first_poll || now - *depth_anchor_binding_last_poll_time_ >= kInputBindingPollInterval) {
        depth_anchor_binding_last_poll_time_ = now;
        const InputBinding* down = PollInputBindingsDown(resolved_settings_.depthxr_bindings.toggle_anchor_bindings);
        depth_anchor_binding_down_cached_ = down != nullptr;
        if (down) {
            depth_anchor_toggle_pressed_ = *down;
        }
    }
    const bool binding_down = depth_anchor_binding_down_cached_;
    if (first_poll) {
        depth_anchor_toggle_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !depth_anchor_toggle_binding_was_down_;
    depth_anchor_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        depth_anchor_toggle_inverted_ = !depth_anchor_toggle_inverted_;
        depth_anchor_active_ = resolved_settings_.depthxr.depth_anchor != depth_anchor_toggle_inverted_;
        depth_view_info_pending_ = true;
        depth_submission_info_pending_ = true;
        depth_submission_info_not_before_time_.reset();
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        const InputBinding binding =
            ActingBinding(depth_anchor_toggle_pressed_, resolved_settings_.depthxr_bindings.toggle_anchor_bindings);
        logger_.Info(std::string("Depth Lock ") +
                     (depth_anchor_active_ ? "enabled" : "disabled") + " via " + BindingLabel(binding) + ".");
        SoundPlayer::Instance().PlayTransition(binding.sound,
                                               depth_anchor_active_, dll_directory_,
                                               resolved_settings_.core.sound_volume,
                                               L"depth-lock-on.wav", L"depth-lock-off.wav");
    }
#endif
}

void OpenXrLayer::ResetTurboToggleState() {
    turbo_toggle_enabled_ = true;
    turbo_toggle_binding_was_down_ = false;
    turbo_binding_last_poll_time_.reset();
    turbo_toggle_pressed_.reset();
    turbo_binding_down_cached_ = false;
}

void OpenXrLayer::StopFrameGenPresenterForTeardown(XrSession session,
                                                   const std::function<void(const char*)>& step) {
    step("stopping Frame Generation's presenter thread");
    if (framegen_.StopPresenter()) {
        return;
    }
    // A runtime may hold a call (its waits once the session is stopping)
    // until the session ends: the application never ended it, or the
    // presenter thread was still inside when it did. Ending it lets the
    // thread go; a runtime whose session already ended refuses harmlessly.
    step("in the runtime's xrEndSession, to let Frame Generation's presenter thread go");
    const XrResult ended = next_end_session_ ? next_end_session_(session) : XR_ERROR_FUNCTION_UNSUPPORTED;
    step("stopping Frame Generation's presenter thread after the runtime's xrEndSession");
    const bool stopped = framegen_.StopPresenter();
    logger_.Info("Frame Generation's presenter thread was still in the runtime at the session's teardown; the layer "
                 "ended the runtime's session (result=" +
                 std::to_string(static_cast<int>(ended)) + ")" +
                 (stopped ? ", and the thread stopped." : ", and the thread is still there."));
    if (!stopped) {
        framegen_.AbandonPresenter();
    }
}

void OpenXrLayer::PinModulesForAbandonedPresenter() {
    // The loader may unload both after xrDestroyInstance returns; a thread
    // left inside the runtime would return into neither.
    const auto pin = [](const void* address) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           static_cast<LPCWSTR>(address), &module);
    };
    pin(reinterpret_cast<const void*>(&SessionStateName));
    if (next_wait_frame_) pin(reinterpret_cast<const void*>(next_wait_frame_));
    if (next_end_frame_) pin(reinterpret_cast<const void*>(next_end_frame_));
}

void OpenXrLayer::StartFrameGen(XrSession session, const void* binding_chain, const std::string& unavailable) {
    framegen_session_enabled_ = resolved_settings_.core.enabled && resolved_settings_.framegen.enabled;
    framegen_toggle_binding_was_down_ = false;
    framegen_binding_down_cached_ = false;
    framegen_binding_last_poll_time_.reset();
    framegen_toggle_pressed_.reset();
    framegen_.BeginSession();
    if (framegen_session_enabled_) {
        framegen_teardowns_watched_.store(true, std::memory_order_release);
        framegen_calls_.wait = [this](XrSession s, const XrFrameWaitInfo* i, XrFrameState* f) {
            return TraceRuntimeWaitFrame(s, i, f);
        };
        framegen_calls_.begin = [this](XrSession s, const XrFrameBeginInfo* i) { return TraceRuntimeBeginFrame(s, i); };
        framegen_calls_.submit = [this](XrSession s, const XrFrameEndInfo* i) { return SubmitRuntimeFrame(s, i); };
        framegen_.Configure(resolved_settings_.framegen);
        // How long a teardown waits on the presenter thread (2 s unless set).
        std::chrono::milliseconds teardown_wait{2000};
        if (const char* wait = std::getenv("VECTORXR_FRAMEGEN_TEARDOWN_WAIT_MS"); wait && *wait) {
            const long long ms = std::atoll(wait);
            if (ms > 0) teardown_wait = std::chrono::milliseconds(std::min(ms, 60000LL));
        }
        framegen_.SetTeardownWait(teardown_wait);
        framegen_.Start(session, binding_chain,
                        {next_enumerate_swapchain_formats_, next_create_swapchain_, next_destroy_swapchain_,
                         next_enumerate_swapchain_images_, next_acquire_swapchain_image_, next_wait_swapchain_image_,
                         next_release_swapchain_image_, next_locate_views_},
                        &logger_, graphics_api_ == "Vulkan" ? &osd_vulkan_queue_mutex_ : nullptr,
                        unavailable.empty() ? framegen_gpu_factory_
                                            : FrameGenPresenter::GpuFactory([unavailable](const void*, std::string& error) {
                                                  error = unavailable;
                                                  return std::unique_ptr<FrameGenGpu>();
                                              }));
        // Research (VECTORXR_FRAMEGEN_DEPTH_BRIDGE_PROBE=1, read with the
        // session's research switches): log only.
        vulkan_bridge_.SetDepthProbe(framegen_.DepthBridgeProbe());
    }
    framegen_owns_pacing_.store(framegen_.OwnsPacing(), std::memory_order_release);
    if (framegen_owns_pacing_.load(std::memory_order_acquire)) {
        turbo_yielded_to_framegen_ = resolved_settings_.turbo.enabled;
        resolved_settings_.turbo.enabled = false;
        // No Turbo pipeline exists before the session's first frame.
        turbo_frame_interception_required_.store(false, std::memory_order_release);
        if (turbo_yielded_to_framegen_) {
            logger_.Info("Turbo yields to Frame Generation for this session: both change frame pacing, "
                         "and Frame Generation paces the application's frames itself.");
        }
    } else if (framegen_session_enabled_) {
        logger_.Info("Frame Generation is enabled but unavailable for this session: " + framegen_.Status().reason);
        if (d3d11_bridge_.Active()) {
            logger_.Info("Frame Generation's Direct3D 11 bridge stays for the session all the same: its session is "
                         "Direct3D 12 for life.");
        }
        if (vulkan_bridge_.Active()) {
            logger_.Info("Frame Generation's Vulkan bridge stays for the session all the same: its session is "
                         "Direct3D 12 for life.");
        }
    }
}

void OpenXrLayer::ConfigureOsdAnchor() {
    // SteamVR with Pimax's own driver (driver direct mode) draws a head-locked
    // (VIEW) quad at the pose its frame carries, and the driver then turns the
    // whole image to the display pose. A generated frame carries its real
    // frame's pose, so the OSD lands off by the head's turn in between: a
    // double image in Extrapolate, a smear in Interpolate. Placed in the world
    // at each frame's head pose, it lands head-locked whatever pose the frame
    // carries. SteamVR in its own direct mode (with other headset drivers) shows
    // a VIEW quad steady, and both report the same system, so the placement
    // is opt-in: VECTORXR_OSD_ANCHOR=world (or view, the default).
    bool world = false;
    if (const char* anchor = std::getenv("VECTORXR_OSD_ANCHOR"); anchor && *anchor) {
        const std::string value(anchor);
        if (value == "world") {
            world = true;
            logger_.Info("OSD is placed in the world at each frame's head pose (VECTORXR_OSD_ANCHOR=world).");
        } else if (value != "view") {
            logger_.Info("VECTORXR_OSD_ANCHOR=" + value + " is ignored: use view or world.");
        }
    }
    osd_.SetWorldAnchored(world);
}

void OpenXrLayer::EndVulkanBridge() {
    if (vulkan_bridge_.Active()) {
        logger_.Info("Frame Generation's Vulkan bridge at session end: " + vulkan_bridge_.Summary());
    }
    vulkan_bridge_.End();
}

void OpenXrLayer::EndD3D11Bridge() {
    if (!d3d11_bridge_.Active()) {
        return;
    }
    logger_.Info("Frame Generation's Direct3D 11 bridge at session end: " + d3d11_bridge_.Summary());
    d3d11_bridge_.End();
}

XrResult OpenXrLayer::AcquireLayerSwapchainImage(XrSwapchain swapchain, uint32_t image_count, uint32_t* index) {
    XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!d3d11_bridge_.Bridged(swapchain)) {
        return next_acquire_swapchain_image_(swapchain, &info, index);
    }
    const XrResult result = framegen_queue_.Acquire(swapchain, image_count, FrameGenQueueCalls(), &info, index);
    if (XR_SUCCEEDED(result) && index) {
        d3d11_bridge_.Acquired(swapchain, *index);
    }
    return result;
}

XrResult OpenXrLayer::WaitLayerSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info,
                                              bool shown_later) {
    if (!d3d11_bridge_.Bridged(swapchain)) {
        return next_wait_swapchain_image_(swapchain, info);
    }
    return framegen_queue_.Wait(swapchain, FrameGenLazy(swapchain) || shown_later, FrameGenQueueCalls(), info);
}

XrResult OpenXrLayer::ReleaseLayerSwapchainImage(XrSwapchain swapchain, uint32_t index, bool drawn,
                                                 bool shown_later) {
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!d3d11_bridge_.Bridged(swapchain)) {
        if (drawn) {
            framegen_.CaptureOnRelease(swapchain, index);
        }
        NoteFrameGenRelease(swapchain, index);
        return next_release_swapchain_image_(swapchain, &info);
    }
    if (drawn) {
        // The bridge's copy into the runtime's image goes ahead of Frame
        // Generation's capture of it, and of the release (as in
        // ReleaseSwapchainImage).
        if (d3d11_bridge_.Released(swapchain)) {
            framegen_.CaptureOnRelease(swapchain, index);
        }
    } else {
        d3d11_bridge_.Abandoned(swapchain);
    }
    // Before the runtime may get the release: a frame Frame Generation's
    // thread picks to show from then on sees it.
    NoteFrameGenRelease(swapchain, index);
    // Shown later by Frame Generation's thread, the frame's submission makes
    // the runtime's queue wait for it (SubmitRuntimeFrame); until then
    // nothing Frame Generation writes waits for the game's frame.
    return framegen_queue_.Release(swapchain, FrameGenLazy(swapchain) || shown_later, FrameGenQueueCalls(), &info);
}

void OpenXrLayer::NoteFrameGenRelease(XrSwapchain swapchain, uint32_t index) {
    framegen_.NoteRelease(swapchain, index);
#if defined(DEPTHXR_TESTING)
    std::scoped_lock lock(framegen_noted_releases_mutex_);
    ++framegen_noted_releases_[swapchain];
#endif
}

XrResult OpenXrLayer::DestroyLayerSwapchain(XrSwapchain swapchain) {
    if (d3d11_bridge_.Bridged(swapchain)) {
        // As an application's bridged swapchain (DestroySwapchain).
        framegen_queue_.Flush(swapchain, FrameGenQueueCalls());
        framegen_queue_.Forget(swapchain);
        if (!framegen_.ForgetSwapchain(swapchain) || !d3d11_bridge_.RemoveSwapchain(swapchain)) {
            logger_.Error("Swapchain " + FormatHandle(swapchain) +
                          " is kept until its session ends: GPU work on it (Frame Generation's, or the Direct3D 11 "
                          "bridge's) did not finish.");
            return XR_SUCCESS;
        }
        return next_destroy_swapchain_(swapchain);
    }
    framegen_.ForgetSwapchain(swapchain);
    return next_destroy_swapchain_(swapchain);
}

void OpenXrLayer::PollFrameGenToggle() {
    if (!framegen_owns_pacing_.load(std::memory_order_acquire)) {
        return;
    }
    // Turned off in settings (or by the suite switch), the toggle would only
    // flip a state nothing uses, with its sound: it waits, primed afresh.
    if (!resolved_settings_.core.enabled || !resolved_settings_.framegen.enabled) {
        framegen_binding_last_poll_time_.reset();
        return;
    }
#if defined(_WIN32)
    const std::vector<InputBinding>& bindings = resolved_settings_.framegen.toggle_bindings;
    const auto now = std::chrono::steady_clock::now();
    const bool first_poll = !framegen_binding_last_poll_time_.has_value();
    if (first_poll || now - *framegen_binding_last_poll_time_ >= kInputBindingPollInterval) {
        framegen_binding_last_poll_time_ = now;
        const InputBinding* down = PollInputBindingsDown(bindings);
        framegen_binding_down_cached_ = down != nullptr;
        if (down) {
            framegen_toggle_pressed_ = *down;
        }
    }
    const bool down = framegen_binding_down_cached_;
    if (first_poll) {
        framegen_toggle_binding_was_down_ = down;
    }
    const bool pressed = down && !framegen_toggle_binding_was_down_;
    framegen_toggle_binding_was_down_ = down;
    if (!pressed) {
        return;
    }
    const bool enabled = framegen_.Toggle();
    const InputBinding binding = ActingBinding(framegen_toggle_pressed_, bindings);
    logger_.Info(std::string("Frame Generation ") + (enabled ? "enabled" : "disabled") + " via " +
                 BindingLabel(binding) + ".");
    SoundPlayer::Instance().PlayTransition(binding.sound, enabled, dll_directory_, resolved_settings_.core.sound_volume);
#endif
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
        const InputBinding* down = PollInputBindingsDown(resolved_settings_.turbo.toggle_bindings);
        turbo_binding_down_cached_ = down != nullptr;
        if (down) {
            turbo_toggle_pressed_ = *down;
        }
    }
    const bool binding_down = turbo_binding_down_cached_;
    if (first_poll) {
        turbo_toggle_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !turbo_toggle_binding_was_down_;
    turbo_toggle_binding_was_down_ = binding_down;

    // The app's Turbo Safety clear retries like a press, but never toggles.
    const bool cleared_in_app = !was_pressed_this_call && turbo_recovery_.ConsumeClearRequest();
    if (was_pressed_this_call || cleared_in_app) {
        const std::string source = cleared_in_app
            ? std::string("the VectorXR app")
            : BindingLabel(ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings));
        if (turbo_recovery_blocked_.load()) {
            turbo_recovery_retry_requested_ = true;
            turbo_toggle_enabled_ = true;
            turbo_recovery_blocked_.store(false);
            logger_.Info("Turbo recovery block cleared via " + source + "; Turbo retries.");
        } else if (turbo_auto_suspended_.load(std::memory_order_relaxed)) {
            // A toggle press after an auto-suspend re-arms turbo rather than
            // flipping the enable state.
            turbo_auto_suspended_.store(false, std::memory_order_relaxed);
            turbo_recovery_retry_requested_ = true;
            turbo_runtime_error_streak_.store(0);
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
            logger_.Info("Turbo mode re-armed after auto-suspend via " + source + ".");
        } else if (was_pressed_this_call) {
            turbo_toggle_enabled_ = !turbo_toggle_enabled_;
            logger_.Info(std::string("Turbo mode ") + (turbo_toggle_enabled_ ? "enabled" : "disabled") +
                         " via " + BindingLabel(ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings)) + ".");
        }
        if (!turbo_toggle_enabled_) SoundPlayer::Instance().PlayTransition(ActingBinding(turbo_toggle_pressed_, resolved_settings_.turbo.toggle_bindings).sound,
                                               false, dll_directory_,
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

XrResult OpenXrLayer::ForwardAppRelease(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* release_info) {
    // The runtime's queue reads the image only after the game's rendering
    // into it (and Frame Generation's capture of it) has run; while Frame
    // Generation shows only images of its own, an image it captures reaches
    // the runtime once that has run, without the runtime's queue waiting.
    return UnmirroredSwapchain(swapchain)
               ? next_release_swapchain_image_(swapchain, release_info)
               : framegen_queue_.Release(swapchain, FrameGenLazy(swapchain), FrameGenQueueCalls(), release_info);
}

XrResult OpenXrLayer::FlushDeferredSwapchainReleaseLocked(XrSwapchain swapchain,
                                                          SwapchainInfo& info,
                                                          std::string_view reason) {
    if (info.deferred_release_count == 0) {
        return XR_SUCCESS;
    }
    if (!next_release_swapchain_image_) {
        logger_.Error("Deferred swapchain release failed: xrReleaseSwapchainImage is unavailable, reason=" +
                      std::string(reason));
        return XR_ERROR_RUNTIME_FAILURE;
    }

    while (info.deferred_release_count > 0) {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        // As the application's own release would have gone: deferring it must
        // not make Frame Generation's runtime queue wait for the game's frame.
        const XrResult result = ForwardAppRelease(swapchain, &release_info);
        if (XR_FAILED(result)) {
            logger_.Error("Deferred swapchain release failed: handle=" + FormatHandle(swapchain) +
                          ", reason=" + std::string(reason) +
                          ", remaining=" + std::to_string(info.deferred_release_count) +
                          ", result=" + FormatHex(static_cast<uint64_t>(result)));
            return result;
        }
        --info.deferred_release_count;
        info.image_states.ReleaseDownstreamOldest();
    }
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

XrResult OpenXrLayer::HandOverBridgedReleasesLocked(XrSwapchain swapchain, SwapchainInfo& info,
                                                    std::string_view reason, bool copy) {
    while (!info.bridge_handovers.empty()) {
        const uint32_t index = info.bridge_handovers.front();
        info.bridge_handovers.pop_front();
        std::optional<bool> copied_ahead;
        if (!info.bridge_copied.empty()) {
            copied_ahead = info.bridge_copied.front();
            info.bridge_copied.pop_front();
        }
        if (copy) {
            // As ReleaseSwapchainImage did at the release: the bridge's copy
            // into the runtime's image goes ahead of Frame Generation's
            // capture of it, and of the release (MarkGameWork must come after
            // the copy on the bridge's queue, or the runtime may read the
            // image before it). A copy skipped (a stalled queue) leaves an
            // older frame there: not captured.
            const bool copied = copied_ahead ? *copied_ahead
                                             : d3d11_bridge_.Released(swapchain) && vulkan_bridge_.Released(swapchain);
            NoteFrameGenRelease(swapchain, index);
            if (framegen_.TracesSwapchainCalls()) {
                framegen_.Trace().Record("hand", reinterpret_cast<uint64_t>(swapchain), index, copied);
            }
            if (copied) {
                framegen_.CaptureOnRelease(swapchain, index);
            }
        }
        ++info.release_count;
        if (ShouldDeferSwapchainRelease(info)) {
            ++info.deferred_release_count;
            continue;
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const XrResult result = ForwardAppRelease(swapchain, &release_info);
        if (XR_FAILED(result)) {
            logger_.Error("Bridged swapchain release failed: handle=" + FormatHandle(swapchain) +
                          ", reason=" + std::string(reason) +
                          ", remaining=" + std::to_string(info.bridge_handovers.size()) +
                          ", result=" + FormatHex(static_cast<uint64_t>(result)));
            return result;
        }
        info.image_states.ReleaseDownstreamOldest();
        if (info.quadviews_session && info.release_count <= 3) {
            LogSwapchainSummary(swapchain, info, "released");
        }
    }
    return XR_SUCCESS;
}

void OpenXrLayer::SubmitKeptVulkanBridgeReleasesLocked() {
    // A game that renders, releases and ends its session with no frame's
    // end after the release still has that rendering on its queue; the
    // bridge's barrier at the release is what End's wait covers it by
    // (vkFreeMemory of the images it renders into). The copy into the
    // runtime's image goes ahead too, unseen: nothing shows it now.
    for (auto& [swapchain, info] : tracked_swapchains_) {
        for (std::size_t kept = 0; kept < info.bridge_handovers.size(); ++kept) {
            vulkan_bridge_.Released(swapchain, true);
        }
    }
}

XrResult OpenXrLayer::HandOverBridgedReleasesLocked(std::string_view reason, bool copy) {
    // While the Vulkan bridge gives the game's motion vector images back at
    // their releases, every swapchain's copies go first: a giving back waits
    // on the GPU for its copy, and a capture of Frame Generation's queued
    // ahead of the copy on the bridge's queue would hold the game's next
    // frame back as well.
    if (copy && vulkan_bridge_motion_) {
        for (auto& [swapchain, info] : tracked_swapchains_) {
            for (std::size_t k = info.bridge_copied.size(); k < info.bridge_handovers.size(); ++k) {
                info.bridge_copied.push_back(d3d11_bridge_.Released(swapchain) && vulkan_bridge_.Released(swapchain));
            }
        }
    }
    XrResult first_failure = XR_SUCCESS;
    for (auto& [swapchain, info] : tracked_swapchains_) {
        const XrResult result = HandOverBridgedReleasesLocked(swapchain, info, reason, copy);
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
            DestroyLayerSwapchain(target.swapchain);
        }
        target = {};
    }
    d3d11_focus_sharpen_.failed_shapes = {};
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
    // A target that could not be built is tried again only for another size
    // or format: not a runtime swapchain created and destroyed, and an error
    // logged, for each eye on every frame.
    const size_t target_index = static_cast<size_t>(&target - d3d11_focus_sharpen_.targets.data());
    auto& failed_shape = d3d11_focus_sharpen_.failed_shapes[target_index];
    if (failed_shape && failed_shape->width == width && failed_shape->height == height &&
        failed_shape->format == format) {
        return false;
    }

    // Recreate on any size/format change.
    const auto release_target = [&]() {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            DestroyLayerSwapchain(target.swapchain);
        }
        target = {};
    };
    release_target();
    // A failure leaves no half-built target behind, and is remembered.
    const auto fail = [&]() {
        release_target();
        failed_shape = D3D11FocusSharpen::TargetShape{width, height, format};
        return false;
    };

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
        target.swapchain = XR_NULL_HANDLE;
        return fail();
    }

    framegen_.TrackSwapchain(target.swapchain, format, 1, create_info.usageFlags);
    uint32_t image_count = 0;
    result = next_enumerate_swapchain_images_(target.swapchain, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        logger_.Error("D3D11 focus sharpen output image count query failed.");
        return fail();
    }
    std::vector<XrSwapchainImageD3D11KHR> images(image_count);
    for (XrSwapchainImageD3D11KHR& image : images) {
        image = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    }
    result = next_enumerate_swapchain_images_(
        target.swapchain, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        logger_.Error("D3D11 focus sharpen output image enumeration failed.");
        return fail();
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
            return fail();
        }
        target.image_render_target_views.push_back(rtv);
    }
    failed_shape.reset();
    return true;
}

bool OpenXrLayer::AcquireFocusSharpenImage(QuadViewsCompositionTarget& target,
                                           uint32_t* index,
                                           const char** reason) {
    XrResult result = XR_SUCCESS;
    if (target.awaiting_wait) {
        // Acquired before, its wait timed out: waited for again, never
        // acquired a second time (a second image would leave the runtime
        // given the older one from then on, one frame stale).
        *index = target.awaiting_index;
    } else {
        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        result = next_acquire_swapchain_image_(target.swapchain, &acquire_info, index);
        if (XR_FAILED(result) || *index >= target.image_render_target_views.size()) {
            *reason = "sharpen output acquire failed";
            return false;
        }
    }
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = kInternalSwapchainWaitTimeout;
    result = next_wait_swapchain_image_(target.swapchain, &wait_info);
    if (result == XR_TIMEOUT_EXPIRED) {
        // The runtime has not handed the image back: it is neither drawn into
        // nor released, and the focus view goes out unsharpened this frame.
        target.awaiting_wait = true;
        target.awaiting_index = *index;
        *reason = "sharpen output wait timed out";
        return false;
    }
    target.awaiting_wait = false;
    if (XR_FAILED(result)) {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_SUCCEEDED(next_release_swapchain_image_(target.swapchain, &release_info))) {
            NoteFrameGenRelease(target.swapchain, *index);
        }
        *reason = "sharpen output wait failed";
        return false;
    }
    return true;
}

void OpenXrLayer::SharpenNativeFocusViews(std::vector<XrCompositionLayerProjectionView>& views,
                                          XrTime display_time) {
    const double sharpen_amount = Clamp(resolved_settings_.quadviews.foveate_sharpness, 0.0, 100.0) / 100.0;
    if (!config_.core.enabled || sharpen_amount <= 0.001 || views.size() < 4) {
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
    ID3DDeviceContextState* application_context_state = nullptr;
    const bool use_context_state = d3d11_quadviews_compositor_.context1 &&
                                   d3d11_quadviews_compositor_.layer_context_state;
    if (use_context_state) {
        d3d11_quadviews_compositor_.context1->SwapDeviceContextState(
            d3d11_quadviews_compositor_.layer_context_state, &application_context_state);
        context->ClearState();
    } else {
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, &saved.depth_stencil);
        context->VSGetShader(&saved.vertex_shader, nullptr, nullptr);
        context->PSGetShader(&saved.pixel_shader, nullptr, nullptr);
        context->IAGetInputLayout(&saved.input_layout);
        context->IAGetPrimitiveTopology(&saved.topology);
        context->PSGetShaderResources(0, 1, saved.shader_resources);
        context->PSGetSamplers(0, 1, saved.samplers);
        context->PSGetConstantBuffers(0, 1, saved.constant_buffers);
        context->RSGetViewports(&saved.viewport_count, saved.viewports);
    }

    uint32_t sharpened_count = 0;
    // Why each focus view was skipped, for the one-shot diagnostic below — the
    // pass otherwise degrades to a silent no-op that looks like a dead slider.
    const char* skip_reason[2] = {"ok", "ok"};
    for (uint32_t i = 2; i < 4; ++i) {
        const char*& reason = skip_reason[i - 2];
        XrCompositionLayerProjectionView& view = views[i];
        if (view.subImage.swapchain == XR_NULL_HANDLE) {
            reason = "null subImage";
            continue;
        }
        const auto it = tracked_swapchains_.find(view.subImage.swapchain);
        if (it == tracked_swapchains_.end() || it->second.d3d11_images.empty() ||
            !(it->second.has_last_acquired_image_index || it->second.has_last_released_image_index)) {
            reason = "swapchain untracked or no acquired image yet";
            continue;
        }
        SwapchainInfo& src = it->second;
        const uint32_t array_slice = view.subImage.imageArrayIndex;
        if (array_slice >= std::max<uint32_t>(1, src.array_size)) {
            reason = "array slice out of range";
            continue;
        }
        if (src.array_size != 1) {
            reason = "array focus sharpen is unsupported; forwarding unsharpened";
            continue;
        }
        // This frame's content: the last-released image (turbo pipelining can
        // advance last_acquired past it before EndFrame runs).
        const uint32_t src_index = src.has_last_released_image_index ? src.last_released_image_index
                                                                     : src.last_acquired_image_index;
        if (src_index >= src.d3d11_images.size()) {
            reason = "image index out of range";
            continue;
        }
        const size_t shader_resource_slot =
            src_index * std::max<uint32_t>(1, src.array_size) + array_slice;
        if (!EnsureD3D11SwapchainShaderResources(src, array_slice) ||
            shader_resource_slot >= src.d3d11_shader_resources.size()) {
            reason = "no shader resource views (focus swapchain needs sampled usage)";
            continue;
        }
        ID3D11ShaderResourceView* focus_srv = src.d3d11_shader_resources[shader_resource_slot];
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
        if (!AcquireFocusSharpenImage(target, &output_index, &reason)) {
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
        // As above, the context-state path has known fullscreen state and needs no pre-clear.
        // Keep the fallback safe when the application's rasterizer/blend state is inherited.
        if (!use_context_state) {
            const float clear_color[4]{0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(render_target, clear_color);
        }
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

        framegen_.CaptureOnRelease(target.swapchain, output_index);
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(next_release_swapchain_image_(target.swapchain, &release_info))) {
            reason = "sharpen output release failed";
            continue;
        }
        NoteFrameGenRelease(target.swapchain, output_index);

        // Repoint the submitted view at our sharpened swapchain.
        view.subImage.swapchain = target.swapchain;
        view.subImage.imageArrayIndex = 0;
        view.subImage.imageRect.offset = {0, 0};
        view.subImage.imageRect.extent = {static_cast<int32_t>(out_width), static_cast<int32_t>(out_height)};
        ++sharpened_count;
    }

    // Restore app context state.
    if (use_context_state) {
        d3d11_quadviews_compositor_.context1->SwapDeviceContextState(application_context_state, nullptr);
        SafeRelease(application_context_state);
    } else {
        context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved.render_targets, saved.depth_stencil);
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

bool OpenXrLayer::EnsureD3D11QuadViewsPixelProbeResources(int64_t format) {
    QuadViewsPixelProbe& probe = d3d11_quadviews_compositor_.pixel_probe;
    const DXGI_FORMAT dxgi_format = static_cast<DXGI_FORMAT>(format);
    const bool has_all_staging_textures = std::all_of(
        probe.staging_textures.begin(), probe.staging_textures.end(),
        [](ID3D11Texture2D* texture) { return texture != nullptr; });
    if (probe.completion && has_all_staging_textures && probe.format == format) {
        return true;
    }
    if (probe.issued || !d3d11_quadviews_compositor_.device || PixelProbeBytesPerPixel(dxgi_format) == 0) {
        if (PixelProbeBytesPerPixel(dxgi_format) == 0) {
            logger_.Debug("D3D11 quadviews pixel probes disabled for unsupported format=" +
                          std::to_string(static_cast<uint32_t>(format)) + ".");
            pending_quadviews_pixel_diagnostics_ = 0;
        }
        return false;
    }

    const bool preserve_history = probe.format == 0 || probe.format == format;
    ReleaseD3D11QuadViewsPixelProbeResources(preserve_history);

    D3D11_TEXTURE2D_DESC staging_desc{};
    staging_desc.Width = kQuadViewsPixelProbeSize;
    staging_desc.Height = kQuadViewsPixelProbeSize;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = dxgi_format;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT result = S_OK;
    for (ID3D11Texture2D*& staging_texture : probe.staging_textures) {
        result = d3d11_quadviews_compositor_.device->CreateTexture2D(
            &staging_desc, nullptr, &staging_texture);
        if (FAILED(result)) {
            break;
        }
    }
    if (SUCCEEDED(result)) {
        D3D11_QUERY_DESC query_desc{};
        query_desc.Query = D3D11_QUERY_EVENT;
        result = d3d11_quadviews_compositor_.device->CreateQuery(&query_desc, &probe.completion);
    }
    if (FAILED(result)) {
        logger_.Info("D3D11 quadviews recovery pixel probes unavailable. hr=" +
                     FormatHex(static_cast<uint32_t>(result)) +
                     ", format=" + std::to_string(static_cast<uint32_t>(format)));
        ReleaseD3D11QuadViewsPixelProbeResources(preserve_history);
        pending_quadviews_pixel_diagnostics_ = 0;
        return false;
    }
    probe.format = format;
    return true;
}

void OpenXrLayer::ReleaseD3D11QuadViewsPixelProbeResources(bool preserve_history) {
    QuadViewsPixelProbe& probe = d3d11_quadviews_compositor_.pixel_probe;
    const std::array<uint64_t, kQuadViewsPixelProbeCount> previous_hashes = probe.previous_hashes;
    const bool has_previous_hashes = probe.has_previous_hashes;
    const int64_t previous_format = probe.format;
    for (ID3D11Texture2D*& staging_texture : probe.staging_textures) {
        SafeRelease(staging_texture);
    }
    SafeRelease(probe.completion);
    probe = {};
    if (preserve_history) {
        probe.previous_hashes = previous_hashes;
        probe.has_previous_hashes = has_previous_hashes;
        probe.format = previous_format;
    }
}

void OpenXrLayer::PollD3D11QuadViewsPixelProbe() {
    QuadViewsPixelProbe& probe = d3d11_quadviews_compositor_.pixel_probe;
    ID3D11DeviceContext* context = d3d11_quadviews_compositor_.context;
    if (!probe.issued || !probe.completion || !context) {
        return;
    }

    BOOL complete = FALSE;
    const HRESULT query_result = context->GetData(
        probe.completion, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (query_result == S_FALSE || (query_result == S_OK && !complete)) {
        return;
    }
    if (FAILED(query_result)) {
        logger_.Info("D3D11 quadviews recovery pixel-probe query failed. hr=" +
                     FormatHex(static_cast<uint32_t>(query_result)));
        probe.issued = false;
        return;
    }

    std::array<QuadViewsPixelMetrics, kQuadViewsPixelProbeCount> metrics{};
    bool all_mapped = true;
    for (uint32_t index = 0; index < kQuadViewsPixelProbeCount; ++index) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = context->Map(
            probe.staging_textures[index], 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(map_result)) {
            logger_.Info("D3D11 quadviews recovery pixel-probe map failed. probe=" +
                         std::to_string(index) +
                         ", hr=" + FormatHex(static_cast<uint32_t>(map_result)));
            all_mapped = false;
            break;
        }
        const bool analyzed = AnalyzePixelProbe(mapped,
                                                static_cast<DXGI_FORMAT>(probe.format),
                                                kQuadViewsPixelProbeSize,
                                                kQuadViewsPixelProbeSize,
                                                &metrics[index]);
        context->Unmap(probe.staging_textures[index], 0);
        if (!analyzed) {
            all_mapped = false;
            break;
        }
    }

    if (all_mapped) {
        static constexpr std::array<std::string_view, kQuadViewsPixelProbeBandsPerEye> kBandNames{
            "top", "center", "bottom"};
        std::ostringstream stream;
        stream << "D3D11 quadviews submitted-pixel probe: frameTime=" << probe.frame_time
               << ", targetGeneration=" << probe.target_generation
               << ", format=" << static_cast<uint32_t>(probe.format)
               << ", outputImageIndices=[" << probe.output_image_indices[0] << ","
               << probe.output_image_indices[1] << "]";
        for (uint32_t eye = 0; eye < 2; ++eye) {
            for (uint32_t band = 0; band < kQuadViewsPixelProbeBandsPerEye; ++band) {
                const uint32_t index = eye * kQuadViewsPixelProbeBandsPerEye + band;
                stream << ", eye" << eye << "." << kBandNames[band]
                       << "{hash=" << FormatHex(metrics[index].hash)
                       << ",changed="
                       << (!probe.has_previous_hashes || metrics[index].hash != probe.previous_hashes[index])
                       << ",mean=" << FormatDiagnosticDouble(metrics[index].mean_luma)
                       << ",stddev=" << FormatDiagnosticDouble(metrics[index].standard_deviation)
                       << ",edge=" << FormatDiagnosticDouble(metrics[index].mean_neighbor_edge)
                       << ",nearBlack=" << FormatDiagnosticDouble(metrics[index].near_black_fraction)
                       << "}";
                probe.previous_hashes[index] = metrics[index].hash;
            }
        }
        probe.has_previous_hashes = true;
        logger_.Debug(stream.str());
    }
    probe.issued = false;
}

void OpenXrLayer::RecycleD3D11QuadViewsCompositionTargets() {
    const uint64_t old_generation = d3d11_quadviews_compositor_.output_target_generation;
    const HRESULT device_removed_reason = d3d11_quadviews_compositor_.device
                                              ? d3d11_quadviews_compositor_.device->GetDeviceRemovedReason()
                                              : E_POINTER;
    logger_.Info("Quadviews compositor recovery recycle starting: oldGeneration=" +
                 std::to_string(old_generation) +
                 ", device=" + FormatHex(reinterpret_cast<uintptr_t>(d3d11_quadviews_compositor_.device)) +
                 ", deviceRemovedReason=" + FormatHex(static_cast<uint32_t>(device_removed_reason)));

    for (uint32_t eye = 0; eye < d3d11_quadviews_compositor_.targets.size(); ++eye) {
        QuadViewsCompositionTarget& target = d3d11_quadviews_compositor_.targets[eye];
        std::ostringstream stream;
        stream << "Quadviews compositor recovery retiring target: eye=" << eye
               << ", generation=" << old_generation
               << ", swapchain=" << FormatHandle(target.swapchain)
               << ", size=" << target.width << "x" << target.height
               << ", format=" << target.format
               << ", declaredImages=" << target.image_count
               << ", runtimeTextures=" << target.d3d11_images.size()
               << ", directRtvs=" << target.image_render_target_views.size()
               << ", privateTexture="
               << FormatHex(reinterpret_cast<uintptr_t>(target.render_texture));
        for (uint32_t image = 0; image < target.d3d11_images.size(); ++image) {
            D3D11_TEXTURE2D_DESC desc{};
            if (target.d3d11_images[image]) {
                target.d3d11_images[image]->GetDesc(&desc);
            }
            stream << ", runtimeTexture" << image << "="
                   << FormatHex(reinterpret_cast<uintptr_t>(target.d3d11_images[image]))
                   << " " << FormatTextureDesc(desc);
        }
        if (target.render_texture) {
            D3D11_TEXTURE2D_DESC desc{};
            target.render_texture->GetDesc(&desc);
            stream << ", private" << FormatTextureDesc(desc);
        }
        logger_.Info(stream.str());

        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            const XrResult destroy_result = DestroyLayerSwapchain(target.swapchain);
            logger_.Info("Quadviews compositor recovery destroyed output swapchain: eye=" +
                         std::to_string(eye) +
                         ", handle=" + FormatHandle(target.swapchain) +
                         ", result=" + FormatHex(static_cast<uint64_t>(destroy_result)));
        }
        target = {};
    }
    ReleaseD3D11QuadViewsPixelProbeResources(true);
    d3d11_quadviews_compositor_.output_target_generation = old_generation + 1;

    // A query issued before the headset session break may never become
    // readable. Preserve the device-level query objects but discard their
    // pending state along with the runtime-owned output swapchains.
    for (QuadViewsGpuTimingQuery& query : d3d11_quadviews_compositor_.gpu_timing_queries) {
        query.issued = false;
        query.frame_time = 0;
    }
    d3d11_quadviews_compositor_.has_logged_prewarm = false;
    d3d11_quadviews_compositor_.has_last_completed_gpu_timing = false;
    d3d11_quadviews_compositor_.failure_logs_remaining = 8;
    d3d11_quadviews_compositor_.next_gpu_timing_query = 0;
    d3d11_quadviews_compositor_.last_completed_gpu_ms = 0.0;
    d3d11_quadviews_compositor_.last_completed_gpu_frame_time = 0;
}

void OpenXrLayer::ResetD3D11QuadViewsCompositor() {
    ResetD3D11FocusSharpen();
    ReleaseD3D11QuadViewsPixelProbeResources(false);
    for (QuadViewsCompositionTarget& target : d3d11_quadviews_compositor_.targets) {
        SafeReleaseVector(target.image_render_target_views);
        SafeRelease(target.render_target_view);
        SafeRelease(target.render_texture);
        if (target.swapchain != XR_NULL_HANDLE && next_destroy_swapchain_) {
            DestroyLayerSwapchain(target.swapchain);
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
    SafeRelease(d3d11_quadviews_compositor_.layer_context_state);
    SafeRelease(d3d11_quadviews_compositor_.context1);
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
    d3d11_quadviews_compositor_.output_target_generation = 0;
    d3d11_quadviews_compositor_.last_completed_gpu_ms = 0.0;
    d3d11_quadviews_compositor_.last_completed_gpu_frame_time = 0;
}

void OpenXrLayer::NoteThread(ThreadCensus::Call call) {
    std::uint64_t count = 0;
    const std::string line = thread_census_.Note(call, static_cast<std::uint32_t>(GetCurrentThreadId()),
                                                 CurrentThreadDescription, &count);
    if (!line.empty()) {
        logger_.Info(line);
    }
    if (call == ThreadCensus::Call::EndFrame && count == kThreadCensusSummaryFrame) {
        logger_.Info("Thread census after " + std::to_string(count) + " frames: " + thread_census_.Summary());
    }
}

MotionVectorSize OpenXrLayer::SpaceWarpVectorSizeLocked() {
    // Frame Generation reads a game's vectors from stage 2 on (their capture
    // at half resolution), in a game it is enabled for, on Direct3D 12 only:
    // a Direct3D 12 game's, or a Vulkan game's through the Vulkan bridge
    // (whose backend is Direct3D 12) unless VECTORXR_FRAMEGEN_ENGINE_BRIDGE=0;
    // never through the Direct3D 11 bridge (the game's depth stays its own
    // there), on another API's backend, nor after a device was removed right
    // after reading them (the sentinel).
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    const FrameGenEngineMode mode =
        ParseFrameGenEngineMode(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV").value_or("").c_str());
    const bool bridge_reads =
        ParseFrameGenEngineBridgeMode(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_BRIDGE").value_or("")) !=
        FrameGenEngineBridgeMode::kOff;
    const bool direct3d12 =
        vulkan_enable_version_ == 0 ? d3d12_extension_enabled_ : vulkan_bridge_extensions_ && bridge_reads;
    const std::filesystem::path removed = FrameGenEngineRemovedPath();
    std::error_code ignored;
    const bool read = resolved_settings_.core.enabled && resolved_settings_.framegen.enabled &&
                      mode >= FrameGenEngineMode::kCapture && !d3d11_graphics_extension_requested_ && direct3d12 &&
                      (removed.empty() || !std::filesystem::exists(removed, ignored));
    return ChooseMotionVectorSize(read,
                                  ParseMotionVectorSize(ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_SIZE").value_or("")));
}

XrExtent2Di OpenXrLayer::SpaceWarpMotionVectorSize(XrInstance instance, XrSystemId system_id, MotionVectorSize size) {
    // The runtime's own stereo views, whatever quad views make of them.
    uint32_t count = 0;
    std::array<XrViewConfigurationView, 2> views{};
    views.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!next_enumerate_view_configuration_views_ ||
        XR_FAILED(next_enumerate_view_configuration_views_(instance, system_id,
                                                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                           static_cast<uint32_t>(views.size()), &count,
                                                           views.data())) ||
        count == 0) {
        return RecommendedMotionVectorSize(0, 0, size);
    }
    return RecommendedMotionVectorSize(views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight, size);
}

std::string OpenXrLayer::DescribeSpaceWarpSwapchainLocked(XrSwapchain swapchain) const {
    const auto found = tracked_swapchains_.find(swapchain);
    if (found == tracked_swapchains_.end()) {
        return FormatHandle(swapchain) + "(untracked)";
    }
    const SwapchainInfo& info = found->second;
    return FormatHandle(swapchain) + "(" + std::to_string(info.width) + "x" + std::to_string(info.height) +
           " format=" + std::to_string(info.format) + " samples=" + std::to_string(info.sample_count) +
           " array=" + std::to_string(info.array_size) + " usage=" + FormatUsageFlags(info.usage_flags) + ")";
}

void OpenXrLayer::NoteSpaceWarpSwapchainLocked(XrSwapchain swapchain, const SwapchainInfo& info) {
    const bool half_float = IsHalfFloatRgbaFormat(info.format);
    const bool motion_vector_size = spacewarp_motion_vector_size_.width > 0 &&
                                    info.width == static_cast<uint32_t>(spacewarp_motion_vector_size_.width) &&
                                    info.height == static_cast<uint32_t>(spacewarp_motion_vector_size_.height);
    if (!half_float && !motion_vector_size) {
        return;
    }
    logger_.Info("Space-warp probe: swapchain that looks like motion vectors or their depth: " +
                 DescribeSpaceWarpSwapchainLocked(swapchain) + ", rgba16f=" + (half_float ? "1" : "0") +
                 ", motionVectorSize=" + (motion_vector_size ? "1" : "0") + " (recommended " +
                 std::to_string(spacewarp_motion_vector_size_.width) + "x" +
                 std::to_string(spacewarp_motion_vector_size_.height) + ").");
}

const XrFrameEndInfo* OpenXrLayer::ProbeSpaceWarpFrameLocked(const XrFrameEndInfo* frame, SpaceWarpFrame& storage) {
    const XrFrameEndInfo* stripped = WithoutSpaceWarp(frame, spacewarp_fb_owned_, spacewarp_ext_owned_, storage);
    const auto now = std::chrono::steady_clock::now();
    if (spacewarp_counts_.frames++ == 0) {
        spacewarp_last_summary_ = now;
    }
    bool fb = false;
    bool ext = false;
    for (const SpaceWarpViewInfo& view : storage.found) {
        (view.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB ? fb : ext) = true;
        ++(view.stripped ? spacewarp_counts_.views_stripped : spacewarp_counts_.views_left);
        if (view.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB &&
            (view.layer_flags & XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB) != 0) {
            ++spacewarp_counts_.frame_skips;
        }
        if (view.type == XR_TYPE_FRAME_SYNTHESIS_INFO_EXT &&
            (view.layer_flags & XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT) != 0) {
            ++spacewarp_counts_.relaxed;
        }
    }
    spacewarp_counts_.frames_fb += fb ? 1 : 0;
    spacewarp_counts_.frames_ext += ext ? 1 : 0;
    if (!storage.found.empty()) {
        const SpaceWarpViewInfo& first = storage.found.front();
        spacewarp_last_view_ = DescribeSpaceWarpView(first) +
                               " mvSwapchain=" + DescribeSpaceWarpSwapchainLocked(first.motion_vector.swapchain) +
                               " depthSwapchain=" + DescribeSpaceWarpSwapchainLocked(first.depth.swapchain);
        if (!spacewarp_first_frame_logged_) {
            spacewarp_first_frame_logged_ = true;
            logger_.Info("Space-warp probe: the session's first frame with motion vectors (frame " +
                         std::to_string(spacewarp_counts_.frames) +
                         ", displayTime=" + std::to_string(frame->displayTime) + ", " +
                         std::to_string(storage.found.size()) + " structures):");
            for (const SpaceWarpViewInfo& view : storage.found) {
                logger_.Info("Space-warp probe:   " + DescribeSpaceWarpView(view) +
                             " mvSwapchain=" + DescribeSpaceWarpSwapchainLocked(view.motion_vector.swapchain) +
                             " depthSwapchain=" + DescribeSpaceWarpSwapchainLocked(view.depth.swapchain));
            }
        }
    }
    if (handshake_census_.Active()) {
        CensusFrameLocked(frame, storage);
    }
    if (now - spacewarp_last_summary_ >= std::chrono::seconds(30)) {
        spacewarp_last_summary_ = now;
        LogSpaceWarpProbeSummaryLocked("30 s");
        if (handshake_census_.Active()) {
            LogCensusSwapchainCallsLocked("30 s");
            CensusFlushProcAddrs("since the last of them");
        }
    }
    return stripped;
}

void OpenXrLayer::CensusCall() {
    if (handshake_census_.FirstApplicationCall()) {
        CensusFlushProcAddrs("before the application's first call the census watches (the loader's dispatch "
                             "table, built in xrCreateInstance, and the application's first lookups)");
    }
}

void OpenXrLayer::CensusNote(std::string_view function, const std::string& detail, bool always) {
    if (const std::optional<std::uint64_t> call = handshake_census_.Note(function, detail, always)) {
        logger_.Info("Handshake census: " + std::string(function) + " call " + std::to_string(*call) + ": " + detail);
    }
}

void OpenXrLayer::CensusProcAddr(std::string_view name, XrResult result, bool resolved) {
    handshake_census_.NoteProcAddr(name, result, resolved);
}

void OpenXrLayer::CensusFlushProcAddrs(std::string_view when) {
    if (const std::string line = handshake_census_.FlushProcAddrs(when); !line.empty()) {
        logger_.Info("Handshake census: " + line);
    }
}

void OpenXrLayer::CensusSwapchainFormats(const int64_t* formats, uint32_t count) {
    std::scoped_lock lock(mutex_);
    census_swapchain_formats_.assign(formats, formats + count);
}

bool OpenXrLayer::CensusDxgiFormats() const {
    return vulkan_enable_version_ == 0 && (d3d12_extension_enabled_ || d3d11_graphics_extension_requested_);
}

void OpenXrLayer::CensusFrameLocked(const XrFrameEndInfo* frame, const SpaceWarpFrame& storage) {
    // The swapchains the session's frames used by role, each the first time:
    // the probe's structures' (motion vectors, their depth), then each
    // projection view's color and depth. A frame that uses a new one logs
    // them all.
    const std::size_t known = census_roles_.size();
    const auto add = [this](const char* role, XrSwapchain swapchain) {
        if (swapchain != XR_NULL_HANDLE &&
            std::find(census_roles_.begin(), census_roles_.end(), std::make_pair(role, swapchain)) ==
                census_roles_.end()) {
            census_roles_.emplace_back(role, swapchain);
        }
    };
    for (const SpaceWarpViewInfo& view : storage.found) {
        add("motionVectors", view.motion_vector.swapchain);
    }
    for (const SpaceWarpViewInfo& view : storage.found) {
        add("spaceWarpDepth", view.depth.swapchain);
    }
    for (std::uint32_t i = 0; frame && frame->layers && i < frame->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = frame->layers[i];
        if (!base) {
            continue;
        }
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            for (std::uint32_t v = 0; projection->views && v < projection->viewCount; ++v) {
                add("color", projection->views[v].subImage.swapchain);
                for (auto* node = static_cast<const XrBaseInStructure*>(projection->views[v].next); node;
                     node = node->next) {
                    if (node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                        add("depth", reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(node)->subImage.swapchain);
                    }
                }
            }
        }
    }
    if (census_roles_.size() != known) {
        const bool dxgi = CensusDxgiFormats();
        for (std::size_t i = known; i < census_roles_.size(); ++i) {
            const auto& [role, swapchain] = census_roles_[i];
            CensusSwapchain census;
            census.handle = swapchain;
            census.role = role;
            const auto found = tracked_swapchains_.find(swapchain);
            if (found == tracked_swapchains_.end()) {
                census.description = "untracked";
            } else {
                const SwapchainInfo& info = found->second;
                const auto offered =
                    std::find(census_swapchain_formats_.begin(), census_swapchain_formats_.end(), info.format);
                std::ostringstream stream;
                // Where the format is in the list the application was given
                // (#1 is the runtime's preferred), "no" when not in it.
                stream << info.width << "x" << info.height << " format=" << CensusSwapchainFormat(info.format, dxgi)
                       << " offered="
                       << (census_swapchain_formats_.empty()        ? std::string("unknown")
                           : offered == census_swapchain_formats_.end() ? std::string("no")
                                                                        : "#" + std::to_string(offered -
                                                                                               census_swapchain_formats_.begin() + 1) +
                                                                              "/" +
                                                                              std::to_string(census_swapchain_formats_.size()))
                       << " samples=" << info.sample_count
                       << " array=" << info.array_size << " mips=" << info.mip_count
                       << " usage=" << FormatUsageFlags(info.usage_flags) << " createFlags=" << info.create_flags;
                census.description = stream.str();
            }
            census_swapchains_.push_back(std::move(census));
        }
        CensusUpdateSwapchainsLocked();
        logger_.Info("Handshake census: the swapchains the frames use by role (frame " +
                     std::to_string(spacewarp_counts_.frames) + "): " + DescribeCensusSwapchains(census_swapchains_));
        return;
    }
    CensusUpdateSwapchainsLocked();
}

void OpenXrLayer::CensusUpdateSwapchainsLocked() {
    for (CensusSwapchain& census : census_swapchains_) {
        if (census.destroyed) {
            continue;
        }
        if (const auto found = tracked_swapchains_.find(census.handle); found != tracked_swapchains_.end()) {
            census.images = found->second.image_count;
            census.acquires = found->second.acquire_count;
            census.waits = found->second.wait_count;
            census.releases = found->second.release_count;
        }
    }
}

void OpenXrLayer::LogCensusSwapchainCallsLocked(std::string_view reason) {
    std::vector<CensusSwapchain> shown;
    for (const CensusSwapchain& census : census_swapchains_) {
        if (census.role == "motionVectors" || census.role == "spaceWarpDepth") {
            shown.push_back(census);
        }
    }
    logger_.Info("Handshake census (" + std::string(reason) + "): motion vector and space-warp depth swapchains, " +
                 "the application's calls so far: " + DescribeCensusSwapchains(shown));
}

FrameGenEngineMotionFrame OpenXrLayer::EngineMotionFrameLocked(const XrFrameEndInfo& stripped,
                                                               const SpaceWarpFrame& storage) {
    bool has_projection = false;
    for (std::uint32_t i = 0; i < stripped.layerCount && !has_projection; ++i) {
        const XrCompositionLayerBaseHeader* base = stripped.layers ? stripped.layers[i] : nullptr;
        const auto* projection = base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
                                     ? reinterpret_cast<const XrCompositionLayerProjection*>(base)
                                     : nullptr;
        has_projection = projection && projection->viewCount > 0 && projection->views;
    }
    if (has_projection) {
        ++spacewarp_projection_frames_;
    }
    const auto swapchains = [this](XrSwapchain swapchain) -> std::optional<EngineMotionSwapchain> {
        const auto found = tracked_swapchains_.find(swapchain);
        if (found == tracked_swapchains_.end()) {
            return std::nullopt;
        }
        return EngineMotionSwapchain{found->second.width, found->second.height, found->second.array_size,
                                     found->second.format};
    };
    if (std::optional<FrameGenEngineMotionFrame> record =
            ToEngineMotionFrame(storage, stripped, spacewarp_projection_frames_, swapchains)) {
        spacewarp_counts_.invalid += record->invalid;
        return std::move(*record);
    }
    // A frame without a projection has no number of its own (never the
    // last projection frame's, whose record it would stand in for).
    FrameGenEngineMotionFrame empty;
    empty.display_time = stripped.displayTime;
    empty.projection_frame = has_projection ? spacewarp_projection_frames_ : 0;
    return empty;
}

void OpenXrLayer::LogSpaceWarpProbeSummaryLocked(std::string_view reason) {
    const SpaceWarpProbeCounts& total = spacewarp_counts_;
    const SpaceWarpProbeCounts& last = spacewarp_counts_summarized_;
    std::ostringstream stream;
    stream << "Space-warp probe summary (" << reason << "): frames=" << total.frames - last.frames
           << ", framesWithFbSpaceWarp=" << total.frames_fb - last.frames_fb
           << ", framesWithFrameSynthesis=" << total.frames_ext - last.frames_ext
           << ", viewsStripped=" << total.views_stripped - last.views_stripped
           << ", viewsLeftBehindUnknownStruct=" << total.views_left - last.views_left
           << ", frameSkipFlags=" << total.frame_skips - last.frame_skips
           << ", relaxedIntervalFlags=" << total.relaxed - last.relaxed
           << ", structuresWithProblems=" << total.invalid - last.invalid << "; session: frames=" << total.frames
           << ", framesWithFbSpaceWarp=" << total.frames_fb << ", framesWithFrameSynthesis=" << total.frames_ext
           << "; last=" << (spacewarp_last_view_.empty() ? std::string("none") : spacewarp_last_view_);
    logger_.Info(stream.str());
    spacewarp_counts_summarized_ = total;
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
           << ", deferredReleases=" << info.deferred_release_count
           << ", imageStates=" << info.image_states.Size()
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

    PollDepthAnchorToggle();

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
        const InputBinding* down = PollInputBindingsDown(resolved_settings_.depthxr_bindings.toggle_enabled_bindings);
        depthxr_binding_down_cached_ = down != nullptr;
        if (down) {
            depthxr_toggle_pressed_ = *down;
        }
    }
    const bool binding_down = depthxr_binding_down_cached_;
    if (first_poll) {
        depthxr_toggle_binding_was_down_ = binding_down;
    }
    const bool was_pressed_this_call = binding_down && !depthxr_toggle_binding_was_down_;
    depthxr_toggle_binding_was_down_ = binding_down;

    if (was_pressed_this_call) {
        depthxr_toggle_enabled_ = !depthxr_toggle_enabled_;
        depth_view_info_pending_ = true;
        depth_submission_info_pending_ = true;
        depth_submission_info_not_before_time_.reset();
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        const InputBinding binding =
            ActingBinding(depthxr_toggle_pressed_, resolved_settings_.depthxr_bindings.toggle_enabled_bindings);
        logger_.Info(std::string("Depth effects ") +
                     (depthxr_toggle_enabled_ ? "enabled" : "disabled") + " via " + BindingLabel(binding) + ".");
        SoundPlayer::Instance().PlayTransition(binding.sound,
                                               depthxr_toggle_enabled_, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    }

    return depthxr_toggle_enabled_;
#else
    return resolved_settings_.depthxr.enabled;
#endif
}

bool OpenXrLayer::PivotSnapEffectActive() const {
    return pivotxr_quick_view_active_ || pivotxr_quick_view_transitioning_ ||
           pivotxr_quick_view_transition_.active;
}

// An exclusive Snap View replaces motion; a stacked one keeps adding it.
bool OpenXrLayer::PivotMotionVisible() const {
    return ActivePivotProfile().behavior == PivotProfileBehavior::EnhancedMotion &&
           (!PivotSnapEffectActive() || pivotxr_snap_stacked_);
}

bool OpenXrLayer::PivotProfilesShareNudgeSet(size_t lhs, size_t rhs) const {
    const auto& profiles = resolved_settings_.pivotxr.profiles;
    if (lhs >= profiles.size() || rhs >= profiles.size()) {
        return false;
    }
    const std::string& left = profiles[lhs].nudge_set_id;
    return !left.empty() && left == profiles[rhs].nudge_set_id;
}

double OpenXrLayer::SelectedQuickViewTransitionSeconds() const {
    const auto& profiles = resolved_settings_.pivotxr.profiles;
    if (pivotxr_quick_view_profile_index_ >= profiles.size()) {
        return 0.0;
    }
    const auto& quick_views = profiles[pivotxr_quick_view_profile_index_].view_controls.quick_views;
    return pivotxr_quick_view_index_ < quick_views.size()
               ? quick_views[pivotxr_quick_view_index_].transition_seconds
               : 0.0;
}

// Position Assist moves the view in a gravity-aligned frame that faces what
// the user is assisted around: the seated origin's heading, or the neutral
// heading of a stacked Snap View while motion is measured from its reference.
// A captured origin's pitch and roll never tilt the assist directions.
double OpenXrLayer::PivotPositionAssistYawRadians() const {
    double heading = pivotxr_origin_ ? pivotxr_origin_->yaw_radians : 0.0;
    if (pivotxr_snap_stacked_ && pivotxr_snap_reference_valid_) {
        heading += pivotxr_snap_reference_yaw_radians_ + pivotxr_quick_view_transition_.target.yaw_radians;
    }
    return heading;
}

void OpenXrLayer::ResetPivotMotionState() {
    pivotxr_smoothed_extra_yaw_radians_ = 0.0;
    pivotxr_smoothed_extra_pitch_radians_ = 0.0;
    pivotxr_position_offset_ = {};
    pivotxr_yaw_step_ = 0;
    pivotxr_pitch_step_ = 0;
    pivotxr_yaw_step_glide_ = {};
    pivotxr_pitch_step_glide_ = {};
}

void OpenXrLayer::ClearPivotSnapState() {
    pivotxr_snap_stacked_ = false;
    pivotxr_snap_reference_pending_ = false;
    pivotxr_snap_reference_valid_ = false;
    pivotxr_snap_return_pending_ = false;
    pivotxr_quick_view_return_engaged_ = false;
}

// The eased motion contribution, whether or not a snap hides it, with the
// Position Assist offset re-expressed in the Snap View offset frame (the seated
// origin's full orientation) so it can be folded into a snap offset.
PivotViewOffset OpenXrLayer::PivotMotionOffset() const {
    const double gain = SmoothStep(pivotxr_activation_gain_);
    const XrQuaternionf assist_heading = YawQuaternion(static_cast<float>(PivotPositionAssistYawRadians()));
    const XrVector3f assist = RotateVector(assist_heading, PivotOffsetTranslation(pivotxr_position_offset_, gain));
    const XrQuaternionf snap_frame =
        pivotxr_origin_ ? pivotxr_origin_->pose.orientation : IdentityPose().orientation;
    PivotViewOffset offset = PivotOffsetFromTranslation(RotateVector(ConjugateQuaternion(snap_frame), assist));
    offset.yaw_radians = pivotxr_smoothed_extra_yaw_radians_ * gain;
    offset.pitch_radians = pivotxr_smoothed_extra_pitch_radians_ * gain;
    return offset;
}

// The motion contribution exactly as it is composed into the view right now.
PivotViewOffset OpenXrLayer::ComposedPivotMotionOffset() const {
    return PivotMotionVisible() ? PivotMotionOffset() : PivotViewOffset{};
}

// Motion folded into a snap entered from normal motion. Normal motion clamps
// the composed yaw to +/-pi and pitch to +/-85 degrees while a snap wraps yaw;
// folding the clamped value keeps the view unchanged and lets the snap
// transition leave the pitch clamp immediately instead of stalling on it.
PivotViewOffset OpenXrLayer::CapturePivotMotionForSnapEntry() const {
    PivotViewOffset motion = ComposedPivotMotionOffset();
    if (PivotSnapEffectActive()) {
        return motion;
    }
    const double nudge_yaw = pivotxr_manual_view_transition_.current.yaw_radians +
                             pivotxr_profile_view_transition_.current.yaw_radians;
    const double nudge_pitch = pivotxr_manual_view_transition_.current.pitch_radians +
                               pivotxr_profile_view_transition_.current.pitch_radians;
    const double max_pitch = DegreesToRadians(kPivotMaxPitchDegrees);
    motion.yaw_radians = std::clamp(nudge_yaw + motion.yaw_radians, -kPi, kPi) - nudge_yaw;
    motion.pitch_radians = std::clamp(nudge_pitch + motion.pitch_radians, -max_pitch, max_pitch) - nudge_pitch;
    return motion;
}

// Moves the motion on screen into the snap offset and restarts motion from
// zero, leaving the composed view unchanged. Unless a new snap pose is about
// to be targeted, the folded amount then eases back out toward the snap's own
// target, which never moves.
void OpenXrLayer::FoldPivotMotionIntoSnap(double settle_seconds) {
    PivotViewTransitionState& transition = pivotxr_quick_view_transition_;
    AddPivotViewOffset(transition.current, ComposedPivotMotionOffset());
    ResetPivotMotionState();
    if (!pivotxr_quick_view_retarget_pending_) {
        const double remaining_seconds =
            transition.active ? std::max(0.0, transition.duration_seconds - transition.elapsed_seconds)
                              : settle_seconds;
        RetargetPivotViewTransition(NearestEquivalentPivotTarget(transition.current, transition.target),
                                    remaining_seconds, transition);
    }
}

void OpenXrLayer::EngagePivotMotionProfile(size_t index) {
    const PivotXrResolvedProfile& profile = resolved_settings_.pivotxr.profiles[index];
    if (pivotxr_quick_view_active_ && profile.apply_motion_to_snap_views) {
        if (!pivotxr_snap_stacked_) {
            // The exclusive snap shows only its own pose: motion frozen for its
            // return is dropped and stacked motion starts around this pose.
            ResetPivotMotionState();
            pivotxr_snap_stacked_ = true;
            pivotxr_snap_reference_pending_ = true;
        } else if (!pivotxr_engaged_ && SmoothStep(pivotxr_activation_gain_) <= kPivotActivationGainEpsilon) {
            // Stacked motion had fully eased out; measure it from here again.
            ResetPivotMotionState();
            pivotxr_snap_reference_pending_ = true;
        }
        // Otherwise a profile switch or a quick re-engage keeps the snap's
        // reference, so the snap centre never moves.
    } else if (pivotxr_quick_view_active_ && pivotxr_snap_stacked_) {
        // A profile that did not opt in keeps the snap exclusive, as Snap
        // Views always behaved; the stacked motion eases out of the snap.
        FoldPivotMotionIntoSnap(SelectedQuickViewTransitionSeconds());
        pivotxr_snap_stacked_ = false;
        pivotxr_snap_reference_pending_ = false;
        pivotxr_snap_reference_valid_ = false;
    }
    // Only the engaged profile can hold a toggle latch, as with Snap Views.
    // A latch left on a profile this one replaced would make that profile's
    // Hold binding act as a toggle the next time it engages.
    for (size_t other = 0; other < pivotxr_profile_input_states_.size(); ++other) {
        if (other != index) pivotxr_profile_input_states_[other].toggle_latched = false;
    }
    pivotxr_active_profile_index_ = index;
    pivotxr_engaged_ = true;
}

void OpenXrLayer::ActivatePivotQuickView(size_t profile_index, size_t view_index) {
    const PivotQuickView& view =
        resolved_settings_.pivotxr.profiles[profile_index].view_controls.quick_views[view_index];
    const bool existing = pivotxr_quick_view_active_ || pivotxr_quick_view_transitioning_;
    // Profile-following nudges belong to the selected snap, otherwise to the
    // motion profile.
    const size_t nudge_owner =
        pivotxr_quick_view_active_ ? pivotxr_quick_view_profile_index_ : pivotxr_active_profile_index_;
    const PivotXrResolvedProfile& motion = ActivePivotProfile();
    const bool stack = (existing && pivotxr_snap_stacked_) ||
                       (motion.behavior == PivotProfileBehavior::EnhancedMotion &&
                        motion.apply_motion_to_snap_views && pivotxr_engaged_);
    PivotViewTransitionState& transition = pivotxr_quick_view_transition_;
    if (!existing) {
        pivotxr_quick_view_return_profile_index_ = pivotxr_active_profile_index_;
        pivotxr_quick_view_return_engaged_ = pivotxr_engaged_;
        transition = {};
        transition.current = CapturePivotMotionForSnapEntry();
        transition.target = transition.current;
        // The snap now carries the motion on screen. Motion that is not handed
        // back on return (stacked, or not engaged) restarts from zero.
        if (stack || !pivotxr_quick_view_return_engaged_) {
            ResetPivotMotionState();
        }
    } else if (stack) {
        AddPivotViewOffset(transition.current, ComposedPivotMotionOffset());
        ResetPivotMotionState();
    }

    if (stack) {
        pivotxr_snap_stacked_ = true;
        pivotxr_snap_reference_pending_ = true;
    } else {
        // An exclusive snap selects its own profile until it is released.
        if (pivotxr_active_profile_index_ != profile_index) {
            pivotxr_engaged_ = false;
            pivotxr_active_profile_index_ = profile_index;
        }
        pivotxr_snap_stacked_ = false;
        pivotxr_snap_reference_pending_ = false;
        pivotxr_snap_reference_valid_ = false;
    }
    pivotxr_snap_return_pending_ = false;
    if (nudge_owner != profile_index && !PivotProfilesShareNudgeSet(nudge_owner, profile_index)) {
        RetargetPivotViewTransition({}, view.transition_seconds, pivotxr_profile_view_transition_);
    }

    pivotxr_quick_view_pending_pose_ = {
        DegreesToRadians(-std::clamp(view.yaw_degrees, -180.0, 180.0)),
        DegreesToRadians(std::clamp(view.pitch_degrees, -kPivotMaxPitchDegrees, kPivotMaxPitchDegrees)),
        view.position_right_cm / 100.0,
        view.position_up_cm / 100.0,
        view.position_forward_cm / 100.0,
    };
    pivotxr_quick_view_pending_duration_seconds_ = view.transition_seconds;
    pivotxr_quick_view_retarget_pending_ = true;
    pivotxr_quick_view_active_ = true;
    pivotxr_quick_view_transitioning_ = true;
    pivotxr_quick_view_profile_index_ = profile_index;
    pivotxr_quick_view_index_ = view_index;
}

void OpenXrLayer::ReleasePivotQuickView(double duration_seconds) {
    const auto& profiles = resolved_settings_.pivotxr.profiles;
    const size_t snap_owner = pivotxr_quick_view_profile_index_;
    PivotViewTransitionState& transition = pivotxr_quick_view_transition_;
    pivotxr_quick_view_retarget_pending_ = false;
    PivotViewOffset target;
    if (pivotxr_snap_stacked_) {
        // Motion stays with its profile; the next drive hands it back from the
        // snap's reference to the seated origin.
        pivotxr_snap_return_pending_ = true;
    } else {
        // Return to the state from before the snap, handing off at the motion
        // transform frozen for it.
        const size_t return_index = pivotxr_quick_view_return_profile_index_;
        if (pivotxr_quick_view_return_engaged_ && return_index < profiles.size() &&
            profiles[return_index].behavior == PivotProfileBehavior::EnhancedMotion) {
            const double max_pitch = DegreesToRadians(kPivotMaxPitchDegrees);
            target = PivotMotionOffset();
            target.yaw_radians = std::clamp(target.yaw_radians, -kPi, kPi);
            target.pitch_radians = std::clamp(target.pitch_radians, -max_pitch, max_pitch);
        }
        pivotxr_active_profile_index_ = return_index;
        pivotxr_engaged_ = pivotxr_quick_view_return_engaged_;
    }
    RetargetPivotViewTransition(NearestEquivalentPivotTarget(transition.current, target), duration_seconds,
                                transition);
    pivotxr_quick_view_active_ = false;
    // The drive ends the snap once the return lands, even an instant one, so
    // locates for the current displayTime keep its transform.
    pivotxr_quick_view_transitioning_ = true;
    // Profile-following nudges carry over when the snap shares its Nudge Set
    // with the engaged motion it returns to; otherwise they ease out.
    const bool returns_to_engaged_motion =
        pivotxr_engaged_ && ActivePivotProfile().behavior == PivotProfileBehavior::EnhancedMotion;
    if (!returns_to_engaged_motion || !PivotProfilesShareNudgeSet(snap_owner, pivotxr_active_profile_index_)) {
        RetargetPivotViewTransition({}, duration_seconds, pivotxr_profile_view_transition_);
    }
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
    // RefreshResolvedSettings resets input state whenever the candidate set
    // changes, so a size mismatch only happens right after such a reset.
    if (pivotxr_profile_input_states_.size() != pivot.profiles.size()) {
        pivotxr_profile_input_states_.assign(pivot.profiles.size(), PivotProfileInputState{});
        pivotxr_active_profile_index_ = std::min(pivotxr_active_profile_index_, pivot.profiles.size() - 1);
    }

    for (size_t i = 0; i < pivot.profiles.size(); ++i) {
        PivotProfileInputState& state = pivotxr_profile_input_states_[i];
        if (state.activation.size() != pivot.profiles[i].activation_bindings.size()) {
            state.activation.assign(pivot.profiles[i].activation_bindings.size(), {});
        }
        if (state.set_origin.size() != pivot.profiles[i].set_origin_bindings.size()) {
            state.set_origin.assign(pivot.profiles[i].set_origin_bindings.size(), {});
        }
        if (state.release_origin.size() != pivot.profiles[i].release_origin_bindings.size()) {
            state.release_origin.assign(pivot.profiles[i].release_origin_bindings.size(), {});
        }
        const PivotNudgeSettings& nudges = pivot.profiles[i].view_controls.nudges;
        if (state.nudge_yaw_left.size() != nudges.yaw_left_bindings.size())
            state.nudge_yaw_left.assign(nudges.yaw_left_bindings.size(), {});
        if (state.nudge_yaw_right.size() != nudges.yaw_right_bindings.size())
            state.nudge_yaw_right.assign(nudges.yaw_right_bindings.size(), {});
        if (state.nudge_pitch_up.size() != nudges.pitch_up_bindings.size())
            state.nudge_pitch_up.assign(nudges.pitch_up_bindings.size(), {});
        if (state.nudge_pitch_down.size() != nudges.pitch_down_bindings.size())
            state.nudge_pitch_down.assign(nudges.pitch_down_bindings.size(), {});
        if (state.nudge_center.size() != nudges.center_bindings.size())
            state.nudge_center.assign(nudges.center_bindings.size(), {});
        const auto& quick_views = pivot.profiles[i].view_controls.quick_views;
        if (state.quick_views.size() != quick_views.size()) {
            state.quick_views.assign(quick_views.size(), {});
        }
        for (size_t view = 0; view < quick_views.size(); ++view) {
            if (state.quick_views[view].activation.size() != quick_views[view].activation_bindings.size()) {
                state.quick_views[view].activation.assign(quick_views[view].activation_bindings.size(), {});
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    // Prime the edge detectors on the first poll after a reset so a button held
    // from the bind gesture does not register as a spurious activation press.
    const bool first_poll = !pivotxr_binding_last_poll_time_.has_value();
    if (first_poll || now - *pivotxr_binding_last_poll_time_ >= kInputBindingPollInterval) {
        pivotxr_binding_last_poll_time_ = now;
        for (size_t i = 0; i < pivot.profiles.size(); ++i) {
            const PivotXrResolvedProfile& profile = pivot.profiles[i];
            PivotProfileInputState& state = pivotxr_profile_input_states_[i];
            for (size_t binding = 0; binding < profile.activation_bindings.size(); ++binding) {
                state.activation[binding].down_cached = PollInputBindingDown(profile.activation_bindings[binding].binding);
            }
            for (size_t binding = 0; binding < profile.set_origin_bindings.size(); ++binding) {
                state.set_origin[binding].down_cached = PollInputBindingDown(profile.set_origin_bindings[binding]);
            }
            for (size_t binding = 0; binding < profile.release_origin_bindings.size(); ++binding) {
                state.release_origin[binding].down_cached = PollInputBindingDown(profile.release_origin_bindings[binding]);
            }
            const PivotNudgeSettings& nudges = profile.view_controls.nudges;
            auto poll_bindings = [&](const std::vector<InputBinding>& bindings,
                                     std::vector<PivotProfileInputState::BindingState>& states) {
                for (size_t binding = 0; binding < bindings.size(); ++binding) {
                    states[binding].down_cached = PollInputBindingDown(bindings[binding]);
                }
            };
            poll_bindings(nudges.yaw_left_bindings, state.nudge_yaw_left);
            poll_bindings(nudges.yaw_right_bindings, state.nudge_yaw_right);
            poll_bindings(nudges.pitch_up_bindings, state.nudge_pitch_up);
            poll_bindings(nudges.pitch_down_bindings, state.nudge_pitch_down);
            poll_bindings(nudges.center_bindings, state.nudge_center);
            for (size_t view = 0; view < profile.view_controls.quick_views.size(); ++view) {
                const PivotQuickView& quick_view = profile.view_controls.quick_views[view];
                auto& view_state = state.quick_views[view];
                for (size_t binding = 0; binding < quick_view.activation_bindings.size(); ++binding) {
                    view_state.activation[binding].down_cached =
                        PollInputBindingDown(quick_view.activation_bindings[binding].binding);
                }
            }
        }
    }

    struct BindingTransitions {
        const InputBinding* pressed{nullptr};
        const InputBinding* released{nullptr};
        bool any_down{false};
    };
    auto consume_transitions = [](const std::vector<InputBinding>& bindings,
                                  std::vector<PivotProfileInputState::BindingState>& states) {
        BindingTransitions transitions;
        for (size_t index = 0; index < bindings.size(); ++index) {
            PivotProfileInputState::BindingState& state = states[index];
            if (state.down_cached && !state.was_down && !transitions.pressed) {
                transitions.pressed = &bindings[index];
            }
            if (!state.down_cached && state.was_down && !transitions.released) {
                transitions.released = &bindings[index];
            }
            transitions.any_down = transitions.any_down || state.down_cached;
            state.was_down = state.down_cached;
        }
        return transitions;
    };
    auto prime_states = [](std::vector<PivotProfileInputState::BindingState>& states) {
        for (PivotProfileInputState::BindingState& state : states) {
            state.was_down = state.down_cached;
        }
    };
    auto note_transition_diagnostics = [&]() {
        pending_locate_views_diagnostics_ = 5;
        pending_end_frame_diagnostics_ = 5;
        pending_pivot_diagnostics_ = kPivotDiagnosticBurstCount;
    };
    // While a Snap View is selected its profile owns the profile-following
    // nudges, so motion profile changes leave them alone.
    auto release_motion_profile_nudges = [&](size_t from_index, size_t to_index) {
        if (!pivotxr_quick_view_active_ && from_index != to_index &&
            !PivotProfilesShareNudgeSet(from_index, to_index)) {
            pivotxr_profile_view_transition_ = {};
        }
    };
    auto engage = [&](size_t index, const InputBinding& trigger) {
        const PivotXrResolvedProfile& profile = pivot.profiles[index];
        const bool switching = pivotxr_engaged_ && pivotxr_active_profile_index_ != index;
        release_motion_profile_nudges(pivotxr_active_profile_index_, index);
        EngagePivotMotionProfile(index);
        note_transition_diagnostics();
        logger_.Info("PivotXR profile '" + profile.name + "' engaged via " +
                     BindingLabel(trigger) + (switching ? " (switched profile)." : "."));
        SoundPlayer::Instance().PlayTransition(trigger.sound, true, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    auto disengage = [&](size_t index, const InputBinding& trigger) {
        const PivotXrResolvedProfile& profile = pivot.profiles[index];
        pivotxr_engaged_ = false;
        if (!pivotxr_quick_view_active_) {
            pivotxr_profile_view_transition_ = {};
        }
        note_transition_diagnostics();
        logger_.Info("PivotXR profile '" + profile.name + "' disengaged via " + BindingLabel(trigger) + ".");
        SoundPlayer::Instance().PlayTransition(trigger.sound, false, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    auto apply_nudge = [&](size_t index, const InputBinding& trigger,
                           double yaw_delta, double pitch_delta, bool center) {
        const PivotXrResolvedProfile& profile = pivot.profiles[index];
        const double duration = profile.view_controls.nudges.transition_seconds;
        if (center) {
            RetargetPivotViewTransition({}, duration, pivotxr_manual_view_transition_);
            RetargetPivotViewTransition({}, duration, pivotxr_profile_view_transition_);
        } else {
            PivotViewTransitionState& transition = profile.allow_inactive_nudges
                ? pivotxr_manual_view_transition_
                : pivotxr_profile_view_transition_;
            PivotViewOffset target = transition.target;
            target.yaw_radians = std::clamp(target.yaw_radians + yaw_delta, -kPi, kPi);
            target.pitch_radians = std::clamp(target.pitch_radians + pitch_delta,
                                              DegreesToRadians(-kPivotMaxPitchDegrees),
                                              DegreesToRadians(kPivotMaxPitchDegrees));
            RetargetPivotViewTransition(target, duration, transition);
        }
        note_transition_diagnostics();
        logger_.Info("PivotXR profile '" + pivot.profiles[index].name + "' " +
                     (center ? "manual view offset centered" : "view nudged") +
                     " via " + BindingLabel(trigger) + ".");
        SoundPlayer::Instance().PlayTransition(trigger.sound, true, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    auto activate_quick_view = [&](size_t profile_index, size_t view_index,
                                   const PivotActivationBinding& trigger) {
        const PivotQuickView& quick_view = pivot.profiles[profile_index].view_controls.quick_views[view_index];
        ActivatePivotQuickView(profile_index, view_index);
        note_transition_diagnostics();
        logger_.Info("PivotXR Quick View '" + quick_view.name + "' activated via " +
                     BindingLabel(trigger.binding) + ".");
        SoundPlayer::Instance().PlayTransition(trigger.binding.sound, true, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    auto deactivate_quick_view = [&](const PivotQuickView& quick_view, const InputBinding& trigger) {
        ReleasePivotQuickView(quick_view.transition_seconds);
        note_transition_diagnostics();
        logger_.Info("PivotXR Quick View '" + quick_view.name + "' released via " +
                     BindingLabel(trigger) + ".");
        SoundPlayer::Instance().PlayTransition(trigger.sound, false, dll_directory_,
                                               resolved_settings_.core.sound_volume);
    };
    // This runs on every xrLocateSpace and xrLocateViews, under mutex_: the
    // claim lists reuse their storage instead of allocating per call.
    std::vector<const InputBinding*>& claimed_view_inputs = pivotxr_claimed_view_inputs_scratch_;
    std::vector<std::string_view>& polled_nudge_sets = pivotxr_polled_nudge_sets_scratch_;
    claimed_view_inputs.clear();
    polled_nudge_sets.clear();
    auto claim_view_input = [&](const InputBinding& binding) {
        if (std::any_of(claimed_view_inputs.begin(), claimed_view_inputs.end(),
                        [&](const InputBinding* claimed) { return SameInputBinding(*claimed, binding); })) {
            return false;
        }
        claimed_view_inputs.push_back(&binding);
        return true;
    };

    // Arbitrate across all candidates: last pressed wins. The resolver prunes
    // colliding activation bindings while retaining every independent binding.
    // The engaged profile reads its input first, then the others in order.
    const size_t first_profile = pivotxr_active_profile_index_;
    for (size_t step = 0; step < pivot.profiles.size(); ++step) {
        const size_t i = first_profile >= pivot.profiles.size() ? step
                         : step == 0                            ? first_profile
                         : step <= first_profile                ? step - 1
                                                                : step;
        const PivotXrResolvedProfile& profile = pivot.profiles[i];
        PivotProfileInputState& input_state = pivotxr_profile_input_states_[i];
        if (first_poll) {
            prime_states(input_state.activation);
            prime_states(input_state.set_origin);
            prime_states(input_state.release_origin);
            prime_states(input_state.nudge_yaw_left);
            prime_states(input_state.nudge_yaw_right);
            prime_states(input_state.nudge_pitch_up);
            prime_states(input_state.nudge_pitch_down);
            prime_states(input_state.nudge_center);
            for (auto& quick_view : input_state.quick_views) {
                prime_states(quick_view.activation);
            }
            continue;
        }

        const BindingTransitions set_origin =
            consume_transitions(profile.set_origin_bindings, input_state.set_origin);
        if (set_origin.pressed) {
            pivotxr_origin_capture_pending_ = true;
            pivotxr_manual_view_transition_ = {};
            pivotxr_profile_view_transition_ = {};
            pivotxr_quick_view_transition_ = {};
            ClearPivotSnapState();
            pivotxr_quick_view_retarget_pending_ = false;
            pivotxr_quick_view_active_ = false;
            pivotxr_quick_view_transitioning_ = false;
            logger_.Info("PivotXR origin capture requested via " + BindingLabel(*set_origin.pressed) + ".");
            SoundPlayer::Instance().PlayTransition(set_origin.pressed->sound, true, dll_directory_,
                                                   resolved_settings_.core.sound_volume,
                                                   L"origin-set.wav", L"origin-set.wav");
        }

        const BindingTransitions release_origin =
            consume_transitions(profile.release_origin_bindings, input_state.release_origin);
        if (release_origin.pressed && (pivotxr_origin_.has_value() || pivotxr_origin_capture_pending_)) {
            pivotxr_origin_.reset();
            pivotxr_origin_capture_pending_ = false;
            pivotxr_translation_anchor_.reset();
            pivotxr_manual_view_transition_ = {};
            pivotxr_profile_view_transition_ = {};
            pivotxr_quick_view_transition_ = {};
            ClearPivotSnapState();
            pivotxr_quick_view_retarget_pending_ = false;
            pivotxr_quick_view_active_ = false;
            pivotxr_quick_view_transitioning_ = false;
            logger_.Info("PivotXR origin released via " + BindingLabel(*release_origin.pressed) +
                         "; reverting to the HMD origin.");
            SoundPlayer::Instance().PlayTransition(release_origin.pressed->sound, true, dll_directory_,
                                                   resolved_settings_.core.sound_volume,
                                                   L"origin-release.wav", L"origin-release.wav");
        }

        const PivotNudgeSettings& nudges = profile.view_controls.nudges;
        const BindingTransitions yaw_left = consume_transitions(nudges.yaw_left_bindings, input_state.nudge_yaw_left);
        const BindingTransitions yaw_right = consume_transitions(nudges.yaw_right_bindings, input_state.nudge_yaw_right);
        const BindingTransitions pitch_up = consume_transitions(nudges.pitch_up_bindings, input_state.nudge_pitch_up);
        const BindingTransitions pitch_down = consume_transitions(nudges.pitch_down_bindings, input_state.nudge_pitch_down);
        const BindingTransitions center = consume_transitions(nudges.center_bindings, input_state.nudge_center);
        // A shared Nudge Set is polled once, by the profile it currently
        // follows: the selected snap's profile, else the engaged motion
        // profile, or any profile whose nudges work while inactive.
        const bool following_owner = pivotxr_quick_view_active_
                                         ? pivotxr_quick_view_profile_index_ == i
                                         : pivotxr_engaged_ && pivotxr_active_profile_index_ == i;
        bool nudge_set_input_owner = following_owner || profile.allow_inactive_nudges;
        if (nudge_set_input_owner && !profile.nudge_set_id.empty()) {
            nudge_set_input_owner =
                std::find(polled_nudge_sets.begin(), polled_nudge_sets.end(),
                          std::string_view(profile.nudge_set_id)) == polled_nudge_sets.end();
            if (nudge_set_input_owner) polled_nudge_sets.push_back(profile.nudge_set_id);
        }
        const bool behavior_active =
            (profile.behavior == PivotProfileBehavior::EnhancedMotion &&
             !pivotxr_quick_view_active_ && pivotxr_engaged_ && pivotxr_active_profile_index_ == i) ||
            (profile.behavior == PivotProfileBehavior::SnapViews &&
             pivotxr_quick_view_active_ && pivotxr_quick_view_profile_index_ == i);
        const bool directional_nudges_enabled =
            nudge_set_input_owner && (behavior_active || profile.allow_inactive_nudges);
        if (directional_nudges_enabled && yaw_left.pressed && claim_view_input(*yaw_left.pressed)) apply_nudge(i, *yaw_left.pressed, DegreesToRadians(nudges.yaw_step_degrees), 0.0, false);
        if (directional_nudges_enabled && yaw_right.pressed && claim_view_input(*yaw_right.pressed)) apply_nudge(i, *yaw_right.pressed, -DegreesToRadians(nudges.yaw_step_degrees), 0.0, false);
        if (directional_nudges_enabled && pitch_up.pressed && claim_view_input(*pitch_up.pressed)) apply_nudge(i, *pitch_up.pressed, 0.0, DegreesToRadians(nudges.pitch_step_degrees), false);
        if (directional_nudges_enabled && pitch_down.pressed && claim_view_input(*pitch_down.pressed)) apply_nudge(i, *pitch_down.pressed, 0.0, -DegreesToRadians(nudges.pitch_step_degrees), false);
        if (nudge_set_input_owner && center.pressed && claim_view_input(*center.pressed)) apply_nudge(i, *center.pressed, 0.0, 0.0, true);

        if (profile.behavior != PivotProfileBehavior::EnhancedMotion) for (size_t view_index = 0; view_index < profile.view_controls.quick_views.size(); ++view_index) {
            const PivotQuickView& quick_view = profile.view_controls.quick_views[view_index];
            auto& view_state = input_state.quick_views[view_index];
            const PivotActivationBinding* toggle_pressed = nullptr;
            const PivotActivationBinding* hold_pressed = nullptr;
            const PivotActivationBinding* hold_released = nullptr;
            bool any_hold_down = false;
            for (size_t binding = 0; binding < quick_view.activation_bindings.size(); ++binding) {
                const PivotActivationBinding& activation = quick_view.activation_bindings[binding];
                auto& binding_state = view_state.activation[binding];
                const bool pressed = binding_state.down_cached && !binding_state.was_down;
                const bool released = !binding_state.down_cached && binding_state.was_down;
                if (activation.behavior == PivotActivationBehavior::Toggle) {
                    if (pressed && !toggle_pressed) toggle_pressed = &activation;
                } else {
                    if (pressed && !hold_pressed) hold_pressed = &activation;
                    if (released && !hold_released) hold_released = &activation;
                    any_hold_down = any_hold_down || binding_state.down_cached;
                }
                binding_state.was_down = binding_state.down_cached;
            }
            const bool this_view_active = pivotxr_quick_view_active_ &&
                pivotxr_quick_view_profile_index_ == i && pivotxr_quick_view_index_ == view_index;
            if (toggle_pressed && claim_view_input(toggle_pressed->binding)) {
                if (this_view_active && view_state.toggle_latched) {
                    view_state.toggle_latched = false;
                    deactivate_quick_view(quick_view, toggle_pressed->binding);
                } else {
                    for (auto& profile_state : pivotxr_profile_input_states_)
                        for (auto& candidate : profile_state.quick_views) candidate.toggle_latched = false;
                    view_state.toggle_latched = true;
                    activate_quick_view(i, view_index, *toggle_pressed);
                }
            }
            if (hold_pressed && claim_view_input(hold_pressed->binding)) {
                activate_quick_view(i, view_index, *hold_pressed);
            }
            const bool active_after_press = pivotxr_quick_view_active_ &&
                pivotxr_quick_view_profile_index_ == i && pivotxr_quick_view_index_ == view_index;
            if (hold_released && !any_hold_down && !view_state.toggle_latched && active_after_press) {
                deactivate_quick_view(quick_view, hold_released->binding);
            }
        }

        const PivotActivationBinding* toggle_pressed = nullptr;
        const PivotActivationBinding* hold_pressed = nullptr;
        const PivotActivationBinding* hold_released = nullptr;
        bool any_hold_down = false;
        for (size_t index = 0; index < profile.activation_bindings.size(); ++index) {
            const PivotActivationBinding& activation = profile.activation_bindings[index];
            PivotProfileInputState::BindingState& binding_state = input_state.activation[index];
            const bool pressed = binding_state.down_cached && !binding_state.was_down;
            const bool released = !binding_state.down_cached && binding_state.was_down;
            if (activation.behavior == PivotActivationBehavior::Toggle) {
                if (pressed && !toggle_pressed) toggle_pressed = &activation;
            } else {
                if (pressed && !hold_pressed) hold_pressed = &activation;
                if (released && !hold_released) hold_released = &activation;
                any_hold_down = any_hold_down || binding_state.down_cached;
            }
            binding_state.was_down = binding_state.down_cached;
        }

        // Snap View profiles have no motion to engage.
        if (profile.behavior != PivotProfileBehavior::EnhancedMotion) continue;
        bool is_engaged_profile = pivotxr_engaged_ && pivotxr_active_profile_index_ == i;
        if (profile.always_active) {
            if (toggle_pressed && claim_view_input(toggle_pressed->binding)) {
                input_state.toggle_suspended = !input_state.toggle_suspended;
                if (input_state.toggle_suspended) {
                    logger_.Info("PivotXR always-active profile '" + profile.name + "' suspended.");
                    if (is_engaged_profile) disengage(i, toggle_pressed->binding);
                } else {
                    logger_.Info("PivotXR always-active profile '" + profile.name + "' resumed.");
                    if (!any_hold_down) engage(i, toggle_pressed->binding);
                }
                is_engaged_profile = pivotxr_engaged_ && pivotxr_active_profile_index_ == i;
            }
            if (hold_pressed && is_engaged_profile && claim_view_input(hold_pressed->binding)) {
                disengage(i, hold_pressed->binding);
                is_engaged_profile = false;
            }
            if (hold_released && !any_hold_down && !input_state.toggle_suspended && !is_engaged_profile) {
                engage(i, hold_released->binding);
            }
        } else {
            if (toggle_pressed && claim_view_input(toggle_pressed->binding)) {
                if (is_engaged_profile && input_state.toggle_latched) {
                    input_state.toggle_latched = false;
                    if (!any_hold_down) disengage(i, toggle_pressed->binding);
                } else {
                    input_state.toggle_latched = true;
                    if (!is_engaged_profile) engage(i, toggle_pressed->binding);
                }
                is_engaged_profile = pivotxr_engaged_ && pivotxr_active_profile_index_ == i;
            }
            if (hold_pressed && !is_engaged_profile && claim_view_input(hold_pressed->binding)) {
                engage(i, hold_pressed->binding);
                is_engaged_profile = true;
            }
            if (hold_released && !any_hold_down && !input_state.toggle_latched && is_engaged_profile) {
                disengage(i, hold_released->binding);
            }
        }
    }
    const bool manual_view_active = pivotxr_manual_view_transition_.active ||
                                    pivotxr_profile_view_transition_.active ||
                                    !PivotViewOffsetNearlyZero(pivotxr_manual_view_transition_.current) ||
                                    !PivotViewOffsetNearlyZero(pivotxr_manual_view_transition_.target) ||
                                    !PivotViewOffsetNearlyZero(pivotxr_profile_view_transition_.current) ||
                                    !PivotViewOffsetNearlyZero(pivotxr_profile_view_transition_.target);
    const bool view_controls_active = manual_view_active || PivotSnapEffectActive();

    // Automatic engagement: when nothing is engaged, the first non-suspended
    // always-on candidate takes over. Silent (no binding sound) because this is
    // not a user action — it fires at session start and whenever another
    // profile releases the pivot. While view controls are active only the
    // current profile may resume, except that an opted-in profile may stack on
    // a snap that already stacks motion; an exclusive snap keeps the state it
    // will restore on release.
    if (!pivotxr_engaged_) {
        for (size_t i = 0; i < pivot.profiles.size(); ++i) {
            const PivotXrResolvedProfile& profile = pivot.profiles[i];
            if (profile.behavior != PivotProfileBehavior::EnhancedMotion) continue;
            if (view_controls_active && pivotxr_active_profile_index_ != i &&
                !(pivotxr_snap_stacked_ && pivotxr_quick_view_active_ && profile.apply_motion_to_snap_views)) {
                continue;
            }
            const PivotProfileInputState& input_state = pivotxr_profile_input_states_[i];
            bool hold_suspended = false;
            for (size_t index = 0; index < profile.activation_bindings.size(); ++index) {
                hold_suspended = hold_suspended ||
                    (profile.activation_bindings[index].behavior == PivotActivationBehavior::Hold &&
                     input_state.activation[index].down_cached);
            }
            if (!profile.always_active || input_state.toggle_suspended || hold_suspended) continue;
            release_motion_profile_nudges(pivotxr_active_profile_index_, i);
            EngagePivotMotionProfile(i);
            note_transition_diagnostics();
            logger_.Info("PivotXR profile '" + profile.name + "' engaged (always active).");
            break;
        }
    }

    return pivotxr_engaged_ || view_controls_active;
#else
    return pivot.enabled;
#endif
}

std::string OpenXrLayer::FrameGenStatusReason(const FrameGenStatus& framegen) {
    // The app shows it beside the state (On, Standby, Paused, ...), in the
    // OSD's words.
    return DescribeFrameGen(framegen, true).reason;
}

void OpenXrLayer::PollRuntimeRelay() {
    std::vector<std::string> sessions_to_remove;
    std::filesystem::path root;
    std::filesystem::path external_status_path;
    std::string session_id;
    {
        std::scoped_lock lock(mutex_);
        root = runtime_relay_root_;
        external_status_path = external_status_path_;
        session_id = runtime_relay_session_id_;
        sessions_to_remove.swap(runtime_relay_sessions_to_remove_);
    }

    std::error_code ec;
    for (const std::string& stale_session_id : sessions_to_remove) {
        std::filesystem::remove(RuntimeStatusPath(root, stale_session_id), ec);
        ec.clear();
        std::filesystem::remove(RuntimeControlPath(root, stale_session_id), ec);
        ec.clear();
    }
    // A session ended: the public status file goes with it, unless another
    // game has written its own since. A session that started meanwhile
    // writes it again below. A reader holding it past the retries: again at
    // the next polls, while no session runs.
    if ((!sessions_to_remove.empty() || (external_status_remove_pending_ && session_id.empty())) &&
        !external_status_path.empty()) {
        external_status_remove_pending_ = RemoveExternalStatusIfOwned(external_status_path, GetCurrentProcessId()) ==
                                          ExternalStatusRemoval::kBusy;
    }
    if (!root.empty() && !runtime_relay_pruned_) {
        // Once per process: files of earlier sessions that ended by a crash,
        // TerminateProcess or a hard shutdown, once they are a day old
        // (Export Debug includes newer ones).
        runtime_relay_pruned_ = true;
        const std::size_t pruned = PruneRuntimeRelayFiles(root);
        if (pruned > 0) {
            logger_.Info("Removed " + std::to_string(pruned) +
                         " runtime status/control file(s) left by ended game sessions.");
        }
    }
    if (!root.empty()) {
        // Rescan inputs in the app: every running game, once per press.
        std::uint64_t requested_at = 0;
        if (ReadInputRescanRequest(InputRescanRequestPath(root), &requested_at) &&
            requested_at > input_rescan_taken_at_) {
            const bool due = InputRescanRequestDue(requested_at, input_rescan_taken_at_, RuntimeRelayUnixMilliseconds());
            input_rescan_taken_at_ = requested_at;
            if (due) {
                RescanInputDevices();
                logger_.Info("Input devices: looking again for bound devices that were not connected (Rescan inputs "
                             "in the VectorXR app).");
            }
        }
    }
    if (root.empty() || session_id.empty()) return;

    RuntimeControlDocument control;
    std::string control_error;
    const bool has_control = ReadRuntimeControl(RuntimeControlPath(root, session_id), &control, &control_error);
    bool state_changed = false;
    bool command_applied = false;
    if (has_control && control.target_session_id == session_id &&
        control.expires_at_unix_milliseconds >= RuntimeRelayUnixMilliseconds()) {
        std::scoped_lock lock(mutex_);
        const bool available = runtime_relay_session_id_ == session_id &&
            active_session_ != XR_NULL_HANDLE && QuadViewsDiagnosticVisualizationAvailable();
        if (available && control.revision > runtime_relay_last_applied_revision_ &&
            control.quadviews_diagnostic_visualization.has_value()) {
            runtime_relay_last_applied_revision_ = control.revision;
            runtime_relay_acknowledged_revision_ = control.revision;
            const bool desired = *control.quadviews_diagnostic_visualization;
            state_changed = quadviews_diagnostic_visualization_enabled_ != desired;
            quadviews_diagnostic_visualization_enabled_ = desired;
            command_applied = true;
            runtime_relay_status_dirty_.store(true, std::memory_order_release);
        }
    }

    if (command_applied && state_changed) {
        logger_.Info(std::string("Quadviews diagnostic visualization ") +
                     (*control.quadviews_diagnostic_visualization ? "shown" : "hidden") +
                     " via the VectorXR app.");
    }

    const auto now = std::chrono::steady_clock::now();
    const bool heartbeat_due = !runtime_relay_last_status_write_.has_value() ||
        now - *runtime_relay_last_status_write_ >= std::chrono::seconds(1);
    if (!heartbeat_due && !runtime_relay_status_dirty_.exchange(false, std::memory_order_acq_rel)) return;

    osd_.ReportDiagnostics();
    RuntimeStatusDocument status;
    ExternalStatusDocument external;
    {
        std::scoped_lock lock(mutex_);
        if (runtime_relay_session_id_ != session_id || active_session_ == XR_NULL_HANDLE) return;
        status.session_id = session_id;
        status.process_id = GetCurrentProcessId();
        status.application = current_exe_name_;
        status.quadviews_dimensions = quadviews_dimensions_;
        status.quadviews_dimensions_at = quadviews_dimensions_at_;
        status.turbo_state = turbo_yielded_to_framegen_ ? "framegen" :
            turbo_recovery_blocked_.load() ? "recovery-disabled" :
            turbo_auto_suspended_.load() ? "suspended" :
            turbo_effective_active_.load() ? (turbo_effective_async_.load() ? "async" : "sequenced") :
            (resolved_settings_.core.enabled && resolved_settings_.turbo.enabled && turbo_toggle_enabled_) ? "waiting" : "off";
        const auto overlay=osd_.Status();
        status.osd_available=overlay.available; status.osd_visible=overlay.visible;
        status.osd_compact=overlay.compact; status.osd_message=overlay.message;
        status.turbo_reason = turbo_recovery_.Reason();
        if (turbo_yielded_to_framegen_)
            status.turbo_reason = "Frame Generation owns frame pacing in this session.";
        const FrameGenStatus framegen = framegen_.Status();
        const FrameGenOsdText described = DescribeFrameGen(framegen, true);
        status.framegen_state = described.state;
        status.framegen_reason = described.reason;
        // Turned on after the game started: this session runs without Frame
        // Generation until the game restarts. The OSD says RESTART PENDING;
        // tell the app too, rather than a bare Off.
        if (status.framegen_state == "off" && status.framegen_reason.empty() && resolved_settings_.core.enabled &&
            resolved_settings_.framegen.enabled && !framegen_session_enabled_)
            status.framegen_reason = "turned on after the game started; restart the game to use it";
        status.framegen_depth = framegen.depth == FrameGenDepthState::kUnknown ? "" : ToString(framegen.depth);
        status.framegen_flow = framegen.interpolate_flow_active;
        status.framegen_flow_mode = framegen.interpolate_flow_mode;
        status.framegen_flow_reason = framegen.interpolate_flow_reason;
        if (framegen_session_enabled_) {
            // The same rates as framegen_reason, as numbers (MSFS_AutoFPS, issue 103).
            status.framegen_rates = FrameGenRates(framegen);
            external.framegen = ExternalFrameGen(framegen);
        }
        if (status.turbo_reason.empty() && turbo_auto_suspended_.load())
            status.turbo_reason = "Runtime pacing repeatedly stalled. Use the Turbo toggle to retry.";

        status.updated_at_unix_milliseconds = RuntimeRelayUnixMilliseconds();
        status.acknowledged_revision = runtime_relay_acknowledged_revision_;
        external.process_id = status.process_id;
        external.application = status.application;
        external.session_id = status.session_id;
        external.updated_at_unix_milliseconds = status.updated_at_unix_milliseconds;
        status.quadviews_diagnostic_visualization_available = QuadViewsDiagnosticVisualizationAvailable();
        status.quadviews_diagnostic_visualization_enabled = quadviews_diagnostic_visualization_enabled_;
    }

    std::string status_error;
    if (WriteRuntimeStatus(RuntimeStatusPath(root, session_id), status, &status_error)) {
        runtime_relay_last_status_write_ = now;
        runtime_relay_status_dirty_.store(false, std::memory_order_release);
    } else {
        runtime_relay_status_dirty_.store(true, std::memory_order_release);
    }
    // The public file on the same heartbeat; a failed write is retried at the
    // next. A session kept only for an abandoned presenter thread (left in
    // the runtime at xrDestroySession) has ended for the game: its file goes.
    if (!external_status_path.empty()) {
        if (framegen_.PresenterAbandoned()) {
            external_status_remove_pending_ = RemoveExternalStatusIfOwned(external_status_path, GetCurrentProcessId()) ==
                                              ExternalStatusRemoval::kBusy;
        } else {
            WriteExternalStatus(external_status_path, external);
        }
    }
}

RuntimeFrameGenRates OpenXrLayer::FrameGenRates(const FrameGenStatus& framegen) {
    RuntimeFrameGenRates rates;
    rates.mode = framegen.mode == FrameGenSynthesisMode::kExtrapolate ? "extrapolate" : "interpolate";
    rates.frame_rate = framegen.dynamic ? "dynamic" : framegen.automatic ? "auto" :
        std::to_string(framegen.multiplier) + "x";
    // As the app's state says it: not while composed quad views frames could
    // not be written for a while either (standing by meanwhile).
    rates.generating = DescribeFrameGen(framegen, true).state == "active";
    rates.multiplier = framegen.multiplier;
    if (framegen.refresh_period_ns > 0) rates.refresh_hz = 1e9 / static_cast<double>(framegen.refresh_period_ns);
    if (rates.generating && !framegen.dynamic && framegen.multiplier >= 2)
        rates.hold_fps = rates.refresh_hz / framegen.multiplier;
    rates.app_fps = framegen.app_fps;
    rates.output_fps = framegen.output_fps;
    rates.distinct_fps = framegen.distinct_fps;
    return rates;
}

ExternalFrameGenStatus OpenXrLayer::ExternalFrameGen(const FrameGenStatus& framegen) {
    const RuntimeFrameGenRates rates = FrameGenRates(framegen);
    ExternalFrameGenStatus external;
    external.state = DescribeFrameGen(framegen, true).state;
    external.mode = rates.mode;
    external.frame_rate = rates.frame_rate;
    external.generating = rates.generating;
    external.multiplier = rates.multiplier;
    external.refresh_hz = rates.refresh_hz;
    external.app_fps = rates.app_fps;
    external.output_fps = rates.output_fps;
    external.distinct_fps = rates.distinct_fps;
    // Measured on the grid the game's frames are timed by (twice the refresh
    // period while the runtime halves the rate); said in headset refreshes.
    if (framegen.refresh_period_ns > 0 && framegen.grid_period_ns > 0) {
        external.refreshes_per_real_frame = framegen.refreshes_per_real_frame *
            static_cast<double>(framegen.grid_period_ns) / static_cast<double>(framegen.refresh_period_ns);
    }
    // The target and holding are on that grid: an N-x frame takes N of its
    // periods. A runtime holding frames to every second refresh (or less)
    // without reporting the longer period shows them at half the refresh
    // rate at most all the same.
    double grid_hz = framegen.grid_period_ns > 0 ? 1e9 / static_cast<double>(framegen.grid_period_ns) : 0.0;
    if (framegen.runtime_halving && framegen.refresh_period_ns > 0 &&
        framegen.grid_period_ns < framegen.refresh_period_ns + framegen.refresh_period_ns / 2) {
        grid_hz = 0.5e9 / static_cast<double>(framegen.refresh_period_ns);
    }
    ExternalFrameGenTarget(&external, grid_hz);
    if (framegen.automatic) {
        ExternalAutoStep step;
        step.step = framegen.multiplier;
        step.step_up_below_fps = framegen.auto_step_up_below_fps;
        step.step_down_above_fps = framegen.auto_step_down_above_fps;
        step.game_own_fps = framegen.auto_game_own_fps;
        external.automatic = step;
    }
    return external;
}
} // namespace depthxr
