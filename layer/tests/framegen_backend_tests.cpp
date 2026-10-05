// Runs the Frame Generation GPU backends on synthetic frames with known motion
// and compares the generated image against analytic ground truth.
//   depthxr_framegen_tests d3d11 | d3d12 | vulkan
// Direct3D runs on WARP so the test needs no GPU; Vulkan needs a device and
// exits 77 (skipped) when none is present.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

#ifdef DEPTHXR_NVOF_RESEARCH
#include <nvOpticalFlowD3D12.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <tuple>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;
using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kSize = 384;
constexpr int kSkip = 77;
// On a hardware adapter (bench-, hw-, profile, nvof, replay) and benchmarking.
bool g_bench = false;
bool g_hardware = false;

void Check(bool ok, const std::string& why) {
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        std::exit(1);
    }
}

using Pixels = std::vector<std::uint32_t>; // RGBA8, sRGB encoded

float Hash(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

// Value noise: smooth enough to match, detailed enough to be unambiguous.
float Noise(float x, float y) {
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = x - ix, fy = y - iy;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}

std::uint32_t Pack(float r, float g, float b) {
    auto channel = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return channel(r) | (channel(g) << 8) | (channel(b) << 16) | 0xff000000u;
}

std::uint32_t Background(float x, float y) {
    const float n = 0.6f * Noise(x / 14.0f, y / 14.0f) + 0.4f * Noise(x / 5.0f + 17.0f, y / 5.0f);
    return Pack(0.15f + 0.5f * n, 0.2f + 0.45f * n, 0.3f + 0.3f * n);
}

std::uint32_t Patch(float x, float y) {
    const float n = Noise(x / 6.0f + 40.0f, y / 6.0f + 40.0f);
    return Pack(0.9f * n + 0.1f, 0.6f * (1.0f - n), 0.25f);
}

// Static background with a 96 px textured square whose top-left is at (px, py).
Pixels MovingSquare(float px, float py) {
    Pixels pixels(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float cx = x + 0.5f, cy = y + 0.5f;
            const bool inside = cx >= px && cx < px + 96.0f && cy >= py && cy < py + 96.0f;
            pixels[y * kSize + x] = inside ? Patch(cx - px, cy - py) : Background(cx, cy);
        }
    }
    return pixels;
}

// A building facade: a regular grid of windows (12 px apart across, 16 px
// down) in lightly textured concrete, shifted right by `offset` pixels and
// antialiased horizontally. One window period away it matches almost as well
// as the true motion, which is what makes skyscrapers alias.
Pixels Facade(float offset) {
    Pixels pixels(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            float glass = 0.0f;
            for (int sample = 0; sample < 4; ++sample) {
                const float u = x + (sample + 0.5f) / 4.0f - offset;
                const float cell = u - 12.0f * std::floor(u / 12.0f);
                const float row = (y + 0.5f) - 16.0f * std::floor((y + 0.5f) / 16.0f);
                glass += (cell >= 3.0f && cell < 9.0f && row >= 4.0f && row < 12.0f) ? 0.25f : 0.0f;
            }
            const float n = 0.04f * Noise((x - offset) / 3.0f, y / 3.0f);
            const float r = (0.72f + n) * (1.0f - glass) + 0.12f * glass;
            const float g = (0.70f + n) * (1.0f - glass) + 0.18f * glass;
            const float b = (0.66f + n) * (1.0f - glass) + 0.26f * glass;
            pixels[y * kSize + x] = Pack(r, g, b);
        }
    }
    return pixels;
}

// Still cockpit detail that shimmers: a textured panel whose texture sits up
// to 2 px off in a direction that changes every frame (the way
// anti-aliasing and upscalers make instrument faces and text wobble), over
// still scenery. Nothing moves, so a generated frame should show the newest
// frame as it is.
constexpr std::uint32_t kShimmerFirst = 96, kShimmerLast = 288; // the panel, both axes
Pixels Shimmer(int frame) {
    const float jx = 4.0f * (Hash(frame, 7) - 0.5f);
    const float jy = 4.0f * (Hash(frame, 11) - 0.5f);
    Pixels pixels(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float cx = x + 0.5f, cy = y + 0.5f;
            const bool inside = x >= kShimmerFirst && x < kShimmerLast && y >= kShimmerFirst && y < kShimmerLast;
            pixels[y * kSize + x] = inside ? Patch(cx - jx, cy - jy) : Background(cx, cy);
        }
    }
    return pixels;
}

// Tangent of the test views' half field of view.
constexpr float kTan = 0.8f;

// A cockpit: a textured panel 0.4 m square, 0.5 m in front of the head, over
// scenery at infinity, seen from a head at (head_x, 0, 0) looking down -Z
// through a view spanning tangents tan_left..tan_right across and +-kTan
// down.
constexpr float kPanelDistance = 0.5f;
constexpr float kPanelHalf = 0.2f;
Pixels Cockpit(float head_x, float tan_left = -kTan, float tan_right = kTan) {
    Pixels pixels(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float tx = tan_left + (tan_right - tan_left) * (x + 0.5f) / kSize;
            const float ty = kTan - 2 * kTan * (y + 0.5f) / kSize;
            const float px = head_x + kPanelDistance * tx;
            const float py = kPanelDistance * ty;
            pixels[y * kSize + x] = std::abs(px) < kPanelHalf && std::abs(py) < kPanelHalf
                                        ? Patch(px * 480.0f + 100.0f, py * 480.0f + 100.0f)
                                        : Background(tx * 240.0f, ty * 240.0f);
        }
    }
    return pixels;
}

// Where the panel appears in the image for a head at head_x.
XrRect2Di CockpitPanelRect(float head_x, float tan_left = -kTan, float tan_right = kTan) {
    auto column = [&](float px, float head) {
        return static_cast<int>(std::lround(((px - head) / kPanelDistance - tan_left) / (tan_right - tan_left) * kSize));
    };
    auto row = [](float py) { return static_cast<int>(std::lround((kTan - py / kPanelDistance) / (2 * kTan) * kSize)); };
    const int x0 = column(-kPanelHalf, head_x), x1 = column(kPanelHalf, head_x);
    const int y0 = row(kPanelHalf), y1 = row(-kPanelHalf);
    return {{x0, y0}, {x1 - x0, y1 - y0}};
}

// A panorama at infinity rendered through a camera yawed by `yaw` radians.
Pixels Panorama(float yaw) {
    Pixels pixels(kSize * kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const float tx = -kTan + 2 * kTan * (x + 0.5f) / kSize;
            const float ty = kTan - 2 * kTan * (y + 0.5f) / kSize;
            // View looks down -Z; yaw rotates about +Y (counterclockwise from above).
            const float dx = tx * std::cos(yaw) - std::sin(yaw);
            const float dz = -tx * std::sin(yaw) - std::cos(yaw);
            const float azimuth = std::atan2(dx, -dz);
            const float elevation = std::atan2(ty, std::sqrt(dx * dx + dz * dz));
            const float u = azimuth * 180.0f, v = elevation * 180.0f;
            const float n = 0.6f * Noise(u / 6.0f, v / 6.0f) + 0.4f * Noise(u / 2.5f + 9.0f, v / 2.5f);
            pixels[y * kSize + x] = Pack(0.2f + 0.6f * n, 0.25f + 0.5f * n, 0.4f * n + 0.1f);
        }
    }
    return pixels;
}

double MeanError(const Pixels& a, const Pixels& b, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1) {
    double sum = 0;
    std::size_t count = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::uint32_t p = a[y * kSize + x], q = b[y * kSize + x];
            for (int c = 0; c < 3; ++c) {
                sum += std::abs(static_cast<int>((p >> (8 * c)) & 255) - static_cast<int>((q >> (8 * c)) & 255));
            }
            count += 3;
        }
    }
    return sum / (255.0 * count);
}

// FG_TEST_DUMP=<directory> writes every compared image as a binary PPM.
void Dump(const std::string& name, const Pixels& pixels) {
    const char* directory = std::getenv("FG_TEST_DUMP");
    if (!directory) return;
    std::FILE* file = std::fopen((std::string(directory) + "/" + name + ".ppm").c_str(), "wb");
    if (!file) return;
    std::fprintf(file, "P6\n%u %u\n255\n", kSize, kSize);
    for (const std::uint32_t pixel : pixels) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(pixel & 255), static_cast<unsigned char>((pixel >> 8) & 255),
                                      static_cast<unsigned char>((pixel >> 16) & 255)};
        std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
}

// ---------------------------------------------------------------------------
// Minimal per-API device harness: images, upload, readback, binding.

struct Harness {
    std::string name;
    std::int64_t format{0};
    const void* binding{nullptr};
    std::function<void*(std::uint32_t, std::uint32_t)> create;
    std::function<void(void*, const Pixels&)> upload;
    std::function<Pixels(void*)> readback;
    std::function<void()> flush = [] {};
    // A depth image cleared to `far_value`, with `rect` cleared to
    // `near_value`; empty where the backend has no depth path.
    std::function<void*(float near_value, XrRect2Di rect, float far_value)> create_depth;
    // Profiling at headset sizes (Direct3D 12 only): the same at any size.
    std::function<void(void*, std::uint32_t, std::uint32_t, const Pixels&)> upload_sized;
    std::function<void*(std::uint32_t, std::uint32_t, float, XrRect2Di, float)> create_depth_sized;
    // Replay (Direct3D 12): a 32-bit float depth image holding `values`, and
    // reading back an image of any size.
    std::function<void*(std::uint32_t, std::uint32_t, const std::vector<float>&)> create_depth_data;
    std::function<Pixels(void*)> readback_sized;
};

std::map<XrSwapchain, std::vector<void*>> g_swapchains;
XrStructureType g_image_type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;

XrResult XRAPI_PTR EnumerateImages(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t* count,
                                   XrSwapchainImageBaseHeader* images) {
    const auto& list = g_swapchains[swapchain];
    *count = static_cast<std::uint32_t>(list.size());
    if (!capacity) return XR_SUCCESS;
    for (std::uint32_t i = 0; i < *count; ++i) {
        switch (g_image_type) {
        case XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR:
            reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i].texture = static_cast<ID3D11Texture2D*>(list[i]);
            break;
        case XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR:
            reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[i].texture = static_cast<ID3D12Resource*>(list[i]);
            break;
        default:
            reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)[i].image = reinterpret_cast<VkImage>(list[i]);
            break;
        }
    }
    return XR_SUCCESS;
}

const XrSwapchain kSource = reinterpret_cast<XrSwapchain>(0x51);
const XrSwapchain kOutput = reinterpret_cast<XrSwapchain>(0x52);
const XrSwapchain kOutputSecond = reinterpret_cast<XrSwapchain>(0x54);
const XrSwapchain kDepthImage = reinterpret_cast<XrSwapchain>(0x55);

FrameGenViewSubmission Submission(std::uint32_t source_index, std::int64_t format, std::int64_t output_format, bool synthesize,
                                  float yaw_newest, float yaw_previous) {
    FrameGenViewSubmission view;
    view.source = {kSource, source_index, 0, {{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}}, format};
    view.outputs = {{kOutput, 0, 0, {{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}}, output_format},
                    {kOutputSecond, 0, 0, {{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}}, output_format}};
    view.request.extent = {kSize, kSize};
    view.request.output = FrameGenOutputKind::kSrgb8;
    view.request.synthesize = synthesize;
    view.request.temporal = true;
    const float angle = std::atan(kTan);
    for (auto* geometry : {&view.request.newest, &view.request.previous}) {
        geometry->angle_left = -angle;
        geometry->angle_right = angle;
        geometry->angle_up = angle;
        geometry->angle_down = -angle;
    }
    view.request.newest.orientation = {0, std::sin(yaw_newest / 2), 0, std::cos(yaw_newest / 2)};
    view.request.previous.orientation = {0, std::sin(yaw_previous / 2), 0, std::cos(yaw_previous / 2)};
    return view;
}

