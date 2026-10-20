// Queue substitution against a strict OpenXR swapchain runtime and real
// Direct3D 12 queue fences. No frame-generation backend, game, headset or
// compositor is needed. A CPU-controlled fence stands for unfinished game
// work, so dashboard/application pauses cannot be confused with GPU work
// still running. WARP by default; `hw` uses FG_TEST_ADAPTER or the first GPU.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_runtime_queue.h"
#include "test_adapter.h"
#include "test_gates.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using depthxr::FrameGenQueueSubstitution;
using Microsoft::WRL::ComPtr;

namespace {

void Check(bool ok, const std::string& why) {
    if (ok) return;
    std::cerr << "FAILED: " << why << '\n';
    test_gates::OpenAllBeforeExit();
    std::exit(1);
}

class Environment {
  public:
    Environment(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) old_ = old;
        _putenv_s(name, value);
    }
    ~Environment() { _putenv_s(name_.c_str(), old_ ? old_->c_str() : ""); }
  private:
    std::string name_;
    std::optional<std::string> old_;
};

// An event is only a wake-up: an old timed-out marker completing later must
// not answer a newer marker. Every check reads that marker's fence value.
class Marker {
  public:
    explicit Marker(ID3D12Device* device) {
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))), "marker fence");
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(event_ != nullptr, "marker event");
    }
    ~Marker() { CloseHandle(event_); }
    bool Reaches(ID3D12CommandQueue* queue, DWORD milliseconds = 5000) {
        const UINT64 value = ++value_;
        Check(SUCCEEDED(queue->Signal(fence_.Get(), value)), "mark the queue");
        Check(SUCCEEDED(fence_->SetEventOnCompletion(value, event_)), "wait for the queue marker");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (fence_->GetCompletedValue() < value) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            WaitForSingleObject(event_, static_cast<DWORD>(left.count()));
        }
        return true;
    }
  private:
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_{nullptr};
    UINT64 value_{0};
};

struct Swapchain {
    std::uint32_t images{3};
    std::deque<std::uint32_t> acquired;
    bool waited{false};
    std::uint32_t next{0};
    std::optional<std::uint32_t> released;
    std::uint32_t releases{0};
    std::string calls;
};

class Runtime {
  public:
    std::map<XrSwapchain, Swapchain> chains;
    std::mutex mutex;
    HANDLE release_entered{nullptr};
    HANDLE release_resume{nullptr};

    XrResult Acquire(XrSwapchain handle, std::uint32_t* index) {
        std::scoped_lock lock(mutex);
        Swapchain& chain = chains.at(handle);
        Check(chain.acquired.size() < chain.images, "runtime acquire has an image available");
        const std::uint32_t image = chain.next++ % chain.images;
        chain.acquired.push_back(image);
        if (index) *index = image;
        chain.calls += 'a';
        return XR_SUCCESS;
    }
    XrResult Wait(XrSwapchain handle) {
        std::scoped_lock lock(mutex);
        Swapchain& chain = chains.at(handle);
        Check(!chain.acquired.empty() && !chain.waited, "runtime waits exactly once for its oldest acquired image");
        chain.waited = true;
        chain.calls += 'w';
        return XR_SUCCESS;
    }
    XrResult Release(XrSwapchain handle) {
        std::scoped_lock lock(mutex);
        Swapchain& chain = chains.at(handle);
        Check(!chain.acquired.empty() && chain.waited, "runtime releases its oldest waited image");
        if (release_entered) {
            SetEvent(release_entered);
            Check(WaitForSingleObject(release_resume, 5000) == WAIT_OBJECT_0, "resume the held runtime call");
        }
        chain.released = chain.acquired.front();
        chain.acquired.pop_front();
        chain.waited = false;
        ++chain.releases;
        chain.calls += 'r';
        return XR_SUCCESS;
    }
    void Show(std::initializer_list<XrSwapchain> handles) {
        std::scoped_lock lock(mutex);
        for (XrSwapchain handle : handles) {
            Check(chains.at(handle).released.has_value(), "a submitted game layer has a runtime release");
        }
    }
};

Runtime* g_runtime{nullptr};
XrResult XRAPI_PTR Acquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    return g_runtime->Acquire(swapchain, index);
}
XrResult XRAPI_PTR Wait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
    return g_runtime->Wait(swapchain);
}
XrResult XRAPI_PTR Release(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
    return g_runtime->Release(swapchain);
}
constexpr FrameGenQueueSubstitution::Calls kCalls{Acquire, Wait, Release};
const XrSwapchain kSource = reinterpret_cast<XrSwapchain>(std::uintptr_t{0xc0});
const XrSwapchain kUi = reinterpret_cast<XrSwapchain>(std::uintptr_t{0x51});

