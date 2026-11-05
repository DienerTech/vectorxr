#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_d3d11_bridge.h"

#include "bridge_depth_ps.h"
#include "bridge_depth_to_twin.h"
#include "bridge_depth_vs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

// How long retiring waits for the bridge's queue, at teardown and when a
// swapchain is destroyed.
constexpr DWORD kRetireWaitMs = 2000;

std::string Hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Failure(const char* operation, HRESULT hr) {
    return std::string(operation) + " failed (hr=" + Hex(static_cast<std::uint32_t>(hr)) + ")";
}

std::string LuidText(const LUID& luid) {
    return Hex(static_cast<std::uint32_t>(luid.HighPart)) + ":" + Hex(luid.LowPart);
}

bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

std::string Utf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr);
    return result;
}

bool IsDepthFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D16_UNORM:
        return true;
    default:
        return false;
    }
}

// Private depth is typeless, so the game may view it as depth and sample it.
DXGI_FORMAT DepthStorageFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    default:
        return format;
    }
}

// The depth plane of private depth, as the bridge's copy into the twin reads
// it (normalized for D16 and D24).
DXGI_FORMAT DepthReadFormat(DXGI_FORMAT storage) {
    switch (storage) {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// A depth twin's format: D16's depth fits R16_UNORM exactly (half the
// memory of iRacing's 6904x3184 depth images), every other R32_FLOAT.
DXGI_FORMAT DepthTwinFormat(DXGI_FORMAT storage) {
    return storage == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT;
}

// The depth-stencil view format of a runtime's depth image, typed or
// typeless: what the bridge's draw writes it through.
DXGI_FORMAT DepthTargetFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_D16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Twins are typeless, as runtimes' Direct3D 11 images usually are: the game
// may view an image as any member of its format's family (an sRGB
// swapchain's images are often rendered through UNORM views).
DXGI_FORMAT ColorStorageFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
        return DXGI_FORMAT_R32_TYPELESS;
    default:
        // Formats without a family (R11G11B10_FLOAT and the like) are shared as they are.
        return format;
    }
}

// A typed member of `format`'s family, for resolving into its twin.
DXGI_FORMAT ResolveFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return format;
    }
}

bool IsDepthSwapchain(const XrSwapchainCreateInfo& info) {
    return IsDepthFormat(static_cast<DXGI_FORMAT>(info.format)) ||
           (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

bool WaitFence(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms) {
    if (!fence || fence->GetCompletedValue() >= value) {
        return true;
    }
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) {
        return false;
    }
    if (SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
        WaitForSingleObject(event, timeout_ms);
    }
    CloseHandle(event);
    return fence->GetCompletedValue() >= value;
}

// Recordings of an image's copy kept at most: a recording is executed again
// only once its last execution is done (D3D12 forbids executing a command
// list still pending), and each image needs another only while its earlier
// copies are still queued.
constexpr std::size_t kCopyRecordings = 4;
// How long a release waits for a recording when all of them are queued
// still: the bridge's queue is that far behind only when stalled.
constexpr DWORD kRecordingWaitMs = 100;

// A release's handover to the bridge's queue this long or longer counts as
// slow in the summary (slowHandovers).
constexpr double kSlowHandoverMs = 2.0;

std::uint64_t MiB(std::uint64_t bytes) {
    return (bytes + (1u << 19)) >> 20;
}

// Holds the game's immediate context for the bridge's calls on it at a
// release (a depth copy changes its state and puts it back). Multithread
// protection serializes each Direct3D 11 call on its own: without the hold,
// another thread's draw could land between the bridge's calls, under the
// bridge's state, or have its own state overwritten when the game's goes
// back. Entered whether protection is on or not (a thread entering it alone
// does not wait; Direct3D 11 takes the same lock for some calls, Flush among
// them, even without protection, and its debug layer to read whether
// protection is on). It serializes the CPU's calls only, nothing waits for
// the GPU inside it.
//
// Lock order: taken before the bridge's mutex, and, at a hand-over
// (D3D11Bridge::StageReleases), before any lock of the layer's. A game thread
// may hold the device's lock (ID3D11Multithread::Enter) through an OpenXR
// call that takes those: waiting for the context while holding one of them
// would deadlock with it.
class ContextHold {
  public:
    ContextHold(ID3D11Multithread* multithread, const std::function<void()>* before_enter) {
        if (multithread) {
            if (before_enter && *before_enter) (*before_enter)();
            multithread_ = multithread;
            multithread_->Enter();
        }
    }
    ~ContextHold() {
        if (multithread_) multithread_->Leave();
    }
    ContextHold(const ContextHold&) = delete;
    ContextHold& operator=(const ContextHold&) = delete;

  private:
    ID3D11Multithread* multithread_{nullptr};
};

// The session's mean and longest handover, "mean/max" in milliseconds.
std::string HandoverText(double total_ms, std::uint64_t count, double max_ms) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.2f/%.1f", count ? total_ms / static_cast<double>(count) : 0.0, max_ms);
    return buffer;
}

} // namespace

struct D3D11Bridge::Impl {
    enum class Mode {
        kTwin,    // rendered into the twin
        kResolve, // multisampled: rendered privately, resolved into the twin at release
        kDepth,   // depth: rendered privately, copied into a twin at release and drawn into the runtime's
        kPrivate, // depth the bridge cannot mirror: rendered privately, never reaches the runtime's images
    };
    // A recording of the copy of a twin into the runtime's image (for depth,
    // the draw of the twin into it).
    struct Copy {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 done{0}; // the done value after its last execution
    };
    struct Image {
        ComPtr<ID3D12Resource> twin;
        ComPtr<ID3D11Texture2D> twin11;    // the twin, opened on the game's device
        ComPtr<ID3D11Texture2D> private11; // rendered into instead of the twin
        // Depth: the game's depth as read into the twin, and the twin as written.
        ComPtr<ID3D11ShaderResourceView> depth_read11;
        ComPtr<ID3D11UnorderedAccessView> twin_write11;
        D3D12_GPU_DESCRIPTOR_HANDLE twin_read{};                // the twin, read by the draw
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> depth_targets; // the runtime's image, by slice
        // The runtime's image, referenced: a copy still queued when the
        // runtime lets the image go writes memory that still exists.
        ComPtr<ID3D12Resource> runtime;
        std::vector<Copy> copies;
        UINT64 done{0}; // the done value after its last copy
    };
    // A release whose Direct3D 11 half ran ahead of its hand-over
    // (StageReleases): the image, its "written" value and that half's time.
    struct Staged {
        std::uint32_t index{0};
        UINT64 written{0};
        double ms{0.0};
    };
    struct Swapchain {
        Mode mode{Mode::kTwin};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; // as the application asked
        DXGI_FORMAT storage{DXGI_FORMAT_UNKNOWN};
        DXGI_FORMAT depth_target{DXGI_FORMAT_UNKNOWN}; // depth: the runtime images' depth-stencil view format
        std::string private_reason;                    // depth kept private: why
        std::string protection; // mirrored depth: the device's multithread protection (NoteDepthProtection)
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t array_size{1};
        std::uint32_t mip_count{1};
        std::uint32_t sample_count{1};
        bool composed{false};
        // Depth: the twins' views read by the draws (shader-visible, kept
        // while the draws are queued) and the runtime images' depth targets.
        ComPtr<ID3D12DescriptorHeap> twin_reads;
        ComPtr<ID3D12DescriptorHeap> depth_targets;
        std::vector<Image> images;
        std::deque<std::uint32_t> acquired; // acquired and not released yet, oldest first
        std::uint32_t pending{0};  // the application's releases whose hand-over waits (ReleaseCalled)
        std::deque<Staged> staged; // releases whose Direct3D 11 half ran (StageReleases), oldest first
        UINT64 last_done{0};
        std::uint64_t twin_bytes{0};
        std::uint64_t copies{0};
        std::uint64_t done_waits{0}; // acquires whose image's copy had not finished: Direct3D 11 waits for it
        std::uint64_t skipped_copies{0}; // releases not copied: the bridge's queue was stalled
        std::uint64_t unmatched_releases{0}; // releases with nothing acquired
    };

    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Multithread> multithread; // the immediate context's lock (ContextHold)
    bool multithread_protected{false};
    ComPtr<ID3D12Device> device12;
    ComPtr<IDXGIAdapter3> adapter; // for video memory figures
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> written; // signalled by Direct3D 11 only
    ComPtr<ID3D12Fence> done;    // signalled by the bridge's queue only
    ComPtr<ID3D12Fence> idle;    // the bridge's queue's progress, for retiring
    ComPtr<ID3D11Fence> written11;
    ComPtr<ID3D11Fence> done11;
    // Depth reaches the runtime's images unless VECTORXR_D3D11_BRIDGE_DEPTH=private.
    bool mirror_depth{true};

