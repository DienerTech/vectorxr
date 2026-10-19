#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_d3d11_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
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
        kPrivate, // depth: rendered privately, never reaches the runtime's images
    };
    // A recording of the copy of a twin into the runtime's image.
    struct Copy {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 done{0}; // the done value after its last execution
    };
    struct Image {
        ComPtr<ID3D12Resource> twin;
        ComPtr<ID3D11Texture2D> twin11;    // the twin, opened on the game's device
        ComPtr<ID3D11Texture2D> private11; // rendered into instead of the twin
        // The runtime's image, referenced: a copy still queued when the
        // runtime lets the image go writes memory that still exists.
        ComPtr<ID3D12Resource> runtime;
        std::vector<Copy> copies;
        UINT64 done{0}; // the done value after its last copy
    };
    struct Swapchain {
        Mode mode{Mode::kTwin};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}; // as the application asked
        DXGI_FORMAT storage{DXGI_FORMAT_UNKNOWN};
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t array_size{1};
        std::uint32_t mip_count{1};
        std::uint32_t sample_count{1};
        bool composed{false};
        std::vector<Image> images;
        std::deque<std::uint32_t> acquired; // acquired and not released yet, oldest first
        UINT64 last_done{0};
        std::uint64_t twin_bytes{0};
        std::uint64_t copies{0};
        std::uint64_t done_waits{0}; // acquires whose image's copy had not finished: Direct3D 11 waits for it
        std::uint64_t skipped_copies{0}; // releases not copied: the bridge's queue was stalled
        std::uint64_t unmatched_releases{0}; // releases with nothing acquired
    };

    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    bool multithread_protected{false};
    ComPtr<ID3D12Device> device12;
    ComPtr<IDXGIAdapter3> adapter; // for video memory figures
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> written; // signalled by Direct3D 11 only
    ComPtr<ID3D12Fence> done;    // signalled by the bridge's queue only
    ComPtr<ID3D12Fence> idle;    // the bridge's queue's progress, for retiring
    ComPtr<ID3D11Fence> written11;
    ComPtr<ID3D11Fence> done11;

    // Everything below, and the order of signals on both APIs: each fence's
    // values are signalled in the order they are handed out.
    mutable std::mutex mutex;
    UINT64 written_value{0};
    UINT64 done_value{0};
    UINT64 idle_value{0};
    DWORD thread{0}; // the first thread a swapchain or frame call came from
    bool reported_second_thread{false};
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
    // The game thread's time in a release's handover (Direct3D 11's flush, a
    // recording of the copy, the bridge queue's submission), milliseconds:
    // the session's total and longest, and how many took kSlowHandoverMs.
    double handover_ms_total{0.0};
    double handover_ms_max{0.0};
    std::uint64_t handovers{0};
    std::uint64_t slow_handovers{0};
    std::function<void(const std::string&)> log;

    // Counts a handover that started at `start` (holding `mutex`).
    void NoteHandover(std::chrono::steady_clock::time_point start) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
    // thread turn that protection on.
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
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context11.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE);
            multithread_protected = multithread->GetMultithreadProtected() != FALSE;
        }
        if (log) {
            log("Direct3D 11 bridge: the game's swapchain and frame calls come from more than one thread (" +
                std::to_string(current) + ", first " + std::to_string(thread) + "); " +
                (multithread_protected
                     ? "turned the game device's multithread protection on, so each of the bridge's Direct3D 11 "
                       "calls is serialized with the game's (calls made before this one were not)."
                     : "its device has no multithread protection to turn on: the bridge's Direct3D 11 calls there "
                       "may race the game's rendering."));
        }
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
        for (Image& image : chain.images) {
            image.twin.Detach();
            image.runtime.Detach();
            for (Copy& copy : image.copies) {
                copy.list.Detach();
                copy.allocator.Detach();
            }
        }
    }

    std::string VideoMemory() const {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
            return "unavailable";
        }
        return std::to_string(MiB(memory.CurrentUsage)) + "/" + std::to_string(MiB(memory.Budget)) + " MiB";
    }

    std::string Describe(const Swapchain& chain) const {
        std::ostringstream stream;
        stream << "mode="
               << (chain.mode == Mode::kTwin ? "twin" : chain.mode == Mode::kResolve ? "resolve" : "private-depth")
               << ", images=" << chain.images.size() << ", format=" << static_cast<int>(chain.format)
               << ", storage=" << static_cast<int>(chain.storage) << ", size=" << chain.width << "x" << chain.height
               << ", array=" << chain.array_size << ", mips=" << chain.mip_count << ", samples=" << chain.sample_count
               << ", twinMiB=" << MiB(chain.twin_bytes) << ", composed=" << (chain.composed ? 1 : 0)
               << ", copies=" << chain.copies << ", doneWaits=" << chain.done_waits
               << ", skippedCopies=" << chain.skipped_copies << ", unmatchedReleases=" << chain.unmatched_releases;
        return stream.str();
    }

    // Records the twin's copy into the runtime's image. The runtime's colour
    // images are render targets whenever the game holds them; the twin, with
    // simultaneous access, needs no transition of its own.
    bool RecordCopy(Image& image, Copy& copy, std::string& reason) {
        HRESULT hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator));
        if (SUCCEEDED(hr)) {
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&copy.list));
        }
        if (FAILED(hr)) {
            reason = Failure("Creating the bridge's copy commands", hr);
            return false;
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
    Copy* ReadyCopy(Image& image) {
        const UINT64 completed = done->GetCompletedValue();
        for (Copy& copy : image.copies) {
            if (copy.done <= completed) {
                return &copy;
            }
        }
        if (image.copies.size() < kCopyRecordings) {
            std::string reason;
            image.copies.emplace_back();
            if (RecordCopy(image, image.copies.back(), reason)) {
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
        return RecordCopy(image, image.copies.back(), reason);
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
        if (chain.mode == Mode::kPrivate) {
            desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (info.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        } else {
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }
        const HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &image.private11);
        if (FAILED(hr)) {
            reason = Failure(chain.mode == Mode::kPrivate ? "Creating a private depth texture"
                                                          : "Creating a private multisampled texture",
                             hr);
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
            for (Impl::Image& image : chain.images) {
                image.twin11.Detach();
                image.private11.Detach();
            }
        };
        for (auto& [swapchain, chain] : impl_->swapchains) {
            detach(chain);
        }
        for (Impl::Swapchain& chain : impl_->retired) {
            detach(chain);
        }
        impl_->queue.Detach();
        impl_->written.Detach();
        impl_->done.Detach();
        impl_->idle.Detach();
        impl_->written11.Detach();
        impl_->done11.Detach();
        impl_->adapter.Detach();
        impl_->device12.Detach();
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
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(immediate.As(&multithread))) {
        impl->multithread_protected = multithread->GetMultithreadProtected() != FALSE;
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
    chain.mode = depth ? Impl::Mode::kPrivate : chain.sample_count > 1 ? Impl::Mode::kResolve : Impl::Mode::kTwin;
    chain.storage = depth ? DepthStorageFormat(chain.format) : ColorStorageFormat(chain.format);

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
        if (chain.mode != Impl::Mode::kPrivate && !impl_->MakeTwin(chain, image, unordered_access, reason)) {
            reason = "image " + std::to_string(i) + ": " + reason;
            return false;
        }
        if (chain.mode != Impl::Mode::kTwin && !impl_->MakePrivate(chain, image, info, reason)) {
            reason = "image " + std::to_string(i) + ": " + reason;
            return false;
        }
    }
    std::scoped_lock lock(impl_->mutex);
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
    std::scoped_lock lock(impl_->mutex);
    Impl::Swapchain* chain = impl_->Find(swapchain);
    if (!chain) {
        return;
    }
    chain->acquired.push_back(index);
    if (chain->mode == Impl::Mode::kPrivate || index >= chain->images.size()) {
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
    impl_->context11->Wait(impl_->done11.Get(), value);
    ++chain->done_waits;
    ++impl_->done_waits;
}

bool D3D11Bridge::Released(XrSwapchain swapchain) {
    if (!impl_) {
        return true;
    }
    std::scoped_lock lock(impl_->mutex);
    Impl::Swapchain* chain = impl_->Find(swapchain);
    if (!chain) {
        return true;
    }
    ++impl_->releases;
    if (chain->acquired.empty()) {
        ++chain->unmatched_releases;
        ++impl_->unmatched_releases;
        return true;
    }
    const std::uint32_t index = chain->acquired.front();
    chain->acquired.pop_front();
    if (chain->mode == Impl::Mode::kPrivate || index >= chain->images.size()) {
        return true;
    }
    impl_->NoteThread();
    Impl::Image& image = chain->images[index];
    if (chain->mode == Impl::Mode::kResolve) {
        const DXGI_FORMAT format = ResolveFormat(chain->format);
        for (UINT slice = 0; slice < chain->array_size; ++slice) {
            const UINT subresource = D3D11CalcSubresource(0, slice, 1);
            impl_->context11->ResolveSubresource(image.twin11.Get(), subresource, image.private11.Get(),
                                                 subresource, format);
        }
    }
    // The game's rendering into the image is handed to the bridge's queue...
    const auto handover = std::chrono::steady_clock::now();
    const UINT64 written = ++impl_->written_value;
    impl_->context11->Signal(impl_->written11.Get(), written);
    impl_->context11->Flush();
    Impl::Copy* copy = impl_->ReadyCopy(image);
    if (!copy) {
        impl_->NoteHandover(handover);
        // Every recording of the copy is still queued: the bridge's queue is
        // stalled. The runtime's image keeps what it had.
        ++chain->skipped_copies;
        ++impl_->skipped_copies;
        if (impl_->skipped_copies == impl_->next_skip_report && impl_->log) {
            impl_->next_skip_report *= 10;
            impl_->log("Direct3D 11 bridge: its queue is stalled; releases not copied into the runtime's images so "
                       "far: " + std::to_string(impl_->skipped_copies) + ".");
        }
        return false;
    }
    // ...which copies it into the runtime's image, ahead of anything else
    // queued on it for the release (Frame Generation's capture).
    impl_->queue->Wait(impl_->written.Get(), written);
    ID3D12CommandList* lists[] = {copy->list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    const UINT64 done = ++impl_->done_value;
    impl_->queue->Signal(impl_->done.Get(), done);
    copy->done = done;
    image.done = done;
    chain->last_done = done;
    ++chain->copies;
    ++impl_->copies;
    impl_->NoteHandover(handover);
    return true;
}

void D3D11Bridge::ReleaseCalled() {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->NoteThread();
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
