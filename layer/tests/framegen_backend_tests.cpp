// Runs the Frame Generation GPU backends on synthetic frames with known motion
// and compares the generated image against analytic ground truth.
//   depthxr_framegen_tests d3d11 | d3d12 | vulkan | hw-d3d12
// Direct3D runs on WARP so the test needs no GPU; Vulkan needs a device and
// exits 77 (skipped) when none is present. hw-d3d12 runs the Direct3D 12
// scenarios on the first hardware adapter (variable-rate shading, the
// optical flow hardware, a GPU's fence budgets: what WARP does not have) and
// passes with a message when there is none. FG_TEST_ADAPTER=<text> puts the
// hardware modes and Vulkan on the adapter it names instead (test_adapter.h);
// naming none fails the hardware modes and skips Vulkan.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/framegen_runtime_queue.h"
#include "depthxr/quadviews_compose.h"
#include "test_adapter.h"
#include "test_gates.h"

#ifdef DEPTHXR_NVOF_RESEARCH
#include <nvOpticalFlowD3D12.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
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
#include <optional>
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
// Direct3D 12: the optical flow hardware's status once one view used it
// (RunScenarios); quad views then need it for all four.
std::string g_flow_status;

void Check(bool ok, const std::string& why) {
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        test_gates::OpenAllBeforeExit();
        std::exit(1);
    }
}

// The hardware modes' adapter: FG_TEST_ADAPTER's, or nullptr without it
// (Direct3D 11 then takes the default adapter, Direct3D 12 the first hardware
// adapter that runs it). Naming no hardware adapter fails.
ComPtr<IDXGIAdapter1> WantedAdapter() {
    const std::optional<std::string> wanted = test_adapter::Wanted();
    if (!wanted) return nullptr;
    ComPtr<IDXGIFactory1> factory;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    std::string why;
    ComPtr<IDXGIAdapter1> adapter = test_adapter::FindDxgiAdapter(factory.Get(), *wanted, why);
    Check(adapter != nullptr, why);
    return adapter;
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
// FG_TEST_NO_STALL=1: the hardware checks that leave a queue waiting on an
// optical flow match that never finishes (the watchdog releases it) are
// skipped; on some driver versions such waits have preceded system crashes.
bool StallChecksAllowed() {
    const char* value = std::getenv("FG_TEST_NO_STALL");
    return !(value && std::string(value) == "1");
}

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
void DumpSized(const std::string& name, const Pixels& pixels, FrameGenExtent extent) {
    const char* directory = std::getenv("FG_TEST_DUMP");
    if (!directory) return;
    std::FILE* file = std::fopen((std::string(directory) + "/" + name + ".ppm").c_str(), "wb");
    if (!file) return;
    std::fprintf(file, "P6\n%u %u\n255\n", extent.width, extent.height);
    for (const std::uint32_t pixel : pixels) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(pixel & 255), static_cast<unsigned char>((pixel >> 8) & 255),
                                      static_cast<unsigned char>((pixel >> 16) & 255)};
        std::fwrite(rgb, 1, 3, file);
    }
    std::fclose(file);
}

void Dump(const std::string& name, const Pixels& pixels) {
    DumpSized(name, pixels, {kSize, kSize});
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
const XrSwapchain kOutputThird = reinterpret_cast<XrSwapchain>(0x5a);
const XrSwapchain kOutputFourth = reinterpret_cast<XrSwapchain>(0x5b);

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
    if (gpu) gpu->SetCopyAfterRelease(true); // the tests' images are their own
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

        // 2d. Four frames in one draw (4x Interpolate): the last, at
        // fraction 1, is the newest real frame's own copy for its slot.
        g_swapchains[kOutputThird] = {harness.create(kSize, kSize)};
        g_swapchains[kOutputFourth] = {harness.create(kSize, kSize)};
        Check(gpu->Track(kOutputThird, EnumerateImages, error) && gpu->Track(kOutputFourth, EnumerateImages, error), error);
        auto four = [&](std::uint32_t index) {
            std::vector<FrameGenViewSubmission> views{Submission(index, harness.format, output_format, true, 0, 0)};
            for (const XrSwapchain output : {kOutputThird, kOutputFourth}) {
                views[0].outputs.push_back(views[0].outputs[0]);
                views[0].outputs.back().swapchain = output;
            }
            Check(gpu->Execute(views, tuning, result, error), harness.name + " execute: " + error);
        };
        tuning.generated = 4;
        tuning.fractions = {0.25f, 0.5f, 0.75f, 1.0f};
        gpu->ResetHistory();
        four(0);
        four(1);
        const Pixels quarter = harness.readback(g_swapchains[kOutput][0]);
        const Pixels three_quarters = harness.readback(g_swapchains[kOutputThird][0]);
        const Pixels whole = harness.readback(g_swapchains[kOutputFourth][0]);
        const double quarter_error = MeanError(quarter, MovingSquare(124, 122), 96, 96, 256, 256);
        const double three_quarters_error = MeanError(three_quarters, MovingSquare(132, 126), 96, 96, 256, 256);
        const double copy_error = MeanError(whole, b, 0, 0, kSize, kSize);
        std::cout << harness.name << " 4x: errors " << quarter_error << ", " << three_quarters_error
                  << ", real frame copy " << copy_error << std::endl;
        Check(quarter_error < 0.4 * third_hold && three_quarters_error < 0.4 * third_hold,
              harness.name + " 4x frames are not placed at 1/4 and 3/4 of the motion");
        Check(copy_error < 0.002, harness.name + " the frame at fraction 1 is not the newest real frame");

        // 2e. Just-in-time synthesis (Direct3D 12): the motion measured once
        // per real frame and each generated frame written on its own, later,
        // is what writing them together gives; from the same measurement a
        // cover frame moves on past the newest frame.
        if (gpu->SupportsJustInTime()) {
            FrameGenTuning together = tuning;
            together.generated = 2;
            together.fractions = {1.0f / 3.0f, 2.0f / 3.0f, 0.0f, 0.0f};
            std::swap(tuning, together);
            gpu->ResetHistory();
            run(0, true, 0, 0);
            const Pixels batch_third = run(1, true, 0, 0);
            const Pixels batch_two_thirds = harness.readback(g_swapchains[kOutputSecond][0]);
            std::swap(tuning, together);
            FrameGenTuning analyze = together;
            analyze.analyze_only = true;
            analyze.cover = true;
            gpu->ResetHistory();
            auto measure = [&](std::uint32_t index) {
                const std::vector<FrameGenViewSubmission> views{Submission(index, harness.format, output_format, true, 0, 0)};
                Check(gpu->Execute(views, analyze, result, error), harness.name + " analyze: " + error);
                Check(result.frame != 0, harness.name + " analyze names the real frame");
                return result.frame;
            };
            measure(0);
            const std::uint64_t frame = measure(1);
            const FrameGenImage first_output = Submission(1, harness.format, output_format, true, 0, 0).outputs[0];
            FrameGenImage second_output = first_output;
            second_output.swapchain = kOutputSecond;
            auto synthesize = [&](FrameGenSynthesisMode mode, float fraction, const FrameGenImage& output) {
                Check(gpu->Synthesize(frame, mode, fraction, {output}, error), harness.name + " synthesize: " + error);
                return harness.readback(g_swapchains[output.swapchain][0]);
            };
            // The later frame first: frames are written in any order.
            const Pixels jit_two_thirds = synthesize(FrameGenSynthesisMode::kInterpolate, 2.0f / 3.0f, second_output);
            const Pixels jit_third = synthesize(FrameGenSynthesisMode::kInterpolate, 1.0f / 3.0f, first_output);
            const double jit_difference = std::max(MeanError(jit_third, batch_third, 0, 0, kSize, kSize),
                                                   MeanError(jit_two_thirds, batch_two_thirds, 0, 0, kSize, kSize));
            std::cout << harness.name << " just in time vs together: difference " << jit_difference << '\n';
            Check(jit_difference < 0.001, harness.name + " frames written one at a time differ from frames written together");
            const Pixels jit_copy = synthesize(FrameGenSynthesisMode::kInterpolate, 1.0f, first_output);
            Check(MeanError(jit_copy, b, 0, 0, kSize, kSize) < 0.002,
                  harness.name + " just in time at fraction 1 is not the newest real frame");
            // Half an interval past B: the square at (144, 132).
            const Pixels cover = synthesize(FrameGenSynthesisMode::kExtrapolate, 0.5f, first_output);
            const double cover_error = MeanError(cover, MovingSquare(144, 132), 96, 96, 272, 272);
            const double cover_hold = MeanError(b, MovingSquare(144, 132), 96, 96, 272, 272);
            std::cout << harness.name << " cover: error " << cover_error << " vs hold " << cover_hold << '\n';
            Check(cover_error < 0.5 * cover_hold, harness.name + " a cover frame does not move content past the newest frame");
            // An output that is not tracked is refused before anything is
            // recorded: the real frame and its views stay as they were.
            FrameGenImage untracked = first_output;
            untracked.swapchain = reinterpret_cast<XrSwapchain>(0x5f);
            Check(!gpu->Synthesize(frame, FrameGenSynthesisMode::kInterpolate, 0.5f, {untracked}, error),
                  harness.name + " an output that is not tracked was written");
            Check(gpu->Writable(frame, false), harness.name + " a refused output dropped its real frame (" + error + ")");
            // Writable says what Synthesize then does (a canvas's ring of
            // three, FrameGenHistoryRing): one real frame on, all of the
            // frame is kept; two on, its previous images are gone (frames
            // moved by it are refused, its exact copy is not); three on, its
            // newest too.
            measure(2);
            Check(gpu->Writable(frame, false) && gpu->Writable(frame, true),
                  harness.name + " a real frame whose images are kept is not writable");
            Check(!gpu->Writable(0, true) && !gpu->Writable(frame + 100, true),
                  harness.name + " a real frame never analyzed is writable");
            measure(0);
            Check(!gpu->Writable(frame, false) && gpu->Writable(frame, true),
                  harness.name + " two real frames on, Writable does not refuse moved frames only");
            Check(!gpu->Synthesize(frame, FrameGenSynthesisMode::kInterpolate, 0.5f, {first_output}, error),
                  harness.name + " a real frame whose images were replaced is not synthesized from");
            const Pixels kept_copy = synthesize(FrameGenSynthesisMode::kInterpolate, 1.0f, first_output);
            Check(MeanError(kept_copy, b, 0, 0, kSize, kSize) < 0.002,
                  harness.name + " a real frame's exact copy is not its newest image while that is kept");
            measure(1);
            Check(!gpu->Writable(frame, true), harness.name + " a real frame whose newest images were replaced is writable");
            Check(!gpu->Synthesize(frame, FrameGenSynthesisMode::kInterpolate, 1.0f, {first_output}, error),
                  harness.name + " a real frame's replaced newest images are copied");

            // 2f. Stale motion: extrapolating, a real frame whose own motion
            // is not measured yet is moved by the previous one's. A, B and C
            // move steadily (16, 8) a frame: C moved half a frame on by B's
            // motion lands near where its own motion puts it, far closer
            // than C held. Only the frame's own previous, itself measured
            // from a pair, may move it.
            harness.upload(g_swapchains[kSource][2], MovingSquare(152, 136));
            FrameGenTuning extrapolating = analyze;
            extrapolating.mode = FrameGenSynthesisMode::kExtrapolate;
            extrapolating.cover = false;
            gpu->ResetHistory();
            auto measure_extrapolating = [&](std::uint32_t index) {
                const std::vector<FrameGenViewSubmission> views{Submission(index, harness.format, output_format, true, 0, 0)};
                Check(gpu->Execute(views, extrapolating, result, error), harness.name + " analyze: " + error);
                return result.frame;
            };
            const std::uint64_t frame_a = measure_extrapolating(0);
            const std::uint64_t frame_b = measure_extrapolating(1);
            const std::uint64_t frame_c = measure_extrapolating(2);
            Check(gpu->WaitIdle(), harness.name + " analyses did not finish");
            Check(gpu->Synthesize(frame_c, FrameGenSynthesisMode::kExtrapolate, 0.5f, {first_output}, error),
                  harness.name + " synthesize: " + error);
            const Pixels own = harness.readback(g_swapchains[first_output.swapchain][0]);
            Check(gpu->SynthesizeStale(frame_c, frame_b, 0.5f, {first_output}, error),
                  harness.name + " stale synthesize: " + error);
            const Pixels stale = harness.readback(g_swapchains[first_output.swapchain][0]);
            const Pixels stale_truth = MovingSquare(160, 140);
            const double own_error = MeanError(own, stale_truth, 112, 104, 288, 280);
            const double stale_error = MeanError(stale, stale_truth, 112, 104, 288, 280);
            const double stale_hold = MeanError(MovingSquare(152, 136), stale_truth, 112, 104, 288, 280);
            std::cout << harness.name << " stale motion: error " << stale_error << " vs own motion " << own_error
                      << " vs hold " << stale_hold << std::endl;
            Check(stale_error < 0.5 * stale_hold && stale_error < own_error + 0.25 * stale_hold,
                  harness.name + " the previous frame's motion does not move a frame on as its own does");
            Check(!gpu->SynthesizeStale(frame_c, frame_a, 0.5f, {first_output}, error),
                  harness.name + " a frame was moved by motion other than its previous frame's");
            Check(!gpu->SynthesizeStale(frame_b, frame_a, 0.5f, {first_output}, error),
                  harness.name + " a frame was moved by a first frame's motion (never measured from a pair)");
            gpu->ResetHistory();
        }
        tuning.generated = 1;
        tuning.fractions = {0.5f, 0.5f, 0.5f, 0.5f};
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
        if (harness.name == "D3D12") g_flow_status = status;
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
        if (status == "active" && StallChecksAllowed()) {
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
        if (backend) backend->SetCopyAfterRelease(true); // the tests' images are their own
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
        if (status == "active" && StallChecksAllowed()) {
            // A match at release that never finishes, with the next
            // capture's work queued behind it (its input can never become
            // ready): the watchdog still releases the game's queue.
            const auto start = std::chrono::steady_clock::now();
            const std::array<Pixels, 3> frames{MovingSquare(104, 112), MovingSquare(120, 120), MovingSquare(136, 128)};
            for (std::uint32_t i = 0; i < 3; ++i) harness.upload(g_swapchains[kSource][i], frames[i]);
            backend->ResetHistory();
            for (std::uint32_t i = 0; i < 3; ++i) {
                const std::vector<FrameGenViewSubmission> views{Submission(i, harness.format, output_format, true, 0, 0)};
                if (i == 1) backend->StallHardwareForTest();
                Check(backend->Capture(0, views[0].source, views[0].request, raw_tuning.quality, error) || error.empty(),
                      harness.name + " capture: " + error);
                Check(backend->Execute(views, raw_tuning, result, error), harness.name + " execute: " + error);
            }
            harness.readback(g_swapchains[kOutput][0]);
            const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            // The next generation takes the hardware out of use.
            const std::vector<FrameGenViewSubmission> after{Submission(0, harness.format, output_format, true, 0, 0)};
            Check(backend->Execute(after, raw_tuning, result, error), harness.name + " execute: " + error);
            std::cout << harness.name << " stalled match at release: released after " << waited << " ms, now "
                      << backend->HardwareFlowStatus() << std::endl;
            Check(backend->HardwareFlowStatus() == "the optical flow hardware stopped responding" && waited < 2000.0,
                  harness.name + " a stalled match at release, queued behind, was not released");
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
        const XrSwapchain right_second = reinterpret_cast<XrSwapchain>(0x59);
        g_swapchains[right_source] = {harness.create(kSize, kSize), harness.create(kSize, kSize)};
        g_swapchains[right_output] = {harness.create(kSize, kSize)};
        g_swapchains[right_second] = {harness.create(kSize, kSize)};
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
        // `eye` meters sideways per unit of generated-frame time. `stale`:
        // just in time, a third frame (the newest image again, the head
        // still since) moved by the newest frame's motion and depth
        // (SynthesizeStale) instead.
        auto run_case = [&](float panel_value, float background_value, const FrameGenDepthEncoding& encoding,
                            bool use_depth, int glass, float eye = 0.0f, bool stale = false) {
            auto backend = CreateFrameGenGpu(harness.binding, error);
            if (backend) backend->SetCopyAfterRelease(true); // the tests' images are their own
            Check(backend != nullptr, harness.name + " backend: " + error);
            g_swapchains[kDepthImage] = {harness.create_depth(panel_value, grow(left_panel, glass), background_value)};
            g_swapchains[right_depth] = {harness.create_depth(panel_value, grow(right_panel, glass), background_value)};
            for (const XrSwapchain chain :
                 {kSource, kOutput, kOutputSecond, right_source, right_output, right_second, kDepthImage, right_depth}) {
                Check(backend->Track(chain, EnumerateImages, error), error);
            }
            auto views = [&](std::uint32_t index, bool head_still = false) {
                FrameGenViewSubmission left = Submission(index, harness.format, output_format, true, 0, 0);
                FrameGenViewSubmission right = left;
                right.source.swapchain = right_source;
                right.outputs = {{right_output, 0, 0, left.outputs[0].rect, output_format},
                                 {right_second, 0, 0, left.outputs[0].rect, output_format}};
                left.outputs.resize(tuning.generated);
                right.outputs.resize(tuning.generated);
                for (auto* geometry : {&left.request.newest, &left.request.previous}) {
                    geometry->angle_left = std::atan(-outer);
                    geometry->angle_right = std::atan(inner);
                }
                for (auto* geometry : {&right.request.newest, &right.request.previous}) {
                    geometry->angle_left = std::atan(-inner);
                    geometry->angle_right = std::atan(outer);
                }
                left.request.newest.position = {shift - half_ipd, 0, 0};
                left.request.previous.position = {head_still ? shift - half_ipd : -half_ipd, 0, 0};
                right.request.newest.position = {shift + half_ipd, 0, 0};
                right.request.previous.position = {head_still ? shift + half_ipd : half_ipd, 0, 0};
                left.request.depth = right.request.depth = use_depth;
                left.request.shift = right.request.shift = {eye, 0.0f, 0.0f};
                return std::vector<FrameGenViewSubmission>{left, right};
            };
            // Color is captured at release, as games' frames are; the depth
            // trust pass needs both eyes' images of the frame.
            auto capture = [&](std::uint32_t index, bool head_still = false) {
                const XrRect2Di rect{{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}};
                const auto requests = views(index, head_still);
                // Each call before its message: arguments are evaluated in no
                // set order, and the message must read the call's error.
                for (std::size_t i = 0; i < 2; ++i) {
                    const bool captured = backend->Capture(i, requests[i].source, requests[i].request, tuning.quality, error);
                    Check(captured, harness.name + " capture: " + error);
                }
                if (index == 1) {
                    const bool depth_captured =
                        backend->CaptureDepth(0, {kDepthImage, 0, 0, rect, 0}, encoding, requests[0].request, error) &&
                        backend->CaptureDepth(1, {right_depth, 0, 0, rect, 0}, encoding, requests[1].request, error);
                    Check(depth_captured, harness.name + " depth capture: " + error);
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
            FrameGenTuning last = tuning;
            last.analyze_only = stale;
            last.cover = false;
            capture(0);
            Check(backend->Execute(views(0), last, result, error), harness.name + " execute: " + error);
            capture(1);
            Check(backend->Execute(views(1), last, result, error), harness.name + " execute: " + error);
            if (stale) {
                const std::uint64_t motion = result.frame;
                capture(1, true);
                Check(backend->Execute(views(1, true), last, result, error), harness.name + " execute: " + error);
                // The previous frame measured, as the presenter waits for.
                Check(backend->WaitIdle(), harness.name + " analyses did not finish");
                const auto requests = views(1, true);
                const bool written = backend->SynthesizeStale(
                    result.frame, motion, tuning.fractions[0], {requests[0].outputs[0], requests[1].outputs[0]}, error);
                Check(written, harness.name + " stale synthesize: " + error);
                Check(backend->WaitIdle(), harness.name + " stale synthesis did not finish");
            }
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
        // 3g''. Just in time, a frame whose own motion is late, moved by the
        // previous frame's (SynthesizeStale): seen from where the eye is at
        // its time all the same, by the previous frame's depth.
        const Outcome stale_moving = run_case(panel, 1.0f, standard, true, 0, eye, true);
        const Outcome stale_still = run_case(panel, 1.0f, standard, true, 0, 0.0f, true);
        Dump(harness.name + "-cockpit-eye-shift-stale", stale_moving.output);
        const double stale_error = moved_error_of(stale_moving.output);
        const double stale_unshifted_error = moved_error_of(stale_still.output);
        std::cout << harness.name << " eye shift, stale motion: error " << stale_error << " vs " << stale_unshifted_error
                  << " unshifted, " << moving_error << " own motion" << std::endl;
        Check(stale_moving.used && stale_error < 0.4 * stale_unshifted_error && stale_error < 2.0 * moving_error + 0.002,
              harness.name + " a frame moved by the previous frame's motion did not follow the eye");
        // The panel faces the eye, so neighbours land at one depth: the map
        // must not depend on which of them the GPU ran first.
        const Outcome again = run_case(panel, 1.0f, standard, true, 0, eye);
        Check(MeanError(again.output, moving.output, 0, 0, kSize, kSize) == 0.0,
              harness.name + " the eye-shift map differs between identical frames");

        // 3g'. Two generated frames, half and a whole interval on: the first
        // still sees the panel from 2 cm further. Scaling one map built for
        // the later frame cut the panel's edge where the scenery comes into
        // view (which surface is in front changes with time).
        tuning.generated = 2;
        tuning.fractions = {0.5f, 1.0f, 0.5f, 0.5f};
        const Outcome two = run_case(panel, 1.0f, standard, true, 0, eye);
        tuning.generated = 1;
        tuning.fractions = {0.5f, 0.5f, 0.5f, 0.5f};
        const double two_error = moved_error_of(two.output);
        std::cout << harness.name << " eye shift, two frames: first " << two_error << " vs " << moving_error << " alone"
                  << std::endl;
        Check(two.used && two_error < 1.2 * moving_error + 0.0005,
              harness.name + " a generated frame before the last one did not see the panel from its own eye position");
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
    if (gpu) gpu->SetCopyAfterRelease(true); // the tests' images are their own
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
// FG_PROFILE_QUAD=1 profiles DCS's quad views instead (RunQuadProfile);
// FG_PROFILE_LOAD_MS=N then also times captures and analyses queued behind
// N ms of the game's GPU work (ProfileUnderLoad; FG_PROFILE_HARDWARE=1 with
// the optical flow hardware).
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

// Profiled categories (PassMilliseconds): the passes, then copies, the
// optical flow hardware's wait and the quad views' compose and flow merge.
const char* const kProfileNames[] = {"luma",        "downsample",  "search",      "refine",      "filter",
                                     "synthesize",  "synthesize16", "depth",      "depth check", "depth trust",
                                     "flow pack",   "flow unpack", "trust update", "shift clear", "shift splat",
                                     "shift resolve", "shift fill", "raw pack",   "trust resolve", "shading rate", "world reduce", "world solve", "trust vote", "depth scale", "scale vote", "depth stale",
                                     "source copy", "output copy",
                                     "hardware wait", "compose", "flow merge"};
static_assert(std::size(kProfileNames) == kFrameGenPassCount + 5, "a name per profiled category");

// --- Quad views (Direct3D 12) ------------------------------------------------
// The layer composes Varjo quad views (a peripheral and a focus view per eye)
// into stereo. Frame Generation composes them itself (SynthesizeComposed):
// each view's generated frame at its own size, then each eye composed as the
// layer's Direct3D 11 compositor composes it.

float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// An sRGB image in linear light, sampled as a linear clamp sampler does.
struct LinearImage {
    std::uint32_t width{0}, height{0};
    std::vector<std::array<float, 3>> texels;

    LinearImage(const Pixels& pixels, FrameGenExtent extent) : width(extent.width), height(extent.height) {
        texels.resize(pixels.size());
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            for (int c = 0; c < 3; ++c) {
                texels[i][c] = SrgbToLinear(static_cast<float>((pixels[i] >> (8 * c)) & 255u) / 255.0f);
            }
        }
    }

    std::array<float, 3> Sample(float u, float v) const {
        const float x = u * width - 0.5f, y = v * height - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        // Filtering weights at the 8 bits of subtexel precision Direct3D
        // requires (and GPUs and WARP use).
        const float fx = std::round((x - x0) * 256.0f) / 256.0f, fy = std::round((y - y0) * 256.0f) / 256.0f;
        auto at = [&](int tx, int ty) -> const std::array<float, 3>& {
            tx = std::clamp(tx, 0, static_cast<int>(width) - 1);
            ty = std::clamp(ty, 0, static_cast<int>(height) - 1);
            return texels[static_cast<std::size_t>(ty) * width + static_cast<std::size_t>(tx)];
        };
        std::array<float, 3> result{};
        for (int c = 0; c < 3; ++c) {
            const float top = at(x0, y0)[c] + (at(x0 + 1, y0)[c] - at(x0, y0)[c]) * fx;
            const float bottom = at(x0, y0 + 1)[c] + (at(x0 + 1, y0 + 1)[c] - at(x0, y0 + 1)[c]) * fx;
            result[c] = top + (bottom - top) * fy;
        }
        return result;
    }
};

// How far into the focus rectangle's blend band an output pixel is: 0 at
// its edge (or outside), 1 inside the band (the compositor's edge alpha).
float ComposeAlpha(const FrameGenConstants& c, float u, float v) {
    const auto& rect = c.fov_current;
    if (u < rect[0] || v < rect[1] || u > rect[2] || v > rect[3]) {
        return -1.0f;
    }
    const float to_x = std::min(u - rect[0], rect[2] - u), to_y = std::min(v - rect[1], rect[3] - v);
    float alpha = std::clamp(std::min(to_x / std::max(c.fov_previous[0], 0.0001f), to_y / std::max(c.fov_previous[1], 0.0001f)),
                             0.0f, 1.0f);
    return alpha * alpha * (3.0f - 2.0f * alpha);
}

// One eye composed on the CPU from its views, as FG_PASS_COMPOSE_DRAW and the
// Direct3D 11 compositor compose it: the reference.
Pixels ComposeReference(const LinearImage& periphery, const LinearImage& focus, const FrameGenConstants& c,
                        FrameGenExtent output) {
    Pixels pixels(std::size_t{output.width} * output.height);
    const auto& rect = c.fov_current;
    for (std::uint32_t y = 0; y < output.height; ++y) {
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const float u = (x + 0.5f) / output.width, v = (y + 0.5f) / output.height;
            std::array<float, 3> composed = periphery.Sample(u, v);
            const float alpha = ComposeAlpha(c, u, v);
            if (alpha >= 0.0f) {
                const float fu = std::clamp((u - rect[0]) / std::max(rect[2] - rect[0], 0.0001f), 0.0f, 1.0f);
                const float fv = std::clamp((v - rect[1]) / std::max(rect[3] - rect[1], 0.0001f), 0.0f, 1.0f);
                std::array<float, 3> sample = focus.Sample(fu, fv);
                const float sharpen = std::clamp(c.fov_previous[2], 0.0f, 1.0f) * alpha;
                if (sharpen > 0.001f) {
                    const float ox = c.rot0[2], oy = c.rot0[3];
                    auto tap = [&](float du, float dv) {
                        return focus.Sample(std::clamp(fu + du, 0.0f, 1.0f), std::clamp(fv + dv, 0.0f, 1.0f));
                    };
                    const auto n = tap(0.0f, -oy), s = tap(0.0f, oy), e = tap(ox, 0.0f), w = tap(-ox, 0.0f);
                    for (int k = 0; k < 3; ++k) {
                        const float average = (n[k] + s[k] + e[k] + w[k]) * 0.25f;
                        const float low = std::min({sample[k], n[k], s[k], e[k], w[k]});
                        const float high = std::max({sample[k], n[k], s[k], e[k], w[k]});
                        const float range = std::max(high - low, 1.0f / 255.0f);
                        sample[k] = std::clamp(sample[k] + std::clamp((sample[k] - average) * (sharpen * 2.0f),
                                                                      -range * 0.5f, range * 0.5f),
                                               0.0f, 1.0f);
                    }
                }
                for (int k = 0; k < 3; ++k) composed[k] += (sample[k] - composed[k]) * alpha;
            }
            pixels[std::size_t{y} * output.width + x] =
                Pack(LinearToSrgb(composed[0]), LinearToSrgb(composed[1]), LinearToSrgb(composed[2]));
        }
    }
    return pixels;
}

// Per-channel differences of two images where `inside` holds: mean (of 1)
// and largest (in 8-bit steps).
struct Difference {
    double mean{0.0};
    int max{0};
    std::size_t pixels{0};
};

Difference Compare(const Pixels& a, const Pixels& b, FrameGenExtent extent,
                   const std::function<bool(std::uint32_t, std::uint32_t)>& inside) {
    Difference difference;
    double sum = 0.0;
    for (std::uint32_t y = 0; y < extent.height; ++y) {
        for (std::uint32_t x = 0; x < extent.width; ++x) {
            if (!inside(x, y)) continue;
            const std::uint32_t p = a[std::size_t{y} * extent.width + x], q = b[std::size_t{y} * extent.width + x];
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>((p >> (8 * c)) & 255) - static_cast<int>((q >> (8 * c)) & 255));
                sum += d;
                difference.max = std::max(difference.max, d);
            }
            ++difference.pixels;
        }
    }
    difference.mean = difference.pixels ? sum / (255.0 * 3.0 * difference.pixels) : 0.0;
    return difference;
}

// The layer's own Direct3D 11 compositor shader (QuadViewsD3D11ShaderSource,
// diagnostics off) composing one eye from the same images, the source rects
// whole images: what Frame Generation's compose must reproduce.
class D3D11Compositor {
  public:
    explicit D3D11Compositor(bool hardware) {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const ComPtr<IDXGIAdapter1> adapter = hardware ? WantedAdapter() : nullptr;
        const D3D_DRIVER_TYPE type =
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
        Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), type, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device_,
                                          nullptr, &context_)),
              "D3D11 compositor device");
        const char* source = QuadViewsD3D11ShaderSource();
        ComPtr<ID3DBlob> vs, ps, errors;
        Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "quadviews", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                                   &vs, &errors)) &&
                  SUCCEEDED(D3DCompile(source, std::strlen(source), "quadviews", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                                       &ps, &errors)),
              std::string("D3D11 compositor shader: ") +
                  (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed"));
        Check(SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex_)) &&
                  SUCCEEDED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel_)),
              "D3D11 compositor shaders");
        D3D11_SAMPLER_DESC sampler{}; // as the layer's compositor
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        Check(SUCCEEDED(device_->CreateSamplerState(&sampler, &sampler_)), "D3D11 compositor sampler");
    }

    Pixels Compose(const Pixels& periphery, FrameGenExtent periphery_extent, const Pixels& focus,
                   FrameGenExtent focus_extent, const FrameGenComposeEye& eye, FrameGenExtent output) {
        auto texture = [&](const Pixels& pixels, FrameGenExtent extent, UINT bind) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = extent.width;
            desc.Height = extent.height;
            desc.MipLevels = desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = bind;
            const D3D11_SUBRESOURCE_DATA data{pixels.data(), extent.width * 4, 0};
            ComPtr<ID3D11Texture2D> created;
            Check(SUCCEEDED(device_->CreateTexture2D(&desc, pixels.empty() ? nullptr : &data, &created)),
                  "D3D11 compositor texture");
            return created;
        };
        const auto periphery_texture = texture(periphery, periphery_extent, D3D11_BIND_SHADER_RESOURCE);
        const auto focus_texture = texture(focus, focus_extent, D3D11_BIND_SHADER_RESOURCE);
        const auto target = texture({}, output, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11ShaderResourceView> views[2];
        ComPtr<ID3D11RenderTargetView> rtv;
        Check(SUCCEEDED(device_->CreateShaderResourceView(periphery_texture.Get(), nullptr, &views[0])) &&
                  SUCCEEDED(device_->CreateShaderResourceView(focus_texture.Get(), nullptr, &views[1])) &&
                  SUCCEEDED(device_->CreateRenderTargetView(target.Get(), nullptr, &rtv)),
              "D3D11 compositor views");
        // QuadViewsConstants: focusRect, blendParams, focusTexel, the source
        // rects, diagnostics (off), the output texel, markers.
        std::array<float, 36> constants{};
        std::copy(eye.focus_rect.begin(), eye.focus_rect.end(), constants.begin());
        constants[4] = eye.feather[0];
        constants[5] = eye.feather[1];
        constants[6] = eye.sharpen;
        constants[8] = eye.focus_texel[0];
        constants[9] = eye.focus_texel[1];
        constants[10] = eye.output_pixel_in_focus[0];
        constants[11] = eye.output_pixel_in_focus[1];
        constants[14] = constants[15] = 1.0f; // peripheral source rect (0, 0, 1, 1)
        constants[18] = constants[19] = 1.0f; // focus source rect
        constants[24] = 1.0f / output.width;
        constants[25] = 1.0f / output.height;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(constants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA buffer_data{constants.data(), 0, 0};
        ComPtr<ID3D11Buffer> cbuffer;
        Check(SUCCEEDED(device_->CreateBuffer(&buffer, &buffer_data, &cbuffer)), "D3D11 compositor constants");
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(output.width), static_cast<float>(output.height), 0.0f,
                                      1.0f};
        ID3D11ShaderResourceView* srvs[] = {views[0].Get(), views[1].Get()};
        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ID3D11SamplerState* samplers[] = {sampler_.Get()};
        ID3D11Buffer* cbuffers[] = {cbuffer.Get()};
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vertex_.Get(), nullptr, 0);
        context_->PSSetShader(pixel_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, cbuffers);
        context_->PSSetShaderResources(0, 2, srvs);
        context_->PSSetSamplers(0, 1, samplers);
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, rtvs, nullptr);
        context_->Draw(3, 0);
        D3D11_TEXTURE2D_DESC desc{};
        target->GetDesc(&desc);
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging)), "D3D11 compositor staging");
        context_->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(SUCCEEDED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "D3D11 compositor map");
        Pixels pixels(std::size_t{output.width} * output.height);
        for (std::uint32_t y = 0; y < output.height; ++y) {
            std::memcpy(&pixels[std::size_t{y} * output.width], static_cast<const std::byte*>(mapped.pData) + y * mapped.RowPitch,
                        std::size_t{output.width} * 4);
        }
        context_->Unmap(staging.Get(), 0);
        return pixels;
    }

  private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11SamplerState> sampler_;
};