int RunScenarios(Harness& harness) {
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, harness.name + " backend: " + error);
    Check(gpu->Classify(harness.format) == FrameGenOutputKind::kSrgb8, harness.name + " classifies sRGB RGBA8");
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();

    g_swapchains[kSource] = {harness.create(kSize, kSize), harness.create(kSize, kSize), harness.create(kSize, kSize)};
    g_swapchains[kOutput] = {harness.create(kSize, kSize)};
    g_swapchains[kOutputSecond] = {harness.create(kSize, kSize)};
    Check(gpu->Track(kSource, EnumerateImages, error) && gpu->Track(kOutput, EnumerateImages, error) &&
              gpu->Track(kOutputSecond, EnumerateImages, error),
          error);

    FrameGenTuning tuning;
    FrameGenExecuteResult result;
    auto run = [&](std::uint32_t index, bool synthesize, float yaw_newest, float yaw_previous) {
        const std::vector<FrameGenViewSubmission> views{
            Submission(index, harness.format, output_format, synthesize, yaw_newest, yaw_previous)};
        const bool executed = gpu->Execute(views, tuning, result, error);
        Check(executed, harness.name + " execute: " + error);
        return harness.readback(g_swapchains[kOutput][0]);
    };

    // 1. Priming: no history, so the newest frame passes through unchanged.
    const Pixels a = MovingSquare(120, 120);
    const Pixels b = MovingSquare(136, 128);
    harness.upload(g_swapchains[kSource][0], a);
    harness.upload(g_swapchains[kSource][1], b);
    const Pixels primed = run(0, true, 0, 0);
    Check(!result.synthesized[0], harness.name + " primed frame must not claim synthesis");
    Check(MeanError(primed, a, 0, 0, kSize, kSize) < 0.004, harness.name + " primed output differs from the source");

    // 2. Interpolation halfway between A and B: the square sits at (128, 124).
    tuning.mode = FrameGenSynthesisMode::kInterpolate;
    tuning.fractions[0] = 0.5f;
    const Pixels interpolated = run(1, true, 0, 0);
    Check(result.synthesized[0], harness.name + " interpolation did not synthesize");
    const Pixels truth_mid = MovingSquare(128, 124);
    Dump(harness.name + "-interpolated", interpolated);
    Dump(harness.name + "-truth-mid", truth_mid);
    if (std::getenv("FG_TEST_DUMP")) {
        for (const auto view : {FrameGenDiagnosticView::kFlow, FrameGenDiagnosticView::kConfidence}) {
            tuning.diagnostic = view;
            gpu->ResetHistory();
            run(0, true, 0, 0);
            Dump(harness.name + (view == FrameGenDiagnosticView::kFlow ? "-flow" : "-confidence"), run(1, true, 0, 0));
        }
        tuning.diagnostic = FrameGenDiagnosticView::kOff;
    }
    const double interp_error = MeanError(interpolated, truth_mid, 96, 96, 256, 256);
    const double hold_error = MeanError(b, truth_mid, 96, 96, 256, 256);
    std::cout << harness.name << " interpolate: error " << interp_error << " vs hold " << hold_error << '\n';
    Check(interp_error < 0.3 * hold_error, harness.name + " interpolation is not better than repeating the newest frame");
    Check(MeanError(interpolated, truth_mid, 0, 0, 64, 64) < 0.01, harness.name + " static background was disturbed");

    // 2b. A 3x multiplier: one flow field feeds two generated frames.
    {
        tuning.generated = 2;
        tuning.fractions = {1.0f / 3.0f, 2.0f / 3.0f, 0.0f};
        gpu->ResetHistory();
        run(0, true, 0, 0);
        const Pixels third = run(1, true, 0, 0);
        const Pixels two_thirds = harness.readback(g_swapchains[kOutputSecond][0]);
        const double third_error = MeanError(third, MovingSquare(120 + 16.0f / 3, 120 + 8.0f / 3), 96, 96, 256, 256);
        const double two_thirds_error = MeanError(two_thirds, MovingSquare(120 + 32.0f / 3, 120 + 16.0f / 3), 96, 96, 256, 256);
        const double third_hold = MeanError(b, MovingSquare(120 + 16.0f / 3, 120 + 8.0f / 3), 96, 96, 256, 256);
        std::cout << harness.name << " 3x: errors " << third_error << ", " << two_thirds_error << " vs hold " << third_hold
                  << '\n';
        Check(third_error < 0.4 * third_hold && two_thirds_error < 0.4 * third_hold,
              harness.name + " 3x frames are not placed at 1/3 and 2/3 of the motion");

        // 2c. Direct3D 12 draws generated frames straight into the outputs;
        // written by compute and copied, as the other backends do, they are
        // the same up to sRGB rounding.
        gpu->SetDirectOutput(false);
        gpu->ResetHistory();
        run(0, true, 0, 0);
        const Pixels third_copied = run(1, true, 0, 0);
        const Pixels two_thirds_copied = harness.readback(g_swapchains[kOutputSecond][0]);
        gpu->SetDirectOutput(true);
        const double drawn_difference = std::max(MeanError(third, third_copied, 0, 0, kSize, kSize),
                                                 MeanError(two_thirds, two_thirds_copied, 0, 0, kSize, kSize));
        std::cout << harness.name << " drawn vs copied: difference " << drawn_difference << '\n';
        Check(drawn_difference < 0.001, harness.name + " drawn generated frames differ from computed ones");
        tuning.generated = 1;
        tuning.fractions = {0.5f, 0.5f, 0.5f};
    }

    // 3. Extrapolation half a step past B: the square sits at (144, 132).
    harness.upload(g_swapchains[kSource][2], b);
    tuning.mode = FrameGenSynthesisMode::kExtrapolate;
    gpu->ResetHistory();
    run(0, true, 0, 0);
    const Pixels extrapolated = run(2, true, 0, 0);
    Check(result.synthesized[0], harness.name + " extrapolation did not synthesize");
    const Pixels truth_ahead = MovingSquare(144, 132);
    const double extrap_error = MeanError(extrapolated, truth_ahead, 96, 96, 272, 272);
    const double extrap_hold = MeanError(b, truth_ahead, 96, 96, 272, 272);
    std::cout << harness.name << " extrapolate: error " << extrap_error << " vs hold " << extrap_hold << '\n';
    Check(extrap_error < 0.5 * extrap_hold, harness.name + " extrapolation is not better than repeating the newest frame");

    // 3b. A repeating facade drifting 3 px per frame (a skyscraper passing
    // slowly). Known limit: windows repeat finer than the coarse pyramid
    // levels resolve, so coarse vectors are noise and finer levels lock onto
    // a neighbouring period; generated frames are then worse than repeating
    // the newest one. Search changes that stop the aliasing (a motion prior,
    // a local search around predictions) also stop finer levels finding
    // real motion the coarse levels missed (case 3 regressed 2-7x). Depth-
    // derived motion or hardware optical flow is the fix; until then this
    // guards against it getting worse.
    {
        // Extrapolated half a step ahead, as the presenter thread does. Three
        // frames 3 px apart: the last pair also has last frame's flow as a
        // predictor, the way a steady scene would.
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.fractions[0] = 0.5f;
        harness.upload(g_swapchains[kSource][0], Facade(0.0f));
        harness.upload(g_swapchains[kSource][1], Facade(3.0f));
        harness.upload(g_swapchains[kSource][2], Facade(6.0f));
        auto sequence = [&] {
            gpu->ResetHistory();
            run(0, true, 0, 0);
            const Pixels first = run(1, true, 0, 0);
            return std::make_pair(first, run(2, true, 0, 0));
        };
        const auto [first, facade] = sequence();
        const double first_error = MeanError(first, Facade(4.5f), 32, 32, kSize - 32, kSize - 32);
        const double first_hold = MeanError(Facade(3.0f), Facade(4.5f), 32, 32, kSize - 32, kSize - 32);
        const double facade_error = MeanError(facade, Facade(7.5f), 32, 32, kSize - 32, kSize - 32);
        const double facade_hold = MeanError(Facade(6.0f), Facade(7.5f), 32, 32, kSize - 32, kSize - 32);
        Dump(harness.name + "-facade", facade);
        Dump(harness.name + "-facade-truth", Facade(7.5f));
        if (std::getenv("FG_TEST_DUMP")) {
            tuning.diagnostic = FrameGenDiagnosticView::kFlow;
            Dump(harness.name + "-facade-flow", sequence().second);
            tuning.diagnostic = FrameGenDiagnosticView::kConfidence;
            Dump(harness.name + "-facade-confidence", sequence().second);
            tuning.diagnostic = FrameGenDiagnosticView::kOff;
        }
        std::cout << harness.name << " facade: errors " << first_error << ", " << facade_error << " vs hold "
                  << first_hold << ", " << facade_hold << std::endl;
        Check(first_error < 2.2 * first_hold && facade_error < 2.2 * facade_hold,
              harness.name + " repeating windows alias worse than before");
    }

    // 3d. Shimmering still detail, extrapolated frame after frame: the
    // matcher sees the shimmer as motion, a different one every frame.
    // Extrapolating it would make still cockpit detail jitter at the real
    // frame rate; motion that does not continue from the last frame pair is
    // not extrapolated.
    {
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.fractions[0] = 0.5f;
        gpu->ResetHistory();
        double error_sum = 0.0, difference_sum = 0.0;
        Pixels previous;
        for (int frame = 0; frame < 6; ++frame) {
            const Pixels image = Shimmer(frame);
            harness.upload(g_swapchains[kSource][frame % 3], image);
            const Pixels output = run(static_cast<std::uint32_t>(frame % 3), true, 0, 0);
            if (frame >= 2) {
                // From the third frame on the last frame pair's motion is known.
                error_sum += MeanError(output, image, kShimmerFirst, kShimmerFirst, kShimmerLast, kShimmerLast);
                difference_sum += MeanError(previous, image, kShimmerFirst, kShimmerFirst, kShimmerLast, kShimmerLast);
            }
            if (frame == 5) {
                Dump(harness.name + "-shimmer", output);
            }
            previous = image;
        }
        std::cout << harness.name << " shimmer: error " << error_sum / 4 << " vs frame-to-frame shimmer "
                  << difference_sum / 4 << std::endl;
        Check(error_sum < 0.25 * difference_sum, harness.name + " shimmering still detail jitters in generated frames");
    }

    // 3f. Motion measured by the optical flow hardware (NVIDIA RTX 20-50 on
    // Direct3D 12): the repeating facade no longer aliases and the moving
    // square still moves. Elsewhere the backend says why not and block
    // matching measures motion instead.
    {
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.fractions[0] = 0.5f;
        tuning.hardware_flow = true;
        auto sequence = [&](const Pixels& a, const Pixels& b, const Pixels& c) {
            harness.upload(g_swapchains[kSource][0], a);
            harness.upload(g_swapchains[kSource][1], b);
            harness.upload(g_swapchains[kSource][2], c);
            gpu->ResetHistory();
            run(0, true, 0, 0);
            run(1, true, 0, 0);
            return run(2, true, 0, 0);
        };
        const Pixels facade = sequence(Facade(0.0f), Facade(3.0f), Facade(6.0f));
        const std::string status = gpu->HardwareFlowStatus();
        const double facade_error = MeanError(facade, Facade(7.5f), 32, 32, kSize - 32, kSize - 32);
        const double facade_hold = MeanError(Facade(6.0f), Facade(7.5f), 32, 32, kSize - 32, kSize - 32);
        const Pixels square = sequence(MovingSquare(104, 112), MovingSquare(120, 120), MovingSquare(136, 128));
        const double square_error = MeanError(square, MovingSquare(144, 132), 96, 96, 288, 288);
        const double square_hold = MeanError(MovingSquare(136, 128), MovingSquare(144, 132), 96, 96, 288, 288);
        std::cout << harness.name << " hardware flow (" << (status.empty() ? "not asked" : status) << "): facade error "
                  << facade_error << " vs hold " << facade_hold << ", square " << square_error << " vs hold " << square_hold
                  << std::endl;
        Dump(harness.name + "-facade-hardware", facade);
        if (status == "active") {
            Check(facade_error < facade_hold, harness.name + " the optical flow hardware's facade is worse than holding");
        }
        Check(square_error < 0.5 * square_hold, harness.name + " motion is not extrapolated with hardware flow asked for");
        if (status == "active") {
            // Hardware that stops answering must not hang the game's queue:
            // the watchdog releases it and block matching takes over.
            gpu->StallHardwareForTest();
            run(0, true, 0, 0);
            const auto start = std::chrono::steady_clock::now();
            const Pixels released = sequence(MovingSquare(104, 112), MovingSquare(120, 120), MovingSquare(136, 128));
            const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << harness.name << " stalled hardware: released after " << waited << " ms, now "
                      << gpu->HardwareFlowStatus() << std::endl;
            Check(gpu->HardwareFlowStatus() == "the optical flow hardware stopped responding",
                  harness.name + " the stalled optical flow hardware was not released");
            Check(MeanError(released, MovingSquare(144, 132), 96, 96, 288, 288) < 0.5 * square_hold,
                  harness.name + " block matching did not take over from stalled hardware");
        }
        tuning.hardware_flow = false;
    }

    // 3h. Captured at release, as games' frames are, the hardware matches
    // the frames as rendered right away; generation then takes the head
    // movement out of its vectors. Motion still moves, and a static
    // panorama seen by a turning head stays where the newest frame has it.
    if (harness.name == "D3D12" && g_hardware) {
        auto backend = CreateFrameGenGpu(harness.binding, error);
        Check(backend != nullptr, harness.name + " backend: " + error);
        Check(backend->Track(kSource, EnumerateImages, error) && backend->Track(kOutput, EnumerateImages, error) &&
                  backend->Track(kOutputSecond, EnumerateImages, error),
              error);
        FrameGenTuning raw_tuning;
        raw_tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        raw_tuning.fractions[0] = 0.5f;
        raw_tuning.hardware_flow = true;
        auto captured = [&](const std::array<Pixels, 3>& frames, const std::array<float, 3>& yaws) {
            for (std::uint32_t i = 0; i < 3; ++i) harness.upload(g_swapchains[kSource][i], frames[i]);
            backend->ResetHistory();
            for (std::uint32_t i = 0; i < 3; ++i) {
                const std::vector<FrameGenViewSubmission> views{
                    Submission(i, harness.format, output_format, true, yaws[i], i ? yaws[i - 1] : yaws[i])};
                Check(backend->Capture(0, views[0].source, views[0].request, raw_tuning.quality, error) || error.empty(),
                      harness.name + " capture: " + error);
                const bool executed = backend->Execute(views, raw_tuning, result, error);
                Check(executed, harness.name + " execute: " + error);
            }
            return harness.readback(g_swapchains[kOutput][0]);
        };
        const Pixels square = captured({MovingSquare(104, 112), MovingSquare(120, 120), MovingSquare(136, 128)}, {0, 0, 0});
        const std::string status = backend->HardwareFlowStatus();
        const double square_error = MeanError(square, MovingSquare(144, 132), 96, 96, 288, 288);
        const double square_hold = MeanError(MovingSquare(136, 128), MovingSquare(144, 132), 96, 96, 288, 288);
        const float turn = 2.0f * 3.14159265f / 180.0f;
        const Pixels turned = captured({Panorama(0), Panorama(turn), Panorama(2 * turn)}, {0, turn, 2 * turn});
        const double turned_error = MeanError(turned, Panorama(2 * turn), 48, 48, kSize - 48, kSize - 48);
        const double turned_baseline = MeanError(Panorama(turn), Panorama(2 * turn), 48, 48, kSize - 48, kSize - 48);
        std::cout << harness.name << " hardware flow from release (" << status << "): square " << square_error
                  << " vs hold " << square_hold << ", turning head " << turned_error << " vs uncompensated "
                  << turned_baseline << std::endl;
        if (status == "active") {
            Check(square_error < 0.5 * square_hold, harness.name + " motion matched at release is not extrapolated");
            Check(turned_error < 0.3 * turned_baseline, harness.name + " head rotation leaked into vectors matched at release");
        }
        backend->Reset();
    }

    // 3e. Steady motion over several frames keeps being extrapolated once the
    // last frame pair's motion is known, also when a real frame came late
    // (the frames' times say how far steady motion moved meanwhile).
    for (const bool late : {false, true}) {
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.fractions[0] = 0.5f;
        gpu->ResetHistory();
        auto position = [](float time) { return std::make_pair(100.0f + 12.0f * time, 110.0f + 6.0f * time); };
        // Frame times in units of the usual interval; the fourth frame late.
        const std::vector<float> times = late ? std::vector<float>{0, 1, 2, 4, 5} : std::vector<float>{0, 1, 2, 3, 4};
        Pixels output;
        for (std::size_t frame = 0; frame < times.size(); ++frame) {
            const auto [x, y] = position(times[frame]);
            harness.upload(g_swapchains[kSource][frame % 3], MovingSquare(x, y));
            tuning.interval_ratio =
                frame >= 2 ? (times[frame] - times[frame - 1]) / (times[frame - 1] - times[frame - 2]) : 1.0f;
            output = run(static_cast<std::uint32_t>(frame % 3), true, 0, 0);
            if (std::getenv("FG_TEST_VERBOSE") && frame >= 1) {
                const float ahead = times[frame] + 0.5f * (times[frame] - times[frame - 1]);
                const auto [ax, ay] = position(ahead);
                std::cout << "  frame " << frame << " ratio " << tuning.interval_ratio << " error "
                          << MeanError(output, MovingSquare(ax, ay), 96, 96, 288, 288) << " hold "
                          << MeanError(MovingSquare(x, y), MovingSquare(ax, ay), 96, 96, 288, 288) << std::endl;
            }
        }
        tuning.interval_ratio = 1.0f;
        const auto [tx, ty] = position(times.back() + 0.5f);
        const auto [hx, hy] = position(times.back());
        const Pixels truth = MovingSquare(tx, ty);
        const double steady_error = MeanError(output, truth, 96, 96, 288, 288);
        const double steady_hold = MeanError(MovingSquare(hx, hy), truth, 96, 96, 288, 288);
        std::cout << harness.name << (late ? " steady motion after a late frame" : " steady motion") << ": error "
                  << steady_error << " vs hold " << steady_hold << std::endl;
        Check(steady_error < 0.5 * steady_hold, harness.name + " steady motion stopped being extrapolated");
    }

    // 3c. Head translation in a cockpit, seen by two eyes 64 mm apart through
    // a wide headset's views (each eye sees a quarter of its view alone): the
    // head moves 2 cm sideways, so a panel 0.5 m away shifts ~10 px while the
    // scenery stays. Nothing moves in the head's space, so a generated frame
    // should show the newest frame as it is (the runtime corrects head
    // motion). Rotation-only matching reads the parallax as motion and pushes
    // the panel on; with depth it is compensated, but only once the depth
    // has lined the eyes up, which also catches depth described wrongly, and
    // only where the other eye confirms it (cockpit glass written to depth).
    if (harness.create_depth) {
        const float shift = 0.02f;
        const float half_ipd = 0.032f;
        // Tangents across: each eye sees further outward than inward.
        const float outer = 1.0f, inner = 0.6f;
        const XrSwapchain right_source = reinterpret_cast<XrSwapchain>(0x56);
        const XrSwapchain right_output = reinterpret_cast<XrSwapchain>(0x57);
        const XrSwapchain right_depth = reinterpret_cast<XrSwapchain>(0x58);
        g_swapchains[right_source] = {harness.create(kSize, kSize), harness.create(kSize, kSize)};
        g_swapchains[right_output] = {harness.create(kSize, kSize)};
        harness.upload(g_swapchains[kSource][0], Cockpit(-half_ipd, -outer, inner));
        harness.upload(g_swapchains[kSource][1], Cockpit(shift - half_ipd, -outer, inner));
        harness.upload(g_swapchains[right_source][0], Cockpit(half_ipd, -inner, outer));
        harness.upload(g_swapchains[right_source][1], Cockpit(shift + half_ipd, -inner, outer));
        const Pixels truth = Cockpit(shift - half_ipd, -outer, inner);
        const XrRect2Di left_panel = CockpitPanelRect(shift - half_ipd, -outer, inner);
        const XrRect2Di right_panel = CockpitPanelRect(shift + half_ipd, -inner, outer);
        FrameGenDepthEncoding standard; // depth 0..1 for 0.1 m..100 m, 1/z linear
        standard.near_z = 0.1f;
        standard.far_z = 100.0f;
        const float panel = (1.0f / kPanelDistance - 1.0f / standard.near_z) /
                            (1.0f / standard.far_z - 1.0f / standard.near_z);
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.fractions[0] = 0.5f;
        tuning.generated = 1;

        // The panel's rect grown by `margin` px: depth claiming scenery there
        // is near, the way glass in front of it would.
        auto grow = [](XrRect2Di rect, int margin) {
            const int x0 = std::max(0, rect.offset.x - margin), y0 = std::max(0, rect.offset.y - margin);
            const int x1 = std::min(static_cast<int>(kSize), rect.offset.x + rect.extent.width + margin);
            const int y1 = std::min(static_cast<int>(kSize), rect.offset.y + rect.extent.height + margin);
            return XrRect2Di{{x0, y0}, {x1 - x0, y1 - y0}};
        };
        struct Outcome {
            Pixels output;
            bool used{false};
            FrameGenDepthReading reading{FrameGenDepthReading::kUnknown};
            float scale{1.0f};
        };
        // Runs one depth case on a fresh backend: depth images for the newest
        // frame (panel and background values, the near value over the panel
        // grown by `glass` px) described by `encoding`; the eyes keep moving
        // `eye` meters sideways per unit of generated-frame time.
        auto run_case = [&](float panel_value, float background_value, const FrameGenDepthEncoding& encoding,
                            bool use_depth, int glass, float eye = 0.0f) {
            auto backend = CreateFrameGenGpu(harness.binding, error);
            Check(backend != nullptr, harness.name + " backend: " + error);
            g_swapchains[kDepthImage] = {harness.create_depth(panel_value, grow(left_panel, glass), background_value)};
            g_swapchains[right_depth] = {harness.create_depth(panel_value, grow(right_panel, glass), background_value)};
            for (const XrSwapchain chain : {kSource, kOutput, right_source, right_output, kDepthImage, right_depth}) {
                Check(backend->Track(chain, EnumerateImages, error), error);
            }
            auto views = [&](std::uint32_t index) {
                FrameGenViewSubmission left = Submission(index, harness.format, output_format, true, 0, 0);
                FrameGenViewSubmission right = left;
                right.source.swapchain = right_source;
                right.outputs = {{right_output, 0, 0, left.outputs[0].rect, output_format}};
                left.outputs.resize(1);
                for (auto* geometry : {&left.request.newest, &left.request.previous}) {
                    geometry->angle_left = std::atan(-outer);
                    geometry->angle_right = std::atan(inner);
                }
                for (auto* geometry : {&right.request.newest, &right.request.previous}) {
                    geometry->angle_left = std::atan(-inner);
                    geometry->angle_right = std::atan(outer);
                }
                left.request.newest.position = {shift - half_ipd, 0, 0};
                left.request.previous.position = {-half_ipd, 0, 0};
                right.request.newest.position = {shift + half_ipd, 0, 0};
                right.request.previous.position = {half_ipd, 0, 0};
                left.request.depth = right.request.depth = use_depth;
                left.request.shift = right.request.shift = {eye, 0.0f, 0.0f};
                return std::vector<FrameGenViewSubmission>{left, right};
            };
            // Color is captured at release, as games' frames are; the depth
            // trust pass needs both eyes' images of the frame.
            auto capture = [&](std::uint32_t index) {
                const XrRect2Di rect{{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}};
                const auto requests = views(index);
                for (std::size_t i = 0; i < 2; ++i) {
                    Check(backend->Capture(i, requests[i].source, requests[i].request, tuning.quality, error),
                          harness.name + " capture: " + error);
                }
                if (index == 1) {
                    Check(backend->CaptureDepth(0, {kDepthImage, 0, 0, rect, 0}, encoding, requests[0].request, error) &&
                              backend->CaptureDepth(1, {right_depth, 0, 0, rect, 0}, encoding, requests[1].request, error),
                          harness.name + " depth capture: " + error);
                }
            };
            // Prime, then frames until the eyes have been checked a few times
            // and the scores read back.
            Check(backend->Execute(views(0), tuning, result, error), harness.name + " execute: " + error);
            for (int i = 0; i < 20; ++i) {
                capture(1);
                Check(backend->Execute(views(1), tuning, result, error), harness.name + " execute: " + error);
            }
            backend->ResetHistory();
            capture(0);
            Check(backend->Execute(views(0), tuning, result, error), harness.name + " execute: " + error);
            capture(1);
            Check(backend->Execute(views(1), tuning, result, error), harness.name + " execute: " + error);
            Outcome outcome;
            outcome.used = result.depth.size() == 2 && result.depth[0];
            outcome.output = harness.readback(g_swapchains[kOutput][0]);
            const FrameGenDepthStatus status = backend->DepthStatus();
            outcome.reading = status.reading;
            outcome.scale = status.scale;
            const FrameGenDepthEvidence& evidence = status.evidence;
            std::cout << harness.name << " depth check: " << ToString(status.reading) << " after " << status.decisions
                      << " decisions; moved " << evidence.decoded.samples << " of " << evidence.binocular
                      << " samples, ratio " << evidence.decoded.Ratio() << " (fallback " << evidence.decoded.fallback
                      << "), swapped " << evidence.swapped.Ratio() << " (" << evidence.swapped.samples << ")" << std::endl;
            backend->Reset();
            return outcome;
        };

        const Outcome rotation_only = run_case(panel, 1.0f, standard, false, 0);
        const Outcome correct = run_case(panel, 1.0f, standard, true, 0);
        // Reversed depth described as standard: near is 1, far is 0.
        const Outcome mislabeled = run_case(1.0f - panel, 0.0f, standard, true, 0);
        // Nonsense: the panel far away and the scenery half a meter off.
        const Outcome nonsense = run_case(1.0f, panel, standard, true, 0);
        // Glass: depth also claims 40 px of scenery around the panel is near.
        const Outcome glass = run_case(panel, 1.0f, standard, true, 40);
        // A near plane described wrongly (0.1 m, rendered with 0.125 m): the
        // panel decodes 20% too near, so head translation would be
        // over-compensated, until the scale is searched (1/0.8 = 1.25).
        const float panel_misplaced = (1.0f / kPanelDistance - 1.0f / 0.125f) / (1.0f / standard.far_z - 1.0f / 0.125f);
        const Outcome misplaced = run_case(panel_misplaced, 1.0f, standard, true, 0);
        Dump(harness.name + "-cockpit-rotation-only", rotation_only.output);
        Dump(harness.name + "-cockpit-depth", correct.output);
        Dump(harness.name + "-cockpit-mislabeled", mislabeled.output);
        Dump(harness.name + "-cockpit-glass", glass.output);
        auto error_of = [&](const Outcome& outcome) { return MeanError(outcome.output, truth, 64, 64, kSize - 64, kSize - 64); };
        const double rotation_error = error_of(rotation_only);
        const double correct_error = error_of(correct);
        const double mislabeled_error = error_of(mislabeled);
        const double nonsense_error = error_of(nonsense);
        const double glass_error = error_of(glass);
        std::cout << harness.name << " cockpit: error " << correct_error << " (" << ToString(correct.reading) << "), "
                  << mislabeled_error << " mislabeled (" << ToString(mislabeled.reading) << "), " << nonsense_error
                  << " nonsense (" << ToString(nonsense.reading) << "), " << glass_error << " glass ("
                  << ToString(glass.reading) << ") vs " << rotation_error << " rotation only" << std::endl;
        Check(!rotation_only.used, harness.name + " depth used without being asked for");
        Check(correct.used && correct.reading == FrameGenDepthReading::kAsSubmitted && correct_error < 0.4 * rotation_error,
              harness.name + " correct depth did not compensate head translation");
        Check(mislabeled.used && mislabeled.reading == FrameGenDepthReading::kReversed &&
                  mislabeled_error < 0.4 * rotation_error,
              harness.name + " reversed depth described as standard was not recognized");
        Check(!nonsense.used && nonsense.reading == FrameGenDepthReading::kUnusable &&
                  nonsense_error < 1.2 * rotation_error,
              harness.name + " nonsense depth was used");
        Check(glass.used && glass.reading == FrameGenDepthReading::kAsSubmitted && glass_error < 0.4 * rotation_error,
              harness.name + " depth wrong around the panel (glass) was not dropped there");
        const double misplaced_error = error_of(misplaced);
        std::cout << harness.name << " near plane described wrongly: scale " << misplaced.scale << ", error "
                  << misplaced_error << " (correct depth: scale " << correct.scale << ")" << std::endl;
        Check(misplaced.used && misplaced.scale > 1.15f && misplaced.scale < 1.35f && misplaced_error < 0.4 * rotation_error,
              harness.name + " a wrongly described near plane was not corrected");
        Check(std::abs(correct.scale - 1.0f) < 0.05f, harness.name + " correctly described depth was rescaled");

        // 3g. The head keeps moving after the newest frame: 4 cm sideways per
        // real frame interval, so the generated frame (half an interval on)
        // sees the panel from 2 cm further, ~10 px of parallax. Runtimes
        // only rotate frames, so the generated frame itself must show it;
        // the scenery (and glass in front of it) stays.
        const float eye = 0.04f;
        const Pixels moved_truth = Cockpit(shift - half_ipd + 0.5f * eye, -outer, inner);
        const Outcome moving = run_case(panel, 1.0f, standard, true, 0, eye);
        const Outcome moving_glass = run_case(panel, 1.0f, standard, true, 40, eye);
        Dump(harness.name + "-cockpit-eye-shift", moving.output);
        Dump(harness.name + "-cockpit-eye-shift-glass", moving_glass.output);
        Dump(harness.name + "-cockpit-eye-shift-truth", moved_truth);
        auto moved_error_of = [&](const Pixels& output) { return MeanError(output, moved_truth, 64, 64, kSize - 64, kSize - 64); };
        const double moving_error = moved_error_of(moving.output);
        const double moving_glass_error = moved_error_of(moving_glass.output);
        const double unshifted_error = moved_error_of(correct.output);
        std::cout << harness.name << " eye shift: error " << moving_error << ", " << moving_glass_error
                  << " glass vs " << unshifted_error << " unshifted" << std::endl;
        Check(moving.used && moving_error < 0.4 * unshifted_error,
              harness.name + " generated frames did not follow the eye");
        Check(moving_glass.used && moving_glass_error < 0.4 * unshifted_error,
              harness.name + " glass moved with the eye");
    }

    // 4. Head rotation only: compensation must leave a static panorama where
    // the newest frame has it. Uncompensated flow would smear it sideways.
    const float yaw = 3.0f * 3.14159265f / 180.0f;
    harness.upload(g_swapchains[kSource][0], Panorama(0));
    harness.upload(g_swapchains[kSource][1], Panorama(yaw));
    tuning.mode = FrameGenSynthesisMode::kInterpolate;
    gpu->ResetHistory();
    run(0, true, 0, 0);
    const Pixels rotated = run(1, true, yaw, 0);
    const Pixels newest = Panorama(yaw);
    const Pixels oldest = Panorama(0);
    const double rotation_error = MeanError(rotated, newest, 48, 48, kSize - 48, kSize - 48);
    const double rotation_baseline = MeanError(oldest, newest, 48, 48, kSize - 48, kSize - 48);
    std::cout << harness.name << " rotation: error " << rotation_error << " vs uncompensated " << rotation_baseline << '\n';
    Check(rotation_error < 0.3 * rotation_baseline, harness.name + " head rotation leaked into generated frames");

    // 5. Capture at release gives the same result as copying in EndFrame,
    // and a capture of a different image falls back to the EndFrame copy.
    harness.upload(g_swapchains[kSource][0], a);
    harness.upload(g_swapchains[kSource][1], b);
    tuning = {};
    tuning.fractions[0] = 0.5f;
    auto capture = [&](std::uint32_t index) {
        const FrameGenViewSubmission view = Submission(index, harness.format, output_format, true, 0, 0);
        const bool captured = gpu->Capture(0, view.source, view.request, tuning.quality, error);
        Check(captured, harness.name + " capture: " + error);
    };
    gpu->ResetHistory();
    capture(0);
    run(0, true, 0, 0);
    capture(1);
    const Pixels captured = run(1, true, 0, 0);
    Check(result.synthesized[0], harness.name + " captured history did not synthesize");
    Check(MeanError(captured, interpolated, 0, 0, kSize, kSize) < 0.002,
          harness.name + " capture at release changed the generated frame");
    gpu->ResetHistory();
    capture(0);
    run(0, true, 0, 0);
    harness.upload(g_swapchains[kSource][2], Panorama(0));
    capture(2); // a different image than the one generation reads
    const Pixels mismatched = run(1, true, 0, 0);
    Check(MeanError(mismatched, interpolated, 0, 0, kSize, kSize) < 0.002,
          harness.name + " a stale capture must not replace the submitted image");

    // 6. Diagnostics and still writes keep working; geometry changes reset history.
    tuning.diagnostic = FrameGenDiagnosticView::kTint;
    run(0, false, 0, 0);
    Check(!result.synthesized[0], harness.name + " synthesize=false must write a still frame");
    Check(gpu->LastGpuMilliseconds() >= -1.0, harness.name + " timing query");
    if (const auto window = gpu->LastGpuWindow()) {
        // On the presenter's clock: shortly before now.
        const auto now = std::chrono::steady_clock::now();
        Check(window->submission > 0 && window->end >= window->start && window->end <= now &&
                  now - window->end < std::chrono::seconds(10),
              harness.name + " GPU timing on the steady clock");
    } else {
        Check(harness.name != "D3D12", harness.name + " GPU timing window");
    }

    gpu->Reset();
    std::cout << harness.name << " Frame Generation backend scenarios passed\n";
    return 0;
}