    // The depth passes, made with the first depth swapchain mirrored (each
    // with `pipeline_mutex`; a draw's pipeline only for its target format).
    std::mutex pipeline_mutex;
    ComPtr<ID3D11ComputeShader> depth_to_twin;
    ComPtr<ID3D12RootSignature> depth_root;
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> depth_pipelines;

    // Everything below, and the order of signals on both APIs: each fence's
    // values are signalled in the order they are handed out.
    mutable std::mutex mutex;
    UINT64 written_value{0};
    UINT64 done_value{0};
    UINT64 idle_value{0};
    DWORD thread{0}; // the first thread a swapchain or frame call came from
    bool reported_second_thread{false};
    DWORD second_thread{0}; // the first other thread NoteThread saw
    // A second thread was seen and protection is off: ApplyWantedProtection
    // turns it on (read without `mutex`).
    std::atomic<bool> protection_wanted{false};
    bool debug_layer{false};          // VECTORXR_D3D11_BRIDGE_DEBUG=1
    std::uint32_t debug_reported{0};  // debug-layer messages reported
    std::uint64_t next_skip_report{1}; // skipped copies are reported at 1, 10, 100...
    std::unordered_map<XrSwapchain, Swapchain> swapchains;
    std::vector<Swapchain> retired; // removed while the GPU may still have used them
    std::uint32_t swapchains_added{0};
    std::uint64_t releases{0};
    std::uint64_t copies{0};
    std::uint64_t skipped_copies{0};
    std::atomic<std::uint64_t> recordings{0}; // also counted outside the mutex, as swapchains are made
    std::uint64_t done_waits{0};
    std::uint64_t unmatched_releases{0};
    std::uint64_t depth_copies{0};         // releases of depth copied into a twin
    std::uint64_t depth_target_unbinds{0}; // of them, with the image still the game's depth target
    // The game thread's time issuing those copies (CopyDepthToTwin, its
    // Direct3D 11 calls only; the GPU's work is not timed), milliseconds: the
    // session's total and longest.
    double depth_copy_ms_total{0.0};
    double depth_copy_ms_max{0.0};
    // The game thread's time in a release's handover (a depth image's copy
    // into its twin or a multisampled image's resolve, Direct3D 11's flush, a
    // recording of the copy, the bridge queue's submission), milliseconds:
    // the session's total and longest, and how many took kSlowHandoverMs.
    double handover_ms_total{0.0};
    double handover_ms_max{0.0};
    std::uint64_t handovers{0};
    std::uint64_t slow_handovers{0};
    std::function<void(const std::string&)> log;
    std::function<void()> depth_copy_hook; // SetDepthCopyHookForTest
    // SetContextWaitHookForTest: read without `mutex` (set while nothing runs).
    std::function<void()> context_wait_hook;
    bool depth_protection_reported{false}; // the device has none to turn on: said once