// A view of the quad views test scene: its field of view in tangents (left,
// right, up, down) and its size.
struct QuadView {
    std::array<float, 4> tangents{};
    FrameGenExtent extent;
};

FrameGenFov QuadFov(const QuadView& view) {
    return {std::atan(view.tangents[0]), std::atan(view.tangents[1]), std::atan(view.tangents[2]),
            std::atan(view.tangents[3])};
}

FrameGenViewGeometry QuadGeometry(const QuadView& view) {
    FrameGenViewGeometry geometry;
    const FrameGenFov fov = QuadFov(view);
    geometry.angle_left = fov.left;
    geometry.angle_right = fov.right;
    geometry.angle_up = fov.up;
    geometry.angle_down = fov.down;
    return geometry;
}

// The quad views test scene's colour (linear light) in a direction: tangents
// across and up.
using QuadColor = std::array<float, 3>;
using QuadScene = std::function<QuadColor(float, float)>;

// Detail at each scale the test's views resolve (tangent units): features
// of about 0.06 (10 peripheral pixels) and 0.02 (10 focus pixels).
float QuadDetail(float x, float y, float seed) {
    return 0.55f * Noise(x * 16.0f + seed, y * 16.0f) + 0.45f * Noise(x * 50.0f + 17.0f + seed, y * 50.0f - seed);
}

QuadColor Scenery(float tx, float ty) {
    const float n = QuadDetail(tx, ty, 0.0f);
    return {0.02f + 0.35f * n, 0.03f + 0.3f * n, 0.05f + 0.2f * n};
}

// The scene through `view`, anti-aliased as a renderer resolves it: 4x4
// samples per pixel averaged in linear light, so the periphery does not
// alias detail only the focus views resolve.
Pixels RenderQuad(const QuadView& view, const QuadScene& scene) {
    const auto& t = view.tangents;
    Pixels pixels(std::size_t{view.extent.width} * view.extent.height);
    auto rows = [&](std::uint32_t first, std::uint32_t step) {
        for (std::uint32_t y = first; y < view.extent.height; y += step) {
            for (std::uint32_t x = 0; x < view.extent.width; ++x) {
                QuadColor sum{};
                for (int sy = 0; sy < 4; ++sy) {
                    const float ty = t[2] - (t[2] - t[3]) * (y + (sy + 0.5f) / 4.0f) / view.extent.height;
                    for (int sx = 0; sx < 4; ++sx) {
                        const float tx = t[0] + (t[1] - t[0]) * (x + (sx + 0.5f) / 4.0f) / view.extent.width;
                        const QuadColor sample = scene(tx, ty);
                        for (int c = 0; c < 3; ++c) sum[c] += sample[c] / 16.0f;
                    }
                }
                pixels[std::size_t{y} * view.extent.width + x] =
                    Pack(LinearToSrgb(sum[0]), LinearToSrgb(sum[1]), LinearToSrgb(sum[2]));
            }
        }
    };
    const std::uint32_t threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    for (std::uint32_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back(rows, thread, threads);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return pixels;
}

// Each eye's composition of quad views 0-3 (peripheral 0/1, focus 2/3).
FrameGenComposition QuadComposition(const std::array<QuadView, 4>& views, FrameGenExtent output, double transition,
                                    double sharpness) {
    FrameGenComposition composition;
    composition.output = output;
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        FrameGenComposeEye composed = BuildFrameGenComposeEye(QuadFov(views[eye]), QuadFov(views[eye + 2]), output,
                                                              views[eye + 2].extent, transition, sharpness);
        composed.peripheral = eye;
        composed.focus = eye + 2;
        composition.eyes.push_back(composed);
    }
    return composition;
}

