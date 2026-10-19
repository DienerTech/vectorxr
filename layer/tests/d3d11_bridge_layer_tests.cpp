// Frame Generation's Direct3D 11 bridge through the layer itself: the real
// hooks, from xrCreateApiLayerInstance on, against a stand-in runtime whose
// swapchain images are Direct3D 12 textures on the device of the binding it
// is given, with a real Direct3D 11 game (WARP; `hw`: the first hardware
// adapter, or the one FG_TEST_ADAPTER names, test_adapter.h; the layer takes
// the game's adapter, as in a session). The stand-in reads back what each
// frame shows on the session's queue, after the images' releases there, as a
// compositor reads what it shows. Frame Generation runs on a stand-in
// backend: it owns frame pacing, as with a real one, and does no GPU work. The host is left alone: settings,
// logs and the layer's other files go to a temporary directory. The
// Direct3D 12 debug layer checks the GPU work (and the Direct3D 11 one when
// installed), after every case's teardown; without either, a warning says so
// and the behavioural checks stand alone. Quad views (XR_VARJO_quad_views)
// run on a stereo stand-in whose waits are paced like a runtime's: the layer
// composes the four views into stereo. The device's removal while Frame
// Generation composes them runs in a process of its own, on hardware only
// (`hw`). Exit code 77: this machine shares nothing between the APIs, or
// FG_TEST_ADAPTER names no adapter.
#include <windows.h>
#include <process.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/framegen_plan.h"
#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"
#include "depthxr/quadviews_sizing.h"
#include "test_adapter.h"
#include "test_gates.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The layer's loader entry point (layer_entry.cpp).
extern "C" XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                        const depthxr::XrApiLayerCreateInfo* layer_info,
                                                        XrInstance* instance);

namespace depthxr {

// Test-only access to the singleton (DEPTHXR_TESTING), for what an
// application cannot reach through OpenXR.
class D3D11BridgeLayerTestPeer {
  public:
    // Frame Generation starts sessions with `factory`'s backend.
    static void UseFrameGenGpu(FrameGenPresenter::GpuFactory factory) {
        OpenXrLayer::Instance().framegen_gpu_factory_ = std::move(factory);
    }
    static D3D11Bridge& Bridge() {
        return OpenXrLayer::Instance().d3d11_bridge_;
    }
    static bool QueueSubstituted() {
        return OpenXrLayer::Instance().framegen_queue_.Active();
    }
    // What keeping shown layers lazy did in the session so far.
    static FrameGenQueueSubstitution::ShownLayerCounts ShownLayers() {
        return OpenXrLayer::Instance().framegen_queue_.ShownLayers();
    }
    static bool FrameGenOwnsPacing() {
        return OpenXrLayer::Instance().framegen_owns_pacing_.load();
    }
    // Nothing of an instance, a session, the bridge or Frame Generation is left.
    static bool Idle() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.instance_ == XR_NULL_HANDLE && layer.active_session_ == XR_NULL_HANDLE &&
               !layer.d3d11_bridge_.Active() && !layer.framegen_queue_.Active() && !layer.framegen_.Started();
    }
    // The layer's log so far: an error reaches the file at once, with
    // everything logged before it.
    static std::string Log() {
        static int reads = 0;
        OpenXrLayer& layer = OpenXrLayer::Instance();
        layer.logger_.Error("D3D11 bridge layer tests: log read " + std::to_string(++reads) + ".");
        std::ifstream stream(layer.logger_.ActiveLogPath(), std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }
    static std::filesystem::path LogPath() {
        return OpenXrLayer::Instance().logger_.ActiveLogPath();
    }
    // Lets the log file go (nothing is logged after the last instance).
    static void ReleaseLog(const std::filesystem::path& path) {
        OpenXrLayer::Instance().logger_.Initialize(path);
    }
    // Frame Generation engages after `steady_frames` steady frames once its
    // session is `minimum_age` old (kept across sessions).
    static void SetFrameGenEngagement(std::uint32_t steady_frames, std::chrono::milliseconds minimum_age) {
        OpenXrLayer::Instance().framegen_.SetEngagementPolicy(steady_frames, minimum_age);
    }
    // Frame Generation's hotkey: whether it is on afterwards.
    static bool ToggleFrameGen() {
        return OpenXrLayer::Instance().framegen_.Toggle();
    }
    // Frame Generation's state and counters (until its session ends).
    static FrameGenStatus FrameGen() {
        return OpenXrLayer::Instance().framegen_.Status();
    }
    // Quad views: the Direct3D 11 compositor's output swapchains (the runtime's).
    static std::array<XrSwapchain, 2> CompositorOutputs() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return {layer.d3d11_quadviews_compositor_.targets[0].swapchain,
                layer.d3d11_quadviews_compositor_.targets[1].swapchain};
    }
    // The bridged session's quad views frames so far: composed by the
    // Direct3D 11 compositor, and split into layers.
    static std::pair<std::uint64_t, std::uint64_t> QuadFrames() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return {layer.d3d11_bridge_quad_frames_.d3d11, layer.d3d11_bridge_quad_frames_.split};
    }
    // The quad views diagnostic overlay, as its binding or the app would set it.
    static void SetQuadViewsDiagnosticVisualization(bool shown) {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        layer.quadviews_diagnostic_visualization_enabled_ = shown;
    }
    static bool QuadViewsDiagnosticVisualization() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.quadviews_diagnostic_visualization_enabled_;
    }
    // Whether the app's live control can show the overlay in the session.
    static bool QuadViewsDiagnosticVisualizationAvailable() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.QuadViewsDiagnosticVisualizationAvailable();
    }
    // The overlay's binding (CaseSettings::diagnostic_binding) held down, or
    // let go, as its polls in xrEndFrame read it from now on, without reading
    // the keyboard; none: they read the keyboard again, as at a session's
    // start (the first poll only notes whether it is down).
    static void HoldQuadViewsDiagnosticBinding(std::optional<bool> down) {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        if (!down) {
            layer.quadviews_diagnostic_visualization_binding_last_poll_time_.reset();
            return;
        }
        layer.quadviews_diagnostic_visualization_binding_down_cached_ = *down;
        layer.quadviews_diagnostic_visualization_binding_last_poll_time_ =
            std::chrono::steady_clock::now() + std::chrono::hours(1);
    }
    // The bridged session's quad views frames Frame Generation composed so far.
    static std::uint64_t QuadFramesComposedByFrameGen() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.d3d11_bridge_quad_frames_.fg;
    }
    // How long the layer composes quad views after Frame Generation's writes
    // kept failing (kept across sessions).
    static void SetFrameGenQuadDecline(std::chrono::milliseconds first, std::chrono::milliseconds longest) {
        OpenXrLayer::Instance().framegen_.SetQuadDecline(first, longest);
    }
    // The bridge's 30 s summary is logged at the next xrEndFrame.
    static void ExpireBridgeSummary() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        layer.d3d11_bridge_last_summary_ = std::chrono::steady_clock::now() - std::chrono::seconds(31);
    }
    // How many releases of `swapchain` Frame Generation was told of (kept across sessions).
    static std::uint64_t FrameGenReleases(XrSwapchain swapchain) {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.framegen_noted_releases_mutex_);
        const auto it = layer.framegen_noted_releases_.find(swapchain);
        return it == layer.framegen_noted_releases_.end() ? 0 : it->second;
    }
    // The app's status line for Frame Generation in `status`.
    static std::string FrameGenStatusReason(const FrameGenStatus& status) {
        return OpenXrLayer::FrameGenStatusReason(status);
    }
    // Frame Generation left a presenter thread behind (for the process).
    static bool FrameGenPresenterAbandoned() {
        return OpenXrLayer::Instance().framegen_.PresenterAbandoned();
    }
};

} // namespace depthxr

namespace {

using Microsoft::WRL::ComPtr;
using Peer = depthxr::D3D11BridgeLayerTestPeer;
using Color = std::array<std::uint8_t, 4>;

constexpr int kSkip = 77;
constexpr const char* kD3D11Extension = "XR_KHR_D3D11_enable";
constexpr const char* kD3D12Extension = "XR_KHR_D3D12_enable";
constexpr const char* kClockExtension = "XR_KHR_win32_convert_performance_counter_time";
constexpr const char* kQuadViewsExtension = "XR_VARJO_quad_views";
constexpr const char* kFoveatedExtension = "XR_VARJO_foveated_rendering";
constexpr XrSystemId kSystem = 7;
constexpr XrDuration kPeriod = 11'111'111;
constexpr std::uint32_t kImages = 3;
constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
// An application structure of no known type, chained after the ones the
// layer reads (it must reach the runtime as it was).
constexpr XrStructureType kApplicationStructType = static_cast<XrStructureType>(1000999000);

std::filesystem::path g_root; // the test's own directory for everything the layer writes

// Without static destructors: a failure inside a layer call (in the stand-in
// runtime) holds the layer's lock, which its teardown takes.
void Check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << "\n(layer log: " << Peer::LogPath().string() << ")" << std::endl;
        test_gates::OpenAllBeforeExit();
        std::_Exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// How many times `part` appears in `text`.
std::size_t Count(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// The layer's own error lines in `log` (not the test's reads of the log), at
// most `limit`, for a failure's message.
std::string LayerErrors(const std::string& log, std::size_t limit = 5) {
    std::string lines;
    std::istringstream stream(log);
    for (std::string line; limit > 0 && std::getline(stream, line);) {
        if (Contains(line, "[error]") && !Contains(line, "[error] D3D11 bridge layer tests: log read")) {
            lines += "\n  " + line;
            --limit;
        }
    }
    return lines;
}

// Logged once for each session the bridge makes Direct3D 12.
constexpr const char* kBridgedSessionLine =
    "the session is Direct3D 12 and the game renders into images shared with it";

// The session's graphics lines (session_graphics.h): the game's API, the
// bridge's, and the runtime each ends with.
constexpr const char* kGameGraphicsLine = "Session graphics: api=Direct3D 11, adapter=\"";
constexpr const char* kBridgeGraphicsLine =
    "Session graphics: api=Direct3D 12 (Frame Generation's Direct3D 11 bridge), adapter=\"";
constexpr const char* kGraphicsRuntime = "runtime=\"VectorXR stand-in runtime\" 1.0.0";

// A figure from the bridge's summary ("name=value").
std::uint64_t SummaryValue(const std::string& summary, const std::string& name) {
    const std::size_t at = summary.find(name + "=");
    Check(at != std::string::npos, "the summary has " + name + ": " + summary);
    return std::stoull(summary.substr(at + name.size() + 1));
}

bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

template <typename T>
T Handle(std::uintptr_t value) {
    return reinterpret_cast<T>(value);
}

std::string Text(const Color& texel) {
    return std::to_string(texel[0]) + "," + std::to_string(texel[1]) + "," + std::to_string(texel[2]) + "," +
           std::to_string(texel[3]);
}

// Each attempt's extensions, in order.
std::string Text(const std::vector<std::vector<std::string>>& attempts) {
    std::string text;
    for (const std::vector<std::string>& names : attempts) {
        text += text.empty() ? "[" : " then [";
        for (std::size_t i = 0; i < names.size(); ++i) {
            text += (i ? " " : "") + names[i];
        }
        text += "]";
    }
    return text.empty() ? "no attempt" : text;
}

bool Near(const std::optional<Color>& texel, const Color& expected) {
    if (!texel) return false;
    for (int i = 0; i < 4; ++i) {
        if (std::abs(static_cast<int>((*texel)[i]) - static_cast<int>(expected[i])) > 1) return false;
    }
    return true;
}

// A linear-light colour as an 8-bit sRGB image holds it (opaque).
Color SrgbColor(float r, float g, float b) {
    const auto encode = [](float c) {
        const float v = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return {encode(r), encode(g), encode(b), 255};
}

// Each frame's colour differs from those of the frames before it in the same image.
Color FrameColor(int frame, int eye) {
    return {static_cast<std::uint8_t>(16 + 37 * frame), static_cast<std::uint8_t>(200 - 29 * frame),
            static_cast<std::uint8_t>(eye ? 220 : 60), 255};
}

// Quad views: P(frame, eye), each periphery's colour, and F(frame, eye), each
// focus view's, differ from each other, from the other eye's, and from those
// of the seven frames before them.
Color PeripheryColor(int frame, int eye) {
    return {static_cast<std::uint8_t>(30 + 23 * (frame % 8)), static_cast<std::uint8_t>(eye ? 50 : 100), 210, 255};
}

Color FocusColor(int frame, int eye) {
    return {220, static_cast<std::uint8_t>(40 + 21 * (frame % 8)), static_cast<std::uint8_t>(eye ? 150 : 90), 255};
}

// Detail in the focus view's interior, right of its centre (Stripes): what
// the compositor sharpens.
Color StripeColor(int frame, int eye) {
    return {static_cast<std::uint8_t>(250 - 25 * (frame % 8)), 20, static_cast<std::uint8_t>(eye ? 30 : 240), 255};
}

void SetEnv(const char* name, const std::string& value) {
    _putenv_s(name, value.c_str());
}

// --- The game ---------------------------------------------------------------

struct Game {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    LUID luid{};
    bool debug{false};
};

bool MakeGame(bool hardware, Game& game) {
    ComPtr<IDXGIAdapter1> adapter;
    if (hardware) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        if (const std::optional<std::string> wanted = test_adapter::Wanted()) {
            std::string why;
            adapter = test_adapter::FindDxgiAdapter(factory.Get(), *wanted, why);
            if (!adapter) {
                std::cout << why << '\n';
                return false;
            }
        } else {
            // The first hardware adapter: none on a machine with no GPU.
            std::string why;
            adapter = test_adapter::FindDxgiAdapter(factory.Get(), "", why);
            if (!adapter) {
                std::cout << "no hardware adapter\n";
                return false;
            }
        }
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE type = hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP;
    // The Direct3D 11 debug layer when its SDK layers are installed.
    game.debug = SUCCEEDED(D3D11CreateDevice(adapter.Get(), type, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels, 2,
                                             D3D11_SDK_VERSION, &game.device, nullptr, &game.context));
    if (!game.debug && FAILED(D3D11CreateDevice(adapter.Get(), type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                levels, 2, D3D11_SDK_VERSION, &game.device, nullptr, &game.context))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> game_adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(game.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&game_adapter)) ||
        FAILED(game_adapter->GetDesc(&desc)) || FAILED(game.context.As(&game.context4))) {
        return false;
    }
    game.luid = desc.AdapterLuid;
    return true;
}

// The game's adapter renders in software (WARP).
bool SoftwareAdapter(const Game& game) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    return FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
           FAILED(factory->EnumAdapterByLuid(game.luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)) ||
           test_adapter::SoftwareDxgiAdapter(desc);
}

// Only errors and corruption are stored: nothing the checks look for is
// crowded out by the message limit.
void StoreErrorsOnly(ID3D12InfoQueue* queue) {
    D3D12_MESSAGE_SEVERITY deny[] = {D3D12_MESSAGE_SEVERITY_WARNING, D3D12_MESSAGE_SEVERITY_INFO,
                                     D3D12_MESSAGE_SEVERITY_MESSAGE};
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deny));
    filter.DenyList.pSeverityList = deny;
    queue->PushStorageFilter(&filter);
}

void StoreErrorsOnly(ID3D11InfoQueue* queue) {
    D3D11_MESSAGE_SEVERITY deny[] = {D3D11_MESSAGE_SEVERITY_WARNING, D3D11_MESSAGE_SEVERITY_INFO,
                                     D3D11_MESSAGE_SEVERITY_MESSAGE};
    D3D11_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deny));
    filter.DenyList.pSeverityList = deny;
    queue->PushStorageFilter(&filter);
}

// The errors each debug layer stored since the last check (messages it had
// to drop counted as errors).
struct Validation {
    std::string d3d12;
    std::string d3d11;
    std::string All() const { return d3d12 + d3d11; }
};

// Every error and corruption message both debug layers stored since the last check.
Validation ValidationMessages(ID3D12Device* device, Game& game) {
    Validation validation;
    std::string& errors12 = validation.d3d12;
    ComPtr<ID3D12InfoQueue> queue12;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue12)))) {
        if (const UINT64 dropped = queue12->GetNumMessagesDiscardedByMessageCountLimit()) {
            errors12 += "[D3D12] " + std::to_string(dropped) + " messages dropped at the message limit\n";
        }
        for (UINT64 i = 0; i < queue12->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue12->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue12->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                errors12 += std::string("[D3D12 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue12->ClearStoredMessages();
    }
    std::string& errors = validation.d3d11;
    ComPtr<ID3D11InfoQueue> queue11;
    if (game.debug && SUCCEEDED(game.device.As(&queue11))) {
        if (const UINT64 dropped = queue11->GetNumMessagesDiscardedByMessageCountLimit()) {
            errors += "[D3D11] " + std::to_string(dropped) + " messages dropped at the message limit\n";
        }
        for (UINT64 i = 0; i < queue11->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue11->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue11->GetMessage(i, message, &size)) &&
                message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                errors += std::string("[D3D11 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue11->ClearStoredMessages();
    }
    return validation;
}

// The game's rendering: a clear of `texture`'s slice through a UNORM
// render-target view, as DCS renders its sRGB images.
void Clear(Game& game, ID3D11Texture2D* texture, const Color& color, std::uint32_t slice) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (desc.SampleDesc.Count > 1) {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        view.Texture2DMSArray.FirstArraySlice = slice;
        view.Texture2DMSArray.ArraySize = 1;
    } else {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
    }
    ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(game.device->CreateRenderTargetView(texture, &view, &target)), "a render-target view of an image");
    const float rgba[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
    game.context->ClearRenderTargetView(target.Get(), rgba);
}

void ClearDepth(Game& game, ID3D11Texture2D* texture, std::uint32_t slice) {
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.FirstArraySlice = slice;
    view.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11DepthStencilView> target;
    Check(SUCCEEDED(game.device->CreateDepthStencilView(texture, &view, &target)), "a depth view of an image");
    game.context->ClearDepthStencilView(target.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
}

// Direct3D 11 still works: an event query after a flush completes.
bool GameRuns(Game& game) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT};
    ComPtr<ID3D11Query> query;
    if (FAILED(game.device->CreateQuery(&desc, &query))) return false;
    game.context->End(query.Get());
    game.context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    BOOL done = FALSE;
    while (std::chrono::steady_clock::now() < deadline) {
        if (game.context->GetData(query.Get(), &done, sizeof(done), 0) == S_OK && done) return true;
        Sleep(1);
    }
    return false;
}

// A fence the test opens: Direct3D 11 (or a Direct3D 12 queue) held back until Open.
struct Gate {
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    Gate(ID3D12Device* device, Game& game) {
        HANDLE handle = nullptr;
        Check(device && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) &&
                  SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)),
              "a gate fence");
        ComPtr<ID3D11Device5> device5;
        Check(SUCCEEDED(game.device.As(&device5)) &&
                  SUCCEEDED(device5->OpenSharedFence(handle, IID_PPV_ARGS(&fence11))),
              "the gate opened on the game's device");
        CloseHandle(handle);
        test_gates::Add(fence.Get());
    }
    ~Gate() {
        test_gates::Remove(fence.Get());
        Open();
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    void HoldGame(Game& game) { game.context4->Wait(fence11.Get(), 1); }
    void HoldQueue(ID3D12CommandQueue* queue) { queue->Wait(fence.Get(), 1); }
    void Open() { fence->Signal(1); }
    // Opened from another thread after `ms`, while the layer waits for what it holds.
    std::thread OpenLater(DWORD ms) {
        return std::thread([this, ms] {
            Sleep(ms);
            Open();
        });
    }
};

bool WaitFence(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms) {
    if (fence->GetCompletedValue() >= value) return true;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event && SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
        WaitForSingleObject(event, timeout_ms);
    }
    if (event) CloseHandle(event);
    return fence->GetCompletedValue() >= value;
}

// --- The stand-in runtime ---------------------------------------------------

struct StandInSwapchain {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<ComPtr<ID3D12Resource>> images12; // a Direct3D 12 session's
    std::vector<ComPtr<ID3D11Texture2D>> images11; // a Direct3D 11 session's
    std::deque<std::uint32_t> acquired;            // oldest first
    std::uint32_t next{0};
    std::optional<std::uint32_t> released; // the image released last
    int acquires{0};
    int waits{0};
    int releases{0};
    std::size_t Count() const { return images12.empty() ? images11.size() : images12.size(); }
};

// What the stand-in showed for one projection view.
struct ShownView {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t slice{0};
    XrStructureType next_type{XR_TYPE_UNKNOWN}; // the first structure of the view's chain
    bool depth{false};                          // the chain had XrCompositionLayerDepthInfoKHR
    std::optional<Color> texel;                 // the centre of the image released last
    std::vector<Color> probed;                  // its texels at StandIn::probes
};

struct ShownFrame {
    XrTime time{0};
    std::vector<ShownView> views;
    bool held{false}; // the readback still waited while the game's rendering was held back
    std::vector<Color> unordered; // read meanwhile on a queue ordered after nothing (the control)
};

// The stand-in runtime: what it does, and what it saw.
struct StandIn {
    std::vector<std::string> extensions{kD3D11Extension, kD3D12Extension}; // listed before the instance
    bool enumeration_offered{true};
    bool refuse_d3d12_instance{false}; // XR_ERROR_EXTENSION_NOT_PRESENT for instances with XR_KHR_D3D12_enable
    XrResult instance_failure{XR_SUCCESS};
    bool refuse_d3d12_session{false}; // XR_ERROR_GRAPHICS_DEVICE_INVALID for Direct3D 12 sessions
    bool refuse_transfer_dst{false};  // swapchains with TRANSFER_DST are refused
    std::uint32_t width_error{0};     // images this much wider than asked for
    LUID luid{};                      // the adapter it composes on
    std::uint32_t recommended_width{kWidth}; // each stereo view's recommended size
    std::uint32_t recommended_height{kHeight};
    // xrWaitFrame returns at the next refresh, as a runtime's does (else at once).
    bool paced{false};
    // xrAcquireSwapchainImage hands out the image released last again, as
    // Pimax's runtime does (else each in turn, as Virtual Desktop's does).
    bool same_image{false};
    // Where each shown view's image is read besides its centre (fractions of its size).
    std::vector<std::array<float, 2>> probes;
    // Direct3D 11 held back behind it: the next readback is checked to wait
    // for the game's rendering, then the gate is opened.
    Gate* hold{nullptr};
    // The next `refusals` acquires of these swapchains fail.
    std::vector<XrSwapchain> refuse_acquire;
    int refusals{0};
    // The next `wait_refusals` waits on these swapchains fail, the image
    // handed back all the same (it may be released).
    std::vector<XrSwapchain> refuse_wait;
    int wait_refusals{0};
    // Each stereo view's field of view, as xrLocateViews returns it.
    std::array<XrFovf, 2> fovs{{{-0.8f, 0.8f, 0.8f, -0.8f}, {-0.8f, 0.8f, 0.8f, -0.8f}}};
    // While `watch_gate` holds Direct3D 11 back: the images of the `watched`
    // swapchains the thread `watch_thread` acquires, and how many of them it
    // releases to the runtime before the gate opens (before the work on
    // them has run).
    const Gate* watch_gate{nullptr};
    std::thread::id watch_thread;
    std::vector<XrSwapchain> watched;
    std::map<XrSwapchain, std::uint32_t> watched_acquired;
    int early_releases{0};

    std::vector<std::vector<std::string>> instance_attempts; // each attempt's extensions
    int pre_instance_queries{0}; // functions asked for before the instance
    std::vector<std::string> calls;
    XrInstance instance{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrStructureType binding{XR_TYPE_UNKNOWN}; // the session's graphics binding
    const void* binding_next{nullptr};        // what followed it
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue; // the session's queue
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D12Fence> released; // signalled on `queue` at each release
    UINT64 released_value{0};
    ComPtr<ID3D12Fence> readback;
    UINT64 readback_value{0};
    ComPtr<ID3D12CommandQueue> control_queue; // the stand-in's own, for the control readbacks
    ComPtr<ID3D12Fence> control;
    UINT64 control_value{0};
    std::map<XrSwapchain, StandInSwapchain> swapchains;
    std::vector<XrSwapchainCreateInfo> swapchain_requests; // as received
    std::vector<XrSwapchain> created;
    std::vector<XrSwapchain> destroyed;
    std::vector<ShownFrame> frames;
    std::function<void(XrSwapchain)> on_destroy_swapchain;
    std::function<void()> on_destroy_session;
    std::function<void()> on_destroy_instance; // before a session still alive goes with the instance
    // At each xrEndFrame, with the frame's views as the stand-in took them,
    // before it reads them back.
    std::function<void(const XrFrameEndInfo&, ShownFrame&)> on_end_frame;
    XrTime time{1'000'000'000};
    std::chrono::steady_clock::time_point refresh{}; // paced: the refresh the last wait returned at
    // The frame loop as the stand-in saw it: every begin follows a wait of its
    // own, every end a begin, and a wait comes only once the frame waited
    // before it began.
    std::uint64_t frame_waits{0};
    std::uint64_t frame_begins{0};
    std::uint64_t frame_ends{0};
    std::uintptr_t handles{0x1000};

    // The index of the first `call` at or after `from`, or -1.
    std::ptrdiff_t Position(const std::string& call, std::ptrdiff_t from = 0) const {
        for (std::size_t i = static_cast<std::size_t>(std::max<std::ptrdiff_t>(from, 0)); i < calls.size(); ++i) {
            if (calls[i] == call) return static_cast<std::ptrdiff_t>(i);
        }
        return -1;
    }
};

StandIn g_runtime;
// Over g_runtime while a case runs: Frame Generation's presenter thread calls
// the runtime as the application's thread does. The test reads what the
// stand-in saw under it too (Shown).
std::recursive_mutex g_runtime_mutex;
// Frame loop calls in progress: xrWaitFrame never overlaps another wait, nor
// xrBeginFrame or xrEndFrame another begin or end, whichever threads make them.
std::atomic<int> g_waits_in{0};
std::atomic<int> g_begins_ends_in{0};

// The stand-in holds its xrWaitFrame calls (once g_hold_waits is set) until
// the session ends (xrEndSession or xrDestroySession), then fails them: a
// runtime holding its waits while the session stops.
std::mutex g_hold_mutex;
std::condition_variable g_hold_cv;
bool g_hold_waits{false};
std::atomic<int> g_waits_held{0};

void ReleaseHeldWaits() {
    {
        std::scoped_lock lock(g_hold_mutex);
        g_hold_waits = false;
    }
    g_hold_cv.notify_all();
}

struct InCall {
    std::atomic<int>& calls;
    InCall(std::atomic<int>& in, const char* call) : calls(in) {
        Check(calls.fetch_add(1) == 0, std::string(call) + " overlaps another frame call of its kind");
    }
    ~InCall() { calls.fetch_sub(1); }
    InCall(const InCall&) = delete;
    InCall& operator=(const InCall&) = delete;
};

// The frames the stand-in showed from the `from`th on.
std::vector<ShownFrame> Shown(std::size_t from = 0) {
    std::scoped_lock lock(g_runtime_mutex);
    const std::vector<ShownFrame>& frames = g_runtime.frames;
    return {frames.begin() + static_cast<std::ptrdiff_t>(std::min(from, frames.size())), frames.end()};
}

std::size_t ShownCount() {
    std::scoped_lock lock(g_runtime_mutex);
    return g_runtime.frames.size();
}

// Until `until`, on a high-resolution timer (a runtime's wait wakes at the refresh).
void SleepUntil(std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();
    if (until <= now) return;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count() / 100); // relative, 100 ns
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        std::this_thread::sleep_until(until);
    }
    if (timer) CloseHandle(timer);
}