// Benchmark: two views at real headset sizes on a hardware adapter. Not a
// CTest; run `depthxr_framegen_tests bench-<api>` and read the GPU times.

int RunBench(Harness& harness) {
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, harness.name + " backend: " + error);
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    const XrSwapchain output_right = reinterpret_cast<XrSwapchain>(0x53);
    for (const std::uint32_t size : {2048u, 2880u}) {
        g_swapchains[kSource] = {harness.create(size, size)};
        g_swapchains[kOutput] = {harness.create(size, size)};
        g_swapchains[output_right] = {harness.create(size, size)};
        gpu->Forget(kSource);
        gpu->Forget(kOutput);
        gpu->Forget(output_right);
        Check(gpu->Track(kSource, EnumerateImages, error) && gpu->Track(kOutput, EnumerateImages, error) &&
                  gpu->Track(output_right, EnumerateImages, error),
              error);
        for (const auto quality : {FrameGenFlowQuality::kPerformance, FrameGenFlowQuality::kBalanced, FrameGenFlowQuality::kQuality}) {
            for (const auto mode : {FrameGenSynthesisMode::kInterpolate, FrameGenSynthesisMode::kExtrapolate}) {
                FrameGenTuning tuning;
                tuning.quality = quality;
                tuning.mode = mode;
                std::vector<FrameGenViewSubmission> views;
                for (const XrSwapchain output : {kOutput, output_right}) {
                    FrameGenViewSubmission view = Submission(0, harness.format, output_format, true, 0.01f, 0.0f);
                    view.source.rect.extent = {static_cast<int>(size), static_cast<int>(size)};
                    view.outputs = {{output, 0, 0, view.source.rect, output_format}};
                    view.request.extent = {size, size};
                    views.push_back(view);
                }
                FrameGenExecuteResult result;
                double sum = 0.0;
                int samples = 0;
                for (int i = 0; i < 90; ++i) {
                    const bool executed = gpu->Execute(views, tuning, result, error);
                    Check(executed, harness.name + " bench execute: " + error);
                    harness.flush();
                    Sleep(4);
                    const double ms = gpu->LastGpuMilliseconds();
                    if (i >= 30 && ms > 0.0) {
                        sum += ms;
                        ++samples;
                    }
                }
                std::printf("%s %ux%u x2 views  %-11s %-11s  %.3f ms GPU\n", harness.name.c_str(), size, size,
                            quality == FrameGenFlowQuality::kPerformance ? "performance"
                            : quality == FrameGenFlowQuality::kBalanced ? "balanced" : "quality",
                            mode == FrameGenSynthesisMode::kInterpolate ? "interpolate" : "extrapolate",
                            samples ? sum / samples : -1.0);
            }
        }
    }
    gpu->Reset();
    return 0;
}

