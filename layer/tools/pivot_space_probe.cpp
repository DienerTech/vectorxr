#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <timeapi.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

// Quad views (--quad-views): peripheral left/right, then focus left/right, as
// DCS renders them.
constexpr uint32_t kMaximumViews = 4;
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;
constexpr uint32_t kMaximumEyeDimension = 1280;

enum class ProbeMode {
    SourceExact,
    CrossSpace,
    CrossSpaceCached,
};

struct Options {
    ProbeMode mode{ProbeMode::CrossSpace};
    double seconds{20.0};
    bool threaded_wait{false};
    bool quad_views{false};
    bool full_size{false};
    bool pattern{false};
    bool depth{false};
    double recreate_seconds{0.0};
    int sessions{1};
    bool d3d11_debug{false};
    double hitch_every{0.0}; // seconds
    double hitch_ms{300.0};
    double frame_ms{0.0};
    double gpu_ms{0.0};       // GPU work per frame (0: none)
    int gpu_frames_ahead{2};  // frames whose GPU work may be queued
    bool gpu_ms_fixed{false}; // keep the calibrated work instead of re-measuring
    bool gpu_calibrate{false};
    bool self_test{false};
    bool help{false};
};

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
        << "  --quad-views            Render four views (XR_VARJO_quad_views) into four color|sampled\n"
        << "                          swapchains at full size, as DCS does.\n"
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
        << "  --gpu-calibrate         Without OpenXR: calibrate --gpu-ms on the default adapter, run 200\n"
        << "                          frames of the load alone (paced at 90 Hz, or --frame-ms when longer),\n"
        << "                          report what they measured and fail if the mean is off by over 3%.\n"
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

// The value after argv[i] (advancing i): all of it a number in [minimum, maximum], whole if asked.
double ParseNumber(int argc, char** argv, int& i, double minimum, double maximum, bool whole) {
    const std::string argument = argv[i];
    if (++i >= argc) {
        throw std::runtime_error(argument + " requires a value");
    }
    const std::string text = argv[i];
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
        } else if (argument == "--hitch-every" || argument == "--hitch-ms") {
            if (++i >= argc) {
                throw std::runtime_error(std::string(argument) + " requires a value");
            }
            const double value = std::stod(argv[i]);
            if (!std::isfinite(value) || value < 0.0 || value > 10000.0) {
                throw std::runtime_error(std::string(argument) + " is out of range");
            }
            (argument == "--hitch-every" ? options.hitch_every : options.hitch_ms) = value;
        } else if (argument == "--recreate-seconds") {
            if (++i >= argc) {
                throw std::runtime_error("--recreate-seconds requires a value");
            }
            options.recreate_seconds = std::stod(argv[i]);
            if (!std::isfinite(options.recreate_seconds) || options.recreate_seconds < 0.0) {
                throw std::runtime_error("--recreate-seconds must not be negative");
            }
        } else if (argument == "--sessions") {
            if (++i >= argc) {
                throw std::runtime_error("--sessions requires a value");
            }
            options.sessions = std::stoi(argv[i]);
            if (options.sessions < 1 || options.sessions > 100) {
                throw std::runtime_error("--sessions must be between 1 and 100");
            }
        } else if (argument == "--frame-ms") {
            if (++i >= argc) {
                throw std::runtime_error("--frame-ms requires a value");
            }
            options.frame_ms = std::stod(argv[i]);
            if (!std::isfinite(options.frame_ms) || options.frame_ms < 0.0 || options.frame_ms > 1000.0) {
                throw std::runtime_error("--frame-ms must be between 0 and 1000");
            }
        } else if (argument == "--gpu-ms") {
            options.gpu_ms = ParseNumber(argc, argv, i, 0.0, 100.0, false);
        } else if (argument == "--gpu-frames-ahead") {
            options.gpu_frames_ahead = static_cast<int>(ParseNumber(argc, argv, i, 0.0, 4.0, true));
        } else if (argument == "--gpu-ms-fixed") {
            options.gpu_ms_fixed = true;
        } else if (argument == "--gpu-calibrate") {
            options.gpu_calibrate = true;
        } else if (argument == "--mode") {
            if (++i >= argc) {
                throw std::runtime_error("--mode requires a value");
            }
            const std::string_view value = argv[i];
            if (value == "source-exact") {
                options.mode = ProbeMode::SourceExact;
            } else if (value == "cross-space") {
                options.mode = ProbeMode::CrossSpace;
            } else if (value == "cross-space-cached") {
                options.mode = ProbeMode::CrossSpaceCached;
            } else {
                throw std::runtime_error("unknown --mode value: " + std::string(value));
            }
        } else if (argument == "--seconds") {
            if (++i >= argc) {
                throw std::runtime_error("--seconds requires a value");
            }
            options.seconds = std::stod(argv[i]);
            if (!std::isfinite(options.seconds) || options.seconds <= 0.0 ||
                options.seconds > 3600.0) {
                throw std::runtime_error("--seconds must be between 0 and 3600");
            }
        } else {
            throw std::runtime_error("unknown argument: " + std::string(argument));
        }
    }
    if (options.gpu_calibrate && options.gpu_ms <= 0.0) {
        throw std::runtime_error("--gpu-calibrate requires --gpu-ms N with N above 0");
    }
    return options;
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

DXGI_FORMAT ResolveRenderTargetFormat(DXGI_FORMAT texture_format,
                                      int64_t requested_format) {
    if (!IsTypelessFormat(texture_format)) {
        return texture_format;
    }
    const DXGI_FORMAT requested = static_cast<DXGI_FORMAT>(requested_format);
    if (requested != DXGI_FORMAT_UNKNOWN && !IsTypelessFormat(requested)) {
        return requested;
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

void CreateSwapchainRenderTarget(ID3D11Device* device,
                                 ID3D11Texture2D* texture,
                                 int64_t requested_format,
                                 ID3D11RenderTargetView** render_target) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);

    D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
    view_desc.Format = ResolveRenderTargetFormat(
        texture_desc.Format, requested_format);
    if (texture_desc.SampleDesc.Count > 1 && texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        view_desc.Texture2DMSArray.FirstArraySlice = 0;
        view_desc.Texture2DMSArray.ArraySize = 1;
    } else if (texture_desc.SampleDesc.Count > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else if (texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view_desc.Texture2DArray.MipSlice = 0;
        view_desc.Texture2DArray.FirstArraySlice = 0;
        view_desc.Texture2DArray.ArraySize = 1;
    } else {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
    }

    const HRESULT result = device->CreateRenderTargetView(
        texture, &view_desc, render_target);
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << "CreateRenderTargetView failed with HRESULT 0x" << std::hex
               << static_cast<uint32_t>(result) << std::dec
               << "; requestedFormat=" << requested_format
               << ", texture={size=" << texture_desc.Width << "x" << texture_desc.Height
               << ", mips=" << texture_desc.MipLevels
               << ", array=" << texture_desc.ArraySize
               << ", format=" << static_cast<uint32_t>(texture_desc.Format)
               << ", samples=" << texture_desc.SampleDesc.Count
               << ", bindFlags=0x" << std::hex << texture_desc.BindFlags << std::dec
               << "}, viewFormat=" << static_cast<uint32_t>(view_desc.Format)
               << ", viewDimension=" << static_cast<uint32_t>(view_desc.ViewDimension);
        throw std::runtime_error(stream.str());
    }
}

