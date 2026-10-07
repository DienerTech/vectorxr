#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_runtime_queue.h"

#include "depthxr/framegen_gpu.h"

#include <cstdlib>
#include <mutex>

namespace depthxr {

using Microsoft::WRL::ComPtr;

struct FrameGenQueueSubstitution::Impl {
    ComPtr<ID3D12CommandQueue> game;    // the application's queue
    ComPtr<ID3D12CommandQueue> runtime; // the queue the runtime was given
    // Game work before a release, for the runtime's queue; the runtime's work
    // after a wait, for the game's.
    ComPtr<ID3D12Fence> released;
    ComPtr<ID3D12Fence> handed_back;
    std::mutex mutex; // fence values only move forward
    UINT64 released_value{0};
    UINT64 handed_back_value{0};
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