// Quad views on Direct3D 12: the compose against the Direct3D 11
// compositor and a CPU reference, generated frames of four views composed
// into two eyes, the seam where an eye blends its views, and the capture
// marks and analysis timings GPU-aware pacing reads.
int RunQuadViews(Harness& harness) {
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, harness.name + " backend: " + error);
    gpu->SetCopyAfterRelease(true); // the tests' images are their own
    // Without just-in-time synthesis (FG_TEST_RUNTIME_QUEUE=0) nothing is
    // composed; GPU-aware pacing (4.) applies all the same.
    const bool composes = gpu->SupportsJustInTime();
    Check(gpu->SupportsComposition() == composes, harness.name + " composes quad views just in time, and only then");
    if (!composes) {
        std::cout << harness.name << " quad views: no just-in-time synthesis here; composing skipped\n";
    }
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    auto chain = [](std::uintptr_t id) { return reinterpret_cast<XrSwapchain>(id); };
    auto whole = [](FrameGenExtent extent) {
        return XrRect2Di{{0, 0}, {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)}};
    };
    auto submission = [&](XrSwapchain source, std::uint32_t index, const QuadView& view, const QuadView& previous,
                          bool synthesize) {
        FrameGenViewSubmission sub;
        sub.source = {source, index, 0, whole(view.extent), harness.format};
        sub.request.extent = view.extent;
        sub.request.output = FrameGenOutputKind::kSrgb8;
        sub.request.synthesize = synthesize;
        sub.request.temporal = true;
        sub.request.newest = QuadGeometry(view);
        sub.request.previous = QuadGeometry(synthesize ? previous : view);
        return sub;
    };

    // 1. The compose: an eye of two exact views (the real frame's own
    // images) is what the Direct3D 11 compositor and the CPU reference
    // compose. A peripheral view of +-0.8 rad, an off-centre focus view, an
    // output of another size; the rect clamped where the focus view reaches
    // past the periphery; sharpening off and on.
    if (composes) {
        const FrameGenExtent periphery_extent{200, 202}, focus_extent{252, 254}, output{280, 282};
        const XrSwapchain periphery_chain = chain(0xd0), focus_chain = chain(0xd1), output_chain = chain(0xd2);
        g_swapchains[periphery_chain] = {harness.create(periphery_extent.width, periphery_extent.height)};
        g_swapchains[focus_chain] = {harness.create(focus_extent.width, focus_extent.height)};
        g_swapchains[output_chain] = {harness.create(output.width, output.height)};
        Check(gpu->Track(periphery_chain, EnumerateImages, error) && gpu->Track(focus_chain, EnumerateImages, error) &&
                  gpu->Track(output_chain, EnumerateImages, error),
              error);
        // The two views' images differ, so the inset shows. Smooth: filtering
        // precision differs between GPUs (sharpening amplifies it at hard
        // edges), the compose must not.
        Pixels periphery_pixels(std::size_t{periphery_extent.width} * periphery_extent.height);
        for (std::uint32_t y = 0; y < periphery_extent.height; ++y) {
            for (std::uint32_t x = 0; x < periphery_extent.width; ++x) {
                periphery_pixels[std::size_t{y} * periphery_extent.width + x] = Background(x * 0.8f, y * 0.8f);
            }
        }
        Pixels focus_pixels(std::size_t{focus_extent.width} * focus_extent.height);
        for (std::uint32_t y = 0; y < focus_extent.height; ++y) {
            for (std::uint32_t x = 0; x < focus_extent.width; ++x) {
                // Soft yellow cells over the patch texture.
                const float cell = std::clamp(0.5f + 1.5f * std::sin(x * 0.13f) * std::sin(y * 0.13f), 0.0f, 1.0f);
                const std::uint32_t patch = Patch(x * 0.7f, y * 0.7f);
                auto channel = [&](int c, float yellow) {
                    const float value = static_cast<float>((patch >> (8 * c)) & 255u) / 255.0f;
                    return value + (yellow - value) * cell;
                };
                focus_pixels[std::size_t{y} * focus_extent.width + x] =
                    Pack(channel(0, 0.95f), channel(1, 0.9f), channel(2, 0.2f));
            }
        }
        harness.upload_sized(g_swapchains[periphery_chain][0], periphery_extent.width, periphery_extent.height,
                             periphery_pixels);
        harness.upload_sized(g_swapchains[focus_chain][0], focus_extent.width, focus_extent.height, focus_pixels);
        const LinearImage periphery_linear(periphery_pixels, periphery_extent), focus_linear(focus_pixels, focus_extent);
        D3D11Compositor d3d11(g_hardware);
        FrameGenTuning analyze;
        analyze.analyze_only = true;
        FrameGenExecuteResult result;
        const QuadView full{{std::tan(-0.8f), std::tan(0.8f), std::tan(0.8f), std::tan(-0.8f)}, periphery_extent};
        struct Case {
            const char* name;
            std::array<float, 4> focus_angles; // left, right, up, down (radians)
            double transition, sharpness;
        };
        for (const Case& test : {Case{"off-centre", {-0.25f, 0.3f, 0.35f, -0.1f}, 25.0, 0.0},
                                 Case{"off-centre sharpened", {-0.25f, 0.3f, 0.35f, -0.1f}, 25.0, 50.0},
                                 Case{"clamped", {-0.95f, -0.2f, 0.9f, 0.3f}, 10.0, 50.0}}) {
            const QuadView focus{{std::tan(test.focus_angles[0]), std::tan(test.focus_angles[1]),
                                  std::tan(test.focus_angles[2]), std::tan(test.focus_angles[3])},
                                 focus_extent};
            const std::vector<FrameGenViewSubmission> views{submission(periphery_chain, 0, full, full, false),
                                                            submission(focus_chain, 0, focus, focus, false)};
            const bool analyzed = gpu->Execute(views, analyze, result, error);
            Check(analyzed && result.frame != 0, harness.name + " analyze: " + error);
            FrameGenComposition composition;
            composition.output = output;
            FrameGenComposeEye eye = BuildFrameGenComposeEye(QuadFov(full), QuadFov(focus), output, focus_extent,
                                                             test.transition, test.sharpness);
            eye.peripheral = 0;
            eye.focus = 1;
            composition.eyes = {eye};
            const FrameGenImage image{output_chain, 0, 0, whole(output), output_format};
            const bool composed_ok =
                gpu->SynthesizeComposed(result.frame, FrameGenSynthesisMode::kInterpolate, 1.0f, composition, {image}, error);
            Check(composed_ok, harness.name + " compose: " + error);
            const Pixels composed = harness.readback_sized(g_swapchains[output_chain][0]);
            const FrameGenConstants constants = BuildFrameGenComposeConstants(eye, output);
            const Pixels reference = ComposeReference(periphery_linear, focus_linear, constants, output);
            const Pixels shipped = d3d11.Compose(periphery_pixels, periphery_extent, focus_pixels, focus_extent, eye, output);
            DumpSized(harness.name + "-compose-" + test.name, composed, output);
            auto region = [&](int which) {
                return [&, which](std::uint32_t x, std::uint32_t y) {
                    const float alpha = ComposeAlpha(constants, (x + 0.5f) / output.width, (y + 0.5f) / output.height);
                    return which == 0 ? alpha < 0.0f : which == 1 ? alpha >= 0.0f && alpha < 1.0f : alpha >= 1.0f;
                };
            };
            auto all = [](std::uint32_t, std::uint32_t) { return true; };
            const Difference to_reference = Compare(composed, reference, output, all);
            const Difference to_shipped = Compare(composed, shipped, output, all);
            const char* region_names[] = {"outside", "band", "inside"};
            // A GPU's sRGB conversions and filter weights are only as exact as
            // Direct3D requires (the shipped shader on it agrees to a step);
            // sharpening at 50% doubles their differences from the reference.
            const int tolerance = g_hardware && test.sharpness > 0.0 ? 4 : 2;
            std::cout << harness.name << " compose " << test.name << ": reference mean " << to_reference.mean << " max "
                      << to_reference.max << ", Direct3D 11 compositor mean " << to_shipped.mean << " max "
                      << to_shipped.max << " (rect " << eye.focus_rect[0] << ' ' << eye.focus_rect[1] << ' '
                      << eye.focus_rect[2] << ' ' << eye.focus_rect[3] << ";";
            for (int which = 0; which < 3; ++which) {
                const Difference part = Compare(composed, reference, output, region(which));
                std::cout << ' ' << region_names[which] << ' ' << part.pixels << " px max " << part.max;
                // Every region is there, and each composes as the reference does.
                Check(part.pixels > 0 && part.max <= tolerance, harness.name + " compose " + test.name + ": the " +
                                                                    region_names[which] + " differs from the reference");
            }
            std::cout << ")\n";
            Check(to_reference.mean < 0.002 && to_reference.max <= tolerance,
                  harness.name + " compose " + test.name + " differs from the CPU reference");
            Check(to_shipped.max <= 1, harness.name + " compose " + test.name + " differs from the Direct3D 11 compositor");
            // The inset is there: inside, the focus view shows, not the periphery.
            const FrameGenConstants no_inset = [&] {
                FrameGenConstants c = constants;
                c.fov_current = {2.0f, 2.0f, 2.0f, 2.0f};
                return c;
            }();
            const Pixels periphery_only = ComposeReference(periphery_linear, focus_linear, no_inset, output);
            Check(Compare(composed, periphery_only, output, region(2)).mean > 0.05,
                  harness.name + " compose " + test.name + ": the focus view is not inset");
        }
        gpu->ResetHistory();
    }

    // 2. Generated frames of quad views: four views of two sizes (the
    // peripheral views 256 px over +-0.8 tangents, the focus views 320 px
    // over 0.64, 3x denser), one analysis measuring all four, both eyes
    // composed from each generated frame. A textured square moves over the
    // scenery inside the inset, across its blend band.
    const FrameGenExtent periphery_extent{256, 256}, focus_extent{320, 320}, output{384, 384};
    const std::array<XrSwapchain, 4> sources{chain(0xd4), chain(0xd5), chain(0xd6), chain(0xd7)};
    const std::array<XrSwapchain, 2> outputs{chain(0xd8), chain(0xd9)};
    constexpr std::uint32_t kQuadImages = 4;
    for (std::size_t i = 0; i < 4; ++i) {
        const FrameGenExtent extent = i < 2 ? periphery_extent : focus_extent;
        g_swapchains[sources[i]].clear();
        for (std::uint32_t image = 0; image < kQuadImages; ++image) {
            g_swapchains[sources[i]].push_back(harness.create(extent.width, extent.height));
        }
        Check(gpu->Track(sources[i], EnumerateImages, error), error);
    }
    for (const XrSwapchain out : outputs) {
        g_swapchains[out] = {harness.create(output.width, output.height)};
        Check(gpu->Track(out, EnumerateImages, error), error);
    }
    const std::vector<FrameGenImage> eye_images{{outputs[0], 0, 0, whole(output), output_format},
                                                {outputs[1], 0, 0, whole(output), output_format}};
    // Each view's frame written on its own (Synthesize, as a canvas is),
    // at its own size.
    const std::array<XrSwapchain, 4> view_outputs{chain(0xda), chain(0xdb), chain(0xdc), chain(0xdd)};
    for (std::size_t i = 0; i < 4; ++i) {
        const FrameGenExtent extent = i < 2 ? periphery_extent : focus_extent;
        g_swapchains[view_outputs[i]] = {harness.create(extent.width, extent.height)};
        Check(gpu->Track(view_outputs[i], EnumerateImages, error), error);
    }
    // On the GPU: whether the backend shades wide views' periphery coarsely
    // (tier 2 variable-rate shading, not turned off), which a composed
    // periphery never is; without it that check cannot see coarse shading.
    bool variable_rate = false;
    if (g_hardware) {
        for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
            if (base->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) continue;
            ID3D12Device* device = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base)->device;
            D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
            variable_rate = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))) &&
                            options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
                            options6.ShadingRateImageTileSize > 0;
        }
        const char* vrs = std::getenv("VECTORXR_FRAMEGEN_VRS");
        variable_rate = variable_rate && !(vrs && std::atof(vrs) <= 0.0);
        if (!variable_rate) {
            std::cout << harness.name << " quad views: no variable-rate shading on this GPU; the composed periphery's "
                         "per-pixel shading is not checked\n";
        }
        if (g_flow_status != "active") {
            std::cout << harness.name << " quad views: no optical flow hardware (" << g_flow_status
                      << "); its four sessions are not checked\n";
        }
    }
    // The views of a frame: the peripheral views share the field of view,
    // the right eye's inset sits a little up and to the right.
    auto quad_views = [&](float focus_x, float focus_y) {
        std::array<QuadView, 4> views{};
        views[0] = views[1] = QuadView{{-0.8f, 0.8f, 0.8f, -0.8f}, periphery_extent};
        views[2] = QuadView{{-0.3f + focus_x, 0.34f + focus_x, 0.3f + focus_y, -0.34f + focus_y}, focus_extent};
        views[3] = QuadView{{-0.26f + focus_x, 0.38f + focus_x, 0.32f + focus_y, -0.32f + focus_y}, focus_extent};
        return views;
    };
    // A real frame of four views: what the backend analyzed.
    struct QuadFrame {
        std::array<QuadView, 4> views;
        std::array<Pixels, 4> pixels;
        FrameGenComposition composition;
        std::uint64_t id{0};
        std::uint64_t submission{0};
        std::chrono::steady_clock::time_point captured{}; // just before its captures
    };
    // Uploads frame `image`'s four views and has the backend capture them,
    // then analyzes them as one real frame (with `merge`, its flow merged;
    // the views in `restarted` without their previous frame).
    auto analyze = [&](FrameGenGpu& backend, std::uint32_t image, const std::array<QuadView, 4>& views,
                       const QuadFrame* previous, const QuadScene& scene, FrameGenTuning tuning, double transition,
                       double sharpness, bool merge, std::uint32_t restarted = 0) {
        QuadFrame frame;
        frame.views = views;
        frame.composition = QuadComposition(views, output, transition, sharpness);
        backend.WaitIdle(); // the image may still be read by an earlier frame
        std::vector<FrameGenViewSubmission> submissions;
        for (std::size_t i = 0; i < 4; ++i) {
            frame.pixels[i] = RenderQuad(views[i], scene);
            harness.upload_sized(g_swapchains[sources[i]][image], views[i].extent.width, views[i].extent.height,
                                 frame.pixels[i]);
            submissions.push_back(submission(sources[i], image, views[i], previous ? previous->views[i] : views[i],
                                             previous != nullptr && !(restarted & (1u << i))));
        }
        // At release, as the presenter captures them.
        frame.captured = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < 4; ++i) {
            const bool captured = backend.Capture(i, submissions[i].source, submissions[i].request, tuning.quality, error);
            Check(captured || error.empty(), harness.name + " quad capture: " + error);
        }
        tuning.analyze_only = true;
        tuning.composition = merge ? &frame.composition : nullptr;
        FrameGenExecuteResult result;
        const bool analyzed = backend.Execute(submissions, tuning, result, error);
        Check(analyzed, harness.name + " quad analyze: " + error);
        Check(result.frame != 0, harness.name + " a quad views analysis names its real frame");
        frame.id = result.frame;
        frame.submission = result.submission;
        return frame;
    };
    // Both eyes of a generated frame.
    auto generate = [&](FrameGenGpu& backend, const QuadFrame& frame, FrameGenSynthesisMode mode, float fraction,
                        const FrameGenComposition& composition) {
        const bool composed = backend.SynthesizeComposed(frame.id, mode, fraction, composition, eye_images, error);
        Check(composed, harness.name + " compose generated frame: " + error);
        return std::array<Pixels, 2>{harness.readback_sized(g_swapchains[outputs[0]][0]),
                                     harness.readback_sized(g_swapchains[outputs[1]][0])};
    };
    // Both eyes composed on the CPU from `scene` seen through `frame`'s views.
    auto composed_truth = [&](const QuadFrame& frame, const FrameGenComposition& composition, const QuadScene& scene) {
        std::array<Pixels, 2> eyes;
        for (std::size_t eye = 0; eye < 2; ++eye) {
            const QuadView& periphery = frame.views[eye];
            const QuadView& focus = frame.views[eye + 2];
            eyes[eye] = ComposeReference(LinearImage(RenderQuad(periphery, scene), periphery.extent),
                                         LinearImage(RenderQuad(focus, scene), focus.extent),
                                         BuildFrameGenComposeConstants(composition.eyes[eye], output), output);
        }
        return eyes;
    };
    // The square (0.3 tangents a side) at time t, moving 0.04 across and
    // 0.02 down per frame (6 and 3 peripheral pixels, 20 and 10 focus).
    auto moving = [&](float t) {
        const float sx = -0.2f + 0.04f * t, sy = 0.18f - 0.02f * t;
        return QuadScene([=](float tx, float ty) {
            const float u = tx - sx, v = sy - ty;
            if (u >= 0.0f && u < 0.3f && v >= 0.0f && v < 0.3f) {
                const float n = QuadDetail(u, v, 40.0f);
                return QuadColor{0.05f + 0.75f * n, 0.4f * (1.0f - n), 0.06f};
            }
            return Scenery(tx, ty);
        });
    };
    // The square's whole path in output pixels (generated frames differ from
    // the newest frame only there), with a margin.
    auto path = [&](std::uint32_t x, std::uint32_t y) {
        const float tx = -0.8f + 1.6f * (x + 0.5f) / output.width, ty = 0.8f - 1.6f * (y + 0.5f) / output.height;
        return tx > -0.33f && tx < 0.37f && ty < 0.28f && ty > -0.24f;
    };
    auto eyes_error = [&](const std::array<Pixels, 2>& eyes, const std::array<Pixels, 2>& truth) {
        return 0.5 * (Compare(eyes[0], truth[0], output, path).mean + Compare(eyes[1], truth[1], output, path).mean);
    };
    if (composes) {
        const auto views = quad_views(0.0f, 0.0f);
        // Interpolating (with cover flow for a late real frame), then
        // extrapolating; on the GPU also with the optical flow hardware:
        // the four views matched on one atlas (flow 1), then with a session
        // each (flow 2).
        for (const int flow : {0, 1, 2}) {
            const bool hardware = flow > 0;
            if (hardware && !g_hardware) continue;
            gpu->SetHardwareAtlas(flow != 2);
            for (const auto mode : {FrameGenSynthesisMode::kInterpolate, FrameGenSynthesisMode::kExtrapolate}) {
                FrameGenTuning tuning;
                tuning.mode = mode;
                tuning.cover = mode == FrameGenSynthesisMode::kInterpolate;
                tuning.hardware_flow = hardware;
                gpu->ResetHistory();
                const QuadFrame a = analyze(*gpu, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
                const QuadFrame b = analyze(*gpu, 1, views, &a, moving(1.0f), tuning, 25.0, 0.0, true);
                // Where one view could use the optical flow hardware, all
                // four views match on it, on the atlas or a session each.
                Check(!hardware || g_flow_status != "active" || gpu->HardwareFlowStatus() == "active",
                      harness.name + " quad views: the optical flow hardware failed with four views: " +
                          gpu->HardwareFlowStatus());
                const std::array<Pixels, 2> hold{
                    ComposeReference(LinearImage(b.pixels[0], periphery_extent), LinearImage(b.pixels[2], focus_extent),
                                     BuildFrameGenComposeConstants(b.composition.eyes[0], output), output),
                    ComposeReference(LinearImage(b.pixels[1], periphery_extent), LinearImage(b.pixels[3], focus_extent),
                                     BuildFrameGenComposeConstants(b.composition.eyes[1], output), output)};
                const bool interpolate = mode == FrameGenSynthesisMode::kInterpolate;
                // One analyzed frame serves all four views: both eyes from its id.
                const auto generated = generate(*gpu, b, mode, 0.5f, b.composition);
                const auto truth = composed_truth(b, b.composition, moving(interpolate ? 0.5f : 1.5f));
                const double error_generated = eyes_error(generated, truth);
                const double error_hold = eyes_error(hold, truth);
                const std::string what = std::string(interpolate ? "interpolate" : "extrapolate") +
                                         (hardware ? std::string(flow == 1 ? " (hardware flow, atlas: " : " (hardware flow, per view: ") +
                                                         gpu->HardwareFlowStatus() + ")"
                                                   : "");
                std::cout << harness.name << " quad views " << what << ": error " << error_generated << " vs hold "
                          << error_hold << '\n';
                DumpSized(harness.name + "-quad-" + (interpolate ? "interpolated" : "extrapolated"), generated[0], output);
                DumpSized(harness.name + "-quad-" + (interpolate ? "interpolated" : "extrapolated") + "-truth", truth[0],
                          output);
                DumpSized(harness.name + "-quad-hold", hold[0], output);
                Check(error_generated < (interpolate ? 0.4 : 0.5) * error_hold,
                      harness.name + " quad views " + what + " is not better than repeating the newest frame");
                // The real frame's own refresh: its views as captured, composed.
                const auto exact = generate(*gpu, b, mode, interpolate ? 1.0f : 0.0f, b.composition);
                Check(Compare(exact[0], hold[0], output, path).max <= 2 && Compare(exact[1], hold[1], output, path).max <= 2,
                      harness.name + " quad views: the real frame's refresh is not its views composed");
                // Far off the axis nothing moves, and the periphery is shaded
                // per pixel there (no coarse shading: the compose magnifies
                // it): the real frame, up to the blend with the previous one
                // (coarse shading on a GPU would quadruple the difference;
                // extrapolating, a little measuring noise moves it on).
                auto off_axis = [&](std::uint32_t x, std::uint32_t y) {
                    const float tx = -0.8f + 1.6f * (x + 0.5f) / output.width, ty = 0.8f - 1.6f * (y + 0.5f) / output.height;
                    return std::hypot(tx, ty) > 1.02f;
                };
                const double still = 0.5 * (Compare(generated[0], exact[0], output, off_axis).mean +
                                            Compare(generated[1], exact[1], output, off_axis).mean);
                std::cout << harness.name << " quad views: still periphery off the axis differs from the real frame by "
                          << still << '\n';
                Check(!interpolate || still < 0.0025, harness.name + " quad views: still periphery far off the axis changed");
                if (variable_rate && interpolate && !hardware) {
                    // That check sees coarse shading: the same frame written
                    // per view (as a canvas is, not composed) is shaded
                    // coarsely there and differs from the real frame more.
                    std::vector<FrameGenImage> view_images;
                    for (std::size_t i = 0; i < 4; ++i) {
                        view_images.push_back({view_outputs[i], 0, 0, whole(views[i].extent), output_format});
                    }
                    Check(gpu->Synthesize(b.id, mode, 0.5f, view_images, error),
                          harness.name + " quad views written per view: " + error);
                    auto view_off_axis = [&](std::uint32_t x, std::uint32_t y) {
                        const float tx = -0.8f + 1.6f * (x + 0.5f) / periphery_extent.width;
                        const float ty = 0.8f - 1.6f * (y + 0.5f) / periphery_extent.height;
                        return std::hypot(tx, ty) > 1.02f;
                    };
                    double coarse = 0.0;
                    for (std::size_t eye = 0; eye < 2; ++eye) {
                        const Pixels written = harness.readback_sized(g_swapchains[view_outputs[eye]][0]);
                        coarse += 0.5 * Compare(written, b.pixels[eye], periphery_extent, view_off_axis).mean;
                    }
                    std::cout << harness.name << " quad views: the periphery written on its own (variable-rate shading) "
                              << "differs there by " << coarse << '\n';
                    Check(coarse > 3.0 * std::max(still, 0.001),
                          harness.name + " quad views: a periphery written on its own is not shaded coarsely off the "
                                         "axis, so the composed periphery's check proves nothing");
                }
                if (interpolate) {
                    // Cover: past the newest frame from the same analysis.
                    const auto cover = generate(*gpu, b, FrameGenSynthesisMode::kExtrapolate, 0.5f, b.composition);
                    const double cover_error = eyes_error(cover, composed_truth(b, b.composition, moving(1.5f)));
                    const double cover_hold = eyes_error(hold, composed_truth(b, b.composition, moving(1.5f)));
                    std::cout << harness.name << " quad views cover: error " << cover_error << " vs hold " << cover_hold
                              << '\n';
                    Check(cover_error < 0.5 * cover_hold, harness.name + " quad views cover frames do not move content");
                }
                if (!interpolate) {
                    // Stale motion: the next real frame moved half a frame on
                    // by this one's motion (its own not measured yet) lands
                    // near where its own motion puts it, far from it held.
                    const QuadFrame c = analyze(*gpu, 2, views, &b, moving(2.0f), tuning, 25.0, 0.0, true);
                    Check(gpu->WaitIdle(), harness.name + " quad views: the analyses did not finish");
                    const auto own = generate(*gpu, c, mode, 0.5f, c.composition);
                    Check(gpu->SynthesizeComposedStale(c.id, b.id, 0.5f, c.composition, eye_images, error),
                          harness.name + " quad views stale motion: " + error);
                    const std::array<Pixels, 2> stale{harness.readback_sized(g_swapchains[outputs[0]][0]),
                                                      harness.readback_sized(g_swapchains[outputs[1]][0])};
                    const auto stale_truth = composed_truth(c, c.composition, moving(2.5f));
                    const std::array<Pixels, 2> c_hold{
                        ComposeReference(LinearImage(c.pixels[0], periphery_extent), LinearImage(c.pixels[2], focus_extent),
                                         BuildFrameGenComposeConstants(c.composition.eyes[0], output), output),
                        ComposeReference(LinearImage(c.pixels[1], periphery_extent), LinearImage(c.pixels[3], focus_extent),
                                         BuildFrameGenComposeConstants(c.composition.eyes[1], output), output)};
                    const double own_error = eyes_error(own, stale_truth);
                    const double stale_error = eyes_error(stale, stale_truth);
                    const double held_error = eyes_error(c_hold, stale_truth);
                    std::cout << harness.name << " quad views stale motion" << (hardware ? " (hardware flow)" : "")
                              << ": error " << stale_error << " vs own motion " << own_error << " vs hold " << held_error
                              << std::endl;
                    Check(stale_error < 0.75 * held_error && stale_error < own_error + 0.25 * held_error,
                          harness.name + " quad views: the previous frame's motion does not move a frame on");
                    Check(!gpu->SynthesizeComposedStale(c.id, a.id, 0.5f, c.composition, eye_images, error),
                          harness.name + " quad views: a frame was moved by motion other than its previous frame's");
                }
                if (!hardware && interpolate) {
                    // Written by compute and copied instead of drawn: the same.
                    gpu->SetDirectOutput(false);
                    const auto copied = generate(*gpu, b, mode, 0.5f, b.composition);
                    gpu->SetDirectOutput(true);
                    const double difference = std::max(Compare(copied[0], generated[0], output, path).mean,
                                                       Compare(copied[1], generated[1], output, path).mean);
                    std::cout << harness.name << " quad views drawn vs copied: difference " << difference << '\n';
                    Check(difference < 0.001, harness.name + " quad views written by compute differ from drawn ones");

                    // An eye's output that cannot be drawn into (not the
                    // composed size) is refused before anything is recorded:
                    // the real frame and its views stay as they were.
                    FrameGenImage narrow = eye_images[1];
                    narrow.rect.extent.width -= 2;
                    Check(!gpu->SynthesizeComposed(b.id, mode, 0.5f, b.composition, {eye_images[0], narrow}, error),
                          harness.name + " a composed eye's output of the wrong size was accepted");
                    Check(gpu->Writable(b.id, false),
                          harness.name + " a refused composed eye's output dropped its real frame (" + error + ")");
                    const auto rewritten = generate(*gpu, b, mode, 0.5f, b.composition);
                    Check(Compare(rewritten[0], generated[0], output, path).max <= 1 &&
                              Compare(rewritten[1], generated[1], output, path).max <= 1,
                          harness.name + " a quad views frame composed after a refused output differs");

                    // A newer real frame with its inset elsewhere is pending:
                    // frames generated from the older one keep its inset.
                    const auto moved_views = quad_views(0.12f, -0.08f);
                    const QuadFrame c = analyze(*gpu, 2, moved_views, &b, moving(2.0f), tuning, 25.0, 0.0, true);
                    const auto kept = generate(*gpu, b, mode, 0.5f, b.composition);
                    const double kept_error = eyes_error(kept, truth);
                    // Composed at the newer frame's inset instead, it is wrong.
                    const double newer_inset = eyes_error(generate(*gpu, b, mode, 0.5f, c.composition), truth);
                    std::cout << harness.name << " quad views older frame with a newer one pending: error " << kept_error
                              << " (" << newer_inset << " at the newer frame's inset)\n";
                    Check(Compare(kept[0], generated[0], output, path).max <= 1 &&
                              Compare(kept[1], generated[1], output, path).max <= 1 && kept_error < 0.5 * newer_inset,
                          harness.name + " a generated frame does not keep its real frame's inset");
                    const auto kept_exact = generate(*gpu, b, mode, 1.0f, b.composition);
                    Check(Compare(kept_exact[0], hold[0], output, path).max <= 2,
                          harness.name + " the older real frame's refresh is not its own views");
                    // Writable says what SynthesizeComposed then does.
                    auto writes = [&](const QuadFrame& frame, float fraction) {
                        return gpu->SynthesizeComposed(frame.id, mode, fraction, frame.composition, eye_images, error);
                    };
                    Check(gpu->Writable(b.id, false) && gpu->Writable(b.id, true) && gpu->Writable(c.id, false),
                          harness.name + " a quad views frame whose images are kept is not writable");
                    // Quad views keep five real frames (FrameGenHistoryRing):
                    // three real frames on, it is still moved; four on, its
                    // previous images are gone: frames moved by it are
                    // refused, its exact copy is not.
                    const QuadFrame d = analyze(*gpu, 3, moved_views, &c, moving(3.0f), tuning, 25.0, 0.0, true);
                    const QuadFrame e = analyze(*gpu, 0, moved_views, &d, moving(4.0f), tuning, 25.0, 0.0, true);
                    Check(gpu->Writable(b.id, false) && writes(b, 0.5f),
                          harness.name + " a quad views frame three real frames on is not composed from: " + error);
                    const QuadFrame f = analyze(*gpu, 1, moved_views, &e, moving(5.0f), tuning, 25.0, 0.0, true);
                    Check(!gpu->Writable(b.id, false) && !writes(b, 0.5f),
                          harness.name + " a quad views frame whose images were replaced is composed from");
                    Check(gpu->Writable(b.id, true) && writes(b, 1.0f),
                          harness.name + " a quad views frame's exact copy is refused while its newest images are kept: " +
                              error);
                    Check(Compare(harness.readback_sized(g_swapchains[outputs[0]][0]), hold[0], output, path).max <= 2,
                          harness.name + " a quad views frame's exact copy is not its own views");
                    // An eye naming a view the frame does not have is refused.
                    FrameGenComposition wrong = c.composition;
                    wrong.eyes[1].focus = 4;
                    Check(!gpu->SynthesizeComposed(c.id, mode, 0.5f, wrong, eye_images, error),
                          harness.name + " a composed eye naming a missing view was accepted");
                    // A newer capture into its newest images (the game's next
                    // release): its exact copy is refused too; the newest
                    // frame is still writable.
                    for (std::size_t i = 0; i < 4; ++i) {
                        const FrameGenViewSubmission next = submission(sources[i], 0, moved_views[i], moved_views[i], true);
                        Check(gpu->Capture(i, next.source, next.request, tuning.quality, error),
                              harness.name + " quad capture: " + error);
                    }
                    Check(!gpu->Writable(b.id, true) && !writes(b, 1.0f),
                          harness.name + " a quad views frame whose newest images were replaced is written");
                    Check(gpu->Writable(f.id, false) && writes(f, 0.5f),
                          harness.name + " the newest quad views frame is not writable: " + error);
                }
            }
        }
        gpu->SetHardwareAtlas(true);

        // The atlas' match that never finishes: the watchdog releases the
        // queue waiting for it, the hardware is not used again, and block
        // matching takes over for all four views.
        if (g_hardware && g_flow_status == "active" && StallChecksAllowed()) {
            auto stalled = CreateFrameGenGpu(harness.binding, error);
            Check(stalled != nullptr, harness.name + " backend: " + error);
            stalled->SetCopyAfterRelease(true);
            for (const XrSwapchain source : sources) Check(stalled->Track(source, EnumerateImages, error), error);
            for (const XrSwapchain out : outputs) Check(stalled->Track(out, EnumerateImages, error), error);
            FrameGenTuning tuning;
            tuning.mode = FrameGenSynthesisMode::kExtrapolate;
            tuning.hardware_flow = true;
            const QuadFrame a = analyze(*stalled, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
            const QuadFrame b = analyze(*stalled, 1, views, &a, moving(1.0f), tuning, 25.0, 0.0, true);
            Check(stalled->HardwareFlowStatus() == "active",
                  harness.name + " quad views atlas: the optical flow hardware is not in use: " + stalled->HardwareFlowStatus());
            stalled->StallHardwareForTest();
            const auto start = std::chrono::steady_clock::now();
            const QuadFrame c = analyze(*stalled, 2, views, &b, moving(2.0f), tuning, 25.0, 0.0, true);
            const QuadFrame d = analyze(*stalled, 3, views, &c, moving(3.0f), tuning, 25.0, 0.0, true);
            Check(stalled->WaitIdle(), harness.name + " quad views atlas: the stalled match was not released");
            const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const QuadFrame e = analyze(*stalled, 0, views, &d, moving(4.0f), tuning, 25.0, 0.0, true);
            std::cout << harness.name << " quad views stalled atlas: released after " << waited << " ms, now "
                      << stalled->HardwareFlowStatus() << '\n';
            Check(stalled->HardwareFlowStatus() == "the optical flow hardware stopped responding" && waited < 2000.0,
                  harness.name + " quad views: a stalled atlas match was not released");
            const QuadFrame f = analyze(*stalled, 1, views, &e, moving(5.0f), tuning, 25.0, 0.0, true);
            const std::array<Pixels, 2> hold{
                ComposeReference(LinearImage(f.pixels[0], periphery_extent), LinearImage(f.pixels[2], focus_extent),
                                 BuildFrameGenComposeConstants(f.composition.eyes[0], output), output),
                ComposeReference(LinearImage(f.pixels[1], periphery_extent), LinearImage(f.pixels[3], focus_extent),
                                 BuildFrameGenComposeConstants(f.composition.eyes[1], output), output)};
            const auto truth = composed_truth(f, f.composition, moving(5.5f));
            const double taken_over = eyes_error(generate(*stalled, f, FrameGenSynthesisMode::kExtrapolate, 0.5f, f.composition), truth);
            std::cout << harness.name << " quad views after a stalled atlas: error " << taken_over << " vs hold "
                      << eyes_error(hold, truth) << '\n';
            Check(taken_over < 0.5 * eyes_error(hold, truth),
                  harness.name + " quad views: block matching did not take over from a stalled atlas");
            stalled->Reset();
        }

        // A focus view limits the motion its periphery would: at the lowest
        // motion limit (2% of the view's width a frame) the periphery's 3%
        // half-frame steps pass, and so do the focus view's 9% (2% of the
        // periphery's width over the 40% the inset spans: 5%).
        FrameGenTuning tuning;
        tuning.mode = FrameGenSynthesisMode::kInterpolate;
        tuning.motion_limit_percent = 2;
        gpu->ResetHistory();
        const QuadFrame a = analyze(*gpu, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
        const QuadFrame b = analyze(*gpu, 1, views, &a, moving(1.0f), tuning, 25.0, 0.0, true);
        std::array<Pixels, 2> hold;
        for (std::size_t eye = 0; eye < 2; ++eye) {
            hold[eye] = ComposeReference(LinearImage(b.pixels[eye], periphery_extent),
                                         LinearImage(b.pixels[eye + 2], focus_extent),
                                         BuildFrameGenComposeConstants(b.composition.eyes[eye], output), output);
        }
        const auto truth = composed_truth(b, b.composition, moving(0.5f));
        const double limited_error = eyes_error(generate(*gpu, b, tuning.mode, 0.5f, b.composition), truth);
        const double limited_hold = eyes_error(hold, truth);
        std::cout << harness.name << " quad views at a 2% motion limit: error " << limited_error << " vs hold "
                  << limited_hold << '\n';
        Check(limited_error < 0.4 * limited_hold, harness.name + " quad views: a focus view limits more motion than its periphery");

        // An eye whose views disagree (its focus view starts over: nothing
        // measured for it) shows its real frame in both, so the two line
        // up; the other eye moves.
        tuning.motion_limit_percent = 10;
        gpu->ResetHistory();
        const QuadFrame c = analyze(*gpu, 2, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
        const QuadFrame d = analyze(*gpu, 3, views, &c, moving(1.0f), tuning, 25.0, 0.0, true, 1u << 2);
        const auto disagreeing = generate(*gpu, d, tuning.mode, 0.5f, d.composition);
        auto all = [](std::uint32_t, std::uint32_t) { return true; };
        const Difference left = Compare(disagreeing[0], hold[0], output, all);
        const double right_error = Compare(disagreeing[1], truth[1], output, path).mean;
        const double right_hold = Compare(hold[1], truth[1], output, path).mean;
        std::cout << harness.name << " quad views, an eye's views disagreeing: that eye differs from its real frame by "
                  << left.max << " steps at most; the other eye's error " << right_error << " vs hold " << right_hold
                  << '\n';
        Check(left.max <= 2, harness.name + " quad views: an eye whose views disagree is not its real frame");
        Check(right_error < 0.4 * right_hold, harness.name + " quad views: the other eye stopped moving");
    }

    // 3. The seam: where an eye blends its views (a quarter of the inset
    // either side of its edge, sharpened), both views' generated frames must
    // move content alike. A facade (windows 12 peripheral pixels apart, 37
    // focus pixels) drifts 3 peripheral pixels a frame behind still bars (an
    // occluder) crossing the band, and the inset moves between the real
    // frames: the periphery's own matching aliases on the windows (as the
    // canvas facade case does), the focus views' does not. With the flow merge
    // the band is within 1.2x of the inset interior's error; without it (the
    // negative control) the band is far worse and the views disagree more.
    if (composes) {
        constexpr float kScroll = 0.02f;  // tangents per frame
        constexpr float kPeriod = 0.075f; // tangents between windows across, 1.33x that down
        auto occluded = [&](float t) {
            return QuadScene([=](float tx, float ty) {
                if (std::abs(tx - 0.02f) < 0.06f || std::abs(ty + 0.01f) < 0.05f) {
                    const float n = QuadDetail(tx, ty, 90.0f);
                    return QuadColor{0.3f + 0.5f * n, 0.04f, 0.08f + 0.3f * n};
                }
                const float u = tx - kScroll * t;
                const float column = u - kPeriod * std::floor(u / kPeriod);
                const float row = ty - 1.33f * kPeriod * std::floor(ty / (1.33f * kPeriod));
                const bool glass = column > 0.25f * kPeriod && column < 0.75f * kPeriod && row > 0.33f * kPeriod &&
                                   row < kPeriod;
                const float n = 0.1f * QuadDetail(u, ty, 5.0f);
                return glass ? QuadColor{0.02f, 0.04f + 0.2f * n, 0.08f} : QuadColor{0.45f + n, 0.42f + n, 0.38f + n};
            });
        };
        struct SeamMetrics {
            double band{0.0}, interior{0.0}, band_hold{0.0}, double_edge{0.0};
        };
        auto seam = [&](bool merge, FrameGenSynthesisMode mode) {
            FrameGenTuning tuning;
            tuning.mode = mode;
            tuning.cover = mode == FrameGenSynthesisMode::kInterpolate;
            gpu->ResetHistory();
            const QuadFrame a = analyze(*gpu, 0, quad_views(0.0f, 0.0f), nullptr, occluded(0.0f), tuning, 25.0, 50.0, merge);
            const QuadFrame b = analyze(*gpu, 1, quad_views(0.03f, 0.02f), &a, occluded(1.0f), tuning, 25.0, 50.0, merge);
            const bool interpolate = mode == FrameGenSynthesisMode::kInterpolate;
            const auto generated = generate(*gpu, b, mode, 0.5f, b.composition);
            const auto truth = composed_truth(b, b.composition, occluded(interpolate ? 0.5f : 1.5f));
            // How far the two views' generated frames disagree in the band:
            // the eye composed from the focus view alone inside the inset
            // (no band) against the periphery alone.
            FrameGenComposition focus_only = b.composition, periphery_only = b.composition;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                focus_only.eyes[eye].feather = {0.0f, 0.0f};
                focus_only.eyes[eye].sharpen = 0.0f;
                periphery_only.eyes[eye].focus_rect = {2.0f, 2.0f, 2.0f, 2.0f};
            }
            const auto focus_alone = generate(*gpu, b, mode, 0.5f, focus_only);
            const auto periphery_alone = generate(*gpu, b, mode, 0.5f, periphery_only);
            SeamMetrics metrics;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                const FrameGenConstants constants = BuildFrameGenComposeConstants(b.composition.eyes[eye], output);
                auto band = [&](std::uint32_t x, std::uint32_t y) {
                    const float alpha = ComposeAlpha(constants, (x + 0.5f) / output.width, (y + 0.5f) / output.height);
                    return alpha > 0.0f && alpha < 1.0f;
                };
                auto interior = [&](std::uint32_t x, std::uint32_t y) {
                    return ComposeAlpha(constants, (x + 0.5f) / output.width, (y + 0.5f) / output.height) >= 1.0f;
                };
                const Pixels hold = ComposeReference(LinearImage(b.pixels[eye], periphery_extent),
                                                     LinearImage(b.pixels[eye + 2], focus_extent), constants, output);
                metrics.band += 0.5 * Compare(generated[eye], truth[eye], output, band).mean;
                metrics.interior += 0.5 * Compare(generated[eye], truth[eye], output, interior).mean;
                metrics.band_hold += 0.5 * Compare(hold, truth[eye], output, band).mean;
                metrics.double_edge += 0.5 * Compare(focus_alone[eye], periphery_alone[eye], output, band).mean;
            }
            DumpSized(harness.name + std::string("-seam-") + (merge ? "merged" : "unmerged"), generated[0], output);
            return metrics;
        };
        for (const auto mode : {FrameGenSynthesisMode::kInterpolate, FrameGenSynthesisMode::kExtrapolate}) {
            const SeamMetrics merged = seam(true, mode);
            const SeamMetrics unmerged = seam(false, mode);
            const std::string what = mode == FrameGenSynthesisMode::kInterpolate ? "interpolate" : "extrapolate";
            std::cout << harness.name << " quad views seam, " << what << ": band error " << merged.band << " vs interior "
                      << merged.interior << " (hold " << merged.band_hold << "), views disagree by " << merged.double_edge
                      << "; without the flow merge: band " << unmerged.band << " vs interior " << unmerged.interior
                      << ", views disagree by " << unmerged.double_edge << '\n';
            // How close each bound is (a ratio of 1 is the limit).
            std::cout << harness.name << " quad views seam, " << what << " margins: band/interior "
                      << merged.band / (1.2 * merged.interior) << ", band/hold " << merged.band / (0.5 * merged.band_hold)
                      << ", merged/unmerged band " << merged.band / (0.5 * unmerged.band) << ", views disagreeing "
                      << merged.double_edge / (0.5 * unmerged.double_edge) << '\n';
            Check(merged.band <= 1.2 * merged.interior,
                  harness.name + " quad views seam (" + what + "): the blend band is worse than the inset");
            Check(merged.band < 0.5 * merged.band_hold,
                  harness.name + " quad views seam (" + what + "): the blend band is not better than holding");
            Check(merged.band < 0.5 * unmerged.band && merged.double_edge < 0.5 * unmerged.double_edge,
                  harness.name + " quad views seam (" + what + "): the flow merge does not line the views up");
        }
    }
    gpu->Reset();

    // 4. GPU-aware pacing: every capture moves the capture mark on, a wait
    // for one returns once the game's queue has run it (and not before, with
    // the queue held back), from any thread; each analysis' GPU times come
    // back once, in order, after the capture it waited for.
    {
        const XrGraphicsBindingD3D12KHR* game = nullptr;
        for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
            if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) game = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
        }
        Check(game != nullptr, "capture marks need the game's Direct3D 12 binding");
        auto backend = CreateFrameGenGpu(harness.binding, error);
        Check(backend != nullptr, harness.name + " backend: " + error);
        backend->SetCopyAfterRelease(true);
        for (std::size_t i = 0; i < 4; ++i) Check(backend->Track(sources[i], EnumerateImages, error), error);
        Check(backend->CaptureMark() == 0 && backend->CaptureReached(0), harness.name + " a capture mark before any capture");
        FrameGenTuning tuning;
        tuning.mode = FrameGenSynthesisMode::kInterpolate;
        tuning.hardware_flow = g_hardware; // on the GPU: the stamps around the optical flow hardware
        const auto views = quad_views(0.0f, 0.0f);
        QuadFrame previous = analyze(*backend, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
        // That analysis copied the images itself (they were not captured):
        // the game's queue reads them again only once it is done.
        backend->WaitIdle();
        const FrameGenViewSubmission view = submission(sources[0], 0, views[0], views[0], false);
        std::uint64_t mark = backend->CaptureMark();
        for (int i = 0; i < 3; ++i) {
            const bool captured = backend->Capture(0, view.source, view.request, tuning.quality, error);
            Check(captured, harness.name + " capture: " + error);
            Check(backend->CaptureMark() > mark, harness.name + " a capture did not move the capture mark on");
            mark = backend->CaptureMark();
        }
        Check(backend->WaitIdle() && backend->CaptureReached(mark) &&
                  backend->WaitCapture(mark, std::chrono::milliseconds(100)),
              harness.name + " a capture that has run is not reached");
        // The game's queue held back: its capture is not reached, and a wait
        // for it ends at its timeout.
        ComPtr<ID3D12Fence> gate;
        Check(SUCCEEDED(game->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "gate fence");
        const test_gates::Guard gate_guard(gate.Get());
        game->queue->Wait(gate.Get(), 1);
        const bool held_captured = backend->Capture(0, view.source, view.request, tuning.quality, error);
        Check(held_captured, harness.name + " capture: " + error);
        const std::uint64_t held = backend->CaptureMark();
        const auto start = std::chrono::steady_clock::now();
        const bool early = backend->WaitCapture(held, std::chrono::milliseconds(60));
        const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Check(!early && !backend->CaptureReached(held) && waited >= 55.0 && waited < 1000.0,
              harness.name + " a wait for a capture held back did not time out (" + std::to_string(waited) + " ms)");
        // Waited for on another thread, as the presenter's wait: released
        // once the queue runs.
        bool reached = false;
        std::thread waiter([&] { reached = backend->WaitCapture(held, std::chrono::milliseconds(5000)); });
        Sleep(20);
        Check(SUCCEEDED(gate->Signal(1)), "gate signal");
        waiter.join();
        Check(reached && backend->CaptureReached(held), harness.name + " a capture that ran was not reached");
        std::cout << harness.name << " capture marks: a held capture's wait timed out after " << waited << " ms\n";

        // Analysis timings: four analyses, each after its views' captures.
        backend->WaitIdle();
        backend->TakeTimings();
        std::vector<QuadFrame> frames;
        for (std::uint32_t i = 1; i <= 4; ++i) {
            frames.push_back(analyze(*backend, i % kQuadImages, views, &previous, moving(static_cast<float>(i)), tuning,
                                     25.0, 0.0, true));
            previous = frames.back();
        }
        Check(backend->WaitIdle(), harness.name + " idle");
        const auto now = std::chrono::steady_clock::now();
        const std::vector<FrameGenAnalysisTiming> timings = backend->TakeTimings();
        Check(timings.size() == frames.size(), harness.name + " an analysis timing is missing (" +
                                                   std::to_string(timings.size()) + " of " +
                                                   std::to_string(frames.size()) + ")");
        // Times from the game's queue and the analysis queue, each on its
        // own calibration: allowed a tenth of a millisecond apart.
        const auto slack = std::chrono::microseconds(100);
        for (std::size_t i = 0; i < timings.size(); ++i) {
            const FrameGenAnalysisTiming& timing = timings[i];
            const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
            std::cout << harness.name << " analysis " << timing.submission << ": captured "
                      << ms(timing.capture_end - frames[i].captured) << " ms after its captures were queued, ran "
                      << ms(timing.start - timing.capture_end) << " ms later for " << ms(timing.end - timing.start)
                      << " ms";
            if (timing.hardware_start != std::chrono::steady_clock::time_point{}) {
                std::cout << " (optical flow hardware " << ms(timing.hardware_end - timing.hardware_start) << " ms)";
            }
            std::cout << '\n';
            Check(timing.submission == frames[i].submission, harness.name + " analysis timings out of order");
            Check(timing.capture_end != std::chrono::steady_clock::time_point{} &&
                      timing.capture_end + slack >= frames[i].captured && timing.capture_end <= timing.start + slack &&
                      timing.start <= timing.end && timing.end <= now + slack,
                  harness.name + " analysis timing stamps out of order");
            if (timing.hardware_start != std::chrono::steady_clock::time_point{}) {
                Check(timing.start <= timing.hardware_start && timing.hardware_start <= timing.hardware_end &&
                          timing.hardware_end <= timing.end,
                      harness.name + " optical flow hardware stamps out of order");
            } else {
                Check(!g_hardware || backend->HardwareFlowStatus() != "active",
                      harness.name + " the optical flow hardware was not timed");
            }
        }
        Check(backend->TakeTimings().empty(), harness.name + " analysis timings returned twice");
        backend->Reset();
    }

    // 4b. The history ring (FrameGenHistoryRing): quad views (analyzed with
    // their composition) keep five real frames, the same views otherwise
    // three, as a canvas does. A real frame is moved until the ring's size
    // less one newer frames were captured, shown until the ring's size; the
    // views' images differ by just the two slots more each (a history image,
    // the flow filtered for and past the newest frame, and depth).
    {
        ID3D12Device* device = nullptr;
        for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
            if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                device = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base)->device;
            }
        }
        Check(device != nullptr, "the history ring's memory needs the game's Direct3D 12 device");
        auto backend = CreateFrameGenGpu(harness.binding, error);
        Check(backend != nullptr, harness.name + " backend: " + error);
        backend->SetCopyAfterRelease(true);
        for (std::size_t i = 0; i < 4; ++i) Check(backend->Track(sources[i], EnumerateImages, error), error);
        FrameGenTuning tuning;
        tuning.mode = FrameGenSynthesisMode::kInterpolate;
        tuning.cover = true;
        const auto views = quad_views(0.0f, 0.0f);
        std::array<std::uint64_t, 2> bytes{}; // composed, not
        for (const bool composed : {true, false}) {
            const std::uint8_t ring = FrameGenHistoryRing(composed);
            QuadFrame newest = analyze(*backend, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, composed);
            const QuadFrame frame = analyze(*backend, 1, views, &newest, moving(1.0f), tuning, 25.0, 0.0, composed);
            Check(backend->WaitIdle(), harness.name + " idle");
            bytes[composed ? 0 : 1] = backend->WorkingMemoryBytes();
            newest = frame;
            for (std::uint32_t newer = 1; newer <= ring; ++newer) {
                newest = analyze(*backend, (newer + 1) % kQuadImages, views, &newest, moving(1.0f + newer), tuning, 25.0,
                                 0.0, composed);
                const bool moved = backend->Writable(frame.id, false), shown = backend->Writable(frame.id, true);
                Check(moved == (newer + 1 < ring) && shown == (newer < ring),
                      harness.name + " a real frame of " + (composed ? "quad views" : "views not composed") + ", " +
                          std::to_string(newer) + " newer ones captured: moved " + (moved ? "writable" : "refused") +
                          ", shown " + (shown ? "writable" : "refused") + " (a ring of " + std::to_string(ring) + ")");
            }
        }
        // A ring slot of the four views, as the backend creates its images.
        auto allocation = [&](FrameGenExtent extent, DXGI_FORMAT format, bool uav) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = extent.width;
            desc.Height = extent.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        };
        std::uint64_t slot = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto levels = FrameGenPyramid(views[i].extent);
            const FrameGenExtent filtered = levels[FrameGenFinalFlowLevel(tuning)];
            slot += allocation(views[i].extent, static_cast<ID3D12Resource*>(g_swapchains[sources[i]][0])->GetDesc().Format,
                               false) +
                    2 * allocation(filtered, DXGI_FORMAT_R16G16B16A16_FLOAT, true) +
                    allocation(levels[1], DXGI_FORMAT_R32_FLOAT, true);
        }
        const std::uint64_t slots = FrameGenHistoryRing(true) - FrameGenHistoryRing(false);
        std::cout << harness.name << " history ring: quad views " << bytes[0] / 1024 << " KiB, not composed "
                  << bytes[1] / 1024 << " KiB (" << slots << " slots of " << slot / 1024 << " KiB)\n";
        Check(bytes[0] == bytes[1] + slots * slot,
              harness.name + " quad views' working memory is not their views' otherwise and two ring slots more");
        backend->Reset();
    }

    // 5. Leaving quad views (a canvas of two views from then on): the focus
    // views' images are freed once the GPU is done with them, frames
    // analyzed with them are no longer written, and quad views coming back
    // set them up again (on the GPU, with their optical flow sessions).
    {
        auto backend = CreateFrameGenGpu(harness.binding, error);
        Check(backend != nullptr, harness.name + " backend: " + error);
        backend->SetCopyAfterRelease(true);
        for (std::size_t i = 0; i < 4; ++i) Check(backend->Track(sources[i], EnumerateImages, error), error);
        for (const XrSwapchain out : outputs) Check(backend->Track(out, EnumerateImages, error), error);
        FrameGenTuning tuning;
        tuning.mode = FrameGenSynthesisMode::kInterpolate;
        tuning.cover = true;
        tuning.hardware_flow = g_hardware;
        const auto views = quad_views(0.0f, 0.0f);
        const QuadFrame a = analyze(*backend, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
        const QuadFrame b = analyze(*backend, 1, views, &a, moving(1.0f), tuning, 25.0, 0.0, true);
        if (composes) generate(*backend, b, tuning.mode, 0.5f, b.composition); // the compose targets too
        const std::uint64_t quad_bytes = backend->WorkingMemoryBytes();
        // The least freeing the focus views gives back: their history rings
        // (RGBA8 at their size), whatever else they hold on this device.
        const std::uint64_t focus_history =
            2ull * FrameGenHistoryRing(true) * focus_extent.width * focus_extent.height * 4;
        FrameGenTuning canvas = tuning;
        canvas.analyze_only = true;
        const std::vector<FrameGenViewSubmission> pair{submission(sources[0], 2, views[0], views[0], false),
                                                       submission(sources[1], 2, views[1], views[1], false)};
        FrameGenExecuteResult canvas_result;
        // Not while a frame written from them waits on the runtime's queue
        // (held back here, so nothing the test does may wait for it): the
        // two views measured in the quad views' ring then (a canvas's ring
        // sets them up anew, which waits for the GPU, as any other change of
        // their images does).
        FrameGenTuning two_views = canvas;
        const FrameGenComposition no_eyes;
        two_views.composition = &no_eyes;
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* runtime_queue = nullptr;
        for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
            if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                device = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base)->device;
            } else if (base->type == kFrameGenRuntimeQueueType) {
                runtime_queue = static_cast<ID3D12CommandQueue*>(reinterpret_cast<const FrameGenRuntimeQueue*>(base)->d3d12_queue);
            }
        }
        if (composes && device && runtime_queue) {
            Check(backend->WaitIdle(), harness.name + " idle");
            ComPtr<ID3D12Fence> gate;
            Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "gate fence");
            const test_gates::Guard gate_guard(gate.Get());
            runtime_queue->Wait(gate.Get(), 1);
            const bool written = backend->SynthesizeComposed(b.id, tuning.mode, 0.5f, b.composition, eye_images, error);
            const bool executed = written && backend->Execute(pair, two_views, canvas_result, error);
            const std::uint64_t pending_bytes = backend->WorkingMemoryBytes();
            Check(SUCCEEDED(gate->Signal(1)), "gate signal");
            Check(written && executed, harness.name + " a canvas after quad views, a frame pending: " + error);
            Check(pending_bytes + focus_history > quad_bytes,
                  harness.name + " the focus views' images were freed while a frame written from them was pending");
        }
        // The peripheral views alone, once the GPU is done with the others.
        Check(backend->WaitIdle(), harness.name + " idle");
        Check(backend->Execute(pair, canvas, canvas_result, error), harness.name + " canvas analyze: " + error);
        const std::uint64_t canvas_bytes = backend->WorkingMemoryBytes();
        std::cout << harness.name << " quad views left for a canvas: working memory " << quad_bytes / 1024 << " KiB -> "
                  << canvas_bytes / 1024 << " KiB (the focus views' history rings: " << focus_history / 1024 << " KiB)\n";
        Check(canvas_bytes > 0 && canvas_bytes + focus_history <= quad_bytes,
              harness.name + " the focus views' images are kept after leaving quad views");
        Check(!backend->Writable(b.id, true) && !backend->SynthesizeComposed(b.id, tuning.mode, 1.0f, b.composition,
                                                                             eye_images, error),
              harness.name + " a quad views frame is written after its focus views' images were freed");
        Check(backend->Writable(canvas_result.frame, true), harness.name + " the canvas frame is not writable");
        // Quad views again: set up anew, measured and composed.
        const QuadFrame c = analyze(*backend, 2, views, nullptr, moving(2.0f), tuning, 25.0, 0.0, true);
        const QuadFrame d = analyze(*backend, 3, views, &c, moving(3.0f), tuning, 25.0, 0.0, true);
        Check(backend->Writable(d.id, false), harness.name + " quad views coming back are not writable");
        Check(!g_hardware || g_flow_status != "active" || backend->HardwareFlowStatus() == "active",
              harness.name + " quad views coming back lost the optical flow hardware: " + backend->HardwareFlowStatus());
        if (composes) {
            const auto again = generate(*backend, d, tuning.mode, 0.5f, d.composition);
            const auto truth = composed_truth(d, d.composition, moving(2.5f));
            std::array<Pixels, 2> hold;
            for (std::size_t eye = 0; eye < 2; ++eye) {
                hold[eye] = ComposeReference(LinearImage(d.pixels[eye], periphery_extent),
                                             LinearImage(d.pixels[eye + 2], focus_extent),
                                             BuildFrameGenComposeConstants(d.composition.eyes[eye], output), output);
            }
            const double again_error = eyes_error(again, truth), again_hold = eyes_error(hold, truth);
            std::cout << harness.name << " quad views back from a canvas: error " << again_error << " vs hold "
                      << again_hold << '\n';
            Check(again_error < 0.4 * again_hold, harness.name + " quad views coming back are not generated");
        }
        backend->Reset();

        // 6. Torn down while frames written from the views wait on the
        // runtime's queue (a GPU stall: held back here), nothing they use is
        // freed under them; on the GPU (its fence waits give up after 2 s,
        // WARP's after 40), with the periphery's shading-rate images in use.
        if (variable_rate && composes && device && runtime_queue) {
            auto stalled = CreateFrameGenGpu(harness.binding, error);
            Check(stalled != nullptr, harness.name + " backend: " + error);
            stalled->SetCopyAfterRelease(true);
            for (std::size_t i = 0; i < 4; ++i) {
                Check(stalled->Track(sources[i], EnumerateImages, error) &&
                          stalled->Track(view_outputs[i], EnumerateImages, error),
                      error);
            }
            for (const XrSwapchain out : outputs) Check(stalled->Track(out, EnumerateImages, error), error);
            const QuadFrame e = analyze(*stalled, 0, views, nullptr, moving(0.0f), tuning, 25.0, 0.0, true);
            const QuadFrame f = analyze(*stalled, 1, views, &e, moving(1.0f), tuning, 25.0, 0.0, true);
            std::vector<FrameGenImage> view_images;
            for (std::size_t i = 0; i < 4; ++i) {
                view_images.push_back({view_outputs[i], 0, 0, whole(views[i].extent), output_format});
            }
            Check(stalled->WaitIdle(), harness.name + " idle");
            ComPtr<ID3D12Fence> gate;
            Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "gate fence");
            const test_gates::Guard gate_guard(gate.Get());
            runtime_queue->Wait(gate.Get(), 1);
            const bool written = stalled->Synthesize(f.id, tuning.mode, 0.5f, view_images, error) &&
                                 stalled->SynthesizeComposed(f.id, tuning.mode, 0.5f, f.composition, eye_images, error);
            const auto before = stalled->MemoryBudget();
            const auto start = std::chrono::steady_clock::now();
            stalled->Reset();
            const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto after = stalled->MemoryBudget();
            Check(SUCCEEDED(gate->Signal(1)), "gate signal");
            Check(written, harness.name + " frames written before a stall: " + error);
            if (before && after) {
                const double freed = (static_cast<double>(before->usage_bytes) - static_cast<double>(after->usage_bytes)) / 1024.0;
                std::cout << harness.name << " torn down during a stall (after " << waited << " ms): " << freed
                          << " KiB of video memory freed\n";
                Check(freed < 64.0, harness.name + " teardown during a stall freed images frames waiting to run use");
            }
        }
    }
    std::cout << harness.name << " quad views passed\n";
    return 0;
}