    // Counts a handover that started at `start`, `ahead_ms` of it having run
    // before (its Direct3D 11 half, StageReleases), holding `mutex`.
    void NoteHandover(std::chrono::steady_clock::time_point start, double ahead_ms = 0.0) {
        const double ms =
            ahead_ms + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        handover_ms_total += ms;
        handover_ms_max = std::max(handover_ms_max, ms);
        ++handovers;
        slow_handovers += ms >= kSlowHandoverMs ? 1 : 0;
    }

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};

    // A swapchain or frame call's thread (holding `mutex`): Direct3D 11's
    // immediate context is the game's, and is only safe from another thread
    // when the device is multithread protected. Calls from more than one
    // thread want that protection on: ApplyWantedProtection turns it on,
    // never here. SetMultithreadProtected waits for the device's lock, which
    // a thread holding the context (ContextHold, or a game thread of its
    // own) may hold while it waits for `mutex`, or for the layer's locks
    // these calls are made under.
    void NoteThread() {
        const DWORD current = GetCurrentThreadId();
        if (!thread) {
            thread = current;
            return;
        }
        if (current == thread || reported_second_thread || multithread_protected) {
            return;
        }
        reported_second_thread = true;
        second_thread = current;
        protection_wanted = true;
    }

    // Turns protection on once NoteThread wanted it, holding neither `mutex`
    // nor any lock of the layer's (or holding the context already, which
    // makes the device's lock this thread's): at the start of a hand-over
    // (StageReleases, called before any of the layer's locks), before the
    // bridge's Direct3D 11 calls there. Until then a second thread's calls
    // are not serialized, as the calls before it never were.
    void ApplyWantedProtection() {
        if (!protection_wanted.exchange(false)) {
            return;
        }
        bool on = false;
        if (multithread) {
            multithread->SetMultithreadProtected(TRUE);
            on = multithread->GetMultithreadProtected() != FALSE;
        }
        std::scoped_lock lock(mutex);
        multithread_protected = multithread_protected || on;
        if (log) {
            log("Direct3D 11 bridge: the game's swapchain and frame calls come from more than one thread (" +
                std::to_string(second_thread) + ", first " + std::to_string(thread) + "); " +
                (on ? "turned the game device's multithread protection on, so each of the bridge's Direct3D 11 "
                      "calls is serialized with the game's (calls made before this one were not)."
                    : "its device has no multithread protection to turn on: the bridge's Direct3D 11 calls there "
                      "may race the game's rendering."));
        }
    }

    // Holds the game's immediate context (ContextHold): before `mutex`.
    ContextHold HoldContext() {
        return ContextHold(multithread.Get(), &context_wait_hook);
    }

    // When a depth swapchain is mirrored, before its first copy (holding
    // `mutex`, AddSwapchain having turned protection on, and read it as
    // `now_protected`, before: Direct3D 11's debug layer reads it under the
    // device's lock, which a thread holding the context may hold while it
    // waits for `mutex`): the copy
    // changes the immediate context's state and puts it back over many
    // calls, which only multithread protection keeps the game's other
    // threads out of (ContextHold), and a game rendering on a thread that
    // never calls OpenXR would otherwise never get it (NoteThread sees
    // OpenXR's threads only). Turned on once, never off; a game's own is
    // left as it is. What the chain's description says of it.
    std::string NoteDepthProtection(bool was_protected, bool now_protected) {
        multithread_protected = now_protected;
        if (multithread_protected) {
            return was_protected ? "on" : "turned on";
        }
        if (log && !depth_protection_reported) {
            depth_protection_reported = true;
            log("Direct3D 11 bridge: the game's device has no multithread protection to turn on: its depth copies "
                "may race a thread of the game's rendering at the same time.");
        }
        return "unavailable";
    }

    // The Direct3D 12 debug layer's errors since the last call (holding `mutex`).
    void ReportDebugMessages() {
        ComPtr<ID3D12InfoQueue> queue12;
        if (!debug_layer || !log || FAILED(device12.As(&queue12))) {
            return;
        }
        constexpr std::uint32_t kMaximum = 100;
        for (UINT64 i = 0; i < queue12->GetNumStoredMessages() && debug_reported < kMaximum; ++i) {
            SIZE_T size = 0;
            queue12->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue12->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                ++debug_reported;
                log("Direct3D 11 bridge: the Direct3D 12 debug layer reports [" + std::to_string(message->ID) + "] " +
                    std::string(message->pDescription, strnlen(message->pDescription, size)));
            }
        }
        queue12->ClearStoredMessages();
    }

    // The bridge's queue's progress so far (holding `mutex`).
    UINT64 MarkIdle() {
        const UINT64 value = ++idle_value;
        queue->Signal(idle.Get(), value);
        return value;
    }

    // For the process: work still queued may use them.
    static void DetachGpu(Swapchain& chain) {
        chain.twin_reads.Detach();
        for (Image& image : chain.images) {
            image.twin.Detach();
            image.runtime.Detach();
            for (Copy& copy : image.copies) {
                copy.list.Detach();
                copy.allocator.Detach();
            }
        }
    }
    // ...and the depth draws' pipelines.
    void DetachPipelines() {
        depth_root.Detach();
        for (auto& [format, pipeline] : depth_pipelines) {
            pipeline.Detach();
        }
    }

    std::string VideoMemory() const {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            return "unavailable";
        }
        std::string text = std::to_string(MiB(memory.CurrentUsage)) + "/" + std::to_string(MiB(memory.Budget)) + " MiB";
        // The process's system memory on the adapter: it grows when Windows
        // moves video memory out over the budget (the GPU then waits on it).
        DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
            text += ", sharedMemory=" + std::to_string(MiB(shared.CurrentUsage)) + " MiB";
        }
        return text;
    }

    std::string Describe(const Swapchain& chain) const {
        std::ostringstream stream;
        stream << "mode="
               << (chain.mode == Mode::kTwin      ? "twin"
                   : chain.mode == Mode::kResolve ? "resolve"
                   : chain.mode == Mode::kDepth   ? "depth"
                                                  : "private-depth");
        if (!chain.private_reason.empty()) {
            stream << " (" << chain.private_reason << ")";
        }
        stream << ", images=" << chain.images.size() << ", format=" << static_cast<int>(chain.format)
               << ", storage=" << static_cast<int>(chain.storage) << ", size=" << chain.width << "x" << chain.height
               << ", array=" << chain.array_size << ", mips=" << chain.mip_count << ", samples=" << chain.sample_count
               << ", twinMiB=" << MiB(chain.twin_bytes) << ", composed=" << (chain.composed ? 1 : 0)
               << ", copies=" << chain.copies << ", doneWaits=" << chain.done_waits
               << ", skippedCopies=" << chain.skipped_copies << ", unmatchedReleases=" << chain.unmatched_releases;
        if (!chain.protection.empty()) {
            stream << ", multithreadProtection=" << chain.protection;
        }
        return stream.str();
    }

    // Records the twin's copy into the runtime's image. The runtime's colour
    // images are render targets whenever the game holds them; the twin, with
    // simultaneous access, needs no transition of its own.
    bool RecordCopy(const Swapchain& chain, Image& image, Copy& copy, std::string& reason) {
        HRESULT hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator));
        if (SUCCEEDED(hr)) {
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&copy.list));
        }
        if (FAILED(hr)) {
            reason = Failure("Creating the bridge's copy commands", hr);
            return false;
        }
        if (chain.mode == Mode::kDepth) {
            return RecordDepthDraw(chain, image, copy, reason);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = image.runtime.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copy.list->ResourceBarrier(1, &barrier);
        copy.list->CopyResource(image.runtime.Get(), image.twin.Get());
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        copy.list->ResourceBarrier(1, &barrier);
        hr = copy.list->Close();
        if (FAILED(hr)) {
            reason = Failure("Recording the bridge's copy", hr);
            return false;
        }
        ++recordings;
        return true;
    }

    // A recording of the image's copy whose last execution is done (holding
    // `mutex`): one more is made while fewer than kCopyRecordings exist, and
    // otherwise the oldest is waited for, a while. nullptr: none in time.
    Copy* ReadyCopy(const Swapchain& chain, Image& image) {
        const UINT64 completed = done->GetCompletedValue();
        for (Copy& copy : image.copies) {
            if (copy.done <= completed) {
                return &copy;
            }
        }
        if (image.copies.size() < kCopyRecordings) {
            std::string reason;
            image.copies.emplace_back();
            if (RecordCopy(chain, image, image.copies.back(), reason)) {
                return &image.copies.back();
            }
            image.copies.pop_back();
        }
        auto oldest = std::min_element(image.copies.begin(), image.copies.end(),
                                       [](const Copy& a, const Copy& b) { return a.done < b.done; });
        if (oldest != image.copies.end() && WaitFence(done.Get(), oldest->done, kRecordingWaitMs)) {
            return &*oldest;
        }
        return nullptr;
    }

    bool MakeTwin(Swapchain& chain, Image& image, bool unordered_access, std::string& reason) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = chain.width;
        desc.Height = chain.height;
        desc.DepthOrArraySize = static_cast<UINT16>(chain.array_size);
        desc.MipLevels = static_cast<UINT16>(chain.mode == Mode::kResolve ? 1 : chain.mip_count);
        desc.Format = chain.storage;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Direct3D 11 opens a Direct3D 12 texture only as a render target, and
        // a typeless (or unordered-access) one only with simultaneous access.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (unordered_access) {
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }

        if (!image.runtime) {
            reason = "the runtime's image is null";
            return false;
        }
        const D3D12_RESOURCE_DESC runtime = image.runtime->GetDesc();
        if (runtime.Width != chain.width || runtime.Height != chain.height ||
            runtime.DepthOrArraySize != desc.DepthOrArraySize || runtime.MipLevels != desc.MipLevels ||
            runtime.SampleDesc.Count != 1 || ColorStorageFormat(runtime.Format) != chain.storage) {
            std::ostringstream stream;
            stream << "the runtime's image (" << runtime.Width << "x" << runtime.Height << ", array "
                   << runtime.DepthOrArraySize << ", mips " << runtime.MipLevels << ", samples "
                   << runtime.SampleDesc.Count << ", format " << static_cast<int>(runtime.Format)
                   << ") does not match its twin (" << desc.Width << "x" << desc.Height << ", array "
                   << desc.DepthOrArraySize << ", mips " << desc.MipLevels << ", format "
                   << static_cast<int>(desc.Format) << ")";
            reason = stream.str();
            return false;
        }
        // The copy leaves the runtime's image a render target, as it was handed out.
        if ((runtime.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
            reason = "the runtime's image is not a render target";
            return false;
        }

        HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&image.twin));
        if (FAILED(hr)) {
            reason = Failure("Creating a shared Direct3D 12 texture", hr);
            return false;
        }
        image.twin->SetName(L"VectorXR Direct3D 11 bridge image");
        chain.twin_bytes += device12->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        HANDLE handle = nullptr;
        hr = device12->CreateSharedHandle(image.twin.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            reason = Failure("Sharing a Direct3D 12 texture", hr);
            return false;
        }
        hr = device11->OpenSharedResource1(handle, IID_PPV_ARGS(&image.twin11));
        CloseHandle(handle);
        if (FAILED(hr)) {
            reason = Failure("Opening a shared texture on the game's Direct3D 11 device", hr);
            return false;
        }
        image.copies.emplace_back();
        return RecordCopy(chain, image, image.copies.back(), reason);
    }

    // Records the draw of a depth twin into the runtime's depth image, slice
    // by slice (mip 0). The runtime's depth images are depth targets whenever
    // the game holds them (XR_KHR_D3D12_enable), so the draw needs no
    // transition; the twin, with simultaneous access, neither.
    bool RecordDepthDraw(const Swapchain& chain, Image& image, Copy& copy, std::string& reason) {
        ID3D12PipelineState* pipeline = nullptr;
        {
            std::scoped_lock lock(pipeline_mutex);
            const auto it = depth_pipelines.find(chain.depth_target);
            pipeline = it == depth_pipelines.end() ? nullptr : it->second.Get();
        }
        if (!pipeline || !depth_root) {
            reason = "the bridge's depth draw has no pipeline";
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {chain.twin_reads.Get()};
        copy.list->SetDescriptorHeaps(1, heaps);
        copy.list->SetGraphicsRootSignature(depth_root.Get());
        copy.list->SetPipelineState(pipeline);
        copy.list->SetGraphicsRootDescriptorTable(1, image.twin_read);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(chain.width), static_cast<float>(chain.height),
                                      0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(chain.width), static_cast<LONG>(chain.height)};
        copy.list->RSSetViewports(1, &viewport);
        copy.list->RSSetScissorRects(1, &scissor);
        copy.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (std::uint32_t slice = 0; slice < chain.array_size; ++slice) {
            copy.list->SetGraphicsRoot32BitConstant(0, slice, 0);
            copy.list->OMSetRenderTargets(0, nullptr, FALSE, &image.depth_targets[slice]);
            copy.list->DrawInstanced(3, 1, 0, 0);
        }
        const HRESULT hr = copy.list->Close();
        if (FAILED(hr)) {
            reason = Failure("Recording the bridge's depth draw", hr);
            return false;
        }
        ++recordings;
        return true;
    }

    // The depth passes: the Direct3D 11 copy into twins (once), the draw's
    // root signature (once) and its pipeline for `target`.
    bool MakeDepthPasses(DXGI_FORMAT target, std::string& reason) {
        std::scoped_lock lock(pipeline_mutex);
        if (!depth_to_twin) {
            const HRESULT hr = device11->CreateComputeShader(kBridgeDepthToTwinDxbc, sizeof(kBridgeDepthToTwinDxbc),
                                                             nullptr, &depth_to_twin);
            if (FAILED(hr)) {
                reason = Failure("Creating the Direct3D 11 depth copy (the game's device needs feature level 11)", hr);
                return false;
            }
        }
        if (!depth_root) {
            D3D12_DESCRIPTOR_RANGE range{};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants.Num32BitValues = 1;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable.NumDescriptorRanges = 1;
            parameters[1].DescriptorTable.pDescriptorRanges = &range;
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC desc{};
            desc.NumParameters = 2;
            desc.pParameters = parameters;
            desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> error;
            HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
            if (SUCCEEDED(hr)) {
                hr = device12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&depth_root));
            }
            if (FAILED(hr)) {
                reason = Failure("Creating the bridge's depth root signature", hr);
                return false;
            }
        }
        if (depth_pipelines.count(target)) {
            return true;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = depth_root.Get();
        desc.VS = {kBridgeDepthVsDxbc, sizeof(kBridgeDepthVsDxbc)};
        desc.PS = {kBridgeDepthPsDxbc, sizeof(kBridgeDepthPsDxbc)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        // Every texel written, whatever the image held.
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 0;
        desc.DSVFormat = target;
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = device12->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) {
            reason = Failure("Creating the bridge's depth draw", hr) + " for format " +
                     std::to_string(static_cast<int>(target));
            return false;
        }
        pipeline->SetName(L"VectorXR Direct3D 11 bridge depth");
        depth_pipelines[target] = pipeline;
        return true;
    }

    // Whether the runtime's images can take the game's depth, and how: depth
    // targets of the depth format's family, one sample, the chain's size.
    bool CheckRuntimeDepth(Swapchain& chain, std::string& reason) {
        for (const Image& image : chain.images) {
            if (!image.runtime) {
                reason = "the runtime's image is null";
                return false;
            }
            const D3D12_RESOURCE_DESC runtime = image.runtime->GetDesc();
            const DXGI_FORMAT target = DepthTargetFormat(runtime.Format);
            if (runtime.Width != chain.width || runtime.Height != chain.height ||
                runtime.DepthOrArraySize != chain.array_size || runtime.SampleDesc.Count != 1 ||
                target == DXGI_FORMAT_UNKNOWN || DepthStorageFormat(target) != chain.storage) {
                std::ostringstream stream;
                stream << "the runtime's depth image (" << runtime.Width << "x" << runtime.Height << ", array "
                       << runtime.DepthOrArraySize << ", samples " << runtime.SampleDesc.Count << ", format "
                       << static_cast<int>(runtime.Format) << ") does not match the game's (" << chain.width << "x"
                       << chain.height << ", array " << chain.array_size << ", format "
                       << static_cast<int>(chain.format) << ")";
                reason = stream.str();
                return false;
            }
            if ((runtime.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                reason = "the runtime's depth image is not a depth target";
                return false;
            }
            chain.depth_target = target;
        }
        return true;
    }

    // A depth image's twin (DepthTwinFormat, shared, opened on the game's
    // device), the views each pass uses and the first recording of its draw.
    // The chain's descriptor heaps hold `index`'s views.
    bool MakeDepthTwin(Swapchain& chain, Image& image, std::uint32_t index, std::string& reason) {
        const DXGI_FORMAT twin_format = DepthTwinFormat(chain.storage);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = chain.width;
        desc.Height = chain.height;
        desc.DepthOrArraySize = static_cast<UINT16>(chain.array_size);
        desc.MipLevels = 1;
        desc.Format = twin_format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // Direct3D 11 opens it only as a render target, and writes it as an
        // unordered-access view, which needs simultaneous access.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&image.twin));
        if (FAILED(hr)) {
            reason = Failure("Creating a shared Direct3D 12 depth twin", hr);
            return false;
        }
        image.twin->SetName(L"VectorXR Direct3D 11 bridge depth twin");
        chain.twin_bytes += device12->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        HANDLE handle = nullptr;
        hr = device12->CreateSharedHandle(image.twin.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            reason = Failure("Sharing a Direct3D 12 depth twin", hr);
            return false;
        }
        hr = device11->OpenSharedResource1(handle, IID_PPV_ARGS(&image.twin11));
        CloseHandle(handle);
        if (FAILED(hr)) {
            reason = Failure("Opening a shared depth twin on the game's Direct3D 11 device", hr);
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC read{};
        read.Format = DepthReadFormat(chain.storage);
        read.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        read.Texture2DArray.MipLevels = 1;
        read.Texture2DArray.ArraySize = chain.array_size;
        hr = device11->CreateShaderResourceView(image.private11.Get(), &read, &image.depth_read11);
        if (SUCCEEDED(hr)) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC write{};
            write.Format = twin_format;
            write.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
            write.Texture2DArray.ArraySize = chain.array_size;
            hr = device11->CreateUnorderedAccessView(image.twin11.Get(), &write, &image.twin_write11);
        }
        if (FAILED(hr)) {
            reason = Failure("Making the Direct3D 11 views of the depth copy", hr);
            return false;
        }

        const UINT read_size = device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC twin_read{};
        twin_read.Format = twin_format;
        twin_read.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        twin_read.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        twin_read.Texture2DArray.MipLevels = 1;
        twin_read.Texture2DArray.ArraySize = chain.array_size;
        D3D12_CPU_DESCRIPTOR_HANDLE read_cpu = chain.twin_reads->GetCPUDescriptorHandleForHeapStart();
        read_cpu.ptr += static_cast<SIZE_T>(index) * read_size;
        device12->CreateShaderResourceView(image.twin.Get(), &twin_read, read_cpu);
        image.twin_read = chain.twin_reads->GetGPUDescriptorHandleForHeapStart();
        image.twin_read.ptr += static_cast<UINT64>(index) * read_size;

        const UINT target_size = device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        for (std::uint32_t slice = 0; slice < chain.array_size; ++slice) {
            D3D12_DEPTH_STENCIL_VIEW_DESC target{};
            target.Format = chain.depth_target;
            target.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            target.Texture2DArray.FirstArraySlice = slice;
            target.Texture2DArray.ArraySize = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE handle12 = chain.depth_targets->GetCPUDescriptorHandleForHeapStart();
            handle12.ptr += static_cast<SIZE_T>(index * chain.array_size + slice) * target_size;
            device12->CreateDepthStencilView(image.runtime.Get(), &target, handle12);
            image.depth_targets.push_back(handle12);
        }
        image.copies.emplace_back();
        return RecordCopy(chain, image, image.copies.back(), reason);
    }

    // The twins and views of a depth chain whose images are made privately:
    // false (with `reason`) when the runtime's images or the devices cannot
    // take them, the chain then stays private.
    bool MirrorDepth(Swapchain& chain, std::string& reason) {
        if (!CheckRuntimeDepth(chain, reason) || !MakeDepthPasses(chain.depth_target, reason)) {
            return false;
        }
        const auto images = static_cast<UINT>(chain.images.size());
        D3D12_DESCRIPTOR_HEAP_DESC reads{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, images,
                                         D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        D3D12_DESCRIPTOR_HEAP_DESC targets{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, images * chain.array_size,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        HRESULT hr = device12->CreateDescriptorHeap(&reads, IID_PPV_ARGS(&chain.twin_reads));
        if (SUCCEEDED(hr)) {
            hr = device12->CreateDescriptorHeap(&targets, IID_PPV_ARGS(&chain.depth_targets));
        }
        if (FAILED(hr)) {
            reason = Failure("Making the bridge's depth descriptors", hr);
            return false;
        }
        for (std::uint32_t i = 0; i < images; ++i) {
            if (!MakeDepthTwin(chain, chain.images[i], i, reason)) {
                reason = "image " + std::to_string(i) + ": " + reason;
                return false;
            }
        }
        return true;
    }

    // Lets a depth chain's twins go, nothing having used them (MirrorDepth failed).
    static void KeepDepthPrivate(Swapchain& chain, const std::string& reason) {
        for (Image& image : chain.images) {
            image.twin.Reset();
            image.twin11.Reset();
            image.depth_read11.Reset();
            image.twin_write11.Reset();
            image.depth_targets.clear();
            image.copies.clear();
        }
        chain.twin_reads.Reset();
        chain.depth_targets.Reset();
        chain.twin_bytes = 0;
        chain.mode = Mode::kPrivate;
        chain.private_reason = reason;
    }

    // Copies the game's depth into the image's twin on the game's immediate
    // context (holding the context, ContextHold, then `mutex`), leaving the
    // context's state as it was. A game still holding the image as its depth
    // target gets the target back after the copy: Direct3D 11 reads no
    // texture bound for writing (it would unbind the copy's read instead).
    // The caller's hold makes the whole sequence one: no other thread's call
    // lands inside it.
    void CopyDepthToTwin(const Swapchain& chain, Image& image) {
        const auto start = std::chrono::steady_clock::now();
        CopyDepthToTwinHeld(chain, image, context11.Get());
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        depth_copy_ms_total += ms;
        depth_copy_ms_max = std::max(depth_copy_ms_max, ms);
        ++depth_copies;
    }

    // CopyDepthToTwin's calls, holding the context.
    void CopyDepthToTwinHeld(const Swapchain& chain, Image& image, ID3D11DeviceContext4* context) {
        // The output merger: only its depth target changes, and only while it
        // is the image. Its render targets are set again up to the last one
        // bound, with its unordered-access views kept: a view in a slot the
        // targets set would be unbound (the keep flag keeps only the slots
        // past them), and Direct3D 11 keeps none in a slot below a bound
        // target, so every one stays where it was, its hidden counter with it
        // (which a view set again could not be given back). OMSetRenderTargets
        // would unbind them all.
        ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth_target_raw = nullptr;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depth_target_raw);
        ComPtr<ID3D11RenderTargetView> held_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        UINT target_count = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            held_targets[i].Attach(targets[i]);
            if (targets[i]) target_count = i + 1;
        }
        ComPtr<ID3D11DepthStencilView> depth_target;
        depth_target.Attach(depth_target_raw);
        bool unbound = false;
        if (depth_target) {
            ComPtr<ID3D11Resource> bound;
            depth_target->GetResource(&bound);
            if (bound.Get() == image.private11.Get()) {
                context->OMSetRenderTargetsAndUnorderedAccessViews(target_count, target_count ? targets : nullptr,
                                                                   nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                                   nullptr, nullptr);
                unbound = true;
                ++depth_target_unbinds;
            }
        }

        ID3D11ComputeShader* shader_raw = nullptr;
        ID3D11ClassInstance* instances[256]{};
        UINT instance_count = 256;
        context->CSGetShader(&shader_raw, instances, &instance_count);
        ComPtr<ID3D11ComputeShader> shader;
        shader.Attach(shader_raw);
        ComPtr<ID3D11ClassInstance> held_instances[256];
        for (UINT i = 0; i < 256; ++i) {
            held_instances[i].Attach(instances[i]);
        }
        ComPtr<ID3D11ShaderResourceView> read;
        context->CSGetShaderResources(0, 1, &read);
        ComPtr<ID3D11UnorderedAccessView> write;
        context->CSGetUnorderedAccessViews(0, 1, &write);

        ID3D11ShaderResourceView* depth_read = image.depth_read11.Get();
        ID3D11UnorderedAccessView* twin_write = image.twin_write11.Get();
        context->CSSetShader(depth_to_twin.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &depth_read);
        context->CSSetUnorderedAccessViews(0, 1, &twin_write, nullptr);
        if (depth_copy_hook) {
            depth_copy_hook();
        }
        context->Dispatch((chain.width + 7) / 8, (chain.height + 7) / 8, chain.array_size);

        // Unbound before the game's views go back, whichever slot they held.
        ID3D11UnorderedAccessView* no_write = nullptr;
        ID3D11ShaderResourceView* no_read = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &no_write, nullptr);
        context->CSSetShaderResources(0, 1, &no_read);
        // -1: the game's view keeps its hidden counter.
        const UINT keep_counter = static_cast<UINT>(-1);
        context->CSSetUnorderedAccessViews(0, 1, write.GetAddressOf(), &keep_counter);
        context->CSSetShaderResources(0, 1, read.GetAddressOf());
        context->CSSetShader(shader.Get(), instance_count ? instances : nullptr, instance_count);
        if (unbound) {
            context->OMSetRenderTargetsAndUnorderedAccessViews(target_count, target_count ? targets : nullptr,
                                                               depth_target.Get(), 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                               nullptr, nullptr);
        }
    }

    // Whether the release of `chain`'s oldest acquired image needs the
    // game's immediate context (holding `mutex`): it is copied, and its
    // Direct3D 11 half has not run yet.
    static bool NeedsContext(const Swapchain& chain) {
        return chain.staged.empty() && !chain.acquired.empty() && chain.mode != Mode::kPrivate &&
               chain.acquired.front() < chain.images.size();
    }

    // The Direct3D 11 half of the release of `chain`'s oldest acquired image
    // (holding the context, then `mutex`; NeedsContext): a depth image's copy
    // into its twin, a multisampled image's resolve, and the "written"
    // signal the bridge's queue waits for, flushed.
    Staged Stage(Swapchain& chain) {
        const auto start = std::chrono::steady_clock::now();
        Staged staged;
        staged.index = chain.acquired.front();
        chain.acquired.pop_front();
        NoteThread();
        Image& image = chain.images[staged.index];
        if (chain.mode == Mode::kDepth) {
            // The twin is written again only after the bridge's queue is done
            // drawing its last release (a wait on the GPU, ending as the
            // colour's at acquire does); then the game's depth goes into it.
            const UINT64 value = image.done;
            if (value && done->GetCompletedValue() < value) {
                context11->Wait(done11.Get(), value);
                ++chain.done_waits;
                ++done_waits;
            }
            CopyDepthToTwin(chain, image);
        }
        if (chain.mode == Mode::kResolve) {
            const DXGI_FORMAT format = ResolveFormat(chain.format);
            for (UINT slice = 0; slice < chain.array_size; ++slice) {
                const UINT subresource = D3D11CalcSubresource(0, slice, 1);
                context11->ResolveSubresource(image.twin11.Get(), subresource, image.private11.Get(), subresource,
                                              format);
            }
        }
        staged.written = ++written_value;
        context11->Signal(written11.Get(), staged.written);
        context11->Flush();
        staged.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return staged;
    }

    // The Direct3D 12 half (holding `mutex`; nothing of the game's context):
    // the bridge's queue copies the twin into the runtime's image once
    // Direct3D 11's "written" is signalled, ahead of anything else queued on
    // it for the release (Frame Generation's capture). False when the copy
    // was skipped (the bridge's queue is stalled).
    bool Submit(Swapchain& chain, const Staged& staged) {
        const auto start = std::chrono::steady_clock::now();
        Image& image = chain.images[staged.index];
        Copy* copy = ReadyCopy(chain, image);
        if (!copy) {
            NoteHandover(start, staged.ms);
            // Every recording of the copy is still queued: the bridge's queue is
            // stalled. The runtime's image keeps what it had.
            ++chain.skipped_copies;
            ++skipped_copies;
            if (skipped_copies == next_skip_report && log) {
                next_skip_report *= 10;
                log("Direct3D 11 bridge: its queue is stalled; releases not copied into the runtime's images so "
                    "far: " + std::to_string(skipped_copies) + ".");
            }
            return false;
        }
        queue->Wait(written.Get(), staged.written);
        ID3D12CommandList* lists[] = {copy->list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 done_at = ++done_value;
        queue->Signal(done.Get(), done_at);
        copy->done = done_at;
        image.done = done_at;
        chain.last_done = done_at;
        ++chain.copies;
        ++copies;
        NoteHandover(start, staged.ms);
        return true;
    }

    bool MakePrivate(const Swapchain& chain, Image& image, const XrSwapchainCreateInfo& info, std::string& reason) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = chain.width;
        desc.Height = chain.height;
        desc.MipLevels = chain.mode == Mode::kResolve ? 1 : chain.mip_count;
        desc.ArraySize = chain.array_size;
        desc.Format = chain.storage;
        desc.SampleDesc.Count = chain.sample_count;
        desc.Usage = D3D11_USAGE_DEFAULT;
        const bool depth = chain.mode == Mode::kDepth || chain.mode == Mode::kPrivate;
        if (depth) {
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            // Mirrored depth is read by the copy into its twin.
            if ((info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) || chain.mode == Mode::kDepth) {
                desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            }
        } else {
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }
        const HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &image.private11);
        if (FAILED(hr)) {
            reason = Failure(depth ? "Creating a private depth texture" : "Creating a private multisampled texture", hr);
            return false;
        }
        return true;
    }

    Swapchain* Find(XrSwapchain swapchain) {
        const auto it = swapchains.find(swapchain);
        return it == swapchains.end() ? nullptr : &it->second;
    }
    const Swapchain* Find(XrSwapchain swapchain) const {
        const auto it = swapchains.find(swapchain);
        return it == swapchains.end() ? nullptr : &it->second;
    }
};

