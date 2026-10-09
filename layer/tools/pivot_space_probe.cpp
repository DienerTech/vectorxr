// vectorxr_hardware_probe: a real-runtime OpenXR client for exercising VectorXR (docs/hardware-probe.md). This file
// holds the options, the self-test and the OpenXR frame loop; rendering is Direct3D 11 (probe_d3d11.cpp) or Vulkan
// (probe_vulkan.cpp), and the GPU load's bookkeeping is in probe_gpu_load.cpp.
#include "probe_common.h"

#include <timeapi.h>
#include <DirectXMath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#pragma comment(lib, "winmm.lib")

using namespace DirectX;

namespace probe {

const char* ModeName(ProbeMode mode) {
    switch (mode) {
    case ProbeMode::SourceExact:
        return "source-exact";
    case ProbeMode::CrossSpace:
        return "cross-space";
    case ProbeMode::CrossSpaceCached:
        return "cross-space-cached";
    }
    return "unknown";
}

void ThrowIfFailed(HRESULT result, std::string_view operation) {
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << operation << " failed with HRESULT 0x" << std::hex
               << static_cast<uint32_t>(result);
        throw std::runtime_error(stream.str());
    }
}

void CheckXr(XrInstance instance, XrResult result, std::string_view operation) {
    if (XR_SUCCEEDED(result)) {
        return;
    }
    char result_text[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE) {
        xrResultToString(instance, result, result_text);
    }
    std::ostringstream stream;
    stream << operation << " failed: " << static_cast<int32_t>(result);
    if (result_text[0] != '\0') {
        stream << " (" << result_text << ")";
    }
    throw std::runtime_error(stream.str());
}

void Expect(bool condition, std::string_view what) {
    if (!condition) {
        throw std::runtime_error("self-test failed: " + std::string(what));
    }
}

std::optional<int64_t> ChooseFormat(const std::vector<int64_t>& available, const std::vector<int64_t>& preferences) {
    for (const int64_t candidate : preferences) {
        if (std::find(available.begin(), available.end(), candidate) != available.end()) {
            return candidate;
        }
    }
    return std::nullopt;
}

Pattern PatternRects(uint32_t width, uint32_t height, PatternKind kind, double seconds) {
    Pattern pattern;
    switch (kind) {
    case PatternKind::Peripheral:
        pattern.bar_color = {1.0f, 1.0f, 1.0f, 1.0f};
        pattern.grid_color = {0.1f, 0.8f, 0.2f, 1.0f};
        break;
    case PatternKind::Focus:
        pattern.bar_color = {1.0f, 1.0f, 0.0f, 1.0f};
        pattern.grid_color = {0.9f, 0.1f, 0.9f, 1.0f};
        break;
    case PatternKind::Quad:
        pattern.bar_color = {1.0f, 0.5f, 0.0f, 1.0f};
        pattern.grid_color = {0.1f, 0.7f, 0.9f, 1.0f};
        break;
    }
    const int32_t w = static_cast<int32_t>(width);
    const int32_t h = static_cast<int32_t>(height);
    const auto add = [&](int32_t left, int32_t top, int32_t right, int32_t bottom) {
        const PixelRect rect{std::clamp(left, 0, w), std::clamp(top, 0, h), std::clamp(right, 0, w),
                             std::clamp(bottom, 0, h)};
        if (rect.right > rect.left && rect.bottom > rect.top) {
            pattern.grid.push_back(rect);
        }
    };
    const int32_t square = std::max(4, w / 40);
    for (int32_t y = 1; y < 8; ++y) {
        for (int32_t x = 1; x < 8; ++x) {
            const int32_t left = w * x / 8 - square / 2;
            const int32_t top = h * y / 8 - square / 2;
            add(left, top, left + square, top + square);
        }
    }
    const int32_t edge = std::max(2, w / 200);
    add(0, 0, w, edge);
    add(0, h - edge, w, h);
    add(0, 0, edge, h);
    add(w - edge, 0, w, h);
    const int32_t bar_width = std::min(w, std::max(4, w / 24));
    const int32_t bar_left =
        static_cast<int32_t>(std::fmod(seconds / 4.0, 1.0) * static_cast<double>(w - bar_width));
    pattern.bar = {bar_left, 0, bar_left + bar_width, h};
    return pattern;
}

std::array<float, 4> ClearColor(uint32_t view) {
    return {view % 2 == 0 ? 0.015f : 0.025f, 0.02f, view % 2 == 0 ? 0.03f : 0.015f, 1.0f};
}

float DepthAt(float meters) {
    // XMMatrixPerspectiveOffCenterRH: depth = far (d - near) / ((far - near) d).
    return kFarZ * (meters - kNearZ) / ((kFarZ - kNearZ) * meters);
}