StandInSwapchain* FindSwapchain(XrSwapchain swapchain) {
    const auto it = g_runtime.swapchains.find(swapchain);
    return it == g_runtime.swapchains.end() ? nullptr : &it->second;
}

// A call on `swapchain` the stand-in watches (StandIn::watch_gate): on the
// watched thread, while the gate is still closed.
bool Watching(XrSwapchain swapchain) {
    const StandIn& rt = g_runtime;
    return rt.watch_gate && std::this_thread::get_id() == rt.watch_thread &&
           std::find(rt.watched.begin(), rt.watched.end(), swapchain) != rt.watched.end() &&
           rt.watch_gate->fence->GetCompletedValue() < 1;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// A texel to read: an image's array slice, and where in it (fractions of its size).
struct Source {
    ID3D12Resource* image{nullptr};
    std::uint32_t slice{0};
    std::array<float, 2> at{0.5f, 0.5f};
};
using Sources = std::vector<Source>;

// The texel at `at` (a fraction) of `size` texels.
UINT TexelAt(float at, UINT size) {
    return std::min(size - 1, static_cast<UINT>(std::floor(static_cast<double>(at) * size)));
}

// A copy of each source's texel, submitted to a queue.
struct Readback {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ID3D12Fence* fence{nullptr};
    UINT64 value{0};
    std::size_t count{0};
};

constexpr UINT64 kReadbackStride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

bool SubmitReadback(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& fence_value, const Sources& sources,
                    Readback& readback) {
    ID3D12Device* device = g_runtime.device12.Get();
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kReadbackStride * sources.size();
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback.buffer))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&readback.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, readback.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&readback.list)))) {
        return false;
    }
    // Colour images are render targets whenever the runtime holds them.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const Source& texel : sources) {
        if (std::none_of(barriers.begin(), barriers.end(), [&](const D3D12_RESOURCE_BARRIER& barrier) {
                return barrier.Transition.pResource == texel.image;
            })) {
            barriers.push_back(
                Transition(texel.image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE));
        }
    }
    readback.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const D3D12_RESOURCE_DESC image = sources[i].image->GetDesc();
        D3D12_TEXTURE_COPY_LOCATION source{sources[i].image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = sources[i].slice * image.MipLevels;
        D3D12_TEXTURE_COPY_LOCATION destination{readback.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint.Offset = kReadbackStride * i;
        destination.PlacedFootprint.Footprint = {image.Format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        const UINT x = TexelAt(sources[i].at[0], static_cast<UINT>(image.Width));
        const UINT y = TexelAt(sources[i].at[1], image.Height);
        const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
        readback.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    }
    for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    readback.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    if (FAILED(readback.list->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {readback.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    readback.fence = fence;
    readback.value = ++fence_value;
    readback.count = sources.size();
    queue->Signal(fence, readback.value);
    return true;
}

bool FinishReadback(Readback& readback, std::vector<Color>& texels) {
    if (!WaitFence(readback.fence, readback.value, 10000)) {
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(kReadbackStride * readback.count)};
    if (FAILED(readback.buffer->Map(0, &range, &mapped))) {
        return false;
    }
    texels.resize(readback.count);
    for (std::size_t i = 0; i < readback.count; ++i) {
        const auto* texel = static_cast<const std::uint8_t*>(mapped) + kReadbackStride * i;
        texels[i] = {texel[0], texel[1], texel[2], texel[3]};
    }
    const D3D12_RANGE written{0, 0};
    readback.buffer->Unmap(0, &written);
    return true;
}

// Reads each source's texel on the session's queue, after everything queued
// there before (the images' releases among it), as a compositor reads what
// it shows. With the game held back (StandIn::hold), the readback must still
// be waiting a while later, nothing ordered after the game's rendering having
// run; as a control, the same images read meanwhile on a queue of the
// stand-in's own, ordered after nothing, do not hold the frame yet.
bool ReadBack12(const Sources& sources, ShownFrame& frame, std::vector<Color>& texels) {
    StandIn& rt = g_runtime;
    Readback shown;
    if (!SubmitReadback(rt.queue.Get(), rt.readback.Get(), rt.readback_value, sources, shown)) {
        return false;
    }
    if (rt.hold) {
        Sleep(50);
        frame.held = rt.readback->GetCompletedValue() < shown.value;
        if (!rt.control_queue) {
            const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            rt.device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&rt.control_queue));
            rt.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.control));
        }
        Readback control;
        if (rt.control_queue && rt.control &&
            SubmitReadback(rt.control_queue.Get(), rt.control.Get(), rt.control_value, sources, control)) {
            FinishReadback(control, frame.unordered);
        }
        rt.hold->Open();
        rt.hold = nullptr;
    }
    return FinishReadback(shown, texels);
}

// A Direct3D 11 session's: on the game's immediate context, after its work.
bool ReadBack11(const std::vector<std::pair<ID3D11Texture2D*, std::uint32_t>>& sources, std::vector<Color>& texels) {
    ComPtr<ID3D11DeviceContext> context;
    g_runtime.device11->GetImmediateContext(&context);
    texels.clear();
    for (const auto& [image, slice] : sources) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        D3D11_TEXTURE2D_DESC staging_desc{};
        staging_desc.Width = 1;
        staging_desc.Height = 1;
        staging_desc.MipLevels = 1;
        staging_desc.ArraySize = 1;
        staging_desc.Format = desc.Format;
        staging_desc.SampleDesc.Count = 1;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(g_runtime.device11->CreateTexture2D(&staging_desc, nullptr, &staging))) {
            return false;
        }
        const D3D11_BOX box{desc.Width / 2, desc.Height / 2, 0, desc.Width / 2 + 1, desc.Height / 2 + 1, 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, image, D3D11CalcSubresource(0, slice, desc.MipLevels),
                                       &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        const auto* texel = static_cast<const std::uint8_t*>(mapped.pData);
        texels.push_back({texel[0], texel[1], texel[2], texel[3]});
        context->Unmap(staging.Get(), 0);
    }
    return true;
}

