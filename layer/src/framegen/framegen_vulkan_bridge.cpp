#include <windows.h>
#include <d3d11.h> // for openxr_platform.h's Direct3D 11 types only
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_vulkan_bridge.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>

// The Vulkan bridge's GPU parts: the Direct3D 12 device, queue and shared
// fences, the twins imported into the game's Vulkan device, the submissions on
// the game's queue and the copies on the bridge's. Its pure parts are in
// framegen_vulkan_formats.cpp.

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

// How long retiring waits for either queue, at teardown and when a swapchain
// is destroyed.
constexpr DWORD kRetireWaitMs = 2000;
// Recordings of an image's copy kept at most: a recording is executed again
// only once its last execution is done (D3D12 forbids executing a command
// list still pending), and each image needs another only while its earlier
// copies are still queued.
constexpr std::size_t kCopyRecordings = 4;
// How long a release waits for a recording when all of them are queued
// still: the bridge's queue is that far behind only when stalled.
constexpr DWORD kRecordingWaitMs = 100;
// Fences for the submissions on the game's queue: made at Begin, and more on
// demand (a fence is reused only once it signalled, never waited for), up to
// the cap; past it a submission goes without one (a later fenced submission
// covers it).
constexpr std::uint32_t kInitialFences = 16;
constexpr std::uint32_t kMaxFences = 64;
// Timing samples kept for the summary's percentiles.
constexpr std::size_t kTimingSamples = 1024;

std::string Hex(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string Failure(const char* operation, HRESULT hr) {
    return std::string(operation) + " failed (hr=" + Hex(static_cast<std::uint32_t>(hr)) + ")";
}

std::string VkFailure(const char* operation, VkResult result) {
    return std::string(operation) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
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

std::string VersionText(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

bool IsDepthFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

bool HasStencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// The UNORM member of an sRGB format (VK_FORMAT_UNDEFINED for others).
VkFormat UnormOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

// A DXGI format's typeless family; depth formats belong to the family of
// their typeless storage. The runtime's images and the twins must share one.
DXGI_FORMAT Family(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        // Formats without a family (R11G11B10_FLOAT) are their own.
        return format;
    }
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

std::uint64_t MiB(std::uint64_t bytes) {
    return (bytes + (1u << 19)) >> 20;
}

// Reports at 1, 10, 100...
bool ReportNow(std::uint64_t count, std::uint64_t& next) {
    if (count != next) {
        return false;
    }
    next *= 10;
    return true;
}

// The value at `fraction` (0..1) of `samples`, sorted.
double Percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t at = std::min(samples.size() - 1, static_cast<std::size_t>(fraction * samples.size()));
    return samples[at];
}

void AddSample(std::vector<double>& samples, std::size_t& next, double value) {
    if (samples.size() < kTimingSamples) {
        samples.push_back(value);
    } else {
        samples[next] = value;
        next = (next + 1) % kTimingSamples;
    }
}

// Every Vulkan function the bridge calls. Core 1.1 functions are resolved by
// their KHR names first (a Vulkan 1.0 game has them through the extensions
// the bridge asks for), then by their core names.
#define VB_INSTANCE_FUNCTIONS(X)                                                                                   \
    X(vkGetDeviceProcAddr)                                                                                         \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
    X(vkGetPhysicalDeviceImageFormatProperties)
#define VB_INSTANCE_FUNCTIONS_11(X)                                                                                \
    X(vkGetPhysicalDeviceProperties2)                                                                              \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)                                                              \
    X(vkGetPhysicalDeviceImageFormatProperties2)
#define VB_DEVICE_EXTENSION_FUNCTIONS(X)                                                                           \
    X(vkGetMemoryWin32HandlePropertiesKHR)                                                                         \
    X(vkImportSemaphoreWin32HandleKHR)
#define VB_DEVICE_FUNCTIONS(X)                                                                                     \
    X(vkGetDeviceQueue)                                                                                            \
    X(vkCreateImage)                                                                                               \
    X(vkDestroyImage)                                                                                              \
    X(vkGetImageMemoryRequirements)                                                                                \
    X(vkAllocateMemory)                                                                                            \
    X(vkFreeMemory)                                                                                                \
    X(vkBindImageMemory)                                                                                           \
    X(vkCreateSemaphore)                                                                                           \
    X(vkDestroySemaphore)                                                                                          \
    X(vkCreateCommandPool)                                                                                         \
    X(vkDestroyCommandPool)                                                                                        \
    X(vkAllocateCommandBuffers)                                                                                    \
    X(vkBeginCommandBuffer)                                                                                        \
    X(vkEndCommandBuffer)                                                                                          \
    X(vkCmdPipelineBarrier)                                                                                        \
    X(vkCmdResolveImage)                                                                                           \
    X(vkQueueSubmit)                                                                                               \
    X(vkCreateFence)                                                                                               \
    X(vkDestroyFence)                                                                                              \
    X(vkGetFenceStatus)                                                                                            \
    X(vkResetFences)
#define VB_DEVICE_FUNCTIONS_11(X) X(vkGetImageMemoryRequirements2)

struct Vulkan {
#define VB_DECLARE(name) PFN_##name name{nullptr};
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr{nullptr};
    VB_INSTANCE_FUNCTIONS(VB_DECLARE)
    VB_INSTANCE_FUNCTIONS_11(VB_DECLARE)
    VB_DEVICE_EXTENSION_FUNCTIONS(VB_DECLARE)
    VB_DEVICE_FUNCTIONS(VB_DECLARE)
    VB_DEVICE_FUNCTIONS_11(VB_DECLARE)
#undef VB_DECLARE
};

// XR_KHR_vulkan_enable's usage table.
VkImageUsageFlags VulkanUsage(XrSwapchainUsageFlags usage) {
    VkImageUsageFlags result = 0;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) result |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) result |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) result |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return result;
}

// What the twin's own copies and the bridge's barriers need of every image,
// whatever the application asked: the attachment layouts OpenXR hands images
// out in are valid only with the attachment bit.
constexpr VkImageUsageFlags kColorUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kDepthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_SAMPLED_BIT;

} // namespace

struct VulkanBridge::Impl {
    enum class Mode {
        kTwin,      // colour, rendered into the twin
        kTwinDepth, // depth, rendered into the twin
        kResolve,   // multisampled colour: rendered privately, resolved into the twin at release
        kPrivate,   // depth the runtime never gets: rendered privately
    };
    // Who has an image the game renders into, as the bridge last submitted it.
    enum class State {
        kUndefined, // never handed to the game (or its content no longer matters)
        kGame,      // the game's queue family has it, in the attachment layout
        kExternal,  // released to Direct3D 12
    };
    // A recording of the copy of a twin into the runtime's image.
    struct Copy {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 done{0};          // the done value after its last execution
        std::uint32_t query{0};  // its two timestamps' first index
        bool timed{false};       // executed, its timestamps not read yet
    };
    struct Image {
        ComPtr<ID3D12Resource> twin;
        // The runtime's image, referenced: a copy still queued when the
        // runtime lets the image go writes memory that still exists.
        ComPtr<ID3D12Resource> runtime;
        VkImage twin_image{VK_NULL_HANDLE};
        VkDeviceMemory twin_memory{VK_NULL_HANDLE};
        VkImage private_image{VK_NULL_HANDLE}; // rendered into instead of the twin
        VkDeviceMemory private_memory{VK_NULL_HANDLE};
        VkCommandBuffer first_acquire{VK_NULL_HANDLE};
        VkCommandBuffer acquire{VK_NULL_HANDLE}; // none for kResolve: its acquire only waits
        VkCommandBuffer release{VK_NULL_HANDLE};
        std::vector<Copy> copies;
        UINT64 done{0}; // the done value after its last copy
        State state{State::kUndefined};
    };
    // What a swapchain's images are, from the application's create info.
    struct Plan {
        Mode mode{Mode::kTwin};
        VkFormat format{VK_FORMAT_UNDEFINED};
        DXGI_FORMAT dxgi{DXGI_FORMAT_UNKNOWN};    // the runtime's
        DXGI_FORMAT storage{DXGI_FORMAT_UNKNOWN}; // the Direct3D 12 twin's
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t array_size{1};
        std::uint32_t mip_count{1};
        std::uint32_t sample_count{1};
        XrSwapchainUsageFlags xr_usage{0};
        VkImageCreateFlags flags{0}; // of the image the game renders into
        VkImageUsageFlags usage{0};
        std::vector<VkFormat> view_formats; // chained when not empty
        bool unordered_access{false};
        VkImageAspectFlags aspect{VK_IMAGE_ASPECT_COLOR_BIT};
        VkImageLayout layout{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; // the attachment layout
    };
    struct Swapchain {
        Plan plan;
        std::string private_reason; // why depth stays private
        VkCommandPool pool{VK_NULL_HANDLE};
        ComPtr<ID3D12QueryHeap> timestamps;
        ComPtr<ID3D12Resource> stamps; // readback of the timestamps, mapped
        const UINT64* stamp_data{nullptr};
        std::vector<Image> images;
        std::deque<std::uint32_t> acquired; // acquired and not released yet, oldest first
        UINT64 last_done{0};
        std::uint64_t last_serial{0}; // the newest submission naming its images
        std::uint64_t twin_bytes{0};
        std::uint64_t copies{0};
        std::uint64_t done_waits{0};         // acquires whose image's copy had not finished (the game's queue waits)
        std::uint64_t skipped_copies{0};     // releases not copied: the bridge's queue was stalled
        std::uint64_t unmatched_releases{0}; // releases with nothing acquired
        std::uint64_t failed_submits{0};
    };
    struct Fence {
        VkFence fence{VK_NULL_HANDLE};
        std::uint64_t serial{0}; // the submission it signals after
    };
    // What a format can be imported as (Begin's table).
    struct Importable {
        bool plain{false};
        bool mutable_format{false};
        VkImageFormatProperties properties{};
    };

