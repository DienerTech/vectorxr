// vectorxr_hardware_probe --render-check: the rendering half of the probe without OpenXR or a headset. The backend
// draws the probe's swapchain shapes into images the probe makes itself (standing in for a runtime's: the layout
// OpenXR hands images over in), the pixels are read back and checked, and the debug layer or validation must report
// nothing. Two rounds, with the swapchains removed in between as --recreate-seconds does.
#include "probe_common.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace probe {
namespace {

constexpr uint32_t kEyeWidth = 160;
constexpr uint32_t kEyeHeight = 120;
constexpr uint32_t kImages = 3;
// More frames than the Vulkan backend has frame slots, so their command buffers are reused within a round.
constexpr int kFrames = 10;

using Rgba = std::array<uint8_t, 4>;

// A colour as an RGBA8 image stores it: sRGB-encoded colour channels for sRGB formats, linear alpha.
Rgba Stored(const std::array<float, 4>& color, bool srgb) {
    Rgba stored{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        float value = std::clamp(color[channel], 0.0f, 1.0f);
        if (srgb && channel < 3) {
            value = value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
        }
        stored[channel] = static_cast<uint8_t>(std::lround(value * 255.0f));
    }
    return stored;
}

struct Checks {
    uint64_t passed{0};
    std::vector<std::string> failures;

    void Expect(bool condition, const std::string& what) {
        if (condition) {
            ++passed;
        } else if (failures.size() < 20) {
            failures.push_back(what);
        }
    }
};

class ImageView {
public:
    ImageView(const std::vector<uint8_t>& pixels, uint32_t width) : pixels_(pixels), width_(width) {}

    Rgba At(int32_t x, int32_t y) const {
        const std::size_t offset = (static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4;
        return {pixels_[offset], pixels_[offset + 1], pixels_[offset + 2], pixels_[offset + 3]};
    }

private:
    const std::vector<uint8_t>& pixels_;
    uint32_t width_;
};

bool Near(const Rgba& actual, const Rgba& expected) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(static_cast<int>(actual[channel]) - static_cast<int>(expected[channel])) > 2) {
            return false;
        }
    }
    return true;
}

std::string Text(const Rgba& color) {
    std::ostringstream text;
    text << static_cast<int>(color[0]) << "," << static_cast<int>(color[1]) << "," << static_cast<int>(color[2]) << ","
         << static_cast<int>(color[3]);
    return text.str();
}