XrResult XRAPI_CALL RtEnumerateInstanceExtensions(const char*, std::uint32_t capacity, std::uint32_t* count,
                                                   XrExtensionProperties* properties) {
    std::scoped_lock lock(g_runtime_mutex);
    const std::vector<std::string>& names = g_runtime.extensions;
    *count = static_cast<std::uint32_t>(names.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < names.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::strncpy(properties[i].extensionName, names[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

// Another API layer's entry between the layer and the stand-in (the stand-in
// answers for it), or none.
depthxr::XrApiLayerNextInfo* g_layer_below = nullptr;

XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                             const depthxr::XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    std::vector<std::string> extensions;
    for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        extensions.emplace_back(info->enabledExtensionNames[i]);
    }
    rt.instance_attempts.push_back(extensions);
    rt.calls.push_back("xrCreateApiLayerInstance");
    // The stand-in is the last of the chain: nothing follows it.
    Check(layer_info && layer_info->nextInfo == g_layer_below, "the layer hands the rest of the chain on");
    if (rt.instance_failure != XR_SUCCESS) return rt.instance_failure;
    if (rt.refuse_d3d12_instance &&
        std::find(extensions.begin(), extensions.end(), kD3D12Extension) != extensions.end()) {
        return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    *instance = rt.instance = Handle<XrInstance>(++rt.handles);
    return XR_SUCCESS;
}

// The session's swapchains (their images) and everything of its device go.
void DropSession(StandIn& rt) {
    rt.swapchains.clear();
    rt.device12.Reset();
    rt.queue.Reset();
    rt.device11.Reset();
    rt.released.Reset();
    rt.readback.Reset();
    rt.control_queue.Reset();
    rt.control.Reset();
    rt.session = XR_NULL_HANDLE;
}

XrResult XRAPI_CALL RtDestroyInstance(XrInstance instance) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroyInstance");
    Check(instance == rt.instance, "the runtime's instance is destroyed");
    if (rt.on_destroy_instance) {
        const auto hook = std::move(rt.on_destroy_instance);
        rt.on_destroy_instance = nullptr;
        hook();
    }
    // A session the application did not destroy goes with its instance.
    DropSession(rt);
    rt.instance = XR_NULL_HANDLE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strncpy(properties->runtimeName, "VectorXR stand-in runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* system) {
    *system = kSystem;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    properties->systemId = kSystem;
    std::strncpy(properties->systemName, "Stand-in headset", XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->graphicsProperties.maxLayerCount = 16;
    properties->graphicsProperties.maxSwapchainImageWidth = 4096;
    properties->graphicsProperties.maxSwapchainImageHeight = 4096;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetD3D11Requirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* requirements) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetD3D11GraphicsRequirementsKHR");
    requirements->adapterLuid = g_runtime.luid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetD3D12Requirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* requirements) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetD3D12GraphicsRequirementsKHR");
    requirements->adapterLuid = g_runtime.luid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateViewConfigurations(XrInstance, XrSystemId, std::uint32_t capacity,
                                                   std::uint32_t* count, XrViewConfigurationType* types) {
    *count = 1;
    if (capacity) types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType type,
                                                      XrViewConfigurationProperties* properties) {
    properties->viewConfigurationType = type;
    properties->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                                       std::uint32_t capacity, std::uint32_t* count,
                                                       XrViewConfigurationView* views) {
    std::scoped_lock lock(g_runtime_mutex);
    *count = 2;
    for (std::uint32_t i = 0; i < std::min(capacity, 2u); ++i) {
        views[i].recommendedImageRectWidth = g_runtime.recommended_width;
        views[i].recommendedImageRectHeight = g_runtime.recommended_height;
        views[i].maxImageRectWidth = 4096;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, std::uint32_t capacity,
                                          std::uint32_t* count, XrEnvironmentBlendMode* modes) {
    *count = 1;
    if (capacity) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtPollEvent(XrInstance, XrEventDataBuffer*) {
    return XR_EVENT_UNAVAILABLE;
}

XrResult XRAPI_CALL RtCreateSession(XrInstance, const XrSessionCreateInfo* info, XrSession* session) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    const auto* first = info ? static_cast<const XrBaseInStructure*>(info->next) : nullptr;
    if (first && first->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        rt.calls.push_back("xrCreateSession:D3D12");
        const auto& binding = *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
        if (rt.refuse_d3d12_session || !binding.device || !binding.queue ||
            FAILED(binding.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.released))) ||
            FAILED(binding.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.readback)))) {
            rt.released.Reset();
            rt.readback.Reset();
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        rt.device12 = binding.device;
        rt.queue = binding.queue;
        rt.binding_next = binding.next;
    } else if (first && first->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
        rt.calls.push_back("xrCreateSession:D3D11");
        const auto& binding = *reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(first);
        if (!binding.device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        rt.device11 = binding.device;
        rt.binding_next = binding.next;
    } else {
        rt.calls.push_back("xrCreateSession:other");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    rt.binding = first->type;
    *session = rt.session = Handle<XrSession>(++rt.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySession(XrSession session) {
    ReleaseHeldWaits();
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroySession");
    Check(session == rt.session, "the runtime's session is destroyed");
    if (rt.on_destroy_session) {
        const auto hook = std::move(rt.on_destroy_session);
        rt.on_destroy_session = nullptr;
        hook();
    }
    // Its swapchains go with it.
    DropSession(rt);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtBeginSession(XrSession, const XrSessionBeginInfo* info) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrBeginSession");
    // Quad views are the layer's: the stand-in has the stereo configuration only.
    Check(info && info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          "the runtime's session begins with the stereo view configuration");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEndSession(XrSession) {
    ReleaseHeldWaits();
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrEndSession");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtAttachActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtSyncActions(XrSession, const XrActionsSyncInfo*) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateReferenceSpaces(XrSession, std::uint32_t capacity, std::uint32_t* count,
                                                XrReferenceSpaceType* spaces) {
    constexpr XrReferenceSpaceType kSpaces[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                XR_REFERENCE_SPACE_TYPE_STAGE};
    *count = 3;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(std::begin(kSpaces), std::end(kSpaces), spaces);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType, XrExtent2Df* bounds) {
    *bounds = {};
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XrResult XRAPI_CALL RtCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
    std::scoped_lock lock(g_runtime_mutex);
    *space = Handle<XrSpace>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) {
    std::scoped_lock lock(g_runtime_mutex);
    *space = Handle<XrSpace>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySpace(XrSpace) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    location->pose = {{0, 0, 0, 1}, {0, 0, 0}};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* state, std::uint32_t capacity,
                                  std::uint32_t* count, XrView* views) {
    std::scoped_lock lock(g_runtime_mutex);
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    for (std::uint32_t i = 0; i < std::min(capacity, 2u); ++i) {
        views[i].pose = {{0, 0, 0, 1}, {i ? 0.032f : -0.032f, 0, 0}};
        views[i].fov = g_runtime.fovs[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    const InCall in(g_waits_in, "xrWaitFrame");
    {
        std::unique_lock hold(g_hold_mutex);
        if (g_hold_waits) {
            ++g_waits_held;
            g_hold_cv.wait(hold, [] { return !g_hold_waits; });
            --g_waits_held;
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
    }
    std::optional<std::chrono::steady_clock::time_point> until;
    XrTime time = 0;
    {
        std::scoped_lock lock(g_runtime_mutex);
        StandIn& rt = g_runtime;
        Check(rt.frame_waits == rt.frame_begins, "xrWaitFrame comes only once the frame waited before it began");
        ++rt.frame_waits;
        XrDuration refreshes = 1;
        if (rt.paced) {
            // The next refresh, or the first one after a wait that comes late.
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::nanoseconds period(kPeriod);
            if (rt.refresh == std::chrono::steady_clock::time_point{}) {
                rt.refresh = now;
            } else {
                rt.refresh += period;
                for (; rt.refresh < now; rt.refresh += period) {
                    ++refreshes;
                }
                until = rt.refresh;
            }
        }
        rt.time += kPeriod * refreshes;
        time = rt.time;
    }
    if (until) SleepUntil(*until);
    state->predictedDisplayTime = time;
    state->predictedDisplayPeriod = kPeriod;
    state->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtBeginFrame(XrSession, const XrFrameBeginInfo*) {
    const InCall in(g_begins_ends_in, "xrBeginFrame or xrEndFrame");
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    Check(rt.frame_begins < rt.frame_waits, "xrBeginFrame begins a frame waited for");
    ++rt.frame_begins;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEndFrame(XrSession, const XrFrameEndInfo* info) {
    const InCall in(g_begins_ends_in, "xrBeginFrame or xrEndFrame");
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrEndFrame");
    Check(rt.frame_ends < rt.frame_begins, "xrEndFrame ends a begun frame");
    ++rt.frame_ends;
    ShownFrame frame;
    frame.time = info->displayTime;
    for (std::uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(base);
        // The stand-in is a stereo runtime, whatever the application's views.
        Check(projection.viewCount == 2,
              "the runtime gets projections of two views, not " + std::to_string(projection.viewCount));
        for (std::uint32_t v = 0; v < projection.viewCount; ++v) {
            const XrCompositionLayerProjectionView& view = projection.views[v];
            ShownView shown;
            shown.swapchain = view.subImage.swapchain;
            shown.slice = view.subImage.imageArrayIndex;
            const auto* chain = static_cast<const XrBaseInStructure*>(view.next);
            shown.next_type = chain ? chain->type : XR_TYPE_UNKNOWN;
            for (; chain; chain = chain->next) {
                shown.depth = shown.depth || chain->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
            }
            frame.views.push_back(shown);
        }
    }
    if (rt.on_end_frame) {
        rt.on_end_frame(*info, frame);
    }
    std::vector<Color> texels;
    bool read = false;
    std::size_t per_view = 1; // texels read per view: the centre, then the probes (Direct3D 12)
    // Nothing is read on a removed device: its fences read as done at once,
    // and WARP may still be running the readback when it is let go.
    if (rt.device12 && SUCCEEDED(rt.device12->GetDeviceRemovedReason())) {
        Sources sources;
        per_view += rt.probes.size();
        for (const ShownView& view : frame.views) {
            const StandInSwapchain* chain = FindSwapchain(view.swapchain);
            if (!chain || !chain->released || chain->images12.empty()) {
                sources.clear();
                break;
            }
            ID3D12Resource* image = chain->images12[*chain->released].Get();
            sources.push_back({image, view.slice});
            for (const std::array<float, 2>& probe : rt.probes) {
                sources.push_back({image, view.slice, probe});
            }
        }
        read = !sources.empty() && ReadBack12(sources, frame, texels);
    } else if (rt.device11) {
        std::vector<std::pair<ID3D11Texture2D*, std::uint32_t>> sources;
        for (const ShownView& view : frame.views) {
            const StandInSwapchain* chain = FindSwapchain(view.swapchain);
            if (!chain || !chain->released || chain->images11.empty()) {
                sources.clear();
                break;
            }
            sources.emplace_back(chain->images11[*chain->released].Get(), view.slice);
        }
        read = !sources.empty() && ReadBack11(sources, texels);
    }
    for (std::size_t i = 0; read && i < frame.views.size(); ++i) {
        frame.views[i].texel = texels[i * per_view];
        frame.views[i].probed.assign(texels.begin() + static_cast<std::ptrdiff_t>(i * per_view + 1),
                                     texels.begin() + static_cast<std::ptrdiff_t>((i + 1) * per_view));
    }
    rt.frames.push_back(frame);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateSwapchainFormats(XrSession, std::uint32_t capacity, std::uint32_t* count,
                                                 std::int64_t* formats) {
    constexpr std::int64_t kFormats[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                                         DXGI_FORMAT_D32_FLOAT};
    *count = 3;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(std::begin(kFormats), std::end(kFormats), formats);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrCreateSwapchain");
    rt.swapchain_requests.push_back(*info);
    if (session != rt.session) return XR_ERROR_HANDLE_INVALID;
    if (rt.refuse_transfer_dst && (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const bool depth = (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    StandInSwapchain chain;
    chain.info = *info;
    chain.info.next = nullptr;
    const std::uint32_t array_size = std::max(1u, info->arraySize) * std::max(1u, info->faceCount);
    for (std::uint32_t i = 0; i < kImages; ++i) {
        if (rt.device12) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = info->width + rt.width_error;
            desc.Height = info->height;
            desc.DepthOrArraySize = static_cast<UINT16>(array_size);
            desc.MipLevels = static_cast<UINT16>(std::max(1u, info->mipCount));
            desc.Format = static_cast<DXGI_FORMAT>(info->format);
            desc.SampleDesc.Count = std::max(1u, info->sampleCount);
            desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            // Handed out as runtimes hand them out: colour images as render targets.
            ComPtr<ID3D12Resource> image;
            if (FAILED(rt.device12->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                    IID_PPV_ARGS(&image)))) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            image->SetName(L"VectorXR stand-in runtime swapchain image");
            chain.images12.push_back(image);
        } else if (rt.device11) {
            // Typeless, as runtimes' Direct3D 11 images usually are.
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = info->width + rt.width_error;
            desc.Height = info->height;
            desc.MipLevels = std::max(1u, info->mipCount);
            desc.ArraySize = array_size;
            desc.Format = depth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
            desc.SampleDesc.Count = std::max(1u, info->sampleCount);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> image;
            if (FAILED(rt.device11->CreateTexture2D(&desc, nullptr, &image))) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            chain.images11.push_back(image);
        } else {
            return XR_ERROR_HANDLE_INVALID;
        }
    }
    *swapchain = Handle<XrSwapchain>(++rt.handles);
    rt.swapchains[*swapchain] = std::move(chain);
    rt.created.push_back(*swapchain);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySwapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroySwapchain");
    if (!FindSwapchain(swapchain)) return XR_ERROR_HANDLE_INVALID;
    if (rt.on_destroy_swapchain) rt.on_destroy_swapchain(swapchain);
    rt.destroyed.push_back(swapchain);
    rt.swapchains.erase(swapchain);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateSwapchainImages(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t* count,
                                                XrSwapchainImageBaseHeader* images) {
    std::scoped_lock lock(g_runtime_mutex);
    const StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    *count = static_cast<std::uint32_t>(chain->Count());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < chain->Count()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!chain->images12.empty()) {
        auto* out = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (std::size_t i = 0; i < chain->images12.size(); ++i) {
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = chain->images12[i].Get();
        }
    } else {
        auto* out = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (std::size_t i = 0; i < chain->images11.size(); ++i) {
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = chain->images11[i].Get();
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtAcquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    std::scoped_lock lock(g_runtime_mutex);
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (g_runtime.refusals > 0 && std::find(g_runtime.refuse_acquire.begin(), g_runtime.refuse_acquire.end(),
                                            swapchain) != g_runtime.refuse_acquire.end()) {
        --g_runtime.refusals;
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (chain->acquired.size() >= chain->Count()) return XR_ERROR_CALL_ORDER_INVALID;
    // Pimax's runtime hands the image back released last, every frame.
    *index = g_runtime.same_image && chain->released && chain->acquired.empty() ? *chain->released : chain->next;
    chain->next = (*index + 1) % static_cast<std::uint32_t>(chain->Count());
    chain->acquired.push_back(*index);
    ++chain->acquires;
    if (Watching(swapchain)) {
        g_runtime.watched_acquired[swapchain] = *index;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtWait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
    std::scoped_lock lock(g_runtime_mutex);
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    // An image acquired and not waited for, and none waited for and not
    // released (OpenXR's order: a wait only after the image waited before it
    // was released).
    if (chain->waits >= chain->acquires || chain->waits > chain->releases) return XR_ERROR_CALL_ORDER_INVALID;
    ++chain->waits;
    const std::vector<XrSwapchain>& refused = g_runtime.refuse_wait;
    if (g_runtime.wait_refusals > 0 && std::find(refused.begin(), refused.end(), swapchain) != refused.end()) {
        --g_runtime.wait_refusals;
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtRelease(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (chain->acquired.empty() || chain->releases >= chain->waits) return XR_ERROR_CALL_ORDER_INVALID;
    chain->released = chain->acquired.front();
    chain->acquired.pop_front();
    ++chain->releases;
    if (Watching(swapchain)) {
        const auto acquired = rt.watched_acquired.find(swapchain);
        rt.early_releases += acquired != rt.watched_acquired.end() && acquired->second == *chain->released ? 1 : 0;
    }
    // As runtimes do: the image counts as written once the session's queue
    // has run everything submitted before the release.
    if (rt.queue) {
        rt.queue->Signal(rt.released.Get(), ++rt.released_value);
    }
    return XR_SUCCESS;
}

template <typename F>
PFN_xrVoidFunction Function(F* function) {
    return reinterpret_cast<PFN_xrVoidFunction>(function);
}

XrResult XRAPI_CALL RtGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    static const std::map<std::string, PFN_xrVoidFunction> kFunctions = {
        {"xrDestroyInstance", Function(RtDestroyInstance)},
        {"xrGetInstanceProperties", Function(RtGetInstanceProperties)},
        {"xrGetSystem", Function(RtGetSystem)},
        {"xrGetSystemProperties", Function(RtGetSystemProperties)},
        {"xrGetD3D11GraphicsRequirementsKHR", Function(RtGetD3D11Requirements)},
        {"xrGetD3D12GraphicsRequirementsKHR", Function(RtGetD3D12Requirements)},
        {"xrEnumerateViewConfigurations", Function(RtEnumerateViewConfigurations)},
        {"xrGetViewConfigurationProperties", Function(RtGetViewConfigurationProperties)},
        {"xrEnumerateViewConfigurationViews", Function(RtEnumerateViewConfigurationViews)},
        {"xrEnumerateEnvironmentBlendModes", Function(RtEnumerateBlendModes)},
        {"xrPollEvent", Function(RtPollEvent)},
        {"xrCreateSession", Function(RtCreateSession)},
        {"xrDestroySession", Function(RtDestroySession)},
        {"xrBeginSession", Function(RtBeginSession)},
        {"xrEndSession", Function(RtEndSession)},
        {"xrAttachSessionActionSets", Function(RtAttachActionSets)},
        {"xrSyncActions", Function(RtSyncActions)},
        {"xrEnumerateReferenceSpaces", Function(RtEnumerateReferenceSpaces)},
        {"xrGetReferenceSpaceBoundsRect", Function(RtGetReferenceSpaceBoundsRect)},
        {"xrCreateReferenceSpace", Function(RtCreateReferenceSpace)},
        {"xrCreateActionSpace", Function(RtCreateActionSpace)},
        {"xrDestroySpace", Function(RtDestroySpace)},
        {"xrLocateSpace", Function(RtLocateSpace)},
        {"xrLocateViews", Function(RtLocateViews)},
        {"xrWaitFrame", Function(RtWaitFrame)},
        {"xrBeginFrame", Function(RtBeginFrame)},
        {"xrEndFrame", Function(RtEndFrame)},
        {"xrEnumerateSwapchainFormats", Function(RtEnumerateSwapchainFormats)},
        {"xrCreateSwapchain", Function(RtCreateSwapchain)},
        {"xrDestroySwapchain", Function(RtDestroySwapchain)},
        {"xrEnumerateSwapchainImages", Function(RtEnumerateSwapchainImages)},
        {"xrAcquireSwapchainImage", Function(RtAcquire)},
        {"xrWaitSwapchainImage", Function(RtWait)},
        {"xrReleaseSwapchainImage", Function(RtRelease)},
    };
    *function = nullptr;
    if (instance == XR_NULL_HANDLE) {
        // Before the instance: the loader's extension scan.
        std::scoped_lock lock(g_runtime_mutex);
        ++g_runtime.pre_instance_queries;
        if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0 && g_runtime.enumeration_offered) {
            *function = Function(RtEnumerateInstanceExtensions);
            return XR_SUCCESS;
        }
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const auto it = kFunctions.find(name);
    if (it == kFunctions.end()) return XR_ERROR_FUNCTION_UNSUPPORTED;
    *function = it->second;
    return XR_SUCCESS;
}

// --- Frame Generation's stand-in backend ------------------------------------

// The chain Frame Generation's backend was made from.
struct FrameGenChain {
    XrStructureType first{XR_TYPE_UNKNOWN};
    void* queue{nullptr};        // FrameGenRuntimeQueue::d3d12_queue
    const void* next{nullptr};   // the game's chain after it (or the chain itself)
};
FrameGenChain g_framegen_chain;

class StandInGpu final : public depthxr::FrameGenGpu {
  public:
    const char* Name() const override { return "Stand-in"; }
    std::optional<depthxr::FrameGenOutputKind> Classify(std::int64_t) const override {
        return depthxr::FrameGenOutputKind::kSrgb8;
    }
    std::vector<std::int64_t> OutputFormats(depthxr::FrameGenOutputKind) const override {
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    }
    XrSwapchainUsageFlags RequiredSourceUsage() const override { return 0; }
    XrSwapchainUsageFlags OutputUsage() const override { return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; }
    bool Track(XrSwapchain, PFN_xrEnumerateSwapchainImages, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Capture(std::size_t, const depthxr::FrameGenImage&, const depthxr::FrameGenViewRequest&,
                 depthxr::FrameGenFlowQuality, std::string&) override {
        return true;
    }
    bool WaitIdle() override { return true; }
    bool Execute(const std::vector<depthxr::FrameGenViewSubmission>& views, const depthxr::FrameGenTuning&,
                 depthxr::FrameGenExecuteResult& result, std::string&) override {
        result.synthesized.assign(views.size(), true);
        return true;
    }
    void ResetHistory() override {}
    void Reset() override {}
};

std::unique_ptr<depthxr::FrameGenGpu> MakeStandInGpu(const void* chain, std::string&) {
    g_framegen_chain = {};
    if (const auto* first = static_cast<const XrBaseInStructure*>(chain)) {
        g_framegen_chain.first = first->type;
        g_framegen_chain.next = chain;
        if (first->type == depthxr::kFrameGenRuntimeQueueType) {
            const auto* marker = static_cast<const depthxr::FrameGenRuntimeQueue*>(chain);
            g_framegen_chain.queue = marker->d3d12_queue;
            g_framegen_chain.next = marker->next;
        }
    }
    return std::make_unique<StandInGpu>();
}

// --- The application --------------------------------------------------------

// The application's OpenXR, from the layer's xrGetInstanceProcAddr.
struct Api {
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrGetSystem get_system{};
    PFN_xrGetD3D11GraphicsRequirementsKHR d3d11_requirements{};
    PFN_xrEnumerateViewConfigurationViews enumerate_views{};
    PFN_xrPollEvent poll_event{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrCreateReferenceSpace create_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrLocateViews locate_views{};
};

template <typename T>
T LayerFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    Check(XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function)) && function,
          std::string("the layer hands out ") + name);
    return reinterpret_cast<T>(function);
}

Api Load(XrInstance instance) {
    Api api;
    api.instance = instance;
    api.destroy_instance = LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance");
    api.get_system = LayerFunction<PFN_xrGetSystem>(instance, "xrGetSystem");
    api.d3d11_requirements =
        LayerFunction<PFN_xrGetD3D11GraphicsRequirementsKHR>(instance, "xrGetD3D11GraphicsRequirementsKHR");
    api.enumerate_views =
        LayerFunction<PFN_xrEnumerateViewConfigurationViews>(instance, "xrEnumerateViewConfigurationViews");
    api.poll_event = LayerFunction<PFN_xrPollEvent>(instance, "xrPollEvent");
    api.create_session = LayerFunction<PFN_xrCreateSession>(instance, "xrCreateSession");
    api.destroy_session = LayerFunction<PFN_xrDestroySession>(instance, "xrDestroySession");
    api.begin_session = LayerFunction<PFN_xrBeginSession>(instance, "xrBeginSession");
    api.end_session = LayerFunction<PFN_xrEndSession>(instance, "xrEndSession");
    api.create_space = LayerFunction<PFN_xrCreateReferenceSpace>(instance, "xrCreateReferenceSpace");
    api.destroy_space = LayerFunction<PFN_xrDestroySpace>(instance, "xrDestroySpace");
    api.create_swapchain = LayerFunction<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain");
    api.destroy_swapchain = LayerFunction<PFN_xrDestroySwapchain>(instance, "xrDestroySwapchain");
    api.enumerate_images = LayerFunction<PFN_xrEnumerateSwapchainImages>(instance, "xrEnumerateSwapchainImages");
    api.acquire = LayerFunction<PFN_xrAcquireSwapchainImage>(instance, "xrAcquireSwapchainImage");
    api.wait = LayerFunction<PFN_xrWaitSwapchainImage>(instance, "xrWaitSwapchainImage");
    api.release = LayerFunction<PFN_xrReleaseSwapchainImage>(instance, "xrReleaseSwapchainImage");
    api.wait_frame = LayerFunction<PFN_xrWaitFrame>(instance, "xrWaitFrame");
    api.begin_frame = LayerFunction<PFN_xrBeginFrame>(instance, "xrBeginFrame");
    api.end_frame = LayerFunction<PFN_xrEndFrame>(instance, "xrEndFrame");
    api.locate_views = LayerFunction<PFN_xrLocateViews>(instance, "xrLocateViews");
    return api;
}

// xrCreateInstance as the loader makes it with the layer enabled: the
// stand-in runtime is the next (and last) of the chain.
XrResult CreateInstance(const std::vector<const char*>& extensions, XrInstance& instance) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "VectorXR bridge test", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "Direct3D 11", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    depthxr::XrApiLayerNextInfo next{};
    next.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next.structVersion = depthxr::XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next.structSize = sizeof(next);
    std::strncpy(next.layerName, "XR_APILAYER_TEST_stand_in_runtime", XR_MAX_API_LAYER_NAME_SIZE - 1);
    next.nextGetInstanceProcAddr = RtGetInstanceProcAddr;
    next.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
    next.next = g_layer_below;
    depthxr::XrApiLayerCreateInfo create{};
    create.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    create.structVersion = depthxr::XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    create.structSize = sizeof(create);
    create.nextInfo = &next;
    instance = XR_NULL_HANDLE;
    return xrCreateApiLayerInstance(&info, &create, &instance);
}

// The application's session create info: its Direct3D 11 binding first, a
// structure of its own after it.
struct SessionCreate {
    XrBaseInStructure application_struct{kApplicationStructType};
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    SessionCreate(ID3D11Device* device, XrSystemId system) {
        binding.next = &application_struct;
        binding.device = device;
        info.next = &binding;
        info.systemId = system;
    }
    SessionCreate(const SessionCreate&) = delete;
    SessionCreate& operator=(const SessionCreate&) = delete;
};

struct App {
    Api api;
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};
};

// A Direct3D 11 game's start: its instance, system and graphics requirements.
App StartApp(const std::vector<const char*>& extensions = {kD3D11Extension}) {
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult result = CreateInstance(extensions, instance);
    Check(result == XR_SUCCESS && instance != XR_NULL_HANDLE,
          "xrCreateInstance through the layer: " + std::to_string(result));
    App app;
    app.api = Load(instance);
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    Check(app.api.get_system(instance, &get, &app.system) == XR_SUCCESS, "xrGetSystem");
    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    Check(app.api.d3d11_requirements(instance, app.system, &requirements) == XR_SUCCESS &&
              SameLuid(requirements.adapterLuid, g_runtime.luid),
          "xrGetD3D11GraphicsRequirementsKHR");
    Check(Contains(Peer::Log(), "Loaded config from"), "the test's settings were loaded");
    return app;
}

void BeginSession(App& app, XrViewConfigurationType configuration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1;
    Check(app.api.create_space(app.session, &space, &app.space) == XR_SUCCESS, "xrCreateReferenceSpace");
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    Check(app.api.poll_event(app.api.instance, &event) == XR_EVENT_UNAVAILABLE, "xrPollEvent");
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
    begin.primaryViewConfigurationType = configuration;
    Check(app.api.begin_session(app.session, &begin) == XR_SUCCESS, "xrBeginSession");
}

// Its session (when it has one) and instance destroyed.
void StopApp(App& app) {
    if (app.space != XR_NULL_HANDLE) {
        Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
        app.space = XR_NULL_HANDLE;
    }
    if (app.session != XR_NULL_HANDLE) {
        Check(app.api.destroy_session(app.session) == XR_SUCCESS, "xrDestroySession");
        app.session = XR_NULL_HANDLE;
    }
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
}

XrSwapchainCreateInfo ColorInfo(std::uint32_t samples = 1, std::uint32_t width = kWidth,
                                std::uint32_t height = kHeight) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    info.sampleCount = samples;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    return info;
}

XrSwapchainCreateInfo DepthInfo(std::uint32_t array_size) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.format = DXGI_FORMAT_D32_FLOAT;
    info.sampleCount = 1;
    info.width = kWidth;
    info.height = kHeight;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    return info;
}

// A swapchain the application renders into, with its images.
struct Target {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<ID3D11Texture2D*> images;
};

std::vector<ID3D11Texture2D*> Images(App& app, XrSwapchain swapchain) {
    std::uint32_t count = 0;
    const XrResult result = app.api.enumerate_images(swapchain, 0, &count, nullptr);
    Check(result == XR_SUCCESS && count == kImages,
          "xrEnumerateSwapchainImages counts the images: " + std::to_string(count));
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    Check(app.api.enumerate_images(swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) == XR_SUCCESS,
          "xrEnumerateSwapchainImages");
    std::vector<ID3D11Texture2D*> textures;
    for (const XrSwapchainImageD3D11KHR& image : images) {
        Check(image.texture != nullptr, "every image is a texture");
        textures.push_back(image.texture);
    }
    return textures;
}

Target MakeTarget(App& app, const XrSwapchainCreateInfo& info, const std::string& what) {
    Target target;
    const XrResult result = app.api.create_swapchain(app.session, &info, &target.swapchain);
    Check(result == XR_SUCCESS && target.swapchain != XR_NULL_HANDLE,
          what + ": xrCreateSwapchain " + std::to_string(result));
    target.images = Images(app, target.swapchain);
    return target;
}

std::uint32_t Acquire(App& app, const Target& target) {
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    Check(app.api.acquire(target.swapchain, &acquire, &index) == XR_SUCCESS && index < target.images.size(),
          "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    Check(app.api.wait(target.swapchain, &wait) == XR_SUCCESS, "xrWaitSwapchainImage");
    return index;
}

void Release(App& app, const Target& target) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(app.api.release(target.swapchain, &release) == XR_SUCCESS, "xrReleaseSwapchainImage");
}

void Draw(Game& game, ID3D11Texture2D* image, const Color& color) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    for (std::uint32_t slice = 0; slice < desc.ArraySize; ++slice) {
        Clear(game, image, color, slice);
    }
}

// Vertical stripes one texel wide, two apart, across 62-72% of the image's
// width: detail away from its centre and edges.
void Stripes(Game& game, ID3D11Texture2D* image, const Color& color) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(game.device->CreateRenderTargetView(image, &view, &target)), "a render-target view of an image");
    std::vector<D3D11_RECT> stripes;
    for (LONG x = static_cast<LONG>(desc.Width * 62 / 100); x < static_cast<LONG>(desc.Width * 72 / 100); x += 3) {
        stripes.push_back({x, 0, x + 1, static_cast<LONG>(desc.Height)});
    }
    const float rgba[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
    game.context4->ClearView(target.Get(), rgba, stripes.data(), static_cast<UINT>(stripes.size()));
}

// An image rendered and released outside a frame.
void Render(App& app, Game& game, const Target& target, const Color& color) {
    Draw(game, target.images[Acquire(app, target)], color);
    Release(app, target);
}

struct Eye {
    const Target* target{nullptr};
    std::uint32_t slice{0};
};

// One application frame: each colour target acquired, waited, cleared to its
// colour and released, in the order given (with `depth`, cleared, when given);
// then a stereo projection of `eyes`, each view chaining its slice of `depth`
// (and a structure of the application's after it) when given. With `hold`,
// Direct3D 11 waits for the gate from just before the LAST colour target's
// release on (after the others' releases): that image alone is not written
// yet, and nothing queued for the releases before it waits for the gate. The
// stand-in opens the gate once it has seen its readback wait all the same.
XrResult Frame(App& app, Game& game, const std::vector<const Target*>& targets, const std::vector<Color>& colors,
               const std::array<Eye, 2>& eyes, const Target* depth = nullptr, Gate* hold = nullptr,
               bool unknown_after_depth = false) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Check(app.api.wait_frame(app.session, &wait_info, &state) == XR_SUCCESS, "xrWaitFrame");
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    Check(app.api.begin_frame(app.session, &begin_info) == XR_SUCCESS, "xrBeginFrame");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = app.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{};
    for (XrView& view : located) view.type = XR_TYPE_VIEW;
    std::uint32_t located_count = 0;
    Check(app.api.locate_views(app.session, &locate, &view_state, 2, &located_count, located.data()) == XR_SUCCESS &&
              located_count == 2,
          "xrLocateViews");

    std::vector<std::uint32_t> indices;
    for (const Target* target : targets) {
        indices.push_back(Acquire(app, *target));
    }
    const std::uint32_t depth_index = depth ? Acquire(app, *depth) : 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        Draw(game, targets[i]->images[indices[i]], colors[i]);
    }
    if (depth) {
        ClearDepth(game, depth->images[depth_index], 0);
        ClearDepth(game, depth->images[depth_index], 1);
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (hold && i + 1 == targets.size()) {
            // A layer that let the runtime's queue wait only for what was
            // queued before this release (not for its own copy) would show
            // the frame without the gate.
            hold->HoldGame(game);
        }
        Release(app, *targets[i]);
    }
    if (depth) {
        Release(app, *depth);
    }

    XrBaseInStructure after_depth{kApplicationStructType};
    std::array<XrCompositionLayerDepthInfoKHR, 2> depth_infos{};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage.swapchain = eyes[eye].target->swapchain;
        views[eye].subImage.imageRect = {{0, 0},
                                         {static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight)}};
        views[eye].subImage.imageArrayIndex = eyes[eye].slice;
        if (depth) {
            depth_infos[eye] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            depth_infos[eye].next = unknown_after_depth ? &after_depth : nullptr;
            depth_infos[eye].subImage.swapchain = depth->swapchain;
            depth_infos[eye].subImage.imageRect = views[eye].subImage.imageRect;
            depth_infos[eye].subImage.imageArrayIndex = eye;
            depth_infos[eye].minDepth = 0.0f;
            depth_infos[eye].maxDepth = 1.0f;
            depth_infos[eye].nearZ = 0.05f;
            depth_infos[eye].farZ = 1000.0f;
            views[eye].next = &depth_infos[eye];
        }
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = app.space;
    projection.viewCount = 2;
    projection.views = views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = layers;
    g_runtime.hold = hold;
    const XrResult result = app.api.end_frame(app.session, &end);
    if (g_runtime.hold) {
        // The stand-in never read the frame back: let the game run on.
        g_runtime.hold->Open();
        g_runtime.hold = nullptr;
        Check(!hold || result != XR_SUCCESS, "the runtime read back a frame the game was held back for");
    }
    return result;
}

// What a Star Citizen-ordered frame's images hold when released (FrameReleasedFirst).
constexpr Color kBeforeRendering{255, 0, 255, 255};

// One frame in Star Citizen's order: each colour target acquired, cleared to
// kBeforeRendering and released before xrBeginFrame, then cleared to its
// colour between it and xrEndFrame. What a runtime reads at xrEndFrame is
// the frame; a copy at the release would be kBeforeRendering.
XrResult FrameReleasedFirst(App& app, Game& game, const std::vector<const Target*>& targets,
                            const std::vector<Color>& colors, const std::array<Eye, 2>& eyes) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Check(app.api.wait_frame(app.session, &wait_info, &state) == XR_SUCCESS, "xrWaitFrame");
    std::vector<std::uint32_t> indices;
    for (const Target* target : targets) {
        indices.push_back(Acquire(app, *target));
        Draw(game, target->images[indices.back()], kBeforeRendering);
        Release(app, *target);
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    Check(app.api.begin_frame(app.session, &begin_info) == XR_SUCCESS, "xrBeginFrame");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = app.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{};
    for (XrView& view : located) view.type = XR_TYPE_VIEW;
    std::uint32_t located_count = 0;
    Check(app.api.locate_views(app.session, &locate, &view_state, 2, &located_count, located.data()) == XR_SUCCESS &&
              located_count == 2,
          "xrLocateViews");
    for (std::size_t i = 0; i < targets.size(); ++i) {
        Draw(game, targets[i]->images[indices[i]], colors[i]);
    }
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage.swapchain = eyes[eye].target->swapchain;
        views[eye].subImage.imageRect = {{0, 0},
                                         {static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight)}};
        views[eye].subImage.imageArrayIndex = eyes[eye].slice;
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = app.space;
    projection.viewCount = 2;
    projection.views = views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = layers;
    return app.api.end_frame(app.session, &end);
}

// 2b. A game rendering into its images after releasing them, before
// xrEndFrame (as Star Citizen does): the runtime shows each frame as
// rendered, never what the images held at the release; with images in turn
// (Virtual Desktop) and with the same image every frame (Pimax). The bridge
// hands an image over at the frame's end, where a runtime reads it.
void CheckRenderAfterRelease(App& app, Game& game, Target& left, Target& right) {
    for (const bool same_image : {false, true}) {
        const std::string name = same_image ? "rendered after release, the same image every frame"
                                            : "rendered after release, images in turn";
        g_runtime.same_image = same_image;
        std::set<std::uint32_t> images;
        for (int frame = 0; frame < 8; ++frame) {
            const std::vector<Color> colors{FrameColor(frame, 0), FrameColor(frame, 1)};
            Check(FrameReleasedFirst(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS,
                  name + ", frame " + std::to_string(frame) + ": xrEndFrame");
            const ShownFrame& shown = g_runtime.frames.back();
            Check(shown.views.size() == 2, name + ": two views");
            for (int eye = 0; eye < 2 && shown.views.size() == 2; ++eye) {
                Check(Near(shown.views[eye].texel, colors[eye]),
                      name + ", frame " + std::to_string(frame) + " eye " + std::to_string(eye) + ": the runtime shows " +
                          (shown.views[eye].texel ? Text(*shown.views[eye].texel) : "nothing") +
                          ", rendered after the release " + Text(colors[eye]) + " (released holding " +
                          Text(kBeforeRendering) + ")");
            }
            if (const StandInSwapchain* chain = FindSwapchain(left.swapchain); chain && chain->released) {
                images.insert(*chain->released);
            }
        }
        Check(same_image ? images.size() == 1 : images.size() > 1,
              name + ": the runtime handed out " + std::to_string(images.size()) + " image(s)");
    }
    g_runtime.same_image = false;
}

// 2c. A game acquiring an image ahead (OpenXR lets it hold several and wait
// for each once the one before it is released): acquire A and B, wait for A,
// render and release it, then wait for B. The runtime gets A's release
// before B's wait, as it orders them, and shows B.
void CheckAcquireAhead(App& app, Game& game, Target& left, Target& right) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Check(app.api.wait_frame(app.session, &wait_info, &state) == XR_SUCCESS, "acquire ahead: xrWaitFrame");
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    Check(app.api.begin_frame(app.session, &begin_info) == XR_SUCCESS, "acquire ahead: xrBeginFrame");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = app.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{};
    for (XrView& view : located) view.type = XR_TYPE_VIEW;
    std::uint32_t located_count = 0;
    Check(app.api.locate_views(app.session, &locate, &view_state, 2, &located_count, located.data()) == XR_SUCCESS &&
              located_count == 2,
          "acquire ahead: xrLocateViews");
    const Color ahead = FrameColor(5, 0);
    const Color shown_left = FrameColor(6, 0);
    const Color shown_right = FrameColor(6, 1);
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    Check(app.api.acquire(left.swapchain, &acquire, &first) == XR_SUCCESS &&
              app.api.acquire(left.swapchain, &acquire, &second) == XR_SUCCESS && first != second &&
              second < left.images.size(),
          "acquire ahead: two images acquired");
    Check(app.api.wait(left.swapchain, &wait) == XR_SUCCESS, "acquire ahead: the first image's wait");
    Draw(game, left.images[first], ahead);
    Release(app, left);
    const XrResult second_wait = app.api.wait(left.swapchain, &wait);
    Check(second_wait == XR_SUCCESS,
          "acquire ahead: the second image's wait, after the first's release, succeeds (" +
              std::to_string(second_wait) + ")");
    Draw(game, left.images[second], shown_left);
    Release(app, left);
    const std::uint32_t right_index = Acquire(app, right);
    Draw(game, right.images[right_index], shown_right);
    Release(app, right);
    std::array<XrCompositionLayerProjectionView, 2> views{};
    const std::array<const Target*, 2> targets{&left, &right};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage.swapchain = targets[eye]->swapchain;
        views[eye].subImage.imageRect = {{0, 0},
                                         {static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight)}};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = app.space;
    projection.viewCount = 2;
    projection.views = views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = layers;
    Check(app.api.end_frame(app.session, &end) == XR_SUCCESS, "acquire ahead: xrEndFrame");
    const ShownFrame& shown = g_runtime.frames.back();
    Check(shown.views.size() == 2 && Near(shown.views[0].texel, shown_left) && Near(shown.views[1].texel, shown_right),
          "acquire ahead: the runtime shows the image released last");
}

// Quad views (XR_VARJO_quad_views): views 0/1 peripheral, 2/3 focus, each with
// a swapchain of its own.
using QuadTargets = std::array<Target, 4>;

// The four quad views at `time`, located through the layer.
std::array<XrView, 4> LocateQuadViews(App& app, XrTime time) {
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
    locate.displayTime = time;
    locate.space = app.space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 4> views{};
    for (XrView& view : views) view.type = XR_TYPE_VIEW;
    std::uint32_t count = 0;
    Check(app.api.locate_views(app.session, &locate, &state, 4, &count, views.data()) == XR_SUCCESS && count == 4,
          "xrLocateViews locates four quad views");
    return views;
}

// What QuadFrame does beyond rendering and submitting the frame.
struct QuadFrameOptions {
    // Direct3D 11 waits for the gate from just before xrEndFrame on: the
    // game's rendering of the frame runs, nothing the layer does with the
    // frame on Direct3D 11 (its composition) runs until the gate opens.
    Gate* hold{nullptr};
    std::int32_t trim{0}; // each view's image rectangle is this many texels narrower than its image
    // The stand-in's stereo fields of view, from the frame's xrLocateViews on.
    std::optional<std::array<XrFovf, 2>> fovs;
    XrTime* display_time{nullptr}; // where the frame's display time goes
    // The session may be lost: xrWaitFrame or xrBeginFrame returning
    // XR_ERROR_SESSION_LOST ends the frame there (returned as it is); any
    // other failure of theirs fails the test.
    bool session_may_be_lost{false};
};

// One application frame of quad views: the four views located at the frame's
// time, each periphery cleared to P(frame, eye) and each focus view to
// F(frame, eye) with stripes of StripeColor(frame, eye), released, then
// submitted as located in one projection.
XrResult QuadFrame(App& app, Game& game, const QuadTargets& targets, int frame, const QuadFrameOptions& options = {}) {
    const auto expect = [&](XrResult result, const char* call) {
        Check(result == XR_SUCCESS || (options.session_may_be_lost && result == XR_ERROR_SESSION_LOST),
              std::string(call) + ": " + std::to_string(result));
        return result;
    };
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (const XrResult result = expect(app.api.wait_frame(app.session, &wait_info, &state), "xrWaitFrame")) {
        return result;
    }
    if (options.display_time) {
        *options.display_time = state.predictedDisplayTime;
    }
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    if (const XrResult result = expect(app.api.begin_frame(app.session, &begin_info), "xrBeginFrame")) {
        return result;
    }
    if (options.fovs) {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.fovs = *options.fovs;
    }
    const std::array<XrView, 4> located = LocateQuadViews(app, state.predictedDisplayTime);

    std::array<std::uint32_t, 4> indices{};
    for (std::size_t i = 0; i < targets.size(); ++i) {
        indices[i] = Acquire(app, targets[i]);
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const int eye = static_cast<int>(i % 2);
        Draw(game, targets[i].images[indices[i]], i < 2 ? PeripheryColor(frame, eye) : FocusColor(frame, eye));
        if (i >= 2) {
            Stripes(game, targets[i].images[indices[i]], StripeColor(frame, eye));
        }
    }
    for (const Target& target : targets) {
        Release(app, target);
    }
    if (options.hold) {
        options.hold->HoldGame(game);
    }

    std::array<XrCompositionLayerProjectionView, 4> views{};
    for (std::size_t i = 0; i < views.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        targets[i].images[indices[i]]->GetDesc(&desc);
        views[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[i].pose = located[i].pose;
        views[i].fov = located[i].fov;
        views[i].subImage.swapchain = targets[i].swapchain;
        views[i].subImage.imageRect = {
            {0, 0}, {static_cast<std::int32_t>(desc.Width) - options.trim, static_cast<std::int32_t>(desc.Height)}};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = app.space;
    projection.viewCount = 4;
    projection.views = views.data();
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = 1;
    end.layers = layers;
    return app.api.end_frame(app.session, &end);
}

// --- The cases --------------------------------------------------------------

// The test's own reference to the Direct3D 12 device on the game's adapter,
// from the start of each case to its end. Direct3D 12 makes one device per
// adapter in a process, so the bridge's device (the session's) is this one,
// and what its debug layer reported is still there to check once the case's
// teardown has let everything of the layer's go.
ComPtr<ID3D12Device> g_device12;
std::string g_case; // begun, and its debug layers not yet checked

ComPtr<ID3D12Device> MakeDevice12(const Game& game) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(game.luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return nullptr;
    }
    ComPtr<ID3D12InfoQueue> queue;
    if (SUCCEEDED(device.As(&queue))) {
        StoreErrorsOnly(queue.Get());
    }
    return device;
}

bool SameObject(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> identity_a;
    ComPtr<IUnknown> identity_b;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&identity_a))) &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&identity_b))) && identity_a.Get() == identity_b.Get();
}

// Neither debug layer reported an error since the last check.
void CheckValidation(Game& game, const std::string& when) {
    const std::string errors =
        ValidationMessages(g_device12 ? g_device12.Get() : g_runtime.device12.Get(), game).All();
    Check(errors.empty(), g_case + " (" + when + "): the debug layers reported\n" + errors);
}

// Quad views cases: the stand-in's stereo views' recommended size, and
// Quadviews' focus scale, blend band and sharpening.
constexpr std::uint32_t kQuadStereoSize = 256;
constexpr double kQuadFocusScale = 1.1;
constexpr double kQuadTransitionPercent = 25.0;
constexpr double kQuadSharpness = 50.0;

// What a case's settings turn on beyond the defaults.
struct CaseSettings {
    bool framegen{true};
    bool debug_log{false};
    // Quadviews' stereo-composite emulation, the focus fixed at the centre.
    bool quadviews{false};
    // With quadviews: the diagnostic visualization's binding is Ctrl+F8
    // (Peer::HoldQuadViewsDiagnosticBinding presses it).
    bool diagnostic_binding{false};
    // Frame Generation's (its default when empty). The cases were written when
    // Auto was the default, so they keep it unless they choose another.
    std::string frame_multiplier{"auto"};
    std::string synthesis_mode;   // Frame Generation's (its default when empty)
};

// A fresh stand-in runtime, a log of the case's own, and the test's settings.
// Every case ends with EndCase.
void BeginCase(const std::string& name, const Game& game, const CaseSettings& case_settings) {
    Check(g_case.empty(), name + ": the case before it (" + g_case + ") ended with its debug layers checked");
    Check(Peer::Idle(), name + ": nothing is left of the cases before it");
    g_case = name;
    g_device12 = MakeDevice12(game);
    // The runtime's clock runs on across sessions, as a runtime's does.
    const XrTime time = g_runtime.time;
    g_runtime = StandIn{};
    g_runtime.time = time;
    g_runtime.luid = game.luid;
    g_framegen_chain = {};
    SetEnv("VECTORXR_LOG_PATH", (g_root / "logs" / name / "vectorxr.log").string());
    std::ofstream settings(g_root / "settings.json", std::ios::trunc);
    settings << R"({"version": 3, "core": {"enabled": true, "logLevel": ")"
             << (case_settings.debug_log ? "debug" : "info")
             << R"(", "trackSeenApps": false}, "applications": [], "modules": {"depthxr": {"enabled": false}, )"
             << R"("pivotxr": {"enabled": false}, )";
    if (case_settings.quadviews) {
        settings << R"("quadviews": {"enabled": true, )"
                 << (case_settings.diagnostic_binding
                         ? R"("diagnosticVisualizationBinding": {"type": "keyboard", "chord": ["Ctrl", "F8"]}, )"
                         : "")
                 << R"("defaults": {"trackingMode": "head", "focusScale": )"
                 << kQuadFocusScale << R"(, "transitionThicknessPercent": )" << kQuadTransitionPercent
                 << R"(, "foveateSharpness": )" << kQuadSharpness << "}}, ";
    } else {
        settings << R"("quadviews": {"enabled": false}, )";
    }
    settings << R"("turbo": {"enabled": false}, "framegen": {"enabled": )"
             << (case_settings.framegen ? "true" : "false");
    if (!case_settings.frame_multiplier.empty() || !case_settings.synthesis_mode.empty()) {
        settings << R"(, "defaults": {)";
        if (!case_settings.frame_multiplier.empty()) {
            settings << R"("frameMultiplier": ")" << case_settings.frame_multiplier << R"(")"
                     << (case_settings.synthesis_mode.empty() ? "" : ", ");
        }
        if (!case_settings.synthesis_mode.empty()) {
            settings << R"("synthesisMode": ")" << case_settings.synthesis_mode << R"(")";
        }
        settings << "}";
    }
    settings << "}}}\n";
}

