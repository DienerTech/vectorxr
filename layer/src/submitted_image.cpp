#include "depthxr/submitted_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace depthxr {
namespace {

// DXGI_FORMAT values (dxgiformat.h), kept as numbers so the CPU half needs no
// graphics header.
enum : std::int64_t {
    kDxgiR16G16B16A16Typeless = 9,
    kDxgiR16G16B16A16Float = 10,
    kDxgiR10G10B10A2Typeless = 23,
    kDxgiR10G10B10A2Unorm = 24,
    kDxgiR8G8B8A8Typeless = 27,
    kDxgiR8G8B8A8Unorm = 28,
    kDxgiR8G8B8A8UnormSrgb = 29,
    kDxgiB8G8R8A8Unorm = 87,
    kDxgiB8G8R8X8Unorm = 88,
    kDxgiB8G8R8A8Typeless = 90,
    kDxgiB8G8R8A8UnormSrgb = 91,
    kDxgiB8G8R8X8Typeless = 92,
    kDxgiB8G8R8X8UnormSrgb = 93,
};

// VkFormat values (vulkan_core.h).
enum : std::int64_t {
    kVkR8G8B8A8Unorm = 37,
    kVkR8G8B8A8Srgb = 43,
    kVkB8G8R8A8Unorm = 44,
    kVkB8G8R8A8Srgb = 50,
    kVkA2B10G10R10UnormPack32 = 64,
    kVkR16G16B16A16Sfloat = 97,
};

SubmittedFormat Make(std::int64_t format, SubmittedPixelKind kind, bool srgb, bool bgra, bool typeless,
                     std::int64_t view_format) {
    SubmittedFormat out;
    out.format = format;
    out.kind = kind;
    out.srgb = srgb;
    out.bgra = bgra;
    out.typeless = typeless;
    out.view_format = view_format;
    out.bytes_per_pixel = kind == SubmittedPixelKind::kFloat16 ? 8 : kind == SubmittedPixelKind::kUnknown ? 0 : 4;
    return out;
}

SubmittedFormat ClassifyDxgi(std::int64_t format, bool typeless_is_srgb) {
    using K = SubmittedPixelKind;
    switch (format) {
    case kDxgiR8G8B8A8Unorm: return Make(format, K::kUnorm8, false, false, false, format);
    case kDxgiR8G8B8A8UnormSrgb: return Make(format, K::kUnorm8, true, false, false, format);
    case kDxgiR8G8B8A8Typeless:
        return Make(format, K::kUnorm8, typeless_is_srgb, false, true,
                    typeless_is_srgb ? kDxgiR8G8B8A8UnormSrgb : kDxgiR8G8B8A8Unorm);
    case kDxgiB8G8R8A8Unorm: return Make(format, K::kUnorm8, false, true, false, format);
    case kDxgiB8G8R8A8UnormSrgb: return Make(format, K::kUnorm8, true, true, false, format);
    case kDxgiB8G8R8A8Typeless:
        return Make(format, K::kUnorm8, typeless_is_srgb, true, true,
                    typeless_is_srgb ? kDxgiB8G8R8A8UnormSrgb : kDxgiB8G8R8A8Unorm);
    case kDxgiB8G8R8X8Unorm: return Make(format, K::kUnorm8, false, true, false, format);
    case kDxgiB8G8R8X8UnormSrgb: return Make(format, K::kUnorm8, true, true, false, format);
    case kDxgiB8G8R8X8Typeless:
        return Make(format, K::kUnorm8, typeless_is_srgb, true, true,
                    typeless_is_srgb ? kDxgiB8G8R8X8UnormSrgb : kDxgiB8G8R8X8Unorm);
    case kDxgiR10G10B10A2Unorm: return Make(format, K::kUnorm10, false, false, false, format);
    case kDxgiR10G10B10A2Typeless: return Make(format, K::kUnorm10, false, false, true, kDxgiR10G10B10A2Unorm);
    case kDxgiR16G16B16A16Float: return Make(format, K::kFloat16, false, false, false, format);
    case kDxgiR16G16B16A16Typeless: return Make(format, K::kFloat16, false, false, true, kDxgiR16G16B16A16Float);
    default: return Make(format, K::kUnknown, false, false, false, format);
    }
}

SubmittedFormat ClassifyVulkan(std::int64_t format) {
    using K = SubmittedPixelKind;
    switch (format) {
    case kVkR8G8B8A8Unorm: return Make(format, K::kUnorm8, false, false, false, format);
    case kVkR8G8B8A8Srgb: return Make(format, K::kUnorm8, true, false, false, format);
    case kVkB8G8R8A8Unorm: return Make(format, K::kUnorm8, false, true, false, format);
    case kVkB8G8R8A8Srgb: return Make(format, K::kUnorm8, true, true, false, format);
    case kVkA2B10G10R10UnormPack32: return Make(format, K::kUnorm10, false, false, false, format);
    case kVkR16G16B16A16Sfloat: return Make(format, K::kFloat16, false, false, false, format);
    default: return Make(format, K::kUnknown, false, false, false, format);
    }
}

// A Direct3D format's family, named by its typeless member (0: none known).
std::int64_t DxgiFamily(std::int64_t format) {
    switch (format) {
    case kDxgiR16G16B16A16Typeless:
    case kDxgiR16G16B16A16Float: return kDxgiR16G16B16A16Typeless;
    case kDxgiR10G10B10A2Typeless:
    case kDxgiR10G10B10A2Unorm: return kDxgiR10G10B10A2Typeless;
    case kDxgiR8G8B8A8Typeless:
    case kDxgiR8G8B8A8Unorm:
    case kDxgiR8G8B8A8UnormSrgb: return kDxgiR8G8B8A8Typeless;
    case kDxgiB8G8R8A8Typeless:
    case kDxgiB8G8R8A8Unorm:
    case kDxgiB8G8R8A8UnormSrgb: return kDxgiB8G8R8A8Typeless;
    case kDxgiB8G8R8X8Typeless:
    case kDxgiB8G8R8X8Unorm:
    case kDxgiB8G8R8X8UnormSrgb: return kDxgiB8G8R8X8Typeless;
    default: return 0;
    }
}

const std::array<std::uint8_t, 1024>& Unorm10Table() {
    static const std::array<std::uint8_t, 1024> table = [] {
        std::array<std::uint8_t, 1024> values{};
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = LinearToSrgb8(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return table;
}

const std::vector<std::uint8_t>& HalfTable() {
    static const std::vector<std::uint8_t> table = [] {
        std::vector<std::uint8_t> values(65536);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = LinearToSrgb8(HalfToFloat(static_cast<std::uint16_t>(i)));
        }
        return values;
    }();
    return table;
}

} // namespace

SubmittedFormat ClassifyFormat(SubmittedImageApi api, std::int64_t format, bool typeless_is_srgb) {
    switch (api) {
    case SubmittedImageApi::kD3D11:
    case SubmittedImageApi::kD3D12: return ClassifyDxgi(format, typeless_is_srgb);
    case SubmittedImageApi::kVulkan: return ClassifyVulkan(format);
    default: return Make(format, SubmittedPixelKind::kUnknown, false, false, false, format);
    }
}

bool SameDxgiFamily(std::int64_t resource_format, std::int64_t view_format) {
    if (resource_format == view_format) return true;
    const std::int64_t family = DxgiFamily(resource_format);
    return family != 0 && family == DxgiFamily(view_format);
}

bool ResolveSubmittedViews(const XrFrameEndInfo& frame, const SubmittedSwapchainLookup& lookup,
                           std::vector<SubmittedView>& views, std::string& problem) {
    views.clear();
    const XrCompositionLayerProjection* projection = nullptr;
    for (std::uint32_t i = 0; frame.layers && i < frame.layerCount && !projection; ++i) {
        const XrCompositionLayerBaseHeader* header = frame.layers[i];
        if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* candidate = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (candidate->views && candidate->viewCount > 0) projection = candidate;
    }
    if (!projection) {
        problem = "the frame has no projection layer";
        return false;
    }
    for (std::uint32_t i = 0; i < projection->viewCount; ++i) {
        const XrSwapchainSubImage& image = projection->views[i].subImage;
        SubmittedView view;
        view.swapchain = image.swapchain;
        view.view_index = i;
        view.array_slice = image.imageArrayIndex;
        view.rect = image.imageRect;
        SubmittedSwapchainState state;
        const XrRect2Di& rect = image.imageRect;
        if (image.swapchain == XR_NULL_HANDLE || !lookup || !lookup(image.swapchain, state)) {
            view.problem = "it names a swapchain the layer does not know";
        } else if (!state.has_image) {
            view.problem = "its swapchain has released no image yet";
        } else if (!state.held) {
            view.problem = "its image has already gone to the runtime";
            view.released = true;
        } else if (image.imageArrayIndex >= std::max<std::uint32_t>(state.array_size, 1)) {
            view.problem = "it names array slice " + std::to_string(image.imageArrayIndex) + " of " +
                           std::to_string(state.array_size);
        } else if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
                   static_cast<std::int64_t>(rect.offset.x) + rect.extent.width > state.width ||
                   static_cast<std::int64_t>(rect.offset.y) + rect.extent.height > state.height) {
            view.problem = "its rectangle (" + std::to_string(rect.offset.x) + "," + std::to_string(rect.offset.y) + " " +
                           std::to_string(rect.extent.width) + "x" + std::to_string(rect.extent.height) +
                           ") is empty or outside its " + std::to_string(state.width) + "x" +
                           std::to_string(state.height) + " image";
        } else {
            view.resolved = true;
            view.image_index = state.image_index;
            view.format = state.format;
            view.width = state.width;
            view.height = state.height;
            view.array_size = std::max<std::uint32_t>(state.array_size, 1);
            view.sample_count = std::max<std::uint32_t>(state.sample_count, 1);
            view.usage = state.usage;
        }
        views.push_back(std::move(view));
    }
    return true;
}