// --gpu-ms: a compute dispatch of 1024 groups of 64 threads, each looping a dependent sequence `iterations` times, so
// its time grows linearly with the iterations and hardly with the GPU's width.
constexpr uint32_t kLoadGroups = 1024;
constexpr uint32_t kLoadThreads = kLoadGroups * 64;
constexpr uint32_t kLoadFirstIterations = 4096;
constexpr uint32_t kLoadMaximumIterations = 1u << 30;
constexpr int kLoadCalibrationRounds = 8;
constexpr int kLoadCalibrationRuns = 5;
constexpr double kLoadTolerance = 0.03;
constexpr int kLoadRecalibrationFrames = 30;
constexpr int kLoadCalibrationFrames = 200; // --gpu-calibrate

// The value at `fraction` of `sorted` (as the probe's other percentiles); 0 without samples.
double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    return sorted[std::min(sorted.size() - 1,
                           static_cast<std::size_t>(static_cast<double>(sorted.size()) * fraction))];
}

uint32_t ScaleIterations(uint32_t iterations, double factor) {
    const double scaled = std::round(static_cast<double>(iterations) * factor);
    if (!std::isfinite(scaled)) {
        return iterations;
    }
    return static_cast<uint32_t>(std::clamp(scaled, 1.0, static_cast<double>(kLoadMaximumIterations)));
}

// Calibration's next scale, bounded so that a first measurement near the timer's resolution cannot ask for seconds of
// work in one dispatch.
double CalibrationFactor(double target_ms, double measured_ms) {
    return measured_ms > 0.0 ? std::clamp(target_ms / measured_ms, 1.0 / 64.0, 64.0) : 64.0;
}

// Recalibration while running: at most 10% a step, so one disturbed window cannot swing the load.
double RecalibrationFactor(double target_ms, double measured_ms) {
    return measured_ms > 0.0 ? std::clamp(target_ms / measured_ms, 0.9, 1.1) : 1.1;
}

// One view's share of a frame's iterations: none without a share, else at least one.
uint32_t ViewIterations(uint32_t iterations, double share) {
    if (!(share > 0.0)) {
        return 0;
    }
    return ScaleIterations(iterations, std::min(share, 1.0));
}

void Expect(bool condition, std::string_view what) {
    if (!condition) {
        throw std::runtime_error("self-test failed: " + std::string(what));
    }
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
    const Options load = parse({"--gpu-ms", "11.5", "--gpu-frames-ahead", "0", "--gpu-ms-fixed", "--quad-views"});
    Expect(load.gpu_ms == 11.5 && load.gpu_frames_ahead == 0 && load.gpu_ms_fixed && load.quad_views,
           "--gpu-ms, --gpu-frames-ahead and --gpu-ms-fixed parse");
    Expect(parse({"--gpu-ms", "0"}).gpu_ms == 0.0 && parse({"--gpu-ms", "100"}).gpu_ms == 100.0 &&
               parse({"--gpu-frames-ahead", "4"}).gpu_frames_ahead == 4,
           "GPU load range ends accepted");
    const Options calibrate = parse({"--gpu-calibrate", "--gpu-ms", "5"});
    Expect(calibrate.gpu_calibrate && calibrate.gpu_ms == 5.0, "--gpu-calibrate with --gpu-ms parses");
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

void RunSelfTest() {
    RunOptionSelfTest();
    RunGpuLoadSelfTest();
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
    if (!NearlyIdentity(error)) {
        throw std::runtime_error("pose composition round-trip failed");
    }
    if (ResolveRenderTargetFormat(
            DXGI_FORMAT_R8G8B8A8_TYPELESS,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) !=
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        throw std::runtime_error("typeless swapchain RTV format resolution failed");
    }
    if (ResolveRenderTargetFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM) !=
        DXGI_FORMAT_B8G8R8A8_UNORM) {
        throw std::runtime_error("typed swapchain RTV format preservation failed");
    }
    std::cout << "vectorxr_hardware_probe self-test passed\n";
}

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 color;
};

struct SceneConstants {
    XMFLOAT4X4 view_projection;
};

void CompileShader(const char* source,
                   const char* entry,
                   const char* profile,
                   ID3DBlob** output) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), "pivot_space_probe.hlsl", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, output, &errors);
    if (FAILED(result)) {
        const std::string detail = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                          errors->GetBufferSize())
            : std::string{};
        throw std::runtime_error("D3DCompile failed for " + std::string(entry) +
                                 ": " + detail);
    }
}

class DiagnosticRenderer {
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
        device_ = device;
        context_ = context;

        static constexpr char kShaderSource[] = R"(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