void BeginCase(const std::string& name, const Game& game, bool framegen = true, bool debug_log = false) {
    CaseSettings settings;
    settings.framegen = framegen;
    settings.debug_log = debug_log;
    BeginCase(name, game, settings);
}

// After the case's teardown: neither debug layer reported an error during the
// case. `device_removed`: the case removed the Direct3D 12 device, and what
// its debug layer reported since the last check is the removal's.
void EndCase(Game& game, bool device_removed = false) {
    Check(Peer::Idle(), g_case + ": nothing is left of the case");
    const Validation validation = ValidationMessages(g_device12 ? g_device12.Get() : nullptr, game);
    const std::string errors = device_removed ? validation.d3d11 : validation.All();
    Check(errors.empty(), g_case + " (after its teardown): the debug layers reported\n" + errors);
    g_device12.Reset();
    g_case.clear();
}

const XrGraphicsBindingD3D12KHR* BridgeBinding() {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(Peer::Bridge().Binding());
}

// 1. A Direct3D 11 game's session with Frame Generation: the runtime gets a
// Direct3D 12 session on the game's adapter, with the queue substitution's
// queue, never the bridge's. False when the bridge cannot share images here.
bool CheckRouting(Game& game, SessionCreate& create) {
    const std::string log = Peer::Log();
    if (g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && Contains(log, "shares no typeless images")) {
        std::cout << "skipped: the driver shares no typeless images between Direct3D 11 and 12\n";
        return false;
    }
    const XrGraphicsBindingD3D12KHR* bridge = BridgeBinding();
    Check(bridge && Peer::Bridge().Active(), "the session is bridged");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,
          "the runtime gets a Direct3D 12 binding first: " + std::to_string(g_runtime.binding));
    Check(SameLuid(g_runtime.device12->GetAdapterLuid(), game.luid), "the session's device is on the game's adapter");
    Check(g_runtime.device12.Get() == bridge->device, "the session's device is the bridge's");
    Check(!g_device12 || SameObject(g_device12.Get(), bridge->device),
          "the bridge's device is the one the test holds for its debug-layer checks");
    Check(g_runtime.queue.Get() != bridge->queue, "the runtime is never given the bridge's queue");
    const D3D12_COMMAND_QUEUE_DESC queue = g_runtime.queue->GetDesc();
    Check(queue.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && queue.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
              Peer::QueueSubstituted(),
          "the runtime's queue is the substitution's high-priority direct queue");
    const std::ptrdiff_t requirements = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
    const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:D3D12");
    Check(requirements >= 0 && requirements < session,
          "the Direct3D 12 requirements are asked for before the session is made");
    Check(g_runtime.binding_next == &create.application_struct,
          "the application's other structures follow the binding");
    // Frame Generation: made from the bridge's binding (capturing on its
    // queue, the game's), writing on the runtime's queue.
    Check(Peer::FrameGenOwnsPacing(), "Frame Generation runs the bridged session");
    Check(g_framegen_chain.first == depthxr::kFrameGenRuntimeQueueType &&
              g_framegen_chain.queue == g_runtime.queue.Get() && g_framegen_chain.next == bridge,
          "Frame Generation is made from the bridge's binding, with the runtime's queue");
    Check(Contains(log, "Direct3D 11 bridge: XR_KHR_D3D12_enable added to the instance's extensions.") &&
              Contains(log, "Frame Generation gives the runtime a Direct3D 12 queue of its own"),
          "the log tells the session's bridge and queue");
    Check(Count(log, std::string("Frame Generation's Direct3D 11 bridge: ") + kBridgedSessionLine) == 1,
          "the log tells once that the session is bridged");
    Check(Count(log, kGameGraphicsLine) == 1 && Count(log, kBridgeGraphicsLine) == 1 &&
              Count(log, kGraphicsRuntime) == 2,
          "the log names the hardware of the game's Direct3D 11 device and of the bridge's Direct3D 12 one, once each");
    return true;
}

// 2. Swapchains on a bridged session.
void CheckSwapchains(App& app, Game& game) {
    // A multisampled one: the runtime's are single-sampled copy destinations.
    std::size_t requests = g_runtime.swapchain_requests.size();
    XrSwapchain msaa = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo msaa_info = ColorInfo(4);
    Check(app.api.create_swapchain(app.session, &msaa_info, &msaa) == XR_SUCCESS, "a multisampled swapchain");
    Check(g_runtime.swapchain_requests.size() == requests + 1, "one runtime swapchain for it");
    const XrSwapchainCreateInfo& asked = g_runtime.swapchain_requests.back();
    Check((asked.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
              (asked.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) && asked.sampleCount == 1 &&
              asked.width == kWidth && asked.height == kHeight && asked.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
          "the runtime's swapchain is a single-sampled colour attachment and copy destination");
    // The two-call idiom, on the bridge's images.
    std::uint32_t count = 0;
    Check(app.api.enumerate_images(msaa, 0, &count, nullptr) == XR_SUCCESS && count == kImages, "the images counted");
    std::vector<XrSwapchainImageD3D11KHR> short_array(kImages - 1, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    count = 0;
    Check(app.api.enumerate_images(msaa, kImages - 1, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(short_array.data())) ==
                  XR_ERROR_SIZE_INSUFFICIENT &&
              count == kImages,
          "too short an array is refused, with the count");
    std::vector<XrSwapchainImageD3D12KHR> d3d12_array(kImages, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    Check(app.api.enumerate_images(msaa, kImages, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d12_array.data())) ==
              XR_ERROR_VALIDATION_FAILURE,
          "Direct3D 12 images are refused: the application's are Direct3D 11");
    Target msaa_target{msaa, Images(app, msaa)};
    const StandInSwapchain* runtime = FindSwapchain(msaa);
    Check(runtime && runtime->images12.size() == kImages, "the runtime made Direct3D 12 images");
    for (ID3D11Texture2D* image : msaa_target.images) {
        for (const ComPtr<ID3D12Resource>& runtime_image : runtime->images12) {
            Check(static_cast<void*>(image) != static_cast<void*>(runtime_image.Get()),
                  "the application's images are not the runtime's");
        }
        ComPtr<ID3D11Device> device;
        image->GetDevice(&device);
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        Check(device.Get() == game.device.Get() && desc.SampleDesc.Count == 4 && desc.Width == kWidth,
              "the application renders four samples on its own device");
    }
    // Its frame reaches the runtime resolved.
    const Color color{90, 180, 30, 255};
    Check(Frame(app, game, {&msaa_target}, {color}, {{{&msaa_target, 0}, {&msaa_target, 0}}}) == XR_SUCCESS,
          "a frame of the multisampled swapchain");
    for (const ShownView& view : g_runtime.frames.back().views) {
        Check(Near(view.texel, color),
              "the resolved frame reaches the runtime: " + (view.texel ? Text(*view.texel) : "none"));
    }
    Check(app.api.destroy_swapchain(msaa) == XR_SUCCESS && g_runtime.destroyed.back() == msaa,
          "the multisampled swapchain is destroyed");

    // A runtime refusing the copy usage is asked again without it.
    g_runtime.refuse_transfer_dst = true;
    requests = g_runtime.swapchain_requests.size();
    XrSwapchain plain = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo plain_info = ColorInfo();
    Check(app.api.create_swapchain(app.session, &plain_info, &plain) == XR_SUCCESS && plain != XR_NULL_HANDLE,
          "a swapchain the runtime makes without the copy usage");
    Check(g_runtime.swapchain_requests.size() == requests + 2 &&
              (g_runtime.swapchain_requests[requests].usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) &&
              !(g_runtime.swapchain_requests[requests + 1].usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) &&
              (g_runtime.swapchain_requests[requests + 1].usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT),
          "the second request drops TRANSFER_DST only");
    Check(Peer::Bridge().Bridged(plain), "it is bridged all the same");
    Check(Contains(Peer::Log(), "Runtime rejected the bridge's copy usage"), "the retry is logged");
    Check(app.api.destroy_swapchain(plain) == XR_SUCCESS, "it is destroyed");
    g_runtime.refuse_transfer_dst = false;

    // Runtime images that do not match are refused, and the runtime's swapchain goes.
    g_runtime.width_error = 2;
    XrSwapchain mismatched = XR_NULL_HANDLE;
    const XrResult result = app.api.create_swapchain(app.session, &plain_info, &mismatched);
    g_runtime.width_error = 0;
    Check(result == XR_ERROR_RUNTIME_FAILURE && mismatched == XR_NULL_HANDLE,
          "images the bridge cannot share fail the swapchain: " + std::to_string(result));
    Check(!g_runtime.created.empty() && g_runtime.destroyed.back() == g_runtime.created.back(),
          "the runtime's swapchain is destroyed again");
    const std::string log = Peer::Log();
    Check(Contains(log, "refuses a swapchain it cannot share") && Contains(log, "does not match"),
          "the refusal is logged with the reason");
}

// 3. Frames: what the runtime shows on its queue is what the game rendered
// that frame, the runtime's queue ordered after the bridge's copies (through
// the queue substitution). In two frames the game is held back from just
// before the last eye's release (the right eye's, then the left's) until the
// runtime's readback is seen waiting for it: the runtime's queue must wait
// for that eye's own copy, not only for what was queued before its release.
void CheckFrames(App& app, Game& game, Target& left, Target& right) {
    for (const Target* target : {&left, &right}) {
        const StandInSwapchain* runtime = FindSwapchain(target->swapchain);
        Check(runtime != nullptr, "the runtime made the eye's swapchain");
        for (ID3D11Texture2D* image : target->images) {
            D3D11_TEXTURE2D_DESC desc{};
            image->GetDesc(&desc);
            Check(desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.SampleDesc.Count == 1 &&
                      (desc.BindFlags & D3D11_BIND_RENDER_TARGET),
                  "an eye's images are typeless twins the game renders into");
            for (const ComPtr<ID3D12Resource>& runtime_image : runtime->images12) {
                Check(static_cast<void*>(image) != static_cast<void*>(runtime_image.Get()),
                      "the application's images are not the runtime's");
            }
        }
    }
    constexpr int kFrames = 10;
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::vector<Color> colors{FrameColor(frame, 0), FrameColor(frame, 1)};
        std::optional<Gate> gate;
        if (frame == 2 || frame == 6) {
            gate.emplace(g_runtime.device12.Get(), game);
        }
        // Frame 6 releases the right eye first: the left is the one held.
        const bool right_first = frame == 6;
        const int held_eye = right_first ? 0 : 1;
        const std::vector<const Target*> order =
            right_first ? std::vector<const Target*>{&right, &left} : std::vector<const Target*>{&left, &right};
        const std::vector<Color> order_colors = right_first ? std::vector<Color>{colors[1], colors[0]} : colors;
        const XrResult result =
            Frame(app, game, order, order_colors, {{{&left, 0}, {&right, 0}}}, nullptr, gate ? &*gate : nullptr);
        Check(result == XR_SUCCESS, "frame " + std::to_string(frame) + ": xrEndFrame " + std::to_string(result));
        const ShownFrame& shown = g_runtime.frames.back();
        Check(shown.views.size() == 2, "frame " + std::to_string(frame) + ": two views");
        if (gate) {
            // First, as what decides it (the texels below may race a copy
            // the runtime's queue does not wait for): the readback was still
            // pending while the eye released last was held back...
            Check(shown.held, "frame " + std::to_string(frame) + ": the runtime's readback still waits for eye " +
                                  std::to_string(held_eye) + ", released last and held back");
            Check(shown.unordered.size() == 2 && !Near(shown.unordered[held_eye], colors[held_eye]),
                  "frame " + std::to_string(frame) + ": read on a queue ordered after nothing meanwhile, eye " +
                      std::to_string(held_eye) + "'s image does not hold the frame yet");
        }
        // ...and, once the gate opened, it read what the game rendered.
        for (int eye = 0; eye < 2; ++eye) {
            const ShownView& view = shown.views[eye];
            Check(view.swapchain == (eye ? right : left).swapchain && !view.depth,
                  "frame " + std::to_string(frame) + ": the eye's view as submitted");
            Check(Near(view.texel, colors[eye]),
                  "frame " + std::to_string(frame) + " eye " + std::to_string(eye) + ": the runtime shows " +
                      (view.texel ? Text(*view.texel) : "nothing") + ", rendered " + Text(colors[eye]));
        }
    }
    for (const Target* target : {&left, &right}) {
        const std::string describe = Peer::Bridge().Describe(target->swapchain);
        Check(Contains(describe, "copies=" + std::to_string(kFrames)) && Contains(describe, "skippedCopies=0"),
              "a copy per frame: " + describe);
    }
}

// 4. Depth on a bridged session stays the game's: its swapchain's calls reach
// the runtime, its depth never reaches submitted frames.
void CheckDepth(App& app, Game& game, Target& left, Target& right, Target& depth) {
    Check(Peer::Bridge().Bridged(depth.swapchain) && Peer::Bridge().Unmirrored(depth.swapchain),
          "the depth swapchain is the game's own");
    for (ID3D11Texture2D* image : depth.images) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        Check((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) && desc.ArraySize == 2 &&
                  desc.Format == DXGI_FORMAT_R32_TYPELESS,
              "the game gets typeless depth images");
    }
    constexpr int kFrames = 8;
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::vector<Color> colors{FrameColor(frame + 10, 0), FrameColor(frame + 10, 1)};
        const XrResult result = Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}, &depth,
                                     nullptr, true); // explicit unknown metadata: preserve it in pass-through
        Check(result == XR_SUCCESS, "depth frame " + std::to_string(frame) + ": xrEndFrame " + std::to_string(result));
        const ShownFrame& shown = g_runtime.frames.back();
        Check(shown.views.size() == 2, "depth frame: two views");
        for (int eye = 0; eye < 2; ++eye) {
            const ShownView& view = shown.views[eye];
            Check(!view.depth, "the runtime gets no depth in a bridged session's views");
            Check(view.next_type == kApplicationStructType,
                  "the rest of a view's chain reaches the runtime: " + std::to_string(view.next_type));
            Check(Near(view.texel, colors[eye]),
                  "depth frame " + std::to_string(frame) + " eye " + std::to_string(eye) + ": the runtime shows " +
                      (view.texel ? Text(*view.texel) : "nothing") + ", rendered " + Text(colors[eye]));
        }
    }
    const StandInSwapchain* runtime = FindSwapchain(depth.swapchain);
    Check(runtime && runtime->acquires == kFrames && runtime->waits == kFrames && runtime->releases == kFrames,
          "the depth swapchain's acquires, waits and releases reach the runtime");
    Check(Contains(Peer::Log(), "keeps the game's depth to itself"), "the log tells depth is left out");
    // CheckFrames' 10 frames, CheckRenderAfterRelease's 16 and
    // CheckAcquireAhead's two releases before these.
    Check(Contains(Peer::Bridge().Describe(left.swapchain), "copies=" + std::to_string(10 + 16 + 2 + kFrames)),
          "colour is still copied: " + Peer::Bridge().Describe(left.swapchain));
}

// 5. Teardown: the runtime frees nothing the bridge's queue still writes.
void CheckTeardown(App& app, Game& game, Target& left, Target& right) {
    ID3D12Device* device = g_runtime.device12.Get();
    ID3D12CommandQueue* bridge_queue = BridgeBinding()->queue;
    {
        // The left eye's last copy waits behind a gate the layer's wait outlasts.
        Gate gate(device, game);
        gate.HoldQueue(bridge_queue);
        Render(app, game, left, {1, 2, 3, 255});
        bool destroyed = false;
        std::string summary;
        g_runtime.on_destroy_swapchain = [&](XrSwapchain swapchain) {
            if (swapchain == left.swapchain) {
                destroyed = true;
                summary = Peer::Bridge().Summary();
            }
        };
        std::thread opener = gate.OpenLater(300);
        const XrResult result = app.api.destroy_swapchain(left.swapchain);
        opener.join();
        g_runtime.on_destroy_swapchain = nullptr;
        Check(result == XR_SUCCESS && destroyed, "the runtime's swapchain is destroyed with the application's");
        Check(SummaryValue(summary, "doneCompleted") == SummaryValue(summary, "doneValue"),
              "the bridge's copies were done when the runtime destroyed its swapchain: " + summary);
        Check(!Peer::Bridge().Bridged(left.swapchain), "the bridge let the swapchain go");
    }

    Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
    app.space = XR_NULL_HANDLE;
    Check(app.api.end_session(app.session) == XR_SUCCESS, "xrEndSession");
    // The right eye (and depth) are left to the session; its last copy waits
    // behind a gate, and a marker after everything on the bridge's queue.
    Gate gate(device, game);
    gate.HoldQueue(bridge_queue);
    Render(app, game, right, {4, 5, 6, 255});
    ComPtr<ID3D12Fence> marker;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))), "a marker fence");
    bridge_queue->Signal(marker.Get(), 1);
    bool marker_done = false;
    bool idle = false;
    std::string summary;
    g_runtime.on_destroy_session = [&] {
        marker_done = marker->GetCompletedValue() >= 1;
        summary = Peer::Bridge().Summary();
        // A signal queued now on the idle queue runs at once (polled briefly:
        // even an idle queue takes a moment to run it). The gate would hold
        // a queue the layer had not waited for far longer.
        for (int poll = 0; poll < 50 && !idle; ++poll) {
            idle = Peer::Bridge().WaitIdle(poll ? 1 : 0);
        }
    };
    std::thread opener = gate.OpenLater(300);
    const XrResult result = app.api.destroy_session(app.session);
    opener.join();
    app.session = XR_NULL_HANDLE;
    Check(result == XR_SUCCESS && g_runtime.Position("xrDestroySession") >= 0, "xrDestroySession");
    Check(marker_done, "the bridge's queue had run everything queued on it when the runtime destroyed the session");
    Check(SummaryValue(summary, "doneCompleted") == SummaryValue(summary, "doneValue"),
          "the bridge's copies were done when the runtime destroyed the session: " + summary);
    Check(idle, "the bridge's queue was idle when the runtime destroyed the session");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(),
          "the bridge and the substitution end with the session");
    const std::string log = Peer::Log();
    Check(Contains(log, "Frame Generation's Direct3D 11 bridge at session end: "), "the bridge's summary is logged");
    Check(!Contains(log, "did not finish") && !Contains(log, "kept until its session ends") &&
              !Contains(log, "[error] Direct3D 11 bridge:") && !Contains(log, "refused a held-back"),
          "nothing was stalled, skipped or refused");
    Check(Count(log, kBridgedSessionLine) == 1, "the session's bridge was logged once, for the whole session");
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS && g_runtime.Position("xrDestroyInstance") >= 0,
          "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
    Check(GameRuns(game), "the game's device works after teardown");
}

// Cases 1 to 5, in one session. False when this machine cannot bridge.
bool TestBridgedSession(Game& game) {
    BeginCase("bridged-session", game);
    App app = StartApp();
    Check(g_runtime.instance_attempts.size() == 1 &&
              g_runtime.instance_attempts[0] == std::vector<std::string>{kD3D11Extension, kD3D12Extension},
          "the instance gets XR_KHR_D3D12_enable");
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    if (!CheckRouting(game, create)) {
        StopApp(app);
        EndCase(game);
        return false;
    }
    BeginSession(app);
    CheckSwapchains(app, game);
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    CheckFrames(app, game, left, right);
    CheckRenderAfterRelease(app, game, left, right);
    CheckAcquireAhead(app, game, left, right);
    Target depth = MakeTarget(app, DepthInfo(2), "depth");
    CheckDepth(app, game, left, right, depth);
    CheckValidation(game, "cases 1 to 4");
    CheckTeardown(app, game, left, right);
    EndCase(game);
    return true;
}

// 1a. VECTORXR_FRAMEGEN_QUEUE=game: the runtime would get the bridge's queue,
// so the session stays Direct3D 11, asking for its requirements again.
void TestKeepGameQueue(Game& game) {
    BeginCase("game-queue", game);
    SetEnv("VECTORXR_FRAMEGEN_QUEUE", "game");
    App app = StartApp();
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && g_runtime.device11.Get() == game.device.Get() &&
              g_runtime.binding_next == &create.application_struct,
          "the runtime gets the application's Direct3D 11 binding");
    const std::ptrdiff_t d3d12 = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
    const std::ptrdiff_t d3d11 = g_runtime.Position("xrGetD3D11GraphicsRequirementsKHR", d3d12 + 1);
    const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:D3D11");
    Check(d3d12 >= 0 && d3d11 > d3d12 && session > d3d11 && g_runtime.Position("xrCreateSession:D3D12") < 0,
          "the Direct3D 11 requirements are asked for again after the Direct3D 12 ones, before the session");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(), "no bridge, no substitution");
    const std::string log = Peer::Log();
    Check(Contains(log, "the runtime would be given the bridge's queue"), "the log tells why");
    Check(!Contains(log, kBridgedSessionLine), "the log does not tell of a bridged session");
    StopApp(app);
    SetEnv("VECTORXR_FRAMEGEN_QUEUE", "");
    EndCase(game);
}

// 1c. VECTORXR_D3D11_BRIDGE=off: the session is the game's Direct3D 11 one,
// with Frame Generation on it, and the log tells why.
void TestBridgeOffSession(Game& game) {
    BeginCase("session-bridge-off", game);
    SetEnv("VECTORXR_D3D11_BRIDGE", "off");
    App app = StartApp();
    Check(g_runtime.instance_attempts.size() == 1 &&
              g_runtime.instance_attempts[0] == std::vector<std::string>{kD3D11Extension},
          "the instance gets no XR_KHR_D3D12_enable: " + Text(g_runtime.instance_attempts));
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && g_runtime.device11.Get() == game.device.Get() &&
              g_runtime.binding_next == &create.application_struct &&
              g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0 &&
              g_runtime.Position("xrCreateSession:D3D12") < 0,
          "the runtime gets the application's Direct3D 11 binding, and nothing of Direct3D 12");
    Check(!Peer::Bridge().Active(), "no bridge");
    Check(g_framegen_chain.first == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, "Frame Generation runs on Direct3D 11");
    const std::string log = Peer::Log();
    Check(Count(log, "Frame Generation keeps this Direct3D 11 session as it is: VECTORXR_D3D11_BRIDGE=off.") == 1,
          "the log tells why, once");
    Check(!Contains(log, kBridgedSessionLine), "the log does not tell of a bridged session");
    Check(Count(log, kGameGraphicsLine) == 1 && !Contains(log, kBridgeGraphicsLine),
          "the log names the hardware of the game's Direct3D 11 device once, and of no bridge");
    BeginSession(app);
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    const std::vector<Color> colors{FrameColor(4, 0), FrameColor(4, 1)};
    Check(Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS,
          "a Direct3D 11 frame");
    for (int eye = 0; eye < 2; ++eye) {
        const ShownView& view = g_runtime.frames.back().views[eye];
        Check(Near(view.texel, colors[eye]), "the Direct3D 11 runtime shows " +
                                                 (view.texel ? Text(*view.texel) : "nothing") + ", rendered " +
                                                 Text(colors[eye]));
    }
    Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS &&
              app.api.destroy_swapchain(right.swapchain) == XR_SUCCESS,
          "the swapchains are destroyed");
    StopApp(app);
    SetEnv("VECTORXR_D3D11_BRIDGE", "");
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}

// 1b. A runtime refusing the Direct3D 12 session gets the game's Direct3D 11
// one, which works as it would without the bridge.
void TestRefusedD3D12Session(Game& game) {
    BeginCase("refused-d3d12-session", game);
    g_runtime.refuse_d3d12_session = true;
    App app = StartApp();
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS,
          "xrCreateSession succeeds with Direct3D 11");
    const std::ptrdiff_t d3d12 = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
    const std::ptrdiff_t refused = g_runtime.Position("xrCreateSession:D3D12");
    const std::ptrdiff_t d3d11 = g_runtime.Position("xrGetD3D11GraphicsRequirementsKHR", refused + 1);
    const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:D3D11");
    Check(d3d12 >= 0 && refused > d3d12 && d3d11 > refused && session > d3d11,
          "the Direct3D 11 requirements are asked for again before the retry");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
              g_runtime.binding_next == &create.application_struct,
          "the retry has the application's own binding");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(), "no bridge, no substitution");
    Check(g_framegen_chain.first == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, "Frame Generation runs on Direct3D 11");
    const std::string log = Peer::Log();
    Check(Contains(log, "xrCreateSession refused the bridged Direct3D 12 session"), "the retry is logged");
    Check(!Contains(log, kBridgedSessionLine), "the log does not tell of a bridged session");
    BeginSession(app);
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    for (const Target* target : {&left, &right}) {
        const StandInSwapchain* runtime = FindSwapchain(target->swapchain);
        Check(runtime && runtime->images11.size() == kImages, "the runtime made Direct3D 11 images");
        for (std::size_t i = 0; i < kImages; ++i) {
            Check(target->images[i] == runtime->images11[i].Get(), "the application renders into the runtime's images");
        }
    }
    for (int frame = 0; frame < 3; ++frame) {
        const std::vector<Color> colors{FrameColor(frame, 0), FrameColor(frame, 1)};
        Check(Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS,
              "a Direct3D 11 frame");
        for (int eye = 0; eye < 2; ++eye) {
            const ShownView& view = g_runtime.frames.back().views[eye];
            Check(Near(view.texel, colors[eye]), "the Direct3D 11 runtime shows " +
                                                     (view.texel ? Text(*view.texel) : "nothing") + ", rendered " +
                                                     Text(colors[eye]));
        }
    }
    Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS &&
              app.api.destroy_swapchain(right.swapchain) == XR_SUCCESS,
          "the swapchains are destroyed");
    StopApp(app);
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}

// One instance of an application asking for `extensions`: the result and the
// extensions the runtime was asked for, attempt by attempt. A created
// instance is destroyed again.
XrResult InstanceAttempts(const std::vector<const char*>& extensions, std::vector<std::vector<std::string>>& attempts,
                          std::string& log) {
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult result = CreateInstance(extensions, instance);
    attempts = g_runtime.instance_attempts;
    log = Peer::Log();
    if (XR_SUCCEEDED(result)) {
        Check(LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance")(instance) == XR_SUCCESS,
              "xrDestroyInstance");
    }
    Check(Peer::Idle(), "nothing is left of the instance");
    return result;
}