// FG_PROFILE_LOAD_MS=N: how late captures and analyses finish while the
// game's GPU is busy: N ms of "game" work (a second backend generating
// frames on the game's queue, as `contention` does) held back by a fence
// with each real frame's captures queued behind it, then released at once.
// `real_frame` captures and analyzes one real frame on `backend` (its
// captures queued) and returns its FrameGenExecuteResult::submission.
void ProfileUnderLoad(Harness& harness, double load_ms, const std::function<std::uint64_t(FrameGenGpu&, int)>& real_frame) {
    std::string error;
    const XrGraphicsBindingD3D12KHR* game = nullptr;
    for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
        if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) game = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
    }
    Check(game != nullptr, "the load needs a Direct3D 12 binding");
    _putenv_s("VECTORXR_FRAMEGEN_QUEUE", "game");
    auto load = CreateFrameGenGpu(game, error);
    _putenv_s("VECTORXR_FRAMEGEN_QUEUE", "");
    Check(load != nullptr, "load backend: " + error);
    load->SetCopyAfterRelease(true);
    FrameGenTuning load_tuning;
    load_tuning.mode = FrameGenSynthesisMode::kExtrapolate;
    load_tuning.generated = 4;
    load_tuning.fractions = {0.25f, 0.5f, 0.75f, 1.0f};
    FrameGenExecuteResult load_result;
    // Three Executes a frame (all the gate can hold back: a fourth waits for
    // the first's slot), each a square view whose GPU time grows with its
    // area: measured at one size, then sized for a third of the load.
    FrameGenViewSubmission load_view;
    auto size_load = [&](std::uint32_t size, std::uintptr_t first_chain) {
        const XrSwapchain source = reinterpret_cast<XrSwapchain>(first_chain);
        g_swapchains[source] = {harness.create(size, size)};
        harness.upload_sized(g_swapchains[source][0], size, size, CockpitAt(size, size, 0.0f, -1.5f, 1.5f, 1.5f, 0.0f));
        Check(load->Track(source, EnumerateImages, error), error);
        const XrRect2Di rect{{0, 0}, {static_cast<int>(size), static_cast<int>(size)}};
        load_view = {};
        load_view.source = {source, 0, 0, rect, harness.format};
        for (std::uintptr_t i = 1; i <= 4; ++i) {
            const XrSwapchain output = reinterpret_cast<XrSwapchain>(first_chain + i);
            g_swapchains[output] = {harness.create(size, size)};
            Check(load->Track(output, EnumerateImages, error), error);
            load_view.outputs.push_back({output, 0, 0, rect, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB});
        }
        load_view.request.extent = {size, size};
        load_view.request.synthesize = true;
        load_view.request.temporal = true;
        std::vector<double> times;
        for (int i = 0; i < 12; ++i) {
            const bool loaded = load->Execute({load_view}, load_tuning, load_result, error);
            Check(loaded, "load execute: " + error);
            load->WaitIdle();
            if (i >= 6 && load->LastGpuMilliseconds() > 0.0) times.push_back(load->LastGpuMilliseconds());
        }
        Check(!times.empty(), "the load's GPU time");
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    };
    std::uint32_t size = 2048;
    double per_execute = size_load(size, 0xe0);
    for (const std::uintptr_t chain : {0xe8u, 0xf0u}) {
        // Fixed costs do not grow with the area: sized twice.
        size = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(size * std::sqrt(load_ms / 3.0 / per_execute)) / 64u * 64u, 256u, 8192u);
        per_execute = size_load(size, chain);
    }

    auto backend = CreateFrameGenGpu(harness.binding, error);
    Check(backend != nullptr, "backend: " + error);
    backend->SetCopyAfterRelease(true);
    ComPtr<ID3D12Fence> gate;
    Check(SUCCEEDED(game->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "load gate");
    const test_gates::Guard gate_guard(gate.Get());
    UINT64 gate_value = 0;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> released;
    std::vector<double> capture_done, measure_done;
    for (int frame = 0; frame < 48; ++frame) {
        // The first frames set the views up (which waits for the GPU): unloaded.
        const bool loaded = frame >= 2;
        if (loaded) {
            game->queue->Wait(gate.Get(), ++gate_value);
            for (int i = 0; i < 3; ++i) {
                const bool queued = load->Execute({load_view}, load_tuning, load_result, error);
                Check(queued, "load execute: " + error);
            }
        }
        const std::uint64_t submission = real_frame(*backend, frame);
        released[submission] = std::chrono::steady_clock::now();
        if (loaded) {
            Check(SUCCEEDED(gate->Signal(gate_value)), "load gate signal");
        }
        // A frame's time: the load, then the analysis, done before the next.
        Sleep(static_cast<DWORD>(std::max(11.0, load_ms * 1.5) + 4.0));
        for (const FrameGenAnalysisTiming& timing : backend->TakeTimings()) {
            const auto found = released.find(timing.submission);
            if (frame < 12 || found == released.end() || timing.capture_end == std::chrono::steady_clock::time_point{}) {
                continue;
            }
            capture_done.push_back(std::chrono::duration<double, std::milli>(timing.capture_end - found->second).count());
            measure_done.push_back(std::chrono::duration<double, std::milli>(timing.end - found->second).count());
        }
    }
    auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? -1.0 : values[values.size() / 2];
    };
    std::printf("\nCaptures queued behind %.1f ms of the game's GPU work (3 x %.2f ms at %ux%u): captures done %.2f ms "
                "after the game's queue ran (median), motion measured %.2f ms after (%zu frames)\n",
                3.0 * per_execute, per_execute, size, size, median(capture_done), median(measure_done),
                capture_done.size());
    load->WaitIdle();
    backend->Reset();
    load->Reset();
}

// A game keeping the GPU busy without a break (FG_PROFILE_D3D11_LOAD=N:
// N ms of work a frame, in chunks of about half a millisecond, at most two
// frames queued), beside Frame Generation's analyses.
class GpuLoad {
  public:
    virtual ~GpuLoad() = default;
    virtual const char* Name() const = 0;
    virtual void Start() = 0;
    // Ends the load once its queued frames have run (never abandons them).
    virtual void Stop() = 0;
    virtual double ChunkMs() const = 0;
    virtual int ChunksPerFrame() const = 0;
    virtual std::uint64_t Frames() const = 0;
    // Since Start: the load thread's time submitting its work (a driver
    // lock shows here) and waiting for its frames on the GPU.
    virtual double SubmitMs() const = 0;
    virtual double WaitMs() const = 0;
};

// FG_PROFILE_LOAD_API=d3d11 (the default): a game like DCS, rendering on a
// Direct3D 11 device of its own beside Frame Generation's Direct3D 12 one
// (the bridge's case): full-screen draws.
class D3D11Load final : public GpuLoad {
  public:
    D3D11Load(LUID luid, double frame_ms, double chunk_ms = 0.5) : chunk_target_ms_(chunk_ms) {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                  SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))),
              "the load's adapter");
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                          D3D11_SDK_VERSION, &device_, nullptr, &context_)),
              "the load's Direct3D 11 device");
        static const char source[] = R"(
cbuffer Load : register(b0) { uint iterations; uint3 pad; };
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float4 v = float4(position.xy * 0.001, 0.5, 1.0);
    [loop] for (uint i = 0; i < iterations; ++i) { v = frac(v * 1.0001 + v.yzwx * 0.37 + 0.11); }
    return v;
}
)";
        ComPtr<ID3DBlob> vs, ps;
        Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "load", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs,
                                   nullptr)) &&
                  SUCCEEDED(D3DCompile(source, std::strlen(source), "load", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                                       &ps, nullptr)),
              "the load's shaders");
        Check(SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
                  SUCCEEDED(device_->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)),
              "the load's shader objects");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = kSize;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Check(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &target_)) &&
                  SUCCEEDED(device_->CreateRenderTargetView(target_.Get(), nullptr, &rtv_)),
              "the load's target");
        D3D11_BUFFER_DESC constants{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        Check(SUCCEEDED(device_->CreateBuffer(&constants, nullptr, &constants_)), "the load's constants");
        const D3D11_QUERY_DESC event{D3D11_QUERY_EVENT, 0};
        for (auto& query : frames_) Check(SUCCEEDED(device_->CreateQuery(&event, &query)), "the load's frame query");
        Calibrate(frame_ms);
    }

    ~D3D11Load() override { Stop(); }

    const char* Name() const override { return "Direct3D 11 (own device)"; }

    void Start() override {
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() override {
        if (!thread_.joinable()) return;
        stop_ = true;
        thread_.join();
        context_->Flush();
        Idle();
    }

    double ChunkMs() const override { return chunk_ms_; }
    int ChunksPerFrame() const override { return chunks_; }
    std::uint64_t Frames() const override { return frames_done_; }
    double SubmitMs() const override { return submit_ms_; }
    double WaitMs() const override { return wait_ms_; }

  private:
    static constexpr UINT kSize = 2048;

    void Bind(std::uint32_t iterations) {
        const std::uint32_t data[4]{iterations, 0, 0, 0};
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, data, 0, 0);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    }

    void Idle() {
        context_->End(frames_[0].Get());
        context_->Flush();
        BOOL done = FALSE;
        while (context_->GetData(frames_[0].Get(), &done, sizeof(done), 0) == S_FALSE) std::this_thread::yield();
    }

    // One draw's GPU time at `iterations` (median of five).
    double Measure(std::uint32_t iterations) {
        ComPtr<ID3D11Query> disjoint, begin, end;
        const D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
        Check(SUCCEEDED(device_->CreateQuery(&disjoint_desc, &disjoint)) && SUCCEEDED(device_->CreateQuery(&stamp, &begin)) &&
                  SUCCEEDED(device_->CreateQuery(&stamp, &end)),
              "the load's timing queries");
        Bind(iterations);
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            context_->Begin(disjoint.Get());
            context_->End(begin.Get());
            context_->Draw(3, 0);
            context_->End(end.Get());
            context_->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
            while (context_->GetData(disjoint.Get(), &frequency, sizeof(frequency), 0) == S_FALSE) std::this_thread::yield();
            UINT64 a = 0, b = 0;
            while (context_->GetData(begin.Get(), &a, sizeof(a), 0) == S_FALSE) std::this_thread::yield();
            while (context_->GetData(end.Get(), &b, sizeof(b), 0) == S_FALSE) std::this_thread::yield();
            if (!frequency.Disjoint && b > a) times.push_back(static_cast<double>(b - a) * 1000.0 / frequency.Frequency);
        }
        std::sort(times.begin(), times.end());
        return times.empty() ? 0.0 : times[times.size() / 2];
    }

    void Calibrate(double frame_ms) {
        std::uint32_t iterations = 64;
        double ms = Measure(iterations);
        while (ms < chunk_target_ms_ * 0.5 && iterations < (1u << 22)) {
            iterations *= 2;
            ms = Measure(iterations);
        }
        iterations_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(iterations * chunk_target_ms_ / std::max(ms, 1e-3)));
        chunk_ms_ = Measure(iterations_);
        chunks_ = std::max(1, static_cast<int>(std::lround(frame_ms / std::max(chunk_ms_, 1e-3))));
    }

    void Run() {
        Bind(iterations_);
        std::uint64_t frame = 0;
        submit_ms_ = wait_ms_ = 0.0;
        using Clock = std::chrono::steady_clock;
        auto ms = [](Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); };
        while (!stop_) {
            const auto submitting = Clock::now();
            for (int chunk = 0; chunk < chunks_; ++chunk) {
                context_->Draw(3, 0);
                if (chunk % 4 == 3) context_->Flush();
            }
            context_->End(frames_[frame % frames_.size()].Get());
            context_->Flush();
            submit_ms_ += ms(submitting);
            ++frame;
            // At most two frames queued, as a game's present allows.
            if (frame >= 2) {
                const auto waiting = Clock::now();
                ID3D11Query* oldest = frames_[(frame - 2) % frames_.size()].Get();
                BOOL done = FALSE;
                while (context_->GetData(oldest, &done, sizeof(done), 0) == S_FALSE) std::this_thread::yield();
                wait_ms_ += ms(waiting);
                ++frames_done_; // counted across runs: Frames() differences are a run's frames
            }
        }
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Buffer> constants_;
    std::array<ComPtr<ID3D11Query>, 3> frames_;
    double chunk_target_ms_{0.5};
    std::uint32_t iterations_{1};
    double chunk_ms_{0.0};
    int chunks_{1};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> frames_done_{0};
    double submit_ms_{0.0};
    double wait_ms_{0.0};
};

// FG_PROFILE_LOAD_API=d3d12-same: a game like MSFS, on Frame Generation's own
// Direct3D 12 device (its queue of its own, as a game's); d3d12-separate: on
// a Direct3D 12 device of its own. Compute dispatches filling the GPU.
class D3D12Load final : public GpuLoad {
  public:
    D3D12Load(ID3D12Device* same_device, LUID luid, double frame_ms, double chunk_ms)
        : chunk_target_ms_(chunk_ms), same_(same_device != nullptr) {
        if (same_device) {
            device_ = same_device;
        } else {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter> adapter;
            Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                      SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) &&
                      SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_))),
                  "the load's Direct3D 12 device");
        }
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        Check(SUCCEEDED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))), "the load's queue");
        for (auto& allocator : allocators_) {
            Check(SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))),
                  "the load's allocator");
        }
        Check(SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(), nullptr,
                                                   IID_PPV_ARGS(&list_))) &&
                  SUCCEEDED(list_->Close()),
              "the load's list");
        Check(SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))), "the load's fence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        static const char source[] = R"(
cbuffer Load : register(b0) { uint iterations; };
RWByteAddressBuffer output : register(u0);
[numthreads(64, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    float4 v = float4(id.x * 0.001, 0.5, 0.25, 1.0);
    [loop] for (uint i = 0; i < iterations; ++i) { v = frac(v * 1.0001 + v.yzwx * 0.37 + 0.11); }
    output.Store((id.x & 1023u) * 4u, asuint(v.x + v.y + v.z + v.w));
}
)";
        ComPtr<ID3DBlob> cs;
        Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "load", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &cs,
                                   nullptr)),
              "the load's compute shader");
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 1;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC root{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized;
        Check(SUCCEEDED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr)) &&
                  SUCCEEDED(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                         IID_PPV_ARGS(&root_))),
              "the load's root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        Check(SUCCEEDED(device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipeline_))), "the load's pipeline");
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 4096;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&output_))),
              "the load's buffer");
        D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        Check(SUCCEEDED(device_->CreateQueryHeap(&query, IID_PPV_ARGS(&queries_))), "the load's timestamps");
        D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
        buffer.Width = 16;
        buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
        Check(SUCCEEDED(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))),
              "the load's readback");
        Calibrate(frame_ms);
    }

    ~D3D12Load() override {
        Stop();
        if (event_) CloseHandle(event_);
    }

    const char* Name() const override { return same_ ? "Direct3D 12 (Frame Generation's device)" : "Direct3D 12 (own device)"; }

    void Start() override {
        stop_ = false;
        thread_ = std::thread([this] { Run(); });
    }

    void Stop() override {
        if (!thread_.joinable()) return;
        stop_ = true;
        thread_.join();
        WaitFor(value_);
    }

    double ChunkMs() const override { return chunk_ms_; }
    int ChunksPerFrame() const override { return chunks_; }
    std::uint64_t Frames() const override { return frames_done_; }
    double SubmitMs() const override { return submit_ms_; }
    double WaitMs() const override { return wait_ms_; }

  private:
    static constexpr UINT kGroups = 8192;

    void WaitFor(UINT64 value) {
        if (fence_->GetCompletedValue() >= value) return;
        Check(SUCCEEDED(fence_->SetEventOnCompletion(value, event_)), "the load's fence wait");
        WaitForSingleObject(event_, INFINITE);
    }

    void Record(ID3D12CommandAllocator* allocator, std::uint32_t iterations, int dispatches, bool timed) {
        Check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list_->Reset(allocator, pipeline_.Get())), "the load's reset");
        list_->SetComputeRootSignature(root_.Get());
        list_->SetComputeRoot32BitConstant(0, iterations, 0);
        list_->SetComputeRootUnorderedAccessView(1, output_->GetGPUVirtualAddress());
        if (timed) list_->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (int i = 0; i < dispatches; ++i) list_->Dispatch(kGroups, 1, 1);
        if (timed) {
            list_->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list_->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback_.Get(), 0);
        }
        Check(SUCCEEDED(list_->Close()), "the load's close");
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        Check(SUCCEEDED(queue_->Signal(fence_.Get(), ++value_)), "the load's signal");
    }

    // One dispatch's GPU time at `iterations` (median of five).
    double Measure(std::uint32_t iterations) {
        UINT64 frequency = 0;
        Check(SUCCEEDED(queue_->GetTimestampFrequency(&frequency)), "the load's timestamp frequency");
        std::vector<double> times;
        for (int i = 0; i < 5; ++i) {
            Record(allocators_[0].Get(), iterations, 1, true);
            WaitFor(value_);
            void* mapped = nullptr;
            const D3D12_RANGE range{0, 16};
            Check(SUCCEEDED(readback_->Map(0, &range, &mapped)), "the load's readback map");
            const auto* stamps = static_cast<const UINT64*>(mapped);
            if (stamps[1] > stamps[0]) times.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / frequency);
            const D3D12_RANGE none{0, 0};
            readback_->Unmap(0, &none);
        }
        std::sort(times.begin(), times.end());
        return times.empty() ? 0.0 : times[times.size() / 2];
    }

    void Calibrate(double frame_ms) {
        std::uint32_t iterations = 64;
        double ms = Measure(iterations);
        while (ms < chunk_target_ms_ * 0.5 && iterations < (1u << 22)) {
            iterations *= 2;
            ms = Measure(iterations);
        }
        iterations_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(iterations * chunk_target_ms_ / std::max(ms, 1e-3)));
        chunk_ms_ = Measure(iterations_);
        chunks_ = std::max(1, static_cast<int>(std::lround(frame_ms / std::max(chunk_ms_, 1e-3))));
    }

    void Run() {
        std::uint64_t frame = 0;
        std::array<UINT64, 3> frame_values{};
        submit_ms_ = wait_ms_ = 0.0;
        using Clock = std::chrono::steady_clock;
        auto ms = [](Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); };
        while (!stop_) {
            // At most two frames queued, as a game's present allows.
            if (frame >= 2) {
                const auto waiting = Clock::now();
                WaitFor(frame_values[(frame - 2) % frame_values.size()]);
                wait_ms_ += ms(waiting);
                ++frames_done_; // counted across runs: Frames() differences are a run's frames
            }
            const auto submitting = Clock::now();
            // A list per four dispatches, as a game submits its frame in parts.
            for (int chunk = 0; chunk < chunks_; chunk += 4) {
                Record(PartAllocator(frame, chunk), iterations_, std::min(4, chunks_ - chunk), false);
            }
            frame_values[frame % frame_values.size()] = value_;
            submit_ms_ += ms(submitting);
            ++frame;
        }
    }

    // A frame's parts each get an allocator of their own (reused once that
    // frame is done, two frames later).
    ID3D12CommandAllocator* PartAllocator(std::uint64_t frame, int chunk) {
        const std::size_t part = static_cast<std::size_t>(chunk / 4);
        auto& allocators = part_allocators_[frame % 3];
        while (allocators.size() <= part) {
            ComPtr<ID3D12CommandAllocator> allocator;
            Check(SUCCEEDED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))),
                  "the load's part allocator");
            allocators.push_back(allocator);
        }
        return allocators[part].Get();
    }

    double chunk_target_ms_{0.5};
    bool same_{false};
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    std::array<ComPtr<ID3D12CommandAllocator>, 1> allocators_;
    std::array<std::vector<ComPtr<ID3D12CommandAllocator>>, 3> part_allocators_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    UINT64 value_{0};
    HANDLE event_{nullptr};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    ComPtr<ID3D12Resource> output_;
    ComPtr<ID3D12QueryHeap> queries_;
    ComPtr<ID3D12Resource> readback_;
    std::uint32_t iterations_{1};
    double chunk_ms_{0.0};
    int chunks_{1};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> frames_done_{0};
    double submit_ms_{0.0};
    double wait_ms_{0.0};
};