class Harness {
  public:
    explicit Harness(ID3D12Device* device) : game_marker(device), runtime_marker(device) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&game))), "game command queue");
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))), "game gate fence");
        gate_guard = std::make_unique<test_gates::Guard>(gate.Get());
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = device;
        binding.queue = game.Get();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
        info.next = &binding;
        std::string note;
        const auto* prepared = substitution.Prepare(&info, note, true);
        Check(prepared != &info && substitution.Active() && substitution.ShownLayersLazy(), "prepare queue substitution: " + note);
        runtime_queue = static_cast<const XrGraphicsBindingD3D12KHR*>(prepared->next)->queue;
        runtime.chains.emplace(kSource, Swapchain{});
        runtime.chains.emplace(kUi, Swapchain{});
        g_runtime = &runtime;
        substitution.SetLog([&](const std::string& text) { errors.push_back(text); });
    }
    ~Harness() {
        gate->Signal(test_gates::kOpenValue);
        Check(game_marker.Reaches(game.Get()) && runtime_marker.Reaches(runtime_queue), "queues finish at teardown");
        Check(errors.empty(), errors.empty() ? "no runtime errors" : errors.front());
        substitution.Reset();
        g_runtime = nullptr;
    }
    void Frame(XrSwapchain handle, bool lazy) {
        std::uint32_t image = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, nullptr, XR_INFINITE_DURATION};
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        Check(substitution.Acquire(handle, 3, kCalls, &acquire, &image) == XR_SUCCESS, "application acquire");
        Check(substitution.Wait(handle, lazy, kCalls, &wait) == XR_SUCCESS, "application wait");
        Check(substitution.Release(handle, lazy, kCalls, &release) == XR_SUCCESS, "application release");
    }
    // Mirrors SubmitRuntimeFrame: flush the swapchains actually named in
    // the submitted layers, immediately before the runtime sees the frame.
    void Submit(std::initializer_list<XrSwapchain> shown, bool shown_lazy) {
        for (XrSwapchain handle : shown) substitution.Flush(handle, kCalls, shown_lazy);
        runtime.Show(shown);
    }
    void HoldGame(UINT64 value) { Check(SUCCEEDED(game->Wait(gate.Get(), value)), "hold the game's GPU work"); }
    void FinishGame(UINT64 value) {
        Check(SUCCEEDED(gate->Signal(value)), "finish the game's GPU work");
        Check(game_marker.Reaches(game.Get()), "the game's queued work finishes");
    }
    std::string Chain(XrSwapchain handle) const {
        const std::string snapshot = substitution.Describe();
        const std::string prefix = "[swapchain=" + std::to_string(reinterpret_cast<std::uintptr_t>(handle)) + " ";
        const auto begin = snapshot.find(prefix);
        Check(begin != std::string::npos, "snapshot identifies the swapchain: " + snapshot);
        return snapshot.substr(begin, snapshot.find(']', begin) - begin + 1);
    }
    FrameGenQueueSubstitution substitution;
    Runtime runtime;
    ComPtr<ID3D12CommandQueue> game;
    ID3D12CommandQueue* runtime_queue{nullptr};
    ComPtr<ID3D12Fence> gate;
    std::unique_ptr<test_gates::Guard> gate_guard;
    Marker game_marker;
    Marker runtime_marker;
    std::vector<std::string> errors;
};

void StoppedSourceTest(ID3D12Device* device) {
    Harness h(device);
    h.HoldGame(1);
    h.Frame(kSource, true);
    h.Frame(kSource, true);
    Check(h.Chain(kSource).find("heldWaits=1 heldReleases=2") != std::string::npos,
          "the source wait stays behind its earlier held release");
    Check(h.runtime_marker.Reaches(h.runtime_queue), "generated-only output does not wait for an unshown source");
    h.FinishGame(1);
    // The application is now stopped. Generated frames name only outputs
    // of the backend, so none of the original source is submitted.
    for (int i = 0; i < 8; ++i) {
        h.Submit({}, true);
        Check(h.runtime_marker.Reaches(h.runtime_queue), "generated-only refresh advances while the application is stopped");
    }
    Check(h.runtime.chains.at(kSource).releases == 0 &&
              h.Chain(kSource).find("heldWaits=1 heldReleases=2") != std::string::npos,
          "a completed held source alone does not prove a compositor queue stall");
    h.Submit({kSource}, false);
    const Swapchain& source = h.runtime.chains.at(kSource);
    Check(source.calls == "awarwr" && source.releases == 2 && source.acquired.empty() && !source.waited,
          "showing the original source drains releases and the wait between them in order");
    Check(h.runtime_marker.Reaches(h.runtime_queue), "the first pass-through source refresh reaches the compositor");
    std::cout << "Stopped producer, captured source, and pass-through drain passed\n";
}

