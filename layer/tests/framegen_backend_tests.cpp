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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace depthxr;
using Microsoft::WRL::ComPtr;

namespace {

constexpr std::uint32_t kSize = 384;
constexpr int kSkip = 77;

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

// A panorama at infinity rendered through a camera yawed by `yaw` radians.
constexpr float kTan = 0.8f;
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

    gpu->Reset();
    std::cout << harness.name << " Frame Generation backend scenarios passed\n";
    return 0;
}

// Benchmark: two views at real headset sizes on a hardware adapter. Not a
// CTest; run `depthxr_framegen_tests bench-<api>` and read the GPU times.
bool g_bench = false;

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

// --- Direct3D 11 ------------------------------------------------------------

int D3D11Test() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, g_bench ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0,
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
    if (g_bench) {
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
    const int result = g_bench ? RunBench(harness) : RunScenarios(harness);
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
    if (api == "d3d12") return D3D12Test();
    if (api == "vulkan") return VulkanTest();
    return D3D11Test();
}