// 6. XR_KHR_D3D12_enable at xrCreateInstance.
void TestInstanceExtensions(Game& game) {
    using Names = std::vector<std::string>;
    std::vector<Names> attempts;
    std::string log;
    const std::string added = "XR_KHR_D3D12_enable added to the instance's extensions";

    BeginCase("instance-offered", game);
    g_runtime.extensions = {"XR_KHR_composition_layer_depth", kD3D11Extension, kD3D12Extension};
    XrResult result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension, kD3D12Extension} &&
              Contains(log, added + "."),
          "added when the runtime offers it: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-not-offered", game);
    g_runtime.extensions = {kD3D11Extension, "XR_KHR_composition_layer_depth"};
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension} &&
              !Contains(log, added) && Contains(log, "the runtime does not offer XR_KHR_D3D12_enable"),
          "not added when a believable list lacks it: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-empty-list", game);
    g_runtime.extensions = {};
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension, kD3D12Extension} &&
              Contains(log, "not believable"),
          "added when the list is empty: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-no-list", game);
    g_runtime.enumeration_offered = false;
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension, kD3D12Extension},
          "added when there is no list: " + Text(attempts));
    EndCase(game);

    // A layer made from mbucchia's template (OpenXR Toolkit) calls through a
    // null pointer when asked for a function before its own instance exists
    // (issue 108): the layer never asks it, and adds the extension as when
    // there is no list.
    BeginCase("instance-template-layer-below", game);
    depthxr::XrApiLayerNextInfo toolkit{};
    toolkit.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    toolkit.structVersion = depthxr::XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    toolkit.structSize = sizeof(toolkit);
    std::strncpy(toolkit.layerName, "XR_APILAYER_MBUCCHIA_toolkit", XR_MAX_API_LAYER_NAME_SIZE - 1);
    toolkit.nextGetInstanceProcAddr = RtGetInstanceProcAddr;
    toolkit.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
    g_layer_below = &toolkit;
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    g_layer_below = nullptr;
    Check(result == XR_SUCCESS && g_runtime.pre_instance_queries == 0 && attempts.size() == 1 &&
              attempts[0] == Names{kD3D11Extension, kD3D12Extension},
          "nothing asked of a template layer before the instance, the extension added: " + Text(attempts));
    Check(Contains(log, "OpenXR API layers below VectorXR: [XR_APILAYER_MBUCCHIA_toolkit]") &&
              Contains(log, "OpenXR Toolkit is loaded below VectorXR."),
          "the log names the layer below and where OpenXR Toolkit is");
    EndCase(game);

    BeginCase("instance-bridge-off", game);
    SetEnv("VECTORXR_D3D11_BRIDGE", "off");
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    SetEnv("VECTORXR_D3D11_BRIDGE", "");
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension} &&
              !Contains(log, "Direct3D 11 bridge"),
          "not added with VECTORXR_D3D11_BRIDGE=off: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-framegen-off", game, false);
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension} &&
              !Contains(log, "Direct3D 11 bridge"),
          "not added without Frame Generation: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-vulkan", game);
    result = InstanceAttempts({"XR_KHR_vulkan_enable2"}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 &&
              attempts[0] == Names{"XR_KHR_vulkan_enable2", kD3D12Extension} &&
              Contains(log, "Frame Generation's Vulkan bridge: " + added) && !Contains(log, "Direct3D 11 bridge"),
          "added for a Vulkan application, for the Vulkan bridge: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-own-d3d12", game);
    result = InstanceAttempts({kD3D11Extension, kD3D12Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kD3D11Extension, kD3D12Extension},
          "not added twice: " + Text(attempts));
    EndCase(game);

    BeginCase("instance-runtime-failure", game);
    g_runtime.instance_failure = XR_ERROR_RUNTIME_FAILURE;
    result = InstanceAttempts({kD3D11Extension}, attempts, log);
    Check(result == XR_ERROR_RUNTIME_FAILURE && attempts.size() == 1 &&
              attempts[0] == Names{kD3D11Extension, kD3D12Extension},
          "another failure is the application's, with no retry: " + Text(attempts));
    EndCase(game);

    // Refused: the very next attempt drops it alone (the turbo clock, another
    // of the layer's additions, stays), and the instance's sessions are not
    // bridged.
    BeginCase("instance-refused", game, true, true);
    g_runtime.extensions = {kD3D11Extension, kD3D12Extension, kClockExtension};
    g_runtime.refuse_d3d12_instance = true;
    App app = StartApp();
    attempts = g_runtime.instance_attempts;
    Check(attempts.size() == 2 && attempts[0] == Names{kD3D11Extension, kD3D12Extension, kClockExtension} &&
              attempts[1] == Names{kD3D11Extension, kClockExtension},
          "the retry drops XR_KHR_D3D12_enable alone: " + Text(attempts));
    Check(Contains(Peer::Log(), "XR_KHR_D3D12_enable left out after the runtime refused an extension"),
          "the refusal is logged");
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
              g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0 && !Peer::Bridge().Active(),
          "the session is not bridged");
    const std::string session_log = Peer::Log();
    Check(Contains(session_log, "the instance has no XR_KHR_D3D12_enable"), "the log tells why");
    Check(!Contains(session_log, kBridgedSessionLine), "the log does not tell of a bridged session");
    StopApp(app);
    EndCase(game);
}

// 7. The instance destroyed with its bridged session still alive (as a game
// that quits may): the runtime frees the session's images with the instance,
// so the bridge's copies into them are done before it is called.
void TestDestroyInstanceWithSession(Game& game) {
    BeginCase("instance-with-session", game);
    App app = StartApp();
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session");
    BeginSession(app);
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    const std::vector<Color> colors{FrameColor(3, 0), FrameColor(3, 1)};
    Check(Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS &&
              Near(g_runtime.frames.back().views[0].texel, colors[0]) &&
              Near(g_runtime.frames.back().views[1].texel, colors[1]),
          "a frame of the bridged session");
    // The left eye's last copy waits behind a gate the layer's wait outlasts,
    // and a marker after everything on the bridge's queue.
    ID3D12CommandQueue* bridge_queue = BridgeBinding()->queue;
    Gate gate(g_runtime.device12.Get(), game);
    gate.HoldQueue(bridge_queue);
    Render(app, game, left, {7, 8, 9, 255});
    ComPtr<ID3D12Fence> marker;
    Check(SUCCEEDED(g_runtime.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))),
          "a marker fence");
    bridge_queue->Signal(marker.Get(), 1);
    bool destroyed = false;
    g_runtime.on_destroy_instance = [&] {
        destroyed = true;
        // Checked here, before the stand-in frees the session's images.
        const std::string summary = Peer::Bridge().Summary();
        Check(marker->GetCompletedValue() >= 1,
              "the bridge's queue had run everything queued on it when the runtime destroyed the instance (with "
              "its session's images): " +
                  summary);
        Check(SummaryValue(summary, "doneCompleted") == SummaryValue(summary, "doneValue"),
              "the bridge's copies were done when the runtime destroyed the instance: " + summary);
    };
    std::thread opener = gate.OpenLater(300);
    const XrResult result = app.api.destroy_instance(app.api.instance);
    opener.join();
    g_runtime.on_destroy_instance = nullptr;
    Check(result == XR_SUCCESS && destroyed, "xrDestroyInstance with the session alive");
    Check(g_runtime.Position("xrDestroySession") < 0, "the session went with the instance, not on its own");
    Check(Peer::Idle() && !Peer::QueueSubstituted(), "nothing is left of the instance and its session");
    const std::string log = Peer::Log();
    Check(Count(log, kBridgedSessionLine) == 1, "the session's bridge was logged once");
    Check(Contains(log, "Frame Generation's Direct3D 11 bridge at session end: "), "the bridge's summary is logged");
    Check(!Contains(log, "did not finish") && !Contains(log, "[error] Direct3D 11 bridge:"),
          "nothing was stalled");
    Check(GameRuns(game), "the game's device works after its instance went with the session");
    EndCase(game);
}

// Each channel of `texel` lies strictly between those of `a` and `b` where
// they differ enough to tell (a blend of the two).
bool Between(const std::optional<Color>& texel, const Color& a, const Color& b) {
    if (!texel) return false;
    for (int i = 0; i < 3; ++i) {
        const int low = std::min(a[i], b[i]);
        const int high = std::max(a[i], b[i]);
        if (high - low >= 8 && ((*texel)[i] <= low + 1 || (*texel)[i] >= high - 1)) return false;
    }
    return true;
}

depthxr::FrameGenFov ToFov(const XrFovf& fov) {
    return {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
}

// 9. Quad views on a bridged session, as DCS renders them: the game renders
// four views (XR_VARJO_quad_views), the layer composes each eye from its
// periphery and focus view with its Direct3D 11 compositor, and the stereo
// runtime gets two views, the compositor's outputs: at their centres the
// focus colour, at a corner the periphery's and in the blend band a blend of
// the two, all of the frame just ended. Frame Generation toggled off, the
// frames are shown as they are; at 2x (the stand-in backend, inline), engaged
// after two steady frames, each frame also has a frame of Frame Generation's
// own outputs before it, and the diagnostic overlay stays out of the
// compositor's images it generates from. The compositor's readbacks are
// printed as a digest to compare builds by.
void TestBridgedQuadViewsComposed(Game& game, bool framegen) {
    CaseSettings settings;
    settings.quadviews = true;
    settings.frame_multiplier = framegen ? "2x" : "";
    BeginCase(framegen ? "quad-views-framegen" : "quad-views", game, settings);
    g_runtime.recommended_width = g_runtime.recommended_height = kQuadStereoSize;
    g_runtime.paced = true;
    if (framegen) {
        Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    }
    App app = StartApp({kD3D11Extension, kQuadViewsExtension, kFoveatedExtension});
    std::uint32_t count = 0;
    std::array<XrViewConfigurationView, 4> configuration{};
    for (XrViewConfigurationView& view : configuration) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    Check(app.api.enumerate_views(app.api.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 0,
                                  &count, nullptr) == XR_SUCCESS &&
              count == 4 &&
              app.api.enumerate_views(app.api.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4,
                                      &count, configuration.data()) == XR_SUCCESS,
          "the layer offers the quad views configuration's four views");
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session");
    if (!framegen) {
        Check(!Peer::ToggleFrameGen(), "Frame Generation toggled off");
    }
    BeginSession(app, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
    QuadTargets targets;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        targets[i] = MakeTarget(app,
                                ColorInfo(1, configuration[i].recommendedImageRectWidth,
                                          configuration[i].recommendedImageRectHeight),
                                i < 2 ? "a peripheral view" : "a focus view");
    }

    // Where the focus view lands, as the compositor places it: the stand-in
    // reads a corner, the middle of the blend band on the centre line, and
    // (for the digest) the centre row and column, through the band and the
    // focus view's stripes.
    const depthxr::QuadViewsCanvasDimensions canvas =
        depthxr::ComputeQuadViewsCanvasDimensions(kQuadStereoSize, kQuadStereoSize, 4096, 4096, kQuadFocusScale);
    XrTime located_at = 0;
    {
        std::scoped_lock lock(g_runtime_mutex);
        located_at = g_runtime.time;
    }
    const std::array<XrView, 4> located = LocateQuadViews(app, located_at);
    std::array<depthxr::FrameGenComposeEye, 2> placed{};
    for (int eye = 0; eye < 2; ++eye) {
        placed[eye] = depthxr::BuildFrameGenComposeEye(
            ToFov(located[eye].fov), ToFov(located[eye + 2].fov), {canvas.width, canvas.height},
            {configuration[eye + 2].recommendedImageRectWidth, configuration[eye + 2].recommendedImageRectHeight},
            kQuadTransitionPercent, kQuadSharpness);
    }
    const std::array<float, 2> corner{0.03f, 0.03f};
    const std::array<float, 2> band{placed[0].focus_rect[0] + placed[0].feather[0] * 0.5f, 0.5f};
    Check(placed[0].focus_rect == placed[1].focus_rect && placed[0].feather == placed[1].feather,
          "both eyes' focus views land alike (the stand-in reads both at the same places)");
    Check(placed[0].focus_rect[0] > 0.1f && placed[0].focus_rect[1] > 0.1f && placed[0].focus_rect[2] < 0.9f &&
              placed[0].feather[0] * static_cast<float>(canvas.width) >= 4.0f,
          "the focus view lands inside the image, with a blend band of at least four texels");
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.probes = {corner, band};
        for (int i = 0; i < 96; ++i) {
            g_runtime.probes.push_back({(i + 0.5f) / 96.0f, 0.5f});
        }
        for (int i = 0; i < 48; ++i) {
            g_runtime.probes.push_back({0.5f, (i + 0.5f) / 48.0f});
        }
    }

    constexpr int kFrames = 12;
    int generated_frames = 0;
    // Per frame, the composed frame the runtime showed: each eye's centre and probes.
    std::vector<std::vector<Color>> readbacks;
    const auto frames_start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::string at = "quad frame " + std::to_string(frame);
        const std::size_t before = ShownCount();
        const XrResult result = QuadFrame(app, game, targets, frame);
        Check(result == XR_SUCCESS, at + ": xrEndFrame " + std::to_string(result));
        const std::array<XrSwapchain, 2> outputs = Peer::CompositorOutputs();
        int composed = 0;
        for (const ShownFrame& shown : Shown(before)) {
            Check(shown.views.size() == 2,
                  at + ": the runtime gets two views, not " + std::to_string(shown.views.size()));
            if (shown.views[0].swapchain != outputs[0] || shown.views[1].swapchain != outputs[1]) {
                // Frame Generation's own outputs (the stand-in backend writes nothing into them).
                Check(framegen, at + ": the runtime shows the compositor's outputs");
                for (const ShownView& view : shown.views) {
                    Check(view.swapchain != outputs[0] && view.swapchain != outputs[1] &&
                              std::none_of(targets.begin(), targets.end(),
                                           [&](const Target& target) { return target.swapchain == view.swapchain; }),
                          at + ": a frame of Frame Generation's shows its own outputs only");
                }
                ++generated_frames;
                continue;
            }
            ++composed;
            std::vector<Color> texels;
            for (int eye = 0; eye < 2; ++eye) {
                const ShownView& view = shown.views[eye];
                const Color periphery = PeripheryColor(frame, eye);
                const Color focus = FocusColor(frame, eye);
                const std::string where = at + " eye " + std::to_string(eye);
                Check(view.texel && view.probed.size() == 2 + 96 + 48,
                      where + ": the runtime read the composed image back");
                Check(Near(view.texel, focus),
                      where + ": the centre shows the focus view, " + Text(*view.texel) + ", rendered " + Text(focus));
                Check(Near(view.probed[0], periphery), where + ": the corner shows the periphery, " +
                                                           Text(view.probed[0]) + ", rendered " + Text(periphery));
                Check(Between(view.probed[1], periphery, focus),
                      where + ": the blend band blends the periphery " + Text(periphery) + " and the focus view " +
                          Text(focus) + ", " + Text(view.probed[1]));
                texels.push_back(*view.texel);
                texels.insert(texels.end(), view.probed.begin(), view.probed.end());
            }
            if (composed == 1) {
                readbacks.push_back(texels);
            } else {
                Check(readbacks.back() == texels, at + ": the runtime shows the same composed frame again");
            }
        }
        Check(composed >= 1, at + ": the runtime shows the composed frame");
    }
    // Paced: the runtime's frames took a refresh each at least (a lower
    // bound alone: a loaded machine only makes them slower), and their
    // display times only ever increase.
    const double frames_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frames_start).count();
    const std::vector<ShownFrame> runtime_frames = Shown();
    Check(frames_ms >= 0.9 * static_cast<double>(runtime_frames.size() - 1) * kPeriod / 1e6,
          "the stand-in paces its waits: " + std::to_string(runtime_frames.size()) + " frames in " +
              std::to_string(frames_ms) + " ms");
    for (std::size_t i = 1; i < runtime_frames.size(); ++i) {
        Check(runtime_frames[i].time > runtime_frames[i - 1].time, "the runtime's display times increase");
    }
    {
        std::scoped_lock lock(g_runtime_mutex);
        const StandInSwapchain* output = FindSwapchain(Peer::CompositorOutputs()[0]);
        Check(output && output->info.width == canvas.width && output->info.height == canvas.height,
              "the compositor's outputs are the canvas's size");
    }
    const auto [d3d11_frames, split_frames] = Peer::QuadFrames();
    Check(d3d11_frames == static_cast<std::uint64_t>(kFrames) && split_frames == 0,
          "Direct3D 11 composed every quad frame: d3d11=" + std::to_string(d3d11_frames) +
              ", split=" + std::to_string(split_frames));
    if (framegen) {
        const depthxr::FrameGenStatus status = Peer::FrameGen();
        Check(status.multiplier == 2 && status.generated > 0 && generated_frames > 0,
              "Frame Generation engaged at 2x: generated=" + std::to_string(status.generated) +
                  ", frames of its own outputs shown=" + std::to_string(generated_frames));
    } else {
        Check(Shown().size() == static_cast<std::size_t>(kFrames), "a runtime frame per quad frame");
    }

    // The bridge's summary counts each window's quad frames: the first all of
    // the frames before it, the next the two since.
    Peer::ExpireBridgeSummary();
    Check(QuadFrame(app, game, targets, kFrames) == XR_SUCCESS, "a quad frame logging the bridge's summary");
    Check(QuadFrame(app, game, targets, kFrames + 1) == XR_SUCCESS, "a quad frame");
    Peer::ExpireBridgeSummary();
    Check(QuadFrame(app, game, targets, kFrames + 2) == XR_SUCCESS, "a quad frame logging the bridge's summary");
    const std::string log = Peer::Log();
    // The stand-in backend does not compose: Frame Generation composes none.
    Check(Contains(log, "quadFrames=fg:0,d3d11:" + std::to_string(kFrames) + ",split:0") &&
              Contains(log, "quadFrames=fg:0,d3d11:2,split:0"),
          "the bridge's summaries count each window's quad frames");
    Check(!Contains(log, "D3D11 quadviews composition failed"), "composing never failed");

    // Readbacks of builds that compose alike print alike (FNV-1a).
    std::uint32_t digest = 2166136261u;
    std::size_t texel_count = 0;
    for (const std::vector<Color>& texels : readbacks) {
        texel_count += texels.size();
        for (const Color& texel : texels) {
            for (const std::uint8_t channel : texel) {
                digest = (digest ^ channel) * 16777619u;
            }
        }
    }
    std::ostringstream hex;
    hex << std::hex << std::setw(8) << std::setfill('0') << digest;
    const std::size_t eye_texels = readbacks.front().size() / 2;
    std::cout << g_case << ": the compositor's readbacks (" << readbacks.size() << " frames, " << texel_count
              << " texels): digest " << hex.str() << ", frame 0 band " << Text(readbacks.front()[2]) << " / "
              << Text(readbacks.front()[eye_texels + 2]) << "\n";

    // An output the runtime hands back with an error is released with
    // nothing drawn, and that frame is split into layers. Frame Generation
    // is told of every release of the outputs all the same: the runtime
    // shows the image released last wherever a frame shows the swapchain.
    const std::array<XrSwapchain, 2> outputs = Peer::CompositorOutputs();
    const auto runtime_releases = [](XrSwapchain swapchain) {
        std::scoped_lock lock(g_runtime_mutex);
        const StandInSwapchain* chain = FindSwapchain(swapchain);
        return chain ? chain->releases : -1;
    };
    std::array<int, 2> released_before{};
    std::array<std::uint64_t, 2> told_before{};
    for (int eye = 0; eye < 2; ++eye) {
        released_before[eye] = runtime_releases(outputs[eye]);
        told_before[eye] = Peer::FrameGenReleases(outputs[eye]);
    }
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.refuse_wait = {outputs[1]};
        g_runtime.wait_refusals = 1;
    }
    const auto [d3d11_before, split_before] = Peer::QuadFrames();
    Check(QuadFrame(app, game, targets, kFrames + 3) == XR_SUCCESS, "a quad frame whose right output's wait fails");
    Check(QuadFrame(app, game, targets, kFrames + 4) == XR_SUCCESS, "a quad frame after it");
    const auto [d3d11_after, split_after] = Peer::QuadFrames();
    Check(d3d11_after == d3d11_before + 1 && split_after == split_before + 1,
          "the frame whose output wait failed is split, the one after it composed");
    for (int eye = 0; eye < 2; ++eye) {
        const int released = runtime_releases(outputs[eye]) - released_before[eye];
        const std::uint64_t told = Peer::FrameGenReleases(outputs[eye]) - told_before[eye];
        Check(released == 2 && told == 2, "output " + std::to_string(eye) + ": the runtime got " +
                                              std::to_string(released) + " releases, Frame Generation was told of " +
                                              std::to_string(told));
    }

    // Frame Generation here (the stand-in backend, inline) cannot compose the
    // quad views: it generates frames from the compositor's images. The
    // diagnostic overlay stays out of them (Frame Generation would measure
    // its motion), and the app cannot show it.
    if (framegen) {
        Peer::SetQuadViewsDiagnosticVisualization(true);
        Check(!Peer::QuadViewsDiagnosticVisualizationAvailable(),
              "overlay: the app cannot show it while Frame Generation generates frames from the composed images");
        for (int frame = kFrames + 5; frame < kFrames + 8; ++frame) {
            const std::string at = "overlay, quad frame " + std::to_string(frame);
            const std::size_t before = ShownCount();
            Check(QuadFrame(app, game, targets, frame) == XR_SUCCESS, at + ": xrEndFrame");
            int composed = 0;
            for (const ShownFrame& shown : Shown(before)) {
                if (shown.views.size() != 2 || shown.views[0].swapchain != outputs[0]) {
                    continue; // Frame Generation's own outputs
                }
                ++composed;
                for (int eye = 0; eye < 2; ++eye) {
                    const ShownView& view = shown.views[eye];
                    Check(view.texel && !view.probed.empty() && Near(view.texel, FocusColor(frame, eye)) &&
                              Near(view.probed[0], PeripheryColor(frame, eye)),
                          at + " eye " + std::to_string(eye) + ": the composed image holds no overlay");
                }
            }
            Check(composed >= 1, at + ": the runtime shows the composed frame");
        }
        Peer::SetQuadViewsDiagnosticVisualization(false);
    }

    for (const Target& target : targets) {
        Check(app.api.destroy_swapchain(target.swapchain) == XR_SUCCESS, "a quad view's swapchain is destroyed");
    }
    StopApp(app);
    if (framegen) {
        Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    }
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}


// The real frame whose periphery colour `texel` shows (frames mod 8), or none.
std::optional<int> PeripheryFrame(const std::optional<Color>& texel, int eye) {
    for (int frame = 0; frame < 8; ++frame) {
        if (Near(texel, PeripheryColor(frame, eye))) return frame;
    }
    return std::nullopt;
}

// Each channel of `texel` lies between those of `a` and `b` (a step either side).
bool Within(const std::optional<Color>& texel, const Color& a, const Color& b) {
    if (!texel) return false;
    for (int i = 0; i < 3; ++i) {
        if ((*texel)[i] < std::min(a[i], b[i]) - 1 || (*texel)[i] > std::max(a[i], b[i]) + 1) return false;
    }
    return true;
}

// Interpolating: a frame between real frames n - 1 and n, its corner between
// their peripheries and its centre between their focus views (frames mod 8).
bool BetweenFrames(const std::optional<Color>& corner, const std::optional<Color>& centre, int eye) {
    for (int frame = 0; frame < 8; ++frame) {
        const int before = (frame + 7) % 8;
        if (Within(corner, PeripheryColor(before, eye), PeripheryColor(frame, eye)) &&
            Within(centre, FocusColor(before, eye), FocusColor(frame, eye))) {
            return true;
        }
    }
    return false;
}