namespace {

void PrintUsage() {
    std::cout
        << "VectorXR real-runtime Pivot space probe\n\n"
        << "Usage:\n"
        << "  vectorxr_hardware_probe [options]\n\n"
        << "Options:\n"
        << "  --mode source-exact|cross-space|cross-space-cached\n"
        << "      source-exact        Locate and submit in source LOCAL space.\n"
        << "      cross-space         Locate in source LOCAL, submit in an offset LOCAL space.\n"
        << "      cross-space-cached  Also locate the offset space at the same display time.\n"
        << "  --seconds N             Active render duration (default: 20).\n"
        << "  --threaded-wait         Overlap the next xrWaitFrame with the current xrEndFrame.\n"
        << "  --graphics d3d11|vulkan Render with Direct3D 11 (default) or Vulkan. Vulkan clears and draws the\n"
        << "                          pattern only (no line scene) and is stereo only.\n"
        << "  --vulkan-enable 1|2     XR_KHR_vulkan_enable or XR_KHR_vulkan_enable2 (default: 2).\n"
        << "  --vulkan-api 1.0|1.1|1.2|1.3\n"
        << "                          The Vulkan instance's API version (default: 1.2).\n"
        << "  --vulkan-validation     Enable the Khronos validation layer (from VK_ADD_LAYER_PATH, else the\n"
        << "                          build's own copy) and report its errors and warnings at the end.\n"
        << "  --vk-late-submit        Vulkan: release each view's image first and submit the frame's rendering\n"
        << "                          only after the last release, before xrEndFrame.\n"
        << "  --quad-views            Render four views (XR_VARJO_quad_views) into four color|sampled\n"
        << "                          swapchains at full size, as DCS does (Direct3D 11).\n"
        << "  --side-by-side          One double-wide color swapchain for both eyes, each eye an imageRect.\n"
        << "  --quad-layer            Also submit a 2048x2048 quad layer after the projection, re-rendered and\n"
        << "                          released every frame (a UI panel).\n"
        << "  --minimal-usage         Create the color swapchains with usage 0.\n"
        << "  --full-size             Use the recommended view sizes (default: at most 1280 px).\n"
        << "  --pattern               Also draw a test pattern fixed to each view (a grid and a moving bar;\n"
        << "                          focus views in other colors), visible wherever the headset points.\n"
        << "  --frame-ms N            Make each frame take at least N ms (a slower game).\n"
        << "  --gpu-ms N              Give each frame N ms of GPU work (0-100), split between the views by\n"
        << "                          pixel count; each view's share runs right before the view renders.\n"
        << "  --gpu-frames-ahead N    At most N frames' GPU work queued (0-4, default: 2): a frame waits for\n"
        << "                          the one N before it; 0 waits for its own before xrEndFrame.\n"
        << "  --gpu-ms-fixed          Keep the calibrated GPU work (default: re-measured every 30 frames to\n"
        << "                          hold --gpu-ms while other GPU work competes).\n"
        << "  --gpu-calibrate         Without OpenXR: calibrate --gpu-ms on the default adapter (--graphics\n"
        << "                          vulkan: the first discrete GPU), run 200 frames of the load alone (paced\n"
        << "                          at 90 Hz, or --frame-ms when longer), report what they measured and fail\n"
        << "                          if the mean is off by over 3%.\n"
        << "  --render-check          Without OpenXR: draw the swapchain shapes into images the probe makes\n"
        << "                          itself, check their pixels, and fail on debug layer or validation messages.\n"
        << "  --depth                 Render depth into depth swapchains and submit it with each view\n"
        << "                          (XR_KHR_composition_layer_depth).\n"
        << "  --recreate-seconds N    Destroy and recreate the swapchains every N seconds.\n"
        << "  --sessions N            Run N sessions one after another in one instance (default: 1),\n"
        << "                          each for --seconds.\n"
        << "  --hitch-every N         Every N seconds, one frame takes --hitch-ms longer (a stutter).\n"
        << "  --hitch-ms M            The stutter's length (default: 300).\n"
        << "  --d3d11-debug           Create the Direct3D 11 device with the debug layer and report its\n"
        << "                          errors and warnings at the end.\n"
        << "  --self-test             Validate pose math, option parsing and GPU load arithmetic without\n"
        << "                          a headset.\n"
        << "  --help                  Show this help.\n\n"
        << "Press Escape in the console window to end an active run early.\n";
}

// The value after argv[i] (advancing i).
std::string ParseValue(int argc, char** argv, int& i) {
    const std::string argument = argv[i];
    if (++i >= argc) {
        throw std::runtime_error(argument + " requires a value");
    }
    return argv[i];
}

// The value after argv[i] (advancing i): all of it a number in [minimum, maximum], whole if asked.
double ParseNumber(int argc, char** argv, int& i, double minimum, double maximum, bool whole) {
    const std::string argument = argv[i];
    const std::string text = ParseValue(argc, argv, i);
    std::size_t used = 0;
    double value = std::numeric_limits<double>::quiet_NaN();
    try {
        value = std::stod(text, &used);
    } catch (const std::exception&) {
        used = 0;
    }
    if (used == 0 || used != text.size() || !std::isfinite(value) || value < minimum || value > maximum ||
        (whole && value != std::floor(value))) {
        std::ostringstream message;
        message << argument << " must be " << (whole ? "a whole number " : "") << "between " << minimum << " and "
                << maximum << " (got '" << text << "')";
        throw std::runtime_error(message.str());
    }
    return value;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    std::string vulkan_only; // the first Vulkan-only option given
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--self-test") {
            options.self_test = true;
        } else if (argument == "--threaded-wait") {
            options.threaded_wait = true;
        } else if (argument == "--quad-views") {
            options.quad_views = true;
            options.full_size = true;
        } else if (argument == "--full-size") {
            options.full_size = true;
        } else if (argument == "--pattern") {
            options.pattern = true;
        } else if (argument == "--depth") {
            options.depth = true;
        } else if (argument == "--d3d11-debug") {
            options.d3d11_debug = true;
        } else if (argument == "--side-by-side") {
            options.side_by_side = true;
        } else if (argument == "--quad-layer") {
            options.quad_layer = true;
        } else if (argument == "--minimal-usage") {
            options.minimal_usage = true;
        } else if (argument == "--render-check") {
            options.render_check = true;
        } else if (argument == "--graphics") {
            const std::string value = ParseValue(argc, argv, i);
            if (value == "d3d11") {
                options.graphics = GraphicsApi::D3D11;
            } else if (value == "vulkan") {
                options.graphics = GraphicsApi::Vulkan;
            } else {
                throw std::runtime_error("--graphics must be d3d11 or vulkan (got '" + value + "')");
            }
        } else if (argument == "--vulkan-enable") {
            options.vulkan_enable = static_cast<int>(ParseNumber(argc, argv, i, 1.0, 2.0, true));
            vulkan_only = vulkan_only.empty() ? std::string(argument) : vulkan_only;
        } else if (argument == "--vulkan-api") {
            const std::string value = ParseValue(argc, argv, i);
            if (value == "1.0" || value == "1.1" || value == "1.2" || value == "1.3") {
                options.vulkan_api = VK_MAKE_API_VERSION(0, 1, static_cast<uint32_t>(value[2] - '0'), 0);
            } else {
                throw std::runtime_error("--vulkan-api must be 1.0, 1.1, 1.2 or 1.3 (got '" + value + "')");
            }
            vulkan_only = vulkan_only.empty() ? std::string(argument) : vulkan_only;
        } else if (argument == "--vulkan-validation") {
            options.vulkan_validation = true;
            vulkan_only = vulkan_only.empty() ? std::string(argument) : vulkan_only;
        } else if (argument == "--vk-late-submit") {
            options.vk_late_submit = true;
            vulkan_only = vulkan_only.empty() ? std::string(argument) : vulkan_only;
        } else if (argument == "--hitch-every" || argument == "--hitch-ms") {
            (argument == "--hitch-every" ? options.hitch_every : options.hitch_ms) =
                ParseNumber(argc, argv, i, 0.0, 10000.0, false);
        } else if (argument == "--recreate-seconds") {
            options.recreate_seconds = ParseNumber(argc, argv, i, 0.0, 3600.0, false);
        } else if (argument == "--sessions") {
            options.sessions = static_cast<int>(ParseNumber(argc, argv, i, 1.0, 100.0, true));
        } else if (argument == "--frame-ms") {
            options.frame_ms = ParseNumber(argc, argv, i, 0.0, 1000.0, false);
        } else if (argument == "--gpu-ms") {
            options.gpu_ms = ParseNumber(argc, argv, i, 0.0, 100.0, false);
        } else if (argument == "--gpu-frames-ahead") {
            options.gpu_frames_ahead = static_cast<int>(ParseNumber(argc, argv, i, 0.0, 4.0, true));
        } else if (argument == "--gpu-ms-fixed") {
            options.gpu_ms_fixed = true;
        } else if (argument == "--gpu-calibrate") {
            options.gpu_calibrate = true;
        } else if (argument == "--mode") {
            const std::string value = ParseValue(argc, argv, i);
            if (value == "source-exact") {
                options.mode = ProbeMode::SourceExact;
            } else if (value == "cross-space") {
                options.mode = ProbeMode::CrossSpace;
            } else if (value == "cross-space-cached") {
                options.mode = ProbeMode::CrossSpaceCached;
            } else {
                throw std::runtime_error("unknown --mode value: " + value);
            }
        } else if (argument == "--seconds") {
            options.seconds = ParseNumber(argc, argv, i, 0.0, 3600.0, false);
            if (options.seconds <= 0.0) {
                throw std::runtime_error("--seconds must be above 0");
            }
        } else {
            throw std::runtime_error("unknown argument: " + std::string(argument));
        }
    }
    if (options.graphics == GraphicsApi::D3D11 && !vulkan_only.empty()) {
        throw std::runtime_error(vulkan_only + " requires --graphics vulkan");
    }
    if (options.graphics == GraphicsApi::Vulkan && options.d3d11_debug) {
        throw std::runtime_error("--d3d11-debug requires --graphics d3d11 (Vulkan: --vulkan-validation)");
    }
    if (options.graphics == GraphicsApi::Vulkan && options.quad_views) {
        throw std::runtime_error("--quad-views requires --graphics d3d11 (the Vulkan mode is stereo only)");
    }
    if (options.side_by_side && options.quad_views) {
        throw std::runtime_error("--side-by-side is a stereo shape: it cannot be combined with --quad-views");
    }
    if (options.gpu_calibrate && options.gpu_ms <= 0.0) {
        throw std::runtime_error("--gpu-calibrate requires --gpu-ms N with N above 0");
    }
    return options;
}