    Vulkan vk;
    HMODULE loader{nullptr}; // vulkan-1.dll, when the bridge loaded it
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue game_queue{VK_NULL_HANDLE};
    std::uint32_t family{0};
    std::uint32_t queue_index{0};
    std::uint32_t api_version{0}; // the physical device's
    // The game's device is known to have Vulkan 1.2 (format lists, sRGB
    // images with storage views). Never so far: the game's API version is not
    // known (its VkApplicationInfo is not passed to the bridge), and a
    // driver's vkGetDeviceProcAddr may return 1.2 functions to a 1.0 device.
    bool api12{false};
    bool timeline{false};         // the shared fences are timeline semaphores
    VkPhysicalDeviceMemoryProperties memory{};
    std::unordered_map<std::int64_t, Importable> importable;

    ComPtr<ID3D12Device> device12;
    ComPtr<IDXGIAdapter3> adapter; // for video memory figures
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> written; // signalled by the game's queue only (or the CPU, at teardown and loss)
    ComPtr<ID3D12Fence> done;    // signalled by the bridge's queue only
    ComPtr<ID3D12Fence> idle;    // the bridge's queue's progress, for retiring
    VkSemaphore written_semaphore{VK_NULL_HANDLE};
    VkSemaphore done_semaphore{VK_NULL_HANDLE};
    UINT64 timestamp_frequency{1};

    // Everything below, the game's queue (inside the application's calls) and
    // the order of signals on both APIs: each fence's values are signalled in
    // the order they are handed out.
    mutable std::mutex mutex;
    UINT64 written_value{0};
    UINT64 done_value{0};
    UINT64 idle_value{0};
    std::deque<Fence> in_flight; // oldest first
    std::vector<VkFence> free_fences;
    std::uint32_t fence_count{0};
    std::uint64_t submit_serial{0};    // the last serial handed out
    std::uint64_t last_serial{0};      // the newest submission that was made
    std::uint64_t completed_serial{0}; // every submission up to it has finished
    bool lost{false};                  // the game's Vulkan device was lost
    std::string lost_reason;
    DWORD thread{0}; // the first thread a swapchain or frame call came from
    bool reported_second_thread{false};
    bool debug_layer{false};          // VECTORXR_VULKAN_BRIDGE_DEBUG=1
    std::uint32_t debug_reported{0};  // debug-layer messages reported
    std::uint64_t next_skip_report{1}; // skipped copies are reported at 1, 10, 100...
    std::uint64_t next_failure_report{1};
    std::uint64_t next_unfenced_report{1};
    std::unordered_map<XrSwapchain, Swapchain> swapchains;
    std::vector<Swapchain> retired; // removed while either queue may still have used them
    std::uint32_t swapchains_added{0};
    std::uint64_t releases{0};
    std::uint64_t copies{0};
    std::uint64_t skipped_copies{0};
    std::uint64_t recordings{0};
    std::uint64_t done_waits{0};
    std::uint64_t unmatched_releases{0};
    std::uint64_t submits{0};
    std::uint64_t failed_submits{0};
    std::uint64_t fence_growth{0};
    std::uint64_t unfenced_submits{0};
    std::vector<double> submit_us;
    std::size_t next_submit_sample{0};
    std::vector<double> copy_gpu_us;
    std::size_t next_copy_sample{0};
    std::function<void(const std::string&)> log;
    std::function<void(const std::string&)> info_log;

    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};

    void Log(const std::string& message) const {
        if (log) {
            log("Vulkan bridge: " + message);
        }
    }

    // Only worth knowing, not a problem (VulkanBridge::SetInfoLog).
    void Info(const std::string& message) const {
        if (info_log) {
            info_log("Vulkan bridge: " + message);
        } else {
            Log(message);
        }
    }

    // A swapchain or frame call's thread (holding `mutex`). The bridge uses
    // the game's queue only inside the application's own calls, which the
    // game synchronises with its other uses of the queue, from whichever
    // thread; a second thread is only reported.
    void NoteThread() {
        const DWORD current = GetCurrentThreadId();
        if (!thread) {
            thread = current;
            return;
        }
        if (current == thread || reported_second_thread) {
            return;
        }
        reported_second_thread = true;
        Info("the game's swapchain and frame calls come from more than one thread (" + std::to_string(current) +
            ", first " + std::to_string(thread) + "); the bridge submits to the game's queue only inside them.");
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
                Log("the Direct3D 12 debug layer reports [" + std::to_string(message->ID) + "] " +
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

    // The game's device was lost (holding `mutex`): it will never signal
    // "written" again, so the bridge's queue is let through its waits.
    void MarkLost(const std::string& reason) {
        if (lost) {
            return;
        }
        lost = true;
        lost_reason = reason;
        if (written) {
            written->Signal(written_value);
        }
        Log("the game's Vulkan device was lost (" + reason + "); the session is lost.");
    }

    // Moves the fences that signalled, oldest first, back to the free list
    // (holding `mutex`). Signals on one queue are in submission order, so the
    // first one still pending ends the walk.
    void PollFences() {
        while (!in_flight.empty()) {
            const Fence fence = in_flight.front();
            const VkResult status = vk.vkGetFenceStatus(device, fence.fence);
            if (status == VK_NOT_READY) {
                return;
            }
            if (status == VK_ERROR_DEVICE_LOST) {
                // A lost device's fences count as complete.
                MarkLost("vkGetFenceStatus returned VK_ERROR_DEVICE_LOST");
            } else if (status != VK_SUCCESS) {
                return;
            } else if (vk.vkResetFences(device, 1, &fence.fence) == VK_ERROR_DEVICE_LOST) {
                MarkLost("vkResetFences returned VK_ERROR_DEVICE_LOST");
            }
            completed_serial = std::max(completed_serial, fence.serial);
            free_fences.push_back(fence.fence);
            in_flight.pop_front();
        }
    }

    // A fence for the next submission (holding `mutex`): one that signalled,
    // else a new one up to the cap, else none.
    VkFence TakeFence() {
        PollFences();
        if (!free_fences.empty()) {
            const VkFence fence = free_fences.back();
            free_fences.pop_back();
            return fence;
        }
        if (fence_count < kMaxFences) {
            VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE;
            if (vk.vkCreateFence(device, &create, nullptr, &fence) == VK_SUCCESS) {
                ++fence_count;
                if (fence_count > kInitialFences && ++fence_growth == 1) {
                    Log("the game's queue is behind by more than " + std::to_string(kInitialFences) +
                        " of the bridge's submissions; more fences are made for them (up to " +
                        std::to_string(kMaxFences) + ").");
                }
                return fence;
            }
        }
        ++unfenced_submits;
        if (ReportNow(unfenced_submits, next_unfenced_report)) {
            Log("submissions made without a fence of their own (the game's queue is far behind) so far: " +
                std::to_string(unfenced_submits) + ".");
        }
        return VK_NULL_HANDLE;
    }

    // Submits `commands` (if any) to the game's queue (holding `mutex`),
    // waiting (on the GPU) for "done" to reach `wait_value` and signalling
    // "written" with `signal_value` when non-zero. `serial` is the
    // submission's, when it was made.
    VkResult Submit(VkCommandBuffer commands, UINT64 wait_value, UINT64 signal_value, std::uint64_t& serial) {
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = wait_value ? 1u : 0u;
        submit.pWaitSemaphores = &done_semaphore;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = commands ? 1u : 0u;
        submit.pCommandBuffers = &commands;
        submit.signalSemaphoreCount = signal_value ? 1u : 0u;
        submit.pSignalSemaphores = &written_semaphore;
        VkTimelineSemaphoreSubmitInfo timeline_values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        VkD3D12FenceSubmitInfoKHR fence_values{VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR};
        if (timeline) {
            timeline_values.waitSemaphoreValueCount = submit.waitSemaphoreCount;
            timeline_values.pWaitSemaphoreValues = &wait_value;
            timeline_values.signalSemaphoreValueCount = submit.signalSemaphoreCount;
            timeline_values.pSignalSemaphoreValues = &signal_value;
            submit.pNext = &timeline_values;
        } else {
            fence_values.waitSemaphoreValuesCount = submit.waitSemaphoreCount;
            fence_values.pWaitSemaphoreValues = &wait_value;
            fence_values.signalSemaphoreValuesCount = submit.signalSemaphoreCount;
            fence_values.pSignalSemaphoreValues = &signal_value;
            submit.pNext = &fence_values;
        }
        const VkFence fence = TakeFence();
        const auto start = std::chrono::steady_clock::now();
        const VkResult result = vk.vkQueueSubmit(game_queue, 1, &submit, fence);
        AddSample(submit_us, next_submit_sample,
                  std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
        if (result != VK_SUCCESS) {
            if (fence) {
                free_fences.push_back(fence);
            }
            ++failed_submits;
            if (result == VK_ERROR_DEVICE_LOST) {
                MarkLost("vkQueueSubmit returned VK_ERROR_DEVICE_LOST");
            } else if (ReportNow(failed_submits, next_failure_report)) {
                Log("the game's queue refused the bridge's submission (VkResult " +
                    std::to_string(static_cast<int>(result)) + "); refused so far: " + std::to_string(failed_submits) +
                    ".");
            }
            return result;
        }
        serial = ++submit_serial;
        last_serial = serial;
        ++submits;
        if (fence) {
            in_flight.push_back({fence, serial});
        }
        return VK_SUCCESS;
    }

    // Waits (bounded) until the game's queue finished every submission up to
    // `target`: by polling the fences (never waiting on one another thread
    // may reset). False when none covers it or the time ran out.
    bool WaitSubmissions(std::uint64_t target, DWORD timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::scoped_lock lock(mutex);
                if (!device) {
                    return true;
                }
                PollFences();
                if (lost || completed_serial >= target) {
                    return true;
                }
                const bool covered = std::any_of(in_flight.begin(), in_flight.end(),
                                                 [&](const Fence& fence) { return fence.serial >= target; });
                if (!covered) {
                    return false;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            Sleep(1);
        }
    }

    // The timestamps of copies whose last execution finished (holding `mutex`).
    void ReadCopyTimes(Swapchain& chain, Image& image, UINT64 completed) {
        for (Copy& copy : image.copies) {
            if (!copy.timed || copy.done > completed || !chain.stamp_data) {
                continue;
            }
            copy.timed = false;
            const UINT64 begin = chain.stamp_data[copy.query];
            const UINT64 end = chain.stamp_data[copy.query + 1];
            if (end >= begin && timestamp_frequency) {
                AddSample(copy_gpu_us, next_copy_sample,
                          static_cast<double>(end - begin) * 1e6 / static_cast<double>(timestamp_frequency));
            }
        }
    }

    void ReadAllCopyTimes() {
        const UINT64 completed = done ? done->GetCompletedValue() : 0;
        for (auto& [swapchain, chain] : swapchains) {
            for (Image& image : chain.images) {
                ReadCopyTimes(chain, image, completed);
            }
        }
    }

    // Lets a swapchain's Vulkan objects go (nothing may use them any more).
    void DestroyVulkan(Swapchain& chain) {
        for (Image& image : chain.images) {
            if (image.twin_image) vk.vkDestroyImage(device, image.twin_image, nullptr);
            if (image.twin_memory) vk.vkFreeMemory(device, image.twin_memory, nullptr);
            if (image.private_image) vk.vkDestroyImage(device, image.private_image, nullptr);
            if (image.private_memory) vk.vkFreeMemory(device, image.private_memory, nullptr);
            image.twin_image = VK_NULL_HANDLE;
            image.twin_memory = VK_NULL_HANDLE;
            image.private_image = VK_NULL_HANDLE;
            image.private_memory = VK_NULL_HANDLE;
            image.first_acquire = image.acquire = image.release = VK_NULL_HANDLE;
        }
        if (chain.pool) {
            vk.vkDestroyCommandPool(device, chain.pool, nullptr); // frees its command buffers
            chain.pool = VK_NULL_HANDLE;
        }
    }

    // The session's own Vulkan objects (nothing may use them any more).
    void DestroySessionVulkan() {
        for (const Fence& fence : in_flight) {
            vk.vkDestroyFence(device, fence.fence, nullptr);
        }
        for (const VkFence fence : free_fences) {
            vk.vkDestroyFence(device, fence, nullptr);
        }
        in_flight.clear();
        free_fences.clear();
        fence_count = 0;
        if (written_semaphore) vk.vkDestroySemaphore(device, written_semaphore, nullptr);
        if (done_semaphore) vk.vkDestroySemaphore(device, done_semaphore, nullptr);
        written_semaphore = VK_NULL_HANDLE;
        done_semaphore = VK_NULL_HANDLE;
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
        chain.timestamps.Detach();
        chain.stamps.Detach();
    }

    std::string VideoMemory() const {
        DXGI_QUERY_VIDEO_MEMORY_INFO video{};
        if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &video))) {
            return "unavailable";
        }
        return std::to_string(MiB(video.CurrentUsage)) + "/" + std::to_string(MiB(video.Budget)) + " MiB";
    }