// The load FG_PROFILE_LOAD_API names (d3d11, d3d12-same, d3d12-separate),
// beside Frame Generation's device `device`.
std::unique_ptr<GpuLoad> MakeLoad(ID3D12Device* device, double frame_ms, double chunk_ms) {
    const std::string api = std::getenv("FG_PROFILE_LOAD_API") ? std::getenv("FG_PROFILE_LOAD_API") : "d3d11";
    if (api == "d3d12-same") return std::make_unique<D3D12Load>(device, device->GetAdapterLuid(), frame_ms, chunk_ms);
    if (api == "d3d12-separate") return std::make_unique<D3D12Load>(nullptr, device->GetAdapterLuid(), frame_ms, chunk_ms);
    Check(api == "d3d11", "FG_PROFILE_LOAD_API is d3d11, d3d12-same or d3d12-separate");
    return std::make_unique<D3D11Load>(device->GetAdapterLuid(), frame_ms, chunk_ms);
}

// Times real frames' analyses (captures, then the analysis, every 14 ms as
// a game at about 70 fps releases them) from the analysis' submission: how
// long before the GPU started it, how long it ran, how long it waited for
// the optical flow hardware, and when it was done.
struct AnalysisLatency {
    std::vector<double> start, run, hardware, done, capture, calls;
};

AnalysisLatency TimeAnalyses(FrameGenGpu& backend, int frames,
                             const std::function<std::uint64_t(FrameGenGpu&, int)>& real_frame) {
    AnalysisLatency latency;
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> submitted;
    auto take = [&](int frame) {
        for (const FrameGenAnalysisTiming& timing : backend.TakeTimings()) {
            const auto found = submitted.find(timing.submission);
            if (frame < 16 || found == submitted.end() || timing.start == std::chrono::steady_clock::time_point{}) {
                continue;
            }
            auto ms = [&](std::chrono::steady_clock::time_point t) {
                return std::chrono::duration<double, std::milli>(t - found->second).count();
            };
            latency.start.push_back(ms(timing.start));
            latency.run.push_back(std::chrono::duration<double, std::milli>(timing.end - timing.start).count());
            latency.done.push_back(ms(timing.end));
            if (timing.hardware_start != std::chrono::steady_clock::time_point{}) {
                latency.hardware.push_back(
                    std::chrono::duration<double, std::milli>(timing.hardware_end - timing.hardware_start).count());
                latency.calls.push_back(timing.hardware_call_ms);
            }
            if (timing.capture_end != std::chrono::steady_clock::time_point{}) latency.capture.push_back(ms(timing.capture_end));
        }
    };
    auto next = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        const std::uint64_t submission = real_frame(backend, frame);
        submitted[submission] = std::chrono::steady_clock::now();
        next += std::chrono::milliseconds(14);
        std::this_thread::sleep_until(next);
        take(frame);
    }
    backend.WaitIdle();
    take(frames);
    return latency;
}

std::string Percentiles(std::vector<double> values) {
    if (values.empty()) return "      -";
    std::sort(values.begin(), values.end());
    char text[64];
    std::snprintf(text, sizeof(text), "%6.2f / %6.2f", values[values.size() / 2], values[values.size() * 9 / 10]);
    return text;
}

void PrintLatency(const char* label, const AnalysisLatency& latency) {
    std::printf("  %-34s started %s  ran %s  hardware %s  done %s  (median / p90 ms after submission, %zu analyses)\n",
                label, Percentiles(latency.start).c_str(), Percentiles(latency.run).c_str(),
                Percentiles(latency.hardware).c_str(), Percentiles(latency.done).c_str(), latency.done.size());
    if (!latency.calls.empty()) {
        std::printf("    the hardware's execute calls took %s ms on the CPU\n", Percentiles(latency.calls).c_str());
    }
}

// FG_PROFILE_VK_OF=1 (with FG_PROFILE_D3D11_LOAD): NVIDIA's optical flow
// through Vulkan (VK_NV_optical_flow) instead of its Direct3D 12 interface:
// the four quad views' matches (their half-size luma, as the Direct3D 12
// hardware path matches) submitted on Vulkan's optical flow queue every
// 14 ms, timed from submission to completion, idle and beside the
// Direct3D 11 load. FG_PROFILE_VK_OF_PRIORITY=high|realtime asks for that
// global priority for the queue.
void ProfileVulkanOpticalFlow(ID3D12Device* d3d12_device, double load_ms, double chunk_ms) {
    const LUID luid = d3d12_device->GetAdapterLuid();
    const HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Check(loader != nullptr, "Vulkan loader");
    const auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    Check(get_instance != nullptr, "vkGetInstanceProcAddr");
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_instance(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VectorXR optical flow profile";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    Check(create_instance(&instance_info, nullptr, &instance) == VK_SUCCESS, "Vulkan 1.3 instance");
#define VKI(name)                                                                                   \
    const auto vk##name = reinterpret_cast<PFN_vk##name>(get_instance(instance, "vk" #name)); \
    Check(vk##name != nullptr, "vk" #name);
    VKI(DestroyInstance)
    VKI(EnumeratePhysicalDevices)
    VKI(GetPhysicalDeviceProperties2)
    VKI(GetPhysicalDeviceQueueFamilyProperties)
    VKI(GetPhysicalDeviceMemoryProperties)
    VKI(CreateDevice)
    VKI(GetDeviceProcAddr)
    VKI(GetPhysicalDeviceOpticalFlowImageFormatsNV)
#undef VKI
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> physicals(count);
    vkEnumeratePhysicalDevices(instance, &count, physicals.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (const VkPhysicalDevice candidate : physicals) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(candidate, &properties);
        if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, &luid, sizeof(luid)) == 0) physical = candidate;
    }
    Check(physical != VK_NULL_HANDLE, "the Vulkan device of the Direct3D 12 adapter");
    VkPhysicalDeviceProperties2 physical_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(physical, &physical_properties);
    const double tick_ns = physical_properties.properties.limits.timestampPeriod;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    std::uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV)) ++family;
    Check(family < count, "an optical flow queue family");
    const std::string priority_name = std::getenv("FG_PROFILE_VK_OF_PRIORITY") ? std::getenv("FG_PROFILE_VK_OF_PRIORITY") : "";
    const float queue_priority = 1.0f;
    VkDeviceQueueGlobalPriorityCreateInfoKHR global{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    global.globalPriority = priority_name == "realtime" ? VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR : VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.pNext = priority_name.empty() ? nullptr : &global;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    VkPhysicalDeviceOpticalFlowFeaturesNV flow_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
    flow_features.opticalFlow = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &flow_features};
    features13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &features13};
    features12.timelineSemaphore = VK_TRUE;
    features12.hostQueryReset = VK_TRUE;
    const char* extensions[] = {VK_NV_OPTICAL_FLOW_EXTENSION_NAME, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
                                VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &features12};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = priority_name.empty() ? 2u : 3u;
    device_info.ppEnabledExtensionNames = extensions;
    VkDevice device = VK_NULL_HANDLE;
    const VkResult created = vkCreateDevice(physical, &device_info, nullptr, &device);
    Check(created == VK_SUCCESS, "Vulkan optical flow device (" + std::to_string(created) + ")");
