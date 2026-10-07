#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_runtime_queue.h"

#include "depthxr/framegen_gpu.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace depthxr {

using Microsoft::WRL::ComPtr;

namespace {

// A held-back wait reaches the runtime once the release before it has. The
// runtime shows none of the swapchain's images by then, so it returns at
// once; bounded all the same, retried at the next call when it is not.
constexpr XrDuration kHeldBackWaitTimeout = 1'000'000'000;
constexpr DWORD kReleaseWaitMs = 100;

} // namespace

struct FrameGenQueueSubstitution::Impl {
    ComPtr<ID3D12CommandQueue> game;    // the application's queue
    ComPtr<ID3D12CommandQueue> runtime; // the queue the runtime was given
    // Game work before a release, for the runtime's queue; the runtime's work
    // after a wait, for the game's.
    ComPtr<ID3D12Fence> released;
    ComPtr<ID3D12Fence> handed_back;
    std::mutex mutex; // the swapchain map, the fence values (in queue order) and the log
    UINT64 released_value{0};
    UINT64 handed_back_value{0};
    std::function<void(const std::string&)> log;
    int failures{0};

    // An application call held back: a release (after the game's work up
    // to `value` on the `released` fence) or a wait.
    struct Call {
        bool wait{false};
        UINT64 value{0};
    };
    // Per application swapchain. `calls` keeps the runtime's calls on it in
    // order and guards the rest; taken before `mutex`.
    struct Swapchain {
        std::mutex calls;
        std::deque<Call> held_back;
        std::uint32_t releases{0};     // releases in `held_back`
        std::uint32_t held{0};         // acquired by the application, not released by it
        std::uint32_t image_count{0};  // as acquiring last said
        std::uint32_t direct_waits{0}; // waits not held back: an image was shown since
        HANDLE event{nullptr};
        ~Swapchain() {
            if (event) CloseHandle(event);
        }
    };
    std::unordered_map<XrSwapchain, std::shared_ptr<Swapchain>> swapchains;

    std::shared_ptr<Swapchain> Find(XrSwapchain swapchain, bool create) {
        std::scoped_lock lock(mutex);
        const auto it = swapchains.find(swapchain);
        if (it != swapchains.end()) {
            return it->second;
        }
        return create ? (swapchains[swapchain] = std::make_shared<Swapchain>()) : nullptr;
    }

    // Marks the game's work so far on its queue.
    UINT64 MarkGameWork() {
        std::scoped_lock lock(mutex);
        const UINT64 value = ++released_value;
        game->Signal(released.Get(), value);
        return value;
    }

    void Note(XrResult result, const char* call) {
        if (XR_SUCCEEDED(result)) {
            return;
        }
        std::function<void(const std::string&)> report;
        {
            std::scoped_lock lock(mutex);
            if (++failures > 5 || !log) return;
            report = log;
        }
        report(std::string("Frame Generation: the runtime refused a held-back ") + call + " of a game image (" +
               std::to_string(result) + ").");
    }

    enum class Until {
        kDone,     // releases whose work has run (and the waits after them)
        kReleases, // every release (the runtime's queue waiting for its work); later waits stay
        kAll,      // everything
        kOldest,   // through the oldest release, the runtime's queue waiting for its work
    };
    // Forwards held-back calls from the oldest (holding `state.calls`).
    // False when a wait the runtime did not return in time stopped it.
    bool Forward(XrSwapchain swapchain, Swapchain& state, const Calls& next, Until until) {
        const UINT64 done = released->GetCompletedValue();
        while (!state.held_back.empty()) {
            const Call call = state.held_back.front();
            if (call.wait) {
                if (until == Until::kReleases && state.releases == 0) break;
                XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                info.timeout = kHeldBackWaitTimeout;
                const XrResult result = next.wait(swapchain, &info);
                if (result == XR_TIMEOUT_EXPIRED) {
                    Note(XR_ERROR_RUNTIME_FAILURE, "wait (timed out; retried)");
                    return false;
                }
                Note(result, "wait");
                state.held_back.pop_front();
                continue;
            }
            if (call.value > done) {
                if (until == Until::kDone) break;
                runtime->Wait(released.Get(), call.value);
            }
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            Note(next.release(swapchain, &info), "release");
            state.held_back.pop_front();
            --state.releases;
            if (until == Until::kOldest) break;
        }
        return true;
    }

