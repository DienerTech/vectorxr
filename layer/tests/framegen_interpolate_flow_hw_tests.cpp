// Positive hardware demonstration of the Dynamic Interpolate flow controller.
// Real D3D12/NVIDIA optical flow, a paced stand-in OpenXR runtime, and ordinary
// static source textures. The research switch only postpones CPU acceptance of
// completed vectors; no GPU waits, stalls, device loss, or invalid resources are injected.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_presenter.h"
#include "depthxr/logger.h"
#include "test_adapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace depthxr;
using namespace std::chrono_literals;
using Microsoft::WRL::ComPtr;

namespace {
using Clock = std::chrono::steady_clock;
constexpr XrDuration kPeriod = 11'111'111;
constexpr std::int64_t kFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr UINT kWidth = 512, kHeight = 256;
const XrSession kSession = reinterpret_cast<XrSession>(0x1);
const XrSpace kSpace = reinterpret_cast<XrSpace>(0x2);
const XrSwapchain kLeft = reinterpret_cast<XrSwapchain>(0x11);
const XrSwapchain kRight = reinterpret_cast<XrSwapchain>(0x12);

void Require(bool ok, const std::string& why) {
    // Exceptions unwind FrameGenPresenter first, draining its normal GPU work
    // before any texture/device/runtime is destroyed. Never terminate a live GPU test.
    if (!ok) throw std::runtime_error(why);
}

struct ScopedEnvironment {
    std::string name, previous;
    bool present{false};
    ScopedEnvironment(const char* variable, const char* value) : name(variable) {
        if (const char* old = std::getenv(variable)) { previous = old; present = true; }
        _putenv_s(variable, value);
    }
    ~ScopedEnvironment() { _putenv_s(name.c_str(), present ? previous.c_str() : ""); }
};

struct Swapchains {
    struct Chain {
        std::array<ComPtr<ID3D12Resource>, 3> textures;
        std::deque<UINT> acquired;
        UINT cursor{0};
        UINT waited{0};
    };
    ComPtr<ID3D12Device> device;
    std::mutex mutex;
    std::map<XrSwapchain, Chain> chains;
    std::uint64_t next{0x100};
    std::atomic<bool> ordering_error{false};
    std::uint64_t acquires{0}, waits{0}, releases{0};
    std::vector<std::string> errors;

    void RecordErrorLocked(const char* call, XrSwapchain handle, const Chain* chain) {
        ordering_error = true;
        if (errors.size() >= 24) return;
        errors.push_back(std::string(call) + " chain=" +
                         std::to_string(reinterpret_cast<std::uintptr_t>(handle)) +
                         " acquired=" + std::to_string(chain ? chain->acquired.size() : 0) +
                         " waited=" + std::to_string(chain ? chain->waited : 0) +
                         " cursor=" + std::to_string(chain ? chain->cursor : 0) +
                         " thread=" + std::to_string(GetCurrentThreadId()));
    }

    void PrintDiagnostics() {
        std::scoped_lock lock(mutex);
        std::cout << "Swapchain call counts: acquire=" << acquires << " wait=" << waits
                  << " release=" << releases << ", ordering errors=" << errors.size() << '\n';
        for (const auto& error : errors) std::cout << "SWAPCHAIN ORDER ERROR: " << error << '\n';
    }

    bool Add(XrSwapchain handle, UINT width, UINT height) {
        Chain chain;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (auto& texture : chain.textures) {
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                      IID_PPV_ARGS(&texture)))) return false;
        }
        chains.emplace(handle, std::move(chain));
        return true;
    }
};
Swapchains* g_swapchains{};