#define VKD(name)                                                                                    \
    const auto vk##name = reinterpret_cast<PFN_vk##name>(vkGetDeviceProcAddr(device, "vk" #name)); \
    Check(vk##name != nullptr, "vk" #name);
    VKD(DestroyDevice)
    VKD(GetDeviceQueue)
    VKD(CreateImage)
    VKD(DestroyImage)
    VKD(GetImageMemoryRequirements)
    VKD(AllocateMemory)
    VKD(FreeMemory)
    VKD(BindImageMemory)
    VKD(CreateImageView)
    VKD(DestroyImageView)
    VKD(CreateBuffer)
    VKD(DestroyBuffer)
    VKD(GetBufferMemoryRequirements)
    VKD(BindBufferMemory)
    VKD(MapMemory)
    VKD(UnmapMemory)
    VKD(CreateCommandPool)
    VKD(DestroyCommandPool)
    VKD(AllocateCommandBuffers)
    VKD(BeginCommandBuffer)
    VKD(EndCommandBuffer)
    VKD(ResetCommandBuffer)
    VKD(CmdPipelineBarrier2)
    VKD(CmdCopyBufferToImage)
    VKD(QueueSubmit2)
    VKD(CreateSemaphore)
    VKD(DestroySemaphore)
    VKD(WaitSemaphores)
    VKD(DeviceWaitIdle)
    VKD(CreateOpticalFlowSessionNV)
    VKD(DestroyOpticalFlowSessionNV)
    VKD(BindOpticalFlowSessionImageNV)
    VKD(CmdOpticalFlowExecuteNV)
    VKD(CreateQueryPool)
    VKD(DestroyQueryPool)
    VKD(ResetQueryPool)
    VKD(CmdWriteTimestamp2)
    VKD(GetQueryPoolResults)
    VKD(GetCalibratedTimestampsKHR)
#undef VKD
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    auto first_format = [&](VkOpticalFlowUsageFlagsNV usage, VkFormat wanted) {
        VkOpticalFlowImageFormatInfoNV info{VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV, nullptr, usage};
        std::uint32_t n = 0;
        vkGetPhysicalDeviceOpticalFlowImageFormatsNV(physical, &info, &n, nullptr);
        std::vector<VkOpticalFlowImageFormatPropertiesNV> formats(n, {VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV});
        vkGetPhysicalDeviceOpticalFlowImageFormatsNV(physical, &info, &n, formats.data());
        for (const auto& format : formats) {
            if (format.format == wanted) return wanted;
        }
        return formats.empty() ? VK_FORMAT_UNDEFINED : formats.front().format;
    };
    const VkFormat input_format = first_format(VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, VK_FORMAT_R8_UNORM);
    const VkFormat flow_format = first_format(VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, VK_FORMAT_R16G16_SFIXED5_NV);
    Check(input_format == VK_FORMAT_R8_UNORM && flow_format == VK_FORMAT_R16G16_SFIXED5_NV,
          "optical flow formats R8 in, S10.5 vectors out (" + std::to_string(input_format) + ", " +
              std::to_string(flow_format) + ")");
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    auto memory_type = [&](std::uint32_t bits, VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        Check(false, "a memory type");
        return 0u;
    };

    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };
    auto make_image = [&](std::uint32_t width, std::uint32_t height, VkFormat format, VkOpticalFlowUsageFlagsNV flow_usage,
                          VkImageUsageFlags usage) {
        VkOpticalFlowImageFormatInfoNV flow{VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV, nullptr, flow_usage};
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &flow};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {width, height, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        Image image;
        Check(vkCreateImage(device, &info, nullptr, &image.image) == VK_SUCCESS, "optical flow image");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &image.memory) == VK_SUCCESS &&
                  vkBindImageMemory(device, image.image, image.memory, 0) == VK_SUCCESS,
              "optical flow image memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device, &view, nullptr, &image.view) == VK_SUCCESS, "optical flow image view");
        return image;
    };

    // The views' half-size luma, as FrameGenPyramid(...)[1] of the quad
    // views (2184x2200 and 2748x2764). FG_PROFILE_VK_OF_LAYOUT=same: four
    // matches on the first view's session; single: that one match only;
    // atlas: one match of the four views packed 2x2 (2466x2482).
    const std::string layout = std::getenv("FG_PROFILE_VK_OF_LAYOUT") ? std::getenv("FG_PROFILE_VK_OF_LAYOUT") : "views";
    std::vector<VkExtent2D> sizes{{1092, 1100}, {1092, 1100}, {1374, 1382}, {1374, 1382}};
    if (layout == "same" || layout == "single") sizes = {{1092, 1100}};
    if (layout == "atlas") sizes = {{2466, 2482}};
    const std::size_t executes = layout == "same" ? 4 : layout == "single" || layout == "atlas" ? 1 : 4;
    {
        VkPhysicalDeviceOpticalFlowPropertiesNV flow_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &flow_properties};
        vkGetPhysicalDeviceProperties2(physical, &properties);
        std::printf("Optical flow limits: %ux%u to %ux%u, output grids 0x%x, regions %u\n", flow_properties.minWidth,
                    flow_properties.minHeight, flow_properties.maxWidth, flow_properties.maxHeight,
                    flow_properties.supportedOutputGridSizes, flow_properties.maxNumRegionsOfInterest);
    }
    struct Session {
        VkOpticalFlowSessionNV session{VK_NULL_HANDLE};
        Image input, reference, flow;
    };
    std::vector<Session> sessions(sizes.size());
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS, "command pool");
    std::array<VkCommandBuffer, 3> commands{};
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = static_cast<std::uint32_t>(commands.size());
    Check(vkAllocateCommandBuffers(device, &allocate, commands.data()) == VK_SUCCESS, "command buffers");
    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline_type};
    VkSemaphore timeline = VK_NULL_HANDLE;
    Check(vkCreateSemaphore(device, &semaphore_info, nullptr, &timeline) == VK_SUCCESS, "timeline semaphore");
    // Two timestamps a frame (before and after its matches), per command
    // buffer.
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_info.queryCount = 15;
    VkQueryPool queries = VK_NULL_HANDLE;
    Check(vkCreateQueryPool(device, &query_info, nullptr, &queries) == VK_SUCCESS, "timestamp queries");
    // The GPU's clock against QueryPerformanceCounter (steady_clock's).
    auto calibrate = [&](std::uint64_t& gpu, std::uint64_t& cpu) {
        VkCalibratedTimestampInfoKHR domains[2]{{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
        domains[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
        domains[1].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
        std::uint64_t values[2]{};
        std::uint64_t deviation = 0;
        Check(vkGetCalibratedTimestampsKHR(device, 2, domains, values, &deviation) == VK_SUCCESS, "calibrated timestamps");
        gpu = values[0];
        cpu = values[1];
    };
    LARGE_INTEGER qpc_frequency{};
    QueryPerformanceFrequency(&qpc_frequency);
    auto gpu_time = [&](std::uint64_t stamp, std::uint64_t gpu, std::uint64_t cpu) {
        const double seconds = static_cast<double>(cpu) / static_cast<double>(qpc_frequency.QuadPart) +
                               (static_cast<double>(stamp) - static_cast<double>(gpu)) * tick_ns * 1e-9;
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)));
    };
    std::uint64_t submitted = 0;
    auto submit = [&](VkCommandBuffer command) {
        VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command_info.commandBuffer = command;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = timeline;
        signal.value = ++submitted;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &command_info;
        info.signalSemaphoreInfoCount = 1;
        info.pSignalSemaphoreInfos = &signal;
        Check(vkQueueSubmit2(queue, 1, &info, VK_NULL_HANDLE) == VK_SUCCESS, "optical flow submit");
        return submitted;
    };
    auto wait = [&](std::uint64_t value) {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = 1;
        info.pSemaphores = &timeline;
        info.pValues = &value;
        return vkWaitSemaphores(device, &info, 2'000'000'000ull) == VK_SUCCESS;
    };

    // Images: a textured input and the same texture moved a few texels as
    // the reference, uploaded through a staging buffer.
    VkDeviceSize staging_size = 0;
    for (const VkExtent2D size : sizes) staging_size += 2ull * size.width * size.height;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = staging_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateBuffer(device, &buffer_info, nullptr, &staging) == VK_SUCCESS, "staging buffer");
    VkMemoryRequirements staging_requirements{};
    vkGetBufferMemoryRequirements(device, staging, &staging_requirements);
    VkMemoryAllocateInfo staging_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    staging_allocate.allocationSize = staging_requirements.size;
    staging_allocate.memoryTypeIndex = memory_type(staging_requirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device, &staging_allocate, nullptr, &staging_memory) == VK_SUCCESS &&
              vkBindBufferMemory(device, staging, staging_memory, 0) == VK_SUCCESS,
          "staging memory");
    void* mapped = nullptr;
    Check(vkMapMemory(device, staging_memory, 0, staging_size, 0, &mapped) == VK_SUCCESS, "staging map");
    auto* bytes = static_cast<std::uint8_t*>(mapped);
    VkDeviceSize offset = 0;
    std::vector<VkDeviceSize> offsets;
    for (const VkExtent2D size : sizes) {
        for (int frame = 0; frame < 2; ++frame) {
            offsets.push_back(offset);
            for (std::uint32_t y = 0; y < size.height; ++y) {
                for (std::uint32_t x = 0; x < size.width; ++x) {
                    // Its green channel as the luma.
                    const std::uint32_t p = Background(static_cast<float>(x + 3 * frame), static_cast<float>(y + 2 * frame));
                    bytes[offset + std::size_t{y} * size.width + x] = static_cast<std::uint8_t>((p >> 8) & 0xffu);
                }
            }
            offset += std::size_t{size.width} * size.height;
        }
    }
    vkUnmapMemory(device, staging_memory);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(commands[0], &begin) == VK_SUCCESS, "upload begin");
    auto barrier = [&](VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
                       VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(command, &dependency);
    };
    const char* perf_name = std::getenv("FG_PROFILE_QUALITY");
    const VkOpticalFlowPerformanceLevelNV perf = perf_name && std::string(perf_name) == "performance"
                                                     ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV
                                                     : VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const VkExtent2D size = sizes[i];
        Session& s = sessions[i];
        s.input = make_image(size.width, size.height, input_format, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        s.reference = make_image(size.width, size.height, input_format, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        s.flow = make_image((size.width + 3) / 4, (size.height + 3) / 4, flow_format, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkOpticalFlowSessionCreateInfoNV info{VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV};
        info.width = size.width;
        info.height = size.height;
        info.imageFormat = input_format;
        info.flowVectorFormat = flow_format;
        info.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
        info.performanceLevel = perf;
        Check(vkCreateOpticalFlowSessionNV(device, &info, nullptr, &s.session) == VK_SUCCESS, "optical flow session");
        Check(vkBindOpticalFlowSessionImageNV(device, s.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, s.input.view,
                                              VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS &&
                  vkBindOpticalFlowSessionImageNV(device, s.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV,
                                                  s.reference.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS &&
                  vkBindOpticalFlowSessionImageNV(device, s.session, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV,
                                                  s.flow.view, VK_IMAGE_LAYOUT_GENERAL) == VK_SUCCESS,
              "optical flow bindings");
        for (int frame = 0; frame < 2; ++frame) {
            const VkImage image = frame ? s.reference.image : s.input.image;
            barrier(commands[0], image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[i * 2 + frame];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {size.width, size.height, 1};
            vkCmdCopyBufferToImage(commands[0], staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier(commands[0], image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV);
        }
        barrier(commands[0], s.flow.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV, VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV);
    }
    Check(vkEndCommandBuffer(commands[0]) == VK_SUCCESS, "upload end");
    Check(wait(submit(commands[0])), "upload");

    // One frame: the four matches, after the last frame's (their vectors
    // are written again).
    auto frame = [&](std::size_t index) {
        const VkCommandBuffer command = commands[index % commands.size()];
        Check(vkResetCommandBuffer(command, 0) == VK_SUCCESS && vkBeginCommandBuffer(command, &begin) == VK_SUCCESS,
              "frame begin");
        VkMemoryBarrier2 previous{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        previous.srcStageMask = previous.dstStageMask = VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV;
        previous.srcAccessMask = VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV;
        previous.dstAccessMask = VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV | VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &previous;
        vkCmdPipelineBarrier2(command, &dependency);
        const std::uint32_t query = static_cast<std::uint32_t>(index % commands.size()) * 5;
        vkResetQueryPool(device, queries, query, 5);
        vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, query);
        std::uint32_t stamp = query;
        for (std::size_t k = 0; k < 4; ++k) {
            if (k < executes) {
                VkOpticalFlowExecuteInfoNV execute{VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV};
                vkCmdOpticalFlowExecuteNV(command, sessions[k % sessions.size()].session, &execute);
            }
            vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, ++stamp);
        }
        Check(vkEndCommandBuffer(command) == VK_SUCCESS, "frame end");
        return submit(command);
    };
    auto run = [&](int frames) {
        std::vector<double> latency;
        std::vector<std::chrono::steady_clock::time_point> at(static_cast<std::size_t>(frames));
        std::vector<std::uint64_t> values(static_cast<std::size_t>(frames));
        std::atomic<int> submitted_frames{0};
        std::atomic<int> completed{0};
        // Completions, timed as they happen on a thread of their own.
        std::vector<std::chrono::steady_clock::time_point> done(static_cast<std::size_t>(frames));
        std::vector<std::chrono::steady_clock::time_point> gpu_start(static_cast<std::size_t>(frames)),
            gpu_end(static_cast<std::size_t>(frames));
        std::array<std::vector<double>, 4> each;
        std::uint64_t calibration_gpu = 0, calibration_cpu = 0;
        calibrate(calibration_gpu, calibration_cpu);
        std::thread waiter([&] {
            for (int i = 0; i < frames; ++i) {
                while (submitted_frames.load() <= i) std::this_thread::yield();
                Check(wait(values[static_cast<std::size_t>(i)]), "optical flow frame");
                done[static_cast<std::size_t>(i)] = std::chrono::steady_clock::now();
                std::uint64_t stamps[5]{};
                const std::uint32_t query = static_cast<std::uint32_t>(i % static_cast<int>(commands.size())) * 5;
                if (vkGetQueryPoolResults(device, queries, query, 5, sizeof(stamps), stamps, sizeof(std::uint64_t),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                    gpu_start[static_cast<std::size_t>(i)] = gpu_time(stamps[0], calibration_gpu, calibration_cpu);
                    gpu_end[static_cast<std::size_t>(i)] = gpu_time(stamps[4], calibration_gpu, calibration_cpu);
                    if (i >= 16) {
                        for (std::size_t k = 0; k < 4; ++k) {
                            each[k].push_back(static_cast<double>(stamps[k + 1] - stamps[k]) * tick_ns * 1e-6);
                        }
                    }
                }
                completed = i + 1;
            }
        });
        auto next = std::chrono::steady_clock::now();
        for (int i = 0; i < frames; ++i) {
            // A command buffer is recorded again once its frame is done.
            while (completed.load() < i - 2) std::this_thread::yield();
            values[static_cast<std::size_t>(i)] = frame(static_cast<std::size_t>(i));
            at[static_cast<std::size_t>(i)] = std::chrono::steady_clock::now();
            submitted_frames = i + 1;
            next += std::chrono::milliseconds(14);
            std::this_thread::sleep_until(next);
        }
        waiter.join();
        std::vector<double> started, ran, noticed;
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(a - b).count(); };
        for (int i = 16; i < frames; ++i) {
            const std::size_t f = static_cast<std::size_t>(i);
            latency.push_back(ms(done[f], at[f]));
            if (gpu_start[f] != std::chrono::steady_clock::time_point{}) {
                started.push_back(ms(gpu_start[f], at[f]));
                ran.push_back(ms(gpu_end[f], gpu_start[f]));
                noticed.push_back(ms(done[f], gpu_end[f]));
            }
        }
        std::printf("            on the GPU: started %s, ran %s, seen done %s ms after (median / p90)\n",
                    Percentiles(started).c_str(), Percentiles(ran).c_str(), Percentiles(noticed).c_str());
        std::printf("            each match:");
        for (const auto& times : each) std::printf("  %s", Percentiles(times).c_str());
        std::printf(" ms\n");
        return latency;
    };
    const std::unique_ptr<GpuLoad> load = MakeLoad(d3d12_device, load_ms, chunk_ms);
    const int frames = std::getenv("FG_PROFILE_D3D11_FRAMES") ? std::atoi(std::getenv("FG_PROFILE_D3D11_FRAMES")) : 200;
    std::printf("\nVulkan optical flow (queue family %u%s): %zu match(es) a frame, layout %s, %s\n", family,
                priority_name.empty() ? "" : (", global priority " + priority_name).c_str(), executes, layout.c_str(),
                perf == VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV ? "fast" : "medium");
    const std::vector<double> idle = run(frames);
    std::printf("  idle      done %s ms after submission (median / p90)\n", Percentiles(idle).c_str());
    const std::uint64_t first = load->Frames();
    const auto began = std::chrono::steady_clock::now();
    load->Start();
    const std::vector<double> loaded = run(frames);
    load->Stop();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("  loaded    done %s ms after submission (median / p90); the %s load ran %.1f frames/s\n",
                Percentiles(loaded).c_str(), load->Name(), (load->Frames() - first) / seconds);
    vkDeviceWaitIdle(device);
    for (Session& s : sessions) {
        vkDestroyOpticalFlowSessionNV(device, s.session, nullptr);
        for (Image* image : {&s.input, &s.reference, &s.flow}) {
            vkDestroyImageView(device, image->view, nullptr);
            vkDestroyImage(device, image->image, nullptr);
            vkFreeMemory(device, image->memory, nullptr);
        }
    }
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_memory, nullptr);
    vkDestroySemaphore(device, timeline, nullptr);
    vkDestroyQueryPool(device, queries, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
}

#ifdef DEPTHXR_NVOF_RESEARCH
void ProfileD3D12OpticalFlow(ID3D12Device* device, ID3D12CommandQueue* queue, double load_ms, double chunk_ms);
#endif

// The cockpit (CockpitAt) through a view of any field of view, rendered on
// every CPU thread.
Pixels CockpitView(const QuadView& view, float head_x, float scroll) {
    Pixels pixels(std::size_t{view.extent.width} * view.extent.height);
    const auto& t = view.tangents;
    auto rows = [&](std::uint32_t first, std::uint32_t step) {
        for (std::uint32_t y = first; y < view.extent.height; y += step) {
            const float ty = t[2] - (t[2] - t[3]) * (y + 0.5f) / view.extent.height;
            for (std::uint32_t x = 0; x < view.extent.width; ++x) {
                const float tx = t[0] + (t[1] - t[0]) * (x + 0.5f) / view.extent.width;
                const float px = head_x + kPanelDistance * tx;
                const float py = kPanelDistance * ty;
                pixels[std::size_t{y} * view.extent.width + x] = std::abs(px) < kPanelHalf && std::abs(py) < kPanelHalf
                                                                     ? Patch(px * 4000.0f + 100.0f, py * 4000.0f + 100.0f)
                                                                     : Background((tx + scroll) * 2400.0f, ty * 2400.0f);
            }
        }
    };
    const std::uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (std::uint32_t t_index = 0; t_index < threads; ++t_index) {
        workers.emplace_back(rows, t_index, threads);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return pixels;
}

// A profile's working memory, in bytes too (comparable across changes).
void PrintWorkingMemory(const FrameGenGpu& gpu) {
    const std::uint64_t bytes = gpu.WorkingMemoryBytes();
    std::printf("  working memory %.0f MB (%llu bytes)\n", static_cast<double>(bytes) / (1024.0 * 1024.0),
                static_cast<unsigned long long>(bytes));
}

// FG_PROFILE_QUAD=1: DCS's quad views on a Pimax Crystal Super, just in
// time: two peripheral views of 2184x2200 and two focus views of 2748x2764
// composed into two eyes of 6864x6908 (no depth: DCS sends none). Per real
// frame its analysis (four views, the flow merge) and the compose of its own
// refresh; per generated frame the four views' frames and the compose.
int RunQuadProfile(Harness& harness, const char* const* names, std::size_t name_count) {
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    Check(gpu != nullptr, harness.name + " backend: " + error);
    gpu->SetCopyAfterRelease(true); // the tests' images are their own
    Check(gpu->SupportsComposition(), harness.name + " composes quad views (just in time)");
    gpu->SetProfiling(true);
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    const FrameGenExtent periphery_extent{2184, 2200}, focus_extent{2748, 2764}, output{6864, 6908};
    const float outer = 2.0f, inner = 1.26f, vertical = 1.64f, half_ipd = 0.0336f;
    std::array<QuadView, 4> views{};
    views[0] = {{-outer, inner, vertical, -vertical}, periphery_extent};
    views[1] = {{-inner, outer, vertical, -vertical}, periphery_extent};
    views[2] = {{-0.55f, 0.5f, 0.52f, -0.48f}, focus_extent};
    views[3] = {{-0.5f, 0.55f, 0.52f, -0.48f}, focus_extent};
    FrameGenComposition composition = QuadComposition(views, output, 25.0, 50.0);
    constexpr int kFrames = 6;
    constexpr float kScroll = 0.004f;
    std::printf("Profiling quad views: 2x %ux%u + 2x %ux%u composed into 2x %ux%u, just in time; rendering test frames...\n",
                periphery_extent.width, periphery_extent.height, focus_extent.width, focus_extent.height, output.width,
                output.height);
    std::array<XrSwapchain, 4> sources{};
    for (std::size_t i = 0; i < 4; ++i) {
        sources[i] = reinterpret_cast<XrSwapchain>(0x61 + i);
        g_swapchains[sources[i]].clear();
        for (int frame = 0; frame < kFrames; ++frame) {
            void* image = harness.create(views[i].extent.width, views[i].extent.height);
            harness.upload_sized(image, views[i].extent.width, views[i].extent.height,
                                 CockpitView(views[i], i % 2 == 0 ? -half_ipd : half_ipd, kScroll * frame));
            g_swapchains[sources[i]].push_back(image);
        }
        Check(gpu->Track(sources[i], EnumerateImages, error), error);
    }
    const std::array<XrSwapchain, 2> eyes{reinterpret_cast<XrSwapchain>(0x70), reinterpret_cast<XrSwapchain>(0x71)};
    std::vector<FrameGenImage> eye_images;
    for (const XrSwapchain eye : eyes) {
        g_swapchains[eye] = {harness.create(output.width, output.height)};
        Check(gpu->Track(eye, EnumerateImages, error), error);
        eye_images.push_back({eye, 0, 0, {{0, 0}, {static_cast<int>(output.width), static_cast<int>(output.height)}},
                              output_format});
    }
    const bool interpolate = std::getenv("FG_PROFILE_MODE") && std::string(std::getenv("FG_PROFILE_MODE")) == "interpolate";
    auto submissions = [&](std::uint32_t frame, bool synthesize) {
        std::vector<FrameGenViewSubmission> result;
        for (std::size_t i = 0; i < 4; ++i) {
            FrameGenViewSubmission view;
            const XrRect2Di rect{{0, 0}, {static_cast<int>(views[i].extent.width), static_cast<int>(views[i].extent.height)}};
            view.source = {sources[i], frame, 0, rect, harness.format};
            view.request.extent = views[i].extent;
            view.request.output = FrameGenOutputKind::kSrgb8;
            view.request.synthesize = synthesize;
            view.request.temporal = true;
            view.request.newest = view.request.previous = QuadGeometry(views[i]);
            for (auto* geometry : {&view.request.newest, &view.request.previous}) {
                geometry->position = {i % 2 == 0 ? -half_ipd : half_ipd, 0.0f, 0.0f};
            }
            result.push_back(view);
        }
        return result;
    };
    // Captures, then the analysis, as the presenter runs a real frame.
    auto real_frame = [&](FrameGenGpu& backend, int i, const FrameGenTuning& tuning, FrameGenExecuteResult& result) {
        const auto views_now = submissions(static_cast<std::uint32_t>(i % kFrames), i > 0);
        for (std::size_t v = 0; v < 4; ++v) {
            const bool captured = backend.Capture(v, views_now[v].source, views_now[v].request, tuning.quality, error);
            Check(captured || error.empty(), "capture: " + error);
        }
        Sleep(2); // released a little before the frame ends
        FrameGenTuning analyze = tuning;
        analyze.analyze_only = true;
        analyze.cover = interpolate;
        analyze.composition = &composition;
        const bool analyzed = backend.Execute(views_now, analyze, result, error);
        Check(analyzed, "quad profile analyze: " + error);
    };
    if (const char* load_ms = std::getenv("FG_PROFILE_D3D11_LOAD")) {
        // DCS's case: analyses beside a game rendering on a Direct3D 11
        // device of its own, with the GPU busy without a break.
        const XrGraphicsBindingD3D12KHR* game = nullptr;
        for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
            if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) game = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
        }
        Check(game != nullptr, "the Direct3D 11 load needs a Direct3D 12 binding");
        gpu->Reset();
        // FG_PROFILE_D3D11_CHUNK_MS: each draw's GPU time (0.5).
        const double chunk_ms = std::getenv("FG_PROFILE_D3D11_CHUNK_MS") ? std::atof(std::getenv("FG_PROFILE_D3D11_CHUNK_MS")) : 0.5;
        if (std::getenv("FG_PROFILE_VK_OF") && std::string(std::getenv("FG_PROFILE_VK_OF")) == "1") {
            ProfileVulkanOpticalFlow(game->device, std::atof(load_ms), chunk_ms);
            return 0;
        }
#ifdef DEPTHXR_NVOF_RESEARCH
        if (std::getenv("FG_PROFILE_D3D12_OF") && std::string(std::getenv("FG_PROFILE_D3D12_OF")) == "1") {
            ProfileD3D12OpticalFlow(game->device, game->queue, std::atof(load_ms), chunk_ms);
            return 0;
        }
#endif
        const std::unique_ptr<GpuLoad> load = MakeLoad(game->device, std::atof(load_ms), chunk_ms);
        std::printf("\n%s load: %d chunks of %.2f ms a frame (%.1f ms), at most two frames queued; %s\n", load->Name(),
                    load->ChunksPerFrame(), load->ChunkMs(), load->ChunksPerFrame() * load->ChunkMs(),
                    interpolate ? "interpolate" : "extrapolate");
        // FG_PROFILE_D3D11_FLOWS=shader|hardware: one of them only;
        // FG_PROFILE_D3D11_FRAMES=N analyses each (200); FG_PROFILE_QUALITY=
        // performance|balanced|quality.
        const std::string flows = std::getenv("FG_PROFILE_D3D11_FLOWS") ? std::getenv("FG_PROFILE_D3D11_FLOWS") : "";
        const int load_frames = std::getenv("FG_PROFILE_D3D11_FRAMES") ? std::atoi(std::getenv("FG_PROFILE_D3D11_FRAMES")) : 200;
        const std::string quality = std::getenv("FG_PROFILE_QUALITY") ? std::getenv("FG_PROFILE_QUALITY") : "balanced";
        for (const bool hardware : {false, true}) {
            if (!flows.empty() && flows != (hardware ? "hardware" : "shader")) continue;
            for (const bool loaded : {false, true}) {
                FrameGenTuning tuning;
                tuning.mode = interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate;
                tuning.quality = quality == "performance" ? FrameGenFlowQuality::kPerformance
                                 : quality == "quality"   ? FrameGenFlowQuality::kQuality
                                                          : FrameGenFlowQuality::kBalanced;
                tuning.hardware_flow = hardware;
                auto backend = CreateFrameGenGpu(harness.binding, error);
                Check(backend != nullptr, "load profile backend: " + error);
                backend->SetCopyAfterRelease(true);
                for (const XrSwapchain source : sources) Check(backend->Track(source, EnumerateImages, error), error);
                for (const XrSwapchain eye : eyes) Check(backend->Track(eye, EnumerateImages, error), error);
                const auto began = std::chrono::steady_clock::now();
                const std::uint64_t first = load->Frames();
                if (loaded) load->Start();
                const AnalysisLatency latency = TimeAnalyses(*backend, load_frames, [&](FrameGenGpu& b, int i) {
                    FrameGenExecuteResult result;
                    real_frame(b, i, tuning, result);
                    return result.submission;
                });
                if (loaded) load->Stop();
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                const std::string label = std::string(hardware ? "hardware (" + backend->HardwareFlowStatus() + ")" : "shader") +
                                          (loaded ? ", loaded" : ", idle");
                PrintLatency(label.c_str(), latency);
                if (loaded) {
                    std::printf("    the load ran %.1f frames/s; its thread spent %.0f%% submitting, %.0f%% waiting for "
                                "the GPU\n",
                                (load->Frames() - first) / seconds, 100.0 * load->SubmitMs() / (seconds * 1000.0),
                                100.0 * load->WaitMs() / (seconds * 1000.0));
                }
                backend->Reset();
            }
        }
        return 0;
    }
    for (const int config : {0, 1, 2}) {
        FrameGenTuning tuning;
        tuning.quality = config == 1 ? FrameGenFlowQuality::kQuality : FrameGenFlowQuality::kBalanced;
        tuning.hardware_flow = config == 2;
        tuning.mode = interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate;
        gpu->ResetHistory();
        std::vector<double> analysis_sums, exact_sums, generated_sums;
        double analysis_total = 0.0, exact_total = 0.0, generated_total = 0.0;
        int analyses = 0, exacts = 0, generated = 0;
        auto add = [](std::vector<double>& sums, double& total, const std::vector<double>& times) {
            sums.resize(std::max(sums.size(), times.size()));
            for (std::size_t p = 0; p < times.size(); ++p) {
                sums[p] += times[p];
                total += times[p];
            }
        };
        for (int i = 0; i < 48; ++i) {
            FrameGenExecuteResult result;
            real_frame(*gpu, i, tuning, result);
            const std::vector<double> analysis = gpu->PassMilliseconds();
            // The real frame's own refresh (composed from its views as
            // captured), then the frames until the next real one.
            const bool composed =
                gpu->SynthesizeComposed(result.frame, tuning.mode, interpolate ? 1.0f : 0.0f, composition, eye_images, error);
            Check(composed, "quad profile compose: " + error);
            const std::vector<double> exact = gpu->PassMilliseconds();
            std::vector<std::vector<double>> frames;
            for (std::size_t k = 0; k < 3; ++k) {
                const float fraction = interpolate ? (k + 1) / 3.0f : 0.25f * static_cast<float>(k + 1);
                const bool written = gpu->SynthesizeComposed(result.frame, tuning.mode, fraction, composition, eye_images, error);
                Check(written, "quad profile synthesize: " + error);
                frames.push_back(gpu->PassMilliseconds());
            }
            if (i >= 24) {
                add(analysis_sums, analysis_total, analysis);
                ++analyses;
                add(exact_sums, exact_total, exact);
                ++exacts;
                for (const auto& times : frames) {
                    add(generated_sums, generated_total, times);
                    ++generated;
                }
            }
        }
        auto print = [&](const std::vector<double>& sums, int samples) {
            for (std::size_t p = 0; p < sums.size() && p < name_count; ++p) {
                if (sums[p] > 0.0) std::printf("    %-14s %6.3f ms\n", names[p], sums[p] / samples);
            }
        };
        std::printf("\n%s flow, 4 views: %.2f ms GPU per real frame (analysis %.2f, compose of its own refresh %.2f)\n",
                    config == 2 ? ("Hardware (" + gpu->HardwareFlowStatus() + ")").c_str()
                    : config == 1 ? "Quality" : "Balanced",
                    (analysis_total + exact_total) / std::max(analyses, 1), analysis_total / std::max(analyses, 1),
                    exact_total / std::max(exacts, 1));
        print(analysis_sums, analyses);
        std::printf("  own refresh:\n");
        print(exact_sums, exacts);
        std::printf("  per generated frame: %.3f ms\n", generated_total / std::max(generated, 1));
        print(generated_sums, generated);
        PrintWorkingMemory(*gpu);
    }
    gpu->Reset();
    if (const char* load = std::getenv("FG_PROFILE_LOAD_MS")) {
        FrameGenTuning tuning;
        tuning.mode = interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate;
        tuning.hardware_flow = std::getenv("FG_PROFILE_HARDWARE") && std::string(std::getenv("FG_PROFILE_HARDWARE")) == "1";
        ProfileUnderLoad(harness, std::atof(load), [&](FrameGenGpu& backend, int i) {
            if (i == 0) {
                for (const XrSwapchain source : sources) Check(backend.Track(source, EnumerateImages, error), error);
                for (const XrSwapchain eye : eyes) Check(backend.Track(eye, EnumerateImages, error), error);
            }
            FrameGenExecuteResult result;
            real_frame(backend, i, tuning, result);
            return result.submission;
        });
    }
    return 0;
}

int RunProfile(Harness& harness) {
    if (std::getenv("FG_PROFILE_QUAD") && std::string(std::getenv("FG_PROFILE_QUAD")) == "1") {
        return RunQuadProfile(harness, kProfileNames, std::size(kProfileNames));
    }
    std::string error;
    auto gpu = CreateFrameGenGpu(harness.binding, error);
    if (gpu) gpu->SetCopyAfterRelease(true); // the tests' images are their own
    Check(gpu != nullptr, harness.name + " backend: " + error);
    gpu->SetProfiling(true);
    const std::int64_t output_format = gpu->OutputFormats(FrameGenOutputKind::kSrgb8).front();
    const std::uint32_t width = g_profile_width, height = g_profile_height;
    const std::uint32_t depth_width = width * 4 / 5, depth_height = height * 4 / 5; // 80% render scale
    // A Pimax Crystal Super's views: each eye sees further outward.
    const float outer = 2.0f, inner = 1.26f, vertical = 1.64f, half_ipd = 0.0336f;
    constexpr int kFrames = 6;
    constexpr float kScroll = 0.004f; // per frame, ~8 px at this size
    // FG_PROFILE_JIT=1: just-in-time synthesis (each real frame measured
    // once, then three frames written one at a time); FG_PROFILE_MODE=
    // interpolate measures for interpolation, with cover flow, and writes
    // two frames between and the real frame's copy.
    const bool jit = std::getenv("FG_PROFILE_JIT") && std::string(std::getenv("FG_PROFILE_JIT")) == "1";
    const bool interpolate = std::getenv("FG_PROFILE_MODE") && std::string(std::getenv("FG_PROFILE_MODE")) == "interpolate";
    std::printf("Profiling %ux%u per eye, depth %ux%u, %s %s; rendering test frames...\n", width, height, depth_width,
                depth_height, interpolate ? "interpolate" : "extrapolate", jit ? "just in time" : "4x");
    std::array<XrSwapchain, 2> sources{reinterpret_cast<XrSwapchain>(0x61), reinterpret_cast<XrSwapchain>(0x62)};
    std::array<XrSwapchain, 2> depths{reinterpret_cast<XrSwapchain>(0x63), reinterpret_cast<XrSwapchain>(0x64)};
    std::array<std::array<XrSwapchain, 4>, 2> outputs{};
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
        for (std::size_t set = 0; set < 4; ++set) {
            outputs[eye][set] = reinterpret_cast<XrSwapchain>(0x70 + eye * 4 + set);
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
    const char* const* names = kProfileNames;
    // A real frame's two views, captured at release as the presenter does:
    // color, then depth.
    auto capture_frame = [&](FrameGenGpu& backend, std::uint32_t frame, FrameGenFlowQuality quality) {
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
            // The head moving 2 cm per real frame (a fast turn; FG_PROFILE_EYE_MM
            // sets it): generated frames follow the eye.
            const char* eye_mm = std::getenv("FG_PROFILE_EYE_MM");
            view.request.shift = {eye_mm ? static_cast<float>(std::atof(eye_mm)) / 1000.0f : 0.02f, 0.0f, 0.0f};
            for (auto* geometry : {&view.request.newest, &view.request.previous}) {
                geometry->angle_left = std::atan(eye == 0 ? -outer : -inner);
                geometry->angle_right = std::atan(eye == 0 ? inner : outer);
                geometry->angle_up = std::atan(vertical);
                geometry->angle_down = -std::atan(vertical);
                geometry->position = {head, 0, 0};
            }
            views.push_back(view);
        }
        for (std::size_t eye = 0; eye < 2; ++eye) {
            const bool captured = backend.Capture(eye, views[eye].source, views[eye].request, quality, error);
            Check(captured || error.empty(), "capture: " + error);
            const XrRect2Di depth_rect{{0, 0}, {static_cast<int>(depth_width), static_cast<int>(depth_height)}};
            const bool depth_captured =
                backend.CaptureDepth(eye, {depths[eye], 0, 0, depth_rect, 0}, encoding, views[eye].request, error);
            Check(depth_captured || error.empty(), "depth capture: " + error);
        }
        return views;
    };
    for (const int config : {0, 1, 2}) {
        const auto quality = config == 1 ? FrameGenFlowQuality::kQuality : FrameGenFlowQuality::kBalanced;
        FrameGenTuning tuning;
        tuning.quality = quality;
        tuning.hardware_flow = config == 2;
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        // 4x: three generated slots and the spare for a late real frame.
        tuning.generated = 4;
        tuning.fractions = {0.25f, 0.5f, 0.75f, 1.0f};
        if (interpolate) {
            tuning.mode = FrameGenSynthesisMode::kInterpolate;
            tuning.generated = 3;
            tuning.fractions = {1.0f / 3.0f, 2.0f / 3.0f, 1.0f, 0.0f};
        }
        gpu->ResetHistory();
        std::vector<double> sums;
        std::vector<double> synthesis_sums;
        double total = 0.0;
        double synthesis_total = 0.0;
        int samples = 0;
        int synthesis_samples = 0;
        bool depth_used = false;
        for (int i = 0; i < 48; ++i) {
            const std::uint32_t frame = static_cast<std::uint32_t>(i % kFrames);
            const std::vector<FrameGenViewSubmission> views = capture_frame(*gpu, frame, quality);
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
            FrameGenTuning executed = tuning;
            executed.analyze_only = jit;
            executed.cover = jit && interpolate;
            Check(gpu->Execute(views, executed, result, error), "profile execute: " + error);
            const std::vector<double> times = gpu->PassMilliseconds();
            if (jit) {
                // The frames until the next real one, each written on its own.
                for (std::size_t k = 0; k < 3; ++k) {
                    const float fraction = interpolate ? tuning.fractions[k] : 0.25f * static_cast<float>(k + 1);
                    std::vector<FrameGenImage> images;
                    for (std::size_t eye = 0; eye < 2; ++eye) {
                        images.push_back(views[eye].outputs[k]);
                    }
                    Check(gpu->Synthesize(result.frame, tuning.mode, fraction, images, error), "profile synthesize: " + error);
                    const std::vector<double> synthesis = gpu->PassMilliseconds();
                    if (i >= 24 && !synthesis.empty()) {
                        synthesis_sums.resize(synthesis.size());
                        for (std::size_t q = 0; q < synthesis.size(); ++q) {
                            synthesis_sums[q] += synthesis[q];
                            synthesis_total += synthesis[q];
                        }
                        ++synthesis_samples;
                    }
                }
            }
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
        std::printf("\n%s flow, 2 eyes, 4 generated frames: %.2f ms GPU per real frame (depth %s, %s)\n",
                    config == 2 ? ("Hardware (" + gpu->HardwareFlowStatus() + ")").c_str()
                    : quality == FrameGenFlowQuality::kQuality ? "Quality" : "Balanced",
                    samples ? total / samples : -1.0, depth_used ? "used" : "not used", ToString(depth.reading));
        for (std::size_t p = 0; p < sums.size() && p < std::size(kProfileNames); ++p) {
            if (sums[p] > 0.0) {
                std::printf("  %-14s %6.3f ms\n", names[p], sums[p] / samples);
            }
        }
        if (synthesis_samples) {
            std::printf("  just in time, per frame written: %.3f ms (%.2f ms for the three per real frame)\n",
                        synthesis_total / synthesis_samples, 3.0 * synthesis_total / synthesis_samples);
            for (std::size_t p = 0; p < synthesis_sums.size() && p < std::size(kProfileNames); ++p) {
                if (synthesis_sums[p] > 0.0) {
                    std::printf("    %-14s %6.3f ms\n", names[p], synthesis_sums[p] / synthesis_samples);
                }
            }
        }
        PrintWorkingMemory(*gpu);
    }
    gpu->Reset();
    if (const char* load = std::getenv("FG_PROFILE_LOAD_MS")) {
        // Each real frame measured just in time, behind the game's work.
        FrameGenTuning tuning;
        tuning.mode = interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate;
        tuning.hardware_flow = std::getenv("FG_PROFILE_HARDWARE") && std::string(std::getenv("FG_PROFILE_HARDWARE")) == "1";
        tuning.analyze_only = true;
        tuning.cover = interpolate;
        ProfileUnderLoad(harness, std::atof(load), [&](FrameGenGpu& backend, int i) {
            if (i == 0) {
                for (std::size_t eye = 0; eye < 2; ++eye) {
                    Check(backend.Track(sources[eye], EnumerateImages, error) && backend.Track(depths[eye], EnumerateImages, error),
                          error);
                    for (const XrSwapchain output : outputs[eye]) {
                        Check(backend.Track(output, EnumerateImages, error), error);
                    }
                }
            }
            const std::vector<FrameGenViewSubmission> views =
                capture_frame(backend, static_cast<std::uint32_t>(i % kFrames), tuning.quality);
            FrameGenExecuteResult result;
            const bool analyzed = backend.Execute(views, tuning, result, error);
            Check(analyzed, "profile execute: " + error);
            return result.submission;
        });
    }
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
// instead of exactly to the next frame's position), FG_REPLAY_JIT=own|stale|
// stale-noshift (extrapolating just in time from frames k - 2..k analyzed
// alone: frame k moved by its own motion, or by frame k - 1's,
// FrameGenGpu::SynthesizeStale, with or without the eye shift).
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
                                " fractions",
                          &mode, &quality, &hardware, &frame.tuning.artifact_suppression,
                          &frame.tuning.motion_limit_percent, &frame.tuning.interval_ratio, &generated) == 7;
    // Three fractions in older captures, kFrameGenMaxGenerated now.
    for (std::size_t i = 0; ok && i < kFrameGenMaxGenerated; ++i) {
        if (std::fscanf(file, " %f", &frame.tuning.fractions[i]) != 1) {
            break;
        }
    }
    ok = ok && std::fscanf(file, " depth_reading %d views %zu", &reading, &count) == 2;
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
// (nearer than 4 m: cockpit), and the share of pixels off by more than a
// tenth (visible artifacts: torn or misplaced edges rather than blur).
struct ReplayError {
    double all{0}, cockpit{0}, outside{0};
    double bad_cockpit{0}, bad_outside{0}; // percent
    // Cockpit pixels on an edge of the truth (luma gradient over a tenth):
    // mean error there, and gradient energy of the image over the truth's
    // (below 1: softer edges; a generated frame alternating with sharp real
    // frames then shimmers).
    double edge_cockpit{0};
    double sharpness_cockpit{0};
    // ... and over the newest real frame's own gradients there, unresampled:
    // what alternates with generated frames in the headset (1: no shimmer).
    double sharpness_newest{0};
};
double PixelLuma(std::uint32_t p) {
    return (0.2126 * (p & 255u) + 0.7152 * ((p >> 8) & 255u) + 0.0722 * ((p >> 16) & 255u)) / 255.0;
}
double Gradient(const Pixels& image, std::size_t index, std::uint32_t width) {
    return std::abs(PixelLuma(image[index + 1]) - PixelLuma(image[index - 1])) +
           std::abs(PixelLuma(image[index + width]) - PixelLuma(image[index - width]));
}
ReplayError CompareTo(const Pixels& image, const Pixels& truth, const std::vector<bool>& valid, const ReplayView& view) {
    double sums[3] = {}, counts[3] = {}, bad[3] = {};
    double edge_sum = 0, edge_count = 0, image_energy = 0, truth_energy = 0, newest_energy = 0;
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
            bad[part] += error > 0.1 ? 1.0 : 0.0;
            if (part == 1 && x > 0 && y > 0 && x + 1 < view.width && y + 1 < view.height && valid[index - 1] &&
                valid[index + 1] && valid[index - view.width] && valid[index + view.width]) {
                const double truth_gradient = Gradient(truth, index, view.width);
                truth_energy += truth_gradient;
                image_energy += Gradient(image, index, view.width);
                newest_energy += Gradient(view.color, index, view.width);
                if (truth_gradient > 0.1) {
                    edge_sum += error;
                    edge_count += 1;
                }
            }
        }
    }
    return {counts[0] ? sums[0] / counts[0] : 0, counts[1] ? sums[1] / counts[1] : 0, counts[2] ? sums[2] / counts[2] : 0,
            counts[1] ? 100.0 * bad[1] / counts[1] : 0, counts[2] ? 100.0 * bad[2] / counts[2] : 0,
            edge_count ? edge_sum / edge_count : 0, truth_energy > 0 ? image_energy / truth_energy : 0,
            newest_energy > 0 ? image_energy / newest_energy : 0};
}

// Research: a full-resolution crop (FG_REPLAY_CROP=x0,y0,x1,y1), scaled up
// `zoom` times with nearest sampling so single pixels stay visible.
void WriteCropPpm(const std::string& path, const Pixels& pixels, std::uint32_t width, std::uint32_t height,
                  const std::array<int, 4>& box, int zoom) {
    const int x0 = std::clamp(box[0], 0, static_cast<int>(width)), y0 = std::clamp(box[1], 0, static_cast<int>(height));
    const int x1 = std::clamp(box[2], x0 + 1, static_cast<int>(width)), y1 = std::clamp(box[3], y0 + 1, static_cast<int>(height));
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const int w = (x1 - x0) * zoom, h = (y1 - y0) * zoom;
    std::fprintf(file, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> row(std::size_t(w) * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint32_t p = pixels[std::size_t(y0 + y / zoom) * width + std::size_t(x0 + x / zoom)];
            for (int c = 0; c < 3; ++c) row[std::size_t(x) * 3 + c] = static_cast<unsigned char>((p >> (8 * c)) & 255u);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

// Research dump: a float image as raw little-endian floats, the size and
// channel count in the name.
void WriteFloats(const std::string& prefix, const std::vector<float>& values, FrameGenExtent extent, std::uint32_t channels) {
    std::FILE* file = std::fopen((prefix + "-" + std::to_string(extent.width) + "x" + std::to_string(extent.height) + "x" +
                                  std::to_string(channels) + ".f32")
                                     .c_str(),
                                 "wb");
    if (file) {
        std::fwrite(values.data(), sizeof(float), values.size(), file);
        std::fclose(file);
    }
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
    if (gpu) gpu->SetCopyAfterRelease(true); // the tests' images are their own
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

    const bool interpolate = env("FG_REPLAY_MODE") == "interpolate";
    const bool dump_maps = env("FG_REPLAY_DUMP") == "1";
    const std::string jit_variant = env("FG_REPLAY_JIT");
    std::optional<std::array<int, 4>> crop;
    if (std::array<int, 4> box{}; std::sscanf(env("FG_REPLAY_CROP").c_str(), "%d,%d,%d,%d", &box[0], &box[1], &box[2], &box[3]) == 4) {
        crop = box;
    }

    // Executes frame k as the newest real frame after frame `before` (none:
    // a fresh sequence, nothing generated), generating one frame at
    // `fraction` seen from the eye moved by shift * time (FrameGenViewRequest).
    auto execute = [&](std::size_t k, std::optional<std::size_t> before, FrameGenSynthesisMode mode, float fraction,
                       const std::vector<std::array<float, 3>>& shifts, FrameGenExecuteResult& result,
                       bool analyze_only = false) {
        const ReplayFrame& frame = frames[k];
        FrameGenTuning tuning = frame.tuning;
        tuning.mode = mode;
        tuning.analyze_only = analyze_only;
        tuning.cover = false;
        tuning.hardware_flow = hardware;
        if (quality == "performance") tuning.quality = FrameGenFlowQuality::kPerformance;
        if (quality == "balanced") tuning.quality = FrameGenFlowQuality::kBalanced;
        if (quality == "quality") tuning.quality = FrameGenFlowQuality::kQuality;
        tuning.generated = 1;
        tuning.spare = false;
        tuning.fractions[0] = fraction;
        if (before && *before + 1 != k) tuning.interval_ratio = 1.0f;
        std::vector<FrameGenViewSubmission> views(view_count);
        for (std::size_t v = 0; v < view_count; ++v) {
            const ReplayView& view = frame.views[v];
            FrameGenViewSubmission& submission = views[v];
            const XrRect2Di rect{{0, 0}, {static_cast<int>(view.width), static_cast<int>(view.height)}};
            submission.source = {sources[v], static_cast<std::uint32_t>(k), 0, rect, harness.format};
            submission.outputs = {{outputs[v], 0, 0, rect, output_format}};
            submission.request.extent = {view.width, view.height};
            submission.request.output = FrameGenOutputKind::kSrgb8;
            submission.request.synthesize = before.has_value();
            submission.request.temporal = true;
            submission.request.depth = !no_depth;
            submission.request.newest = view.newest;
            submission.request.previous = before ? frames[*before].views[v].newest : view.newest;
            if (v < shifts.size()) submission.request.shift = shifts[v];
            Check(gpu->Capture(v, submission.source, submission.request, tuning.quality, error) || error.empty(),
                  "replay capture: " + error);
            const XrRect2Di depth_rect{{0, 0}, {static_cast<int>(view.half_width), static_cast<int>(view.half_height)}};
            Check(gpu->CaptureDepth(v, {depths[v], static_cast<std::uint32_t>(k), 0, depth_rect, 0}, encoding,
                                    submission.request, error) ||
                      error.empty(),
                  "replay depth capture: " + error);
        }
        Check(gpu->Execute(views, tuning, result, error), "replay execute: " + error);
    };
    // Where the eye was at frame `to`, from frame `from`'s eye, in frame
    // `from`'s view space, divided by `time`.
    auto eye_shifts = [&](std::size_t from, std::size_t to, float time) {
        std::vector<std::array<float, 3>> shifts(view_count, {0.0f, 0.0f, 0.0f});
        for (std::size_t v = 0; v < view_count && !no_shift && time > 0.0f; ++v) {
            const ReplayView& view = frames[from].views[v];
            const auto rotation = ReplayRotation(view.newest.orientation);
            const auto& target = frames[to].views[v].newest.position;
            const double delta[3] = {target[0] - view.newest.position[0], target[1] - view.newest.position[1],
                                     target[2] - view.newest.position[2]};
            for (int i = 0; i < 3; ++i) {
                const double local = rotation[0][i] * delta[0] + rotation[1][i] * delta[1] + rotation[2][i] * delta[2];
                shifts[v][i] = static_cast<float>(local / time);
            }
        }
        return shifts;
    };
    // Compares each view's generated frame (in frame `view_frame`'s view)
    // with frame `truth_frame` turned to that view; `hold` (view 0) is what
    // showing a real frame instead would give.
    double sums[7] = {};
    std::size_t evaluated = 0;
    auto evaluate = [&](std::size_t k, std::size_t view_frame, std::size_t truth_frame, const Pixels& hold,
                        const FrameGenExecuteResult& result, float fraction) {
        for (std::size_t v = 0; v < view_count; ++v) {
            const ReplayView& view = frames[view_frame].views[v];
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
            const std::string prefix = g_replay_output + "/frame-" + std::to_string(k) + "-view" + std::to_string(v);
            if (!g_replay_output.empty() && v == 0 && !xy.empty()) {
                WriteFloats(prefix + "-flow", xy, cells, 2);
            }
            if (!g_replay_output.empty() && v == 0 && dump_maps) {
                const std::pair<const char*, FrameGenTexture> maps[] = {
                    {"depth", {FrameGenTextureKind::kDepth, 0}},
                    {"trust", {FrameGenTextureKind::kTrustNewest, 0}},
                    {"shift0", {FrameGenTextureKind::kShiftMap, 0}},
                    {"keys", {FrameGenTextureKind::kShiftKeys, 0}},
                    {"filtered", {FrameGenTextureKind::kFilteredNewest, 0}},
                    {"world", {FrameGenTextureKind::kWorldParams, 0}},
                    {"scalevotes", {FrameGenTextureKind::kDepthScaleVotes, 0}},
                    {"scales", {FrameGenTextureKind::kDepthScales, 0}},
                };
                for (const auto& [name, texture] : maps) {
                    std::vector<float> values;
                    FrameGenExtent extent{};
                    std::uint32_t channels = 0;
                    if (gpu->ReadTexture(v, texture, values, extent, channels)) {
                        WriteFloats(prefix + "-" + name, values, extent, channels);
                    }
                }
            }
            if (cockpit_cells > 0.0) {
                std::printf("frame %zu view %zu cockpit motion: mean %.3f px, %.1f%% of cells above 0.5 px\n", k, v,
                            cockpit_motion / cockpit_cells, 100.0 * cockpit_moving / cockpit_cells);
            }
            const Pixels generated = harness.readback_sized(g_swapchains[outputs[v]][0]);
            std::vector<bool> valid;
            const Pixels truth = TurnTo(view, frames[truth_frame].views[v], valid);
            const ReplayError generated_error = CompareTo(generated, truth, valid, view);
            const ReplayError hold_error = CompareTo(v == 0 ? hold : frames[view_frame].views[v].color, truth, valid, view);
            std::printf("frame %zu view %zu (depth %s, %.2f): generated %.4f (cockpit %.4f %.2f%% bad, outside %.4f %.2f%% bad,"
                        " cockpit edges %.4f, sharpness %.3f, vs newest %.3f)  hold %.4f (cockpit %.4f %.2f%%, outside %.4f %.2f%%)\n",
                        k, v, result.depth.size() > v && result.depth[v] ? "used" : "not used", fraction,
                        generated_error.all, generated_error.cockpit, generated_error.bad_cockpit, generated_error.outside,
                        generated_error.bad_outside, generated_error.edge_cockpit, generated_error.sharpness_cockpit,
                        generated_error.sharpness_newest, hold_error.all, hold_error.cockpit, hold_error.bad_cockpit, hold_error.outside, hold_error.bad_outside);
            sums[0] += generated_error.cockpit;
            sums[1] += generated_error.bad_cockpit;
            sums[2] += generated_error.outside;
            sums[3] += generated_error.bad_outside;
            sums[4] += generated_error.edge_cockpit;
            sums[5] += generated_error.sharpness_cockpit;
            sums[6] += generated_error.sharpness_newest;
            ++evaluated;
            if (!g_replay_output.empty() && v == 0 && crop) {
                WriteCropPpm(prefix + "-crop-generated.ppm", generated, view.width, view.height, *crop, 3);
                WriteCropPpm(prefix + "-crop-truth.ppm", truth, view.width, view.height, *crop, 3);
                WriteCropPpm(prefix + "-crop-newest.ppm", frames[view_frame].views[v].color, view.width, view.height, *crop, 3);
            }
            if (!g_replay_output.empty() && v == 0) {
                WriteHalfPpm(prefix + "-generated.ppm", generated, view.width, view.height);
                WriteHalfPpm(prefix + "-truth.ppm", truth, view.width, view.height);
                WriteHalfPpm(prefix + "-newest.ppm", frames[view_frame].views[v].color, view.width, view.height);
            }
        }
    };
    // Three times through to settle the depth check, then once to measure.
    for (int pass = 0; pass < 4; ++pass) {
        gpu->ResetHistory();
        const bool measure = pass == 3;
        for (std::size_t k = 0; k < frames.size(); ++k) {
            FrameGenExecuteResult result;
            const bool has_next = k + 1 < frames.size();
            if (!jit_variant.empty() && measure && !interpolate) {
                // Just in time: frames k - 2, k - 1 and k analyzed alone, then
                // frame k moved to frame k + 1 by its own motion or by frame
                // k - 1's (seen from the eye moved as frame k's own, by frame
                // k - 1's depth; FG_REPLAY_JIT=stale-noshift: unmoved, as
                // before 2026-10-10).
                if (k < 2 || !has_next) continue;
                const float fraction = std::clamp(frames[k + 1].tuning.interval_ratio, 0.25f, 1.0f);
                gpu->ResetHistory();
                std::uint64_t ids[3] = {};
                for (std::size_t j = 0; j < 3; ++j) {
                    const std::size_t f = k - 2 + j;
                    const auto shifts = f == k && jit_variant != "stale-noshift" ? eye_shifts(k, k + 1, fraction)
                                                                                : std::vector<std::array<float, 3>>{};
                    execute(f, j > 0 ? std::optional<std::size_t>(f - 1) : std::nullopt, FrameGenSynthesisMode::kExtrapolate,
                            fraction, shifts, result, true);
                    ids[j] = result.frame;
                }
                Check(gpu->WaitIdle(), "replay: the analyses did not finish");
                std::vector<FrameGenImage> images;
                for (std::size_t v = 0; v < view_count; ++v) {
                    const ReplayView& view = frames[k].views[v];
                    images.push_back({outputs[v], 0, 0,
                                      {{0, 0}, {static_cast<int>(view.width), static_cast<int>(view.height)}},
                                      output_format});
                }
                const bool written =
                    jit_variant.rfind("stale", 0) == 0
                        ? gpu->SynthesizeStale(ids[2], ids[1], fraction, images, error)
                        : gpu->Synthesize(ids[2], FrameGenSynthesisMode::kExtrapolate, fraction, images, error);
                Check(written, "replay just in time (" + jit_variant + "): " + error);
                Check(gpu->WaitIdle(), "replay: the frame was not written");
                result.synthesized.assign(view_count, true);
                evaluate(k, k, k + 1, frames[k].views[0].color, result, fraction);
                continue;
            }
            if (interpolate && measure) {
                // Frame k from the frames either side of it: k - 1, then k + 1
                // as the next real frame, at k's share of the two intervals.
                if (k == 0 || !has_next) continue;
                const float ratio = std::clamp(frames[k + 1].tuning.interval_ratio, 0.25f, 4.0f);
                const float fraction = 1.0f / (1.0f + ratio);
                gpu->ResetHistory();
                execute(k - 1, std::nullopt, FrameGenSynthesisMode::kInterpolate, fraction, {}, result);
                execute(k + 1, k - 1, FrameGenSynthesisMode::kInterpolate, fraction,
                        eye_shifts(k + 1, k, 1.0f - fraction), result);
                evaluate(k, k + 1, k, frames[k + 1].views[0].color, result, fraction);
                continue;
            }
            // Frame k extrapolated to frame k + 1: one frame at the time ratio
            // of the two intervals, from the eye position frame k + 1 was
            // rendered at (predicting: on as it moved from frame k - 1).
            const float fraction = has_next ? std::clamp(frames[k + 1].tuning.interval_ratio, 0.25f, 1.0f) : 1.0f;
            std::vector<std::array<float, 3>> shifts;
            if (has_next && k > 0) {
                if (predict) {
                    shifts = eye_shifts(k - 1, k, 1.0f);
                    for (std::size_t v = 0; v < view_count; ++v) {
                        // From frame k - 1's view space into frame k's; the eye
                        // keeps moving as it did, for the intervals' ratio.
                        const auto a = ReplayRotation(frames[k - 1].views[v].newest.orientation);
                        const auto b = ReplayRotation(frames[k].views[v].newest.orientation);
                        double world[3], local[3];
                        for (int i = 0; i < 3; ++i) world[i] = a[i][0] * shifts[v][0] + a[i][1] * shifts[v][1] + a[i][2] * shifts[v][2];
                        for (int i = 0; i < 3; ++i) local[i] = b[0][i] * world[0] + b[1][i] * world[1] + b[2][i] * world[2];
                        for (int i = 0; i < 3; ++i) {
                            shifts[v][i] = static_cast<float>(local[i] * frames[k + 1].tuning.interval_ratio / fraction);
                        }
                    }
                } else {
                    shifts = eye_shifts(k, k + 1, fraction);
                }
            }
            execute(k, k > 0 ? std::optional<std::size_t>(k - 1) : std::nullopt, FrameGenSynthesisMode::kExtrapolate,
                    fraction, shifts, result);
            if (measure && k > 0 && has_next && !interpolate) {
                evaluate(k, k, k + 1, frames[k].views[0].color, result, fraction);
            }
        }
        if (pass == 2) {
            std::printf("Depth reading: %s, scale %.3f\n", ToString(gpu->DepthStatus().reading),
                        static_cast<double>(gpu->DepthStatus().scale));
        }
    }
    if (evaluated) {
        std::printf("Mean over %zu views: cockpit %.4f (%.2f%% bad), outside %.4f (%.2f%% bad), cockpit edges %.4f,"
                    " sharpness %.3f, vs newest %.3f\n",
                    evaluated, sums[0] / evaluated, sums[1] / evaluated, sums[2] / evaluated, sums[3] / evaluated,
                    sums[4] / evaluated, sums[5] / evaluated, sums[6] / evaluated);
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
    // Flow from input `input` to input `reference` without waiting; the
    // fence value it signals, and whether it has.
    void Submit(int input, int reference, bool temporal) { Execute(input, reference, temporal); }
    UINT64 Value() const { return fence_value_; }
    bool Done(UINT64 value) const { return fence_->GetCompletedValue() >= value; }

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

// FG_PROFILE_D3D12_OF=1 (with FG_PROFILE_D3D11_LOAD and FG_PROFILE_LOAD_API):
// NVIDIA's optical flow through its Direct3D 12 interface, as the backend
// calls it, alone: FG_PROFILE_VK_OF_LAYOUT's layouts (views: the four quad
// views' sessions; single: the first only; atlas: one session for the four
// packed 2x2), executed every 14 ms with their inputs ready, each execute's
// completion seen on the CPU, idle and beside the load.
void ProfileD3D12OpticalFlow(ID3D12Device* device, ID3D12CommandQueue* queue, double load_ms, double chunk_ms) {
    const std::string layout = std::getenv("FG_PROFILE_VK_OF_LAYOUT") ? std::getenv("FG_PROFILE_VK_OF_LAYOUT") : "views";
    std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes{{1092, 1100}, {1092, 1100}, {1374, 1382}, {1374, 1382}};
    if (layout == "single") sizes = {{1092, 1100}};
    if (layout == "atlas") sizes = {{2466, 2482}};
    const char* perf_name = std::getenv("FG_PROFILE_QUALITY");
    const NV_OF_PERF_LEVEL perf =
        perf_name && std::string(perf_name) == "performance" ? NV_OF_PERF_LEVEL_FAST : NV_OF_PERF_LEVEL_MEDIUM;
    std::vector<std::unique_ptr<NvofSession>> sessions;
    for (const auto& [width, height] : sizes) {
        auto session = std::make_unique<NvofSession>();
        std::string error;
        Check(session->Create(device, queue, width, height, 2, NV_OF_OUTPUT_VECTOR_GRID_SIZE_4, perf, error), error);
        for (int frame = 0; frame < 2; ++frame) {
            std::vector<std::uint8_t> gray(std::size_t{width} * height);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const std::uint32_t pixel = Background(static_cast<float>(x + 3 * frame), static_cast<float>(y + 2 * frame));
                    gray[std::size_t{y} * width + x] = static_cast<std::uint8_t>((pixel >> 8) & 0xffu);
                }
            }
            session->Upload(frame, gray);
        }
        sessions.push_back(std::move(session));
    }
    auto run = [&](int frames) {
        using Clock = std::chrono::steady_clock;
        std::vector<Clock::time_point> at(static_cast<std::size_t>(frames));
        std::vector<std::vector<UINT64>> values(static_cast<std::size_t>(frames));
        std::vector<std::vector<Clock::time_point>> done(static_cast<std::size_t>(frames));
        std::atomic<int> submitted{0};
        std::atomic<int> completed{0};
        std::thread waiter([&] {
            for (int i = 0; i < frames; ++i) {
                while (submitted.load() <= i) std::this_thread::yield();
                const std::size_t f = static_cast<std::size_t>(i);
                done[f].resize(sessions.size());
                for (std::size_t k = 0; k < sessions.size(); ++k) {
                    while (!sessions[k]->Done(values[f][k])) std::this_thread::yield();
                    done[f][k] = Clock::now();
                }
                completed = i + 1;
            }
        });
        auto next = Clock::now();
        for (int i = 0; i < frames; ++i) {
            while (completed.load() < i - 2) std::this_thread::yield();
            const std::size_t f = static_cast<std::size_t>(i);
            for (const auto& session : sessions) {
                session->Submit(0, 1, true);
                values[f].push_back(session->Value());
            }
            at[f] = Clock::now();
            submitted = i + 1;
            next += std::chrono::milliseconds(14);
            std::this_thread::sleep_until(next);
        }
        waiter.join();
        std::vector<std::vector<double>> each(sessions.size());
        std::vector<double> total;
        for (int i = 16; i < frames; ++i) {
            const std::size_t f = static_cast<std::size_t>(i);
            for (std::size_t k = 0; k < sessions.size(); ++k) {
                each[k].push_back(std::chrono::duration<double, std::milli>(done[f][k] - at[f]).count());
            }
            total.push_back(each.back().back());
        }
        std::printf("            each execute done:");
        for (const auto& times : each) std::printf("  %s", Percentiles(times).c_str());
        std::printf(" ms after submission\n");
        return total;
    };
    const std::unique_ptr<GpuLoad> load = MakeLoad(device, load_ms, chunk_ms);
    const int frames = std::getenv("FG_PROFILE_D3D11_FRAMES") ? std::atoi(std::getenv("FG_PROFILE_D3D11_FRAMES")) : 200;
    std::printf("\nDirect3D 12 optical flow: %zu execute(s) a frame, layout %s, %s\n", sessions.size(), layout.c_str(),
                perf == NV_OF_PERF_LEVEL_FAST ? "fast" : "medium");
    const std::vector<double> idle = run(frames);
    std::printf("  idle      done %s ms after submission (median / p90)\n", Percentiles(idle).c_str());
    const std::uint64_t first = load->Frames();
    const auto began = std::chrono::steady_clock::now();
    load->Start();
    const std::vector<double> loaded = run(frames);
    load->Stop();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::printf("  loaded    done %s ms after submission (median / p90); the %s load ran %.1f frames/s\n",
                Percentiles(loaded).c_str(), load->Name(), (load->Frames() - first) / seconds);
}


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
            if (gpu) gpu->SetCopyAfterRelease(true); // the tests' images are their own
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
    const bool hardware = g_bench || g_hardware;
    const ComPtr<IDXGIAdapter1> adapter = hardware ? WantedAdapter() : nullptr;
    const D3D_DRIVER_TYPE type =
        adapter ? D3D_DRIVER_TYPE_UNKNOWN : hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP;
    Check(SUCCEEDED(D3D11CreateDevice(adapter.Get(), type, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                                      &context)),
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

// Research (`contention`, Direct3D 12 on the GPU): how long after its
// submission a just-in-time frame can be used by the runtime (a fence the
// runtime would signal at its release, on the queue the runtime was given)
// while the GPU is saturated by "game" work on the game's queue (a second
// backend generating headset-size frames back to back there):
// - substituted: the runtime has Frame Generation's own high-priority queue;
// - own queue: Frame Generation writes on its own queue, the runtime has the
//   game's (so the release waits behind the game's queued work);
// - game queue: everything on the game's queue.
// A frame must be usable within about a refresh of its submission.
bool g_contention = false;
XrResult XRAPI_PTR IgnoreAcquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    if (index) *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR IgnoreWait(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_PTR IgnoreRelease(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

int RunContention(Harness& harness) {
    const std::uint32_t width = g_profile_width, height = g_profile_height;
    const float outer = 2.0f, inner = 1.26f, vertical = 1.64f, half_ipd = 0.0336f;
    std::string error;
    // The game's binding (the harness may chain a runtime queue ahead of it).
    const XrGraphicsBindingD3D12KHR* game = nullptr;
    for (auto* base = static_cast<const XrBaseInStructure*>(harness.binding); base; base = base->next) {
        if (base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) game = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(base);
    }
    Check(game != nullptr, "contention needs a Direct3D 12 binding");
    // The layer's queue substitution, as in a session: the runtime's queue,
    // and the game's releases (each just after its frame's work was queued,
    // as games release before the GPU has run it).
    FrameGenQueueSubstitution substitution;
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = game;
    std::string note;
    const auto* substituted_info = substitution.Prepare(&session_info, note);
    Check(substitution.Active(), "contention: queue substitution (" + note + ")");
    ID3D12CommandQueue* runtime = static_cast<const XrGraphicsBindingD3D12KHR*>(substituted_info->next)->queue;
    const void* marker = substitution.FrameGenChain(game);
    const XrSwapchain game_swapchain = reinterpret_cast<XrSwapchain>(0xee);
    const XrSwapchainImageWaitInfo game_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    const XrSwapchainImageReleaseInfo game_release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const FrameGenQueueSubstitution::Calls ignore{IgnoreAcquire, IgnoreWait, IgnoreRelease};
    auto make = [&](const char* queue, bool substituted) {
        _putenv_s("VECTORXR_FRAMEGEN_QUEUE", queue);
        _putenv_s("VECTORXR_FRAMEGEN_JIT", "1");
        auto gpu = CreateFrameGenGpu(substituted ? marker : game, error);
        _putenv_s("VECTORXR_FRAMEGEN_QUEUE", "");
        _putenv_s("VECTORXR_FRAMEGEN_JIT", "");
        Check(gpu != nullptr, harness.name + " backend: " + error);
        gpu->SetCopyAfterRelease(true);
        return gpu;
    };
    ComPtr<ID3D12Fence> release_fence;
    Check(SUCCEEDED(game->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&release_fence))), "release fence");
    UINT64 release_value = 0;
    const HANDLE release_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const std::int64_t output_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    std::array<XrSwapchain, 2> sources{reinterpret_cast<XrSwapchain>(0x81), reinterpret_cast<XrSwapchain>(0x82)};
    std::array<std::array<XrSwapchain, 4>, 2> outputs{};
    std::array<XrSwapchain, 2> jit_outputs{reinterpret_cast<XrSwapchain>(0x91), reinterpret_cast<XrSwapchain>(0x92)};
    for (std::size_t eye = 0; eye < 2; ++eye) {
        const float head = eye == 0 ? -half_ipd : half_ipd;
        const float left = eye == 0 ? -outer : -inner, right = eye == 0 ? inner : outer;
        g_swapchains[sources[eye]].clear();
        for (int frame = 0; frame < 2; ++frame) {
            void* image = harness.create(width, height);
            harness.upload_sized(image, width, height, CockpitAt(width, height, head, left, right, vertical, 0.004f * frame));
            g_swapchains[sources[eye]].push_back(image);
        }
        for (std::size_t set = 0; set < 4; ++set) {
            outputs[eye][set] = reinterpret_cast<XrSwapchain>(0xa0 + eye * 4 + set);
            g_swapchains[outputs[eye][set]] = {harness.create(width, height)};
        }
        g_swapchains[jit_outputs[eye]] = {harness.create(width, height)};
    }
    auto views_for = [&](std::uint32_t frame, bool with_outputs) {
        std::vector<FrameGenViewSubmission> views;
        for (std::size_t eye = 0; eye < 2; ++eye) {
            const float head = eye == 0 ? -half_ipd : half_ipd;
            FrameGenViewSubmission view;
            const XrRect2Di rect{{0, 0}, {static_cast<int>(width), static_cast<int>(height)}};
            view.source = {sources[eye], frame, 0, rect, harness.format};
            if (with_outputs) {
                for (const XrSwapchain output : outputs[eye]) view.outputs.push_back({output, 0, 0, rect, output_format});
            }
            view.request.extent = {width, height};
            view.request.output = FrameGenOutputKind::kSrgb8;
            view.request.synthesize = true;
            view.request.temporal = true;
            for (auto* geometry : {&view.request.newest, &view.request.previous}) {
                geometry->angle_left = std::atan(eye == 0 ? -outer : -inner);
                geometry->angle_right = std::atan(eye == 0 ? inner : outer);
                geometry->angle_up = std::atan(vertical);
                geometry->angle_down = -std::atan(vertical);
                geometry->position = {head, 0, 0};
            }
            views.push_back(view);
        }
        return views;
    };
    auto track = [&](FrameGenGpu& gpu) {
        for (std::size_t eye = 0; eye < 2; ++eye) {
            Check(gpu.Track(sources[eye], EnumerateImages, error), error);
            for (const XrSwapchain output : outputs[eye]) Check(gpu.Track(output, EnumerateImages, error), error);
            Check(gpu.Track(jit_outputs[eye], EnumerateImages, error), error);
        }
    };
    // The "game": frames generated at headset size, back to back, on the
    // session queue (4 outputs each: several milliseconds of GPU work).
    auto load = make("game", false);
    track(*load);
    std::atomic<bool> loading{false}, stop{false};
    std::thread game_thread([&] {
        FrameGenTuning tuning;
        tuning.mode = FrameGenSynthesisMode::kExtrapolate;
        tuning.generated = 4;
        tuning.fractions = {0.25f, 0.5f, 0.75f, 1.0f};
        std::string game_error;
        std::uint32_t frame = 0;
        while (!stop) {
            if (!loading) {
                Sleep(1);
                continue;
            }
            FrameGenExecuteResult result;
            load->Execute(views_for(frame++ % 2, true), tuning, result, game_error);
        }
    });
    struct Variant {
        const char* name;
        const char* queue;
        bool substituted;
        bool lazy; // the game's releases (substituted)
    };
    for (const Variant& variant :
         {Variant{"substituted, game releases lazy", "", true, true},
          Variant{"substituted, runtime queue waiting for game releases", "", true, false},
          Variant{"own queue, runtime has the game's", "", false, false},
          Variant{"all on the game's queue", "game", false, false}}) {
        auto jit = make(variant.queue, variant.substituted);
        ID3D12CommandQueue* runtime_side = variant.substituted ? runtime : game->queue;
        track(*jit);
        FrameGenTuning analyze;
        analyze.mode = FrameGenSynthesisMode::kExtrapolate;
        analyze.analyze_only = true;
        FrameGenExecuteResult result;
        Check(jit->Execute(views_for(0, false), analyze, result, error), "analyze: " + error);
        Check(jit->Execute(views_for(1, false), analyze, result, error), "analyze: " + error);
        jit->WaitIdle();
        const XrRect2Di rect{{0, 0}, {static_cast<int>(width), static_cast<int>(height)}};
        const std::vector<FrameGenImage> images{{jit_outputs[0], 0, 0, rect, output_format}, {jit_outputs[1], 0, 0, rect, output_format}};
        for (const bool loaded : {false, true}) {
            loading = loaded;
            Sleep(loaded ? 200 : 20);
            std::vector<double> latencies;
            for (int i = 0; i < 60; ++i) {
                if (variant.substituted) {
                    // The game takes an image and releases it while its GPU
                    // is still rendering the frame.
                    std::uint32_t index = 0;
                    substitution.Acquire(game_swapchain, 1000, ignore, nullptr, &index);
                    substitution.Wait(game_swapchain, variant.lazy, ignore, &game_wait);
                    substitution.Release(game_swapchain, variant.lazy, ignore, &game_release);
                }
                const auto start = std::chrono::steady_clock::now();
                Check(jit->Synthesize(result.frame, FrameGenSynthesisMode::kExtrapolate, 0.5f, images, error),
                      "synthesize: " + error);
                // The runtime's release signal, on the queue it was given.
                Check(SUCCEEDED(runtime_side->Signal(release_fence.Get(), ++release_value)) &&
                          SUCCEEDED(release_fence->SetEventOnCompletion(release_value, release_event)) &&
                          WaitForSingleObject(release_event, 2000) == WAIT_OBJECT_0,
                      "the release did not complete");
                latencies.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                Sleep(5);
            }
            std::sort(latencies.begin(), latencies.end());
            std::printf("%s, %s: usable by the runtime %.2f ms after submission (median), %.2f (90%%), %.2f (max)\n",
                        variant.name, loaded ? "GPU saturated by the game" : "idle GPU", latencies[latencies.size() / 2],
                        latencies[latencies.size() * 9 / 10], latencies.back());
        }
        loading = false;
        load->WaitIdle();
        substitution.Flush(game_swapchain, ignore);
        jit->Reset();
    }
    stop = true;
    game_thread.join();
    load->Reset();
    substitution.Reset();
    CloseHandle(release_event);
    return 0;
}

// The tests' gates (test_gates.h): what a failed check does before it exits
// opens a gate still shut, and the queue held back by it runs on.
void GatesOpenBeforeExitTest(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> gate;
    ComPtr<ID3D12Fence> mark;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mark))),
          "gates: fences");
    {
        const test_gates::Guard guard(gate.Get());
        queue->Wait(gate.Get(), 1);
        queue->Signal(mark.Get(), 1);
        Sleep(50);
        const bool held = mark->GetCompletedValue() == 0;
        test_gates::OpenAllBeforeExit();
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool ran = SUCCEEDED(mark->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!ran) gate->Signal(test_gates::kOpenValue); // not left waiting either way
        Check(held && ran, std::string("gates: a failed check opens a gate still shut (held ") + (held ? "yes" : "no") +
                               ", ran on " + (ran ? "yes" : "no") + ")");
    }
    // Once its guard is gone a gate is not opened again.
    ComPtr<ID3D12Fence> later;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&later))), "gates: fence");
    { const test_gates::Guard guard(later.Get()); }
    test_gates::OpenAllBeforeExit();
    Check(later->GetCompletedValue() == 0, "gates: a gate whose guard is gone is left alone");
    std::cout << "D3D12 test gates passed\n";
}