        ComPtr<ID3DBlob> vertex_blob;
        ComPtr<ID3DBlob> pixel_blob;
        CompileShader(kShaderSource, "VSMain", "vs_5_0", &vertex_blob);
        CompileShader(kShaderSource, "PSMain", "ps_5_0", &pixel_blob);
        ThrowIfFailed(device_->CreateVertexShader(
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          nullptr, &vertex_shader_),
                      "CreateVertexShader");
        ThrowIfFailed(device_->CreatePixelShader(
                          pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                          nullptr, &pixel_shader_),
                      "CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, position)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, color)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ThrowIfFailed(device_->CreateInputLayout(
                          elements, static_cast<UINT>(std::size(elements)),
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          &input_layout_),
                      "CreateInputLayout");

        const std::vector<Vertex> vertices = BuildScene();
        vertex_count_ = static_cast<UINT>(vertices.size());
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = vertices.data();
        ThrowIfFailed(device_->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer_),
                      "CreateBuffer(vertex)");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = sizeof(SceneConstants);
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device_->CreateBuffer(&constant_desc, nullptr, &constant_buffer_),
                      "CreateBuffer(constants)");

        D3D11_DEPTH_STENCIL_DESC depth_state_desc{};
        depth_state_desc.DepthEnable = TRUE;
        depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;
        ThrowIfFailed(device_->CreateDepthStencilState(&depth_state_desc, &depth_state_),
                      "CreateDepthStencilState");
    }

    void Render(ID3D11RenderTargetView* render_target,
                ID3D11DepthStencilView* depth_target,
                uint32_t width,
                uint32_t height,
                const XrPosef& view_pose,
                const XrFovf& fov,
                uint32_t eye) {
        const float clear_color[4] = {
            eye == 0 ? 0.015f : 0.025f,
            0.02f,
            eye == 0 ? 0.03f : 0.015f,
            1.0f,
        };
        context_->ClearRenderTargetView(render_target, clear_color);
        context_->ClearDepthStencilView(depth_target, D3D11_CLEAR_DEPTH, 1.0f, 0);

        const XMVECTOR orientation = XMVectorSet(
            view_pose.orientation.x, view_pose.orientation.y,
            view_pose.orientation.z, view_pose.orientation.w);
        const XMMATRIX world_from_view =
            XMMatrixRotationQuaternion(orientation) *
            XMMatrixTranslation(view_pose.position.x,
                                view_pose.position.y,
                                view_pose.position.z);
        const XMMATRIX view_from_world = XMMatrixInverse(nullptr, world_from_view);
        const float left = std::tan(fov.angleLeft) * kNearZ;
        const float right = std::tan(fov.angleRight) * kNearZ;
        const float bottom = std::tan(fov.angleDown) * kNearZ;
        const float top = std::tan(fov.angleUp) * kNearZ;
        const XMMATRIX projection = XMMatrixPerspectiveOffCenterRH(
            left, right, bottom, top, kNearZ, kFarZ);

        SceneConstants constants{};
        XMStoreFloat4x4(&constants.view_projection, view_from_world * projection);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(constant_buffer_.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map(constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constant_buffer_.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &render_target, depth_target);
        context_->OMSetDepthStencilState(depth_state_.Get(), 0);
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        ID3D11Buffer* constant_buffer = constant_buffer_.Get();
        context_->VSSetConstantBuffers(0, 1, &constant_buffer);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context_->Draw(vertex_count_, 0);
    }

private:
    static std::vector<Vertex> BuildScene() {
        std::vector<Vertex> vertices;
        const auto line = [&](XMFLOAT3 start, XMFLOAT3 end, XMFLOAT3 color) {
            vertices.push_back({start, color});
            vertices.push_back({end, color});
        };

        const XMFLOAT3 grid{0.18f, 0.22f, 0.27f};
        for (int x = -10; x <= 10; ++x) {
            line({static_cast<float>(x), -1.5f, -1.0f},
                 {static_cast<float>(x), -1.5f, -21.0f}, grid);
        }
        for (int z = 1; z <= 21; ++z) {
            line({-10.0f, -1.5f, -static_cast<float>(z)},
                 {10.0f, -1.5f, -static_cast<float>(z)}, grid);
        }

        const XMFLOAT3 origin{0.0f, -1.5f, -4.0f};
        line(origin, {2.5f, -1.5f, -4.0f}, {1.0f, 0.1f, 0.1f});
        line(origin, {0.0f, 1.0f, -4.0f}, {0.1f, 1.0f, 0.1f});
        line(origin, {0.0f, -1.5f, -7.0f}, {0.1f, 0.35f, 1.0f});

        const auto beacon = [&](float x, float z, XMFLOAT3 color) {
            line({x, -1.5f, z}, {x, 2.5f, z}, color);
            line({x - 0.4f, 2.1f, z}, {x + 0.4f, 2.1f, z}, color);
            line({x, 1.7f, z - 0.4f}, {x, 1.7f, z + 0.4f}, color);
        };
        beacon(-3.0f, -7.0f, {1.0f, 0.85f, 0.1f});
        beacon(4.0f, -12.0f, {1.0f, 0.1f, 0.85f});
        return vertices;
    }

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11DepthStencilState> depth_state_;
    UINT vertex_count_{0};
};