// Profile: the MSFS-like case at headset size on the GPU. Two eyes of a
// cockpit over scrolling scenery, with depth, extrapolated 4x; prints the
// GPU time of every pass. Not a CTest: `depthxr_framegen_tests profile`.
bool g_profile = false;
std::uint32_t g_profile_width = 6240, g_profile_height = 6280;

// The stereo cockpit at any size: the panel (fine detail) 0.5 m away over
// scenery at infinity scrolled `scroll` tangent units sideways.
Pixels CockpitAt(std::uint32_t width, std::uint32_t height, float head_x, float tan_left, float tan_right,
                 float tan_vertical, float scroll) {
    Pixels pixels(std::size_t{width} * height);
    auto rows = [&](std::uint32_t first, std::uint32_t step) {
        for (std::uint32_t y = first; y < height; y += step) {
            const float ty = tan_vertical - 2 * tan_vertical * (y + 0.5f) / height;
            for (std::uint32_t x = 0; x < width; ++x) {
                const float tx = tan_left + (tan_right - tan_left) * (x + 0.5f) / width;
                const float px = head_x + kPanelDistance * tx;
                const float py = kPanelDistance * ty;
                pixels[std::size_t{y} * width + x] = std::abs(px) < kPanelHalf && std::abs(py) < kPanelHalf
                                                         ? Patch(px * 4000.0f + 100.0f, py * 4000.0f + 100.0f)
                                                         : Background((tx + scroll) * 2400.0f, ty * 2400.0f);
            }
        }
    };
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (std::uint32_t t = 0; t < threads; ++t) {
        workers.emplace_back(rows, t, threads);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return pixels;
}

int RunProfile(Harness& harness) {
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, harness.name + " backend: " + error);
    gpu->SetProfiling(true);
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    const std::uint32_t width = g_profile_width, height = g_profile_height;
    const std::uint32_t depth_width = width * 4 / 5, depth_height = height * 4 / 5; // 80% render scale
    // A Pimax Crystal Super's views: each eye sees further outward.
    const float outer = 2.0f, inner = 1.26f, vertical = 1.64f, half_ipd = 0.0336f;
    constexpr int kFrames = 6;
    constexpr float kScroll = 0.004f; // per frame, ~8 px at this size
    std::printf("Profiling %ux%u per eye, depth %ux%u, extrapolate 4x; rendering test frames...\n", width, height,
                depth_width, depth_height);
    std::array<XrSwapchain, 2> sources{reinterpret_cast<XrSwapchain>(0x61), reinterpret_cast<XrSwapchain>(0x62)};
    std::array<XrSwapchain, 2> depths{reinterpret_cast<XrSwapchain>(0x63), reinterpret_cast<XrSwapchain>(0x64)};
    std::array<std::array<XrSwapchain, 3>, 2> outputs{};
    for (std::size_t eye = 0; eye < 2; ++eye) {
        const float head = eye == 0 ? -half_ipd : half_ipd;
        const float left = eye == 0 ? -outer : -inner, right = eye == 0 ? inner : outer;
        g_swapchains[sources[eye]].clear();
        for (int frame = 0; frame < kFrames; ++frame) {
            void* image = harness.create(width, height);
            harness.upload_sized(image, width, height, CockpitAt(width, height, head, left, right, vertical, kScroll * frame));
            g_swapchains[sources[eye]].push_back(image);
        }
        // The panel in depth texels, depth MSFS-style: reversed, 1000 m to 0.1 m.
        auto column = [&](float px) {
            return static_cast<int>(((px - head) / kPanelDistance - left) / (right - left) * depth_width);
        };
        auto row = [&](float py) { return static_cast<int>((vertical - py / kPanelDistance) / (2 * vertical) * depth_height); };
        const XrRect2Di panel{{column(-kPanelHalf), row(kPanelHalf)},
                              {column(kPanelHalf) - column(-kPanelHalf), row(-kPanelHalf) - row(kPanelHalf)}};
        const float panel_depth = (1.0f / kPanelDistance - 1.0f / 1000.0f) / (1.0f / 0.1f - 1.0f / 1000.0f);
        g_swapchains[depths[eye]] = {harness.create_depth_sized(depth_width, depth_height, panel_depth, panel, 0.0f)};
        for (std::size_t set = 0; set < 3; ++set) {
            outputs[eye][set] = reinterpret_cast<XrSwapchain>(0x70 + eye * 3 + set);
            g_swapchains[outputs[eye][set]] = {harness.create(width, height)};
        }
        Check(gpu->Track(sources[eye], EnumerateImages, error) && gpu->Track(depths[eye], EnumerateImages, error), error);
        for (const XrSwapchain output : outputs[eye]) {
            Check(gpu->Track(output, EnumerateImages, error), error);
        }
    }
    FrameGenDepthEncoding encoding;
    encoding.near_z = 1000.0f;
    encoding.far_z = 0.1f;
    const char* names[] = {"luma",        "downsample",  "search",      "refine",      "filter",
                           "synthesize",  "synthesize16", "depth",      "depth check", "depth trust",
                           "flow pack",   "flow unpack", "trust update", "shift clear", "shift splat",
                           "shift resolve", "shift fill", "raw pack",   "source copy", "output copy", "hardware wait"};
    static_assert(std::size(names) == kFrameGenPassCount + 3, "a name per profiled category");
    for (const int config : {0, 1, 2}) {
        const auto quality = config == 1 ? FrameGenFlowQuality::kQuality : FrameGenFlowQuality::kBalanced;
        FrameGenTuning tuning;
        tuning.quality = quality;
        tuning.hardware_flow = config == 2;
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.generated = 3;
        tuning.fractions = {0.25f, 0.5f, 0.75f};
        gpu->ResetHistory();
        std::vector<double> sums;
        double total = 0.0;
        int samples = 0;
        bool depth_used = false;
        for (int i = 0; i < 48; ++i) {
            const std::uint32_t frame = static_cast<std::uint32_t>(i % kFrames);
            std::vector<FrameGenViewSubmission> views;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                const float head = eye == 0 ? -half_ipd : half_ipd;
                FrameGenViewSubmission view;
                const XrRect2Di rect{{0, 0}, {static_cast<int>(width), static_cast<int>(height)}};
                view.source = {sources[eye], frame, 0, rect, harness.format};
                for (const XrSwapchain output : outputs[eye]) {
                    view.outputs.push_back({output, 0, 0, rect, output_format});
                }
                view.request.extent = {width, height};
                view.request.output = FrameGenOutputKind::kSrgb8;
                view.request.synthesize = true;
                view.request.temporal = true;
                view.request.depth = true;
                // The head moving 2 cm per real frame: generated frames follow the eye.
                view.request.shift = {0.02f, 0.0f, 0.0f};
                for (auto* geometry : {&view.request.newest, &view.request.previous}) {
                    geometry->angle_left = std::atan(eye == 0 ? -outer : -inner);
                    geometry->angle_right = std::atan(eye == 0 ? inner : outer);
                    geometry->angle_up = std::atan(vertical);
                    geometry->angle_down = -std::atan(vertical);
                    geometry->position = {head, 0, 0};
                }
                views.push_back(view);
            }
            // At release, as the presenter does: color, then depth.
            for (std::size_t eye = 0; eye < 2; ++eye) {
                Check(gpu->Capture(eye, views[eye].source, views[eye].request, quality, error) || error.empty(),
                      "capture: " + error);
                const XrRect2Di depth_rect{{0, 0}, {static_cast<int>(depth_width), static_cast<int>(depth_height)}};
                Check(gpu->CaptureDepth(eye, {depths[eye], 0, 0, depth_rect, 0}, encoding, views[eye].request, error) ||
                          error.empty(),
                      "depth capture: " + error);
            }
            // Games release their images a few milliseconds before the frame
            // ends and generation starts (FG_PROFILE_GAP_MS, default 6).
            const char* gap = std::getenv("FG_PROFILE_GAP_MS");
            Sleep(static_cast<DWORD>(gap ? std::atoi(gap) : 6));
            // FG_PROFILE_DUMP=<directory>: frames 30-34 of the first run as a
            // capture for `replay`.
            if (const char* dump = std::getenv("FG_PROFILE_DUMP"); dump && config == 0 && i >= 30 && i < 35) {
                const std::string directory = std::string(dump) + "/frame-" + std::to_string(i - 30);
                CreateDirectoryA(directory.c_str(), nullptr);
                gpu->DumpNextExecute(directory);
            }
            FrameGenExecuteResult result;
            Check(gpu->Execute(views, tuning, result, error), "profile execute: " + error);
            const std::vector<double> times = gpu->PassMilliseconds();
            if (i >= 24 && !times.empty()) {
                sums.resize(times.size());
                for (std::size_t p = 0; p < times.size(); ++p) {
                    sums[p] += times[p];
                    total += times[p];
                }
                ++samples;
                depth_used = depth_used || (result.depth.size() == 2 && result.depth[0]);
            }
        }
        const FrameGenDepthStatus depth = gpu->DepthStatus();
        std::printf("\n%s flow, 2 eyes, 3 generated frames: %.2f ms GPU per real frame (depth %s, %s)\n",
                    config == 2 ? ("Hardware (" + gpu->HardwareFlowStatus() + ")").c_str()
                    : quality == FrameGenFlowQuality::kQuality ? "Quality" : "Balanced",
                    samples ? total / samples : -1.0, depth_used ? "used" : "not used", ToString(depth.reading));
        for (std::size_t p = 0; p < sums.size() && p < std::size(names); ++p) {
            if (sums[p] > 0.0) {
                std::printf("  %-14s %6.3f ms\n", names[p], sums[p] / samples);
            }
        }
    }
    gpu->Reset();
    return 0;
}