// 10. Quad views on a bridged session with Frame Generation composing them,
// on the real Direct3D 12 backend (on the bridge's device: WARP, or the
// first hardware adapter with `hw`), at 2x, extrapolating or interpolating.
// Engaged, Frame Generation claims each frame at the layer's compose site:
// the Direct3D 11 compositor composes nothing more, the runtime gets two
// views naming Frame Generation's own output images, and each shows, in its
// centre, the focus colour and, at a corner, the periphery colour of one same
// real frame (interpolating, of the same two real frames it lies between).
// Its binding shows the diagnostic overlay in every frame, Frame Generation
// still composing them. Output acquires the runtime refuses fall back to the
// last frame written (logged), and when they keep failing the layer composes
// for a while. Toggled off, the Direct3D 11 compositor composes again, with
// the right colours, and no frame shown on the way is black.
void TestBridgedQuadFrameGen(Game& game, const std::string& mode) {
    CaseSettings settings;
    settings.quadviews = true;
    settings.diagnostic_binding = true;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = mode;
    BeginCase("quad-views-framegen-composed-" + mode, game, settings);
    g_runtime.recommended_width = g_runtime.recommended_height = kQuadStereoSize;
    g_runtime.paced = true;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::SetFrameGenQuadDecline(std::chrono::milliseconds(300), std::chrono::milliseconds(600));
    Peer::UseFrameGenGpu(depthxr::CreateFrameGenGpu);
    App app = StartApp({kD3D11Extension, kQuadViewsExtension, kFoveatedExtension});
    std::uint32_t count = 0;
    std::array<XrViewConfigurationView, 4> configuration{};
    for (XrViewConfigurationView& view : configuration) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    Check(app.api.enumerate_views(app.api.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4,
                                  &count, configuration.data()) == XR_SUCCESS &&
              count == 4,
          "the layer offers the quad views configuration's four views");
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session");
    BeginSession(app, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
    QuadTargets targets;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        targets[i] = MakeTarget(app,
                                ColorInfo(1, configuration[i].recommendedImageRectWidth,
                                          configuration[i].recommendedImageRectHeight),
                                i < 2 ? "a peripheral view" : "a focus view");
    }
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.probes = {{0.03f, 0.03f}};
    }

    int frame = 0;
    // Every frame the runtime showed from `from` on: two views, each read
    // back, none black; those of Frame Generation's own outputs show one real
    // frame's focus at the centre and its periphery at the corner. Returns
    // how many showed its outputs.
    // The frames shown from `from` on, or the first `count` of them (the
    // presenter thread keeps showing frames: counts compared with another
    // look at them must stop where it did).
    const auto check_shown = [&](std::size_t from, const std::string& at,
                                 std::size_t count = std::numeric_limits<std::size_t>::max()) {
        const std::array<XrSwapchain, 2> compositor = Peer::CompositorOutputs();
        std::size_t own = 0;
        std::vector<ShownFrame> frames = Shown(from);
        frames.resize(std::min(frames.size(), count));
        for (const ShownFrame& shown : frames) {
            Check(shown.views.size() == 2, at + ": the runtime gets two views, not " + std::to_string(shown.views.size()));
            const bool composed = shown.views[0].swapchain == compositor[0] && shown.views[1].swapchain == compositor[1];
            for (int eye = 0; eye < 2; ++eye) {
                const ShownView& view = shown.views[eye];
                Check(std::none_of(targets.begin(), targets.end(),
                                   [&](const Target& target) { return target.swapchain == view.swapchain; }),
                      at + ": the runtime never shows a quad view itself");
                Check(view.texel && view.probed.size() == 1, at + ": the runtime read the frame back");
                Check(*view.texel != Color{0, 0, 0, 255} && *view.texel != Color{0, 0, 0, 0},
                      at + ": no frame shown is black (eye " + std::to_string(eye) + ")");
                if (!composed && mode == "interpolate") {
                    // Frames between two real ones blend them.
                    Check(BetweenFrames(view.probed[0], view.texel, eye),
                          at + ": the corner and the centre show the periphery and the focus view between the same "
                               "two real frames, " +
                              Text(view.probed[0]) + " and " + Text(*view.texel) + " (eye " + std::to_string(eye) + ")");
                    continue;
                }
                const std::optional<int> real = PeripheryFrame(view.probed[0], eye);
                Check(real.has_value(), at + ": the corner shows a periphery, " + Text(view.probed[0]) + " (eye " +
                                            std::to_string(eye) + (composed ? ", composed by Direct3D 11)" : ")"));
                Check(Near(view.texel, FocusColor(*real, eye)),
                      at + ": the centre shows the focus view of the same real frame, " + Text(*view.texel) +
                          ", rendered " + Text(FocusColor(*real, eye)) + (composed ? " (Direct3D 11)" : ""));
            }
            own += composed ? 0 : 1;
        }
        return own;
    };
    const auto run = [&](int frames, const std::string& at) {
        for (int i = 0; i < frames; ++i, ++frame) {
            Check(QuadFrame(app, game, targets, frame) == XR_SUCCESS, at + ": xrEndFrame of quad frame " +
                                                                          std::to_string(frame));
        }
    };

    // Engaged after two steady frames: the first frame presented from Frame
    // Generation's thread sets the views up (composed by Direct3D 11), the
    // ones after it are claimed.
    run(8, "engaging");
    depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.threaded && status.quad && status.quad_setups == 1,
          "Frame Generation composes the quad views itself after setting the views up once: quad=" +
              std::to_string(status.quad) + ", setups=" + std::to_string(status.quad_setups));
    const std::uint64_t d3d11_engaged = Peer::QuadFrames().first;
    const std::uint64_t fg_engaged = Peer::QuadFramesComposedByFrameGen();
    const std::size_t engaged_from = ShownCount();
    const std::uint64_t unwritten_engaged = status.unwritten;
    const std::uint64_t unready_engaged = status.unready_capture;
    // Until frames are generated between the real ones (a loaded machine may
    // analyze none in time for a while).
    int composing_frames = 0;
    do {
        run(24, "composing");
        composing_frames += 24;
    } while (Peer::FrameGen().generated == 0 && composing_frames < 96);
    const std::vector<ShownFrame> composing = Shown(engaged_from);
    const std::size_t own = check_shown(engaged_from, "composing", composing.size());
    // Once its first frame is written, every refresh shows Frame Generation's
    // outputs, but for one it could not write, or whose real frame was not
    // captured yet (a loaded machine stalls it): that shows the frame shown
    // last, or the one Direct3D 11 composed to set the views up, again. So do
    // the refreshes before its first frame.
    const XrSwapchain compositor_left = Peer::CompositorOutputs()[0];
    const auto is_own = [&](const ShownFrame& shown) { return shown.views[0].swapchain != compositor_left; };
    const auto first_own = std::find_if(composing.begin(), composing.end(), is_own);
    const std::size_t from_first_own = static_cast<std::size_t>(composing.end() - first_own);
    const depthxr::FrameGenStatus composed_status = Peer::FrameGen();
    const std::uint64_t unwritten = composed_status.unwritten - unwritten_engaged;
    const std::uint64_t unready = composed_status.unready_capture - unready_engaged;
    Check(own >= static_cast<std::size_t>(composing_frames) && own + unwritten + unready >= from_first_own,
          "composing: every refresh shows Frame Generation's outputs (" + std::to_string(own) + " of " +
              std::to_string(from_first_own) + " from its first, " + std::to_string(unwritten) + " not written, " +
              std::to_string(unready) + " not captured yet)");
    Check(Peer::QuadFrames().first == d3d11_engaged &&
              Peer::QuadFramesComposedByFrameGen() == fg_engaged + static_cast<std::uint64_t>(composing_frames),
          "composing: the Direct3D 11 compositor composes nothing more, Frame Generation every frame");
    status = Peer::FrameGen();
    Check(status.generated > 0 && status.quad_claimed >= static_cast<std::uint64_t>(composing_frames),
          "composing: frames are generated between the real ones (" + std::to_string(status.generated) + ")");
    const auto last_own = std::find_if(composing.rbegin(), composing.rend(), is_own);
    const std::array<XrSwapchain, 2> outputs{last_own->views[0].swapchain, last_own->views[1].swapchain};
    CheckValidation(game, "composing");

    // The diagnostic overlay while Frame Generation composes the quad views,
    // shown and hidden with its binding: every frame shown draws it, Frame
    // Generation's own (real and generated, each with the overlay of the real
    // frame it comes from) as the Direct3D 11 compositor would, and Frame
    // Generation keeps composing: frames are never handed back for it (that
    // restarted Frame Generation on the composed full-size views, and stalled
    // SteamVR for seconds). The centre shows the head's centre (a white
    // cross), the inset's left edge on the centre row its outline (green: the
    // focus follows the head). Hidden again, the frames show the views alone.
    {
        const depthxr::QuadViewsCanvasDimensions canvas =
            depthxr::ComputeQuadViewsCanvasDimensions(kQuadStereoSize, kQuadStereoSize, 4096, 4096, kQuadFocusScale);
        XrTime located_at = 0;
        {
            std::scoped_lock lock(g_runtime_mutex);
            located_at = g_runtime.time;
        }
        const std::array<XrView, 4> located = LocateQuadViews(app, located_at);
        const depthxr::FrameGenComposeEye placed = depthxr::BuildFrameGenComposeEye(
            ToFov(located[0].fov), ToFov(located[2].fov), {canvas.width, canvas.height},
            {configuration[2].recommendedImageRectWidth, configuration[2].recommendedImageRectHeight},
            kQuadTransitionPercent, kQuadSharpness);
        Check((0.5f - placed.focus_rect[0]) * static_cast<float>(canvas.width) > 20.0f,
              "overlay: the inset's left edge lies clear of the head's centre marker");
        const std::array<float, 2> edge{placed.focus_rect[0], 0.5f};
        const Color white{255, 255, 255, 255};
        const Color outline = SrgbColor(0.15f, 1.0f, 0.3f);

        const std::uint64_t d3d11_before = Peer::QuadFrames().first;
        const std::uint64_t fg_before = Peer::QuadFramesComposedByFrameGen();
        const depthxr::FrameGenStatus before = Peer::FrameGen();
        Check(Peer::QuadViewsDiagnosticVisualizationAvailable(),
              "overlay: the app can show it while Frame Generation composes the quad views");
        {
            std::scoped_lock lock(g_runtime_mutex);
            g_runtime.probes = {{0.03f, 0.03f}, edge};
        }
        Peer::HoldQuadViewsDiagnosticBinding(true);
        run(1, "overlay shown");
        Peer::HoldQuadViewsDiagnosticBinding(false);
        Check(Peer::QuadViewsDiagnosticVisualization(),
              "overlay: its binding shows it while Frame Generation composes");
        // Frames ended before it may still be shown for a few refreshes.
        run(7, "overlay shown");
        const std::size_t shown_from = ShownCount();
        run(8, "overlay shown");
        const std::array<XrSwapchain, 2> compositor = Peer::CompositorOutputs();
        std::size_t own_overlaid = 0;
        for (const ShownFrame& shown : Shown(shown_from)) {
            Check(shown.views.size() == 2, "overlay shown: the runtime gets two views");
            for (int eye = 0; eye < 2; ++eye) {
                const ShownView& view = shown.views[eye];
                const std::string where = "overlay shown, eye " + std::to_string(eye);
                Check(view.texel && view.probed.size() == 2, where + ": the runtime read the frame back");
                Check(Near(view.texel, white),
                      where + ": the centre shows the head's centre marked, " + Text(*view.texel));
                Check(Near(view.probed[1], outline),
                      where + ": the inset's edge shows its outline " + Text(outline) + ", " + Text(view.probed[1]));
            }
            own_overlaid += shown.views[0].swapchain != compositor[0] ? 1 : 0;
        }
        Check(own_overlaid > 0, "overlay shown: Frame Generation's own frames show it");

        Peer::HoldQuadViewsDiagnosticBinding(true);
        run(1, "overlay hidden");
        Peer::HoldQuadViewsDiagnosticBinding(std::nullopt);
        Check(!Peer::QuadViewsDiagnosticVisualization(), "overlay: its binding hides it again");
        {
            std::scoped_lock lock(g_runtime_mutex);
            g_runtime.probes = {{0.03f, 0.03f}};
        }
        run(7, "overlay hidden");
        const std::size_t hidden_from = ShownCount();
        run(8, "overlay hidden");
        Check(check_shown(hidden_from, "overlay hidden") > 0,
              "overlay hidden: Frame Generation's frames show the views alone");

        const depthxr::FrameGenStatus after = Peer::FrameGen();
        Check(Peer::QuadFrames().first == d3d11_before && Peer::QuadFramesComposedByFrameGen() == fg_before + 32 &&
                  after.quad && after.quad_setups == before.quad_setups &&
                  after.quad_write_declines == before.quad_write_declines,
              "overlay: Frame Generation keeps composing the quad views (Direct3D 11 composed " +
                  std::to_string(Peer::QuadFrames().first - d3d11_before) + ", setups " +
                  std::to_string(after.quad_setups) + ", was " + std::to_string(before.quad_setups) + ")");
        const std::string log = Peer::Log();
        Check(Count(log, "Quadviews diagnostic visualization shown via ") == 1 &&
                  Count(log, "Quadviews diagnostic visualization hidden via ") == 1 &&
                  !Contains(log, "is unavailable while Frame Generation"),
              "overlay: its binding shows and hides it (logged)");
        Check(!Contains(log, "restarts on the layer's composed quad views"),
              "overlay: Frame Generation never restarts on the composed views for it");
    }

    // A frame whose views' rectangles change cannot be claimed (their
    // releases were captured with the old ones): Direct3D 11 composes it,
    // and Frame Generation's thread shows it later as it is. The
    // compositor's outputs reach the runtime once their composition has run
    // on the GPU, never before it with the runtime's queue (Frame
    // Generation's, which writes every refresh) waiting for it: Direct3D 11
    // is held back from just before xrEndFrame until the frame has ended,
    // and the stand-in counts the outputs' releases the application's thread
    // makes meanwhile.
    {
        const std::array<XrSwapchain, 2> compositor = Peer::CompositorOutputs();
        Gate gate(g_runtime.device12.Get(), game);
        {
            std::scoped_lock lock(g_runtime_mutex);
            g_runtime.watch_gate = &gate;
            g_runtime.watch_thread = std::this_thread::get_id();
            g_runtime.watched = {compositor[0], compositor[1]};
            g_runtime.watched_acquired.clear();
            g_runtime.early_releases = 0;
        }
        // Opened after a while whatever happens: a runtime's queue waiting
        // for the composition would hold the stand-in's readbacks.
        const HANDLE ended = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread opener([&] {
            WaitForSingleObject(ended, 3000);
            gate.Open();
        });
        const std::uint64_t d3d11_before = Peer::QuadFrames().first;
        const std::size_t moved_from = ShownCount();
        QuadFrameOptions moved;
        moved.hold = &gate;
        moved.trim = 2;
        const XrResult result = QuadFrame(app, game, targets, frame++, moved);
        gate.Open();
        SetEvent(ended);
        opener.join();
        CloseHandle(ended);
        int early = 0;
        {
            std::scoped_lock lock(g_runtime_mutex);
            early = g_runtime.early_releases;
            g_runtime.watch_gate = nullptr;
            g_runtime.watched.clear();
        }
        Check(result == XR_SUCCESS, "rectangles changed: xrEndFrame " + std::to_string(result));
        Check(Peer::QuadFrames().first == d3d11_before + 1, "rectangles changed: Direct3D 11 composes the frame");
        Check(early == 0, "rectangles changed: " + std::to_string(early) +
                              " of the compositor's outputs reached the runtime before their composition ran");
        // Back to the whole images (one frame more for Direct3D 11): Frame
        // Generation composes the views again.
        const std::uint64_t fg_moved = Peer::QuadFramesComposedByFrameGen();
        run(6, "rectangles changed back");
        Check(Peer::QuadFramesComposedByFrameGen() > fg_moved,
              "rectangles changed back: Frame Generation composes the quad views again");
        check_shown(moved_from, "rectangles changed");
    }

    // The runtime refuses Frame Generation's output acquires: the last
    // frame written is shown again (logged); refused ten times in a row, the
    // layer composes for a while, then Frame Generation composes again.
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.refuse_acquire = {outputs[0], outputs[1]};
        g_runtime.refusals = 12;
    }
    const std::size_t refused_from = ShownCount();
    const std::uint64_t d3d11_refused = Peer::QuadFrames().first;
    run(12, "refused");
    std::string log = Peer::Log();
    Check(Contains(log, "Frame Generation could not write a composed frame ("), "refused: failed writes are logged");
    Check(Contains(log, "Frame Generation could not write 10 composed frames in a row (output image acquire failed") &&
              Peer::FrameGen().quad_write_declines == 1,
          "refused: ten in a row hand the quad views back to the layer");
    Check(Peer::QuadFrames().first > d3d11_refused, "refused: the Direct3D 11 compositor composes meanwhile");
    check_shown(refused_from, "refused");
    // The app's status line says so while it lasts (when the case is still
    // that quick).
    if (const depthxr::FrameGenStatus declined = Peer::FrameGen(); declined.quad_declining) {
        const std::string line = Peer::FrameGenStatusReason(declined);
        Check(Contains(line, "could not be shown") && !Contains(line, " fps"),
              "refused: the app's status line says generated frames could not be shown for a while: " + line);
    }
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.refusals = 0;
    }
    Sleep(350); // the decline ends
    const std::uint64_t fg_resumed = Peer::QuadFramesComposedByFrameGen();
    const std::size_t resumed_from = ShownCount();
    run(10, "resumed");
    Check(Peer::QuadFramesComposedByFrameGen() >= fg_resumed + 9 && check_shown(resumed_from, "resumed") > 0,
          "resumed: Frame Generation composes again after the decline");

    // Toggled off: frames already in flight end through Frame Generation,
    // then the Direct3D 11 compositor composes each frame as it ends.
    Check(!Peer::ToggleFrameGen(), "Frame Generation toggled off");
    const std::size_t off_from = ShownCount();
    run(6, "toggled off");
    check_shown(off_from, "toggled off");
    const std::uint64_t d3d11_off = Peer::QuadFrames().first;
    const std::size_t inline_from = ShownCount();
    run(4, "toggled off");
    Check(Peer::QuadFrames().first == d3d11_off + 4 && check_shown(inline_from, "toggled off") == 0,
          "toggled off: the Direct3D 11 compositor composes every frame");
    const std::vector<ShownFrame> last = Shown(inline_from);
    Check(last.size() == 4 && Near(last.back().views[0].probed[0], PeripheryColor((frame - 1) % 8, 0)) &&
              Near(last.back().views[0].texel, FocusColor((frame - 1) % 8, 0)),
          "toggled off: the runtime shows the frame just ended");

    Peer::ExpireBridgeSummary();
    run(1, "summary");
    log = Peer::Log();
    Check(Contains(log, "quadFrames=fg:") && !Contains(log, "quadFrames=fg:0,"),
          "the bridge's summary counts the frames Frame Generation composed");
    Check(!Contains(log, "D3D11 quadviews composition failed") && !Contains(log, "Frame Generation paused"),
          "nothing failed or paused");
    for (const Target& target : targets) {
        Check(app.api.destroy_swapchain(target.swapchain) == XR_SUCCESS, "a quad view's swapchain is destroyed");
    }
    StopApp(app);
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    Peer::SetFrameGenQuadDecline(std::chrono::milliseconds(10000), std::chrono::milliseconds(80000));
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}

// A bridged quad views session with Frame Generation composing the views
// itself on the real Direct3D 12 backend, at 2x in `mode`, engaged after two
// steady frames: the application and its four views' targets.
struct QuadFrameGenSession {
    App app;
    std::unique_ptr<SessionCreate> create;
    QuadTargets targets;
};

QuadFrameGenSession StartQuadFrameGen(Game& game, const std::string& name, const std::string& mode) {
    CaseSettings settings;
    settings.quadviews = true;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = mode;
    BeginCase(name, game, settings);
    g_runtime.recommended_width = g_runtime.recommended_height = kQuadStereoSize;
    g_runtime.paced = true;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(depthxr::CreateFrameGenGpu);
    QuadFrameGenSession session;
    App& app = session.app;
    app = StartApp({kD3D11Extension, kQuadViewsExtension, kFoveatedExtension});
    std::uint32_t count = 0;
    std::array<XrViewConfigurationView, 4> configuration{};
    for (XrViewConfigurationView& view : configuration) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    Check(app.api.enumerate_views(app.api.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4,
                                  &count, configuration.data()) == XR_SUCCESS &&
              count == 4,
          "the layer offers the quad views configuration's four views");
    session.create = std::make_unique<SessionCreate>(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &session.create->info, &app.session) == XR_SUCCESS &&
              Peer::Bridge().Active(),
          "a bridged session");
    BeginSession(app, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO);
    for (std::size_t i = 0; i < session.targets.size(); ++i) {
        session.targets[i] = MakeTarget(app,
                                        ColorInfo(1, configuration[i].recommendedImageRectWidth,
                                                  configuration[i].recommendedImageRectHeight),
                                        i < 2 ? "a peripheral view" : "a focus view");
    }
    return session;
}

// Its swapchains, session and instance destroyed (within 10 s), and the
// case ended (`device_removed`: as EndCase). How long the teardown took.
double StopQuadFrameGen(Game& game, QuadFrameGenSession& session, bool device_removed = false) {
    const auto start = std::chrono::steady_clock::now();
    for (const Target& target : session.targets) {
        Check(session.app.api.destroy_swapchain(target.swapchain) == XR_SUCCESS,
              "a quad view's swapchain is destroyed");
    }
    StopApp(session.app);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(seconds < 10.0, "the teardown took " + std::to_string(seconds) + " s");
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    Check(GameRuns(game), "the game's device works");
    EndCase(game, device_removed);
    return seconds;
}

// The stand-in's stereo fields of view for frame `frame` of the geometry
// case: the eyes' differ (each is wider towards its own side, so its focus
// view lands off its image's centre, the other way from the other eye's),
// and they move from frame to frame (each focus view with them).
std::array<XrFovf, 2> GeometryFovs(int frame) {
    const float shift = 0.04f * static_cast<float>(frame % 4);
    return {{{-0.95f + shift, 0.7f + shift, 0.8f, -0.75f}, {-0.7f - shift, 0.95f - shift, 0.75f, -0.8f}}};
}

// 12. What Frame Generation composes is what the Direct3D 11 compositor
// composes from the same frame: at each real frame's own refresh
// (extrapolating, nothing moved yet), each eye's image matches (a step
// either way), at a corner and across a row and three columns through its
// focus view and blend band, the one Direct3D 11 composes from the same
// frame with Frame Generation toggled off. The eyes' fields of view differ
// and move from frame to frame, so each eye's focus view lands in a place of
// its own in each frame: Frame Generation must compose each eye of each
// frame with that frame's own composition of that eye. A frame's content
// (its colours, and its fields of view) repeats every 8 frames: Direct3D 11
// composes one such period for reference.
void TestBridgedQuadFrameGenGeometry(Game& game) {
    QuadFrameGenSession session = StartQuadFrameGen(game, "quad-views-framegen-geometry", "extrapolate");
    App& app = session.app;
    // A corner, the centre row, and three columns.
    constexpr int kRowProbes = 96;
    constexpr int kColumnProbes = 48;
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.probes = {{0.03f, 0.03f}};
        for (int i = 0; i < kRowProbes; ++i) {
            g_runtime.probes.push_back({(i + 0.5f) / kRowProbes, 0.5f});
        }
        for (const float column : {0.4f, 0.5f, 0.6f}) {
            for (int i = 0; i < kColumnProbes; ++i) {
                g_runtime.probes.push_back({column, (i + 0.5f) / kColumnProbes});
            }
        }
    }
    constexpr int kPeriod = 8;
    int frame = 0;
    // The next `count` frames, each with its own fields of view; their
    // display times by frame.
    const auto run = [&](int count, const std::string& at) {
        std::map<int, XrTime> times;
        for (const int last = frame + count; frame < last; ++frame) {
            QuadFrameOptions options;
            options.fovs = GeometryFovs(frame);
            options.display_time = &times[frame];
            Check(QuadFrame(app, game, session.targets, frame, options) == XR_SUCCESS,
                  at + ": xrEndFrame of quad frame " + std::to_string(frame));
        }
        return times;
    };
    // Each eye's texels (its centre, then the probes) of the frame the
    // runtime showed at each frame's display time from `from` on, when it
    // shows Frame Generation's outputs (`own`) or the compositor's, and its
    // corner that frame's periphery.
    using EyeTexels = std::array<std::vector<Color>, 2>;
    const auto shown_at = [&](std::size_t from, const std::map<int, XrTime>& times, bool own) {
        const std::array<XrSwapchain, 2> compositor = Peer::CompositorOutputs();
        std::map<int, EyeTexels> texels;
        const std::vector<ShownFrame> shown = Shown(from);
        for (const auto& [shown_frame, time] : times) {
            for (const ShownFrame& candidate : shown) {
                if (candidate.time != time) continue;
                const bool composed =
                    candidate.views[0].swapchain == compositor[0] && candidate.views[1].swapchain == compositor[1];
                if (composed == own) continue;
                EyeTexels eyes;
                for (int eye = 0; eye < 2; ++eye) {
                    const ShownView& view = candidate.views[eye];
                    Check(view.texel && view.probed.size() == g_runtime.probes.size(),
                          "geometry: the runtime read the frame back");
                    if (!Near(view.probed[0], PeripheryColor(shown_frame, eye))) break;
                    eyes[eye].push_back(*view.texel);
                    eyes[eye].insert(eyes[eye].end(), view.probed.begin(), view.probed.end());
                }
                if (!eyes[0].empty() && !eyes[1].empty()) texels[shown_frame] = eyes;
            }
        }
        return texels;
    };

    run(kPeriod, "engaging");
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.threaded && status.quad, "Frame Generation composes the quad views itself");
    // Real frames Frame Generation showed at their own refreshes (the others
    // came late, and are left out): a few periods' worth, until there are
    // enough of them on a loaded machine.
    std::map<int, EyeTexels> composed;
    for (int periods = 0; periods < 8 && composed.size() < 4; periods += 2) {
        const std::size_t from = ShownCount();
        composed.merge(shown_at(from, run(2 * kPeriod, "composing"), true));
    }
    Check(composed.size() >= 4, "Frame Generation showed at least four real frames at their own refreshes (" +
                                    std::to_string(composed.size()) + " in " + std::to_string(frame) + " frames)");
    CheckValidation(game, "composing");

    // A period of frames, Frame Generation toggled off: each composed by
    // Direct3D 11 as it ends, and shown at its display time.
    Check(!Peer::ToggleFrameGen(), "Frame Generation toggled off");
    run(kPeriod + (kPeriod - frame % kPeriod), "toggled off");
    const std::size_t reference_from = ShownCount();
    std::map<int, EyeTexels> reference;
    for (const auto& [reference_frame, texels] : shown_at(reference_from, run(kPeriod, "reference"), false)) {
        reference[reference_frame % kPeriod] = texels;
    }
    Check(reference.size() == static_cast<std::size_t>(kPeriod),
          "the runtime shows each frame Direct3D 11 composed at its display time (" +
              std::to_string(reference.size()) + " of " + std::to_string(kPeriod) + ")");

    // Each eye's focus view lands in a place of its own, moving from frame to
    // frame (where the centre row first shows its focus colour), with a
    // blend band on the way.
    const auto focus_starts = [&](int phase, int eye) {
        const std::vector<Color>& texels = reference.at(phase)[eye];
        for (int i = 0; i < kRowProbes; ++i) {
            if (Near(texels[2 + i], FocusColor(phase, eye))) return i;
        }
        return -1;
    };
    for (int phase = 0; phase < kPeriod; ++phase) {
        const std::string at = "geometry frame phase " + std::to_string(phase);
        for (int eye = 0; eye < 2; ++eye) {
            const std::string where = at + " eye " + std::to_string(eye);
            const std::vector<Color>& texels = reference.at(phase)[eye];
            Check(focus_starts(phase, eye) > 0, where + ": the centre row crosses the focus view");
            Check(std::any_of(texels.begin() + 2, texels.begin() + 2 + kRowProbes,
                              [&](const Color& texel) {
                                  return Between(texel, PeripheryColor(phase, eye), FocusColor(phase, eye));
                              }),
                  where + ": the centre row crosses the blend band");
        }
        Check(focus_starts(phase, 0) != focus_starts(phase, 1),
              at + ": each eye's focus view lands in a place of its own");
        const int before = (phase + kPeriod - 2) % kPeriod;
        Check(focus_starts(phase, 0) != focus_starts(before, 0) && focus_starts(phase, 1) != focus_starts(before, 1),
              at + ": the focus views move from frame to frame");
    }

    // Frame Generation's own frames match them.
    int most = 0;
    for (const auto& [composed_frame, eyes] : composed) {
        for (int eye = 0; eye < 2; ++eye) {
            const std::vector<Color>& expected = reference.at(composed_frame % kPeriod)[eye];
            for (std::size_t i = 0; i < eyes[eye].size(); ++i) {
                for (int channel = 0; channel < 4; ++channel) {
                    const int difference = std::abs(static_cast<int>(eyes[eye][i][channel]) -
                                                    static_cast<int>(expected[i][channel]));
                    most = std::max(most, difference);
                    Check(difference <= 1, "geometry frame " + std::to_string(composed_frame) + " eye " +
                                               std::to_string(eye) + ", texel " + std::to_string(i) +
                                               ": Frame Generation composed " + Text(eyes[eye][i]) +
                                               ", Direct3D 11 " + Text(expected[i]));
                }
            }
        }
    }
    std::cout << g_case << ": " << composed.size() << " real frames compared at their own refreshes, largest "
              << "difference " << most << "\n";
    StopQuadFrameGen(game, session);
}

// 13. The bridge's device removed while Frame Generation composes the quad
// views (on the real backend, its presenter thread writing every refresh):
// the session is lost at once, no projection of four views reaches the
// runtime, and the teardown waits on nothing of the removed device. In a
// process of its own: Direct3D 12 makes one device per adapter in a process.
void TestBridgedQuadDeviceLost(Game& game) {
    QuadFrameGenSession session = StartQuadFrameGen(game, "quad-views-framegen-device-lost", "extrapolate");
    App& app = session.app;
    int frame = 0;
    for (; frame < 14; ++frame) {
        Check(QuadFrame(app, game, session.targets, frame) == XR_SUCCESS,
              "xrEndFrame of quad frame " + std::to_string(frame));
    }
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.threaded && status.quad && Peer::QuadFramesComposedByFrameGen() > 0,
          "Frame Generation composes the quad views itself");
    ComPtr<ID3D12Device5> device5;
    if (FAILED(BridgeBinding()->device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        std::cout << "quad views device loss not tested: no ID3D12Device5\n";
        StopQuadFrameGen(game, session);
        return;
    }
    CheckValidation(game, "before the device's removal");
    {
        // Not while the stand-in reads a frame of the presenter thread's back.
        std::scoped_lock lock(g_runtime_mutex);
        device5->RemoveDevice();
    }
    device5.Reset();
    const std::size_t shown = ShownCount();
    QuadFrameOptions lost;
    lost.session_may_be_lost = true;
    for (int attempt = 0; attempt < 2; ++attempt, ++frame) {
        const XrResult result = QuadFrame(app, game, session.targets, frame, lost);
        Check(result == XR_ERROR_SESSION_LOST, "quad frame " + std::to_string(attempt + 1) +
                                                   " after the removal loses the session: " + std::to_string(result));
    }
    for (const ShownFrame& shown_frame : Shown(shown)) {
        for (const ShownView& view : shown_frame.views) {
            Check(std::none_of(session.targets.begin(), session.targets.end(),
                               [&](const Target& target) { return target.swapchain == view.swapchain; }),
                  "after the removal, the runtime never gets a quad view itself");
        }
    }
    const std::string log = Peer::Log();
    Check(Count(log, "the session is lost") == 1, "the loss is logged once, for two frames");
    const std::string name = g_case;
    const double seconds = StopQuadFrameGen(game, session, true);
    std::cout << name << ": the session was lost, its teardown took " << seconds << " s\n";
}