    static const char* ModeName(Mode mode) {
        switch (mode) {
        case Mode::kTwin:
            return "twin";
        case Mode::kTwinDepth:
            return "twin-depth";
        case Mode::kResolve:
            return "resolve";
        default:
            return "private-depth";
        }
    }

    std::string Describe(const Swapchain& chain) const {
        const Plan& plan = chain.plan;
        std::ostringstream stream;
        stream << "mode=" << ModeName(plan.mode) << ", images=" << chain.images.size()
               << ", format=" << static_cast<int>(plan.format) << ", dxgi=" << static_cast<int>(plan.dxgi)
               << ", storage=" << static_cast<int>(plan.storage) << ", size=" << plan.width << "x" << plan.height
               << ", array=" << plan.array_size << ", mips=" << plan.mip_count << ", samples=" << plan.sample_count
               << ", usage=" << Hex(plan.usage) << ", flags=" << Hex(plan.flags)
               << ", viewFormats=" << plan.view_formats.size() << ", twinMiB=" << MiB(chain.twin_bytes)
               << ", copies=" << chain.copies << ", doneWaits=" << chain.done_waits
               << ", skippedCopies=" << chain.skipped_copies << ", unmatchedReleases=" << chain.unmatched_releases
               << ", failedSubmits=" << chain.failed_submits << ", semaphores=" << (timeline ? "timeline" : "binary");
        if (!chain.private_reason.empty()) {
            stream << ", private because " << chain.private_reason;
        }
        return stream.str();
    }

    // Whether the game's device can import a Direct3D 12 image so made.
    bool QueryImport(VkFormat format, VkImageCreateFlags flags, VkImageUsageFlags usage,
                     const std::vector<VkFormat>& view_formats, VkImageFormatProperties& properties) const {
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        if (!view_formats.empty()) {
            list.viewFormatCount = static_cast<std::uint32_t>(view_formats.size());
            list.pViewFormats = view_formats.data();
            external.pNext = &list;
        }
        VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.pNext = &external;
        query.format = format;
        query.type = VK_IMAGE_TYPE_2D;
        query.tiling = VK_IMAGE_TILING_OPTIMAL;
        query.usage = usage;
        query.flags = flags;
        VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        result.pNext = &external_properties;
        if (vk.vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &result) != VK_SUCCESS ||
            !(external_properties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            return false;
        }
        properties = result.imageFormatProperties;
        return true;
    }

