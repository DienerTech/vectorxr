#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_runtime_queue.h"

#include "depthxr/framegen_gpu.h"

#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace depthxr {

using Microsoft::WRL::ComPtr;

struct FrameGenQueueSubstitution::Impl {
    ComPtr<ID3D12CommandQueue> game;    // the application's queue
    ComPtr<ID3D12CommandQueue> runtime; // the queue the runtime was given
    // Game work before a release, for the runtime's queue; the runtime's work
    // after a wait, for the game's.
    ComPtr<ID3D12Fence> released;
    ComPtr<ID3D12Fence> handed_back;
    std::mutex mutex; // fence values only move forward; the swapchains below
    UINT64 released_value{0};
    UINT64 handed_back_value{0};
    // Per application swapchain: lazy releases not forwarded yet (the
    // `released` value marking the game's work on each image, oldest first),
    // and images the application holds (acquired, not released).
    struct Swapchain {
        std::deque<UINT64> pending;
        std::uint32_t held{0};
    };
    std::unordered_map<XrSwapchain, Swapchain> swapchains;
    HANDLE event{nullptr};

    ~Impl() {
        if (event) CloseHandle(event);
    }

    // Forwards pending releases from the oldest: those whose work has run
    // (`all`: every one, the runtime's queue waiting for the rest).
    void Forward(XrSwapchain swapchain, Swapchain& state, PFN_xrReleaseSwapchainImage next, bool all) {
        const UINT64 done = released->GetCompletedValue();
        while (!state.pending.empty() && (all || state.pending.front() <= done)) {
            if (state.pending.front() > done) {
                runtime->Wait(released.Get(), state.pending.front());
            }
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next(swapchain, &info);
            state.pending.pop_front();
        }
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
    impl_ = std::move(impl);
    note = "Frame Generation gives the runtime a Direct3D 12 queue of its own: frames it writes reach the runtime "
           "as soon as they are written, not after the game's queued work; the game's images are kept in order "
           "across the two queues.";
    return &impl_->info;
}

bool FrameGenQueueSubstitution::Active() const {
    return impl_ != nullptr;
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

XrResult FrameGenQueueSubstitution::Release(XrSwapchain swapchain, bool lazy, PFN_xrReleaseSwapchainImage next,
                                            const XrSwapchainImageReleaseInfo* info) {
    if (!impl_) {
        return next(swapchain, info);
    }
    std::scoped_lock lock(impl_->mutex);
    Impl::Swapchain& state = impl_->swapchains[swapchain];
    state.held = state.held ? state.held - 1 : 0;
    const UINT64 value = ++impl_->released_value;
    impl_->game->Signal(impl_->released.Get(), value);
    if (lazy) {
        state.pending.push_back(value);
        impl_->Forward(swapchain, state, next, false);
        return XR_SUCCESS;
    }
    impl_->Forward(swapchain, state, next, true);
    impl_->runtime->Wait(impl_->released.Get(), value);
    return next(swapchain, info);
}

void FrameGenQueueSubstitution::BeforeAcquire(XrSwapchain swapchain, std::uint32_t image_count,
                                              PFN_xrReleaseSwapchainImage next) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->swapchains.find(swapchain);
    if (it == impl_->swapchains.end()) {
        return;
    }
    Impl::Swapchain& state = it->second;
    impl_->Forward(swapchain, state, next, false);
    // The runtime hands out an image only while the application (to its
    // knowledge) holds fewer than all of them.
    while (!state.pending.empty() && image_count && state.held + state.pending.size() >= image_count) {
        if (!impl_->event) impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (impl_->event && SUCCEEDED(impl_->released->SetEventOnCompletion(state.pending.front(), impl_->event))) {
            WaitForSingleObject(impl_->event, 100);
        }
        const UINT64 oldest = state.pending.front();
        impl_->Forward(swapchain, state, next, false);
        if (!state.pending.empty() && state.pending.front() == oldest) {
            // Still running after 100 ms: forwarded with the runtime's queue
            // waiting for it instead.
            impl_->runtime->Wait(impl_->released.Get(), oldest);
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            next(swapchain, &info);
            state.pending.pop_front();
        }
    }
}

void FrameGenQueueSubstitution::AfterAcquire(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    ++impl_->swapchains[swapchain].held;
}

void FrameGenQueueSubstitution::Flush(XrSwapchain swapchain, PFN_xrReleaseSwapchainImage next) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->swapchains.find(swapchain);
    if (it != impl_->swapchains.end()) {
        impl_->Forward(swapchain, it->second, next, true);
    }
}

void FrameGenQueueSubstitution::Forget(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->swapchains.erase(swapchain);
}

void FrameGenQueueSubstitution::BeforeRelease() {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    const UINT64 value = ++impl_->released_value;
    impl_->game->Signal(impl_->released.Get(), value);
    impl_->runtime->Wait(impl_->released.Get(), value);
}

void FrameGenQueueSubstitution::AfterWait() {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    const UINT64 value = ++impl_->handed_back_value;
    impl_->runtime->Signal(impl_->handed_back.Get(), value);
    impl_->game->Wait(impl_->handed_back.Get(), value);
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