// --- X-Plane's shapes -------------------------------------------------------

// The side-by-side image's halves in frame `frame`: each eye's differs from
// the other eye's in blue whatever the frame (so a frame generated from
// either half shows its eye's), and from those of the seven frames before it.
Color SideBySideColor(int frame, int eye) {
    return {static_cast<std::uint8_t>(20 + 29 * (frame % 8)), static_cast<std::uint8_t>(230 - 23 * (frame % 8)),
            static_cast<std::uint8_t>(eye ? 220 : 60), 255};
}

// The quad layer's at the game's `release`th release of its swapchain: it
// differs from those of the fifteen releases before it.
Color QuadLayerColor(int release) {
    return {static_cast<std::uint8_t>(250 - 13 * (release % 16)), 120,
            static_cast<std::uint8_t>(10 + 15 * (release % 16)), 255};
}

// Each eye's view of the side-by-side session, and the quad layer's swapchain.
constexpr std::uint32_t kSideBySideEyeWidth = 128;
constexpr std::uint32_t kSideBySideEyeHeight = 96;
constexpr std::uint32_t kQuadLayerSize = 128;
// How long the game's GPU work of a loaded frame takes (three refreshes).
constexpr DWORD kGameLoadMs = 30;

// X-Plane's swapchains: sRGB, with no usage asked for.
XrSwapchainCreateInfo XPlaneInfo(std::uint32_t width, std::uint32_t height) {
    XrSwapchainCreateInfo info = ColorInfo(1, width, height);
    info.usageFlags = 0;
    return info;
}

// Each half of `image` (its first slice) cleared to its eye's colour.
void ClearHalves(Game& game, ID3D11Texture2D* image, const std::array<Color, 2>& colors) {
    D3D11_TEXTURE2D_DESC desc{};
    image->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(game.device->CreateRenderTargetView(image, &view, &target)), "a render-target view of an image");
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        const D3D11_RECT half{static_cast<LONG>(eye * desc.Width / 2), 0, static_cast<LONG>((eye + 1) * desc.Width / 2),
                              static_cast<LONG>(desc.Height)};
        const Color& color = colors[eye];
        const float rgba[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
        game.context4->ClearView(target.Get(), rgba, &half, 1);
    }
}

// A GPU-bound game: its Direct3D 11 work of a frame held back from just
// before its rendering for kGameLoadMs, each hold opened by a thread of its
// own, which first notes when, and how far the stand-in's marks on the
// runtime's queue (StandIn::readback, one at each xrEndFrame) had run by then.
struct GameLoad {
    using Clock = std::chrono::steady_clock;
    // A mark made this long before a hold opened that had not run by then
    // waited for it: nothing else on the runtime's queue takes this long.
    static constexpr std::chrono::milliseconds kTell{15};
    struct Hold {
        std::unique_ptr<Gate> gate;
        std::thread opener;
        bool open{false};
        Clock::time_point opened;
        UINT64 marks_at_open{0};
    };
    ComPtr<ID3D12Fence> marks;
    std::mutex mutex;
    std::deque<Hold> holds;

    explicit GameLoad(ID3D12Fence* fence) : marks(fence) {}
    GameLoad(const GameLoad&) = delete;
    GameLoad& operator=(const GameLoad&) = delete;
    ~GameLoad() { Finish(); }

    void HoldGame(Game& game) {
        std::scoped_lock lock(mutex);
        Hold& hold = holds.emplace_back();
        hold.gate = std::make_unique<Gate>(g_runtime.device12.Get(), game);
        hold.gate->HoldGame(game);
        hold.opener = std::thread([this, &hold] {
            Sleep(kGameLoadMs);
            {
                std::scoped_lock opening(mutex);
                hold.marks_at_open = marks->GetCompletedValue();
                hold.opened = Clock::now();
                hold.open = true;
            }
            hold.gate->Open();
        });
    }
    // A mark of `value` on `queue`, after everything queued there, made at
    // `at`: the hold still closed meanwhile, if any.
    std::optional<std::size_t> Mark(ID3D12CommandQueue* queue, UINT64 value, Clock::time_point& at) {
        std::scoped_lock lock(mutex);
        queue->Signal(marks.Get(), value);
        at = Clock::now();
        for (std::size_t i = 0; i < holds.size(); ++i) {
            if (!holds[i].open) return i;
        }
        return std::nullopt;
    }
    // Every hold opened.
    void Finish() {
        for (Hold& hold : holds) {
            if (hold.opener.joinable()) hold.opener.join();
        }
    }
    // Whether the mark `value`, made at `at` while hold `index` was closed,
    // was reached only once the hold opened (the runtime's queue waited for
    // the game's work); none when it was made too shortly before to tell.
    std::optional<bool> Waited(std::size_t index, UINT64 value, Clock::time_point at) {
        std::scoped_lock lock(mutex);
        const Hold& hold = holds[index];
        if (!hold.open || hold.opened - at < kTell) return std::nullopt;
        return value > hold.marks_at_open;
    }
};

// What the stand-in saw of each frame beyond its views (by its index in
// StandIn::frames).
struct SideBySideShown {
    std::uint32_t layers{0};
    bool projection_first{false};
    std::array<XrRect2Di, 2> rects{};  // the projection's views'
    bool quad_layer{false};            // the game's quad layer, after the projection
    std::optional<Color> quad_texel;   // what it showed, read on the runtime's queue
    int quad_releases{0};              // the game's releases of its swapchain the runtime had by then
    int game_quad_releases{0};         // the game's releases of it in frames ended by then (read first)
    double latency_ms{0.0};            // from xrEndFrame until the runtime's queue reached the frame
    std::optional<std::size_t> during; // the game's hold (GameLoad) still closed at xrEndFrame
    UINT64 mark{0};
    GameLoad::Clock::time_point marked;
    // The quad layer's read on the runtime's queue right after the mark (0:
    // not read), and from xrEndFrame until it was done.
    UINT64 read_mark{0};
    double read_latency_ms{0.0};
};

// The X-Plane-shaped game: its session and swapchains, what it rendered.
struct SideBySideSession {
    App app;
    std::unique_ptr<SessionCreate> create;
    Target eyes;  // both eyes side by side
    Target ui;    // the quad layer's
    Target depth; // both eyes' depth side by side
    int frames{0};
    int ui_releases{0};
    // The game's releases of the UI whose frame's xrEndFrame returned: a
    // bridged release reaches the runtime at the frame's end (the layer
    // hands it over there, as a runtime reads the image).
    std::atomic<int> ui_released{0};
};

// What a side-by-side frame does beyond rendering both eyes.
struct SideBySideFrameOptions {
    bool quad_layer{true};   // the UI rendered, released and submitted as a quad layer after the projection
    bool depth{false};       // depth rendered and chained to each view, its half
    GameLoad* load{nullptr}; // the game's work held back from before the eyes' rendering
    bool unknown_after_depth{false}; // separate from the supported depth-only generation case
};

// One frame of the X-Plane-shaped game: both eyes rendered into their halves
// of one image and released, depth likewise, then the UI rendered and
// released; submitted as a stereo projection naming each eye's half by its
// rectangle, then the UI's quad layer.
void SideBySideFrame(SideBySideSession& session, Game& game, const SideBySideFrameOptions& options) {
    App& app = session.app;
    const int frame = session.frames++;
    const std::string at = "side-by-side frame " + std::to_string(frame);
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Check(app.api.wait_frame(app.session, &wait_info, &state) == XR_SUCCESS, at + ": xrWaitFrame");
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    Check(app.api.begin_frame(app.session, &begin_info) == XR_SUCCESS, at + ": xrBeginFrame");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = app.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{};
    for (XrView& view : located) view.type = XR_TYPE_VIEW;
    std::uint32_t located_count = 0;
    Check(app.api.locate_views(app.session, &locate, &view_state, 2, &located_count, located.data()) == XR_SUCCESS &&
              located_count == 2,
          at + ": xrLocateViews");

    const std::uint32_t eyes_index = Acquire(app, session.eyes);
    const std::uint32_t depth_index = options.depth ? Acquire(app, session.depth) : 0;
    const std::uint32_t ui_index = options.quad_layer ? Acquire(app, session.ui) : 0;
    if (options.load) {
        options.load->HoldGame(game);
    }
    ClearHalves(game, session.eyes.images[eyes_index], {SideBySideColor(frame, 0), SideBySideColor(frame, 1)});
    if (options.depth) {
        ClearDepth(game, session.depth.images[depth_index], 0);
    }
    Release(app, session.eyes);
    if (options.depth) {
        Release(app, session.depth);
    }
    if (options.quad_layer) {
        Draw(game, session.ui.images[ui_index], QuadLayerColor(session.ui_releases++));
        Release(app, session.ui);
    }

    XrBaseInStructure after_depth{kApplicationStructType};
    std::array<XrCompositionLayerDepthInfoKHR, 2> depth_infos{};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        const XrRect2Di half{{static_cast<std::int32_t>(eye * kSideBySideEyeWidth), 0},
                             {static_cast<std::int32_t>(kSideBySideEyeWidth),
                              static_cast<std::int32_t>(kSideBySideEyeHeight)}};
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage.swapchain = session.eyes.swapchain;
        views[eye].subImage.imageRect = half;
        if (options.depth) {
            depth_infos[eye] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            depth_infos[eye].next = options.unknown_after_depth ? &after_depth : nullptr;
            depth_infos[eye].subImage.swapchain = session.depth.swapchain;
            depth_infos[eye].subImage.imageRect = half;
            depth_infos[eye].minDepth = 0.0f;
            depth_infos[eye].maxDepth = 1.0f;
            depth_infos[eye].nearZ = 0.05f;
            depth_infos[eye].farZ = 1000.0f;
            views[eye].next = &depth_infos[eye];
        }
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = app.space;
    projection.viewCount = 2;
    projection.views = views.data();
    XrCompositionLayerQuad ui{XR_TYPE_COMPOSITION_LAYER_QUAD};
    ui.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    ui.space = app.space;
    ui.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    ui.subImage.swapchain = session.ui.swapchain;
    ui.subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(kQuadLayerSize), static_cast<std::int32_t>(kQuadLayerSize)}};
    ui.pose = {{0, 0, 0, 1}, {0, 0, -1.0f}};
    ui.size = {0.6f, 0.6f};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ui)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = options.quad_layer ? 2 : 1;
    end.layers = layers;
    const XrResult result = app.api.end_frame(app.session, &end);
    Check(result == XR_SUCCESS, at + ": xrEndFrame " + std::to_string(result));
    if (options.quad_layer) {
        // A bridged release reaches the runtime at the frame's end.
        ++session.ui_released;
    }
}

// The real frame (frames mod 8) whose eye `eye` `texel` shows, or none.
std::optional<int> SideBySideFrameOf(const std::optional<Color>& texel, int eye) {
    for (int frame = 0; frame < 8; ++frame) {
        if (Near(texel, SideBySideColor(frame, eye))) return frame;
    }
    return std::nullopt;
}

// `values`' median and largest.
std::string Spread(std::vector<double> values) {
    if (values.empty()) return "none";
    std::sort(values.begin(), values.end());
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << "p50 " << values[values.size() / 2] << " ms, max " << values.back()
         << " ms";
    return text.str();
}

// 14. An X-Plane-shaped bridged session (X-Plane 12 renders through Direct3D
// 11 to OpenXR), Frame Generation on the real Direct3D 12 backend (on the
// bridge's device) at 2x in `mode`, its presenter paced by the stand-in: the
// game renders both eyes into one sRGB swapchain asked for with no usage, its
// views naming each eye's half by its rectangle, and renders and releases
// a small UI swapchain every frame, submitted as a quad layer after the
// projection. The session is bridged; once engaged, every refresh shows
// Frame Generation's outputs, each eye its own half (real frames as the
// game rendered both halves, generated ones between or after them); the
// quad layer reaches the runtime on every refresh, after the projection,
// showing what the game rendered for its latest release (its rendering
// done: read on the runtime's queue). Under a GPU load on the game (its work
// of each frame held back for three refreshes) the quad layer still shows
// that on every refresh, which costs what the stand-in measures and the case
// prints: a released layer Frame Generation does not capture makes the
// runtime's queue wait for the game's frame before each generated frame
// queued after the release. A game without the quad layer is measured alike
// for comparison. Depth the game submits in the same shape (one swapchain,
// each eye's half) never reaches the runtime, Frame Generation on or off,
// and colour is still captured and shown through Frame Generation.
// `shown_lazy` (the default since 0.19.2; VECTORXR_FRAMEGEN_SHOWN_LAYERS=off
// otherwise, for what that costs): the quad layer's
// releases are kept lazy instead, so under the same load no frame of Frame
// Generation's waits for the game's work on the runtime's queue (the
// game's next wait on the quad's swapchain waits for its release on the
// CPU), and every refresh shows the quad's newest finished release, the
// game's latest or the one before it; the runtime still sees every call on
// it in order.
void TestSideBySideFrameGen(Game& game, const std::string& mode, bool shown_lazy = false) {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = mode;
    BeginCase("side-by-side-framegen-" + mode + (shown_lazy ? "-shown-lazy" : ""), game, settings);
    SetEnv("VECTORXR_FRAMEGEN_SHOWN_LAYERS", shown_lazy ? "" : "off");
    g_runtime.recommended_width = kSideBySideEyeWidth;
    g_runtime.recommended_height = kSideBySideEyeHeight;
    g_runtime.paced = true;
    {
        std::scoped_lock lock(g_runtime_mutex);
        // Each eye's half of the side-by-side image (the whole of an eye's
        // own output image).
        g_runtime.probes = {{0.25f, 0.5f}, {0.75f, 0.5f}};
    }
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(depthxr::CreateFrameGenGpu);
    SideBySideSession session;
    App& app = session.app;
    app = StartApp();
    session.create = std::make_unique<SessionCreate>(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &session.create->info, &app.session) == XR_SUCCESS &&
              Peer::Bridge().Active() && g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
              Peer::QueueSubstituted(),
          "the side-by-side session is bridged");
    Check(Count(Peer::Log(), kBridgedSessionLine) == 1, "the log tells once that the session is bridged");
    SetEnv("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "");
    constexpr const char* kShownLazyLine = "Frame Generation keeps releases of layers it does not capture lazy";
    Check(Count(Peer::Log(), kShownLazyLine) == (shown_lazy ? 1 : 0),
          std::string("the log tells once that shown layers are kept lazy, only when they are (") +
              (shown_lazy ? "on" : "off") + ")");
    BeginSession(app);
    session.eyes = MakeTarget(app, XPlaneInfo(2 * kSideBySideEyeWidth, kSideBySideEyeHeight), "both eyes side by side");
    session.ui = MakeTarget(app, XPlaneInfo(kQuadLayerSize, kQuadLayerSize), "the quad layer's");
    XrSwapchainCreateInfo depth_info = DepthInfo(1);
    depth_info.width = 2 * kSideBySideEyeWidth;
    depth_info.height = kSideBySideEyeHeight;
    session.depth = MakeTarget(app, depth_info, "both eyes' depth side by side");
    {
        std::scoped_lock lock(g_runtime_mutex);
        const StandInSwapchain* eyes = FindSwapchain(session.eyes.swapchain);
        const StandInSwapchain* ui = FindSwapchain(session.ui.swapchain);
        Check(eyes && ui && eyes->info.width == 2 * kSideBySideEyeWidth && eyes->info.format == 29 &&
                  (eyes->info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
                  (eyes->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) && ui->info.width == kQuadLayerSize &&
                  (ui->info.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
              "the runtime's swapchains are the bridge's copy destinations, whatever usage the game asked for");
    }
    Check(Peer::Bridge().Bridged(session.eyes.swapchain) && Peer::Bridge().Bridged(session.ui.swapchain) &&
              Peer::Bridge().Unmirrored(session.depth.swapchain),
          "both colour swapchains are bridged, depth is the game's own");

    // What the stand-in saw of each frame: its layers, and the runtime's
    // queue reaching it (with the quad layer read there).
    std::vector<SideBySideShown> extras;
    GameLoad* load = nullptr;
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.on_end_frame = [&](const XrFrameEndInfo& info, ShownFrame&) {
            StandIn& rt = g_runtime;
            Check(extras.size() == rt.frames.size(), "the stand-in notes every frame");
            SideBySideShown shown;
            // Before the runtime's count (no release reaches it meanwhile).
            shown.game_quad_releases = session.ui_released.load();
            shown.layers = info.layerCount;
            const XrCompositionLayerBaseHeader* first = info.layerCount ? info.layers[0] : nullptr;
            shown.projection_first = first && first->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION;
            if (shown.projection_first) {
                const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(first);
                for (std::uint32_t v = 0; v < 2 && v < projection.viewCount; ++v) {
                    shown.rects[v] = projection.views[v].subImage.imageRect;
                }
            }
            const XrCompositionLayerQuad* quad =
                info.layerCount >= 2 && info.layers[1] && info.layers[1]->type == XR_TYPE_COMPOSITION_LAYER_QUAD
                    ? reinterpret_cast<const XrCompositionLayerQuad*>(info.layers[1])
                    : nullptr;
            shown.quad_layer = quad && quad->subImage.swapchain == session.ui.swapchain;
            // A mark after everything queued on the runtime's queue: how long
            // a compositor would wait to show the frame.
            const auto start = std::chrono::steady_clock::now();
            shown.mark = ++rt.readback_value;
            if (load) {
                shown.during = load->Mark(rt.queue.Get(), shown.mark, shown.marked);
            } else {
                rt.queue->Signal(rt.readback.Get(), shown.mark);
            }
            Check(WaitFence(rt.readback.Get(), shown.mark, 10000), "the runtime's queue reaches the frame");
            shown.latency_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const StandInSwapchain* chain = quad ? FindSwapchain(quad->subImage.swapchain) : nullptr;
            if (shown.quad_layer && chain && chain->released && !chain->images12.empty()) {
                shown.quad_releases = chain->releases;
                Readback readback;
                std::vector<Color> texels;
                if (SubmitReadback(rt.queue.Get(), rt.readback.Get(), rt.readback_value,
                                   {{chain->images12[*chain->released].Get(), 0}}, readback)) {
                    shown.read_mark = readback.value;
                    if (FinishReadback(readback, texels)) {
                        shown.quad_texel = texels[0];
                    }
                    shown.read_latency_ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                }
            }
            extras.push_back(shown);
        };
    }
    const auto run = [&](int frames, const SideBySideFrameOptions& options = {}) {
        for (int i = 0; i < frames; ++i) {
            SideBySideFrame(session, game, options);
        }
    };
    // Frames in `options` until the runtime shows their layers: the frames
    // shown are the lead's refreshes behind the game's, and a GPU-bound
    // phase grows the lead past four of the game's frames (a canvas keeps
    // five real frames, so those before are still shown as paced).
    const auto settle = [&](const SideBySideFrameOptions& options) {
        run(4, options);
        const std::uint32_t layers = options.quad_layer ? 2u : 1u;
        for (int i = 0; i < 32; ++i) {
            {
                std::scoped_lock lock(g_runtime_mutex);
                if (!extras.empty() && extras.back().layers == layers) break;
            }
            run(1, options);
        }
    };
    // Every frame the runtime showed from `from` on: two views; the quad
    // layer after the projection (`quad_layer`) showing the game's latest
    // release; Frame Generation's outputs, or the game's image, each eye
    // its own half. How many showed Frame Generation's outputs.
    constexpr auto kEyeWidth = static_cast<std::int32_t>(kSideBySideEyeWidth);
    constexpr auto kEyeHeight = static_cast<std::int32_t>(kSideBySideEyeHeight);
    // Refreshes whose quad layer showed the game's release before its latest.
    std::size_t quad_behind = 0;
    const auto check_shown = [&](std::size_t from, const std::string& at, bool quad_layer) {
        std::scoped_lock lock(g_runtime_mutex);
        std::size_t own = 0;
        for (std::size_t i = from; i < g_runtime.frames.size(); ++i) {
            const ShownFrame& shown = g_runtime.frames[i];
            const SideBySideShown& extra = extras[i];
            const std::string where = at + ", runtime frame " + std::to_string(i);
            Check(shown.views.size() == 2 && extra.projection_first,
                  where + ": the runtime gets the projection first, of two views");
            if (quad_layer) {
                Check(extra.layers == 2 && extra.quad_layer,
                      where + ": the quad layer reaches the runtime after the projection (" +
                          std::to_string(extra.layers) + " layers)");
                const Color expected = QuadLayerColor(extra.quad_releases - 1);
                Check(Near(extra.quad_texel, expected),
                      where + ": the quad layer shows " + (extra.quad_texel ? Text(*extra.quad_texel) : "nothing") +
                          ", the game's latest release (" + std::to_string(extra.quad_releases) + ") rendered " +
                          Text(expected));
                // Lazy, the newest release whose rendering is done: the
                // game's latest, or the one before it.
                const int behind = extra.game_quad_releases - extra.quad_releases;
                Check(behind <= (shown_lazy ? 1 : 0),
                      where + ": the runtime had the game's releases of the quad layer's swapchain but " +
                          std::to_string(behind) + " (" + std::to_string(extra.quad_releases) + " of " +
                          std::to_string(extra.game_quad_releases) + ")");
                quad_behind += behind > 0 ? 1 : 0;
            } else {
                Check(extra.layers == 1, where + ": the projection alone reaches the runtime (" +
                                             std::to_string(extra.layers) + " layers, " +
                                             (shown.views.empty() || shown.views[0].swapchain == session.eyes.swapchain
                                                  ? "the game's image"
                                                  : "Frame Generation's") +
                                             ", " + std::to_string(i - from) + " frames into the check)");
            }
            const bool real = shown.views[0].swapchain == session.eyes.swapchain;
            for (int eye = 0; eye < 2; ++eye) {
                const ShownView& view = shown.views[eye];
                const std::string seen = where + " eye " + std::to_string(eye);
                Check(view.probed.size() == 2, seen + ": the runtime read the frame back");
                const XrRect2Di& rect = extra.rects[eye];
                if (real) {
                    // The game's image itself: the view names its eye's half.
                    Check(view.swapchain == session.eyes.swapchain && rect.offset.x == eye * kEyeWidth &&
                              rect.offset.y == 0 && rect.extent.width == kEyeWidth && rect.extent.height == kEyeHeight,
                          seen + ": the view names its eye's half of the game's image");
                    const std::optional<int> frame = SideBySideFrameOf(view.probed[0], 0);
                    Check(frame && Near(view.probed[1], SideBySideColor(*frame, 1)),
                          seen + ": the image shows one real frame's halves, " + Text(view.probed[0]) + " and " +
                              Text(view.probed[1]));
                    continue;
                }
                // Frame Generation's own output: its eye alone, the whole image.
                Check(view.swapchain != session.eyes.swapchain && view.swapchain != session.ui.swapchain &&
                          view.swapchain != shown.views[1 - eye].swapchain,
                      seen + ": a frame of Frame Generation's shows an output image of its own for each eye");
                Check(rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width == kEyeWidth &&
                          rect.extent.height == kEyeHeight,
                      seen + ": the output is the eye's whole image, " + std::to_string(rect.extent.width) + "x" +
                          std::to_string(rect.extent.height) + " at " + std::to_string(rect.offset.x));
                const std::uint8_t blue = SideBySideColor(0, eye)[2];
                for (const Color& texel : view.probed) {
                    Check(std::abs(static_cast<int>(texel[2]) - blue) <= 1 && texel != Color{0, 0, 0, 255},
                          seen + ": the output shows its eye's half, " + Text(texel));
                    bool rendered = false;
                    for (int frame = 0; frame < 8 && !rendered; ++frame) {
                        rendered = mode == "interpolate" ? Within(texel, SideBySideColor(frame + 7, eye),
                                                                  SideBySideColor(frame, eye))
                                                         : Near(texel, SideBySideColor(frame, eye));
                    }
                    Check(rendered, seen + ": the output shows " +
                                        (mode == "interpolate" ? "a frame between two real ones, "
                                                               : "a real frame's half, ") +
                                        Text(texel));
                }
            }
            own += real ? 0 : 1;
        }
        return own;
    };

    // The first frames pass straight through: the game's image, each view its half.
    run(2);
    check_shown(0, "passing through", true);
    // Engaged after two steady frames, until frames are generated between
    // the real ones (a loaded machine may analyze none in time for a while).
    run(6);
    const std::size_t engaged_from = ShownCount();
    const depthxr::FrameGenStatus engaged = Peer::FrameGen();
    Check(engaged.threaded, "Frame Generation presents from its own thread");
    int steady_frames = 0;
    do {
        run(24);
        steady_frames += 24;
    } while (Peer::FrameGen().generated == 0 && steady_frames < 96);
    const std::size_t own = check_shown(engaged_from, "steady", true);
    // Once its first frame is written, every refresh shows Frame
    // Generation's outputs, but for one it could not write, or whose real
    // frame was not captured yet (a loaded machine stalls it).
    const std::vector<ShownFrame> steady = Shown(engaged_from);
    const auto first_own = std::find_if(steady.begin(), steady.end(), [&](const ShownFrame& shown) {
        return shown.views[0].swapchain != session.eyes.swapchain;
    });
    const std::size_t from_first_own = static_cast<std::size_t>(steady.end() - first_own);
    depthxr::FrameGenStatus status = Peer::FrameGen();
    const std::uint64_t unwritten = status.unwritten - engaged.unwritten;
    const std::uint64_t unready = status.unready_capture - engaged.unready_capture;
    Check(status.generated > 0 && own >= static_cast<std::size_t>(steady_frames) &&
              own + unwritten + unready >= from_first_own,
          "steady: every refresh shows Frame Generation's outputs (" + std::to_string(own) + " of " +
              std::to_string(from_first_own) + " from its first, " + std::to_string(unwritten) + " not written, " +
              std::to_string(unready) + " not captured yet; " + std::to_string(status.generated) + " generated)");
    CheckValidation(game, "steady");

    // The game GPU-bound, with the quad layer and without it: every frame
    // ended while the game's work was held back, of Frame Generation's
    // outputs, waited for that work on the runtime's queue or not: the
    // queue reached its mark, or its quad layer read right after (as a
    // compositor reads what it shows), only once the work could run.
    // Frame Generation submits each frame as the runtime releases its wait,
    // as the game's wait is released: one submitted a moment before the
    // game's release of the quad layer is not behind it on the queue, but
    // reading it is.
    struct Backlog {
        std::size_t during{0}; // frames of Frame Generation's ended while the game's work was held back
        std::size_t waited{0}; // ...that the runtime's queue reached only once the work could run
        std::vector<double> latencies; // of those that waited
        std::vector<double> others;    // of the others
        std::uint64_t moved{0};    // frames Frame Generation generated meanwhile
        std::uint64_t repeated{0}; // refreshes it repeated the frame before at
        depthxr::FrameGenQueueSubstitution::ShownLayerCounts shown; // the quad's, meanwhile
        std::size_t behind{0}; // refreshes whose quad layer showed the release before the game's latest
        double game_ms{0.0};   // the game's 16 frames took
    };
    const auto loaded = [&](bool quad_layer, const std::string& at) {
        Backlog backlog;
        const depthxr::FrameGenStatus before = Peer::FrameGen();
        const depthxr::FrameGenQueueSubstitution::ShownLayerCounts shown_before = Peer::ShownLayers();
        const std::size_t behind_before = quad_behind;
        const std::size_t from = ShownCount();
        {
            GameLoad game_load(g_runtime.readback.Get());
            {
                std::scoped_lock lock(g_runtime_mutex);
                load = &game_load;
            }
            SideBySideFrameOptions options;
            options.quad_layer = quad_layer;
            options.load = &game_load;
            const auto game_start = std::chrono::steady_clock::now();
            run(16, options);
            backlog.game_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - game_start).count();
            game_load.Finish();
            std::scoped_lock lock(g_runtime_mutex);
            load = nullptr;
            for (std::size_t i = from; i < extras.size(); ++i) {
                const SideBySideShown& extra = extras[i];
                const std::vector<ShownView>& views = g_runtime.frames[i].views;
                if (views.empty() || views[0].swapchain == session.eyes.swapchain) continue;
                Check(extra.latency_ms < kGameLoadMs + 2000.0,
                      at + ": the runtime's queue reaches a frame of Frame Generation's within the game's work (" +
                          std::to_string(extra.latency_ms) + " ms)");
                std::optional<bool> waited =
                    extra.during ? game_load.Waited(*extra.during, extra.mark, extra.marked) : std::nullopt;
                double latency_ms = extra.latency_ms;
                if (waited && !*waited && extra.read_mark) {
                    waited = game_load.Waited(*extra.during, extra.read_mark, extra.marked);
                    latency_ms = extra.read_latency_ms;
                }
                if (!waited) continue;
                ++backlog.during;
                if (*waited) {
                    ++backlog.waited;
                    backlog.latencies.push_back(latency_ms);
                } else {
                    backlog.others.push_back(extra.latency_ms);
                }
            }
        }
        // Each frame's content held all the same.
        check_shown(from, at, quad_layer);
        const depthxr::FrameGenStatus after = Peer::FrameGen();
        backlog.moved = after.generated - before.generated;
        backlog.repeated = after.repeated - before.repeated;
        const depthxr::FrameGenQueueSubstitution::ShownLayerCounts shown_after = Peer::ShownLayers();
        backlog.shown.releases = shown_after.releases - shown_before.releases;
        backlog.shown.cpu_waits = shown_after.cpu_waits - shown_before.cpu_waits;
        backlog.shown.gpu_waits = shown_after.gpu_waits - shown_before.gpu_waits;
        backlog.behind = quad_behind - behind_before;
        return backlog;
    };
    const Backlog with_quad = loaded(true, "GPU-bound, with the quad layer");
    // The game's frames without the quad layer for a while first: frames
    // shown until one of them is real still have it.
    SideBySideFrameOptions no_quad_layer;
    no_quad_layer.quad_layer = false;
    settle(no_quad_layer);
    const Backlog without_quad = loaded(false, "GPU-bound, without the quad layer");
    const auto describe = [](const Backlog& backlog) {
        return std::to_string(backlog.waited) + " of " + std::to_string(backlog.during) + " (waited " +
               Spread(backlog.latencies) + "; the others " + Spread(backlog.others) + "; " +
               std::to_string(backlog.moved) + " generated, " + std::to_string(backlog.repeated) + " repeated; " +
               "the game's 16 frames in " + std::to_string(static_cast<int>(backlog.game_ms)) + " ms)";
    };
    if (shown_lazy) {
        // The quad layer's releases kept lazy: no frame of Frame
        // Generation's waits for the game's work, with the quad layer or
        // without it. Each of the game's releases of the quad (one a frame)
        // was held back, its next wait waiting for it on the CPU, on the
        // runtime's queue only past that wait's bound (far beyond the game's
        // held-back work).
        const auto counts = [](const Backlog& backlog) {
            return std::to_string(backlog.shown.releases) + " lazy releases, " +
                   std::to_string(backlog.shown.cpu_waits) + " waited for on the CPU, " +
                   std::to_string(backlog.shown.gpu_waits) + " on the runtime's queue; " +
                   std::to_string(backlog.behind) + " refreshes showed the release before the game's latest";
        };
        std::cout << g_case << ": shown layers kept lazy: the game's GPU work held back " << kGameLoadMs
                  << " ms a frame, the runtime's queue waited for it before frames of Frame Generation's ended "
                  << "meanwhile: with a quad layer released every frame " << describe(with_quad) << " ("
                  << counts(with_quad) << "); without it " << describe(without_quad) << ".\n";
        Check(with_quad.during > 0 && with_quad.waited == 0,
              "GPU-bound, with the quad layer kept lazy: no frame of Frame Generation's waits for the game's work on "
              "the runtime's queue: " +
                  describe(with_quad));
        // A release whose work outlasts the CPU wait's bound (100 ms: a busy
        // or software GPU) goes on the runtime's queue instead, as designed;
        // one of six local runs saw one (2026-10-14).
        Check(with_quad.shown.releases == 16 && with_quad.shown.cpu_waits > 0 && with_quad.shown.gpu_waits <= 2,
              "GPU-bound, with the quad layer kept lazy: its releases are held back until their work has run, "
              "the game's next wait waiting for them on the CPU: " +
                  counts(with_quad));
        Check(without_quad.during > 0 && without_quad.waited == 0 && without_quad.shown.releases == 0,
              "GPU-bound, without the quad layer: no frame of Frame Generation's waits for the game's work: " +
                  describe(without_quad));
        CheckValidation(game, "GPU-bound");
    } else {
        std::cout << g_case << ": KNOWN LIMITATION (the uncaptured-layer backlog, shown layers not lazy): the "
                  << "game's GPU work held back " << kGameLoadMs << " ms a frame, the runtime's queue waited for it "
                  << "before frames of Frame Generation's ended meanwhile: with a quad layer released every frame "
                  << describe(with_quad) << "; without it " << describe(without_quad)
                  << ". While a layer Frame Generation does not capture is shown, its frames wait for the game's "
                  << "(with VECTORXR_FRAMEGEN_SHOWN_LAYERS=off).\n";
        // What the code does with shown layers not lazy: each release of a
        // swapchain Frame Generation does not capture queues a wait for the
        // game's work on the runtime's queue
        // (FrameGenQueueSubstitution::Release), and every frame of its own
        // queued after it waits there (`shown_lazy` keeps them lazy
        // instead). Without the quad layer nothing of the game's is waited
        // for before them.
        Check(with_quad.waited > 0,
              "GPU-bound, with the quad layer: frames of Frame Generation's wait for the game's work on the runtime's "
              "queue: " +
                  describe(with_quad));
        Check(without_quad.during > 0 && without_quad.waited == 0,
              "GPU-bound, without the quad layer: no frame of Frame Generation's waits for the game's work: " +
                  describe(without_quad));
        CheckValidation(game, "GPU-bound");
        const depthxr::FrameGenQueueSubstitution::ShownLayerCounts off = Peer::ShownLayers();
        Check(off.releases == 0 && off.cpu_waits == 0 && off.gpu_waits == 0 && quad_behind == 0,
              "shown layers are not kept lazy with VECTORXR_FRAMEGEN_SHOWN_LAYERS=off");
    }

    // Depth in the same shape (one swapchain, each view's depth its half):
    // colour still captured and shown through Frame Generation; then, Frame
    // Generation toggled off, the game's frames reach the runtime as they
    // are, without the bridge-private depth. Unknown metadata is checked
    // separately below, once supported depth has passed generation.
    settle({});
    const depthxr::FrameGenStatus before_depth = Peer::FrameGen();
    const std::size_t depth_from = ShownCount();
    SideBySideFrameOptions with_depth;
    with_depth.depth = true;
    run(16, with_depth);
    const std::size_t depth_own = check_shown(depth_from, "depth", true);
    status = Peer::FrameGen();
    // Both eyes' halves of (nearly) every release; frames of Frame
    // Generation's shown from them (moved or written as exact copies).
    Check(status.captured >= before_depth.captured + 2 * 12 && depth_own >= 12,
          "depth: colour is still captured and shown through Frame Generation (captured " +
              std::to_string(status.captured - before_depth.captured) + " in 16 frames, " + std::to_string(depth_own) +
              " frames of its outputs shown, " + std::to_string(status.generated - before_depth.generated) +
              " generated)");
    Check(!Peer::ToggleFrameGen(), "Frame Generation toggled off");
    run(4, with_depth); // frames already in flight end through Frame Generation
    const std::size_t as_is_from = ShownCount();
    run(6, with_depth);
    Check(check_shown(as_is_from, "depth, Frame Generation off", true) == 0,
          "depth, Frame Generation off: the game's frames reach the runtime as they are");
    {
        std::scoped_lock lock(g_runtime_mutex);
        for (std::size_t i = depth_from; i < g_runtime.frames.size(); ++i) {
            for (const ShownView& view : g_runtime.frames[i].views) {
                Check(!view.depth, "depth: the runtime gets no depth in a bridged session's views");
                if (i >= as_is_from) {
                    Check(view.next_type == XR_TYPE_UNKNOWN,
                          "depth-only, Frame Generation off: the stripped view has no other metadata");
                }
            }
        }
        const StandInSwapchain* depth = FindSwapchain(session.depth.swapchain);
        Check(depth && depth->releases == 26, "depth: the depth swapchain's releases reach the runtime");
    }
    std::string log = Peer::Log();
    Check(Contains(log, "keeps the game's depth to itself"), "depth: the log tells depth is left out");

    for (const Target* target : {&session.eyes, &session.ui}) {
        const std::string describe = Peer::Bridge().Describe(target->swapchain);
        Check(Contains(describe, "skippedCopies=0"), "every release copied: " + describe);
    }
    log = Peer::Log();
    Check(!Contains(log, "the runtime refused") && !Contains(log, "could not write") &&
              !Contains(log, "Frame Generation could not capture") && !Contains(log, "[error] Direct3D 11 bridge:") &&
              !Contains(log, "Frame Generation paused"),
          "nothing was refused, failed or paused");
    // The layer logged no error of its own (the test's reads of the log are logged as errors). In
    // software, the GPU-bound frames above can outlast Frame Generation's wait for its own work of
    // three frames before (100 ms): it skips that frame, as after a GPU stall (a hosted CI runner's
    // WARP, 2026-10-13). On a GPU that is an error here too.
    const std::size_t software_waits =
        SoftwareAdapter(game)
            ? Count(log, "[error] Frame Generation could not generate a frame: D3D12 Frame Generation GPU work did "
                         "not finish in time") +
                  Count(log, "[error] Frame Generation could not generate a frame: D3D12 Frame Generation synthesis "
                             "did not finish in time")
            : 0;
    Check(Count(log, "[error]") == Count(log, "[error] D3D11 bridge layer tests: log read") + software_waits,
          "the layer logged no error" + LayerErrors(log));
    CheckValidation(game, "depth");
    if (mode == "extrapolate" && shown_lazy) {
        // Unknown metadata is a different contract from supported depth.
        // Re-engage on depth alone, then verify the unknown-chain drain and
        // pass-through without teaching the presenter to silently drop it.
        Check(Peer::ToggleFrameGen(), "Frame Generation toggled back on for the unknown-depth-chain check");
        const auto generated_before = Peer::FrameGen().generated;
        for (int i = 0; i < 48 && Peer::FrameGen().generated == generated_before; ++i) run(1, with_depth);
        Check(Peer::FrameGen().generated > generated_before && Peer::FrameGen().threaded,
              "depth alone re-engages generation before unknown metadata is introduced");
        SideBySideFrameOptions unknown_depth = with_depth;
        unknown_depth.unknown_after_depth = true;
        run(4, unknown_depth); // drain frames already handed to the presenter
        const std::size_t unknown_from = ShownCount();
        const auto refused = Peer::FrameGen();
        run(6, unknown_depth);
        Check(check_shown(unknown_from, "unknown metadata after depth", true) == 0,
              "unknown metadata is passed through on the game's own images");
        const auto after_unknown = Peer::FrameGen();
        Check(after_unknown.state == depthxr::FrameGenState::kUnavailable && !after_unknown.threaded &&
                  Contains(after_unknown.reason, "projection view extension type " + std::to_string(kApplicationStructType)) &&
                  after_unknown.generated == refused.generated && after_unknown.captured == refused.captured,
              "unknown metadata after depth stops capture/generation with a precise reason");
        {
            std::scoped_lock lock(g_runtime_mutex);
            Check(g_runtime.frames.size() >= unknown_from + 6, "unknown metadata frames reached the runtime");
            for (std::size_t i = unknown_from; i < g_runtime.frames.size(); ++i) {
                for (const ShownView& view : g_runtime.frames[i].views) {
                    Check(!view.depth && view.next_type == kApplicationStructType,
                          "the bridge strips private depth and preserves the following unknown metadata");
                }
            }
        }
        const std::size_t supported_from = ShownCount();
        run(6, with_depth);
        Check(Peer::FrameGen().state == depthxr::FrameGenState::kUnavailable &&
                  check_shown(supported_from, "supported depth after the refusal", true) == 0,
              "a later supported frame cannot silently clear the unsupported-chain latch");
        CheckValidation(game, "unknown metadata after depth");
    }
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.on_end_frame = nullptr;
    }
    for (const Target* target : {&session.eyes, &session.ui, &session.depth}) {
        Check(app.api.destroy_swapchain(target->swapchain) == XR_SUCCESS, "a swapchain is destroyed");
    }
    StopApp(app);
    Check(Count(Peer::Log(), "Frame Generation's shown layers at session end") == (shown_lazy ? 1 : 0),
          "the session's end tells what keeping shown layers lazy did, only when it was on");
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}