    // What the images of a swapchain made from `info` are, and whether the
    // game's device can make them. False with the application's result and
    // `reason`.
    bool MakePlan(const XrSwapchainCreateInfo& info, Plan& plan, XrResult& refusal, std::string& reason) const {
        reason.clear();
        refusal = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        plan = {};
        plan.format = static_cast<VkFormat>(info.format);
        plan.dxgi = static_cast<DXGI_FORMAT>(RuntimeFormat(info.format));
        if (plan.dxgi == DXGI_FORMAT_UNKNOWN) {
            reason = "VkFormat " + std::to_string(info.format) + " has no Direct3D 12 equivalent";
            return false;
        }
        const auto table = importable.find(info.format);
        if (table == importable.end() || !table->second.plain) {
            reason = "the game's Vulkan device cannot import VkFormat " + std::to_string(info.format) +
                     " from Direct3D 12";
            return false;
        }
        refusal = XR_ERROR_FEATURE_UNSUPPORTED;
        const bool depth = IsDepthFormat(plan.format);
        if (!depth && (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            reason = "depth-stencil usage on colour VkFormat " + std::to_string(info.format);
            return false;
        }
        if (info.faceCount > 1) {
            reason = "cube swapchains are not bridged";
            return false;
        }
        plan.width = info.width;
        plan.height = info.height;
        plan.array_size = std::max(1u, info.arraySize);
        plan.mip_count = std::max(1u, info.mipCount);
        plan.sample_count = std::max(1u, info.sampleCount);
        plan.xr_usage = info.usageFlags;
        // Vulkan sample counts are single bits, and multisampled images have
        // one mip.
        if ((plan.sample_count & (plan.sample_count - 1)) != 0) {
            reason = std::to_string(plan.sample_count) + " samples is not a Vulkan sample count";
            return false;
        }
        if (plan.sample_count > 1 && plan.mip_count > 1) {
            reason = "multisampled images cannot have mips";
            return false;
        }
        // Measured on NVIDIA: an imported image that is an array and has mips
        // lays out its later layers' mips unlike Direct3D 12, and a mipmapped
        // depth-stencil image its stencil (they would reach the runtime
        // wrong). Neither is a usual swapchain.
        if (plan.array_size > 1 && plan.mip_count > 1) {
            reason = "arrays of mipmapped images are not bridged";
            return false;
        }
        if (HasStencil(plan.format) && plan.mip_count > 1) {
            reason = "mipmapped depth-stencil images are not bridged";
            return false;
        }

        VkImageUsageFlags usage = VulkanUsage(info.usageFlags);
        VkImageCreateFlags flags = (info.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT)
                                       ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT}
                                       : VkImageCreateFlags{0};
        std::vector<VkFormat> listed;
        for (auto* next = static_cast<const XrBaseInStructure*>(info.next); next; next = next->next) {
            if (next->type == XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR) {
                const auto& list = *reinterpret_cast<const XrVulkanSwapchainFormatListCreateInfoKHR*>(next);
                for (std::uint32_t i = 0; i < list.viewFormatCount && list.viewFormats; ++i) {
                    listed.push_back(list.viewFormats[i]);
                }
            } else if (next->type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META) {
                const auto& meta = *reinterpret_cast<const XrVulkanSwapchainCreateInfoMETA*>(next);
                constexpr VkImageCreateFlags kAllowed =
                    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
                if (meta.additionalCreateFlags & ~kAllowed) {
                    reason = "additional VkImageCreateFlags " + Hex(meta.additionalCreateFlags) +
                             " are not bridged";
                    return false;
                }
                flags |= meta.additionalCreateFlags;
                usage |= meta.additionalUsageFlags;
            }
        }
        plan.unordered_access = (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
        if (plan.unordered_access && depth) {
            reason = "storage usage on a depth swapchain";
            return false;
        }
        const VkFormat unorm = UnormOf(plan.format);
        if (plan.unordered_access && unorm != VK_FORMAT_UNDEFINED) {
            // sRGB images have no storage views: the game's are UNORM ones.
            if (!api12) {
                reason = "storage usage on an sRGB swapchain needs Vulkan 1.2 on the game's device";
                return false;
            }
            flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            listed.push_back(plan.format);
            listed.push_back(unorm);
        }
        // Format lists are chained only where the game's device has them.
        if (api12 && (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
            for (const VkFormat format : listed) {
                if (std::find(plan.view_formats.begin(), plan.view_formats.end(), format) == plan.view_formats.end()) {
                    plan.view_formats.push_back(format);
                }
            }
        }
        plan.flags = flags;
        plan.usage = usage | (depth ? kDepthUsage : kColorUsage);
        if (depth) {
            plan.mode = plan.sample_count > 1 ? Mode::kPrivate : Mode::kTwinDepth;
            plan.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(plan.format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            plan.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            plan.storage = plan.dxgi;
        } else {
            plan.mode = plan.sample_count > 1 ? Mode::kResolve : Mode::kTwin;
            // Mutable and unordered-access twins are typeless, as runtimes'
            // images usually are; the others are typed.
            const bool typeless =
                plan.mode == Mode::kTwin && ((flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || plan.unordered_access);
            plan.storage = typeless ? Family(plan.dxgi) : plan.dxgi;
        }

        const auto fits = [&](const VkImageFormatProperties& properties, std::uint32_t mips) {
            return plan.width <= properties.maxExtent.width && plan.height <= properties.maxExtent.height &&
                   plan.array_size <= properties.maxArrayLayers && mips <= properties.maxMipLevels;
        };
        VkImageFormatProperties properties{};
        if (plan.mode == Mode::kTwin || plan.mode == Mode::kTwinDepth) {
            if (!QueryImport(plan.format, plan.flags, plan.usage, plan.view_formats, properties)) {
                reason = "the game's Vulkan device cannot import its images (VkFormat " +
                         std::to_string(info.format) + ", usage " + Hex(plan.usage) + ", flags " + Hex(plan.flags) +
                         ")";
                return false;
            }
            if (!fits(properties, plan.mip_count)) {
                reason = "its size, layers or mips exceed what the game's Vulkan device imports";
                return false;
            }
            return true;
        }
        if (plan.mode == Mode::kResolve) {
            if (!QueryImport(plan.format, 0, kColorUsage, {}, properties) || !fits(properties, 1)) {
                reason = "the game's Vulkan device cannot import its resolved images";
                return false;
            }
        }
        if (vk.vkGetPhysicalDeviceImageFormatProperties(physical, plan.format, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, plan.usage, plan.flags,
                                                         &properties) != VK_SUCCESS ||
            (properties.sampleCounts & plan.sample_count) != plan.sample_count || !fits(properties, plan.mip_count)) {
            reason = "the game's Vulkan device cannot make its " + std::to_string(plan.sample_count) +
                     "-sample images";
            return false;
        }
        return true;
    }

    bool MemoryType(std::uint32_t bits, std::uint32_t& type) const {
        for (const VkMemoryPropertyFlags wanted : {VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT},
                                                   VkMemoryPropertyFlags{0}}) {
            for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
                if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
                    type = i;
                    return true;
                }
            }
        }
        return false;
    }

    // A private image of the swapchain's (multisampled, or depth the runtime
    // never gets), in memory of the game's device.
    bool MakePrivate(const Plan& plan, Image& image, std::string& reason) {
        VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        if (!plan.view_formats.empty()) {
            list.viewFormatCount = static_cast<std::uint32_t>(plan.view_formats.size());
            list.pViewFormats = plan.view_formats.data();
            create.pNext = &list;
        }
        create.flags = plan.flags;
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = plan.format;
        create.extent = {plan.width, plan.height, 1};
        create.mipLevels = plan.mip_count;
        create.arrayLayers = plan.array_size;
        create.samples = static_cast<VkSampleCountFlagBits>(plan.sample_count);
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = plan.usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vk.vkCreateImage(device, &create, nullptr, &image.private_image);
        if (result != VK_SUCCESS) {
            reason = VkFailure("Creating a private image", result);
            return false;
        }
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(device, image.private_image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        if (!MemoryType(requirements.memoryTypeBits, allocate.memoryTypeIndex)) {
            reason = "no memory type for a private image";
            return false;
        }
        result = vk.vkAllocateMemory(device, &allocate, nullptr, &image.private_memory);
        if (result == VK_SUCCESS) {
            result = vk.vkBindImageMemory(device, image.private_image, image.private_memory, 0);
        }
        if (result != VK_SUCCESS) {
            reason = VkFailure("Allocating a private image's memory", result);
            return false;
        }
        return true;
    }

    // Checks the runtime's image against its twin.
    bool CheckRuntime(const Plan& plan, const Image& image, std::string& reason) const {
        if (!image.runtime) {
            reason = "the runtime's image is null";
            return false;
        }
        const std::uint32_t mips = plan.mode == Mode::kResolve ? 1 : plan.mip_count;
        const D3D12_RESOURCE_DESC runtime = image.runtime->GetDesc();
        if (runtime.Width != plan.width || runtime.Height != plan.height ||
            runtime.DepthOrArraySize != plan.array_size || runtime.MipLevels != mips ||
            runtime.SampleDesc.Count != 1 || Family(runtime.Format) != Family(plan.storage)) {
            std::ostringstream stream;
            stream << "the runtime's image (" << runtime.Width << "x" << runtime.Height << ", array "
                   << runtime.DepthOrArraySize << ", mips " << runtime.MipLevels << ", samples "
                   << runtime.SampleDesc.Count << ", format " << static_cast<int>(runtime.Format)
                   << ") does not match its twin (" << plan.width << "x" << plan.height << ", array "
                   << plan.array_size << ", mips " << mips << ", format " << static_cast<int>(plan.storage) << ")";
            reason = stream.str();
            return false;
        }
        // The copy leaves the runtime's colour image a render target, as it was handed out.
        if (plan.mode != Mode::kTwinDepth && (runtime.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
            reason = "the runtime's image is not a render target";
            return false;
        }
        return true;
    }

    // The Direct3D 12 twin, shared and imported into the game's device.
    bool MakeTwin(Swapchain& chain, Image& image, std::string& reason) {
        const Plan& plan = chain.plan;
        const bool resolve = plan.mode == Mode::kResolve;
        const bool depth = plan.mode == Mode::kTwinDepth;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = plan.width;
        desc.Height = plan.height;
        desc.DepthOrArraySize = static_cast<UINT16>(plan.array_size);
        desc.MipLevels = static_cast<UINT16>(resolve ? 1 : plan.mip_count);
        desc.Format = plan.storage;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (depth) {
            // Direct3D 12 allows no simultaneous access to depth: the copy
            // transitions the twin itself.
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        } else {
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            if (plan.unordered_access && !resolve) {
                desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
        }
        HRESULT hr = device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image.twin));
        if (FAILED(hr)) {
            reason = Failure("Creating a shared Direct3D 12 texture", hr);
            return false;
        }
        image.twin->SetName(L"VectorXR Vulkan bridge image");
        chain.twin_bytes += device12->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        HANDLE handle = nullptr;
        hr = device12->CreateSharedHandle(image.twin.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(hr)) {
            reason = Failure("Sharing a Direct3D 12 texture", hr);
            return false;
        }
        const bool imported = ImportTwin(plan, image, handle, desc.MipLevels, reason);
        CloseHandle(handle);
        return imported;
    }

    bool ImportTwin(const Plan& plan, Image& image, HANDLE handle, std::uint32_t mips, std::string& reason) {
        const bool resolve = plan.mode == Mode::kResolve;
        VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        if (!resolve && !plan.view_formats.empty()) {
            list.viewFormatCount = static_cast<std::uint32_t>(plan.view_formats.size());
            list.pViewFormats = plan.view_formats.data();
            external.pNext = &list;
        }
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.pNext = &external;
        create.flags = resolve ? 0 : plan.flags;
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = plan.format;
        create.extent = {plan.width, plan.height, 1};
        create.mipLevels = mips;
        create.arrayLayers = plan.array_size;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = resolve ? kColorUsage : plan.usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vk.vkCreateImage(device, &create, nullptr, &image.twin_image);
        if (result != VK_SUCCESS) {
            reason = VkFailure("Creating the Vulkan image of a shared texture", result);
            return false;
        }
        VkMemoryDedicatedRequirements dedicated_requirements{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated_requirements;
        VkImageMemoryRequirementsInfo2 requirements_info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        requirements_info.image = image.twin_image;
        vk.vkGetImageMemoryRequirements2(device, &requirements_info, &requirements);
        VkMemoryWin32HandlePropertiesKHR handle_properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        result = vk.vkGetMemoryWin32HandlePropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                                        handle, &handle_properties);
        if (result != VK_SUCCESS) {
            reason = VkFailure("vkGetMemoryWin32HandlePropertiesKHR", result);
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image.twin_image;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import;
        allocate.allocationSize = requirements.memoryRequirements.size;
        if (!MemoryType(requirements.memoryRequirements.memoryTypeBits & handle_properties.memoryTypeBits,
                        allocate.memoryTypeIndex)) {
            reason = "no memory type imports the shared texture";
            return false;
        }
        result = vk.vkAllocateMemory(device, &allocate, nullptr, &image.twin_memory);
        if (result == VK_SUCCESS) {
            result = vk.vkBindImageMemory(device, image.twin_image, image.twin_memory, 0);
        }
        if (result != VK_SUCCESS) {
            reason = VkFailure("Importing a shared texture's memory", result);
            return false;
        }
        return true;
    }

    // Records the twin's copy into the runtime's image. The runtime's colour
    // images are render targets whenever the game holds them, its depth images
    // depth targets; colour twins, with simultaneous access, need no
    // transition of their own, depth twins go to copy source and back.
    bool RecordCopy(Swapchain& chain, Image& image, Copy& copy, std::uint32_t query, std::string& reason) {
        HRESULT hr = device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copy.allocator));
        if (SUCCEEDED(hr)) {
            hr = device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&copy.list));
        }
        if (FAILED(hr)) {
            reason = Failure("Creating the bridge's copy commands", hr);
            return false;
        }
        const bool depth = chain.plan.mode == Mode::kTwinDepth;
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = image.runtime.Get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore =
            depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = image.twin.Get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const UINT count = depth ? 2 : 1;
        copy.list->ResourceBarrier(count, barriers);
        copy.query = query;
        if (chain.timestamps) {
            copy.list->EndQuery(chain.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
        }
        copy.list->CopyResource(image.runtime.Get(), image.twin.Get());
        if (chain.timestamps) {
            copy.list->EndQuery(chain.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
        }
        for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        }
        copy.list->ResourceBarrier(count, barriers);
        if (chain.timestamps) {
            copy.list->ResolveQueryData(chain.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 2,
                                        chain.stamps.Get(), query * sizeof(UINT64));
        }
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
    Copy* ReadyCopy(Swapchain& chain, Image& image, std::uint32_t index) {
        UINT64 completed = done->GetCompletedValue();
        ReadCopyTimes(chain, image, completed);
        for (Copy& copy : image.copies) {
            if (copy.done <= completed) {
                return &copy;
            }
        }
        if (image.copies.size() < kCopyRecordings) {
            std::string reason;
            const auto query = static_cast<std::uint32_t>((index * kCopyRecordings + image.copies.size()) * 2);
            image.copies.emplace_back();
            if (RecordCopy(chain, image, image.copies.back(), query, reason)) {
                return &image.copies.back();
            }
            image.copies.pop_back();
        }
        auto oldest = std::min_element(image.copies.begin(), image.copies.end(),
                                       [](const Copy& a, const Copy& b) { return a.done < b.done; });
        if (oldest != image.copies.end() && WaitFence(done.Get(), oldest->done, kRecordingWaitMs)) {
            completed = done->GetCompletedValue();
            ReadCopyTimes(chain, image, completed);
            return &*oldest;
        }
        return nullptr;
    }

    // One image barrier over every mip and layer of `image`.
    static VkImageMemoryBarrier Barrier(VkImage image, VkImageAspectFlags aspect, const Plan& plan,
                                        std::uint32_t mips, VkImageLayout from, VkImageLayout to,
                                        VkAccessFlags source_access, VkAccessFlags destination_access,
                                        std::uint32_t source_family, std::uint32_t destination_family) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = source_family;
        barrier.dstQueueFamilyIndex = destination_family;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, mips, 0, plan.array_size};
        return barrier;
    }

    bool Record(VkCommandBuffer commands, const std::function<void(VkCommandBuffer)>& record) const {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        // Submitted again while earlier submissions of it are pending.
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        if (vk.vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) {
            return false;
        }
        record(commands);
        return vk.vkEndCommandBuffer(commands) == VK_SUCCESS;
    }

    // The image's hand-overs between the game's queue and Direct3D 12,
    // recorded once (recording does not touch the queue).
    //   first_acquire: from UNDEFINED to the attachment layout (first use, or
    //     after a release that was never submitted);
    //   acquire: from Direct3D 12 (the external queue family, GENERAL) to the
    //     game's queue family and the attachment layout, after the done wait;
    //   release: from the attachment layout and the game's queue family to
    //     Direct3D 12, in the submission that signals "written".
    // kResolve: the game renders into its private image, which stays the
    // game's; the release resolves it into the twin (taken over implicitly,
    // its content undefined) and hands the twin to Direct3D 12. kPrivate: only
    // first_acquire.
    bool RecordHandOvers(Swapchain& chain, Image& image, std::string& reason) {
        const Plan& plan = chain.plan;
        VkCommandBuffer buffers[3]{};
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = chain.pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 3;
        VkResult result = vk.vkAllocateCommandBuffers(device, &allocate, buffers);
        if (result != VK_SUCCESS) {
            reason = VkFailure("Allocating the bridge's command buffers", result);
            return false;
        }
        constexpr VkAccessFlags kReadWrite = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        constexpr std::uint32_t kIgnored = VK_QUEUE_FAMILY_IGNORED;
        constexpr std::uint32_t kExternal = VK_QUEUE_FAMILY_EXTERNAL;
        const VkImage game_image = image.private_image ? image.private_image : image.twin_image;
        bool recorded = true;
        if (plan.mode == Mode::kResolve) {
            const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
            image.first_acquire = buffers[0];
            recorded = Record(buffers[0], [&](VkCommandBuffer commands) {
                const VkImageMemoryBarrier barrier =
                    Barrier(image.private_image, color, plan, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, kIgnored,
                            kIgnored);
                vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                        &barrier);
            });
            image.release = buffers[2];
            recorded = recorded && Record(buffers[2], [&](VkCommandBuffer commands) {
                // After the game's rendering, and (the source stage chained
                // with the acquire's done wait) after the copy that last read
                // the twin.
                const VkImageMemoryBarrier before[2] = {
                    Barrier(image.private_image, color, plan, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT, kIgnored, kIgnored),
                    Barrier(image.twin_image, color, plan, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, kIgnored,
                            kIgnored),
                };
                vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                        0, nullptr, 0, nullptr, 2, before);
                VkImageResolve region{};
                region.srcSubresource = {color, 0, 0, plan.array_size};
                region.dstSubresource = {color, 0, 0, plan.array_size};
                region.extent = {plan.width, plan.height, 1};
                vk.vkCmdResolveImage(commands, image.private_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     image.twin_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                // The private image back in the layout OpenXR promises; the
                // twin to Direct3D 12.
                const VkImageMemoryBarrier after[2] = {
                    Barrier(image.private_image, color, plan, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, kIgnored,
                            kIgnored),
                    Barrier(image.twin_image, color, plan, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0, family, kExternal),
                };
                vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        0, 0, nullptr, 0, nullptr, 2, after);
            });
        } else {
            image.first_acquire = buffers[0];
            recorded = Record(buffers[0], [&](VkCommandBuffer commands) {
                const VkImageMemoryBarrier barrier =
                    Barrier(game_image, plan.aspect, plan, plan.mip_count, VK_IMAGE_LAYOUT_UNDEFINED, plan.layout, 0,
                            kReadWrite, kIgnored, kIgnored);
                vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            });
            if (plan.mode != Mode::kPrivate) {
                image.acquire = buffers[1];
                recorded = recorded && Record(buffers[1], [&](VkCommandBuffer commands) {
                    const VkImageMemoryBarrier barrier =
                        Barrier(game_image, plan.aspect, plan, plan.mip_count, VK_IMAGE_LAYOUT_GENERAL, plan.layout, 0,
                                kReadWrite, kExternal, family);
                    vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                });
                image.release = buffers[2];
                recorded = recorded && Record(buffers[2], [&](VkCommandBuffer commands) {
                    const VkImageMemoryBarrier barrier =
                        Barrier(game_image, plan.aspect, plan, plan.mip_count, plan.layout, VK_IMAGE_LAYOUT_GENERAL,
                                VK_ACCESS_MEMORY_WRITE_BIT, 0, family, kExternal);
                    vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                            &barrier);
                });
            }
        }
        if (!recorded) {
            reason = "Recording the bridge's hand-overs failed";
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

VulkanBridge::VulkanBridge() = default;

VulkanBridge::~VulkanBridge() {
    // Process teardown: the devices may be gone, so nothing is waited for,
    // destroyed or released; the Vulkan handles are dropped.
    if (impl_) {
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
        impl_->adapter.Detach();
        impl_->device12.Detach();
    }
}

void VulkanBridge::SetLog(std::function<void(const std::string&)> log) {
    log_ = std::move(log);
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->log = log_;
    }
}