D3D11Bridge::D3D11Bridge() = default;

D3D11Bridge::~D3D11Bridge() {
    // Process teardown: the devices may be gone, so nothing is waited for or released.
    if (impl_) {
        const auto detach = [](Impl::Swapchain& chain) {
            Impl::DetachGpu(chain);
            chain.depth_targets.Detach();
            for (Impl::Image& image : chain.images) {
                image.twin11.Detach();
                image.private11.Detach();
                image.depth_read11.Detach();
                image.twin_write11.Detach();
            }
        };
        for (auto& [swapchain, chain] : impl_->swapchains) {
            detach(chain);
        }
        for (Impl::Swapchain& chain : impl_->retired) {
            detach(chain);
        }
        impl_->DetachPipelines();
        impl_->depth_to_twin.Detach();
        impl_->queue.Detach();
        impl_->written.Detach();
        impl_->done.Detach();
        impl_->idle.Detach();
        impl_->written11.Detach();
        impl_->done11.Detach();
        impl_->adapter.Detach();
        impl_->device12.Detach();
        impl_->multithread.Detach();
        impl_->context11.Detach();
        impl_->device11.Detach();
    }
}

void D3D11Bridge::SetLog(std::function<void(const std::string&)> log) {
    log_ = std::move(log);
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->log = log_;
    }
}