void RunOptionSelfTest() {
    const auto parse = [](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), "vectorxr_hardware_probe");
        std::vector<char*> argv;
        for (std::string& argument : arguments) {
            argv.push_back(argument.data());
        }
        return ParseOptions(static_cast<int>(argv.size()), argv.data());
    };
    const auto rejected = [&](std::vector<std::string> arguments) {
        try {
            parse(std::move(arguments));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };

    const Options defaults = parse({});
    Expect(defaults.gpu_ms == 0.0 && defaults.gpu_frames_ahead == 2 && !defaults.gpu_ms_fixed &&
               !defaults.gpu_calibrate,
           "GPU load defaults");
    Expect(defaults.graphics == GraphicsApi::D3D11 && defaults.vulkan_enable == 2 &&
               defaults.vulkan_api == VK_API_VERSION_1_2 && !defaults.vulkan_validation && !defaults.vk_late_submit &&
               !defaults.side_by_side && !defaults.quad_layer && !defaults.minimal_usage,
           "graphics and shape defaults");
    const Options load = parse({"--gpu-ms", "11.5", "--gpu-frames-ahead", "0", "--gpu-ms-fixed", "--quad-views"});
    Expect(load.gpu_ms == 11.5 && load.gpu_frames_ahead == 0 && load.gpu_ms_fixed && load.quad_views,
           "--gpu-ms, --gpu-frames-ahead and --gpu-ms-fixed parse");
    Expect(parse({"--gpu-ms", "0"}).gpu_ms == 0.0 && parse({"--gpu-ms", "100"}).gpu_ms == 100.0 &&
               parse({"--gpu-frames-ahead", "4"}).gpu_frames_ahead == 4,
           "GPU load range ends accepted");
    const Options calibrate = parse({"--gpu-calibrate", "--gpu-ms", "5"});
    Expect(calibrate.gpu_calibrate && calibrate.gpu_ms == 5.0, "--gpu-calibrate with --gpu-ms parses");
    const Options vulkan = parse({"--vulkan-enable", "1", "--vulkan-api", "1.0", "--graphics", "vulkan",
                                  "--vulkan-validation", "--vk-late-submit", "--depth", "--pattern"});
    Expect(vulkan.graphics == GraphicsApi::Vulkan && vulkan.vulkan_enable == 1 &&
               vulkan.vulkan_api == VK_API_VERSION_1_0 && vulkan.vulkan_validation && vulkan.vk_late_submit &&
               vulkan.depth && vulkan.pattern,
           "Vulkan options parse in any order");
    Expect(parse({"--graphics", "vulkan", "--vulkan-api", "1.3"}).vulkan_api == VK_API_VERSION_1_3 &&
               parse({"--graphics", "vulkan", "--vulkan-api", "1.1"}).vulkan_api == VK_API_VERSION_1_1 &&
               parse({"--graphics", "vulkan", "--vulkan-enable", "2"}).vulkan_enable == 2 &&
               parse({"--graphics", "d3d11"}).graphics == GraphicsApi::D3D11,
           "--graphics, --vulkan-api and --vulkan-enable values");
    for (const GraphicsApi api : {GraphicsApi::D3D11, GraphicsApi::Vulkan}) {
        const Options shapes = parse({"--side-by-side", "--quad-layer", "--minimal-usage", "--graphics",
                                      api == GraphicsApi::Vulkan ? "vulkan" : "d3d11"});
        Expect(shapes.side_by_side && shapes.quad_layer && shapes.minimal_usage && shapes.graphics == api,
               "X-Plane shapes parse for both graphics APIs");
    }
    Expect(parse({"--graphics", "vulkan", "--gpu-calibrate", "--gpu-ms", "11"}).gpu_calibrate,
           "--gpu-calibrate with --graphics vulkan");
    Expect(!defaults.render_check && parse({"--render-check", "--graphics", "vulkan", "--vk-late-submit"}).render_check,
           "--render-check");
    Expect(parse({"--seconds", "0.5", "--sessions", "3", "--recreate-seconds", "2.5", "--frame-ms", "15",
                  "--hitch-every", "4", "--hitch-ms", "250"})
                   .sessions == 3,
           "frame options parse");
    for (const std::vector<std::string>& arguments : std::vector<std::vector<std::string>>{
             {"--gpu-ms"},
             {"--gpu-ms", "-1"},
             {"--gpu-ms", "100.5"},
             {"--gpu-ms", "nan"},
             {"--gpu-ms", "inf"},
             {"--gpu-ms", "abc"},
             {"--gpu-ms", "11ms"},
             {"--gpu-ms", ""},
             {"--gpu-frames-ahead"},
             {"--gpu-frames-ahead", "5"},
             {"--gpu-frames-ahead", "-1"},
             {"--gpu-frames-ahead", "1.5"},
             {"--gpu-calibrate"},
             {"--gpu-calibrate", "--gpu-ms", "0"},
             {"--graphics"},
             {"--graphics", "Vulkan"},
             {"--graphics", "d3d12"},
             {"--graphics", ""},
             {"--vulkan-enable", "2"},
             {"--vulkan-validation"},
             {"--vk-late-submit"},
             {"--vulkan-api", "1.2"},
             {"--graphics", "d3d11", "--vulkan-enable", "1"},
             {"--graphics", "vulkan", "--vulkan-enable"},
             {"--graphics", "vulkan", "--vulkan-enable", "0"},
             {"--graphics", "vulkan", "--vulkan-enable", "3"},
             {"--graphics", "vulkan", "--vulkan-enable", "1.5"},
             {"--graphics", "vulkan", "--vulkan-enable", "2x"},
             {"--graphics", "vulkan", "--vulkan-api"},
             {"--graphics", "vulkan", "--vulkan-api", "1"},
             {"--graphics", "vulkan", "--vulkan-api", "1.4"},
             {"--graphics", "vulkan", "--vulkan-api", "1.2.0"},
             {"--graphics", "vulkan", "--vulkan-api", "v1.2"},
             {"--graphics", "vulkan", "--d3d11-debug"},
             {"--graphics", "vulkan", "--quad-views"},
             {"--side-by-side", "--quad-views"},
             {"--seconds", "0"},
             {"--seconds", "15s"},
             {"--seconds", "3601"},
             {"--sessions", "1.5"},
             {"--sessions", "0"},
             {"--frame-ms", "abc"},
             {"--hitch-ms", "-1"},
             {"--recreate-seconds", "nan"},
             {"--mode"},
             {"--mode", "cross"},
             {"--side-by-side=1"},
         }) {
        std::string joined;
        for (const std::string& argument : arguments) {
            joined += (joined.empty() ? "" : " ") + argument;
        }
        Expect(rejected(arguments), "rejects: " + joined);
    }
}

void RunGpuLoadSelfTest() {
    Expect(CalibrationFactor(20.0, 10.0) == 2.0, "calibration scales by target / measured");
    Expect(CalibrationFactor(20.0, 0.001) == 64.0 && CalibrationFactor(20.0, 0.0) == 64.0 &&
               CalibrationFactor(0.5, 100.0) == 1.0 / 64.0,
           "calibration scale is bounded");
    Expect(RecalibrationFactor(11.0, 22.0) == 0.9 && RecalibrationFactor(11.0, 5.0) == 1.1 &&
               RecalibrationFactor(11.0, 0.0) == 1.1,
           "recalibration moves at most 10%");
    Expect(std::abs(RecalibrationFactor(11.0, 10.5) - 11.0 / 10.5) < 1.0e-12, "recalibration inside its bounds");
    Expect(ScaleIterations(kLoadFirstIterations, 64.0) == 262144 && ScaleIterations(3, 0.01) == 1 &&
               ScaleIterations(kLoadMaximumIterations, 64.0) == kLoadMaximumIterations &&
               ScaleIterations(1000, std::numeric_limits<double>::quiet_NaN()) == 1000,
           "iterations scale within [1, maximum]");

    // Quad views as DCS sizes them: the shares follow the pixel counts and add up to the frame's load.
    const std::array<double, 4> pixels{2184.0 * 2200.0, 2184.0 * 2200.0, 2748.0 * 2764.0, 2748.0 * 2764.0};
    double total_pixels = 0.0;
    for (const double count : pixels) {
        total_pixels += count;
    }
    constexpr uint32_t kIterations = 1000003;
    uint64_t sum = 0;
    for (const double count : pixels) {
        sum += ViewIterations(kIterations, count / total_pixels);
    }
    Expect(sum + 2 >= kIterations && sum <= kIterations + 2, "view shares add up to the frame's iterations");
    Expect(ViewIterations(kIterations, pixels[2] / total_pixels) > ViewIterations(kIterations, pixels[0] / total_pixels),
           "a larger view gets a larger share");
    Expect(ViewIterations(kIterations, 0.0) == 0 && ViewIterations(1, 1.0e-9) == 1 &&
               ViewIterations(kIterations, 2.0) == kIterations,
           "view share edges");

    std::vector<double> samples;
    for (int i = 1; i <= 100; ++i) {
        samples.push_back(static_cast<double>(i));
    }
    Expect(Percentile(samples, 0.5) == 51.0 && Percentile(samples, 0.99) == 100.0 && Percentile({}, 0.5) == 0.0,
           "percentiles");
}