// --gpu-ms: GPU work that gives each of the probe's frames a game's GPU time. Calibrated with timestamp queries before
// the first frame. Each view's share is recorded right before the view renders, so the view's release (and anything a
// layer queues behind it) waits for it. An event query per frame holds the probe until its frame --gpu-frames-ahead
// before has run on the GPU, as a game's own frame queue would.
class GpuLoad {
public:
    GpuLoad(ID3D11Device* device, ID3D11DeviceContext* context, const Options& options)
        : device_(device),
          context_(context),
          target_ms_(options.gpu_ms),
          frames_ahead_(static_cast<uint64_t>(options.gpu_frames_ahead)),
          fixed_(options.gpu_ms_fixed) {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            throw std::runtime_error("--gpu-ms needs a Direct3D 11.0 GPU (compute shaders)");
        }
        static constexpr char kShaderSource[] = R"(
cbuffer LoadConstants : register(b0) {
    uint iterations;
};

RWStructuredBuffer<float> values : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float v = values[id.x];
    // The values carry over from dispatch to dispatch: start again long before they could overflow.
    if (!(abs(v) < 1.0e4)) {
        v = 1.0 + id.x * 1.0e-6;
    }
    [loop]
    for (uint i = 0; i < iterations; ++i) {
        v = mad(v, 1.000001, sin(v) * 1.0e-3);
    }
    values[id.x] = v;
}
)";
        ComPtr<ID3DBlob> blob;
        CompileShader(kShaderSource, "CSMain", "cs_5_0", &blob);
        ThrowIfFailed(device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                   &shader_),
                      "CreateComputeShader(GPU load)");

        std::vector<float> seeds(kLoadThreads);
        for (uint32_t i = 0; i < kLoadThreads; ++i) {
            seeds[i] = 1.0f + static_cast<float>(i) * 1.0e-6f;
        }
        D3D11_BUFFER_DESC values_desc{};
        values_desc.ByteWidth = kLoadThreads * sizeof(float);
        values_desc.Usage = D3D11_USAGE_DEFAULT;
        values_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        values_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        values_desc.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA values_data{};
        values_data.pSysMem = seeds.data();
        ThrowIfFailed(device_->CreateBuffer(&values_desc, &values_data, &values_), "CreateBuffer(GPU load)");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
        view_desc.Format = DXGI_FORMAT_UNKNOWN;
        view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view_desc.Buffer.NumElements = kLoadThreads;
        ThrowIfFailed(device_->CreateUnorderedAccessView(values_.Get(), &view_desc, &values_view_),
                      "CreateUnorderedAccessView(GPU load)");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = 16;
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device_->CreateBuffer(&constant_desc, nullptr, &constants_),
                      "CreateBuffer(GPU load constants)");

        for (Timer& timer : calibration_.views) {
            timer = CreateTimer();
        }
        for (Frame& frame : ring_) {
            for (Timer& timer : frame.views) {
                timer = CreateTimer();
            }
            frame.done = CreateQuery(D3D11_QUERY_EVENT);
        }
    }

    // The iterations that take --gpu-ms as frames record them, split evenly into `views` timed dispatches (a split
    // load measures a few percent above one dispatch of the same iterations): 30 warm-up dispatches, then rounds of
    // 5 timed frames, each scaled by target / median, until within 3% (at most 8 rounds).
    void Calibrate(uint32_t views) {
        views = std::clamp<uint32_t>(views, 1, kMaximumViews);
        iterations_ = kLoadFirstIterations;
        for (int i = 0; i < 30; ++i) {
            Dispatch(iterations_);
        }
        double measured = 0.0;
        for (int round = 0; round < kLoadCalibrationRounds; ++round) {
            std::vector<double> runs;
            for (int run = 0; run < kLoadCalibrationRuns; ++run) {
                calibration_.views_used = 0;
                for (uint32_t view = 0; view < views; ++view) {
                    RecordTimed(calibration_.views[calibration_.views_used++],
                                ViewIterations(iterations_, 1.0 / views));
                }
                if (const std::optional<double> ms = ReadFrame(calibration_)) {
                    runs.push_back(*ms);
                }
            }
            if (runs.empty()) {
                continue; // every run straddled a clock change
            }
            std::sort(runs.begin(), runs.end());
            measured = Percentile(runs, 0.5);
            if (std::abs(measured - target_ms_) <= kLoadTolerance * target_ms_ ||
                round + 1 == kLoadCalibrationRounds) {
                break;
            }
            iterations_ = ScaleIterations(iterations_, CalibrationFactor(target_ms_, measured));
        }
        calibrated_iterations_ = iterations_;
        std::ostringstream line;
        line << std::fixed << std::setprecision(2) << "GPU load: target " << target_ms_ << " ms = " << iterations_
             << " iterations in " << views << " views (measured " << measured << " ms)\n";
        std::cout << line.str();
    }

    // A frame begins: waits until the frame --gpu-frames-ahead before it has run on the GPU.
    void BeginFrame() {
        if (frames_ahead_ > 0 && frames_ >= frames_ahead_) {
            WaitForFrame(frames_ - frames_ahead_);
        }
        Collect(false);
        // Frames finish in order, so every frame from the oldest uncollected one on is still queued.
        max_queued_ = std::max(max_queued_, frames_ - collected_);
    }

    // One view's share of this frame's load (its pixels / all views' pixels), right before the view renders.
    void DispatchView(double share) {
        Frame& frame = ring_[frames_ % kRing];
        if (!open_) {
            while (frames_ - collected_ >= kRing) {
                CollectOldest(true);
            }
            frame.views_used = 0;
            frame.iterations = iterations_;
            open_ = true;
        }
        const uint32_t iterations = ViewIterations(frame.iterations, share);
        if (iterations == 0 || frame.views_used >= frame.views.size()) {
            return;
        }
        RecordTimed(frame.views[frame.views_used++], iterations);
    }

    // The frame's last view was recorded; with --gpu-frames-ahead 0 the frame waits for its own GPU work here.
    void EndFrame() {
        if (!open_) {
            return;
        }
        context_->End(ring_[frames_ % kRing].done.Get());
        open_ = false;
        ++frames_;
        if (frames_ahead_ == 0) {
            WaitForFrame(frames_ - 1);
        }
    }

    // Collects every recorded frame's timings (waits for the GPU).
    void Drain() {
        Collect(true);
    }

    // Whether no frame began with more earlier frames queued on the GPU than --gpu-frames-ahead allows.
    bool QueueBounded() const {
        return max_queued_ <= (frames_ahead_ > 0 ? frames_ahead_ - 1 : 0);
    }

    double MeanMs() const {
        double sum = 0.0;
        for (const double ms : frame_ms_) {
            sum += ms;
        }
        return frame_ms_.empty() ? 0.0 : sum / static_cast<double>(frame_ms_.size());
    }

    void Report(std::ostream& out) const {
        std::vector<double> sorted = frame_ms_;
        std::sort(sorted.begin(), sorted.end());
        std::ostringstream line;
        line << std::fixed << std::setprecision(2) << "GPU load: target " << target_ms_ << " ms, mean " << MeanMs()
             << " ms, p50 " << Percentile(sorted, 0.5) << " ms, p99 " << Percentile(sorted, 0.99) << " ms ("
             << sorted.size() << " frames timed";
        if (disjoint_frames_ > 0) {
            line << ", " << disjoint_frames_ << " straddled a clock change";
        }
        line << "), iterations " << iterations_ << " (calibrated " << calibrated_iterations_ << ", "
             << (fixed_ ? std::string("fixed") : std::to_string(adjustments_) + " adjustments") << ")\n"
             << "GPU frames ahead " << frames_ahead_ << ": at most " << max_queued_
             << " earlier frames queued when a frame began, " << stalls_ << " stalls of " << frames_ << " frames";
        if (stalls_ > 0) {
            line << ", mean " << stall_ms_ / static_cast<double>(stalls_) << " ms, max " << stall_max_ms_
                 << " ms, total " << stall_ms_ << " ms";
        }
        line << "\n";
        out << line.str();
    }