// Replay: real frames captured from a game (FrameGenGpu::DumpNextExecute,
// armed by %LOCALAPPDATA%\VectorXR\framegen-capture.request) fed back
// through Direct3D 12 on the GPU. Each frame is extrapolated exactly to the
// next captured frame's time and eye position, and compared with that frame
// turned to the same orientation (what the runtime does to every frame):
// a measure of generated-frame quality on real content, split into cockpit
// (trusted depth nearer than 4 m) and the rest. Not a CTest:
//   depthxr_framegen_tests replay <capture directory> [output directory]
// Environment: FG_REPLAY_QUALITY=performance|balanced|quality,
// FG_REPLAY_HARDWARE=1 (optical flow hardware), FG_REPLAY_NOSHIFT=1 (no eye
// shift), FG_REPLAY_NODEPTH=1 (rotation only), FG_REPLAY_PREDICT=1 (the eye
// keeps moving as it did from the previous frame, as the presenter shifts,
// instead of exactly to the next frame's position).
bool g_replay = false;
std::string g_replay_directory, g_replay_output;

struct ReplayView {
    std::uint32_t width{0}, height{0}, half_width{0}, half_height{0};
    int format{0};
    FrameGenViewGeometry newest, previous;
    std::array<float, 3> shift{};
    Pixels color;
    std::vector<float> depth; // linear meters at half resolution, before trust
};

struct ReplayFrame {
    FrameGenTuning tuning;
    std::vector<ReplayView> views;
};

bool ReadBinary(const std::string& path, void* data, std::size_t bytes) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    const std::size_t read = std::fread(data, 1, bytes, file);
    std::fclose(file);
    return read == bytes;
}

bool LoadReplayFrame(const std::string& directory, ReplayFrame& frame) {
    std::FILE* file = std::fopen((directory + "/frame.txt").c_str(), "r");
    if (!file) return false;
    char key[64];
    auto geometry = [&](FrameGenViewGeometry& g) {
        return std::fscanf(file, " %63s %f %f %f %f %f %f %f %f %f %f %f", key, &g.orientation[0], &g.orientation[1],
                           &g.orientation[2], &g.orientation[3], &g.position[0], &g.position[1], &g.position[2],
                           &g.angle_left, &g.angle_right, &g.angle_up, &g.angle_down) == 12;
    };
    int mode = 0, quality = 0, hardware = 0, generated = 0, reading = 0;
    std::size_t count = 0;
    bool ok = std::fscanf(file, " mode %d quality %d hardware %d suppression %d limit %d interval_ratio %f generated %d"
                                " fractions %f %f %f depth_reading %d views %zu",
                          &mode, &quality, &hardware, &frame.tuning.artifact_suppression,
                          &frame.tuning.motion_limit_percent, &frame.tuning.interval_ratio, &generated,
                          &frame.tuning.fractions[0], &frame.tuning.fractions[1], &frame.tuning.fractions[2], &reading,
                          &count) == 12;
    frame.tuning.mode = static_cast<FrameGenSynthesisMode>(mode);
    frame.tuning.quality = static_cast<FrameGenFlowQuality>(quality);
    frame.tuning.hardware_flow = hardware != 0;
    frame.tuning.generated = static_cast<std::uint8_t>(generated);
    frame.views.resize(count);
    for (std::size_t i = 0; i < count && ok; ++i) {
        ReplayView& view = frame.views[i];
        int index = 0, output = 0, synthesize = 0, depth = 0, used = 0;
        ok = std::fscanf(file, " view %d extent %u %u half %u %u format %d output %d synthesize %d depth %d used %d shift %f %f %f",
                         &index, &view.width, &view.height, &view.half_width, &view.half_height, &view.format, &output,
                         &synthesize, &depth, &used, &view.shift[0], &view.shift[1], &view.shift[2]) == 13 &&
             geometry(view.newest) && geometry(view.previous);
    }
    std::fclose(file);
    for (std::size_t i = 0; i < count && ok; ++i) {
        ReplayView& view = frame.views[i];
        view.color.resize(std::size_t{view.width} * view.height);
        view.depth.resize(std::size_t{view.half_width} * view.half_height);
        ok = ReadBinary(directory + "/view" + std::to_string(i) + "-color.raw", view.color.data(), view.color.size() * 4) &&
             ReadBinary(directory + "/view" + std::to_string(i) + "-depth.f32", view.depth.data(), view.depth.size() * 4);
        if (view.format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || view.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            for (std::uint32_t& p : view.color) p = (p & 0xff00ff00u) | ((p & 0xffu) << 16) | ((p >> 16) & 0xffu);
        }
    }
    return ok;
}

// Half-resolution binary PPM of an RGBA image (2x2 box filter).
void WriteHalfPpm(const std::string& path, const Pixels& pixels, std::uint32_t width, std::uint32_t height) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const std::uint32_t w = width / 2, h = height / 2;
    std::fprintf(file, "P6\n%u %u\n255\n", w, h);
    std::vector<unsigned char> row(std::size_t{w} * 3);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                unsigned sum = 0;
                for (int i = 0; i < 4; ++i) {
                    sum += (pixels[std::size_t{2 * y + i / 2} * width + 2 * x + i % 2] >> (8 * c)) & 255u;
                }
                row[std::size_t{x} * 3 + c] = static_cast<unsigned char>(sum / 4);
            }
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

std::array<std::array<double, 3>, 3> ReplayRotation(const std::array<float, 4>& q) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

// `next` turned to `view`'s orientation (the runtime's correction), sampled
// bilinearly; `valid` marks pixels inside `next`'s view.
Pixels TurnTo(const ReplayView& view, const ReplayView& next, std::vector<bool>& valid) {
    const auto a = ReplayRotation(view.newest.orientation);
    const auto b = ReplayRotation(next.newest.orientation);
    const double l = std::tan(view.newest.angle_left), r = std::tan(view.newest.angle_right);
    const double u = std::tan(view.newest.angle_up), d = std::tan(view.newest.angle_down);
    const double nl = std::tan(next.newest.angle_left), nr = std::tan(next.newest.angle_right);
    const double nu = std::tan(next.newest.angle_up), nd = std::tan(next.newest.angle_down);
    Pixels out(view.color.size(), 0xff000000u);
    valid.assign(view.color.size(), false);
    auto rows = [&](std::uint32_t first, std::uint32_t step) {
        for (std::uint32_t y = first; y < view.height; y += step) {
            for (std::uint32_t x = 0; x < view.width; ++x) {
                const double dir[3] = {l + (r - l) * (x + 0.5) / view.width, u + (d - u) * (y + 0.5) / view.height, -1.0};
                double world[3], local[3];
                for (int i = 0; i < 3; ++i) world[i] = a[i][0] * dir[0] + a[i][1] * dir[1] + a[i][2] * dir[2];
                for (int i = 0; i < 3; ++i) local[i] = b[0][i] * world[0] + b[1][i] * world[1] + b[2][i] * world[2];
                if (local[2] > -1e-6) continue;
                const double sx = ((local[0] / -local[2]) - nl) / (nr - nl) * next.width - 0.5;
                const double sy = ((local[1] / -local[2]) - nu) / (nd - nu) * next.height - 0.5;
                if (sx < 0 || sy < 0 || sx > next.width - 1.001 || sy > next.height - 1.001) continue;
                const int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
                const double fx = sx - ix, fy = sy - iy;
                std::uint32_t pixel = 0xff000000u;
                for (int c = 0; c < 3; ++c) {
                    auto at = [&](int px, int py) {
                        return static_cast<double>((next.color[std::size_t{static_cast<std::uint32_t>(py)} * next.width + px] >> (8 * c)) & 255u);
                    };
                    const double value = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                                         (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
                    pixel |= static_cast<std::uint32_t>(std::lround(value)) << (8 * c);
                }
                const std::size_t index = std::size_t{y} * view.width + x;
                out[index] = pixel;
                valid[index] = true;
            }
        }
    };
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (std::uint32_t t = 0; t < threads; ++t) workers.emplace_back(rows, t, threads);
    for (auto& worker : workers) worker.join();
    return out;
}

// Mean error over valid pixels, all and split by the newest frame's depth
// (nearer than 4 m: cockpit).
struct ReplayError {
    double all{0}, cockpit{0}, outside{0};
};
ReplayError CompareTo(const Pixels& image, const Pixels& truth, const std::vector<bool>& valid, const ReplayView& view) {
    double sums[3] = {}, counts[3] = {};
    for (std::uint32_t y = 0; y < view.height; ++y) {
        const std::uint32_t hy = std::min(y / 2, view.half_height - 1);
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t index = std::size_t{y} * view.width + x;
            if (!valid[index]) continue;
            double error = 0;
            for (int c = 0; c < 3; ++c) {
                error += std::abs(static_cast<int>((image[index] >> (8 * c)) & 255u) - static_cast<int>((truth[index] >> (8 * c)) & 255u));
            }
            error /= 3 * 255.0;
            const float depth = std::abs(view.depth[std::size_t{hy} * view.half_width + std::min(x / 2, view.half_width - 1)]);
            const int part = depth > 0.0f && depth < 4.0f ? 1 : 2;
            sums[0] += error;
            counts[0] += 1;
            sums[part] += error;
            counts[part] += 1;
        }
    }
    return {counts[0] ? sums[0] / counts[0] : 0, counts[1] ? sums[1] / counts[1] : 0, counts[2] ? sums[2] / counts[2] : 0};
}