void RunPatternSelfTest() {
    const auto inside = [](const PixelRect& rect, uint32_t width, uint32_t height) {
        return rect.left >= 0 && rect.top >= 0 && rect.right <= static_cast<int32_t>(width) &&
               rect.bottom <= static_cast<int32_t>(height) && rect.right > rect.left && rect.bottom > rect.top;
    };
    for (const auto& [width, height] : std::vector<std::pair<uint32_t, uint32_t>>{
             {1280, 1200}, {2184, 2200}, {kQuadLayerSize, kQuadLayerSize}, {8, 6}}) {
        for (const double seconds : {0.0, 1.3, 3.999, 123456.7}) {
            const Pattern pattern = PatternRects(width, height, PatternKind::Peripheral, seconds);
            Expect(std::all_of(pattern.grid.begin(), pattern.grid.end(),
                               [&](const PixelRect& rect) { return inside(rect, width, height); }) &&
                       inside(pattern.bar, width, height),
                   "pattern rects stay inside the view");
        }
    }
    const Pattern pattern = PatternRects(1280, 1200, PatternKind::Focus, 0.0);
    Expect(pattern.grid.size() == 7 * 7 + 4, "the pattern is 49 squares and a frame of 4 edges");
    Expect(pattern.bar.left == 0 && pattern.bar.right == 1280 / 24 && pattern.bar.bottom == 1200,
           "the bar starts at the left edge");
    Expect(PatternRects(1280, 1200, PatternKind::Focus, 2.0).bar.left == (1280 - 1280 / 24) / 2,
           "the bar crosses the view every four seconds");
    Expect(pattern.grid_color != PatternRects(1280, 1200, PatternKind::Peripheral, 0.0).grid_color &&
               pattern.grid_color != PatternRects(1280, 1200, PatternKind::Quad, 0.0).grid_color,
           "peripheral, focus and quad patterns differ in colour");
    Expect(ClearColor(0) != ClearColor(1) && ClearColor(2) == ClearColor(0), "per-eye clear colours");
    Expect(std::abs(DepthAt(kNearZ)) < 1.0e-6f && std::abs(DepthAt(kFarZ) - 1.0f) < 1.0e-6f &&
               DepthAt(2.0f) > 0.97f && DepthAt(2.0f) < 0.98f && DepthAt(1.0f) < DepthAt(2.0f),
           "depth of a point in front of the eye");
}

void RunSelfTest() {
    RunOptionSelfTest();
    RunGpuLoadSelfTest();
    RunPatternSelfTest();
    RunD3D11SelfTest();
    RunVulkanSelfTest();
}

} // namespace
} // namespace probe

namespace {

using namespace probe;

XrPosef IdentityPose() {
    return {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
}

XrQuaternionf NormalizeQuaternion(const XrQuaternionf& quaternion) {
    const float magnitude = std::sqrt(
        quaternion.x * quaternion.x + quaternion.y * quaternion.y +
        quaternion.z * quaternion.z + quaternion.w * quaternion.w);
    if (magnitude <= 1.0e-8f) {
        return IdentityPose().orientation;
    }
    return {quaternion.x / magnitude, quaternion.y / magnitude,
            quaternion.z / magnitude, quaternion.w / magnitude};
}

XrQuaternionf MultiplyQuaternion(const XrQuaternionf& lhs,
                                 const XrQuaternionf& rhs) {
    return NormalizeQuaternion({
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    });
}

XrVector3f RotateVector(const XrQuaternionf& rotation, const XrVector3f& vector) {
    const XMVECTOR quaternion = XMVectorSet(
        rotation.x, rotation.y, rotation.z, rotation.w);
    const XMVECTOR source = XMVectorSet(vector.x, vector.y, vector.z, 0.0f);
    XMFLOAT3 rotated{};
    XMStoreFloat3(&rotated, XMVector3Rotate(source, quaternion));
    return {rotated.x, rotated.y, rotated.z};
}

// Conventional rigid composition: parent_in_world * local_in_parent.
XrPosef ComposePose(const XrPosef& parent, const XrPosef& local) {
    const XrVector3f rotated = RotateVector(parent.orientation, local.position);
    return {
        MultiplyQuaternion(parent.orientation, local.orientation),
        {parent.position.x + rotated.x,
         parent.position.y + rotated.y,
         parent.position.z + rotated.z},
    };
}

XrPosef InvertPose(const XrPosef& pose) {
    const XrQuaternionf inverse_rotation =
        NormalizeQuaternion({-pose.orientation.x, -pose.orientation.y,
                             -pose.orientation.z, pose.orientation.w});
    const XrVector3f inverse_position = RotateVector(
        inverse_rotation,
        {-pose.position.x, -pose.position.y, -pose.position.z});
    return {inverse_rotation, inverse_position};
}

bool NearlyEqual(float lhs, float rhs, float epsilon = 1.0e-4f) {
    return std::abs(lhs - rhs) <= epsilon;
}

bool NearlyIdentity(const XrPosef& pose) {
    return NearlyEqual(pose.orientation.x, 0.0f) &&
           NearlyEqual(pose.orientation.y, 0.0f) &&
           NearlyEqual(pose.orientation.z, 0.0f) &&
           NearlyEqual(std::abs(pose.orientation.w), 1.0f) &&
           NearlyEqual(pose.position.x, 0.0f) &&
           NearlyEqual(pose.position.y, 0.0f) &&
           NearlyEqual(pose.position.z, 0.0f);
}

void RunPoseSelfTest() {
    constexpr float kPi = 3.14159265358979323846f;
    const float half_yaw = 0.5f * kPi / 2.0f;
    const XrPosef target{
        {0.0f, std::sin(half_yaw), 0.0f, std::cos(half_yaw)},
        {1.25f, 0.35f, -0.75f},
    };
    const XrPosef view{
        NormalizeQuaternion({0.11f, -0.23f, 0.07f, 0.96f}),
        {-0.4f, 1.6f, -2.5f},
    };
    const XrPosef transformed = ComposePose(target, view);
    const XrPosef round_trip = ComposePose(InvertPose(target), transformed);
    const XrPosef error = ComposePose(InvertPose(view), round_trip);
    Expect(NearlyIdentity(error), "pose composition round-trip");
}

// An OpenXR swapchain the views render into, and its depth swapchain (--depth).
struct ProbeSwapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    uint32_t id{0}; // the graphics' id
    uint32_t width{0};
    uint32_t height{0};
    XrSwapchain depth_handle{XR_NULL_HANDLE};
    uint32_t depth_id{0};
};

// Where a view renders: its swapchain and its pixels there (--side-by-side: one half).
struct ViewTarget {
    uint32_t swapchain{0};
    PixelRect rect{};
};

int32_t RectWidth(const PixelRect& rect) {
    return rect.right - rect.left;
}

int32_t RectHeight(const PixelRect& rect) {
    return rect.bottom - rect.top;
}

struct WaitedFrame {
    XrResult result{XR_SUCCESS};
    XrFrameState state{XR_TYPE_FRAME_STATE};
};

class WaitFrameWorker {
public:
    explicit WaitFrameWorker(XrSession session) : session_(session) {
        thread_ = std::thread([this] { Run(); });
    }

    ~WaitFrameWorker() {
        {
            std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Request() {
        std::scoped_lock lock(mutex_);
        if (requested_ || result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame request overlapped another request");
        }
        call_started_ = false;
        requested_ = true;
        condition_.notify_all();
    }

    void WaitUntilCallStarted() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return call_started_ || result_ready_ || stop_; });
        if (!call_started_ && !result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame worker stopped before entering the call");
        }
    }

    WaitedFrame Wait() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return result_ready_ || stop_; });
        if (!result_ready_) {
            throw std::runtime_error("threaded xrWaitFrame worker stopped without a result");
        }
        WaitedFrame result = result_;
        result_ready_ = false;
        return result;
    }

private:
    void Run() {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return requested_ || stop_; });
                if (stop_) {
                    return;
                }
                requested_ = false;
                call_started_ = true;
                condition_.notify_all();
            }

            WaitedFrame waited;
            const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
            waited.result = xrWaitFrame(session_, &wait_info, &waited.state);

            {
                std::scoped_lock lock(mutex_);
                result_ = waited;
                result_ready_ = true;
            }
            condition_.notify_all();
        }
    }

    XrSession session_{XR_NULL_HANDLE};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool requested_{false};
    bool call_started_{false};
    bool result_ready_{false};
    bool stop_{false};
    WaitedFrame result_{};
};

class ProbeApp {
public:
    explicit ProbeApp(Options options)
        : options_(options),
          view_configuration_(options.quad_views ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO
                                                 : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO),
          graphics_(options.graphics == GraphicsApi::Vulkan ? CreateVulkanGraphics(options)
                                                            : CreateD3D11Graphics(options)) {}