private:
    static constexpr std::size_t kRing = 8;
    static_assert(kRing > 4 + 1, "the ring must hold more frames than --gpu-frames-ahead lets queue");

    struct Timer {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> start;
        ComPtr<ID3D11Query> end;
    };

    struct Frame {
        std::array<Timer, kMaximumViews> views;
        ComPtr<ID3D11Query> done; // event: the frame's GPU work has run
        uint32_t views_used{0};
        uint32_t iterations{0}; // the frame's total, before its views' shares
    };

    ComPtr<ID3D11Query> CreateQuery(D3D11_QUERY type) {
        D3D11_QUERY_DESC desc{};
        desc.Query = type;
        ComPtr<ID3D11Query> query;
        ThrowIfFailed(device_->CreateQuery(&desc, &query), "CreateQuery(GPU load)");
        return query;
    }

    Timer CreateTimer() {
        return {CreateQuery(D3D11_QUERY_TIMESTAMP_DISJOINT), CreateQuery(D3D11_QUERY_TIMESTAMP),
                CreateQuery(D3D11_QUERY_TIMESTAMP)};
    }

    void Dispatch(uint32_t iterations) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map(GPU load constants)");
        const std::array<uint32_t, 4> constants{iterations, 0, 0, 0};
        std::memcpy(mapped.pData, constants.data(), sizeof(constants));
        context_->Unmap(constants_.Get(), 0);
        ID3D11Buffer* constant_buffer = constants_.Get();
        ID3D11UnorderedAccessView* view = values_view_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &constant_buffer);
        context_->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
        context_->Dispatch(kLoadGroups, 1, 1);
        // No compute state is left behind for the frame's rendering or a layer's work.
        ID3D11UnorderedAccessView* no_view = nullptr;
        ID3D11Buffer* no_buffer = nullptr;
        context_->CSSetUnorderedAccessViews(0, 1, &no_view, nullptr);
        context_->CSSetConstantBuffers(0, 1, &no_buffer);
        context_->CSSetShader(nullptr, nullptr, 0);
    }

    // Whether `query` has its result (written to `data` when given). Only the first poll of a wait flushes.
    bool Poll(ID3D11Query* query, void* data, UINT size, bool flush) {
        const HRESULT result =
            context_->GetData(query, data, size, flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        ThrowIfFailed(result, "ID3D11DeviceContext::GetData(GPU load)");
        return result == S_OK;
    }

    void Wait(ID3D11Query* query, void* data, UINT size) {
        if (Poll(query, data, size, true)) {
            return;
        }
        while (!Poll(query, data, size, false)) {
            std::this_thread::yield();
        }
    }

    // A timer's span in ms; nullopt when it straddled a clock change.
    std::optional<double> Read(const Timer& timer) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0;
        UINT64 end = 0;
        Wait(timer.disjoint.Get(), &disjoint, sizeof(disjoint));
        Wait(timer.start.Get(), &start, sizeof(start));
        Wait(timer.end.Get(), &end, sizeof(end));
        if (disjoint.Disjoint || disjoint.Frequency == 0 || end < start) {
            return std::nullopt;
        }
        return static_cast<double>(end - start) * 1000.0 / static_cast<double>(disjoint.Frequency);
    }

    // A frame's load: the sum of its views' timers; nullopt when one straddled a clock change.
    std::optional<double> ReadFrame(const Frame& frame) {
        double ms = 0.0;
        for (uint32_t view = 0; view < frame.views_used; ++view) {
            const std::optional<double> view_ms = Read(frame.views[view]);
            if (!view_ms) {
                return std::nullopt;
            }
            ms += *view_ms;
        }
        return ms;
    }

    void RecordTimed(const Timer& timer, uint32_t iterations) {
        context_->Begin(timer.disjoint.Get());
        context_->End(timer.start.Get());
        Dispatch(iterations);
        context_->End(timer.end.Get());
        context_->End(timer.disjoint.Get());
    }

    // Blocks until frame `index` has run on the GPU; a wait that blocks is a stall.
    void WaitForFrame(uint64_t index) {
        if (index < collected_) {
            return; // collected: it has run
        }
        ID3D11Query* done = ring_[index % kRing].done.Get();
        if (Poll(done, nullptr, 0, true)) {
            return;
        }
        const auto begun = std::chrono::steady_clock::now();
        while (!Poll(done, nullptr, 0, false)) {
            std::this_thread::yield();
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
        ++stalls_;
        stall_ms_ += ms;
        stall_max_ms_ = std::max(stall_max_ms_, ms);
    }

    void Collect(bool wait) {
        while (collected_ < frames_ && CollectOldest(wait)) {
        }
    }

    // The oldest uncollected frame's timing, once it has run; recalibrates every 30 frames unless --gpu-ms-fixed.
    bool CollectOldest(bool wait) {
        const Frame& frame = ring_[collected_ % kRing];
        if (wait) {
            Wait(frame.done.Get(), nullptr, 0);
        } else if (!Poll(frame.done.Get(), nullptr, 0, false)) {
            return false;
        }
        ++collected_;
        if (frame.views_used == 0) {
            return true;
        }
        const std::optional<double> ms = ReadFrame(frame);
        if (!ms) {
            ++disjoint_frames_;
            return true;
        }
        frame_ms_.push_back(*ms);
        // Only frames recorded at the current iterations judge them (older ones may still be in flight).
        if (!fixed_ && frame.iterations == iterations_) {
            window_ms_ += *ms;
            if (++window_frames_ >= kLoadRecalibrationFrames) {
                iterations_ = ScaleIterations(
                    iterations_, RecalibrationFactor(target_ms_, window_ms_ / static_cast<double>(window_frames_)));
                ++adjustments_;
                window_ms_ = 0.0;
                window_frames_ = 0;
            }
        }
        return true;
    }

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    double target_ms_{0.0};
    uint64_t frames_ahead_{0};
    bool fixed_{false};
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> values_;
    ComPtr<ID3D11UnorderedAccessView> values_view_;
    ComPtr<ID3D11Buffer> constants_;
    Frame calibration_; // its views' timers only
    std::array<Frame, kRing> ring_;
    uint64_t frames_{0};    // frames recorded
    uint64_t collected_{0}; // frames whose timings were read, oldest first
    bool open_{false};      // the current frame has recorded a view
    uint32_t iterations_{kLoadFirstIterations};
    uint32_t calibrated_iterations_{0};
    uint64_t adjustments_{0};
    double window_ms_{0.0};
    int window_frames_{0};
    std::vector<double> frame_ms_;
    uint64_t disjoint_frames_{0};
    uint64_t stalls_{0};
    double stall_ms_{0.0};
    double stall_max_ms_{0.0};
    uint64_t max_queued_{0};
};