// 15. A game quitting while Frame Generation presents from its own thread,
// the runtime holding that thread inside xrWaitFrame until the session ends
// (DCS hung so on quitting, 2026-10-09). With `ends_session` the game ends
// its session first; otherwise it destroys its swapchain, space and session
// at once. Either way its teardown is held at most about the teardown wait
// (VECTORXR_FRAMEGEN_TEARDOWN_WAIT_MS): when the game did not end the
// session, the layer ends the runtime's session itself, which lets the
// thread go. Nothing is left behind, the runtime sees xrEndSession before
// xrDestroySession, and the log tells what the thread was in.
void TestQuitWhilePresenterHeld(Game& game, bool ends_session) {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = "extrapolate";
    BeginCase(std::string("quit-presenter-held") + (ends_session ? "-session-ended" : ""), game, settings);
    SetEnv("VECTORXR_FRAMEGEN_TEARDOWN_WAIT_MS", "400");
    g_runtime.recommended_width = kSideBySideEyeWidth;
    g_runtime.recommended_height = kSideBySideEyeHeight;
    g_runtime.paced = true;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(depthxr::CreateFrameGenGpu);
    SideBySideSession session;
    App& app = session.app;
    app = StartApp();
    session.create = std::make_unique<SessionCreate>(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &session.create->info, &app.session) == XR_SUCCESS &&
              Peer::Bridge().Active(),
          "a bridged session");
    SetEnv("VECTORXR_FRAMEGEN_TEARDOWN_WAIT_MS", "");
    BeginSession(app);
    session.eyes = MakeTarget(app, XPlaneInfo(2 * kSideBySideEyeWidth, kSideBySideEyeHeight), "both eyes side by side");
    SideBySideFrameOptions options;
    options.quad_layer = false;
    int frames = 0;
    do {
        SideBySideFrame(session, game, options);
    } while (!Peer::FrameGen().threaded && ++frames < 48);
    Check(Peer::FrameGen().threaded, "Frame Generation presents from its own thread");
    for (int i = 0; i < 6; ++i) {
        SideBySideFrame(session, game, options);
    }

    {
        std::scoped_lock lock(g_hold_mutex);
        g_hold_waits = true;
    }
    const auto held_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_waits_held.load() == 0 && std::chrono::steady_clock::now() < held_until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(g_waits_held.load() == 1, "the runtime holds Frame Generation's presenter thread in xrWaitFrame");
    const auto start = std::chrono::steady_clock::now();
    if (ends_session) {
        Check(app.api.end_session(app.session) == XR_SUCCESS, "xrEndSession");
    }
    Check(app.api.destroy_swapchain(session.eyes.swapchain) == XR_SUCCESS, "the eyes' swapchain is destroyed");
    Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
    app.space = XR_NULL_HANDLE;
    Check(app.api.destroy_session(app.session) == XR_SUCCESS, "xrDestroySession");
    app.session = XR_NULL_HANDLE;
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "  quitting with the presenter thread held took " << std::fixed << std::setprecision(2) << took
              << " s" << std::endl;
    Check(took < 2.0, "the game's teardown is held at most about the teardown wait (" + std::to_string(took) + " s)");
    Check(g_waits_held.load() == 0 && !Peer::FrameGenPresenterAbandoned(),
          "the presenter thread was let go, not left behind");
    {
        std::scoped_lock lock(g_runtime_mutex);
        const std::ptrdiff_t ended = g_runtime.Position("xrEndSession");
        const std::ptrdiff_t destroyed = g_runtime.Position("xrDestroySession");
        Check(ended >= 0 && destroyed > ended, "the runtime's session ended before it was destroyed");
    }
    const std::string log = Peer::Log();
    Check(Contains(log, "its presenter thread has not stopped after") &&
              Contains(log, "in the runtime's xrWaitFrame for"),
          "the log tells what the presenter thread was in");
    Check(Contains(log, ends_session ? "xrEndSession: Frame Generation's presenter thread was still in the runtime; it "
                                       "stopped once the runtime's session ended."
                                     : "the layer ended the runtime's session (result=0), and the thread stopped."),
          "the log tells how the presenter thread was let go");
    Check(Contains(log, "xrDestroySession requested by application."), "the log tells the session's teardown");
    std::cout << "  teardown watch " << (Contains(log, "Teardown: xrDestroySession has run") ||
                                                 Contains(log, "Teardown: xrEndSession has run")
                                             ? "reported"
                                             : "did not report (the teardown ended first)")
              << std::endl;
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    Check(GameRuns(game), "the game's device works");
    EndCase(game);
}

// 8. The bridge's device removed: the session is lost. Last: Direct3D 12
// makes one device per adapter in a process.
void TestDeviceLost(Game& game) {
    BeginCase("device-lost", game);
    App app = StartApp();
    SessionCreate create(game.device.Get(), app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session");
    BeginSession(app);
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    const std::vector<Color> colors{FrameColor(0, 0), FrameColor(0, 1)};
    Check(Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS &&
              Near(g_runtime.frames.back().views[0].texel, colors[0]),
          "a frame before the loss");
    ComPtr<ID3D12Device5> device5;
    if (FAILED(BridgeBinding()->device->QueryInterface(IID_PPV_ARGS(&device5)))) {
        std::cout << "device loss not tested: no ID3D12Device5\n";
        Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS, "xrDestroySwapchain");
        StopApp(app);
        EndCase(game);
        return;
    }
    CheckValidation(game, "before the device's removal");
    device5->RemoveDevice();
    device5.Reset();
    const std::size_t shown = g_runtime.frames.size();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const XrResult result = Frame(app, game, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}});
        Check(result == XR_ERROR_SESSION_LOST,
              "xrEndFrame " + std::to_string(attempt + 1) + " after the removal loses the session: " +
                  std::to_string(result));
        Check(g_runtime.frames.size() == shown, "the frame does not reach the runtime");
    }
    const std::string log = Peer::Log();
    Check(Count(log, "the session is lost") == 1, "the loss is logged once, for two frames");
    Check(Count(log, kBridgedSessionLine) == 1, "the session's bridge was logged once");
    const auto start = std::chrono::steady_clock::now();
    Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS, "xrDestroySwapchain on a lost session");
    StopApp(app);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(seconds < 10.0, "teardown does not wait on a removed device: " + std::to_string(seconds) + " s");
    Check(GameRuns(game), "the game's Direct3D 11 device runs on");
    // What Direct3D 12 reports after the removal is the removal's; Direct3D
    // 11's is still checked.
    EndCase(game, true);
}

// 11. The app's status line for Frame Generation (the runtime relay's), in
// the OSD's words: while composed quad views frames could not be written for
// a while, it says generated frames could not be shown, whatever else the
// state says meanwhile.
void CheckFrameGenStatusLine() {
    const std::string dot = " \xC2\xB7 ";
    const std::string arrow = " \xE2\x86\x92 ";
    depthxr::FrameGenStatus status;
    status.state = depthxr::FrameGenState::kActive;
    status.multiplier = 2;
    status.app_fps = 45.2;
    status.distinct_fps = 89.6;
    status.quad = true;
    std::string line = Peer::FrameGenStatusReason(status);
    Check(line == "Interpolate" + dot + "2x" + dot + "45" + arrow + "90 fps", "composing: " + line);
    status.quad_declining = true;
    status.distinct_fps = 45.0;
    const std::string declining = Peer::FrameGenStatusReason(status);
    Check(Contains(declining, "could not be shown") && Contains(declining, "retrying soon") && !Contains(declining, " fps"),
          "declining: " + declining);
    // Frames passing straight through meanwhile, standing by.
    status.full_rate = true;
    status.reason = "frames pass straight through";
    line = Peer::FrameGenStatusReason(status);
    Check(line == declining, "declining, at full rate: " + line);
    status.dynamic = true;
    status.standby = true;
    line = Peer::FrameGenStatusReason(status);
    Check(line == declining, "declining, Dynamic standing by: " + line);
    status.quad_declining = false;
    status.reason.clear();
    Check(Peer::FrameGenStatusReason(status) == "Dynamic" + dot + "game at 45 fps", "standing by");
    status.state = depthxr::FrameGenState::kPaused;
    status.reason = "paused";
    status.quad_declining = true;
    Check(Peer::FrameGenStatusReason(status) == "paused", "paused: the reason alone");
}

// Everything the layer reads and writes on the host goes to the test's own directory.
void IsolateHost() {
    wchar_t temp[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, temp);
    g_root = std::filesystem::path(temp) /
             ("vectorxr-d3d11-bridge-layer-tests-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(g_root, error);
    std::filesystem::create_directories(g_root);
    SetEnv("VECTORXR_CONFIG_PATH", (g_root / "settings.json").string());
    SetEnv("VECTORXR_LOG_PATH", (g_root / "logs" / "vectorxr.log").string());
    SetEnv("VECTORXR_SEEN_APPS_PATH", (g_root / "seen-apps.json").string());
    SetEnv("VECTORXR_RUNTIME_RELAY_PATH", (g_root / "runtime").string());
    SetEnv("VECTORXR_RUNTIME_PACING_PATH", (g_root / "runtime-pacing.json").string());
    SetEnv("VECTORXR_TURBO_METRICS_PATH", (g_root / "turbo-metrics.json").string());
    SetEnv("XR_RUNTIME_JSON", (g_root / "no-runtime.json").string());
    // Whatever the host sets for the bridge does not apply.
    SetEnv("VECTORXR_FRAMEGEN_QUEUE", "");
    SetEnv("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "");
    SetEnv("VECTORXR_D3D11_BRIDGE", "");
    SetEnv("VECTORXR_D3D11_BRIDGE_DEBUG", "");
}

// Case 13 (TestBridgedQuadDeviceLost) in a process of its own: this program
// again, on the first hardware adapter ("hw"), then `kQuadDeviceLost`. Its
// exit code.
constexpr const char* kQuadDeviceLost = "quad-device-lost";

int RunQuadDeviceLostProcess() {
    char path[MAX_PATH + 1]{};
    Check(GetModuleFileNameA(nullptr, path, MAX_PATH) > 0, "this program's path");
    const std::string quoted = std::string("\"") + path + "\"";
    std::cout << std::flush;
    return static_cast<int>(_spawnl(_P_WAIT, path, quoted.c_str(), "hw", kQuadDeviceLost, nullptr));
}

} // namespace

int main(int argc, char** argv) {
    const bool hardware = argc > 1 && std::string(argv[1]) == "hw";
    const bool quad_device_lost = std::string(argv[argc - 1]) == kQuadDeviceLost;
    // Before any Direct3D 12 device exists.
    ComPtr<ID3D12Debug> debug;
    const HRESULT debug_result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (SUCCEEDED(debug_result)) {
        debug->EnableDebugLayer();
    } else {
        debug.Reset();
        std::ostringstream hr;
        hr << std::hex << static_cast<std::uint32_t>(debug_result);
        std::cout << "WARNING: no Direct3D 12 debug layer (D3D12GetDebugInterface failed, hr=0x" << hr.str()
                  << "; install the Graphics Tools optional feature): the bridge's Direct3D 12 work is NOT validated, "
                     "only the behavioural checks run\n"
                  << std::flush;
    }
    Game game;
    if (!MakeGame(hardware, game)) {
        std::cout << "skipped: no Direct3D 11 device\n";
        return kSkip;
    }
    ComPtr<ID3D11InfoQueue> queue11;
    if (game.debug && SUCCEEDED(game.device.As(&queue11))) {
        StoreErrorsOnly(queue11.Get());
    } else {
        std::cout << "WARNING: no Direct3D 11 debug layer (its SDK layers are not installed): the game's Direct3D 11 "
                     "work is NOT validated, only the behavioural checks run\n"
                  << std::flush;
    }
    IsolateHost();
    Peer::UseFrameGenGpu(MakeStandInGpu);
    if (quad_device_lost) {
        // Only run once the cases before it found this machine bridges.
        if (SoftwareAdapter(game)) {
            std::cout << "skipped: quad views device loss is not tested on WARP\n";
            return kSkip;
        }
        TestBridgedQuadDeviceLost(game);
        g_runtime = StandIn{};
        Peer::ReleaseLog(g_root / "released" / "vectorxr.log");
        std::error_code error;
        std::filesystem::remove_all(g_root, error);
        return 0;
    }
    CheckFrameGenStatusLine();
    const bool bridged = TestBridgedSession(game);
    if (bridged) {
        TestKeepGameQueue(game);
        TestBridgeOffSession(game);
        TestRefusedD3D12Session(game);
        TestInstanceExtensions(game);
        TestDestroyInstanceWithSession(game);
        TestBridgedQuadViewsComposed(game, false);
        TestBridgedQuadViewsComposed(game, true);
        TestBridgedQuadFrameGen(game, "extrapolate");
        TestBridgedQuadFrameGen(game, "interpolate");
        TestBridgedQuadFrameGenGeometry(game);
        TestSideBySideFrameGen(game, "extrapolate");
        TestSideBySideFrameGen(game, "interpolate");
        TestSideBySideFrameGen(game, "extrapolate", true);
        TestSideBySideFrameGen(game, "interpolate", true);
        TestQuitWhilePresenterHeld(game, false);
        TestQuitWhilePresenterHeld(game, true);
        TestDeviceLost(game);
    }
    g_runtime = StandIn{};
    Peer::ReleaseLog(g_root / "released" / "vectorxr.log");
    std::error_code error;
    std::filesystem::remove_all(g_root, error);
    if (!bridged) {
        return kSkip;
    }
    if (SoftwareAdapter(game)) {
        // Its Direct3D 12 work is still running when it reads as done after
        // the device's removal: letting it go at the teardown crashes WARP.
        std::cout << "quad views device loss not tested on WARP (`hw` tests it)\n";
    } else {
        const int quad_device_lost_code = RunQuadDeviceLostProcess();
        Check(quad_device_lost_code == 0, "the quad views device loss case (a process of its own) exited with " +
                                              std::to_string(quad_device_lost_code));
    }
    std::cout << "d3d11 bridge layer tests passed (" << (hardware ? "hardware" : "WARP")
              << (debug ? ", D3D12 debug layer" : ", WITHOUT the D3D12 debug layer")
              << (game.debug ? ", D3D11 debug layer" : ", WITHOUT the D3D11 debug layer") << ")\n";
    return 0;
}