// One view's pixels within an image: the clear colour, and with the pattern a frame, 7x7 squares and a bar of the
// right width. Row `quiet` passes between the squares: only the frame's sides and the bar cross it.
void CheckView(Checks& checks, const ImageView& image, const std::optional<std::vector<float>>& depth,
               uint32_t image_width, const PixelRect& rect, const std::array<float, 4>& clear, bool pattern,
               PatternKind kind, bool srgb, const std::string& name) {
    const uint32_t width = static_cast<uint32_t>(rect.right - rect.left);
    const uint32_t height = static_cast<uint32_t>(rect.bottom - rect.top);
    const Pattern reference = PatternRects(width, height, kind, 0.0);
    const Rgba clear_stored = Stored(clear, srgb);
    const Rgba grid = Stored(reference.grid_color, srgb);
    const Rgba bar = Stored(reference.bar_color, srgb);
    const int32_t edge = std::max<int32_t>(2, static_cast<int32_t>(width) / 200);
    const int32_t quiet = static_cast<int32_t>(height) * 3 / 16;
    const auto depth_at = [&](int32_t x, int32_t y) {
        return (*depth)[static_cast<std::size_t>(rect.top + y) * image_width + static_cast<std::size_t>(rect.left + x)];
    };

    int32_t bar_pixels = 0;
    int32_t bar_first = -1;
    int32_t bar_last = -1;
    bool row_ok = true;
    std::string row_problem;
    for (int32_t x = 0; x < static_cast<int32_t>(width); ++x) {
        const Rgba pixel = image.At(rect.left + x, rect.top + quiet);
        const bool side = x < edge || x >= static_cast<int32_t>(width) - edge;
        if (pattern && Near(pixel, bar)) {
            ++bar_pixels;
            bar_first = bar_first < 0 ? x : bar_first;
            bar_last = x;
            continue;
        }
        const Rgba& expected = pattern && side ? grid : clear_stored;
        if (!Near(pixel, expected) && row_ok) {
            row_ok = false;
            row_problem = "x=" + std::to_string(x) + " is " + Text(pixel) + ", expected " + Text(expected);
        }
        if (depth && pattern && !side && row_ok && std::abs(depth_at(x, quiet) - 1.0f) > 1.0e-6f) {
            row_ok = false;
            row_problem = "x=" + std::to_string(x) + " has depth " + std::to_string(depth_at(x, quiet)) + ", expected 1";
        }
    }
    checks.Expect(row_ok, name + ": the row between the squares: " + row_problem);
    if (pattern) {
        const int32_t bar_width = reference.bar.right - reference.bar.left;
        checks.Expect(bar_pixels == bar_width && bar_last - bar_first + 1 == bar_width,
                      name + ": the bar is " + std::to_string(bar_pixels) + " pixels wide, expected " +
                          std::to_string(bar_width));
    }

    int32_t squares = 0;
    bool squares_ok = true;
    for (int32_t column = 1; column < 8; ++column) {
        const int32_t x = static_cast<int32_t>(width) * column / 8;
        const int32_t y = static_cast<int32_t>(height) * 4 / 8;
        const Rgba pixel = image.At(rect.left + x, rect.top + y);
        if (!pattern) {
            squares_ok = squares_ok && Near(pixel, clear_stored);
            continue;
        }
        if (Near(pixel, grid)) {
            ++squares;
        } else if (!Near(pixel, bar)) {
            squares_ok = false;
        }
        if (depth) {
            squares_ok = squares_ok && std::abs(depth_at(x, y) - DepthAt(2.0f)) < 1.0e-5f;
        }
    }
    checks.Expect(squares_ok && (!pattern || squares >= 6),
                  name + ": the squares' centres (" + std::to_string(squares) + " of 7 in the grid colour)");
}

ViewDraw View(uint32_t color, uint32_t image, const PixelRect& rect, uint32_t view, bool pattern, bool first) {
    // Looking straight up with a narrow field of view: the Direct3D 11 line scene stays out of sight.
    constexpr float kHalf = 0.70710678f;
    ViewDraw draw;
    draw.color = color;
    draw.color_image = image;
    draw.rect = rect;
    draw.first_in_image = first;
    draw.pose = {{kHalf, 0.0f, 0.0f, kHalf}, {0.0f, 0.0f, 0.0f}};
    draw.fov = {-0.17f, 0.17f, 0.17f, -0.17f};
    draw.view = view;
    draw.pattern = pattern;
    return draw;
}

XrSwapchainCreateInfo Info(uint32_t width, uint32_t height, int64_t format, XrSwapchainUsageFlags usage) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = usage;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    return info;
}

} // namespace