// The Direct3D 11 debug layer's errors and warnings over the run (the first
// few of them, and how many).
void ReportD3D11Messages(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        std::cout << "Direct3D 11 debug layer: unavailable\n";
        return;
    }
    uint64_t errors = 0;
    uint64_t warnings = 0;
    int shown = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size))) continue;
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            ++errors;
        } else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            ++warnings;
        } else {
            continue;
        }
        if (shown++ < 10) {
            std::cout << "Direct3D 11 debug layer [" << message->ID << "]: " << message->pDescription << "\n";
        }
    }
    std::cout << "Direct3D 11 debug layer: " << errors << " errors, " << warnings << " warnings (of "
              << queue->GetNumStoredMessages() << " stored, " << queue->GetNumMessagesDiscardedByMessageCountLimit()
              << " discarded)\n";
}

// --gpu-calibrate: the GPU load alone, without OpenXR, on the default hardware adapter. Its frames are paced as a
// 90 Hz runtime would (or at --frame-ms when longer) and split into two views (four with --quad-views). True when
// their mean load is within 3% of --gpu-ms and no frame began with more frames queued than --gpu-frames-ahead allows.
bool RunGpuCalibration(const Options& options) {
    const std::array<D3D_FEATURE_LEVEL, 4> feature_levels{
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ThrowIfFailed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    options.d3d11_debug ? D3D11_CREATE_DEVICE_DEBUG : 0, feature_levels.data(),
                                    static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &device, nullptr,
                                    &context),
                  "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&description))) {
        std::wcout << L"Graphics adapter: " << description.Description << L"\n";
    }

    GpuLoad load(device.Get(), context.Get(), options);
    const uint32_t views = options.quad_views ? 4u : 2u;
    load.Calibrate(views);
    const double period_ms = std::max(options.frame_ms, 1000.0 / 90.0);
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double, std::milli>(period_ms));
    const auto started = std::chrono::steady_clock::now();
    auto next = started;
    for (int frame = 0; frame < kLoadCalibrationFrames; ++frame) {
        load.BeginFrame();
        for (uint32_t view = 0; view < views; ++view) {
            load.DispatchView(1.0 / views);
        }
        load.EndFrame();
        context->Flush(); // as a frame's release would
        // A late frame (the GPU holding it back) starts the next one at once, without catching up.
        next = std::max(next + period, std::chrono::steady_clock::now());
        while (std::chrono::steady_clock::now() < next) {
            if (next - std::chrono::steady_clock::now() > std::chrono::milliseconds(2)) {
                Sleep(1);
            } else {
                std::this_thread::yield();
            }
        }
    }
    load.Drain();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    load.Report(std::cout);
    const double error = (load.MeanMs() - options.gpu_ms) / options.gpu_ms;
    const bool within = std::abs(error) <= kLoadTolerance;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2) << "Frames: " << kLoadCalibrationFrames << " in " << seconds << " s ("
         << kLoadCalibrationFrames / seconds << " fps, paced at " << 1000.0 / period_ms << " Hz, " << views
         << " views)\n"
         << "GPU load check: the mean is " << std::showpos << error * 100.0 << std::noshowpos << "% off the target ("
         << (within ? "within" : "outside") << " 3%); the frame queue "
         << (load.QueueBounded() ? "stayed within" : "exceeded") << " --gpu-frames-ahead\n";
    std::cout << line.str();
    if (options.d3d11_debug) {
        ReportD3D11Messages(device.Get());
    }
    return within && load.QueueBounded();
}