void VulkanBridge::SetInfoLog(std::function<void(const std::string&)> log) {
    info_log_ = std::move(log);
    if (impl_) {
        std::scoped_lock lock(impl_->mutex);
        impl_->info_log = info_log_;
    }
}

const XrSessionCreateInfo* VulkanBridge::Begin(XrInstance instance, const XrSessionCreateInfo* info,
                                               PFN_xrGetInstanceProcAddr get_instance_proc_addr,
                                               const VulkanBridgeGame& game, std::string& reason,
                                               std::string& description) {
    End();
    reason.clear();
    description.clear();
    queried_d3d12_requirements_ = false;
    const auto* first = info ? static_cast<const XrBaseInStructure*>(info->next) : nullptr;
    if (!first || first->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        reason = "its Vulkan binding is not the first structure of the session's create info";
        return nullptr;
    }
    if (const char* bridge = std::getenv("VECTORXR_VULKAN_BRIDGE"); bridge && std::string(bridge) == "off") {
        reason = "VECTORXR_VULKAN_BRIDGE=off";
        return nullptr;
    }
    const auto& vulkan_binding = *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(first);
    if (!vulkan_binding.instance || !vulkan_binding.physicalDevice || !vulkan_binding.device) {
        reason = "the Vulkan binding names no instance, physical device or device";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->instance = vulkan_binding.instance;
    impl->physical = vulkan_binding.physicalDevice;
    impl->device = vulkan_binding.device;
    impl->family = vulkan_binding.queueFamilyIndex;
    impl->queue_index = vulkan_binding.queueIndex;
    impl->log = log_;
    impl->info_log = info_log_;
    Vulkan& vk = impl->vk;

    // The game's own vkGetInstanceProcAddr, else the loader's it already
    // loaded: every Vulkan call goes through the game's layers.
    vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(game.get_instance_proc_addr);
    if (!vk.vkGetInstanceProcAddr) {
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        if (!loader) {
            loader = impl->loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (loader) {
            vk.vkGetInstanceProcAddr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        }
    }
    const auto release_loader = [&] {
        if (impl->loader) {
            FreeLibrary(impl->loader);
            impl->loader = nullptr;
        }
    };
    if (!vk.vkGetInstanceProcAddr) {
        release_loader();
        reason = "no vkGetInstanceProcAddr (the Vulkan loader is not loaded)";
        return nullptr;
    }
    const VkInstance game_instance = impl->instance;
    const VkDevice game_device = impl->device;
    const auto instance_function = [&](const char* name) { return vk.vkGetInstanceProcAddr(game_instance, name); };
    const char* missing = nullptr;
#define VB_LOAD_INSTANCE(name)                                                                                     \
    vk.name = reinterpret_cast<PFN_##name>(instance_function(#name));                                              \
    if (!vk.name && !missing) missing = #name;
#define VB_LOAD_INSTANCE_11(name)                                                                                  \
    vk.name = reinterpret_cast<PFN_##name>(instance_function(#name "KHR"));                                        \
    if (!vk.name) vk.name = reinterpret_cast<PFN_##name>(instance_function(#name));                                \
    if (!vk.name && !missing) missing = #name "KHR";
    VB_INSTANCE_FUNCTIONS(VB_LOAD_INSTANCE)
    VB_INSTANCE_FUNCTIONS_11(VB_LOAD_INSTANCE_11)
#undef VB_LOAD_INSTANCE
#undef VB_LOAD_INSTANCE_11
    if (missing) {
        release_loader();
        reason = std::string("the game's Vulkan instance lacks ") + missing;
        return nullptr;
    }
    const auto device_function = [&](const char* name) { return vk.vkGetDeviceProcAddr(game_device, name); };
#define VB_LOAD_DEVICE(name)                                                                                       \
    vk.name = reinterpret_cast<PFN_##name>(device_function(#name));                                                \
    if (!vk.name && !missing) missing = #name;
#define VB_LOAD_DEVICE_11(name)                                                                                    \
    vk.name = reinterpret_cast<PFN_##name>(device_function(#name "KHR"));                                          \
    if (!vk.name) vk.name = reinterpret_cast<PFN_##name>(device_function(#name));                                  \
    if (!vk.name && !missing) missing = #name "KHR";
    VB_DEVICE_EXTENSION_FUNCTIONS(VB_LOAD_DEVICE)
    if (missing) {
        release_loader();
        reason = std::string("the game's Vulkan device lacks the bridge's extensions (") + missing + ")";
        return nullptr;
    }
    VB_DEVICE_FUNCTIONS(VB_LOAD_DEVICE)
    VB_DEVICE_FUNCTIONS_11(VB_LOAD_DEVICE_11)
#undef VB_LOAD_DEVICE
#undef VB_LOAD_DEVICE_11
    if (missing) {
        release_loader();
        reason = std::string("the game's Vulkan device lacks ") + missing;
        return nullptr;
    }

    // The game's adapter.
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id;
    vk.vkGetPhysicalDeviceProperties2(impl->physical, &properties);
    if (!id.deviceLUIDValid) {
        release_loader();
        reason = "the game's Vulkan device has no adapter LUID";
        return nullptr;
    }
    LUID game_luid{};
    static_assert(sizeof(game_luid) == VK_LUID_SIZE);
    std::memcpy(&game_luid, id.deviceLUID, VK_LUID_SIZE);
    impl->api_version = properties.properties.apiVersion;

    // The runtime requires its graphics requirements to be asked for before a
    // Direct3D 12 session, and names the adapter it composes on.
    PFN_xrGetD3D12GraphicsRequirementsKHR get_requirements = nullptr;
    if (!get_instance_proc_addr ||
        XR_FAILED(get_instance_proc_addr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) ||
        !get_requirements) {
        release_loader();
        reason = "the runtime offers no xrGetD3D12GraphicsRequirementsKHR";
        return nullptr;
    }
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    queried_d3d12_requirements_ = true;
    const XrResult requirements_result = get_requirements(instance, info->systemId, &requirements);
    if (XR_FAILED(requirements_result)) {
        release_loader();
        reason = "xrGetD3D12GraphicsRequirementsKHR failed (" + std::to_string(requirements_result) + ")";
        return nullptr;
    }
    if (!SameLuid(requirements.adapterLuid, game_luid)) {
        release_loader();
        reason = "the runtime asks for Direct3D 12 on adapter " + LuidText(requirements.adapterLuid) +
                 ", and the game's Vulkan device is on adapter " + LuidText(game_luid);
        return nullptr;
    }

    // What the game's device imports: the fences as semaphores of the kind its
    // device allows (timeline semaphores when it has them, else binary ones
    // with Direct3D 12 fence values), and images by format.
    const auto semaphore_importable = [&](bool timeline) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo semaphore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        semaphore.pNext = timeline ? &type : nullptr;
        semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties result{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        vk.vkGetPhysicalDeviceExternalSemaphoreProperties(impl->physical, &semaphore, &result);
        return (result.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) != 0;
    };
    impl->timeline = game.timeline_semaphores && semaphore_importable(true);
    if (!impl->timeline && !semaphore_importable(false)) {
        release_loader();
        reason = "the game's Vulkan device cannot import Direct3D 12 fences";
        return nullptr;
    }
    vk.vkGetPhysicalDeviceMemoryProperties(impl->physical, &impl->memory);
    for (std::int64_t format = 1; format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++format) {
        if (RuntimeFormat(format) == 0) {
            continue;
        }
        const VkImageUsageFlags usage = IsDepthFormat(static_cast<VkFormat>(format)) ? kDepthUsage : kColorUsage;
        Impl::Importable entry;
        entry.plain = impl->QueryImport(static_cast<VkFormat>(format), 0, usage, {}, entry.properties);
        VkImageFormatProperties mutable_properties{};
        entry.mutable_format = impl->QueryImport(static_cast<VkFormat>(format), VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                                 usage, {}, mutable_properties);
        impl->importable[format] = entry;
    }
    if (!impl->importable[VK_FORMAT_R8G8B8A8_SRGB].plain) {
        release_loader();
        reason = "the game's Vulkan device cannot import Direct3D 12 images";
        return nullptr;
    }

    if (const char* debug = std::getenv("VECTORXR_VULKAN_BRIDGE_DEBUG"); debug && std::string(debug) == "1") {
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
        hr = factory->EnumAdapterByLuid(game_luid, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(hr)) {
        release_loader();
        reason = Failure("Finding the game's adapter for Direct3D 12", hr);
        return nullptr;
    }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    adapter.As(&impl->adapter);
    const D3D_FEATURE_LEVEL feature_level = std::max(requirements.minFeatureLevel, D3D_FEATURE_LEVEL_11_0);
    hr = D3D12CreateDevice(adapter.Get(), feature_level, IID_PPV_ARGS(&impl->device12));
    if (FAILED(hr)) {
        release_loader();
        reason = Failure("Creating a Direct3D 12 device on the game's adapter", hr);
        return nullptr;
    }

    // High priority, as Frame Generation's other queues: it carries only the
    // short copies and captures their work waits for, never the game's.
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                                        D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    hr = impl->device12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&impl->queue));
    if (FAILED(hr)) {
        release_loader();
        reason = Failure("Creating the bridge's queue", hr);
        return nullptr;
    }
    impl->queue->SetName(L"VectorXR Vulkan bridge");
    if (FAILED(impl->queue->GetTimestampFrequency(&impl->timestamp_frequency))) {
        impl->timestamp_frequency = 0;
    }

    // Begin's own failures from here on let go of what they made: nothing was
    // submitted yet.
    const auto fail = [&](const std::string& why) -> const XrSessionCreateInfo* {
        impl->DestroySessionVulkan();
        release_loader();
        reason = why;
        return nullptr;
    };
    const auto share_fence = [&](ComPtr<ID3D12Fence>& fence, VkSemaphore& semaphore, std::string& why) {
        HRESULT result = impl->device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        HANDLE handle = nullptr;
        if (SUCCEEDED(result)) {
            result = impl->device12->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        }
        if (FAILED(result)) {
            why = Failure("Sharing the bridge's fences", result);
            return false;
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        create.pNext = impl->timeline ? &type : nullptr;
        VkResult vk_result = vk.vkCreateSemaphore(game_device, &create, nullptr, &semaphore);
        if (vk_result == VK_SUCCESS) {
            VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
            import.semaphore = semaphore;
            import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            import.handle = handle;
            vk_result = vk.vkImportSemaphoreWin32HandleKHR(game_device, &import);
        }
        CloseHandle(handle);
        if (vk_result != VK_SUCCESS) {
            why = VkFailure("Importing the bridge's fences into the game's Vulkan device", vk_result);
            return false;
        }
        return true;
    };
    std::string why;
    if (!share_fence(impl->written, impl->written_semaphore, why) ||
        !share_fence(impl->done, impl->done_semaphore, why)) {
        return fail(why);
    }
    hr = impl->device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl->idle));
    if (FAILED(hr)) {
        return fail(Failure("Creating the bridge's idle fence", hr));
    }

    // The game's queue, used only inside the application's acquire and release calls.
    vk.vkGetDeviceQueue(game_device, impl->family, impl->queue_index, &impl->game_queue);
    if (!impl->game_queue) {
        return fail("the game's queue (family " + std::to_string(impl->family) + ", index " +
                    std::to_string(impl->queue_index) + ") does not exist");
    }
    for (std::uint32_t i = 0; i < kInitialFences; ++i) {
        VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        const VkResult result = vk.vkCreateFence(game_device, &create, nullptr, &fence);
        if (result != VK_SUCCESS) {
            return fail(VkFailure("Creating the bridge's fences", result));
        }
        impl->free_fences.push_back(fence);
        ++impl->fence_count;
    }

    impl->binding.next = vulkan_binding.next;
    impl->binding.device = impl->device12.Get();
    impl->binding.queue = impl->queue.Get();
    impl->info = *info;
    impl->info.next = &impl->binding;

    std::ostringstream stream;
    stream << "adapter=\"" << Utf8(adapter_desc.Description) << "\" luid=" << LuidText(game_luid)
           << ", d3d12FeatureLevel=" << Hex(feature_level) << " (runtime minimum " << Hex(requirements.minFeatureLevel)
           << "), vulkanDevice=\"" << properties.properties.deviceName << "\" api="
           << VersionText(impl->api_version) << " driver=" << Hex(properties.properties.driverVersion)
           << ", queueFamily=" << impl->family << ", queueIndex=" << impl->queue_index
           << ", semaphores=" << (impl->timeline ? "timeline" : "binary")
           << ", formatLists=" << (impl->api12 ? "yes" : "no") << ", videoMemory=" << impl->VideoMemory()
           << (impl->debug_layer ? ", debugLayer=on" : "");
    description = stream.str();
    impl_ = std::move(impl);
    return &impl_->info;
}