const XrSessionCreateInfo* D3D11Bridge::Begin(XrInstance instance, const XrSessionCreateInfo* info,
                                              PFN_xrGetInstanceProcAddr get_instance_proc_addr,
                                              std::string& reason, std::string& description) {
    End();
    reason.clear();
    description.clear();
    queried_d3d12_requirements_ = false;
    const auto* first = info ? static_cast<const XrBaseInStructure*>(info->next) : nullptr;
    if (!first || first->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
        reason = "its Direct3D 11 binding is not the first structure of the session's create info";
        return nullptr;
    }
    if (const char* bridge = std::getenv("VECTORXR_D3D11_BRIDGE"); bridge && std::string(bridge) == "off") {
        reason = "VECTORXR_D3D11_BRIDGE=off";
        return nullptr;
    }
    const auto& game = *reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(first);
    if (!game.device) {
        reason = "the Direct3D 11 binding names no device";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    if (FAILED(game.device->QueryInterface(IID_PPV_ARGS(&impl->device11)))) {
        reason = "the game's device has no ID3D11Device5 (shared fences need Windows 10 1703 or later)";
        return nullptr;
    }
    ComPtr<ID3D11DeviceContext> immediate;
    game.device->GetImmediateContext(&immediate);
    if (!immediate || FAILED(immediate.As(&impl->context11))) {
        reason = "the game's immediate context has no ID3D11DeviceContext4";
        return nullptr;
    }
    if (SUCCEEDED(immediate.As(&impl->multithread))) {
        impl->multithread_protected = impl->multithread->GetMultithreadProtected() != FALSE;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> game_adapter;
    DXGI_ADAPTER_DESC game_adapter_desc{};
    if (FAILED(impl->device11.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&game_adapter)) ||
        FAILED(game_adapter->GetDesc(&game_adapter_desc))) {
        reason = "the game's adapter could not be identified";
        return nullptr;
    }

    // The runtime requires its graphics requirements to be asked for before a
    // Direct3D 12 session, and names the adapter it composes on.
    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    if (!get_instance_proc_addr ||
        XR_FAILED(get_instance_proc_addr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) ||
        !get_requirements) {
        reason = "the runtime offers no xrGetD3D12GraphicsRequirementsKHR";
        return nullptr;
    }
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    queried_d3d12_requirements_ = true;
    const XrResult requirements_result = get_requirements(instance, info->systemId, &requirements);
    if (XR_FAILED(requirements_result)) {
        reason = "xrGetD3D12GraphicsRequirementsKHR failed (" + std::to_string(requirements_result) + ")";
        return nullptr;
    }
    if (!SameLuid(requirements.adapterLuid, game_adapter_desc.AdapterLuid)) {
        reason = "the runtime asks for Direct3D 12 on adapter " + LuidText(requirements.adapterLuid) +
                 ", and the game's device is on adapter " + LuidText(game_adapter_desc.AdapterLuid);
        return nullptr;
    }

    if (const char* debug = std::getenv("VECTORXR_D3D11_BRIDGE_DEBUG"); debug && std::string(debug) == "1") {
        ComPtr<ID3D12Debug> debug_interface;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface)))) {
            debug_interface->EnableDebugLayer();
            impl->debug_layer = true;
        }
    }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->EnumAdapterByLuid(game_adapter_desc.AdapterLuid, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(hr)) {
        reason = Failure("Finding the game's adapter for Direct3D 12", hr);
        return nullptr;
    }
    adapter.As(&impl->adapter);
    const D3D_FEATURE_LEVEL feature_level = std::max(requirements.minFeatureLevel, D3D_FEATURE_LEVEL_11_0);
    hr = D3D12CreateDevice(adapter.Get(), feature_level, IID_PPV_ARGS(&impl->device12));
    if (FAILED(hr)) {
        reason = Failure("Creating a Direct3D 12 device on the game's adapter", hr);
        return nullptr;
    }

    // Typeless images are shared only from compatibility tier 1 on.
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
    impl->device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4));
    D3D11_FEATURE_DATA_D3D11_OPTIONS5 options5{};
    impl->device11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &options5, sizeof(options5));
    if (options4.SharedResourceCompatibilityTier < D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_1) {
        reason = "the driver shares no typeless images between the APIs (Direct3D 12 shared resource "
                 "compatibility tier " +
                 std::to_string(static_cast<int>(options4.SharedResourceCompatibilityTier)) + ")";
        return nullptr;
    }

    // High priority, as Frame Generation's other queues: it carries only the
    // short copies and captures their work waits for, never the game's.
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                                        D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    hr = impl->device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&impl->queue));
    if (FAILED(hr)) {
        reason = Failure("Creating the bridge's queue", hr);
        return nullptr;
    }
    impl->queue->SetName(L"VectorXR Direct3D 11 bridge");

    const auto share_fence = [&](ComPtr<ID3D12Fence>& fence, ComPtr<ID3D11Fence>& fence11) {
        HRESULT result = impl->device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        HANDLE handle = nullptr;
        if (SUCCEEDED(result)) {
            result = impl->device12->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        }
        if (SUCCEEDED(result)) {
            result = impl->device11->OpenSharedFence(handle, IID_PPV_ARGS(&fence11));
        }
        if (handle) {
            CloseHandle(handle);
        }
        return result;
    };
    if (FAILED(hr = share_fence(impl->written, impl->written11)) ||
        FAILED(hr = share_fence(impl->done, impl->done11)) ||
        FAILED(hr = impl->device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->idle)))) {
        reason = Failure("Sharing the bridge's fences with the game's device", hr);
        return nullptr;
    }

    // Once the session is Direct3D 12, a swapchain whose images the game's
    // device cannot open fails the game's xrCreateSwapchain. A device
    // wrapped by another Direct3D 11 mod (REAL VR's dxgi.dll) can refuse
    // shared textures while sharing fences, so one is opened here first and
    // the session stays Direct3D 11 if that fails.
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 16;
        desc.Height = 16;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        ComPtr<ID3D12Resource> probe;
        ComPtr<ID3D11Texture2D> probe11;
        HANDLE handle = nullptr;
        hr = impl->device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&probe));
        if (SUCCEEDED(hr)) {
            hr = impl->device12->CreateSharedHandle(probe.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        }
        if (SUCCEEDED(hr)) {
            hr = impl->device11->OpenSharedResource1(handle, IID_PPV_ARGS(&probe11));
        }
        if (handle) {
            CloseHandle(handle);
        }
        if (FAILED(hr)) {
            reason = Failure("Opening a shared texture on the game's Direct3D 11 device", hr) +
                     " (another Direct3D 11 mod may wrap the game's device)";
            return nullptr;
        }
    }

    // VECTORXR_D3D11_BRIDGE_DEPTH=private keeps depth the game's own, as
    // before the bridge mirrored it (left out of submitted frames).
    if (const char* depth = std::getenv("VECTORXR_D3D11_BRIDGE_DEPTH"); depth && std::string(depth) == "private") {
        impl->mirror_depth = false;
    }

    impl->binding.next = game.next;
    impl->binding.device = impl->device12.Get();
    impl->binding.queue = impl->queue.Get();
    impl->info = *info;
    impl->info.next = &impl->binding;
    impl->log = log_;

    std::ostringstream stream;
    stream << "adapter=\"" << Utf8(game_adapter_desc.Description) << "\" luid="
           << LuidText(game_adapter_desc.AdapterLuid) << ", d3d12FeatureLevel=" << Hex(feature_level)
           << " (runtime minimum " << Hex(requirements.minFeatureLevel) << ")"
           << ", sharedResourceCompatibilityTier=" << static_cast<int>(options4.SharedResourceCompatibilityTier)
           << ", d3d11SharedResourceTier=" << static_cast<int>(options5.SharedResourceTier)
           << ", multithreadProtected=" << (impl->multithread_protected ? "yes" : "no")
           << ", depth=" << (impl->mirror_depth ? "mirrored" : "private (VECTORXR_D3D11_BRIDGE_DEPTH=private)")
           << ", videoMemory=" << impl->VideoMemory() << (impl->debug_layer ? ", debugLayer=on" : "");
    description = stream.str();
    impl_ = std::move(impl);
    return &impl_->info;
}