    // After the runtime handed an image back: anything it queued for the
    // image runs before the game's rendering into it.
    void AfterWait() {
        std::scoped_lock lock(mutex);
        const UINT64 value = ++handed_back_value;
        runtime->Signal(handed_back.Get(), value);
        game->Wait(handed_back.Get(), value);
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    FrameGenRuntimeQueue marker;
};

FrameGenQueueSubstitution::FrameGenQueueSubstitution() = default;
FrameGenQueueSubstitution::~FrameGenQueueSubstitution() {
    // Process teardown: the device may be gone, so nothing is waited for.
    if (impl_) {
        impl_->runtime.Detach();
        impl_->game.Detach();
        impl_->released.Detach();
        impl_->handed_back.Detach();
    }
}

const XrSessionCreateInfo* FrameGenQueueSubstitution::Prepare(const XrSessionCreateInfo* info, std::string& note) {
    Reset();
    note.clear();
    if (!info || !info->next) {
        return info;
    }
    const auto* first = static_cast<const XrBaseInStructure*>(info->next);
    bool d3d12 = false;
    for (auto* base = first; base; base = base->next) {
        d3d12 = d3d12 || base->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
    }
    if (!d3d12) {
        return info;
    }
    if (first->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        note = "Frame Generation keeps the game's Direct3D 12 queue for the runtime: its graphics binding is not the "
               "first structure of the session's create info.";
        return info;
    }
    if (const char* queue = std::getenv("VECTORXR_FRAMEGEN_QUEUE"); queue && std::string(queue) == "game") {
        note = "Frame Generation keeps the game's Direct3D 12 queue for the runtime (VECTORXR_FRAMEGEN_QUEUE=game).";
        return info;
    }
    const auto& binding = *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
    if (!binding.device || !binding.queue || binding.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        return info;
    }
    auto impl = std::make_unique<Impl>();
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                                  D3D12_COMMAND_QUEUE_FLAG_NONE, binding.queue->GetDesc().NodeMask};
    if (FAILED(binding.device->CreateCommandQueue(&desc, IID_PPV_ARGS(&impl->runtime))) ||
        FAILED(binding.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->released))) ||
        FAILED(binding.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->handed_back)))) {
        note = "Frame Generation keeps the game's Direct3D 12 queue for the runtime: a queue of its own could not "
               "be created.";
        return info;
    }
    impl->runtime->SetName(L"VectorXR Frame Generation");
    impl->game = binding.queue;
    impl->binding = binding;
    impl->binding.queue = impl->runtime.Get();
    impl->info = *info;
    impl->info.next = &impl->binding;
    impl->marker.d3d12_queue = impl->runtime.Get();
    impl->log = log_;
    impl_ = std::move(impl);
    note = "Frame Generation gives the runtime a Direct3D 12 queue of its own: frames it writes reach the runtime "
           "as soon as they are written, not after the game's queued work; the game's images are kept in order "
           "across the two queues.";
    return &impl_->info;
}

bool FrameGenQueueSubstitution::Active() const {
    return impl_ != nullptr;
}

void FrameGenQueueSubstitution::SetLog(std::function<void(const std::string&)> log) {
    log_ = std::move(log);
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->log = log_;
    }
}

const void* FrameGenQueueSubstitution::FrameGenChain(const void* game_chain) {
    if (!impl_) {
        return game_chain;
    }
    impl_->marker.next = game_chain;
    return &impl_->marker;
}

const void* FrameGenQueueSubstitution::RuntimeChain() const {
    return impl_ ? static_cast<const void*>(&impl_->binding) : nullptr;
}

XrResult FrameGenQueueSubstitution::Acquire(XrSwapchain swapchain, std::uint32_t image_count, const Calls& next,
                                            const XrSwapchainImageAcquireInfo* info, std::uint32_t* index) {
    if (!impl_) {
        return next.acquire(swapchain, info, index);
    }
    const auto state = impl_->Find(swapchain, true);
    std::scoped_lock calls(state->calls);
    if (image_count) {
        state->image_count = image_count;
    }
    impl_->Forward(swapchain, *state, next, Impl::Until::kDone);
    // The runtime hands out an image only while the application (to its
    // knowledge) holds fewer than all of them.
    for (int forced = 0; state->releases && image_count && state->held + state->releases >= image_count &&
                         forced < static_cast<int>(image_count);
         ++forced) {
        const auto oldest = std::find_if(state->held_back.begin(), state->held_back.end(),
                                         [](const Impl::Call& call) { return !call.wait; });
        if (!state->event) state->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (state->event && SUCCEEDED(impl_->released->SetEventOnCompletion(oldest->value, state->event))) {
            WaitForSingleObject(state->event, kReleaseWaitMs);
        }
        // Still running after that: forwarded with the runtime's queue
        // waiting for it instead.
        if (!impl_->Forward(swapchain, *state, next, Impl::Until::kOldest)) {
            break;
        }
        impl_->Forward(swapchain, *state, next, Impl::Until::kDone);
    }
    const XrResult result = next.acquire(swapchain, info, index);
    if (XR_SUCCEEDED(result)) {
        ++state->held;
    }
    return result;
}