bool VulkanBridge::QueriedD3D12Requirements() const {
    return queried_d3d12_requirements_;
}

bool VulkanBridge::Active() const {
    return impl_ != nullptr;
}

const void* VulkanBridge::Binding() const {
    return impl_ ? &impl_->binding : nullptr;
}

void VulkanBridge::SessionCreated() {
    if (impl_) {
        impl_->binding.next = nullptr;
        impl_->info.next = &impl_->binding;
    }
}

bool VulkanBridge::Escape() {
    if (!impl_) {
        return true;
    }
    UINT64 target = 0;
    bool lost = false;
    {
        std::scoped_lock lock(impl_->mutex);
        target = impl_->written_value;
        lost = impl_->lost;
    }
    if (!lost && WaitFence(impl_->written.Get(), target, kRetireWaitMs)) {
        return true;
    }
    // The game's queue did not get past its last release (or never will):
    // what it drew no longer matters, and Frame Generation's waits on the
    // bridge's queue must end. Deliberately so, though the game imported
    // "written" as a timeline semaphore: should its queue still run the
    // signals of those releases later, they no longer raise its value,
    // which Vulkan's rules for timeline semaphores forbid. Only after the
    // game's queue missed kRetireWaitMs; an escape from a hang, never seen
    // in a log, and the GPU bridge tests cover it on NVIDIA.
    std::scoped_lock lock(impl_->mutex);
    impl_->written->Signal(impl_->written_value);
    if (!lost) {
        impl_->Log("the game's queue did not reach its last release in time at teardown; the bridge's queue is "
                   "let through.");
    }
    return false;
}