bool D3D11Bridge::QueriedD3D12Requirements() const {
    return queried_d3d12_requirements_;
}

bool D3D11Bridge::Active() const {
    return impl_ != nullptr;
}

const void* D3D11Bridge::Binding() const {
    return impl_ ? &impl_->binding : nullptr;
}

void D3D11Bridge::SessionCreated() {
    if (impl_) {
        impl_->binding.next = nullptr;
        impl_->info.next = &impl_->binding;
    }
}

bool D3D11Bridge::Drain() {
    if (!impl_) {
        return true;
    }
    if (WaitIdle(kRetireWaitMs)) {
        return true;
    }
    {
        // The bridge's queue waits for Direct3D 11 work that did not run in
        // time: whatever it drew no longer matters, and the queue must finish
        // while the runtime's images still exist.
        std::scoped_lock lock(impl_->mutex);
        impl_->written->Signal(impl_->written_value);
    }
    const bool idle = WaitIdle(kRetireWaitMs);
    if (!idle && impl_->log) {
        impl_->log("Direct3D 11 bridge: its queue did not finish before the session ended.");
    }
    return idle;
}

void D3D11Bridge::End() {
    if (!impl_) {
        return;
    }
    if (!WaitIdle(kRetireWaitMs)) {
        // Direct3D 11 must not wait for good for copies that never run, so
        // "done" reaches every value handed out. "written" is left alone:
        // the copies still queued wait for it, and whatever they use is kept
        // for the process (the runtime's images included).
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->done->Signal(impl_->done_value);
        }
        if (impl_->log) {
            impl_->log("Direct3D 11 bridge: its queue did not finish at teardown; its Direct3D 12 objects are kept "
                       "for the process.");
        }
        for (auto& [swapchain, chain] : impl_->swapchains) {
            Impl::DetachGpu(chain);
        }
        for (Impl::Swapchain& chain : impl_->retired) {
            Impl::DetachGpu(chain);
        }
        impl_->DetachPipelines();
        impl_->queue.Detach();
        impl_->written.Detach();
        impl_->done.Detach();
        impl_->idle.Detach();
        impl_->device12.Detach();
    }
    impl_.reset();
}