int RunReplay(Harness& harness) {
    std::vector<ReplayFrame> frames;
    for (int k = 0;; ++k) {
        ReplayFrame frame;
        if (!LoadReplayFrame(g_replay_directory + "/frame-" + std::to_string(k), frame)) break;
        frames.push_back(std::move(frame));
    }
    Check(frames.size() >= 3 && frames[0].views.size() >= 2, "replay: needs at least three captured stereo frames in " +
                                                                 g_replay_directory);
    const std::size_t view_count = frames[0].views.size();
    const ReplayView& first = frames[0].views[0];
    std::printf("Replaying %zu frames, %zu views, %ux%u (depth grid %ux%u)\n", frames.size(), view_count, first.width,
                first.height, first.half_width, first.half_height);
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, "replay backend: " + error);
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    std::vector<XrSwapchain> sources(view_count), depths(view_count), outputs(view_count);
    for (std::size_t v = 0; v < view_count; ++v) {
        const ReplayView& view = frames[0].views[v];
        sources[v] = reinterpret_cast<XrSwapchain>(0x100 + v);
        depths[v] = reinterpret_cast<XrSwapchain>(0x200 + v);
        outputs[v] = reinterpret_cast<XrSwapchain>(0x300 + v);
        g_swapchains[sources[v]].clear();
        g_swapchains[depths[v]].clear();
        for (const ReplayFrame& frame : frames) {
            void* image = harness.create(view.width, view.height);
            harness.upload_sized(image, view.width, view.height, frame.views[v].color);
            g_swapchains[sources[v]].push_back(image);
            // Depth for an encoding the depth pass decodes exactly:
            // 1/z = 100 * raw (near infinitely far, far 1 cm).
            std::vector<float> raw(frame.views[v].depth.size());
            for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = 0.01f / std::max(std::abs(frame.views[v].depth[i]), 0.01f);
            g_swapchains[depths[v]].push_back(harness.create_depth_data(view.half_width, view.half_height, raw));
        }
        g_swapchains[outputs[v]] = {harness.create(view.width, view.height)};
        Check(gpu->Track(sources[v], EnumerateImages, error) && gpu->Track(depths[v], EnumerateImages, error) &&
                  gpu->Track(outputs[v], EnumerateImages, error),
              error);
    }
    FrameGenDepthEncoding encoding;
    encoding.near_z = std::numeric_limits<float>::infinity();
    encoding.far_z = 0.01f;
    auto env = [](const char* name) {
        const char* value = std::getenv(name);
        return std::string(value ? value : "");
    };
    const std::string quality = env("FG_REPLAY_QUALITY");
    const bool hardware = env("FG_REPLAY_HARDWARE") == "1";
    const bool no_shift = env("FG_REPLAY_NOSHIFT") == "1";
    const bool no_depth = env("FG_REPLAY_NODEPTH") == "1";
    const bool predict = env("FG_REPLAY_PREDICT") == "1";

    // Frame k extrapolated to frame k + 1: one frame at the time ratio of
    // the two intervals, from the eye position frame k + 1 was rendered at.
    auto run = [&](std::size_t k, bool synthesize, bool evaluate) {
        const ReplayFrame& frame = frames[k];
        FrameGenTuning tuning = frame.tuning;
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.hardware_flow = hardware;
        if (quality == "performance") tuning.quality = FrameGenFlowQuality::kPerformance;
        if (quality == "balanced") tuning.quality = FrameGenFlowQuality::kBalanced;
        if (quality == "quality") tuning.quality = FrameGenFlowQuality::kQuality;
        tuning.generated = 1;
        const bool has_next = k + 1 < frames.size();
        tuning.fractions[0] = has_next ? std::clamp(frames[k + 1].tuning.interval_ratio, 0.25f, 1.0f) : 1.0f;
        std::vector<FrameGenViewSubmission> views(view_count);
        for (std::size_t v = 0; v < view_count; ++v) {
            const ReplayView& view = frame.views[v];
            FrameGenViewSubmission& submission = views[v];
            const XrRect2Di rect{{0, 0}, {static_cast<int>(view.width), static_cast<int>(view.height)}};
            submission.source = {sources[v], static_cast<std::uint32_t>(k), 0, rect, harness.format};
            submission.outputs = {{outputs[v], 0, 0, rect, output_format}};
            submission.request.extent = {view.width, view.height};
            submission.request.output = FrameGenOutputKind::kSrgb8;
            submission.request.synthesize = synthesize;
            submission.request.temporal = true;
            submission.request.depth = !no_depth;
            submission.request.newest = view.newest;
            submission.request.previous = synthesize ? view.previous : view.newest;
            if (has_next && !no_shift && (!predict || synthesize)) {
                // Where the eye was for frame k + 1, in frame k's view space;
                // predicting, on as it moved from frame k - 1, for as long as
                // frame k + 1 really came after (captures hitch, and the
                // fraction stops at one interval).
                const auto rotation = ReplayRotation(view.newest.orientation);
                const auto& to = predict ? view.newest.position : frames[k + 1].views[v].newest.position;
                const auto& from = predict ? view.previous.position : view.newest.position;
                const double delta[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
                for (int i = 0; i < 3; ++i) {
                    const double local = rotation[0][i] * delta[0] + rotation[1][i] * delta[1] + rotation[2][i] * delta[2];
                    submission.request.shift[i] = static_cast<float>(
                        local / tuning.fractions[0] * (predict ? frames[k + 1].tuning.interval_ratio : 1.0f));
                }
            }
            Check(gpu->Capture(v, submission.source, submission.request, tuning.quality, error) || error.empty(),
                  "replay capture: " + error);
            const XrRect2Di depth_rect{{0, 0}, {static_cast<int>(view.half_width), static_cast<int>(view.half_height)}};
            Check(gpu->CaptureDepth(v, {depths[v], static_cast<std::uint32_t>(k), 0, depth_rect, 0}, encoding,
                                    submission.request, error) ||
                      error.empty(),
                  "replay depth capture: " + error);
        }
        FrameGenExecuteResult result;
        Check(gpu->Execute(views, tuning, result, error), "replay execute: " + error);
        if (!evaluate || !has_next) {
            return;
        }
        for (std::size_t v = 0; v < view_count; ++v) {
            const ReplayView& view = frame.views[v];
            // Motion measured on the cockpit (depth nearer than 4 m), which
            // stands still once head movement is taken out: noise there is
            // jitter in generated frames.
            std::vector<float> xy;
            FrameGenExtent cells{};
            double cockpit_motion = 0.0, cockpit_cells = 0.0, cockpit_moving = 0.0;
            if (gpu->ReadFlow(v, xy, cells) && cells.width && cells.height) {
                for (std::uint32_t cy = 0; cy < cells.height; ++cy) {
                    for (std::uint32_t cx = 0; cx < cells.width; ++cx) {
                        const std::uint32_t hx = std::min(view.half_width - 1, (cx * 2 + 1) * view.half_width / (cells.width * 2));
                        const std::uint32_t hy = std::min(view.half_height - 1, (cy * 2 + 1) * view.half_height / (cells.height * 2));
                        const float depth = std::abs(view.depth[std::size_t{hy} * view.half_width + hx]);
                        if (!(depth > 0.0f && depth < 4.0f)) continue;
                        const std::size_t i = (std::size_t{cy} * cells.width + cx) * 2;
                        const double length = std::hypot(xy[i], xy[i + 1]);
                        cockpit_motion += length;
                        cockpit_moving += length > 0.5 ? 1.0 : 0.0;
                        cockpit_cells += 1.0;
                    }
                }
            }
            if (!g_replay_output.empty() && v == 0 && !xy.empty()) {
                // The flow as a little-endian float image (x, y per cell).
                std::FILE* file = std::fopen((g_replay_output + "/frame-" + std::to_string(k) + "-flow-" +
                                              std::to_string(cells.width) + "x" + std::to_string(cells.height) + ".f32")
                                                 .c_str(),
                                             "wb");
                if (file) {
                    std::fwrite(xy.data(), sizeof(float), xy.size(), file);
                    std::fclose(file);
                }
            }
            if (cockpit_cells > 0.0) {
                std::printf("frame %zu view %zu cockpit motion: mean %.3f px, %.1f%% of cells above 0.5 px\n", k, v,
                            cockpit_motion / cockpit_cells, 100.0 * cockpit_moving / cockpit_cells);
            }
            const Pixels generated = harness.readback_sized(g_swapchains[outputs[v]][0]);
            std::vector<bool> valid;
            const Pixels truth = TurnTo(view, frames[k + 1].views[v], valid);
            const ReplayError generated_error = CompareTo(generated, truth, valid, view);
            const ReplayError hold_error = CompareTo(view.color, truth, valid, view);
            std::printf("frame %zu view %zu (depth %s, %.2f of an interval): generated %.4f (cockpit %.4f, outside %.4f)"
                        "  hold %.4f (cockpit %.4f, outside %.4f)\n",
                        k, v, result.depth.size() > v && result.depth[v] ? "used" : "not used", tuning.fractions[0],
                        generated_error.all, generated_error.cockpit, generated_error.outside, hold_error.all,
                        hold_error.cockpit, hold_error.outside);
            if (!g_replay_output.empty() && v == 0) {
                const std::string prefix = g_replay_output + "/frame-" + std::to_string(k) + "-view" + std::to_string(v);
                WriteHalfPpm(prefix + "-generated.ppm", generated, view.width, view.height);
                WriteHalfPpm(prefix + "-truth.ppm", truth, view.width, view.height);
                WriteHalfPpm(prefix + "-newest.ppm", view.color, view.width, view.height);
            }
        }
    };
    // Three times through to settle the depth check, then once to measure.
    for (int pass = 0; pass < 4; ++pass) {
        gpu->ResetHistory();
        for (std::size_t k = 0; k < frames.size(); ++k) {
            run(k, k > 0, pass == 3 && k > 0);
        }
        if (pass == 2) {
            std::printf("Depth reading: %s, scale %.3f\n", ToString(gpu->DepthStatus().reading),
                        static_cast<double>(gpu->DepthStatus().scale));
        }
    }
    gpu->Reset();
    return 0;
}

#ifdef DEPTHXR_NVOF_RESEARCH
// Research: NVIDIA's optical flow accelerator (NVOFA, RTX 20-50) against our
// block matching, on the scenes the scenarios use and at headset size.
// `depthxr_framegen_tests nvof` on an NVIDIA GPU; built only with the SDK.
bool g_nvof = false;

std::vector<std::uint8_t> Gray(const Pixels& pixels) {
    std::vector<std::uint8_t> gray(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const std::uint32_t p = pixels[i];
        gray[i] = static_cast<std::uint8_t>(std::lround(0.2126 * (p & 255) + 0.7152 * ((p >> 8) & 255) +
                                                        0.0722 * ((p >> 16) & 255)));
    }
    return gray;
}

class NvofSession {
  public:
    ~NvofSession() {
        if (handle_) {
            Wait(fence_value_);
            for (auto buffer : registered_) {
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{buffer};
                const NV_OF_STATUS status = api_.nvOFUnregisterResourceD3D12(&params);
                if (std::getenv("FG_TEST_VERBOSE")) std::printf("    unregister %d\n", static_cast<int>(status));
            }
            const NV_OF_STATUS status = api_.nvOFDestroy(handle_);
            if (std::getenv("FG_TEST_VERBOSE")) std::printf("    destroy %d\n", static_cast<int>(status));
            // Releasing the registered images or the fence after the session
            // is destroyed crashes in the driver (5.0.7 on a 5090); a
            // research run leaks them instead.
            for (auto& input : inputs_) input.Detach();
            output_.Detach();
            fence_.Detach();
        }
    }

    bool Create(ID3D12Device* device, ID3D12CommandQueue* queue, std::uint32_t width, std::uint32_t height, int inputs,
                NV_OF_OUTPUT_VECTOR_GRID_SIZE grid, NV_OF_PERF_LEVEL perf, std::string& error) {
        device_ = device;
        queue_ = queue;
        width_ = width;
        height_ = height;
        grid_ = grid;
        if (std::getenv("FG_TEST_VERBOSE")) std::printf("    loading\n");
        const HMODULE library = LoadLibraryW(L"nvofapi64.dll");
        auto create = library ? reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
                                    GetProcAddress(library, "NvOFAPICreateInstanceD3D12"))
                              : nullptr;
        if (!create || create(NV_OF_API_VERSION, &api_) != NV_OF_SUCCESS) {
            error = "nvofapi64.dll / NvOFAPICreateInstanceD3D12 unavailable";
            return false;
        }
        if (std::getenv("FG_TEST_VERBOSE")) std::printf("    create %ux%u grid %d\n", width, height, static_cast<int>(grid));
        if (api_.nvCreateOpticalFlowD3D12(device, &handle_) != NV_OF_SUCCESS) {
            error = "nvCreateOpticalFlowD3D12 failed";
            return false;
        }
        if (std::getenv("FG_TEST_VERBOSE")) std::printf("    created\n");
        NV_OF_INIT_PARAMS init{};
        init.width = width;
        init.height = height;
        init.outGridSize = grid;
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = perf;
        init.enableExternalHints = NV_OF_FALSE;
        init.enableOutputCost = NV_OF_FALSE;
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        if (api_.nvOFInit(handle_, &init) != NV_OF_SUCCESS) {
            error = "nvOFInit failed: " + LastError();
            return false;
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            error = "fence";
            return false;
        }
        for (int i = 0; i < inputs; ++i) {
            ComPtr<ID3D12Resource> input;
            NvOFGPUBufferHandle buffer{};
            if (!CreateTexture(width, height, DXGI_FORMAT_R8_UNORM, input, buffer, error)) return false; // GRAYSCALE8
            inputs_.push_back(input);
            input_handles_.push_back(buffer);
        }
        output_width_ = (width + grid - 1) / grid;
        output_height_ = (height + grid - 1) / grid;
        return CreateTexture(output_width_, output_height_, DXGI_FORMAT_R16G16_SINT, output_, output_handle_, error); // SHORT2
    }

    // Copies an 8-bit gray image into input `slot` on the queue.
    void Upload(int slot, const std::vector<std::uint8_t>& gray) {
        Copy(inputs_[slot].Get(), const_cast<std::uint8_t*>(gray.data()), 1, true);
    }

    // Flow from input `input` to input `reference`, waiting for it; `xy`
    // receives the vectors in pixels.
    bool Run(int input, int reference, bool temporal, std::vector<float>* xy) {
        Execute(input, reference, temporal);
        Wait(fence_value_);
        if (xy) {
            std::vector<std::int16_t> raw(std::size_t{output_width_} * output_height_ * 2);
            Copy(output_.Get(), raw.data(), 4, false);
            xy->resize(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                (*xy)[i] = raw[i] / 32.0f; // S10.5
            }
        }
        return true;
    }

    // Average milliseconds per execute, `count` back to back.
    double Time(int count) {
        Run(0, 1, true, nullptr);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
            Execute(i & 1, (i + 1) & 1, true);
        }
        Wait(fence_value_);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / count;
    }

    std::uint32_t OutputWidth() const { return output_width_; }
    std::uint32_t OutputHeight() const { return output_height_; }

  private:
    std::string LastError() {
        char text[MIN_ERROR_STRING_SIZE * 4] = {};
        std::uint32_t size = sizeof(text);
        api_.nvOFGetLastError(handle_, text, &size);
        return text;
    }

    bool CreateTexture(std::uint32_t width, std::uint32_t height, DXGI_FORMAT format, ComPtr<ID3D12Resource>& texture,
                       NvOFGPUBufferHandle& buffer, std::string& error) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        if (format == DXGI_FORMAT_UNKNOWN ||
            FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                    IID_PPV_ARGS(&texture)))) {
            error = "NVOF texture of format " + std::to_string(format);
            return false;
        }
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
        params.resource = texture.Get();
        params.inputFencePoint = {fence_.Get(), fence_value_};
        params.hOFGpuBuffer = &buffer;
        params.outputFencePoint = {fence_.Get(), ++fence_value_};
        if (api_.nvOFRegisterResourceD3D12(handle_, &params) != NV_OF_SUCCESS) {
            error = "nvOFRegisterResourceD3D12: " + LastError();
            return false;
        }
        registered_.push_back(buffer);
        Wait(fence_value_);
        return true;
    }

    void Execute(int input, int reference, bool temporal) {
        NV_OF_FENCE_POINT ready{fence_.Get(), fence_value_};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
        in.inputFrame = input_handles_[input];
        in.referenceFrame = input_handles_[reference];
        in.disableTemporalHints = temporal ? NV_OF_FALSE : NV_OF_TRUE;
        in.numFencePoints = 1;
        in.fencePoint = &ready;
        NV_OF_FENCE_POINT done{fence_.Get(), ++fence_value_};
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
        out.outputBuffer = output_handle_;
        out.fencePoint = &done;
        if (api_.nvOFExecuteD3D12(handle_, &in, &out) != NV_OF_SUCCESS) {
            std::cerr << "nvOFExecuteD3D12: " << LastError() << '\n';
        }
    }

    void Wait(UINT64 value) {
        while (fence_->GetCompletedValue() < value) {
            Sleep(0);
        }
    }

    // Uploads `data` into or reads it from `texture` (texel size `texel`) on
    // the queue, ordered with the optical flow work by the fence.
    void Copy(ID3D12Resource* texture, void* data, UINT texel, bool upload) {
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = bytes;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> buffer;
        Check(SUCCEEDED(device_->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                  upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))),
              "NVOF staging buffer");
        const std::size_t row_bytes = std::size_t{desc.Width} * texel;
        std::byte* mapped = nullptr;
        if (upload) {
            Check(SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "map");
            for (UINT y = 0; y < desc.Height; ++y) {
                std::memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                            static_cast<const std::byte*>(data) + y * row_bytes, row_bytes);
            }
            buffer->Unmap(0, nullptr);
        }
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                  SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&list))),
              "NVOF copy list");
        D3D12_TEXTURE_COPY_LOCATION texture_location{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION buffer_location{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        buffer_location.PlacedFootprint = footprint;
        // The optical flow engine shares images in COMMON.
        const D3D12_RESOURCE_STATES copy_state = upload ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, copy_state};
        list->ResourceBarrier(1, &barrier);
        if (upload) {
            list->CopyTextureRegion(&texture_location, 0, 0, 0, &buffer_location, nullptr);
        } else {
            list->CopyTextureRegion(&buffer_location, 0, 0, 0, &texture_location, nullptr);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        Check(SUCCEEDED(list->Close()), "NVOF copy close");
        queue_->Wait(fence_.Get(), fence_value_);
        ID3D12CommandList* lists[] = {list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fence_value_);
        Wait(fence_value_);
        if (!upload) {
            Check(SUCCEEDED(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "map");
            for (UINT y = 0; y < desc.Height; ++y) {
                std::memcpy(static_cast<std::byte*>(data) + y * row_bytes,
                            mapped + footprint.Offset + y * footprint.Footprint.RowPitch, row_bytes);
            }
            buffer->Unmap(0, nullptr);
        }
    }

    NV_OF_D3D12_API_FUNCTION_LIST api_{};
    NvOFHandle handle_{};
    ID3D12Device* device_{nullptr};
    ID3D12CommandQueue* queue_{nullptr};
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_{0};
    std::uint32_t width_{0}, height_{0}, output_width_{0}, output_height_{0};
    NV_OF_OUTPUT_VECTOR_GRID_SIZE grid_{NV_OF_OUTPUT_VECTOR_GRID_SIZE_4};
    std::vector<ComPtr<ID3D12Resource>> inputs_;
    std::vector<NvOFGPUBufferHandle> input_handles_;
    ComPtr<ID3D12Resource> output_;
    NvOFGPUBufferHandle output_handle_{};
    std::vector<NvOFGPUBufferHandle> registered_;
};

// Mean endpoint error (pixels) of a flow field against `truth(x, y)` over
// cells whose centre lies in [margin, size - margin).
template <typename Truth>
double EndpointError(const std::vector<float>& xy, std::uint32_t width, std::uint32_t height, std::uint32_t cell,
                     std::uint32_t margin, Truth truth, double* within_half = nullptr) {
    double sum = 0.0, good = 0.0;
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float cx = (x + 0.5f) * cell, cy = (y + 0.5f) * cell;
            if (cx < margin || cy < margin || cx >= kSize - margin || cy >= kSize - margin) continue;
            const auto [tx, ty] = truth(cx, cy);
            const double error = std::hypot(xy[(std::size_t{y} * width + x) * 2] - tx, xy[(std::size_t{y} * width + x) * 2 + 1] - ty);
            sum += error;
            good += error < 0.5 ? 1.0 : 0.0;
            ++count;
        }
    }
    if (within_half) *within_half = count ? good / count : 0.0;
    return count ? sum / count : -1.0;
}