void StoppedShownLayerTest(ID3D12Device* device) {
    Harness h(device);
    h.Frame(kUi, false);
    h.Submit({kUi}, true);
    Check(h.runtime_marker.Reaches(h.runtime_queue), "prime a valid shown UI image");
    h.HoldGame(1);
    h.Frame(kSource, true);
    h.Frame(kUi, false);
    for (int i = 0; i < 8; ++i) {
        h.Submit({kUi}, true);
        Check(h.runtime.chains.at(kUi).releases == 1, "unfinished UI work is not prematurely released");
        Check(h.runtime_marker.Reaches(h.runtime_queue), "old UI plus generated output keeps the runtime queue advancing");
    }
    Check(h.Chain(kUi).find("heldReleases=1") != std::string::npos && h.substitution.ShownLayers().gpu_waits == 0,
          "the latest UI release stays held without adding a runtime GPU wait");
    h.FinishGame(1);
    // No new acquire, wait or release: only the presenter's repeated frame.
    h.Submit({kUi}, true);
    Check(h.runtime.chains.at(kUi).releases == 2 && h.runtime.chains.at(kUi).released == 1 &&
              h.Chain(kUi).find("heldReleases=0") != std::string::npos,
          "the next presenter submission drains finished UI work without another application call");
    Check(h.runtime.chains.at(kSource).releases == 0 && h.Chain(kSource).find("heldReleases=1") != std::string::npos,
          "the same refresh intentionally retains its unshown captured source");
    Check(h.runtime_marker.Reaches(h.runtime_queue), "the refreshed UI reaches the compositor without a game frame");
    std::cout << "Stopped producer, shown UI, and pending versus completed work passed\n";
}

void PassThroughUnderLoadTest(ID3D12Device* device) {
    Harness h(device);
    h.Frame(kUi, false);
    h.Submit({kUi}, true);
    Check(h.runtime_marker.Reaches(h.runtime_queue), "prime the UI before pass-through handover");
    h.HoldGame(1);
    h.Frame(kSource, true);
    h.Frame(kUi, false);
    // A/B generation off still uses the session's substituted queue. The
    // original source and UI must both be released before downstream EndFrame,
    // and the runtime queue must wait for their unfinished game work.
    h.Submit({kSource, kUi}, false);
    Check(h.runtime.chains.at(kSource).releases == 1 && h.runtime.chains.at(kUi).releases == 2,
          "pass-through forwards both the captured source and shown UI");
    Check(!h.runtime_marker.Reaches(h.runtime_queue, 20), "pass-through preserves the game's unfinished GPU dependency");
    Check(h.Chain(kUi).find("shownLazily=0") != std::string::npos, "pass-through ends the UI's lazy release policy");
    h.FinishGame(1);
    Check(h.runtime_marker.Reaches(h.runtime_queue), "the runtime queue resumes when the real dependency completes");
    h.Frame(kSource, false);
    h.Frame(kUi, false);
    h.Submit({kSource, kUi}, false);
    Check(h.runtime_marker.Reaches(h.runtime_queue) && h.game_marker.Reaches(h.game.Get()),
          "resumed pass-through hand-backs create no cross-queue dependency cycle");
    std::cout << "A/B pass-through and unfinished GPU dependency handover passed\n";
}

void FirstShownImageTest(ID3D12Device* device) {
    Harness h(device);
    h.HoldGame(1);
    h.Frame(kUi, true);
    h.Submit({kUi}, true);
    Check(h.runtime.chains.at(kUi).releases == 1 && h.substitution.ShownLayers().gpu_waits == 1,
          "a newly shown layer cannot use an image the runtime has never received");
    Check(!h.runtime_marker.Reaches(h.runtime_queue, 20), "the first shown image preserves its GPU dependency");
    h.FinishGame(1);
    Check(h.runtime_marker.Reaches(h.runtime_queue), "the first shown image's queue resumes after completion");
    std::cout << "First shown image synchronization passed\n";
}