bool D3D11Bridge::Lost(std::string& reason) const {
    if (!impl_) {
        return false;
    }
    const HRESULT hr = impl_->device12->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) {
        return false;
    }
    reason = "the bridge's Direct3D 12 device was removed (hr=" + Hex(static_cast<std::uint32_t>(hr)) + ")";
    return true;
}

void D3D11Bridge::FrameEnded() {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->NoteThread();
    impl_->ReportDebugMessages();
}

XrSwapchainCreateInfo D3D11Bridge::RuntimeCreateInfo(const XrSwapchainCreateInfo& info) {
    XrSwapchainCreateInfo runtime = info;
    if (!IsDepthSwapchain(info)) {
        runtime.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        runtime.sampleCount = 1;
    } else {
        // The bridge's draw writes depth through a depth target: PimaxXR and
        // Virtual Desktop make an image one (in the depth-write state) only
        // when asked for depth-stencil use.
        runtime.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    return runtime;
}

bool D3D11Bridge::AddSwapchain(XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                               PFN_xrEnumerateSwapchainImages enumerate, std::string& reason) {
    reason.clear();
    if (!impl_) {
        reason = "the session is not bridged";
        return false;
    }
    Impl::Swapchain chain;
    chain.format = static_cast<DXGI_FORMAT>(info.format);
    chain.width = info.width;
    chain.height = info.height;
    chain.array_size = std::max(1u, info.arraySize) * std::max(1u, info.faceCount);
    chain.mip_count = std::max(1u, info.mipCount);
    chain.sample_count = std::max(1u, info.sampleCount);
    const bool depth = IsDepthSwapchain(info);
    chain.mode = depth ? Impl::Mode::kDepth : chain.sample_count > 1 ? Impl::Mode::kResolve : Impl::Mode::kTwin;
    chain.storage = depth ? DepthStorageFormat(chain.format) : ColorStorageFormat(chain.format);
    // Depth the bridge cannot mirror stays the game's own (with why).
    if (depth && !impl_->mirror_depth) {
        chain.mode = Impl::Mode::kPrivate;
        chain.private_reason = "VECTORXR_D3D11_BRIDGE_DEPTH=private";
    } else if (depth && chain.sample_count > 1) {
        chain.mode = Impl::Mode::kPrivate;
        chain.private_reason = "multisampled";
    } else if (depth && !IsDepthFormat(chain.format)) {
        chain.mode = Impl::Mode::kPrivate;
        chain.private_reason = "format " + std::to_string(static_cast<int>(chain.format)) + " is not depth";
    }

    std::uint32_t count = 0;
    if (!enumerate || XR_FAILED(enumerate(swapchain, 0, &count, nullptr)) || count == 0) {
        reason = "the runtime's images could not be counted";
        return false;
    }
    std::vector<XrSwapchainImageD3D12KHR> runtime_images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    if (XR_FAILED(enumerate(swapchain, count, &count,
                            reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data())))) {
        reason = "the runtime's Direct3D 12 images could not be enumerated";
        return false;
    }
    chain.images.resize(count);
    const bool unordered_access = (info.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0 &&
                                  chain.mode == Impl::Mode::kTwin;
    for (std::uint32_t i = 0; i < count; ++i) {
        Impl::Image& image = chain.images[i];
        image.runtime = runtime_images[i].texture;
        const bool twin = chain.mode == Impl::Mode::kTwin || chain.mode == Impl::Mode::kResolve;
        if (twin && !impl_->MakeTwin(chain, image, unordered_access, reason)) {
            reason = "image " + std::to_string(i) + ": " + reason;
            return false;
        }
        if (chain.mode != Impl::Mode::kTwin && !impl_->MakePrivate(chain, image, info, reason)) {
            reason = "image " + std::to_string(i) + ": " + reason;
            return false;
        }
    }
    if (chain.mode == Impl::Mode::kDepth) {
        std::string why;
        if (!impl_->MirrorDepth(chain, why)) {
            Impl::KeepDepthPrivate(chain, why);
        }
    }
    // Before taking `mutex`: turning protection on may wait for the
    // context's lock.
    const bool protect = chain.mode == Impl::Mode::kDepth && impl_->multithread;
    const bool was_protected = protect && impl_->multithread->GetMultithreadProtected() != FALSE;
    if (protect && !was_protected) {
        impl_->multithread->SetMultithreadProtected(TRUE);
    }
    const bool now_protected = protect && impl_->multithread->GetMultithreadProtected() != FALSE;
    if (chain.mode == Impl::Mode::kPrivate && chain.private_reason.rfind("VECTORXR_", 0) != 0 && impl_->log) {
        impl_->log("Direct3D 11 bridge: depth swapchain " + Hex(reinterpret_cast<std::uintptr_t>(swapchain)) +
                   " stays the game's own (its depth is left out of submitted frames): " + chain.private_reason + ".");
    }
    std::scoped_lock lock(impl_->mutex);
    if (protect) {
        chain.protection = impl_->NoteDepthProtection(was_protected, now_protected);
    }
    impl_->swapchains[swapchain] = std::move(chain);
    ++impl_->swapchains_added;
    return true;
}

bool D3D11Bridge::Bridged(XrSwapchain swapchain) const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->Find(swapchain) != nullptr;
}

void D3D11Bridge::SetComposed(XrSwapchain swapchain, bool composed) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    if (Impl::Swapchain* chain = impl_->Find(swapchain)) {
        chain->composed = composed;
    }
}

bool D3D11Bridge::Composed(XrSwapchain swapchain) const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    const Impl::Swapchain* chain = impl_->Find(swapchain);
    return chain && chain->composed;
}

bool D3D11Bridge::Unmirrored(XrSwapchain swapchain) const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    const Impl::Swapchain* chain = impl_->Find(swapchain);
    return chain && chain->mode == Impl::Mode::kPrivate;
}

bool D3D11Bridge::HasUnmirrored() const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return std::any_of(impl_->swapchains.begin(), impl_->swapchains.end(),
                       [](const auto& entry) { return entry.second.mode == Impl::Mode::kPrivate; });
}

std::vector<ID3D11Texture2D*> D3D11Bridge::Images(XrSwapchain swapchain) const {
    std::vector<ID3D11Texture2D*> textures;
    if (!impl_) {
        return textures;
    }
    std::scoped_lock lock(impl_->mutex);
    if (const Impl::Swapchain* chain = impl_->Find(swapchain)) {
        for (const Impl::Image& image : chain->images) {
            textures.push_back(image.private11 ? image.private11.Get() : image.twin11.Get());
        }
    }
    return textures;
}