bool VulkanBridge::Drain() {
    if (!impl_) {
        return true;
    }
    if (WaitIdle(kRetireWaitMs)) {
        return true;
    }
    {
        // The bridge's queue waits for Vulkan work that did not run in time:
        // whatever it drew no longer matters, and the queue must finish while
        // the runtime's images still exist.
        std::scoped_lock lock(impl_->mutex);
        impl_->written->Signal(impl_->written_value);
    }
    const bool idle = WaitIdle(kRetireWaitMs);
    if (!idle) {
        impl_->Log("its queue did not finish before the session ended.");
    }
    return idle;
}

void VulkanBridge::End() {
    queried_d3d12_requirements_ = false;
    if (!impl_) {
        return;
    }
    const bool idle = WaitIdle(kRetireWaitMs);
    if (!idle) {
        // The game's queue must not wait for good for copies that never run,
        // so "done" reaches every value handed out. "written" is left alone:
        // the copies still queued wait for it, and whatever they use is kept
        // for the process (the runtime's images included).
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->done->Signal(impl_->done_value);
        }
        impl_->Log("its queue did not finish at teardown; its Direct3D 12 objects are kept for the process.");
    }
    std::uint64_t last_serial = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        last_serial = impl_->last_serial;
    }
    if (impl_->WaitSubmissions(last_serial, kRetireWaitMs)) {
        std::scoped_lock lock(impl_->mutex);
        for (auto& [swapchain, chain] : impl_->swapchains) {
            impl_->DestroyVulkan(chain);
        }
        for (Impl::Swapchain& chain : impl_->retired) {
            impl_->DestroyVulkan(chain);
        }
        impl_->DestroySessionVulkan();
    } else {
        impl_->Log("the game's queue did not finish the bridge's submissions at teardown; their Vulkan objects are "
                   "kept for the process.");
    }
    if (!idle) {
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
    if (impl_->loader) {
        FreeLibrary(impl_->loader);
    }
    impl_.reset();
}

bool VulkanBridge::Lost(std::string& reason) const {
    if (!impl_) {
        return false;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->lost) {
            reason = "the game's Vulkan device was lost (" + impl_->lost_reason + ")";
            return true;
        }
    }
    const HRESULT hr = impl_->device12->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) {
        return false;
    }
    reason = "the bridge's Direct3D 12 device was removed (hr=" + Hex(static_cast<std::uint32_t>(hr)) + ")";
    return true;
}

void VulkanBridge::FrameEnded() {
    if (!impl_) {
        return;
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->NoteThread();
    impl_->ReportDebugMessages();
    // A lost device shows in its fences too (no queue is touched).
    impl_->PollFences();
}

std::vector<std::int64_t> VulkanBridge::GameFormats(const std::vector<std::int64_t>& runtime_formats) const {
    std::vector<std::int64_t> formats = TranslatedFormats(runtime_formats);
    if (!impl_) {
        return formats;
    }
    formats.erase(std::remove_if(formats.begin(), formats.end(),
                                 [&](std::int64_t format) {
                                     const auto it = impl_->importable.find(format);
                                     return it == impl_->importable.end() || !it->second.plain;
                                 }),
                  formats.end());
    return formats;
}

bool VulkanBridge::CheckSwapchain(const XrSwapchainCreateInfo& info, XrResult& refusal, std::string& reason) const {
    if (!impl_) {
        refusal = XR_ERROR_RUNTIME_FAILURE;
        reason = "the session is not bridged";
        return false;
    }
    Impl::Plan plan;
    if (!impl_->MakePlan(info, plan, refusal, reason)) {
        return false;
    }
    refusal = XR_SUCCESS;
    return true;
}

bool VulkanBridge::AddSwapchain(XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                                PFN_xrEnumerateSwapchainImages enumerate, std::string& reason) {
    reason.clear();
    if (!impl_) {
        reason = "the session is not bridged";
        return false;
    }
    Impl& impl = *impl_;
    Impl::Swapchain chain;
    XrResult refusal = XR_SUCCESS;
    if (!impl.MakePlan(info, chain.plan, refusal, reason)) {
        return false;
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
    if (chain.plan.mode == Impl::Mode::kPrivate) {
        chain.private_reason = "it is multisampled";
    } else if (chain.plan.mode == Impl::Mode::kTwinDepth) {
        for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
            if (image.texture && (image.texture->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                // A runtime that made no depth target of it gets no depth from
                // the game: the depth stays the game's own.
                chain.plan.mode = Impl::Mode::kPrivate;
                chain.private_reason = "the runtime's depth images are not depth targets";
                impl.Info("a depth swapchain stays private: " + chain.private_reason + ".");
                break;
            }
        }
    }
    const Impl::Plan& plan = chain.plan;

    // Everything made here is the swapchain's alone until it is added, and
    // nothing of it was submitted: a failure lets it all go.
    const auto fail = [&](std::uint32_t index, const std::string& why) {
        impl.DestroyVulkan(chain);
        reason = "image " + std::to_string(index) + ": " + why;
        return false;
    };
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = impl.family;
    const VkResult pool_result = impl.vk.vkCreateCommandPool(impl.device, &pool_info, nullptr, &chain.pool);
    if (pool_result != VK_SUCCESS) {
        chain.pool = VK_NULL_HANDLE;
        reason = VkFailure("Creating the bridge's command pool", pool_result);
        return false;
    }
    if (plan.mode != Impl::Mode::kPrivate && impl.timestamp_frequency) {
        // Two timestamps around each recording of each image's copy, read back
        // once the copy is done.
        const UINT queries = count * static_cast<UINT>(kCopyRecordings) * 2;
        D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, queries, 0};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = queries * sizeof(UINT64);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        if (FAILED(impl.device12->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&chain.timestamps))) ||
            FAILED(impl.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&chain.stamps))) ||
            FAILED(chain.stamps->Map(0, nullptr, &mapped))) {
            // The copies go untimed.
            chain.timestamps.Reset();
            chain.stamps.Reset();
        } else {
            chain.stamp_data = static_cast<const UINT64*>(mapped);
        }
    }
    chain.images.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        Impl::Image& image = chain.images[i];
        image.runtime = runtime_images[i].texture;
        std::string why;
        if (plan.mode != Impl::Mode::kPrivate &&
            (!impl.CheckRuntime(plan, image, why) || !impl.MakeTwin(chain, image, why))) {
            return fail(i, why);
        }
        if ((plan.mode == Impl::Mode::kResolve || plan.mode == Impl::Mode::kPrivate) &&
            !impl.MakePrivate(plan, image, why)) {
            return fail(i, why);
        }
        if (!impl.RecordHandOvers(chain, image, why)) {
            return fail(i, why);
        }
        if (plan.mode != Impl::Mode::kPrivate) {
            image.copies.emplace_back();
            std::scoped_lock lock(impl.mutex);
            if (!impl.RecordCopy(chain, image, image.copies.back(), i * kCopyRecordings * 2, why)) {
                return fail(i, why);
            }
        }
    }
    std::scoped_lock lock(impl.mutex);
    impl.swapchains[swapchain] = std::move(chain);
    ++impl.swapchains_added;
    return true;
}