// Queue substitution (FrameGenQueueSubstitution) against a stand-in runtime
// that keeps OpenXR's swapchain call order as strictly as runtimes may: an
// image is acquired only while the application holds fewer than all of them,
// waited for only when no image is waited for and not yet released, and
// released only once waited for. The runtime is given a queue of its own;
// lazy releases reach it only once the game's GPU work on the image has run
// (here held back by a fence the test signals), with the game's next waits
// held back behind them, in order; earlier only as far as the runtime needs
// one to hand out an image. A release that is not lazy, or a frame showing
// the swapchain, sends everything held back first, and after a frame showing
// the swapchain its waits are not held back.
struct StrictSwapchain {
    std::uint32_t images{3};
    std::uint32_t acquired{0};
    bool waited{false};
    std::uint32_t next{0};
    std::string calls; // a(cquire), w(ait), r(elease), as the runtime saw them
    int refused{0};
};
StrictSwapchain g_strict;
XrResult XRAPI_PTR StrictAcquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) {
    if (g_strict.acquired == g_strict.images) {
        ++g_strict.refused;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    ++g_strict.acquired;
    if (index) *index = g_strict.next;
    g_strict.next = (g_strict.next + 1) % g_strict.images;
    g_strict.calls += 'a';
    return XR_SUCCESS;
}
XrResult XRAPI_PTR StrictWait(XrSwapchain, const XrSwapchainImageWaitInfo*) {
    if (g_strict.waited || g_strict.acquired == 0) {
        ++g_strict.refused;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    g_strict.waited = true;
    g_strict.calls += 'w';
    return XR_SUCCESS;
}
XrResult XRAPI_PTR StrictRelease(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    if (!g_strict.waited) {
        ++g_strict.refused;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    g_strict.waited = false;
    --g_strict.acquired;
    g_strict.calls += 'r';
    return XR_SUCCESS;
}

void SubstitutionTest(ID3D12Device* device, ID3D12CommandQueue* game_queue) {
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = game_queue;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    FrameGenQueueSubstitution substitution;
    std::vector<std::string> logged;
    substitution.SetLog([&](const std::string& message) { logged.push_back(message); });
    std::string note;
    const XrSessionCreateInfo* created = substitution.Prepare(&info, note);
    Check(substitution.Active() && created != &info, "substitution: a Direct3D 12 session gets the layer's queue (" + note + ")");
    const auto* runtime_binding = static_cast<const XrGraphicsBindingD3D12KHR*>(created->next);
    Check(runtime_binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && runtime_binding->device == device &&
              runtime_binding->queue && runtime_binding->queue != game_queue,
          "substitution: the runtime's binding names another queue on the same device");
    const auto* marker = static_cast<const FrameGenRuntimeQueue*>(substitution.FrameGenChain(&binding));
    Check(marker->type == kFrameGenRuntimeQueueType && marker->d3d12_queue == runtime_binding->queue && marker->next == &binding,
          "substitution: Frame Generation's chain carries the runtime's queue ahead of the game's binding");

    const FrameGenQueueSubstitution::Calls strict{StrictAcquire, StrictWait, StrictRelease};
    const XrSwapchain color = reinterpret_cast<XrSwapchain>(0xc0);
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g_strict = {};
    // One game frame: its image taken, waited for, rendered (queued) and
    // released.
    auto frame = [&](bool lazy, const std::string& what) {
        std::uint32_t index = 0;
        Check(substitution.Acquire(color, g_strict.images, strict, nullptr, &index) == XR_SUCCESS,
              "substitution: " + what + ": acquire (" + g_strict.calls + ")");
        Check(substitution.Wait(color, lazy, strict, &wait) == XR_SUCCESS,
              "substitution: " + what + ": wait (" + g_strict.calls + ")");
        Check(substitution.Release(color, lazy, strict, &release) == XR_SUCCESS,
              "substitution: " + what + ": release (" + g_strict.calls + ")");
        Check(g_strict.refused == 0, "substitution: " + what + ": the runtime saw its calls in order (" + g_strict.calls + ")");
    };
    ComPtr<ID3D12Fence> busy;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&busy))), "substitution fence");
    const test_gates::Guard busy_gate(busy.Get());
    ComPtr<ID3D12Fence> idle;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle))), "substitution fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 idle_value = 0;
    auto finish_game_queue = [&](const std::string& what) {
        game_queue->Signal(idle.Get(), ++idle_value);
        idle->SetEventOnCompletion(idle_value, event);
        Check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "substitution: " + what);
    };

    // The game's GPU is busy: its queue waits on a fence the test holds back.
    game_queue->Wait(busy.Get(), 1);
    frame(true, "first frame");
    Check(g_strict.calls == "aw", "substitution: a release waits for the game's GPU work on its image (" + g_strict.calls + ")");
    frame(true, "second frame");
    frame(true, "third frame");
    Check(g_strict.calls == "awaa",
          "substitution: waits behind a held-back release are held back with it (" + g_strict.calls + ")");
    const auto start = std::chrono::steady_clock::now();
    frame(true, "fourth frame"); // all three images held, as the runtime sees it
    Check(g_strict.calls == "awaarwa",
          "substitution: an acquire the runtime could not serve gets the oldest release and the wait after it (" +
              g_strict.calls + ")");
    Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000),
          "substitution: waiting for the oldest release is bounded");
    // The game's work runs: the rest go at its next call, in order.
    Check(SUCCEEDED(busy->Signal(1)), "substitution: release the game's queue");
    finish_game_queue("the game's queue finishes");
    frame(true, "frame after the game's work ran");
    Check(g_strict.calls.rfind("awaarwarwrwraw", 0) == 0,
          "substitution: done calls go at the game's next call (" + g_strict.calls + ")");
    // A frame showing the swapchain: every release first; the game's next
    // waits (one per image) are its own again.
    substitution.Flush(color, strict);
    Check(g_strict.acquired == 0 && !g_strict.waited,
          "substitution: a frame showing the swapchain sends held-back releases first (" + g_strict.calls + ")");
    game_queue->Wait(busy.Get(), 2);
    const std::size_t shown = g_strict.calls.size();
    frame(true, "frame after the swapchain was shown");
    frame(true, "second frame after the swapchain was shown");
    Check(g_strict.calls.substr(shown) == "awarw",
          "substitution: after a frame showing the swapchain, waits are not held back (" + g_strict.calls.substr(shown) + ")");
    // Not lazy: forwarded at once (after anything held back).
    frame(true, "lazy frame");
    std::uint32_t index = 0;
    Check(substitution.Acquire(color, g_strict.images, strict, nullptr, &index) == XR_SUCCESS &&
              substitution.Wait(color, false, strict, &wait) == XR_SUCCESS &&
              substitution.Release(color, false, strict, &release) == XR_SUCCESS,
          "substitution: frame that is not lazy");
    Check(g_strict.refused == 0 && g_strict.acquired == 0 && !g_strict.waited,
          "substitution: a release that is not lazy goes at once, after held-back calls (" + g_strict.calls + ")");
    Check(logged.empty(), "substitution: no held-back call failed (" + (logged.empty() ? std::string() : logged.front()) + ")");
    Check(SUCCEEDED(busy->Signal(2)), "substitution: release the game's queue");
    substitution.Reset();
    Check(!substitution.Active(), "substitution: reset at session end");
    finish_game_queue("the game's queue is not left waiting");
    CloseHandle(event);
    std::cout << "D3D12 queue substitution passed\n";
}