bool RunRenderCheck(const Options& options) {
    const std::unique_ptr<CheckImages> images = options.graphics == GraphicsApi::Vulkan
                                                    ? CreateVulkanCheckImages(options)
                                                    : CreateD3D11CheckImages(options);
    ProbeGraphics& graphics = images->Graphics();
    const int64_t color_format = graphics.ColorFormats().front(); // RGBA, sRGB
    const int64_t depth_format = graphics.DepthFormat();
    std::cout << "Render check: " << graphics.Describe() << ", " << kFrames << " frames of " << kImages
              << " images, twice\n";

    Checks checks;
    for (int round = 0; round < 2; ++round) {
        // As the probe's three shapes: side by side with a depth swapchain, one swapchain per view (the right eye
        // without the pattern), and the quad layer.
        const uint32_t wide = images->Add(Info(kEyeWidth * 2, kEyeHeight, color_format,
                                               XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT),
                                          SwapchainRole::ProjectionWithDepth, kImages);
        const uint32_t wide_depth = images->Add(Info(kEyeWidth * 2, kEyeHeight, depth_format,
                                                     XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
                                                SwapchainRole::Depth, kImages);
        std::array<uint32_t, 2> separate{};
        for (uint32_t& id : separate) {
            id = images->Add(Info(kEyeWidth, kEyeHeight, color_format, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT),
                             SwapchainRole::Projection, kImages);
        }
        const uint32_t quad = images->Add(Info(kQuadLayerSize, kQuadLayerSize, color_format,
                                               XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT),
                                          SwapchainRole::Quad, kImages);
        const auto eye_rect = [](uint32_t eye) {
            const int32_t left = static_cast<int32_t>(kEyeWidth * eye);
            return PixelRect{left, 0, left + static_cast<int32_t>(kEyeWidth), static_cast<int32_t>(kEyeHeight)};
        };
        const PixelRect full{0, 0, static_cast<int32_t>(kEyeWidth), static_cast<int32_t>(kEyeHeight)};

        for (int frame = 0; frame < kFrames; ++frame) {
            const uint32_t image = static_cast<uint32_t>(frame) % kImages;
            graphics.BeginFrame();
            for (uint32_t eye = 0; eye < 2; ++eye) {
                ViewDraw draw = View(wide, image, eye_rect(eye), eye, true, eye == 0);
                draw.depth = wide_depth;
                draw.depth_image = image;
                graphics.Draw(draw);
            }
            graphics.ImageDone();
            for (uint32_t eye = 0; eye < 2; ++eye) {
                graphics.Draw(View(separate[eye], image, full, eye, eye == 0, true));
                graphics.ImageDone();
            }
            ViewDraw quad_draw = View(quad, image, {0, 0, kQuadLayerSize, kQuadLayerSize}, 0, false, true);
            quad_draw.quad = true;
            graphics.Draw(quad_draw);
            graphics.ImageDone();
            graphics.EndFrame();
        }
        if (round == 0) {
            images->Remove(); // right after the frames' submissions, as a recreate is
            continue;
        }

        const uint32_t last = static_cast<uint32_t>(kFrames - 1) % kImages;
        const std::vector<uint8_t> wide_pixels = images->ColorPixels(wide, last);
        const std::optional<std::vector<float>> wide_depth_values = images->DepthPixels(wide_depth, last);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            CheckView(checks, ImageView(wide_pixels, kEyeWidth * 2), wide_depth_values, kEyeWidth * 2, eye_rect(eye),
                      ClearColor(eye), true, PatternKind::Peripheral, true,
                      "side-by-side " + std::string(eye == 0 ? "left" : "right") + " eye");
            const std::vector<uint8_t> pixels = images->ColorPixels(separate[eye], last);
            CheckView(checks, ImageView(pixels, kEyeWidth), std::nullopt, kEyeWidth, full, ClearColor(eye), eye == 0,
                      PatternKind::Peripheral, true, std::string(eye == 0 ? "left" : "right") + " eye's swapchain");
        }
        const std::vector<uint8_t> quad_pixels = images->ColorPixels(quad, last);
        CheckView(checks, ImageView(quad_pixels, kQuadLayerSize), std::nullopt, kQuadLayerSize,
                  {0, 0, kQuadLayerSize, kQuadLayerSize}, kQuadBackground, true, PatternKind::Quad, true,
                  "quad layer");
        if (!wide_depth_values) {
            std::cout << "Render check: depth not checked (the Direct3D 11 pattern writes none)\n";
        }
        images->Remove();
    }

    const uint64_t problems = graphics.Report();
    for (const std::string& failure : checks.failures) {
        std::cout << "Render check failed: " << failure << "\n";
    }
    const bool passed = checks.failures.empty() && problems == 0;
    std::cout << "Render check: " << checks.passed << " checks passed, " << checks.failures.size() << " failed"
              << (problems > 0 ? ", " + std::to_string(problems) + " debug/validation messages" : std::string())
              << (passed ? ": passed" : ": FAILED") << "\n";
    return passed;
}

} // namespace probe