void AcquireExhaustionTest(ID3D12Device* device) {
    Harness h(device);
    h.HoldGame(1);
    for (int i = 0; i < 3; ++i) h.Frame(kSource, true);
    const auto start = std::chrono::steady_clock::now();
    h.Frame(kSource, true);
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "acquire exhausts the CPU wait within its bound");
    Check(h.runtime.chains.at(kSource).calls == "awaarwa", "exhaustion forwards just enough calls in runtime order");
    h.FinishGame(1);
    h.Submit({kSource}, false);
    Check(h.runtime_marker.Reaches(h.runtime_queue) && h.runtime.chains.at(kSource).acquired.empty(),
          "all exhausted-image hand-backs eventually drain without a queue cycle");
    std::cout << "Image exhaustion and deferred wait ordering passed\n";
}

void BusyDiagnosticsTest(ID3D12Device* device) {
    Harness h(device);
    h.runtime.release_entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    h.runtime.release_resume = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(h.runtime.release_entered && h.runtime.release_resume, "runtime stall events");
    std::thread application([&] { h.Frame(kSource, false); });
    Check(WaitForSingleObject(h.runtime.release_entered, 5000) == WAIT_OBJECT_0, "runtime call is held under the swapchain mutex");
    const auto start = std::chrono::steady_clock::now();
    const std::string snapshot = h.substitution.Describe();
    Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250) &&
              snapshot.find("calls=busy") != std::string::npos,
          "diagnostics do not wait for a held downstream call: " + snapshot);
    SetEvent(h.runtime.release_resume);
    application.join();
    CloseHandle(h.runtime.release_entered);
    CloseHandle(h.runtime.release_resume);
    h.runtime.release_entered = h.runtime.release_resume = nullptr;
    Check(h.substitution.Describe().find("released=1/") != std::string::npos &&
              h.Chain(kSource).find("heldReleases=0") != std::string::npos,
          "diagnostics identify the queue and swapchain once the held call returns");
    std::cout << "Nonblocking queue stall diagnostics passed\n";
}

} // namespace

int main(int argc, char** argv) {
    const Environment queue_choice("VECTORXR_FRAMEGEN_QUEUE", "");
    const Environment shown_choice("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "");
    const bool hardware = argc > 1 && std::string(argv[1]) == "hw";
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    } else {
        std::cout << "Direct3D 12 debug layer unavailable; behavioral checks still run\n";
    }
    ComPtr<IDXGIFactory4> factory;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    if (hardware) {
        std::string why;
        adapter = test_adapter::FindDxgiAdapter(static_cast<IDXGIFactory1*>(factory.Get()),
                                              test_adapter::Wanted().value_or(""), why);
        if (!adapter) {
            std::cout << "SKIPPED: " << why << '\n';
            return 77;
        }
    } else {
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    }
    DXGI_ADAPTER_DESC1 description{};
    adapter->GetDesc1(&description);
    std::cout << "Queue harness: " << test_adapter::Utf8(description.Description) << '\n';
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "Direct3D 12 device");
    FrameGenQueueSubstitution inactive;
    Check(inactive.Describe() == "not substituted", "inactive queue diagnostics");
    StoppedSourceTest(device.Get());
    StoppedShownLayerTest(device.Get());
    PassThroughUnderLoadTest(device.Get());
    FirstShownImageTest(device.Get());
    AcquireExhaustionTest(device.Get());
    BusyDiagnosticsTest(device.Get());
    Check(SUCCEEDED(device->GetDeviceRemovedReason()), "queue tests leave the device healthy");
    ComPtr<ID3D12InfoQueue> messages;
    if (SUCCEEDED(device.As(&messages))) {
        const UINT64 count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < count; ++index) {
            SIZE_T bytes = 0;
            messages->GetMessage(index, nullptr, &bytes);
            std::vector<std::uint8_t> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(SUCCEEDED(messages->GetMessage(index, message, &bytes)), "read Direct3D 12 validation message");
            Check(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION && message->Severity != D3D12_MESSAGE_SEVERITY_ERROR,
                  "Direct3D 12 validation: " + std::string(message->pDescription));
        }
    }
    return 0;
}