// A layer the runtime shows that Frame Generation does not capture (a native
// Direct3D 12 game's quad layer, say), against the strict stand-in, with
// VECTORXR_FRAMEGEN_SHOWN_LAYERS=lazy. Until a frame shows it lazily its
// releases go at once (the runtime's queue waiting for the game's work);
// after, a release whose work has not run is held back, a frame showing the
// layer does not get it and the runtime's queue never waits for the game's
// work, and the game's next wait first waits for it on the CPU (then it
// goes, before the wait); only once that takes too long does the runtime's
// queue wait instead. Off by default: a frame showing it so changes nothing.
void SubstitutionShownTest(ID3D12Device* device, ID3D12CommandQueue* game_queue) {
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = game_queue;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    FrameGenQueueSubstitution substitution;
    std::vector<std::string> logged;
    substitution.SetLog([&](const std::string& message) { logged.push_back(message); });
    std::string note;
    // Who decides: the session's default (lazy for Vulkan games), which
    // VECTORXR_FRAMEGEN_SHOWN_LAYERS=off or =lazy overrides.
    for (const auto& [setting, by_default, lazy] : {std::tuple{"", false, false}, std::tuple{"", true, true},
                                                    std::tuple{"off", true, false}, std::tuple{"lazy", false, true}}) {
        _putenv_s("VECTORXR_FRAMEGEN_SHOWN_LAYERS", setting);
        FrameGenQueueSubstitution choice;
        Check(choice.Prepare(&info, note, by_default) != &info && choice.ShownLayersLazy() == lazy,
              std::string("shown layers: VECTORXR_FRAMEGEN_SHOWN_LAYERS=") + setting + (by_default ? ", lazy by default" : "") +
                  (lazy ? " keeps them lazy" : " keeps them as they are"));
        choice.Reset();
    }
    _putenv_s("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "lazy");
    const XrSessionCreateInfo* created = substitution.Prepare(&info, note);
    _putenv_s("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "");
    Check(substitution.Active() && substitution.ShownLayersLazy(),
          "shown layers: the session keeps shown layers lazy (VECTORXR_FRAMEGEN_SHOWN_LAYERS=lazy)");
    // The session's runtime queue (another after each Prepare: Reset releases it).
    ID3D12CommandQueue* runtime = static_cast<const XrGraphicsBindingD3D12KHR*>(created->next)->queue;

    const FrameGenQueueSubstitution::Calls strict{StrictAcquire, StrictWait, StrictRelease};
    const XrSwapchain ui = reinterpret_cast<XrSwapchain>(0x51);
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
    const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    g_strict = {};
    ComPtr<ID3D12Fence> busy;
    ComPtr<ID3D12Fence> idle;
    ComPtr<ID3D12Fence> marks;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&busy))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marks))),
          "shown layers: fences");
    const test_gates::Guard busy_gate(busy.Get());
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // A mark that did not run in time completes later: it has an event of its
    // own, so it never ends a wait for the game's queue, and a later mark is
    // reached only when the fence says so (the event just wakes the wait).
    const HANDLE mark_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 idle_value = 0;
    UINT64 mark_value = 0;
    auto finish_game_queue = [&](const std::string& what) {
        game_queue->Signal(idle.Get(), ++idle_value);
        idle->SetEventOnCompletion(idle_value, event);
        Check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "shown layers: " + what);
    };
    // Whether a mark after everything queued on the runtime's queue runs
    // within `ms` (Frame Generation's next frame would).
    auto runtime_reaches = [&](DWORD ms) {
        runtime->Signal(marks.Get(), ++mark_value);
        marks->SetEventOnCompletion(mark_value, mark_event);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (marks->GetCompletedValue() < mark_value) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return false;
            WaitForSingleObject(mark_event, static_cast<DWORD>(left));
        }
        return true;
    };
    auto acquire = [&](const std::string& what) {
        std::uint32_t index = 0;
        Check(substitution.Acquire(ui, g_strict.images, strict, nullptr, &index) == XR_SUCCESS,
              "shown layers: " + what + ": acquire (" + g_strict.calls + ")");
    };
    // The layer's calls on a swapchain Frame Generation does not capture are
    // never lazy themselves.
    auto wait_release = [&](const std::string& what) {
        Check(substitution.Wait(ui, false, strict, &wait) == XR_SUCCESS,
              "shown layers: " + what + ": wait (" + g_strict.calls + ")");
        Check(substitution.Release(ui, false, strict, &release) == XR_SUCCESS,
              "shown layers: " + what + ": release (" + g_strict.calls + ")");
    };

    // A mark that timed out and completed later does not answer the next one.
    ComPtr<ID3D12Fence> hold;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold))), "shown layers: hold fence");
    const test_gates::Guard hold_gate(hold.Get());
    runtime->Wait(hold.Get(), 1);
    Check(!runtime_reaches(20), "shown layers: a mark behind a held runtime queue is not reached");
    Check(SUCCEEDED(hold->Signal(1)), "shown layers: release the runtime's queue");
    for (int i = 0; i < 500 && marks->GetCompletedValue() < mark_value; ++i) Sleep(1);
    runtime->Wait(hold.Get(), 2);
    const bool late_answered = runtime_reaches(50);
    Check(SUCCEEDED(hold->Signal(2)), "shown layers: release the runtime's queue again");
    Check(!late_answered, "shown layers: a late mark's completion does not answer the next mark");
    Check(runtime_reaches(5000), "shown layers: the runtime's queue runs on after the held marks");

    // Not shown lazily yet: its first release goes at once, the runtime's
    // queue waiting for the game's work.
    game_queue->Wait(busy.Get(), 1);
    acquire("first frame");
    wait_release("first frame");
    Check(g_strict.calls == "awr", "shown layers: a release before a frame showed the layer lazily goes at once (" +
                                       g_strict.calls + ")");
    Check(!runtime_reaches(50), "shown layers: ...with the runtime's queue waiting for the game's work");
    Check(SUCCEEDED(busy->Signal(1)), "shown layers: release the game's queue");
    finish_game_queue("the game's queue finishes");
    Check(runtime_reaches(5000), "shown layers: the runtime's queue runs on once the game's work has");

    // A frame shows it lazily: its next release is held back while its work
    // has not run, and a frame showing it meanwhile does not get it.
    substitution.Flush(ui, strict, true);
    game_queue->Wait(busy.Get(), 2);
    acquire("second frame");
    wait_release("second frame");
    substitution.Flush(ui, strict, true);
    Check(g_strict.calls == "awraw" && substitution.ShownLayers().releases == 1,
          "shown layers: a release whose work has not run is held back, past a frame showing the layer (" +
              g_strict.calls + ")");
    Check(runtime_reaches(1000), "shown layers: the runtime's queue does not wait for the game's work");
    // The game's next wait: the release before it goes first, once its work
    // has run (here 30 ms later), waited for on the CPU. No gpu_waits shows
    // the signal ended the wait; the bound is the CPU wait's own (100 ms)
    // plus a timer tick, so a loaded machine's late wake-up is no failure.
    constexpr double kShownWaitBoundMs = 100.0 + 15.6;
    acquire("third frame");
    std::thread work([&] {
        Sleep(30);
        busy->Signal(2);
    });
    const auto start = std::chrono::steady_clock::now();
    Check(substitution.Wait(ui, false, strict, &wait) == XR_SUCCESS, "shown layers: third frame: wait");
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    work.join();
    FrameGenQueueSubstitution::ShownLayerCounts counts = substitution.ShownLayers();
    Check(g_strict.calls == "awrawarw" && counts.cpu_waits == 1 && counts.gpu_waits == 0 && waited >= 20.0 &&
              waited < kShownWaitBoundMs,
          "shown layers: the game's next wait gets the release before it first, once its work has run (" +
              g_strict.calls + ", waited " + std::to_string(waited) + " ms, " + std::to_string(counts.cpu_waits) +
              " CPU waits, " + std::to_string(counts.gpu_waits) + " on the runtime's queue)");
    Check(runtime_reaches(1000), "shown layers: the runtime's queue never waited for the game's work");
    Check(substitution.Release(ui, false, strict, &release) == XR_SUCCESS, "shown layers: third frame: release");
    finish_game_queue("the game's queue finishes the third frame");
    substitution.Flush(ui, strict, true);
    Check(g_strict.calls == "awrawarwr",
          "shown layers: a frame showing the layer gets a release whose work has run (" + g_strict.calls + ")");

    // The game's work takes longer than the CPU waits: the runtime's queue
    // waits for the rest (counted).
    game_queue->Wait(busy.Get(), 3);
    acquire("fourth frame");
    wait_release("fourth frame");
    acquire("fifth frame");
    const auto bounded = std::chrono::steady_clock::now();
    Check(substitution.Wait(ui, false, strict, &wait) == XR_SUCCESS, "shown layers: fifth frame: wait");
    counts = substitution.ShownLayers();
    Check(std::chrono::steady_clock::now() - bounded < std::chrono::milliseconds(1000) &&
              g_strict.calls == "awrawarwrawarw" && counts.cpu_waits == 2 && counts.gpu_waits == 1,
          "shown layers: a CPU wait that expires leaves the release to the runtime's queue (" + g_strict.calls + ", " +
              std::to_string(counts.gpu_waits) + " on the runtime's queue)");
    Check(!runtime_reaches(50), "shown layers: ...whose queue then waits for the game's work");
    Check(SUCCEEDED(busy->Signal(3)), "shown layers: release the game's queue");
    finish_game_queue("the game's queue finishes the fourth frame");
    Check(runtime_reaches(5000), "shown layers: the runtime's queue runs on");
    // The expired wait's completion does not cut the next one short.
    game_queue->Wait(busy.Get(), 4);
    Check(substitution.Release(ui, false, strict, &release) == XR_SUCCESS, "shown layers: fifth frame: release");
    acquire("sixth frame");
    std::thread later_work([&] {
        Sleep(30);
        busy->Signal(4);
    });
    const auto again = std::chrono::steady_clock::now();
    Check(substitution.Wait(ui, false, strict, &wait) == XR_SUCCESS, "shown layers: sixth frame: wait");
    const double waited_again =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - again).count();
    later_work.join();
    counts = substitution.ShownLayers();
    Check(g_strict.calls == "awrawarwrawarwarw" && counts.cpu_waits == 3 && counts.gpu_waits == 1 &&
              waited_again >= 20.0 && waited_again < kShownWaitBoundMs,
          "shown layers: a wait after an expired one still waits for its release on the CPU (" + g_strict.calls +
              ", waited " + std::to_string(waited_again) + " ms, " + std::to_string(counts.gpu_waits) +
              " on the runtime's queue)");
    Check(substitution.Release(ui, false, strict, &release) == XR_SUCCESS, "shown layers: sixth frame: release");
    finish_game_queue("the game's queue finishes the sixth frame");
    // A frame showing it otherwise (passing through, say): every release
    // goes, and its next releases go at once again.
    substitution.Flush(ui, strict, false);
    acquire("seventh frame");
    wait_release("seventh frame");
    Check(g_strict.calls == "awrawarwrawarwarwrawr" && substitution.ShownLayers().releases == 5,
          "shown layers: once shown otherwise, releases go at once again (" + g_strict.calls + ")");
    Check(g_strict.refused == 0, "shown layers: the runtime saw every call in order (" + g_strict.calls + ")");
    Check(logged.empty(), "shown layers: no call failed (" + (logged.empty() ? std::string() : logged.front()) + ")");
    Check(substitution.ShownLayersSummary().find("runtimeQueueWaits=1") != std::string::npos,
          "shown layers: the summary counts the runtime's queue's wait (" + substitution.ShownLayersSummary() + ")");
    // A swapchain shown lazily before the runtime had any release of it:
    // a frame showing it gets its first release all the same.
    const XrSwapchain fresh = reinterpret_cast<XrSwapchain>(0x52);
    g_strict = {};
    std::uint32_t fresh_index = 0;
    Check(substitution.Acquire(fresh, g_strict.images, strict, nullptr, &fresh_index) == XR_SUCCESS,
          "shown layers: a fresh swapchain: acquire");
    substitution.Flush(fresh, strict, true);
    game_queue->Wait(busy.Get(), 5);
    Check(substitution.Wait(fresh, false, strict, &wait) == XR_SUCCESS &&
              substitution.Release(fresh, false, strict, &release) == XR_SUCCESS,
          "shown layers: a fresh swapchain: wait and release");
    Check(g_strict.calls == "aw", "shown layers: a fresh swapchain's release is held back (" + g_strict.calls + ")");
    substitution.Flush(fresh, strict, true);
    Check(g_strict.calls == "awr" && g_strict.refused == 0 && substitution.ShownLayers().gpu_waits == 2 &&
              !runtime_reaches(50),
          "shown layers: a frame showing a swapchain the runtime has no release of gets its first, the runtime's "
          "queue waiting for its work (" +
              g_strict.calls + ")");
    Check(SUCCEEDED(busy->Signal(5)), "shown layers: release the game's queue");
    finish_game_queue("the game's queue finishes the fresh swapchain's frame");
    Check(runtime_reaches(5000), "shown layers: the runtime's queue runs on after the fresh swapchain's frame");
    substitution.Reset();

    // Off by default: a frame showing the layer so changes nothing.
    created = substitution.Prepare(&info, note);
    Check(substitution.Active() && !substitution.ShownLayersLazy() && substitution.ShownLayersSummary().empty(),
          "shown layers: off by default");
    runtime = static_cast<const XrGraphicsBindingD3D12KHR*>(created->next)->queue;
    g_strict = {};
    acquire("off");
    wait_release("off");
    substitution.Flush(ui, strict, true);
    game_queue->Wait(busy.Get(), 6);
    acquire("off, second frame");
    wait_release("off, second frame");
    Check(g_strict.calls == "awrawr" && g_strict.refused == 0 && !runtime_reaches(50),
          "shown layers, off: releases go at once, the runtime's queue waiting for the game's work (" +
              g_strict.calls + ")");
    Check(SUCCEEDED(busy->Signal(6)), "shown layers: release the game's queue");
    finish_game_queue("the game's queue is not left waiting");
    Check(runtime_reaches(5000), "shown layers, off: the runtime's queue runs on");
    substitution.Reset();
    CloseHandle(mark_event);
    CloseHandle(event);
    std::cout << "D3D12 queue substitution with shown layers kept lazy passed\n";
}

int D3D12Test() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    // FG_D3D12_DEBUG=1: the debug layer validates every call (and with
    // FG_D3D12_DEBUG=gpu, resource states as the GPU executes); any error
    // fails the run.
    const std::string debug_mode = std::getenv("FG_D3D12_DEBUG") ? std::getenv("FG_D3D12_DEBUG") : "";
    if (!debug_mode.empty()) {
        ComPtr<ID3D12Debug> debug;
        Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))), "D3D12 debug layer (Graphics Tools) is not installed");
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> debug1;
        if (debug_mode == "gpu" && SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableGPUBasedValidation(TRUE);
        }
    }
    if (const ComPtr<IDXGIAdapter1> wanted = g_bench || g_hardware ? WantedAdapter() : nullptr) {
        // FG_TEST_ADAPTER's adapter, which must run Direct3D 12.
        DXGI_ADAPTER_DESC1 desc{};
        Check(SUCCEEDED(wanted->GetDesc1(&desc)), "the adapter's description");
        const std::string name = test_adapter::Utf8(desc.Description);
        Check(SUCCEEDED(D3D12CreateDevice(wanted.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
              "a Direct3D 12 device on " + name + " (FG_TEST_ADAPTER)");
        warp = wanted;
        std::cout << "D3D12 on " << name << " (FG_TEST_ADAPTER)\n";
    } else if (g_bench || g_hardware) {
        // The first hardware adapter that runs Direct3D 12; without one
        // (a machine with no GPU, or only Microsoft's software adapter)
        // there is nothing to run on, which is not a failure.
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; !device && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                warp = adapter;
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                std::cout << "D3D12 on " << name << '\n';
            }
        }
        if (!device) {
            std::cout << "skipped: no hardware Direct3D 12 adapter\n";
            return 0;
        }
    } else {
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
        Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "D3D12 WARP device");
    }
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
    // As in a session (FrameGenQueueSubstitution): the runtime was given a
    // queue of Frame Generation's own, and the test's queue plays the
    // game's. FG_TEST_RUNTIME_QUEUE=0: the runtime has the game's queue.
    const bool substitute = !(std::getenv("FG_TEST_RUNTIME_QUEUE") && std::string(std::getenv("FG_TEST_RUNTIME_QUEUE")) == "0");
    ComPtr<ID3D12CommandQueue> runtime_queue;
    ComPtr<ID3D12Fence> runtime_fence;
    UINT64 runtime_value = 0;
    if (substitute) {
        D3D12_COMMAND_QUEUE_DESC runtime_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                                              D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
        Check(SUCCEEDED(device->CreateCommandQueue(&runtime_desc, IID_PPV_ARGS(&runtime_queue))) &&
                  SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&runtime_fence))),
              "D3D12 runtime queue");
    }
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
        if (runtime_queue) {
            // The test reads and writes images as the runtime and the game
            // would: after Frame Generation's work on the runtime's queue.
            runtime_queue->Signal(runtime_fence.Get(), ++runtime_value);
            queue->Wait(runtime_fence.Get(), runtime_value);
        }
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
    FrameGenRuntimeQueue runtime_marker;
    runtime_marker.next = &binding;
    runtime_marker.d3d12_queue = runtime_queue.Get();
    harness.binding = runtime_queue ? static_cast<const void*>(&runtime_marker) : &binding;
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
    if (!g_replay && !g_contention && !g_profile && !g_bench) {
        GatesOpenBeforeExitTest(device.Get(), queue.Get());
        SubstitutionTest(device.Get(), queue.Get());
        SubstitutionShownTest(device.Get(), queue.Get());
    }
    int result = g_replay      ? RunReplay(harness)
                 : g_contention ? RunContention(harness)
                 : g_profile   ? RunProfile(harness)
                 : g_bench   ? RunBench(harness)
                             : RunScenarios(harness);
    if (result == 0 && !g_replay && !g_contention && !g_profile && !g_bench) {
        result = RunQuadViews(harness);
    }
    CloseHandle(event);
    if (!debug_mode.empty()) {
        ComPtr<ID3D12InfoQueue> info;
        std::size_t errors = 0;
        if (SUCCEEDED(device.As(&info))) {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                std::vector<char> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(i, message, &size)) &&
                    (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                     message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                    if (++errors <= 10) std::cerr << "D3D12 debug layer: " << message->pDescription << '\n';
                }
            }
        }
        std::cout << "D3D12 debug layer: " << errors << " errors\n";
        Check(errors == 0, "the D3D12 debug layer reported errors");
    }
    return result;
}

// --- Vulkan -------------------------------------------------------------------

#define VK_TEST_FUNCTIONS(X)                                                                                             \
    X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)                       \
    X(GetPhysicalDeviceQueueFamilyProperties)                                                                             \
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
    VkPhysicalDevice physical = devices.front();
    if (const std::optional<std::string> wanted = test_adapter::Wanted()) {
        // FG_TEST_ADAPTER: the first device it names.
        std::vector<test_adapter::Adapter> adapters;
        for (const VkPhysicalDevice device : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(device, &properties);
            adapters.push_back({properties.deviceName, properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU});
        }
        const std::optional<std::size_t> chosen = test_adapter::FirstMatch(adapters, *wanted);
        if (!chosen) {
            std::cout << test_adapter::NoMatch(*wanted, adapters) << "; skipping\n";
            vkDestroyInstance(instance, nullptr);
            FreeLibrary(loader);
            return kSkip;
        }
        physical = devices[*chosen];
        std::cout << "Vulkan on " << adapters[*chosen].name << " (FG_TEST_ADAPTER)\n";
    }
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
    if (api == "contention") {
        // depthxr_framegen_tests contention [width height]: Direct3D 12 on the GPU.
        g_contention = true;
        api = "profile";
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