XrResult XRAPI_PTR Formats(XrSession, UINT capacity, UINT* count, std::int64_t* formats) {
    *count = 1;
    if (capacity && formats) formats[0] = kFormat;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* handle) {
    std::scoped_lock lock(g_swapchains->mutex);
    *handle = reinterpret_cast<XrSwapchain>(++g_swapchains->next);
    return g_swapchains->Add(*handle, info->width, info->height) ? XR_SUCCESS : XR_ERROR_OUT_OF_MEMORY;
}
XrResult XRAPI_PTR Destroy(XrSwapchain handle) {
    std::scoped_lock lock(g_swapchains->mutex);
    g_swapchains->chains.erase(handle);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Images(XrSwapchain handle, UINT capacity, UINT* count, XrSwapchainImageBaseHeader* images) {
    std::scoped_lock lock(g_swapchains->mutex);
    const auto found = g_swapchains->chains.find(handle);
    if (found == g_swapchains->chains.end()) return XR_ERROR_HANDLE_INVALID;
    *count = 3;
    if (!capacity) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* native = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (UINT index = 0; index < 3; ++index) {
        native[index].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        native[index].texture = found->second.textures[index].Get();
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Acquire(XrSwapchain handle, const XrSwapchainImageAcquireInfo*, UINT* index) {
    std::scoped_lock lock(g_swapchains->mutex);
    ++g_swapchains->acquires;
    const auto found = g_swapchains->chains.find(handle);
    if (found == g_swapchains->chains.end()) {
        g_swapchains->RecordErrorLocked("acquire unknown chain", handle, nullptr);
        return XR_ERROR_HANDLE_INVALID;
    }
    auto& chain = found->second;
    if (chain.acquired.size() >= chain.textures.size()) {
        g_swapchains->RecordErrorLocked("acquire all images already owned", handle, &chain);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *index = chain.cursor++ % 3;
    chain.acquired.push_back(*index);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Wait(XrSwapchain handle, const XrSwapchainImageWaitInfo*) {
    std::scoped_lock lock(g_swapchains->mutex);
    ++g_swapchains->waits;
    const auto found = g_swapchains->chains.find(handle);
    if (found == g_swapchains->chains.end()) {
        g_swapchains->RecordErrorLocked("wait unknown chain", handle, nullptr);
        return XR_ERROR_HANDLE_INVALID;
    }
    auto& chain = found->second;
    if (chain.waited >= chain.acquired.size()) {
        g_swapchains->RecordErrorLocked("wait without an acquired/unwaited image", handle, &chain);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    ++chain.waited;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Release(XrSwapchain handle, const XrSwapchainImageReleaseInfo*) {
    std::scoped_lock lock(g_swapchains->mutex);
    ++g_swapchains->releases;
    const auto found = g_swapchains->chains.find(handle);
    if (found == g_swapchains->chains.end()) {
        g_swapchains->RecordErrorLocked("release unknown chain", handle, nullptr);
        return XR_ERROR_HANDLE_INVALID;
    }
    auto& chain = found->second;
    if (chain.acquired.empty() || !chain.waited) {
        g_swapchains->RecordErrorLocked("release without an acquired/waited image", handle, &chain);
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    chain.acquired.pop_front();
    --chain.waited;
    return XR_SUCCESS;
}

struct Runtime {
    Clock::time_point epoch{Clock::now()};
    std::mutex mutex;
    std::int64_t tick{0};
    bool begun{false};
    int outstanding_waits{0};
    std::atomic<bool> ordering_error{false};
    std::uint64_t waits{0}, begins{0}, submissions{0}, discarded{0};
    XrTime last_returned{0}, last_submitted{0};
    std::vector<std::string> errors;

    void RecordErrorLocked(const char* call, XrTime submitted = 0) {
        ordering_error = true;
        if (errors.size() >= 24) return;
        errors.push_back(std::string(call) + " waits=" + std::to_string(waits) +
                         " begins=" + std::to_string(begins) + " ends=" + std::to_string(submissions) +
                         " outstanding_waits=" + std::to_string(outstanding_waits) +
                         " begun=" + std::to_string(begun) + " last_returned=" + std::to_string(last_returned) +
                         " last_submitted=" + std::to_string(last_submitted) + " current=" + std::to_string(submitted) +
                         " thread=" + std::to_string(GetCurrentThreadId()));
    }

    void PrintDiagnostics() {
        std::scoped_lock lock(mutex);
        std::cout << "Runtime call counts: wait=" << waits << " begin=" << begins << " end=" << submissions
                  << " discarded=" << discarded << " outstanding_waits=" << outstanding_waits
                  << " begun=" << begun << " last_returned=" << last_returned
                  << " last_submitted=" << last_submitted << ", ordering errors=" << errors.size() << '\n';
        for (const auto& error : errors) std::cout << "RUNTIME ORDER ERROR: " << error << '\n';
    }

    FrameGenRuntimeCalls Calls() {
        FrameGenRuntimeCalls calls;
        calls.wait = [this](XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
            std::int64_t released_tick;
            {
                std::scoped_lock lock(mutex);
                ++waits;
                if (++outstanding_waits > 1) RecordErrorLocked("wait overlaps an outstanding wait");
                // Like a compositor: one wait per physical refresh; missed ticks
                // are skipped, and predicted display time stays one refresh ahead.
                const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch).count();
                tick = std::max(tick + 1, elapsed / kPeriod + 1);
                released_tick = tick;
            }
            std::this_thread::sleep_until(epoch + std::chrono::nanoseconds(released_tick * kPeriod));
            state->predictedDisplayPeriod = kPeriod;
            state->predictedDisplayTime = 1'000'000'000 + (released_tick + 1) * kPeriod;
            state->shouldRender = XR_TRUE;
            {
                std::scoped_lock lock(mutex);
                last_returned = state->predictedDisplayTime;
            }
            return XR_SUCCESS;
        };
        calls.begin = [this](XrSession, const XrFrameBeginInfo*) {
            std::scoped_lock lock(mutex);
            ++begins;
            if (outstanding_waits != 1) RecordErrorLocked("begin without exactly one preceding wait");
            --outstanding_waits;
            const bool open = begun;
            begun = true;
            if (open) {
                // The production presenter deliberately leaves a frame open when
                // no current content can be shown. OpenXR discards it at this next
                // begin; this matches Runtime in framegen_presenter_tests.cpp.
                ++discarded;
                return XR_FRAME_DISCARDED;
            }
            return XR_SUCCESS;
        };
        calls.submit = [this](XrSession, const XrFrameEndInfo* info) {
            std::scoped_lock lock(mutex);
            ++submissions;
            if (!begun) RecordErrorLocked("end without a begun frame", info->displayTime);
            if (info->displayTime <= last_submitted) RecordErrorLocked("non-increasing submitted display time", info->displayTime);
            begun = false;
            last_submitted = info->displayTime;
            return XR_SUCCESS;
        };
        return calls;
    }
};

// Ordinary startup rendering, completed once before a presenter starts reading.
// This is a normal fence completion wait; it neither holds a GPU queue nor injects a delay.
void InitializeSources(Swapchains& swapchains, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Fence> fence;
    Require(SUCCEEDED(swapchains.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "startup allocator");
    Require(SUCCEEDED(swapchains.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                        IID_PPV_ARGS(&commands))), "startup command list");
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 6};
    Require(SUCCEEDED(swapchains.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors))), "startup RTV descriptors");
    auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = swapchains.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (XrSwapchain handle : {kLeft, kRight}) {
        for (auto& texture : swapchains.chains.at(handle).textures) {
            swapchains.device->CreateRenderTargetView(texture.Get(), &rtv, descriptor);
            const float background[4]{0.05f, 0.15f, 0.25f, 1.0f};
            commands->ClearRenderTargetView(descriptor, background, 0, nullptr);
            const float bright[4]{0.8f, 0.65f, 0.4f, 1.0f};
            for (UINT row = 0; row < 8; ++row) {
                for (UINT column = 0; column < 16; ++column) {
                    if ((row + column) % 2) continue;
                    const D3D12_RECT rect{static_cast<LONG>(column * 32), static_cast<LONG>(row * 32),
                                          static_cast<LONG>((column + 1) * 32), static_cast<LONG>((row + 1) * 32)};
                    commands->ClearRenderTargetView(descriptor, bright, 1, &rect);
                }
            }
            descriptor.ptr += stride;
        }
    }
    Require(SUCCEEDED(commands->Close()), "startup commands close");
    Require(SUCCEEDED(swapchains.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "startup fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "startup fence event");
    ID3D12CommandList* lists[]{commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    const HRESULT signaled = queue->Signal(fence.Get(), 1);
    const HRESULT armed = fence->SetEventOnCompletion(1, event);
    // Deliberately no timeout or forced process exit: GPU work must finish before resources go away.
    if (SUCCEEDED(signaled) && SUCCEEDED(armed)) WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    Require(SUCCEEDED(signaled) && SUCCEEDED(armed), "startup fence completion");
}

int Run() {
    ScopedEnvironment delay("VECTORXR_FRAMEGEN_INTERPOLATE_DELAY_MS", "100");
    ScopedEnvironment duration("VECTORXR_FRAMEGEN_INTERPOLATE_DELAY_FOR_MS", "3500");
    ScopedEnvironment legacy("VECTORXR_FRAMEGEN_INTERPOLATE_HARDWARE", "");
    const auto own_appdata = std::filesystem::temp_directory_path() / "vectorxr-interpolate-flow-hw" /
                             ("appdata-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(own_appdata);
    ScopedEnvironment appdata("LOCALAPPDATA", own_appdata.string().c_str());
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    std::string why;
    auto adapter = test_adapter::FindDxgiAdapter(static_cast<IDXGIFactory1*>(factory.Get()), test_adapter::Wanted().value_or("NVIDIA"), why);
    if (!adapter) { std::cout << "skipped: " << why << '\n'; return 77; }
    DXGI_ADAPTER_DESC1 description{};
    Require(SUCCEEDED(adapter->GetDesc1(&description)), "adapter description");
    if (description.VendorId != 0x10de) { std::cout << "skipped: NVIDIA optical flow hardware required\n"; return 77; }
    std::cout << "D3D12 hardware fallback demonstration on " << test_adapter::Utf8(description.Description) << '\n';
    Swapchains swapchains;
    Require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&swapchains.device))), "D3D12 hardware device");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    Require(SUCCEEDED(swapchains.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))), "D3D12 game queue");
    // As in the layer's queue substitution: the application keeps its game
    // queue, while the runtime synchronizes with the separate present queue.
    // This enables the real backend's just-in-time Dynamic presentation path.
    ComPtr<ID3D12CommandQueue> runtime_queue;
    D3D12_COMMAND_QUEUE_DESC runtime_desc{};
    runtime_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    runtime_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    Require(SUCCEEDED(swapchains.device->CreateCommandQueue(&runtime_desc, IID_PPV_ARGS(&runtime_queue))),
            "D3D12 runtime/presenter queue");
    Require(swapchains.Add(kLeft, kWidth, kHeight) && swapchains.Add(kRight, kWidth, kHeight), "application source textures");
    InitializeSources(swapchains, queue.Get());
    g_swapchains = &swapchains;
    Runtime runtime;
    const auto calls = runtime.Calls();
    std::filesystem::path log_path;
    FrameGenStatus final;
    bool saw_shader = false, saw_probe = false, recovered = false;
    {
        Logger logger;
        const auto directory = std::filesystem::temp_directory_path() / "vectorxr-interpolate-flow-hw";
        std::filesystem::create_directories(directory);
        logger.Initialize(directory / ("run-" + std::to_string(GetCurrentProcessId()) + ".log"));
        log_path = logger.ActiveLogPath();
        FrameGenPresenter presenter;
        FrameGenResolvedSettings settings;
        settings.enabled = true;
        settings.synthesis_mode = FrameGenSynthesisMode::kInterpolate;
        settings.motion_estimation = FrameGenMotionEstimation::kHardware;
        settings.interpolate_flow = FrameGenInterpolateFlow::kAuto;
        settings.multiplier = FrameGenMultiplier::kDynamic;
        presenter.Configure(settings);
        presenter.SetEngagementPolicy(3, 0ms);
        FrameGenDynamicPolicy dynamic;
        dynamic.engage_after = dynamic.settle = 200ms;
        dynamic.guard_window = 60s;
        dynamic.probe_after = dynamic.probe_max = 600s;
        presenter.SetDynamicPolicy(dynamic);
        FrameGenInterpolateFlowPolicy flow;
        flow.window = 500ms;
        flow.shader_hold = 1500ms;
        flow.probe = 1000ms;
        flow.hold_max = 6000ms;
        flow.min_generated = 6;
        flow.min_hardware_samples = 3;
        presenter.SetInterpolateFlowPolicy(flow);
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = swapchains.device.Get();
        binding.queue = queue.Get();
        FrameGenRuntimeQueue runtime_binding;
        runtime_binding.next = &binding;
        runtime_binding.d3d12_queue = runtime_queue.Get();
        presenter.Start(kSession, &runtime_binding, {Formats, Create, Destroy, Images, Acquire, Wait, Release}, &logger, nullptr);
        Require(presenter.Started(), "real D3D12 presenter starts");
        constexpr XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        presenter.TrackSwapchain(kLeft, kFormat, 1, usage);
        presenter.TrackSwapchain(kRight, kFormat, 1, usage);
        std::array<XrCompositionLayerProjectionView, 2> views{};
        for (UINT eye = 0; eye < 2; ++eye) {
            views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            views[eye].pose.orientation.w = 1.0f;
            views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
            views[eye].subImage = {eye ? kRight : kLeft, {{0, 0}, {static_cast<int>(kWidth), static_cast<int>(kHeight)}}, 0};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = kSpace;
        projection.viewCount = 2;
        projection.views = views.data();
        const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
        const auto started = Clock::now();
        std::uint64_t frames = 0, last_switches = ~std::uint64_t{0};
        std::optional<Clock::time_point> healthy_since;
        std::cout << "Accelerated test policy: window500ms, shader hold1500ms, probe1000ms; CPU vector acceptance delayed100ms for3500ms only\n";
        while (Clock::now() - started < 20s) {
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState state{XR_TYPE_FRAME_STATE};
            Require(XR_SUCCEEDED(presenter.WaitFrame(kSession, &wait, &state, calls)), "application wait");
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            Require(XR_SUCCEEDED(presenter.BeginFrame(kSession, &begin, calls)), "application begin");
            // CPU application rendering cadence, about 45 fps against a 90 Hz runtime.
            std::this_thread::sleep_for(22ms);
            const UINT image = static_cast<UINT>(++frames % 3);
            for (XrSwapchain chain : {kLeft, kRight}) {
                presenter.NoteRelease(chain, image);
                presenter.CaptureOnRelease(chain, image);
            }
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = state.predictedDisplayTime;
            end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            end.layerCount = 1;
            end.layers = layers;
            Require(XR_SUCCEEDED(presenter.EndFrame(kSession, &end, calls)), "application end");
            final = presenter.Status();
            saw_shader = saw_shader || (final.interpolate_flow_active == "shader" && final.interpolate_flow_fallbacks > 0);
            saw_probe = saw_probe || final.interpolate_flow_probes > 0;
            if (final.interpolate_flow_switches != last_switches) {
                std::cout << "t=" << std::chrono::duration<double>(Clock::now() - started).count()
                          << " active=" << final.interpolate_flow_active << " reason=" << final.interpolate_flow_reason
                          << " switches=" << final.interpolate_flow_switches << " probes=" << final.interpolate_flow_probes
                          << " fallbacks=" << final.interpolate_flow_fallbacks << std::endl;
                last_switches = final.interpolate_flow_switches;
            }
            if (saw_shader && saw_probe && final.interpolate_flow_active == "hardware" &&
                !final.interpolate_flow_probing &&
                final.interpolate_flow_reason.find("recovered") != std::string::npos && Clock::now() - started > 6s) {
                if (!healthy_since) healthy_since = Clock::now();
                if (Clock::now() - *healthy_since >= 1500ms) { recovered = true; break; }
            } else {
                healthy_since.reset();
            }
        }
        presenter.Stop(); // ordinary completion/drain before callbacks/textures/device go away
    }
    std::ifstream log(log_path);
    const std::string contents((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    std::cout << "Presenter log: " << log_path.string() << '\n';
    const bool actual_hardware = contents.find("measures motion with NVIDIA's optical flow hardware") != std::string::npos;
    Require(actual_hardware, "the real backend must report NVIDIA optical flow active (shader emulation does not satisfy this demonstration)");
    Require(saw_shader && final.interpolate_flow_fallbacks > 0, "acceptance delay must trigger automatic shader fallback");
    Require(saw_probe && recovered, "after finite delay expires, a hardware probe must recover and remain on hardware");
    Require(final.threaded && final.dynamic && final.generated > 0, "Dynamic Interpolate must actually generate on the real backend");
    runtime.PrintDiagnostics();
    swapchains.PrintDiagnostics();
    Require(!runtime.ordering_error && !swapchains.ordering_error, "runtime and output image calls remain ordered through switches");
    std::cout << "PASS: real NVIDIA flow -> automatic shader fallback -> hardware probe -> stable recovery; generated="
              << final.generated << ", switches=" << final.interpolate_flow_switches << ", probes=" << final.interpolate_flow_probes
              << ", fallbacks=" << final.interpolate_flow_fallbacks << '\n';
    return 0;
}
} // namespace

int main() {
    try { return Run(); }
    catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