void D3D11Bridge::Acquired(XrSwapchain swapchain, std::uint32_t index) {
    if (!impl_) {
        return;
    }
    std::unique_lock lock(impl_->mutex);
    Impl::Swapchain* chain = impl_->Find(swapchain);
    if (!chain) {
        return;
    }
    chain->acquired.push_back(index);
    // Depth is rendered privately: its twin is written only at release.
    if (chain->mode == Impl::Mode::kPrivate || chain->mode == Impl::Mode::kDepth || index >= chain->images.size()) {
        return;
    }
    // Until the bridge's queue is done reading the image's last release, the
    // game must not write it again. The value's signal is already submitted
    // (at that release), so the wait always ends.
    const UINT64 value = chain->images[index].done;
    if (!value || impl_->done->GetCompletedValue() >= value) {
        return;
    }
    impl_->NoteThread();
    ++chain->done_waits;
    ++impl_->done_waits;
    // Queued without `mutex`: a call on the game's context may wait for its
    // lock, which a game thread may hold while it waits for `mutex`.
    lock.unlock();
    impl_->context11->Wait(impl_->done11.Get(), value);
}

bool D3D11Bridge::Released(XrSwapchain swapchain) {
    if (!impl_) {
        return true;
    }
    std::unique_lock lock(impl_->mutex);
    Impl::Swapchain* chain = impl_->Find(swapchain);
    if (!chain) {
        return true;
    }
    // A release whose Direct3D 11 half did not run ahead (StageReleases)
    // runs it here, holding the context first (ContextHold's lock order).
    std::optional<ContextHold> hold;
    if (Impl::NeedsContext(*chain)) {
        lock.unlock();
        hold.emplace(impl_->multithread.Get(), &impl_->context_wait_hook);
        lock.lock();
        chain = impl_->Find(swapchain);
        if (!chain) {
            return true;
        }
    }
    ++impl_->releases;
    if (chain->pending > 0) {
        --chain->pending;
    }
    if (!chain->staged.empty()) {
        // ...and the bridge's queue then copies it into the runtime's image.
        const Impl::Staged staged = chain->staged.front();
        chain->staged.pop_front();
        hold.reset();
        return impl_->Submit(*chain, staged);
    }
    if (chain->acquired.empty()) {
        ++chain->unmatched_releases;
        ++impl_->unmatched_releases;
        return true;
    }
    if (!Impl::NeedsContext(*chain)) {
        // Private depth (or an index past the images): nothing to copy.
        chain->acquired.pop_front();
        return true;
    }
    // The game's rendering into the image is handed to the bridge's queue
    // (depth copied into its twin first, multisampled images resolved)...
    const Impl::Staged staged = impl_->Stage(*chain);
    hold.reset();
    return impl_->Submit(*chain, staged);
}

void D3D11Bridge::StageReleases(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    const auto waiting = [&](const Impl::Swapchain& chain) {
        return chain.pending > chain.staged.size() && chain.acquired.size() > 0;
    };
    bool any = false;
    {
        std::scoped_lock lock(impl_->mutex);
        // This call's thread counts (a frame's or swapchain call's, as
        // FrameEnded and ReleaseCalled note theirs).
        impl_->NoteThread();
        if (swapchain != XR_NULL_HANDLE) {
            const Impl::Swapchain* chain = impl_->Find(swapchain);
            any = chain && waiting(*chain);
        } else {
            any = std::any_of(impl_->swapchains.begin(), impl_->swapchains.end(),
                              [&](const auto& entry) { return waiting(entry.second); });
        }
    }
    // Before the context and `mutex`: a second thread seen since the last
    // hand-over (this one included) turns protection on first.
    impl_->ApplyWantedProtection();
    if (!any) {
        return;
    }
    // The context first, holding nothing else (ContextHold's lock order)...
    const ContextHold hold = impl_->HoldContext();
    std::scoped_lock lock(impl_->mutex);
    const auto stage = [&](Impl::Swapchain& chain) {
        // ...each release the application made, oldest first: the image
        // acquired earliest, as Released hands it over.
        while (waiting(chain)) {
            if (chain.mode == Impl::Mode::kPrivate || chain.acquired.front() >= chain.images.size()) {
                return; // nothing to run ahead: Released hands it over as it is
            }
            chain.staged.push_back(impl_->Stage(chain));
        }
    };
    if (swapchain != XR_NULL_HANDLE) {
        if (Impl::Swapchain* chain = impl_->Find(swapchain)) {
            stage(*chain);
        }
    } else {
        for (auto& [handle, chain] : impl_->swapchains) {
            stage(chain);
        }
    }
}

void D3D11Bridge::SetContextWaitHookForTest(std::function<void()> hook) {
    if (impl_) {
        impl_->context_wait_hook = std::move(hook);
    }
}

void D3D11Bridge::SetDepthCopyHookForTest(std::function<void()> hook) {
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->depth_copy_hook = std::move(hook);
    }
}

void D3D11Bridge::ReleaseCalled(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->NoteThread();
    if (Impl::Swapchain* chain = impl_->Find(swapchain)) {
        ++chain->pending;
    }
}

void D3D11Bridge::Abandoned(XrSwapchain swapchain) {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    Impl::Swapchain* chain = impl_->Find(swapchain);
    if (!chain) {
        return;
    }
    ++impl_->releases;
    if (chain->acquired.empty()) {
        ++chain->unmatched_releases;
        ++impl_->unmatched_releases;
        return;
    }
    chain->acquired.pop_front();
}

bool D3D11Bridge::RemoveSwapchain(XrSwapchain swapchain) {
    if (!impl_) {
        return true;
    }
    UINT64 last_done = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        const Impl::Swapchain* chain = impl_->Find(swapchain);
        if (!chain) {
            return true;
        }
        last_done = chain->last_done;
    }
    const bool finished = WaitFence(impl_->done.Get(), last_done, kRetireWaitMs);
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->swapchains.find(swapchain);
    if (it == impl_->swapchains.end()) {
        return true;
    }
    if (!finished) {
        if (impl_->log) {
            impl_->log("Direct3D 11 bridge: copies into a destroyed swapchain did not finish; it is kept until the "
                       "session ends.");
        }
        impl_->retired.push_back(std::move(it->second));
    }
    impl_->swapchains.erase(it);
    return finished;
}

bool D3D11Bridge::WaitIdle(std::uint32_t timeout_ms) {
    if (!impl_) {
        return true;
    }
    UINT64 value = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        value = impl_->MarkIdle();
    }
    return WaitFence(impl_->idle.Get(), value, timeout_ms);
}

std::string D3D11Bridge::Describe(XrSwapchain swapchain) const {
    if (!impl_) {
        return "not bridged";
    }
    std::scoped_lock lock(impl_->mutex);
    const Impl::Swapchain* chain = impl_->Find(swapchain);
    return chain ? impl_->Describe(*chain) : "not bridged";
}

std::string D3D11Bridge::Summary() const {
    if (!impl_) {
        return "inactive";
    }
    std::scoped_lock lock(impl_->mutex);
    std::uint64_t twin_bytes = 0;
    for (const auto& [swapchain, chain] : impl_->swapchains) {
        twin_bytes += chain.twin_bytes;
    }
    std::ostringstream stream;
    stream << "swapchains=" << impl_->swapchains_added << " (" << impl_->swapchains.size() << " live, "
           << impl_->retired.size() << " kept), liveTwinMiB=" << MiB(twin_bytes)
           << ", releases=" << impl_->releases << ", copies=" << impl_->copies
           << ", skippedCopies=" << impl_->skipped_copies << ", copyRecordings=" << impl_->recordings.load()
           << ", doneWaits=" << impl_->done_waits << ", unmatchedReleases=" << impl_->unmatched_releases
           << ", depthCopies=" << impl_->depth_copies << ", depthTargetUnbinds=" << impl_->depth_target_unbinds
           << ", depthCopyMs="
           << HandoverText(impl_->depth_copy_ms_total, impl_->depth_copies, impl_->depth_copy_ms_max)
           << ", handoverMs=" << HandoverText(impl_->handover_ms_total, impl_->handovers, impl_->handover_ms_max)
           << ", slowHandovers=" << impl_->slow_handovers
           << ", writtenValue=" << impl_->written_value << ", doneValue=" << impl_->done_value
           << ", doneCompleted=" << (impl_->done ? impl_->done->GetCompletedValue() : 0)
           << ", secondThread=" << (impl_->reported_second_thread ? "yes" : "no")
           << ", multithreadProtected=" << (impl_->multithread_protected ? "yes" : "no")
           << ", videoMemory=" << impl_->VideoMemory();
    return stream.str();
}

} // namespace depthxr