float HalfToFloat(std::uint16_t half) {
    const std::uint32_t sign = (half >> 15) & 1u;
    const std::uint32_t exponent = (half >> 10) & 0x1fu;
    const std::uint32_t mantissa = half & 0x3ffu;
    float value = 0.0f;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::nanf("") : INFINITY;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

std::uint8_t LinearToSrgb8(float linear) {
    if (!(linear > 0.0f)) return 0; // negatives and NaN
    if (linear >= 1.0f) return 255;
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
}

bool ConvertToRgba8(const SubmittedPixels& pixels, std::uint8_t* out) {
    if (!pixels.data || !out || pixels.width == 0 || pixels.height == 0 ||
        pixels.row_pitch < static_cast<std::uint64_t>(pixels.width) * pixels.format.bytes_per_pixel) {
        return false;
    }
    switch (pixels.format.kind) {
    case SubmittedPixelKind::kUnorm8: {
        const int red = pixels.format.bgra ? 2 : 0;
        const int blue = pixels.format.bgra ? 0 : 2;
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            const std::uint8_t* in = pixels.data + static_cast<std::size_t>(y) * pixels.row_pitch;
            std::uint8_t* row = out + static_cast<std::size_t>(y) * pixels.width * 4;
            for (std::uint32_t x = 0; x < pixels.width; ++x, in += 4, row += 4) {
                row[0] = in[red];
                row[1] = in[1];
                row[2] = in[blue];
                row[3] = 255;
            }
        }
        return true;
    }
    case SubmittedPixelKind::kUnorm10: {
        const auto& table = Unorm10Table();
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            const std::uint8_t* in = pixels.data + static_cast<std::size_t>(y) * pixels.row_pitch;
            std::uint8_t* row = out + static_cast<std::size_t>(y) * pixels.width * 4;
            for (std::uint32_t x = 0; x < pixels.width; ++x, in += 4, row += 4) {
                std::uint32_t texel;
                std::memcpy(&texel, in, 4);
                row[0] = table[texel & 0x3ffu];
                row[1] = table[(texel >> 10) & 0x3ffu];
                row[2] = table[(texel >> 20) & 0x3ffu];
                row[3] = 255;
            }
        }
        return true;
    }
    case SubmittedPixelKind::kFloat16: {
        const auto& table = HalfTable();
        for (std::uint32_t y = 0; y < pixels.height; ++y) {
            const std::uint8_t* in = pixels.data + static_cast<std::size_t>(y) * pixels.row_pitch;
            std::uint8_t* row = out + static_cast<std::size_t>(y) * pixels.width * 4;
            for (std::uint32_t x = 0; x < pixels.width; ++x, in += 8, row += 4) {
                std::uint16_t texel[3];
                std::memcpy(texel, in, 6);
                row[0] = table[texel[0]];
                row[1] = table[texel[1]];
                row[2] = table[texel[2]];
                row[3] = 255;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace depthxr