int RunNvof(Harness& harness) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto* binding = static_cast<const XrGraphicsBindingD3D12KHR*>(harness.binding);
    std::string error;
    struct Scene {
        const char* name;
        std::vector<Pixels> frames; // previous ... newest
        std::function<std::pair<float, float>(float, float)> motion; // newest frame's motion at a pixel
    };
    const auto square_motion = [](float x, float y) {
        const bool inside = x >= 136 && x < 232 && y >= 128 && y < 224;
        return inside ? std::make_pair(16.0f, 8.0f) : std::make_pair(0.0f, 0.0f);
    };
    std::vector<Scene> scenes;
    scenes.push_back({"facade (3 px/frame, 12 px windows)", {Facade(0), Facade(3), Facade(6)},
                      [](float, float) { return std::make_pair(3.0f, 0.0f); }});
    scenes.push_back({"moving square (16, 8)", {MovingSquare(104, 112), MovingSquare(120, 120), MovingSquare(136, 128)},
                      square_motion});
    scenes.push_back({"shimmering still panel", {Shimmer(0), Shimmer(1), Shimmer(2)},
                      [](float, float) { return std::make_pair(0.0f, 0.0f); }});

    std::printf("Flow accuracy at %ux%u: mean endpoint error in pixels (share of cells within 0.5 px)\n", kSize, kSize);
    for (const Scene& scene : scenes) {
        std::printf("\n%s\n", scene.name);
        // Ours: the field synthesis uses, after the last frame pair.
        for (const auto quality : {FrameGenFlowQuality::kBalanced, FrameGenFlowQuality::kQuality}) {
            auto gpu = CreateFrameGenGpu(harness.binding, error);
            Check(gpu != nullptr, error);
            const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
            g_swapchains[kSource] = {harness.create(kSize, kSize), harness.create(kSize, kSize), harness.create(kSize, kSize)};
            g_swapchains[kOutput] = {harness.create(kSize, kSize)};
            g_swapchains[kOutputSecond] = {harness.create(kSize, kSize)};
            Check(gpu->Track(kSource, EnumerateImages, error) && gpu->Track(kOutput, EnumerateImages, error) &&
                      gpu->Track(kOutputSecond, EnumerateImages, error),
                  error);
            FrameGenTuning tuning;
            tuning.quality = quality;
            tuning.mode = FrameGenSynthesisMode::kExtrapolate;
            FrameGenExecuteResult result;
            for (std::uint32_t i = 0; i < scene.frames.size(); ++i) {
                harness.upload(g_swapchains[kSource][i], scene.frames[i]);
                Check(gpu->Execute({Submission(i, harness.format, output_format, true, 0, 0)}, tuning, result, error), error);
            }
            std::vector<float> xy;
            FrameGenExtent cells;
            Check(gpu->ReadFlow(0, xy, cells), "read flow");
            if (std::getenv("FG_TEST_VERBOSE")) {
                const std::size_t middle = (std::size_t{cells.height} / 2 * cells.width + cells.width / 2) * 2;
                std::printf("    centre cell flow (%.3f, %.3f), first (%.3f, %.3f)\n", xy[middle], xy[middle + 1], xy[0], xy[1]);
            }
            double within = 0.0;
            const double epe = EndpointError(xy, cells.width, cells.height, kSize / cells.width, 32, scene.motion, &within);
            std::printf("  ours %-12s (%ux%u cells)          %6.3f px (%3.0f%%)\n",
                        quality == FrameGenFlowQuality::kQuality ? "Quality" : "Balanced", cells.width, cells.height, epe,
                        100.0 * within);
            gpu->Reset();
        }
        // NVOFA: forward flow from the newest frame to the one before it is
        // minus the motion; the last pair runs with the first pair's
        // vectors as temporal hints, as a frame sequence would.
        for (const auto grid : {NV_OF_OUTPUT_VECTOR_GRID_SIZE_1, NV_OF_OUTPUT_VECTOR_GRID_SIZE_4}) {
            for (const auto perf : {NV_OF_PERF_LEVEL_SLOW, NV_OF_PERF_LEVEL_FAST}) {
                NvofSession session;
                if (!session.Create(binding->device, binding->queue, kSize, kSize, 3, grid, perf, error)) {
                    std::printf("  NVOFA unavailable: %s\n", error.c_str());
                    return 0;
                }
                for (int i = 0; i < 3; ++i) {
                    session.Upload(i, Gray(scene.frames[i]));
                }
                std::vector<float> xy;
                session.Run(1, 0, false, nullptr);
                session.Run(2, 1, true, &xy);
                for (float& v : xy) v = -v;
                double within = 0.0;
                const double epe = EndpointError(xy, session.OutputWidth(), session.OutputHeight(), grid, 32, scene.motion,
                                                 &within);
                std::printf("  NVOFA grid %d %-6s (%ux%u cells)    %6.3f px (%3.0f%%)\n", static_cast<int>(grid),
                            perf == NV_OF_PERF_LEVEL_SLOW ? "slow" : "fast", session.OutputWidth(), session.OutputHeight(),
                            epe, 100.0 * within);
            }
        }
    }

    // Half resolution keeps NVOFA affordable at headset size, but halves the
    // window period too: the facade at 192x192 (box-filtered), 1.5 px/frame.
    {
        auto half = [](const Pixels& pixels) {
            std::vector<std::uint8_t> gray = Gray(pixels), out(kSize / 2 * (kSize / 2));
            for (std::uint32_t y = 0; y < kSize / 2; ++y) {
                for (std::uint32_t x = 0; x < kSize / 2; ++x) {
                    const std::size_t i = std::size_t{y} * 2 * kSize + x * 2;
                    out[std::size_t{y} * (kSize / 2) + x] =
                        static_cast<std::uint8_t>((gray[i] + gray[i + 1] + gray[i + kSize] + gray[i + kSize + 1] + 2) / 4);
                }
            }
            return out;
        };
        std::printf("\nfacade at half resolution (NVOFA input 192x192, 6 px windows)\n");
        for (const auto grid : {NV_OF_OUTPUT_VECTOR_GRID_SIZE_2, NV_OF_OUTPUT_VECTOR_GRID_SIZE_4}) {
            for (const auto perf : {NV_OF_PERF_LEVEL_SLOW, NV_OF_PERF_LEVEL_MEDIUM}) {
                NvofSession session;
                if (!session.Create(binding->device, binding->queue, kSize / 2, kSize / 2, 3, grid, perf, error)) {
                    std::printf("  NVOFA unavailable: %s\n", error.c_str());
                    break;
                }
                session.Upload(0, half(Facade(0)));
                session.Upload(1, half(Facade(3)));
                session.Upload(2, half(Facade(6)));
                std::vector<float> xy;
                session.Run(1, 0, false, nullptr);
                session.Run(2, 1, true, &xy);
                // Back to full-resolution pixels.
                for (float& v : xy) v = -2.0f * v;
                double within = 0.0;
                const double epe = EndpointError(xy, session.OutputWidth(), session.OutputHeight(), 2 * grid, 32,
                                                 [](float, float) { return std::make_pair(3.0f, 0.0f); }, &within);
                std::printf("  NVOFA grid %d %-6s (%ux%u cells)    %6.3f px (%3.0f%%)\n", static_cast<int>(grid),
                            perf == NV_OF_PERF_LEVEL_SLOW ? "slow" : "medium", session.OutputWidth(), session.OutputHeight(),
                            epe, 100.0 * within);
            }
        }
    }

    // Speed at a Pimax Crystal Super's 6240x6280 per eye: full resolution with
    // a 4x4 grid (Quality's flow resolution) and half resolution with 2x2.
    std::printf("\nNVOFA time per eye (wall clock, back to back):\n");
    struct Size {
        std::uint32_t width, height;
        NV_OF_OUTPUT_VECTOR_GRID_SIZE grid;
    };
    for (const Size size : {Size{6240, 6280, NV_OF_OUTPUT_VECTOR_GRID_SIZE_4}, Size{3120, 3140, NV_OF_OUTPUT_VECTOR_GRID_SIZE_2},
                            Size{3120, 3140, NV_OF_OUTPUT_VECTOR_GRID_SIZE_4}}) {
        for (const auto perf : {NV_OF_PERF_LEVEL_SLOW, NV_OF_PERF_LEVEL_MEDIUM, NV_OF_PERF_LEVEL_FAST}) {
            NvofSession session;
            if (!session.Create(binding->device, binding->queue, size.width, size.height, 2, size.grid, perf, error)) {
                std::printf("  %ux%u: %s\n", size.width, size.height, error.c_str());
                continue;
            }
            std::vector<std::uint8_t> gray(std::size_t{size.width} * size.height);
            for (int frame = 0; frame < 2; ++frame) {
                for (std::uint32_t y = 0; y < size.height; ++y) {
                    for (std::uint32_t x = 0; x < size.width; ++x) {
                        gray[std::size_t{y} * size.width + x] =
                            static_cast<std::uint8_t>(255.0f * Noise((x + 6.0f * frame) / 9.0f, y / 9.0f));
                    }
                }
                session.Upload(frame, gray);
            }
            std::printf("  %ux%u grid %d %-6s %6.2f ms\n", size.width, size.height, static_cast<int>(size.grid),
                        perf == NV_OF_PERF_LEVEL_SLOW ? "slow" : perf == NV_OF_PERF_LEVEL_MEDIUM ? "medium" : "fast",
                        session.Time(20));
        }
    }
    return 0;
}
#endif

// --- Direct3D 11 ------------------------------------------------------------

int D3D11Test() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, g_bench || g_hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
                                      &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context)),
          "D3D11 device");
    std::vector<ComPtr<ID3D11Texture2D>> keep;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    g_image_type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    Harness harness;
    harness.name = "D3D11";
    harness.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    harness.binding = &binding;
    harness.create = [&](std::uint32_t w, std::uint32_t h) -> void* {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; // runtimes commonly hand out typeless images
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)), "D3D11 test texture");
        keep.push_back(texture);
        return texture.Get();
    };
    harness.upload = [&](void* texture, const Pixels& pixels) {
        context->UpdateSubresource(static_cast<ID3D11Texture2D*>(texture), 0, nullptr, pixels.data(), kSize * 4, 0);
    };
    harness.readback = [&](void* texture) {
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D*>(texture)->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "D3D11 staging");
        context->CopyResource(staging.Get(), static_cast<ID3D11Texture2D*>(texture));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 map");
        Pixels pixels(kSize * kSize);
        for (std::uint32_t y = 0; y < kSize; ++y) {
            std::memcpy(&pixels[y * kSize], static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch, kSize * 4);
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    };
    harness.flush = [&] { context->Flush(); };
    return g_bench ? RunBench(harness) : RunScenarios(harness);
}