    // The graphics device outlives the session and the instance that used it, and the GPU's work on the swapchains
    // ends before they are destroyed (Vulkan needs both; Direct3D 11 keeps its resources alive by itself).
    ~ProbeApp() {
        DestroySwapchains();
        gpu_load_.reset();
        DestroySession();
        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
        }
        graphics_.reset();
    }

    void DestroySwapchains() {
        graphics_->RemoveSwapchains();
        for (const ProbeSwapchain& swapchain : swapchains_) {
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
            }
            if (swapchain.depth_handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.depth_handle);
            }
        }
        if (quad_ && quad_->handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(quad_->handle);
        }
        swapchains_.clear();
        views_.clear();
        quad_.reset();
    }

    void DestroySession() {
        if (target_space_ != XR_NULL_HANDLE) {
            xrDestroySpace(target_space_);
            target_space_ = XR_NULL_HANDLE;
        }
        if (source_space_ != XR_NULL_HANDLE) {
            xrDestroySpace(source_space_);
            source_space_ = XR_NULL_HANDLE;
        }
        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }
        session_running_ = false;
        session_state_ = XR_SESSION_STATE_UNKNOWN;
    }

    void Run() {
        CreateInstance();
        CreateSystemAndDevice();
        for (int session = 0; session < options_.sessions; ++session) {
            if (session > 0) {
                DestroySwapchains();
                DestroySession();
                std::cout << "Session " << session + 1 << " of " << options_.sessions << "\n";
            }
            CreateSession();
            CreateSpaces();
            CreateSwapchains();
            WaitForSessionReady();

            std::cout << "Probe active: graphics=" << graphics_->Describe() << ", mode=" << ModeName(options_.mode)
                      << ", threadedWait=" << (options_.threaded_wait ? "yes" : "no")
                      << ", views=" << ViewCount()
                      << ", swapchains=" << (options_.side_by_side ? "side-by-side" : "one per view")
                      << (options_.quad_layer ? ", quadLayer=yes" : "")
                      << (options_.minimal_usage ? ", colorUsage=0" : "")
                      << ", frameMs=" << options_.frame_ms;
            if (gpu_load_) {
                std::cout << ", gpuMs=" << options_.gpu_ms << " (framesAhead " << options_.gpu_frames_ahead
                          << (options_.gpu_ms_fixed ? ", fixed" : "") << ")";
            }
            std::cout << ", duration=" << options_.seconds << "s\n"
                      << "Use a Pivot profile with both yaw and pitch for the strongest visual test.\n";

            RenderLoop();
            RequestAndDrainExit();
        }

        std::cout << "Probe complete: frames=" << rendered_frames_
                  << ", swapchainRecreations=" << recreations_ << ", hitches=" << hitches_
                  << ", invalidViewFrames=" << invalid_view_frames_
                  << ", spaceRelationFailures=" << space_relation_failures_ << "\n";
        graphics_->Report();
        static const char* const call_names[] = {"acquire", "wait", "release", "endFrame"};
        for (int call = 0; call < 4; ++call) {
            std::vector<double> sorted = call_ms_[call];
            if (sorted.empty()) continue;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (const double ms : sorted) sum += ms;
            std::cout << std::fixed << std::setprecision(2) << "Call " << call_names[call] << ": mean "
                      << sum / sorted.size() << " ms, p99 " << sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)]
                      << " ms, max " << sorted.back() << " ms (" << sorted.size() << " calls)\n";
        }
        if (frame_ms_.size() > 1) {
            std::vector<double> sorted(frame_ms_.begin() + 1, frame_ms_.end());
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (const double ms : sorted) sum += ms;
            std::cout << std::fixed << std::setprecision(2) << "Frame interval (xrWaitFrame to xrWaitFrame): mean "
                      << sum / sorted.size() << " ms, median " << sorted[sorted.size() / 2] << " ms, p99 "
                      << sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)] << " ms\n";
        }
        if (gpu_load_) {
            gpu_load_->Drain();
            gpu_load_->Report(std::cout);
        }
    }