XrResult FrameGenQueueSubstitution::Wait(XrSwapchain swapchain, bool lazy, const Calls& next,
                                         const XrSwapchainImageWaitInfo* info) {
    if (!impl_) {
        return next.wait(swapchain, info);
    }
    if (const auto state = impl_->Find(swapchain, false)) {
        std::unique_lock calls(state->calls);
        impl_->Forward(swapchain, *state, next, Impl::Until::kDone);
        if (!state->held_back.empty() && lazy && !state->direct_waits) {
            // A release before it is still held back: so is this wait.
            state->held_back.push_back({true, 0});
            calls.unlock();
            impl_->AfterWait();
            return XR_SUCCESS;
        }
        if (!impl_->Forward(swapchain, *state, next, Impl::Until::kAll)) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        if (state->direct_waits) {
            --state->direct_waits;
        }
        // Nothing is held back now, and only this thread holds calls back on
        // the swapchain: the wait (which may block) goes without `calls`.
    }
    const XrResult result = next.wait(swapchain, info);
    if (XR_SUCCEEDED(result) && result != XR_TIMEOUT_EXPIRED) {
        impl_->AfterWait();
    }
    return result;
}

XrResult FrameGenQueueSubstitution::Release(XrSwapchain swapchain, bool lazy, const Calls& next,
                                            const XrSwapchainImageReleaseInfo* info) {
    if (!impl_) {
        return next.release(swapchain, info);
    }
    const auto state = impl_->Find(swapchain, true);
    std::scoped_lock calls(state->calls);
    state->held = state->held ? state->held - 1 : 0;
    const UINT64 value = impl_->MarkGameWork();
    if (lazy) {
        state->held_back.push_back({false, value});
        ++state->releases;
        impl_->Forward(swapchain, *state, next, Impl::Until::kDone);
        return XR_SUCCESS;
    }
    if (!impl_->Forward(swapchain, *state, next, Impl::Until::kAll)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    impl_->runtime->Wait(impl_->released.Get(), value);
    return next.release(swapchain, info);
}

void FrameGenQueueSubstitution::Flush(XrSwapchain swapchain, const Calls& next) {
    if (!impl_) {
        return;
    }
    if (const auto state = impl_->Find(swapchain, false)) {
        std::scoped_lock calls(state->calls);
        impl_->Forward(swapchain, *state, next, Impl::Until::kReleases);
        state->direct_waits = state->image_count ? state->image_count : 3;
    }
}

void FrameGenQueueSubstitution::Forget(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->swapchains.erase(swapchain);
}

void FrameGenQueueSubstitution::BeforeRelease(XrSwapchain swapchain, const Calls& next) {
    if (!impl_) {
        return;
    }
    if (const auto state = impl_->Find(swapchain, false)) {
        std::scoped_lock calls(state->calls);
        impl_->Forward(swapchain, *state, next, Impl::Until::kAll);
        state->held = state->held ? state->held - 1 : 0;
    }
    impl_->runtime->Wait(impl_->released.Get(), impl_->MarkGameWork());
}

void FrameGenQueueSubstitution::Reset() {
    if (!impl_) {
        return;
    }
    // Everything queued on the runtime's queue finishes before it goes.
    ComPtr<ID3D12Fence> done;
    ComPtr<ID3D12Device> device;
    if (SUCCEEDED(impl_->runtime->GetDevice(IID_PPV_ARGS(&device))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done))) &&
        SUCCEEDED(impl_->runtime->Signal(done.Get(), 1))) {
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event && SUCCEEDED(done->SetEventOnCompletion(1, event))) {
            WaitForSingleObject(event, 2000);
        }
        if (event) CloseHandle(event);
    }
    if (!done || done->GetCompletedValue() < 1) {
        // Work still queued may use them: kept for the process.
        impl_->runtime.Detach();
        impl_->released.Detach();
        impl_->handed_back.Detach();
    }
    impl_.reset();
}

} // namespace depthxr