// --- Direct3D 12 ------------------------------------------------------------

int D3D12Test() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    if (g_bench || g_hardware) {
        Check(SUCCEEDED(factory->EnumAdapters(0, &warp)), "hardware adapter");
    } else {
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
    }
    Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12 WARP device");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Check(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "D3D12 queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "D3D12 allocator");
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))),
          "D3D12 list");
    Check(SUCCEEDED(commands->Close()), "D3D12 close");
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "D3D12 fence");
    UINT64 fence_value = 0;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto submit_and_wait = [&] {
        Check(SUCCEEDED(commands->Close()), "D3D12 test close");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        Check(SUCCEEDED(queue->Signal(fence.Get(), ++fence_value)), "D3D12 signal");
        Check(SUCCEEDED(fence->SetEventOnCompletion(fence_value, event)), "D3D12 event");
        Check(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "D3D12 GPU timeout");
    };
    auto begin = [&] {
        Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(commands->Reset(allocator.Get(), nullptr)), "D3D12 reset");
    };
    std::vector<ComPtr<ID3D12Resource>> keep;
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device.Get();
    binding.queue = queue.Get();
    g_image_type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    D3D12_RESOURCE_DESC image_desc{};
    image_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image_desc.Width = kSize;
    image_desc.Height = kSize;
    image_desc.DepthOrArraySize = 1;
    image_desc.MipLevels = 1;
    image_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    image_desc.SampleDesc.Count = 1;
    image_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{type};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource))),
              "D3D12 buffer");
        return resource;
    };
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        commands->ResourceBarrier(1, &barrier);
    };
    Harness harness;
    harness.name = "D3D12";
    harness.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    harness.binding = &binding;
    harness.create = [&](std::uint32_t w, std::uint32_t h) -> void* {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc = image_desc;
        desc.Width = w;
        desc.Height = h;
        ComPtr<ID3D12Resource> resource;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&resource))),
              "D3D12 image");
        keep.push_back(resource);
        return resource.Get();
    };
    harness.upload = [&](void* image, const Pixels& pixels) {
        auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "D3D12 upload map");
        for (std::uint32_t y = 0; y < kSize; ++y) {
            std::memcpy(static_cast<std::byte*>(mapped) + y * footprint.Footprint.RowPitch, &pixels[y * kSize], kSize * 4);
        }
        upload->Unmap(0, nullptr);
        begin();
        auto* resource = static_cast<ID3D12Resource*>(image);
        transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit_and_wait();
    };
    harness.upload_sized = [&](void* image, std::uint32_t width, std::uint32_t height, const Pixels& pixels) {
        auto* resource = static_cast<ID3D12Resource*>(image);
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sized{};
        UINT64 size_bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &sized, nullptr, nullptr, &size_bytes);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = size_bytes;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))),
              "D3D12 upload buffer");
        void* mapped = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "D3D12 upload map");
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(static_cast<std::byte*>(mapped) + y * sized.Footprint.RowPitch, &pixels[std::size_t{y} * width],
                        std::size_t{width} * 4);
        }
        upload->Unmap(0, nullptr);
        begin();
        transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = sized;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit_and_wait();
    };
    std::vector<ComPtr<ID3D12DescriptorHeap>> keep_heaps;
    std::uint32_t depth_width = kSize, depth_height = kSize;
    harness.create_depth = [&](float near_value, XrRect2Di rect, float far_value) -> void* {
        // The format MSFS uses: 32-bit float depth with stencil.
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = depth_width;
        desc.Height = depth_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = desc.Format;
        clear.DepthStencil = {far_value, 0};
        ComPtr<ID3D12Resource> resource;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                        &clear, IID_PPV_ARGS(&resource))),
              "D3D12 depth image");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ComPtr<ID3D12DescriptorHeap> dsv_heap;
        Check(SUCCEEDED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap))), "D3D12 DSV heap");
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(resource.Get(), nullptr, dsv);
        begin();
        commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, far_value, 0, 0, nullptr);
        const D3D12_RECT area{rect.offset.x, rect.offset.y, rect.offset.x + rect.extent.width,
                              rect.offset.y + rect.extent.height};
        commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, near_value, 0, 1, &area);
        submit_and_wait();
        keep.push_back(resource);
        keep_heaps.push_back(dsv_heap);
        return resource.Get();
    };
    harness.create_depth_sized = [&](std::uint32_t width, std::uint32_t height, float near_value, XrRect2Di rect,
                                     float far_value) -> void* {
        depth_width = width;
        depth_height = height;
        void* image = harness.create_depth(near_value, rect, far_value);
        depth_width = depth_height = kSize;
        return image;
    };
    harness.readback = [&](void* image) {
        auto readback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        begin();
        auto* resource = static_cast<ID3D12Resource*>(image);
        transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit_and_wait();
        void* mapped = nullptr;
        Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "D3D12 readback map");
        Pixels pixels(kSize * kSize);
        for (std::uint32_t y = 0; y < kSize; ++y) {
            std::memcpy(&pixels[y * kSize], static_cast<const std::byte*>(mapped) + y * footprint.Footprint.RowPitch, kSize * 4);
        }
        readback->Unmap(0, nullptr);
        return pixels;
    };
    harness.create_depth_data = [&](std::uint32_t width, std::uint32_t height, const std::vector<float>& values) -> void* {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> resource;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&resource))),
              "D3D12 depth data image");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sized{};
        UINT64 size_bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &sized, nullptr, nullptr, &size_bytes);
        D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = size_bytes;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        Check(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))),
              "D3D12 depth upload buffer");
        void* mapped = nullptr;
        Check(SUCCEEDED(upload->Map(0, nullptr, &mapped)), "D3D12 depth upload map");
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(static_cast<std::byte*>(mapped) + y * sized.Footprint.RowPitch, &values[std::size_t{y} * width],
                        std::size_t{width} * 4);
        }
        upload->Unmap(0, nullptr);
        begin();
        D3D12_TEXTURE_COPY_LOCATION dst{resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = sized;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        submit_and_wait();
        keep.push_back(resource);
        return resource.Get();
    };
    harness.readback_sized = [&](void* image) {
        auto* resource = static_cast<ID3D12Resource*>(image);
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT sized{};
        UINT64 size_bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &sized, nullptr, nullptr, &size_bytes);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = size_bytes;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))),
              "D3D12 readback buffer");
        begin();
        transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = sized;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit_and_wait();
        void* mapped = nullptr;
        Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "D3D12 readback map");
        const std::uint32_t width = static_cast<std::uint32_t>(desc.Width), height = desc.Height;
        Pixels pixels(std::size_t{width} * height);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(&pixels[std::size_t{y} * width], static_cast<const std::byte*>(mapped) + y * sized.Footprint.RowPitch,
                        std::size_t{width} * 4);
        }
        readback->Unmap(0, nullptr);
        return pixels;
    };
#ifdef DEPTHXR_NVOF_RESEARCH
    if (g_nvof) {
        const int nvof = RunNvof(harness);
        CloseHandle(event);
        return nvof;
    }
#endif
    const int result = g_replay    ? RunReplay(harness)
                       : g_profile ? RunProfile(harness)
                       : g_bench   ? RunBench(harness)
                                   : RunScenarios(harness);
    CloseHandle(event);
    return result;
}

// --- Vulkan -------------------------------------------------------------------

#define VK_TEST_FUNCTIONS(X)                                                                                             \
    X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties)            \
    X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer)      \
    X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool)                   \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer)          \
    X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage) X(CreateFence) X(DestroyFence) X(ResetFences)   \
    X(WaitForFences) X(QueueSubmit) X(DeviceWaitIdle)

int VulkanTest() {
    const HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) {
        std::cout << "Vulkan loader absent; skipping\n";
        return kSkip;
    }
#define LOAD(name)                                                                       \
    auto vk##name = reinterpret_cast<PFN_vk##name>(GetProcAddress(loader, "vk" #name)); \
    Check(vk##name != nullptr, "Vulkan entry " #name);
    VK_TEST_FUNCTIONS(LOAD)
#undef LOAD
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VectorXR Frame Generation backend test";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        std::cout << "No Vulkan instance; skipping\n";
        return kSkip;
    }
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) {
        std::cout << "No Vulkan device; skipping\n";
        return kSkip;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    const VkPhysicalDevice physical = devices.front();
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    Check(family < count, "Vulkan graphics family");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice device = VK_NULL_HANDLE;
    Check(vkCreateDevice(physical, &device_info, nullptr, &device) == VK_SUCCESS, "Vulkan device");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    auto memory_type = [&](std::uint32_t bits, VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        Check(false, "Vulkan memory type");
        return 0u;
    };
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS, "Vulkan pool");
    VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    buffer_info.commandPool = pool;
    buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    buffer_info.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Check(vkAllocateCommandBuffers(device, &buffer_info, &commands) == VK_SUCCESS, "Vulkan command buffer");
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    Check(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS, "Vulkan fence");
    auto begin = [&] {
        Check(vkResetCommandPool(device, pool, 0) == VK_SUCCESS, "Vulkan reset pool");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(commands, &info) == VK_SUCCESS, "Vulkan begin");
    };
    auto submit = [&] {
        Check(vkEndCommandBuffer(commands) == VK_SUCCESS, "Vulkan end");
        Check(vkResetFences(device, 1, &fence) == VK_SUCCESS, "Vulkan reset fence");
        VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &commands;
        Check(vkQueueSubmit(queue, 1, &submit_info, fence) == VK_SUCCESS, "Vulkan submit");
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, 30'000'000'000ull) == VK_SUCCESS, "Vulkan wait");
    };
    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
    };
    VkBufferCreateInfo staging_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    staging_info.size = kSize * kSize * 4;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    Check(vkCreateBuffer(device, &staging_info, nullptr, &staging) == VK_SUCCESS, "Vulkan staging buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, staging, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    Check(vkAllocateMemory(device, &allocate, nullptr, &staging_memory) == VK_SUCCESS &&
              vkBindBufferMemory(device, staging, staging_memory, 0) == VK_SUCCESS,
          "Vulkan staging memory");
    void* mapped = nullptr;
    Check(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS, "Vulkan map");

    struct OwnedImage {
        VkImage image;
        VkDeviceMemory memory;
    };
    std::vector<OwnedImage> owned;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical;
    binding.device = device;
    binding.queueFamilyIndex = family;
    binding.queueIndex = 0;
    g_image_type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    Harness harness;
    harness.name = "Vulkan";
    harness.format = VK_FORMAT_R8G8B8A8_SRGB;
    harness.binding = &binding;
    harness.create = [&](std::uint32_t w, std::uint32_t h) -> void* {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_SRGB;
        info.extent = {w, h, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = VK_NULL_HANDLE;
        Check(vkCreateImage(device, &info, nullptr, &image) == VK_SUCCESS, "Vulkan image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        Check(vkAllocateMemory(device, &alloc, nullptr, &image_memory) == VK_SUCCESS &&
                  vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS,
              "Vulkan image memory");
        owned.push_back({image, image_memory});
        begin();
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        submit();
        return reinterpret_cast<void*>(image);
    };
    harness.upload = [&](void* handle, const Pixels& pixels) {
        std::memcpy(mapped, pixels.data(), pixels.size() * 4);
        const VkImage image = reinterpret_cast<VkImage>(handle);
        begin();
        barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kSize, kSize, 1};
        vkCmdCopyBufferToImage(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        submit();
    };
    harness.readback = [&](void* handle) {
        Check(vkDeviceWaitIdle(device) == VK_SUCCESS, "Vulkan idle");
        const VkImage image = reinterpret_cast<VkImage>(handle);
        begin();
        barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
        barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        submit();
        Pixels pixels(kSize * kSize);
        std::memcpy(pixels.data(), mapped, pixels.size() * 4);
        return pixels;
    };
    const int result = g_bench ? RunBench(harness) : RunScenarios(harness);
    vkDeviceWaitIdle(device);
    for (const auto& image : owned) {
        vkDestroyImage(device, image.image, nullptr);
        vkFreeMemory(device, image.memory, nullptr);
    }
    vkUnmapMemory(device, staging_memory);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    FreeLibrary(loader);
    return result;
}

} // namespace

int main(int argc, char** argv) {
    std::string api = argc > 1 ? argv[1] : "d3d11";
    if (api.rfind("bench-", 0) == 0) {
        g_bench = true;
        api = api.substr(6);
    }
    if (api.rfind("hw-", 0) == 0) {
        // The scenarios on the GPU instead of WARP / the software device.
        g_hardware = true;
        api = api.substr(3);
    }
#ifdef DEPTHXR_NVOF_RESEARCH
    if (api == "nvof") {
        g_nvof = g_hardware = true;
        api = "d3d12";
    }
#endif
    if (api == "replay") {
        // depthxr_framegen_tests replay <capture directory> [output directory]
        Check(argc > 2, "replay needs a capture directory");
        g_replay = g_hardware = true;
        g_replay_directory = argv[2];
        g_replay_output = argc > 3 ? argv[3] : "";
        api = "d3d12";
    }
    if (api == "profile") {
        // depthxr_framegen_tests profile [width height]: Direct3D 12 on the GPU.
        g_bench = g_profile = true;
        api = "d3d12";
        if (argc > 3) {
            g_profile_width = static_cast<std::uint32_t>(std::atoi(argv[2]));
            g_profile_height = static_cast<std::uint32_t>(std::atoi(argv[3]));
        }
    }
    if (api == "d3d12") return D3D12Test();
    if (api == "vulkan") return VulkanTest();
    return D3D11Test();
}