private:
    void CreateInstance() {
        uint32_t extension_count = 0;
        CheckXr(XR_NULL_HANDLE,
                xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr),
                "xrEnumerateInstanceExtensionProperties(count)");
        std::vector<XrExtensionProperties> extensions(
            extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
        CheckXr(XR_NULL_HANDLE,
                xrEnumerateInstanceExtensionProperties(
                    nullptr, extension_count, &extension_count, extensions.data()),
                "xrEnumerateInstanceExtensionProperties");
        const auto offered = [&](const char* name) {
            return std::any_of(extensions.begin(), extensions.end(), [&](const XrExtensionProperties& extension) {
                return std::strcmp(extension.extensionName, name) == 0;
            });
        };
        const char* graphics_extension = graphics_->InstanceExtension();
        if (!offered(graphics_extension)) {
            throw std::runtime_error(std::string("active OpenXR runtime does not expose ") + graphics_extension);
        }

        std::vector<const char*> enabled_extensions{graphics_extension};
        if (options_.depth) {
            if (!offered(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) {
                throw std::runtime_error("the runtime does not offer XR_KHR_composition_layer_depth");
            }
            enabled_extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
        }
        if (options_.quad_views) {
            // As DCS asks for them; VectorXR offers them on runtimes without.
            for (const char* name : {XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME}) {
                if (!offered(name)) {
                    throw std::runtime_error(std::string(name) + " is not offered: is VectorXR loaded?");
                }
                enabled_extensions.push_back(name);
            }
        }
        XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(create_info.applicationInfo.applicationName,
                 XR_MAX_APPLICATION_NAME_SIZE,
                 "VectorXR Hardware Probe");
        create_info.applicationInfo.applicationVersion = 1;
        strcpy_s(create_info.applicationInfo.engineName,
                 XR_MAX_ENGINE_NAME_SIZE,
                 "VectorXR Probe");
        create_info.applicationInfo.engineVersion = 1;
        create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        create_info.enabledExtensionNames = enabled_extensions.data();
        CheckXr(XR_NULL_HANDLE, xrCreateInstance(&create_info, &instance_),
                "xrCreateInstance");

        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        CheckXr(instance_, xrGetInstanceProperties(instance_, &properties),
                "xrGetInstanceProperties");
        std::cout << "Runtime: " << properties.runtimeName
                  << " " << XR_VERSION_MAJOR(properties.runtimeVersion) << "."
                  << XR_VERSION_MINOR(properties.runtimeVersion) << "."
                  << XR_VERSION_PATCH(properties.runtimeVersion) << "\n";
    }

    void CreateSystemAndDevice() {
        const XrSystemGetInfo system_info{
            XR_TYPE_SYSTEM_GET_INFO, nullptr, XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        CheckXr(instance_, xrGetSystem(instance_, &system_info, &system_id_),
                "xrGetSystem");

        XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
        CheckXr(instance_, xrGetSystemProperties(instance_, system_id_, &system_properties),
                "xrGetSystemProperties");
        std::cout << "System: " << system_properties.systemName << "\n";

        graphics_->CreateDevice(instance_, system_id_);
        if (options_.gpu_ms > 0.0) {
            gpu_load_ = graphics_->CreateGpuLoad(options_);
            gpu_load_->Calibrate(options_.quad_views ? 4u : 2u);
        }
    }

    void CreateSession() {
        XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
        create_info.next = graphics_->Binding();
        create_info.systemId = system_id_;
        CheckXr(instance_, xrCreateSession(instance_, &create_info, &session_),
                "xrCreateSession");

        uint32_t blend_count = 0;
        CheckXr(instance_, xrEnumerateEnvironmentBlendModes(
                               instance_, system_id_, view_configuration_,
                               0, &blend_count, nullptr),
                "xrEnumerateEnvironmentBlendModes(count)");
        std::vector<XrEnvironmentBlendMode> modes(blend_count);
        CheckXr(instance_, xrEnumerateEnvironmentBlendModes(
                               instance_, system_id_, view_configuration_,
                               blend_count, &blend_count, modes.data()),
                "xrEnumerateEnvironmentBlendModes");
        const auto opaque = std::find(
            modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
        if (modes.empty()) {
            throw std::runtime_error("runtime returned no environment blend modes");
        }
        blend_mode_ = opaque != modes.end() ? *opaque : modes.front();
    }

    void CreateSpaces() {
        XrReferenceSpaceCreateInfo source_info{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        source_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        source_info.poseInReferenceSpace = IdentityPose();
        CheckXr(instance_, xrCreateReferenceSpace(session_, &source_info, &source_space_),
                "xrCreateReferenceSpace(source LOCAL)");

        constexpr float kPi = 3.14159265358979323846f;
        const float half_angle = kPi / 4.0f;
        XrReferenceSpaceCreateInfo target_info{
            XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        target_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        target_info.poseInReferenceSpace = {
            {0.0f, std::sin(half_angle), 0.0f, std::cos(half_angle)},
            {1.25f, 0.35f, -0.75f},
        };
        CheckXr(instance_, xrCreateReferenceSpace(session_, &target_info, &target_space_),
                "xrCreateReferenceSpace(offset LOCAL)");
    }

    // One color swapchain per view, or one double-wide for both eyes (--side-by-side), each with a depth swapchain
    // of its size (--depth); and the quad layer's (--quad-layer).
    void CreateSwapchains() {
        uint32_t view_count = 0;
        CheckXr(instance_, xrEnumerateViewConfigurationViews(
                               instance_, system_id_, view_configuration_,
                               0, &view_count, nullptr),
                "xrEnumerateViewConfigurationViews(count)");
        if (view_count != (options_.quad_views ? 4u : 2u)) {
            throw std::runtime_error(options_.quad_views ? "the quad views configuration does not have four views"
                                                         : "probe requires a two-view PRIMARY_STEREO configuration");
        }
        view_configuration_views_.assign(
            view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CheckXr(instance_, xrEnumerateViewConfigurationViews(
                               instance_, system_id_, view_configuration_,
                               view_count, &view_count, view_configuration_views_.data()),
                "xrEnumerateViewConfigurationViews");

        uint32_t format_count = 0;
        CheckXr(instance_, xrEnumerateSwapchainFormats(
                               session_, 0, &format_count, nullptr),
                "xrEnumerateSwapchainFormats(count)");
        std::vector<int64_t> formats(format_count);
        CheckXr(instance_, xrEnumerateSwapchainFormats(
                               session_, format_count, &format_count, formats.data()),
                "xrEnumerateSwapchainFormats");
        const std::optional<int64_t> color_format = ChooseFormat(formats, graphics_->ColorFormats());
        if (!color_format) {
            throw std::runtime_error("runtime exposes no supported RGBA/BGRA render-target format");
        }
        if (options_.depth &&
            std::find(formats.begin(), formats.end(), graphics_->DepthFormat()) == formats.end()) {
            throw std::runtime_error("the runtime offers no 32-bit float depth swapchains");
        }

        std::vector<std::pair<uint32_t, uint32_t>> sizes;
        for (uint32_t view = 0; view < view_count; ++view) {
            const uint32_t limit = options_.full_size ? UINT32_MAX : kMaximumEyeDimension;
            const uint32_t width = std::min(view_configuration_views_[view].recommendedImageRectWidth, limit);
            const uint32_t height = std::min(view_configuration_views_[view].recommendedImageRectHeight, limit);
            if (width == 0 || height == 0) {
                throw std::runtime_error("runtime returned a zero recommended view dimension");
            }
            sizes.emplace_back(width, height);
        }

        views_.assign(view_count, {});
        if (options_.side_by_side) {
            // As X-Plane: both eyes in one image, the left eye in the left half.
            uint32_t eye_width = 0;
            uint32_t eye_height = 0;
            for (const auto& [width, height] : sizes) {
                eye_width = std::max(eye_width, width);
                eye_height = std::max(eye_height, height);
            }
            CreateProjectionSwapchain(eye_width * view_count, eye_height, *color_format);
            for (uint32_t view = 0; view < view_count; ++view) {
                const int32_t left = static_cast<int32_t>(eye_width * view);
                views_[view] = {0, {left, 0, left + static_cast<int32_t>(sizes[view].first),
                                    static_cast<int32_t>(sizes[view].second)}};
            }
        } else {
            for (uint32_t view = 0; view < view_count; ++view) {
                CreateProjectionSwapchain(sizes[view].first, sizes[view].second, *color_format);
                views_[view] = {view, {0, 0, static_cast<int32_t>(sizes[view].first),
                                       static_cast<int32_t>(sizes[view].second)}};
            }
        }
        if (options_.quad_layer) {
            quad_.emplace();
            const XrSwapchainCreateInfo info = SwapchainInfo(
                kQuadLayerSize, kQuadLayerSize, *color_format,
                options_.minimal_usage ? 0 : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
            CheckXr(instance_, xrCreateSwapchain(session_, &info, &quad_->handle), "xrCreateSwapchain(quad layer)");
            quad_->width = info.width;
            quad_->height = info.height;
            quad_->id = graphics_->AddSwapchain(instance_, quad_->handle, info, SwapchainRole::Quad);
        }

        std::cout << "Probe swapchains:";
        for (const ProbeSwapchain& swapchain : swapchains_) {
            std::cout << " " << swapchain.width << "x" << swapchain.height;
        }
        std::cout << " (format " << *color_format << ", " << graphics_->ImageCount(swapchains_[0].id)
                  << " images each" << (options_.depth ? ", with depth swapchains" : "")
                  << (options_.minimal_usage ? ", usage 0" : "") << ")";
        if (quad_) {
            std::cout << "; quad layer " << quad_->width << "x" << quad_->height << " ("
                      << graphics_->ImageCount(quad_->id) << " images)";
        }
        std::cout << "\n";
    }

    static XrSwapchainCreateInfo SwapchainInfo(uint32_t width, uint32_t height, int64_t format,
                                               XrSwapchainUsageFlags usage) {
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

    void CreateProjectionSwapchain(uint32_t width, uint32_t height, int64_t format) {
        swapchains_.emplace_back();
        ProbeSwapchain& swapchain = swapchains_.back();
        swapchain.width = width;
        swapchain.height = height;
        const XrSwapchainUsageFlags usage =
            options_.minimal_usage ? 0
                                   : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                         (options_.quad_views ? XR_SWAPCHAIN_USAGE_SAMPLED_BIT : 0);
        const XrSwapchainCreateInfo info = SwapchainInfo(width, height, format, usage);
        CheckXr(instance_, xrCreateSwapchain(session_, &info, &swapchain.handle), "xrCreateSwapchain");
        swapchain.id = graphics_->AddSwapchain(
            instance_, swapchain.handle, info,
            options_.depth ? SwapchainRole::ProjectionWithDepth : SwapchainRole::Projection);
        if (options_.depth) {
            const XrSwapchainCreateInfo depth_info = SwapchainInfo(
                width, height, graphics_->DepthFormat(), XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            CheckXr(instance_, xrCreateSwapchain(session_, &depth_info, &swapchain.depth_handle),
                    "xrCreateSwapchain(depth)");
            swapchain.depth_id =
                graphics_->AddSwapchain(instance_, swapchain.depth_handle, depth_info, SwapchainRole::Depth);
        }
    }

    bool PollEvents() {
        for (;;) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            const XrResult poll_result = xrPollEvent(instance_, &event);
            if (poll_result == XR_EVENT_UNAVAILABLE) {
                break;
            }
            CheckXr(instance_, poll_result, "xrPollEvent");
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto* changed =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
                session_state_ = changed->state;
                std::cout << "Session state: " << static_cast<int>(session_state_) << "\n";
                if (session_state_ == XR_SESSION_STATE_READY && !session_running_) {
                    const XrSessionBeginInfo begin_info{
                        XR_TYPE_SESSION_BEGIN_INFO, nullptr, view_configuration_};
                    CheckXr(instance_, xrBeginSession(session_, &begin_info),
                            "xrBeginSession");
                    session_running_ = true;
                } else if (session_state_ == XR_SESSION_STATE_STOPPING && session_running_) {
                    CheckXr(instance_, xrEndSession(session_), "xrEndSession");
                    session_running_ = false;
                } else if (session_state_ == XR_SESSION_STATE_EXITING ||
                           session_state_ == XR_SESSION_STATE_LOSS_PENDING) {
                    return false;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                return false;
            }
        }
        return true;
    }

    void WaitForSessionReady() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!session_running_) {
            if (!PollEvents()) {
                throw std::runtime_error("runtime exited before the probe session became ready");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    "timed out waiting for the headset session; wake or wear the headset and retry");
            }
            Sleep(10);
        }
    }

    WaitedFrame WaitOnCallingThread() {
        WaitedFrame waited;
        const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        waited.result = xrWaitFrame(session_, &wait_info, &waited.state);
        return waited;
    }

    bool LocateSourceViews(XrTime display_time,
                           std::array<XrView, kMaximumViews>* views,
                           XrViewState* view_state) {
        views->fill(XrView{XR_TYPE_VIEW});
        *view_state = {XR_TYPE_VIEW_STATE};
        const XrViewLocateInfo locate_info{
            XR_TYPE_VIEW_LOCATE_INFO, nullptr, view_configuration_,
            display_time, source_space_};
        uint32_t count = 0;
        CheckXr(instance_, xrLocateViews(session_, &locate_info, view_state,
                                         ViewCount(), &count, views->data()),
                "xrLocateViews(source)");
        constexpr XrViewStateFlags kRequired =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        return count == ViewCount() &&
               (view_state->viewStateFlags & kRequired) == kRequired;
    }

    void LocateTargetViewsForCache(XrTime display_time) {
        std::array<XrView, kMaximumViews> target_views;
        target_views.fill(XrView{XR_TYPE_VIEW});
        XrViewState target_state{XR_TYPE_VIEW_STATE};
        const XrViewLocateInfo locate_info{
            XR_TYPE_VIEW_LOCATE_INFO, nullptr, view_configuration_,
            display_time, target_space_};
        uint32_t count = 0;
        CheckXr(instance_, xrLocateViews(session_, &locate_info, &target_state,
                                         ViewCount(), &count, target_views.data()),
                "xrLocateViews(offset target cache)");
        if (count != ViewCount()) {
            throw std::runtime_error("target-space xrLocateViews returned an unexpected view count");
        }
    }

    std::optional<XrPosef> LocateSourceInTarget(XrTime display_time) {
        XrSpaceLocation relation{XR_TYPE_SPACE_LOCATION};
        const XrResult result = xrLocateSpace(
            source_space_, target_space_, display_time, &relation);
        constexpr XrSpaceLocationFlags kRequired =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_FAILED(result) || (relation.locationFlags & kRequired) != kRequired) {
            ++space_relation_failures_;
            return std::nullopt;
        }
        return relation.pose;
    }

    // How long a swapchain call took; a slow one is reported (a few times).
    void NoteCall(int call, std::chrono::steady_clock::time_point start) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        call_ms_[call].push_back(ms);
        if (ms > 30.0 && slow_calls_reported_ < 40) {
            ++slow_calls_reported_;
            static const char* const names[] = {"xrAcquireSwapchainImage", "xrWaitSwapchainImage",
                                                "xrReleaseSwapchainImage", "xrEndFrame"};
            std::cout << "Slow " << names[call] << ": " << std::fixed << std::setprecision(1) << ms
                      << " ms at frame " << rendered_frames_ << " (recreations so far " << recreations_ << ")\n";
        }
    }

    // Acquires and waits for the swapchain's color image (timed) and its depth image.
    void AcquireImages(const ProbeSwapchain& swapchain, uint32_t* color_index, uint32_t* depth_index) {
        const XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        auto call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrAcquireSwapchainImage(
                               swapchain.handle, &acquire_info, color_index),
                "xrAcquireSwapchainImage");
        NoteCall(0, call_start);
        const XrSwapchainImageWaitInfo wait_info{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrWaitSwapchainImage(swapchain.handle, &wait_info),
                "xrWaitSwapchainImage");
        NoteCall(1, call_start);
        if (swapchain.depth_handle != XR_NULL_HANDLE) {
            CheckXr(instance_, xrAcquireSwapchainImage(swapchain.depth_handle, &acquire_info, depth_index),
                    "xrAcquireSwapchainImage(depth)");
            CheckXr(instance_, xrWaitSwapchainImage(swapchain.depth_handle, &wait_info),
                    "xrWaitSwapchainImage(depth)");
        }
    }

    void ReleaseImages(const ProbeSwapchain& swapchain) {
        const XrSwapchainImageReleaseInfo release_info{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrReleaseSwapchainImage(
                               swapchain.handle, &release_info),
                "xrReleaseSwapchainImage");
        NoteCall(2, call_start);
        if (swapchain.depth_handle != XR_NULL_HANDLE) {
            CheckXr(instance_, xrReleaseSwapchainImage(swapchain.depth_handle, &release_info),
                    "xrReleaseSwapchainImage(depth)");
        }
    }

    // The views that render into swapchain `index`: each view's GPU load share right before it renders (the first
    // one's before the acquire), the image released after its last view.
    void RenderSwapchain(uint32_t index, const std::array<XrView, kMaximumViews>& views, double total_pixels) {
        const ProbeSwapchain& swapchain = swapchains_[index];
        uint32_t color_index = 0;
        uint32_t depth_index = 0;
        bool acquired = false;
        for (uint32_t view = 0; view < ViewCount(); ++view) {
            const ViewTarget& target = views_[view];
            if (target.swapchain != index) {
                continue;
            }
            if (gpu_load_) {
                gpu_load_->DispatchView(static_cast<double>(RectWidth(target.rect)) * RectHeight(target.rect) /
                                        total_pixels);
            }
            ViewDraw draw;
            draw.first_in_image = !acquired;
            if (!acquired) {
                AcquireImages(swapchain, &color_index, &depth_index);
                acquired = true;
            }
            draw.color = swapchain.id;
            draw.color_image = color_index;
            if (swapchain.depth_handle != XR_NULL_HANDLE) {
                draw.depth = swapchain.depth_id;
                draw.depth_image = depth_index;
            }
            draw.rect = target.rect;
            draw.pose = views[view].pose;
            draw.fov = views[view].fov;
            draw.view = view;
            draw.pattern = options_.pattern;
            graphics_->Draw(draw);
        }
        if (!acquired) {
            return;
        }
        graphics_->ImageDone();
        ReleaseImages(swapchain);
    }

    // --quad-layer: as X-Plane's UI panel, re-rendered and released every frame.
    void RenderQuad() {
        uint32_t color_index = 0;
        const XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        CheckXr(instance_, xrAcquireSwapchainImage(quad_->handle, &acquire_info, &color_index),
                "xrAcquireSwapchainImage(quad layer)");
        const XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        CheckXr(instance_, xrWaitSwapchainImage(quad_->handle, &wait_info), "xrWaitSwapchainImage(quad layer)");
        ViewDraw draw;
        draw.color = quad_->id;
        draw.color_image = color_index;
        draw.rect = {0, 0, static_cast<int32_t>(quad_->width), static_cast<int32_t>(quad_->height)};
        draw.quad = true;
        graphics_->Draw(draw);
        graphics_->ImageDone();
        const XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        CheckXr(instance_, xrReleaseSwapchainImage(quad_->handle, &release_info),
                "xrReleaseSwapchainImage(quad layer)");
    }

    void RenderLoop() {
        std::unique_ptr<WaitFrameWorker> wait_worker;
        if (options_.threaded_wait) {
            wait_worker = std::make_unique<WaitFrameWorker>(session_);
            wait_worker->Request();
        }
        WaitedFrame waited = options_.threaded_wait
            ? wait_worker->Wait()
            : WaitOnCallingThread();

        const auto started = std::chrono::steady_clock::now();
        auto last_recreate = started;
        auto last_hitch = started;
        last_frame_started_.reset();
        bool exit_requested = false;
        while (session_running_ && !exit_requested) {
            CheckXr(instance_, waited.result, "xrWaitFrame");
            const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            CheckXr(instance_, xrBeginFrame(session_, &begin_info), "xrBeginFrame");
            if (gpu_load_) {
                gpu_load_->BeginFrame();
            }
            graphics_->BeginFrame();

            const auto frame_started = std::chrono::steady_clock::now();
            if (last_frame_started_) {
                frame_ms_.push_back(
                    std::chrono::duration<double, std::milli>(frame_started - *last_frame_started_).count());
            }
            last_frame_started_ = frame_started;
            std::array<XrCompositionLayerDepthInfoKHR, kMaximumViews> depth_infos;
            depth_infos.fill(XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
            std::array<XrView, kMaximumViews> source_views;
            source_views.fill(XrView{XR_TYPE_VIEW});
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::array<XrCompositionLayerProjectionView, kMaximumViews> projection_views;
            projection_views.fill(XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
            bool submit_projection = false;
            XrSpace projection_space = source_space_;

            if (waited.state.shouldRender == XR_TRUE &&
                LocateSourceViews(waited.state.predictedDisplayTime,
                                  &source_views, &view_state)) {
                if (options_.mode == ProbeMode::CrossSpaceCached) {
                    LocateTargetViewsForCache(waited.state.predictedDisplayTime);
                }

                std::optional<XrPosef> source_in_target;
                if (options_.mode != ProbeMode::SourceExact) {
                    source_in_target = LocateSourceInTarget(
                        waited.state.predictedDisplayTime);
                    projection_space = target_space_;
                }

                if (options_.mode == ProbeMode::SourceExact || source_in_target.has_value()) {
                    double total_pixels = 0.0;
                    for (const ViewTarget& target : views_) {
                        total_pixels += static_cast<double>(RectWidth(target.rect)) * RectHeight(target.rect);
                    }
                    for (uint32_t swapchain = 0; swapchain < swapchains_.size(); ++swapchain) {
                        RenderSwapchain(swapchain, source_views, total_pixels);
                    }
                    for (uint32_t eye = 0; eye < ViewCount(); ++eye) {
                        const ProbeSwapchain& swapchain = swapchains_[views_[eye].swapchain];
                        const PixelRect& rect = views_[eye].rect;
                        projection_views[eye].pose =
                            source_in_target.has_value()
                                ? ComposePose(*source_in_target, source_views[eye].pose)
                                : source_views[eye].pose;
                        projection_views[eye].fov = source_views[eye].fov;
                        projection_views[eye].subImage.swapchain = swapchain.handle;
                        projection_views[eye].subImage.imageRect = {
                            {rect.left, rect.top},
                            {RectWidth(rect), RectHeight(rect)},
                        };
                        projection_views[eye].subImage.imageArrayIndex = 0;
                        if (swapchain.depth_handle != XR_NULL_HANDLE) {
                            XrCompositionLayerDepthInfoKHR& depth = depth_infos[eye];
                            depth.subImage = projection_views[eye].subImage;
                            depth.subImage.swapchain = swapchain.depth_handle;
                            depth.minDepth = 0.0f;
                            depth.maxDepth = 1.0f;
                            depth.nearZ = kNearZ;
                            depth.farZ = kFarZ;
                            projection_views[eye].next = &depth;
                        }
                    }
                    submit_projection = true;
                }
            } else if (waited.state.shouldRender == XR_TRUE) {
                ++invalid_view_frames_;
            }
            const bool submit_quad = quad_.has_value() && waited.state.shouldRender == XR_TRUE;
            if (submit_quad) {
                RenderQuad();
            }
            graphics_->EndFrame();
            if (gpu_load_) {
                gpu_load_->EndFrame();
            }

            XrCompositionLayerProjection projection{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = projection_space;
            projection.viewCount = ViewCount();
            projection.views = projection_views.data();
            // The quad layer: a panel a metre ahead of where the session started, on top of the projection.
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            if (submit_quad) {
                quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                quad.space = source_space_;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = quad_->handle;
                quad.subImage.imageRect = {{0, 0},
                                           {static_cast<int32_t>(quad_->width), static_cast<int32_t>(quad_->height)}};
                quad.subImage.imageArrayIndex = 0;
                quad.pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, -0.15f, -1.0f}};
                quad.size = {0.6f, 0.6f};
            }
            std::vector<const XrCompositionLayerBaseHeader*> layers;
            if (submit_projection) {
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
            }
            if (submit_quad) {
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
            }

            // A slower game: the frame takes at least --frame-ms (plus, now and
            // then, a stutter: --hitch-every).
            double frame_ms = options_.frame_ms;
            if (options_.hitch_every > 0.0 &&
                std::chrono::duration<double>(frame_started - last_hitch).count() >= options_.hitch_every) {
                frame_ms += options_.hitch_ms;
                last_hitch = frame_started;
                ++hitches_;
            }
            while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_started)
                       .count() < frame_ms) {
                Sleep(1);
            }
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            exit_requested = elapsed >= options_.seconds ||
                             (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            bool next_wait_requested = false;
            if (options_.threaded_wait && !exit_requested) {
                // This is the intentional DCS-style overlap: the persistent
                // wait thread enters xrWaitFrame(N+1) while this thread is
                // about to submit xrEndFrame(N).
                wait_worker->Request();
                wait_worker->WaitUntilCallStarted();
                next_wait_requested = true;
                SwitchToThread();
            }

            XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
            end_info.displayTime = waited.state.predictedDisplayTime;
            end_info.environmentBlendMode = blend_mode_;
            end_info.layerCount = static_cast<uint32_t>(layers.size());
            end_info.layers = layers.empty() ? nullptr : layers.data();
            const auto end_start = std::chrono::steady_clock::now();
            const XrResult end_result = xrEndFrame(session_, &end_info);
            NoteCall(3, end_start);
            ++rendered_frames_;

            if (next_wait_requested) {
                waited = wait_worker->Wait();
            }
            CheckXr(instance_, end_result, "xrEndFrame");
            if (!PollEvents()) {
                break;
            }
            if (exit_requested || !session_running_) {
                break;
            }
            if (options_.recreate_seconds > 0.0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - last_recreate).count() >=
                    options_.recreate_seconds) {
                // As a game that changes its render resolution does: no image is held.
                DestroySwapchains();
                CreateSwapchains();
                ++recreations_;
                last_recreate = std::chrono::steady_clock::now();
            }
            if (!options_.threaded_wait) {
                waited = WaitOnCallingThread();
            }

            if (rendered_frames_ % 300 == 0) {
                std::cout << "Rendered " << rendered_frames_ << " frames\n";
            }
        }
    }

    void RequestAndDrainExit() {
        if (session_running_) {
            const XrResult request_result = xrRequestExitSession(session_);
            if (XR_FAILED(request_result) && request_result != XR_ERROR_SESSION_NOT_RUNNING) {
                CheckXr(instance_, request_result, "xrRequestExitSession");
            }
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (session_running_ && std::chrono::steady_clock::now() < deadline) {
            if (!PollEvents()) {
                break;
            }
            Sleep(10);
        }
    }

    uint32_t ViewCount() const {
        return static_cast<uint32_t>(views_.size());
    }

    std::optional<std::chrono::steady_clock::time_point> last_frame_started_;
    std::vector<double> frame_ms_;
    uint64_t recreations_{0};
    uint64_t hitches_{0};
    std::array<std::vector<double>, 4> call_ms_;
    int slow_calls_reported_{0};

    Options options_;
    XrViewConfigurationType view_configuration_;
    std::unique_ptr<ProbeGraphics> graphics_;
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId system_id_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace source_space_{XR_NULL_HANDLE};
    XrSpace target_space_{XR_NULL_HANDLE};
    XrSessionState session_state_{XR_SESSION_STATE_UNKNOWN};
    XrEnvironmentBlendMode blend_mode_{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    bool session_running_{false};
    std::vector<XrViewConfigurationView> view_configuration_views_;
    std::vector<ProbeSwapchain> swapchains_;
    std::vector<ViewTarget> views_;
    std::optional<ProbeSwapchain> quad_;
    std::unique_ptr<GpuLoad> gpu_load_;
    uint64_t rendered_frames_{0};
    uint64_t invalid_view_frames_{0};
    uint64_t space_relation_failures_{0};
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = probe::ParseOptions(argc, argv);
        if (options.help) {
            probe::PrintUsage();
            return 0;
        }
        if (options.self_test) {
            probe::RunSelfTest();
            RunPoseSelfTest();
            std::cout << "vectorxr_hardware_probe self-test passed\n";
            return 0;
        }
        if (options.render_check) {
            return probe::RunRenderCheck(options) ? 0 : 1;
        }
        if (options.gpu_calibrate) {
            timeBeginPeriod(1);
            const bool within = options.graphics == GraphicsApi::Vulkan ? probe::RunVulkanGpuCalibration(options)
                                                                        : probe::RunD3D11GpuCalibration(options);
            timeEndPeriod(1);
            return within ? 0 : 1;
        }
        // Sleep(1) in --frame-ms pacing sleeps a whole timer period otherwise.
        timeBeginPeriod(1);
        ProbeApp(options).Run();
        timeEndPeriod(1);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vectorxr_hardware_probe: " << error.what() << "\n";
        return 1;
    }
}