bool VulkanBridge::Bridged(XrSwapchain swapchain) const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return impl_->Find(swapchain) != nullptr;
}

bool VulkanBridge::Unmirrored(XrSwapchain swapchain) const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    const Impl::Swapchain* chain = impl_->Find(swapchain);
    return chain && chain->plan.mode == Impl::Mode::kPrivate;
}

bool VulkanBridge::HasUnmirrored() const {
    if (!impl_) {
        return false;
    }
    std::scoped_lock lock(impl_->mutex);
    return std::any_of(impl_->swapchains.begin(), impl_->swapchains.end(),
                       [](const auto& entry) { return entry.second.plan.mode == Impl::Mode::kPrivate; });
}

std::vector<std::uint64_t> VulkanBridge::Images(XrSwapchain swapchain) const {
    std::vector<std::uint64_t> images;
    if (!impl_) {
        return images;
    }
    std::scoped_lock lock(impl_->mutex);
    if (const Impl::Swapchain* chain = impl_->Find(swapchain)) {
        for (const Impl::Image& image : chain->images) {
            images.push_back(reinterpret_cast<std::uint64_t>(image.private_image ? image.private_image
                                                                                 : image.twin_image));
        }
    }
    return images;
}

XrResult VulkanBridge::EnumerateImages(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t* count,
                                       XrSwapchainImageBaseHeader* images) const {
    const std::vector<std::uint64_t> handles = Images(swapchain);
    if (handles.empty()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (!count) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *count = static_cast<std::uint32_t>(handles.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < handles.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    if (!images || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    auto* vulkan_images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        vulkan_images[i].image = reinterpret_cast<VkImage>(handles[i]);
    }
    return XR_SUCCESS;
}

void VulkanBridge::Acquired(XrSwapchain swapchain, std::uint32_t index) {
    if (!impl_) {
        return;
    }
    Impl& impl = *impl_;
    std::scoped_lock lock(impl.mutex);
    Impl::Swapchain* chain = impl.Find(swapchain);
    if (!chain) {
        return;
    }
    chain->acquired.push_back(index);
    if (index >= chain->images.size()) {
        return;
    }
    impl.NoteThread();
    if (impl.lost) {
        return;
    }
    Impl::Image& image = chain->images[index];
    const Impl::Mode mode = chain->plan.mode;
    if (mode == Impl::Mode::kPrivate) {
        // Private images stay the game's: only their first use needs a layout.
        if (image.state != Impl::State::kUndefined) {
            return;
        }
        std::uint64_t serial = 0;
        if (impl.Submit(image.first_acquire, 0, 0, serial) == VK_SUCCESS) {
            image.state = Impl::State::kGame;
            chain->last_serial = serial;
        } else {
            ++chain->failed_submits;
        }
        return;
    }
    // Until the bridge's queue is done reading the image's last release, the
    // game must not write it again. The value's signal is already submitted
    // (at that release), so the wait always ends.
    const UINT64 wait = image.done && impl.done->GetCompletedValue() < image.done ? image.done : 0;
    // An image the game had last, without a release submitted (or never had),
    // starts over from UNDEFINED; one Direct3D 12 had comes back from it.
    const VkCommandBuffer commands = image.state == Impl::State::kExternal ? image.acquire : image.first_acquire;
    if (!commands && !wait) {
        image.state = Impl::State::kGame;
        return;
    }
    std::uint64_t serial = 0;
    if (impl.Submit(commands, wait, 0, serial) != VK_SUCCESS) {
        // The image stays where it was: its release submits nothing and copies nothing.
        ++chain->failed_submits;
        return;
    }
    image.state = Impl::State::kGame;
    chain->last_serial = serial;
    if (wait) {
        ++chain->done_waits;
        ++impl.done_waits;
    }
}

bool VulkanBridge::Released(XrSwapchain swapchain) {
    if (!impl_) {
        return true;
    }
    Impl& impl = *impl_;
    std::scoped_lock lock(impl.mutex);
    Impl::Swapchain* chain = impl.Find(swapchain);
    if (!chain) {
        return true;
    }
    ++impl.releases;
    if (chain->acquired.empty()) {
        ++chain->unmatched_releases;
        ++impl.unmatched_releases;
        return true;
    }
    const std::uint32_t index = chain->acquired.front();
    chain->acquired.pop_front();
    if (chain->plan.mode == Impl::Mode::kPrivate || index >= chain->images.size()) {
        return true;
    }
    impl.NoteThread();
    Impl::Image& image = chain->images[index];
    if (impl.lost || image.state != Impl::State::kGame) {
        // The game's device is lost, or the acquire's submission was refused:
        // the image was never the game's to hand over.
        return false;
    }
    // The game's rendering into the image is handed to the bridge's queue...
    const UINT64 written = impl.written_value + 1;
    std::uint64_t serial = 0;
    if (impl.Submit(image.release, 0, written, serial) != VK_SUCCESS) {
        // Nothing was handed over: no copy, and the image stays the game's.
        ++chain->failed_submits;
        return false;
    }
    impl.written_value = written;
    image.state = Impl::State::kExternal;
    chain->last_serial = serial;
    Impl::Copy* copy = impl.ReadyCopy(*chain, image, index);
    if (!copy) {
        // Every recording of the copy is still queued: the bridge's queue is
        // stalled. The runtime's image keeps what it had.
        ++chain->skipped_copies;
        ++impl.skipped_copies;
        if (ReportNow(impl.skipped_copies, impl.next_skip_report)) {
            impl.Log("its queue is stalled; releases not copied into the runtime's images so far: " +
                     std::to_string(impl.skipped_copies) + ".");
        }
        return false;
    }
    // ...which copies it into the runtime's image, ahead of anything else
    // queued on it for the release (Frame Generation's capture).
    impl.queue->Wait(impl.written.Get(), written);
    ID3D12CommandList* lists[] = {copy->list.Get()};
    impl.queue->ExecuteCommandLists(1, lists);
    const UINT64 done = ++impl.done_value;
    impl.queue->Signal(impl.done.Get(), done);
    copy->done = done;
    copy->timed = chain->stamp_data != nullptr;
    image.done = done;
    chain->last_done = done;
    ++chain->copies;
    ++impl.copies;
    return true;
}

bool VulkanBridge::RemoveSwapchain(XrSwapchain swapchain) {
    if (!impl_) {
        return true;
    }
    UINT64 last_done = 0;
    std::uint64_t last_serial = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        const Impl::Swapchain* chain = impl_->Find(swapchain);
        if (!chain) {
            return true;
        }
        last_done = chain->last_done;
        last_serial = chain->last_serial;
    }
    const bool copied = WaitFence(impl_->done.Get(), last_done, kRetireWaitMs);
    const bool submitted = copied && impl_->WaitSubmissions(last_serial, kRetireWaitMs);
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->swapchains.find(swapchain);
    if (it == impl_->swapchains.end()) {
        return true;
    }
    if (copied && submitted) {
        impl_->DestroyVulkan(it->second);
        impl_->swapchains.erase(it);
        return true;
    }
    impl_->Log(copied ? "the game's queue did not finish the bridge's submissions for a destroyed swapchain; it is "
                        "kept until the session ends."
                      : "copies into a destroyed swapchain did not finish; it is kept until the session ends.");
    impl_->retired.push_back(std::move(it->second));
    impl_->swapchains.erase(it);
    return false;
}

bool VulkanBridge::WaitIdle(std::uint32_t timeout_ms) {
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

std::string VulkanBridge::Describe(XrSwapchain swapchain) const {
    if (!impl_) {
        return "not bridged";
    }
    std::scoped_lock lock(impl_->mutex);
    const Impl::Swapchain* chain = impl_->Find(swapchain);
    return chain ? impl_->Describe(*chain) : "not bridged";
}

std::string VulkanBridge::Summary() const {
    if (!impl_) {
        return "inactive";
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->ReadAllCopyTimes();
    std::uint64_t twin_bytes = 0;
    for (const auto& [swapchain, chain] : impl_->swapchains) {
        twin_bytes += chain.twin_bytes;
    }
    const auto whole = [](double value) { return static_cast<std::uint64_t>(value + 0.5); };
    std::ostringstream stream;
    stream << "swapchains=" << impl_->swapchains_added << " (" << impl_->swapchains.size() << " live, "
           << impl_->retired.size() << " kept), liveTwinMiB=" << MiB(twin_bytes)
           << ", releases=" << impl_->releases << ", copies=" << impl_->copies
           << ", skippedCopies=" << impl_->skipped_copies << ", copyRecordings=" << impl_->recordings
           << ", doneWaits=" << impl_->done_waits << ", unmatchedReleases=" << impl_->unmatched_releases
           << ", submits=" << impl_->submits << ", failedSubmits=" << impl_->failed_submits
           << ", submitUs p50/max=" << whole(Percentile(impl_->submit_us, 0.5)) << "/"
           << whole(Percentile(impl_->submit_us, 1.0)) << ", liveFences=" << impl_->fence_count
           << ", fenceGrowth=" << impl_->fence_growth << ", unfencedSubmits=" << impl_->unfenced_submits
           << ", semaphores=" << (impl_->timeline ? "timeline" : "binary")
           << ", copyGpuUs p50/p99=" << whole(Percentile(impl_->copy_gpu_us, 0.5)) << "/"
           << whole(Percentile(impl_->copy_gpu_us, 0.99)) << ", writtenValue=" << impl_->written_value
           << ", doneValue=" << impl_->done_value
           << ", doneCompleted=" << (impl_->done ? impl_->done->GetCompletedValue() : 0)
           << ", secondThread=" << (impl_->reported_second_thread ? "yes" : "no")
           << ", lost=" << (impl_->lost ? "yes" : "no") << ", videoMemory=" << impl_->VideoMemory();
    return stream.str();
}

} // namespace depthxr