struct EyeSwapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    uint32_t width{0};
    uint32_t height{0};
    int64_t format{0};
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ComPtr<ID3D11RenderTargetView>> render_targets;
    ComPtr<ID3D11Texture2D> depth_texture;
    ComPtr<ID3D11DepthStencilView> depth_target;
    // --depth: a depth swapchain, one view per image, instead of depth_texture.
    XrSwapchain depth_handle{XR_NULL_HANDLE};
    std::vector<ComPtr<ID3D11DepthStencilView>> depth_targets;
};

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
                                                 : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {}

    ~ProbeApp() {
        DestroySwapchains();
        gpu_load_.reset();
        renderer_.reset();
        context1_.Reset();
        context_.Reset();
        device_.Reset();
        DestroySession();
        if (instance_ != XR_NULL_HANDLE) {
            xrDestroyInstance(instance_);
        }
    }

    void DestroySwapchains() {
        for (EyeSwapchain& swapchain : swapchains_) {
            swapchain.depth_target.Reset();
            swapchain.depth_texture.Reset();
            swapchain.depth_targets.clear();
            swapchain.render_targets.clear();
            swapchain.images.clear();
            if (swapchain.handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.handle);
            }
            if (swapchain.depth_handle != XR_NULL_HANDLE) {
                xrDestroySwapchain(swapchain.depth_handle);
            }
        }
        swapchains_.clear();
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

            std::cout << "Probe active: mode=" << ModeName(options_.mode)
                      << ", threadedWait=" << (options_.threaded_wait ? "yes" : "no")
                      << ", views=" << swapchains_.size()
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
        if (options_.d3d11_debug) {
            ReportD3D11Messages(device_.Get());
        }
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
        if (!offered(XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
            throw std::runtime_error("active OpenXR runtime does not expose XR_KHR_D3D11_enable");
        }

        std::vector<const char*> enabled_extensions{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
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

        PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
        CheckXr(instance_, xrGetInstanceProcAddr(
                               instance_, "xrGetD3D11GraphicsRequirementsKHR",
                               reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
                "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CheckXr(instance_, get_requirements(instance_, system_id_, &requirements),
                "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIFactory1> factory;
        ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selected_adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            ThrowIfFailed(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
            if (description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart) {
                selected_adapter = candidate;
                std::wcout << L"Graphics adapter: " << description.Description << L"\n";
                break;
            }
        }
        if (!selected_adapter) {
            throw std::runtime_error("could not find the D3D11 adapter requested by the runtime");
        }

        const std::array<D3D_FEATURE_LEVEL, 7> feature_levels{
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };
        D3D_FEATURE_LEVEL created_level{};
        ThrowIfFailed(D3D11CreateDevice(
                          selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                              (options_.d3d11_debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
                          feature_levels.data(), static_cast<UINT>(feature_levels.size()),
                          D3D11_SDK_VERSION, &device_, &created_level, &context_),
                      "D3D11CreateDevice");
        if (created_level < requirements.minFeatureLevel) {
            throw std::runtime_error("created D3D11 device does not meet the runtime feature-level requirement");
        }
        if (options_.gpu_ms > 0.0) {
            gpu_load_ = std::make_unique<GpuLoad>(device_.Get(), context_.Get(), options_);
            gpu_load_->Calibrate(options_.quad_views ? 4u : 2u);
        }
    }

    void CreateSession() {
        XrGraphicsBindingD3D11KHR graphics_binding{
            XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        graphics_binding.device = device_.Get();
        XrSessionCreateInfo create_info{XR_TYPE_SESSION_CREATE_INFO};
        create_info.next = &graphics_binding;
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
        const std::array<int64_t, 4> preferences{
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM,
        };
        const auto preferred = std::find_if(
            preferences.begin(), preferences.end(), [&](int64_t candidate) {
                return std::find(formats.begin(), formats.end(), candidate) != formats.end();
            });
        if (preferred == preferences.end()) {
            throw std::runtime_error("runtime exposes no supported RGBA/BGRA render-target format");
        }

        swapchains_.resize(view_count);
        for (uint32_t eye = 0; eye < view_count; ++eye) {
            EyeSwapchain& swapchain = swapchains_[eye];
            const uint32_t limit = options_.full_size ? UINT32_MAX : kMaximumEyeDimension;
            swapchain.width = std::min(view_configuration_views_[eye].recommendedImageRectWidth, limit);
            swapchain.height = std::min(view_configuration_views_[eye].recommendedImageRectHeight, limit);
            if (swapchain.width == 0 || swapchain.height == 0) {
                throw std::runtime_error("runtime returned a zero recommended view dimension");
            }
            swapchain.format = *preferred;

            XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                     (options_.quad_views ? XR_SWAPCHAIN_USAGE_SAMPLED_BIT : 0);
            create_info.format = swapchain.format;
            create_info.sampleCount = 1;
            create_info.width = swapchain.width;
            create_info.height = swapchain.height;
            create_info.faceCount = 1;
            create_info.arraySize = 1;
            create_info.mipCount = 1;
            CheckXr(instance_, xrCreateSwapchain(session_, &create_info, &swapchain.handle),
                    "xrCreateSwapchain");

            uint32_t image_count = 0;
            CheckXr(instance_, xrEnumerateSwapchainImages(
                                   swapchain.handle, 0, &image_count, nullptr),
                    "xrEnumerateSwapchainImages(count)");
            swapchain.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            CheckXr(instance_, xrEnumerateSwapchainImages(
                                   swapchain.handle, image_count, &image_count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(
                                       swapchain.images.data())),
                    "xrEnumerateSwapchainImages");
            swapchain.render_targets.resize(image_count);
            for (uint32_t image = 0; image < image_count; ++image) {
                CreateSwapchainRenderTarget(
                    device_.Get(), swapchain.images[image].texture,
                    swapchain.format, &swapchain.render_targets[image]);
            }

            if (options_.depth) {
                if (std::find(formats.begin(), formats.end(), DXGI_FORMAT_D32_FLOAT) == formats.end()) {
                    throw std::runtime_error("the runtime offers no D32_FLOAT swapchains");
                }
                XrSwapchainCreateInfo depth_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                depth_info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                depth_info.format = DXGI_FORMAT_D32_FLOAT;
                depth_info.sampleCount = 1;
                depth_info.width = swapchain.width;
                depth_info.height = swapchain.height;
                depth_info.faceCount = 1;
                depth_info.arraySize = 1;
                depth_info.mipCount = 1;
                CheckXr(instance_, xrCreateSwapchain(session_, &depth_info, &swapchain.depth_handle),
                        "xrCreateSwapchain(depth)");
                uint32_t depth_count = 0;
                CheckXr(instance_, xrEnumerateSwapchainImages(swapchain.depth_handle, 0, &depth_count, nullptr),
                        "xrEnumerateSwapchainImages(depth count)");
                std::vector<XrSwapchainImageD3D11KHR> depth_images(depth_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
                CheckXr(instance_,
                        xrEnumerateSwapchainImages(swapchain.depth_handle, depth_count, &depth_count,
                                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(depth_images.data())),
                        "xrEnumerateSwapchainImages(depth)");
                for (const XrSwapchainImageD3D11KHR& image : depth_images) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_D32_FLOAT;
                    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    ComPtr<ID3D11DepthStencilView> target;
                    ThrowIfFailed(device_->CreateDepthStencilView(image.texture, &view, &target),
                                  "CreateDepthStencilView(depth swapchain)");
                    swapchain.depth_targets.push_back(target);
                }
                continue;
            }

            D3D11_TEXTURE2D_DESC depth_desc{};
            depth_desc.Width = swapchain.width;
            depth_desc.Height = swapchain.height;
            depth_desc.MipLevels = 1;
            depth_desc.ArraySize = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Usage = D3D11_USAGE_DEFAULT;
            depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ThrowIfFailed(device_->CreateTexture2D(
                              &depth_desc, nullptr, &swapchain.depth_texture),
                          "CreateTexture2D(depth)");
            ThrowIfFailed(device_->CreateDepthStencilView(
                              swapchain.depth_texture.Get(), nullptr,
                              &swapchain.depth_target),
                          "CreateDepthStencilView");
        }

        renderer_ = std::make_unique<DiagnosticRenderer>();
        renderer_->Initialize(device_.Get(), context_.Get());
        std::cout << "Probe swapchains:";
        for (const EyeSwapchain& swapchain : swapchains_) {
            std::cout << " " << swapchain.width << "x" << swapchain.height;
        }
        std::cout << " (format " << swapchains_[0].format << ", "
                  << swapchains_[0].images.size() << " images each)\n";
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

    void RenderEye(uint32_t eye, const XrView& view) {
        EyeSwapchain& swapchain = swapchains_[eye];
        uint32_t image_index = 0;
        const XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        auto call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrAcquireSwapchainImage(
                               swapchain.handle, &acquire_info, &image_index),
                "xrAcquireSwapchainImage");
        NoteCall(0, call_start);
        const XrSwapchainImageWaitInfo wait_info{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrWaitSwapchainImage(swapchain.handle, &wait_info),
                "xrWaitSwapchainImage");
        NoteCall(1, call_start);
        ID3D11DepthStencilView* depth_target = swapchain.depth_target.Get();
        if (swapchain.depth_handle != XR_NULL_HANDLE) {
            uint32_t depth_index = 0;
            CheckXr(instance_, xrAcquireSwapchainImage(swapchain.depth_handle, &acquire_info, &depth_index),
                    "xrAcquireSwapchainImage(depth)");
            CheckXr(instance_, xrWaitSwapchainImage(swapchain.depth_handle, &wait_info),
                    "xrWaitSwapchainImage(depth)");
            depth_target = swapchain.depth_targets[depth_index].Get();
        }
        renderer_->Render(swapchain.render_targets[image_index].Get(),
                          depth_target,
                          swapchain.width, swapchain.height,
                          view.pose, view.fov, eye % 2);
        if (options_.pattern) {
            DrawPattern(swapchain.render_targets[image_index].Get(), swapchain.width, swapchain.height, eye);
        }
        const XrSwapchainImageReleaseInfo release_info{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        call_start = std::chrono::steady_clock::now();
        CheckXr(instance_, xrReleaseSwapchainImage(
                               swapchain.handle, &release_info),
                "xrReleaseSwapchainImage");
        NoteCall(2, call_start);
        if (swapchain.depth_handle != XR_NULL_HANDLE) {
            CheckXr(instance_, xrReleaseSwapchainImage(swapchain.depth_handle, &release_info),
                    "xrReleaseSwapchainImage(depth)");
        }
    }

    // A grid of squares, a frame and a bar sweeping across once every four
    // seconds, in the view's own pixels: peripheral views white and green,
    // focus views yellow and magenta.
    void DrawPattern(ID3D11RenderTargetView* target, uint32_t width, uint32_t height, uint32_t view) {
        if (!context1_) {
            ThrowIfFailed(context_.As(&context1_), "ID3D11DeviceContext1");
        }
        const bool focus = view >= 2;
        const float bar[4] = {1.0f, 1.0f, focus ? 0.0f : 1.0f, 1.0f};
        const float grid[4] = {focus ? 0.9f : 0.1f, focus ? 0.1f : 0.8f, focus ? 0.9f : 0.2f, 1.0f};
        const LONG w = static_cast<LONG>(width);
        const LONG h = static_cast<LONG>(height);
        std::vector<D3D11_RECT> squares;
        const LONG square = std::max<LONG>(4, w / 40);
        for (int y = 1; y < 8; ++y) {
            for (int x = 1; x < 8; ++x) {
                const LONG left = w * x / 8 - square / 2;
                const LONG top = h * y / 8 - square / 2;
                squares.push_back({left, top, left + square, top + square});
            }
        }
        const LONG edge = std::max<LONG>(2, w / 200);
        squares.push_back({0, 0, w, edge});
        squares.push_back({0, h - edge, w, h});
        squares.push_back({0, 0, edge, h});
        squares.push_back({w - edge, 0, w, h});
        context1_->ClearView(target, grid, squares.data(), static_cast<UINT>(squares.size()));
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const LONG bar_width = std::max<LONG>(4, w / 24);
        const LONG bar_left = static_cast<LONG>(std::fmod(seconds / 4.0, 1.0) * static_cast<double>(w - bar_width));
        const D3D11_RECT bar_rect{bar_left, 0, bar_left + bar_width, h};
        context1_->ClearView(target, bar, &bar_rect, 1);
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
                    for (const EyeSwapchain& swapchain : swapchains_) {
                        total_pixels += static_cast<double>(swapchain.width) * swapchain.height;
                    }
                    for (uint32_t eye = 0; eye < ViewCount(); ++eye) {
                        if (gpu_load_) {
                            gpu_load_->DispatchView(
                                static_cast<double>(swapchains_[eye].width) * swapchains_[eye].height / total_pixels);
                        }
                        RenderEye(eye, source_views[eye]);
                        projection_views[eye].pose =
                            source_in_target.has_value()
                                ? ComposePose(*source_in_target, source_views[eye].pose)
                                : source_views[eye].pose;
                        projection_views[eye].fov = source_views[eye].fov;
                        projection_views[eye].subImage.swapchain =
                            swapchains_[eye].handle;
                        projection_views[eye].subImage.imageRect = {
                            {0, 0},
                            {static_cast<int32_t>(swapchains_[eye].width),
                             static_cast<int32_t>(swapchains_[eye].height)},
                        };
                        projection_views[eye].subImage.imageArrayIndex = 0;
                        if (swapchains_[eye].depth_handle != XR_NULL_HANDLE) {
                            XrCompositionLayerDepthInfoKHR& depth = depth_infos[eye];
                            depth.subImage = projection_views[eye].subImage;
                            depth.subImage.swapchain = swapchains_[eye].depth_handle;
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
            if (gpu_load_) {
                gpu_load_->EndFrame();
            }

            XrCompositionLayerProjection projection{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = projection_space;
            projection.viewCount = ViewCount();
            projection.views = projection_views.data();
            const XrCompositionLayerBaseHeader* layers[] = {
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};

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
            end_info.layerCount = submit_projection ? 1u : 0u;
            end_info.layers = submit_projection ? layers : nullptr;
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

    std::optional<std::chrono::steady_clock::time_point> last_frame_started_;
    std::vector<double> frame_ms_;
    uint64_t recreations_{0};
    uint64_t hitches_{0};
    std::array<std::vector<double>, 4> call_ms_;
    int slow_calls_reported_{0};

    uint32_t ViewCount() const {
        return static_cast<uint32_t>(swapchains_.size());
    }

    Options options_;
    XrViewConfigurationType view_configuration_;
    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId system_id_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace source_space_{XR_NULL_HANDLE};
    XrSpace target_space_{XR_NULL_HANDLE};
    XrSessionState session_state_{XR_SESSION_STATE_UNKNOWN};
    XrEnvironmentBlendMode blend_mode_{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    bool session_running_{false};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    std::vector<XrViewConfigurationView> view_configuration_views_;
    std::vector<EyeSwapchain> swapchains_;
    std::unique_ptr<DiagnosticRenderer> renderer_;
    std::unique_ptr<GpuLoad> gpu_load_;
    uint64_t rendered_frames_{0};
    uint64_t invalid_view_frames_{0};
    uint64_t space_relation_failures_{0};
};

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = ParseOptions(argc, argv);
        if (options.help) {
            PrintUsage();
            return 0;
        }
        if (options.self_test) {
            RunSelfTest();
            return 0;
        }
        if (options.gpu_calibrate) {
            timeBeginPeriod(1);
            const bool within = RunGpuCalibration(options);
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
