#include <windows.h>
#include <d3d11.h> // for openxr_platform.h's Direct3D 11 types only
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <openxr/openxr_platform.h>

#include "depthxr/submitted_image.h"
#include "submitted_image_mirror.h"
#include "submitted_image_vulkan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

// The stabilized mirror window for a Vulkan game whose session with the
// runtime is Vulkan too. The game's Vulkan device gets no window, surface or
// swap chain: inside the game's xrEndFrame its queue copies the eye's
// rectangle into an image that Direct3D 12 made and shares (the mechanism of
// Frame Generation's Vulkan bridge: VK_KHR_external_memory_win32), and a
// Direct3D 12 device of VectorXR's own, on the game's adapter, draws that
// image into the window with the Direct3D 12 mirror (submitted_image_mirror.h).
//
// The window is far smaller than an eye's image, so where the format can be
// blitted the game's queue scales the eye down as it copies it
// (vkCmdBlitImage, linear): to the size the window's picture needs of it, in
// steps of an eighth and never below half (a linear blit averages two by
// two texels at most; the mirror's draw filters what is left). The shared
// images are then about the window's size instead of the eye's. A format
// that cannot be blitted is copied whole (vkCmdCopyImage).
//
// Nothing waits for either GPU queue (MirrorRelay): the copy carries a
// VkFence that later calls poll; the picture is drawn by the first call
// that finds the fence signalled, and the image is copied into again only
// once the mirror's own queue is done with it. So the window shows a frame
// one call after its copy was queued. The Direct3D 12 device, its queue and
// the shared images are made on the window's thread, which also imports the
// images into the game's device and makes the copies' command buffers and
// fences: the frame thread makes nothing that takes time.

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

// How long a swapchain's end and the session's wait for the game's queue to
// finish a copy that reads the swapchain's image.
constexpr std::uint64_t kCopyWaitNs = 2'000'000'000ull;
// A picture copied this long ago is not shown any more (the window was
// hidden meanwhile).
constexpr std::chrono::milliseconds kStalePicture{250};

std::string Failure(const char* operation, HRESULT result) {
    char code[16];
    sprintf_s(code, "0x%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (" + code + ")";
}

std::string VkFailure(const char* operation, VkResult result) {
    return std::string(operation) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
}

// The Direct3D format whose texels are a Vulkan format's, for the formats
// the mirror shows (ClassifyFormat's).
DXGI_FORMAT DxgiOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// Every Vulkan function the mirror calls, resolved through the game's own
// vkGetInstanceProcAddr (so the game's Vulkan layers see them). Core 1.1
// functions by their KHR names first: a Vulkan 1.0 game has them through
// the extensions the mirror asked for.
#define VM_INSTANCE_FUNCTIONS(X)                                                                                   \
    X(vkGetDeviceProcAddr)                                                                                         \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
    X(vkGetPhysicalDeviceFormatProperties)
#define VM_INSTANCE_FUNCTIONS_11(X)                                                                                \
    X(vkGetPhysicalDeviceProperties2)                                                                              \
    X(vkGetPhysicalDeviceImageFormatProperties2)
#define VM_DEVICE_FUNCTIONS(X)                                                                                     \
    X(vkGetDeviceQueue)                                                                                            \
    X(vkCreateImage)                                                                                               \
    X(vkDestroyImage)                                                                                              \
    X(vkGetImageMemoryRequirements)                                                                                \
    X(vkAllocateMemory)                                                                                            \
    X(vkFreeMemory)                                                                                                \
    X(vkBindImageMemory)                                                                                           \
    X(vkCreateCommandPool)                                                                                         \
    X(vkDestroyCommandPool)                                                                                        \
    X(vkAllocateCommandBuffers)                                                                                    \
    X(vkResetCommandBuffer)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                        \
    X(vkEndCommandBuffer)                                                                                          \
    X(vkCmdPipelineBarrier)                                                                                        \
    X(vkCmdCopyImage)                                                                                              \
    X(vkCmdBlitImage)                                                                                              \
    X(vkQueueSubmit)                                                                                               \
    X(vkCreateFence)                                                                                               \
    X(vkDestroyFence)                                                                                              \
    X(vkGetFenceStatus)                                                                                            \
    X(vkResetFences)                                                                                               \
    X(vkWaitForFences)                                                                                             \
    X(vkCreateQueryPool)                                                                                           \
    X(vkDestroyQueryPool)                                                                                          \
    X(vkCmdResetQueryPool)                                                                                         \
    X(vkCmdWriteTimestamp)                                                                                         \
    X(vkGetQueryPoolResults)

struct Vulkan {
#define VM_DECLARE(name) PFN_##name name{nullptr};
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr{nullptr};
    VM_INSTANCE_FUNCTIONS(VM_DECLARE)
    VM_INSTANCE_FUNCTIONS_11(VM_DECLARE)
    VM_DEVICE_FUNCTIONS(VM_DECLARE)
    PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR{nullptr};
#undef VM_DECLARE
};

// What the window's thread hands back: the Direct3D 12 device on the game's
// adapter and its queue.
struct Host {
    std::mutex mutex;
    bool busy{true};
    bool failed{false};
    std::string error;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
};

void MakeHost(const std::shared_ptr<Host>& host, LUID luid) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::string error;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        error = Failure("Creating the DXGI factory", result);
    } else {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart &&
                desc.AdapterLuid.HighPart == luid.HighPart) {
                adapter = candidate;
                break;
            }
        }
        if (!adapter) error = "Direct3D 12 does not list the game's Vulkan adapter";
    }
    if (error.empty()) {
        result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(result)) error = Failure("Creating the mirror's Direct3D 12 device", result);
    }
    if (error.empty()) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        result = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        if (FAILED(result)) error = Failure("Creating the mirror's Direct3D 12 queue", result);
    }
    if (error.empty()) {
        device->SetName(L"VectorXR mirror device");
        queue->SetName(L"VectorXR mirror queue");
    }
    std::scoped_lock lock(host->mutex);
    host->busy = false;
    host->failed = !error.empty();
    host->error = std::move(error);
    if (!host->failed) {
        host->device = std::move(device);
        host->queue = std::move(queue);
    }
}

std::atomic<std::uint32_t> g_import_delay_ms{0};
// Tests: called under the queue mutex, right before the copy is submitted.
void (*g_submit_hook)() = nullptr;

// The two images both APIs share, as the window's thread hands them back:
// the Direct3D 12 textures and their Vulkan images in the game's device,
// and (the first time) the command buffers and fences of the copies.
// Everything that takes time is made there: Direct3D 12's creation calls
// are free-threaded, and Vulkan lets images, memory, pools and fences be
// made on any thread while the game uses its device on its own.
struct Shared {
    std::mutex mutex;
    bool busy{true};
    bool failed{false};
    bool abandoned{false};
    // The game's device may be gone: no call is made on it any more.
    bool device_gone{false};
    std::string error;
    ComPtr<ID3D12Resource> textures[MirrorRelay::kSlots];
    VkImage images[MirrorRelay::kSlots]{};
    VkDeviceMemory memories[MirrorRelay::kSlots]{};
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer commands[MirrorRelay::kSlots]{};
    VkFence fences[MirrorRelay::kSlots]{};
};

// What the window's thread needs to make them (all by value: it may run
// after the frame thread went on).
struct SharedRequest {
    Vulkan vk;
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory{};
    std::uint32_t family{0};
    ComPtr<ID3D12Device> host;
    VkFormat format{VK_FORMAT_UNDEFINED};
    DXGI_FORMAT dxgi{DXGI_FORMAT_UNKNOWN};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool make_commands{false};
    // The images these replace, let go of first (nothing uses them any more).
    ComPtr<ID3D12Resource> old_textures[MirrorRelay::kSlots];
    VkImage old_images[MirrorRelay::kSlots]{};
    VkDeviceMemory old_memories[MirrorRelay::kSlots]{};
};

void DestroyImages(const Vulkan& vk, VkDevice device, VkImage* images, VkDeviceMemory* memories) {
    for (int slot = 0; slot < MirrorRelay::kSlots; ++slot) {
        if (images[slot]) vk.vkDestroyImage(device, images[slot], nullptr);
        if (memories[slot]) vk.vkFreeMemory(device, memories[slot], nullptr);
        images[slot] = VK_NULL_HANDLE;
        memories[slot] = VK_NULL_HANDLE;
    }
}

void DestroyCommands(const Vulkan& vk, VkDevice device, VkCommandPool& pool, VkFence* fences) {
    for (int slot = 0; slot < MirrorRelay::kSlots; ++slot) {
        if (fences[slot]) vk.vkDestroyFence(device, fences[slot], nullptr);
        fences[slot] = VK_NULL_HANDLE;
    }
    if (pool) vk.vkDestroyCommandPool(device, pool, nullptr);
    pool = VK_NULL_HANDLE;
}

// Whether the game's device imports a Direct3D 12 image of the format.
bool Importable(const SharedRequest& request) {
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    query.pNext = &external;
    query.format = request.format;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_OPTIMAL;
    query.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    result.pNext = &external_properties;
    return request.vk.vkGetPhysicalDeviceImageFormatProperties2(request.physical, &query, &result) == VK_SUCCESS &&
           (external_properties.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

// The Direct3D 12 texture behind `handle` as an image of the game's device.
bool Import(const SharedRequest& request, HANDLE handle, VkImage& image, VkDeviceMemory& memory,
            std::string& error) {
    const Vulkan& vk = request.vk;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.pNext = &external;
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = request.format;
    create.extent = {request.width, request.height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vk.vkCreateImage(request.device, &create, nullptr, &image);
    if (result != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        error = VkFailure("Creating the Vulkan image of the mirror's shared texture", result);
        return false;
    }
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(request.device, image, &requirements);
    VkMemoryWin32HandlePropertiesKHR handle_properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    result = vk.vkGetMemoryWin32HandlePropertiesKHR(request.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                                    handle, &handle_properties);
    if (result != VK_SUCCESS) {
        error = VkFailure("vkGetMemoryWin32HandlePropertiesKHR", result);
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    import.handle = handle;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &import;
    allocate.allocationSize = requirements.size;
    const std::uint32_t types = requirements.memoryTypeBits & handle_properties.memoryTypeBits;
    bool found = false;
    for (std::uint32_t i = 0; i < request.memory.memoryTypeCount && !found; ++i) {
        if (types & (1u << i)) {
            allocate.memoryTypeIndex = i;
            found = true;
        }
    }
    if (!found) {
        error = "no memory type of the game's device imports the mirror's shared texture";
        return false;
    }
    result = vk.vkAllocateMemory(request.device, &allocate, nullptr, &memory);
    if (result != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
        error = VkFailure("Importing the mirror's shared texture", result);
        return false;
    }
    result = vk.vkBindImageMemory(request.device, image, memory, 0);
    if (result != VK_SUCCESS) {
        error = VkFailure("Binding the mirror's shared texture", result);
        return false;
    }
    return true;
}

// On the window's thread (or the caller's, when the window's cannot take it).
void MakeShared(const std::shared_ptr<Shared>& shared, SharedRequest request) {
    const Vulkan& vk = request.vk;
    {
        std::scoped_lock lock(shared->mutex);
        if (shared->device_gone) {
            // Run after the session whose device it was given: nothing of
            // that device is touched (what it held of it is the device's).
            shared->busy = false;
            shared->failed = true;
            shared->error = "abandoned";
            return;
        }
    }
    // The images these replace go first: their video memory is free for the new.
    DestroyImages(vk, request.device, request.old_images, request.old_memories);
    for (ComPtr<ID3D12Resource>& texture : request.old_textures) texture.Reset();
    if (const std::uint32_t delay = g_import_delay_ms.load(std::memory_order_acquire)) Sleep(delay);
    ComPtr<ID3D12Resource> textures[MirrorRelay::kSlots];
    VkImage images[MirrorRelay::kSlots]{};
    VkDeviceMemory memories[MirrorRelay::kSlots]{};
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[MirrorRelay::kSlots]{};
    VkFence fences[MirrorRelay::kSlots]{};
    std::string error;
    bool abandoned = false;
    {
        std::scoped_lock lock(shared->mutex);
        abandoned = shared->abandoned;
    }
    if (!abandoned && !Importable(request)) {
        error = "the game's Vulkan device cannot import a Direct3D 12 image of format " +
                std::to_string(static_cast<int>(request.format));
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = request.width;
    desc.Height = request.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = request.dxgi;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // As the Vulkan bridge makes the images it shares with a game's device.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    for (int slot = 0; slot < MirrorRelay::kSlots && !abandoned && error.empty(); ++slot) {
        HRESULT result = request.host->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                               D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                               IID_PPV_ARGS(&textures[slot]));
        if (FAILED(result)) {
            error = Failure("Creating a shared Direct3D 12 texture", result);
            break;
        }
        textures[slot]->SetName(L"VectorXR mirror image");
        HANDLE handle = nullptr;
        result = request.host->CreateSharedHandle(textures[slot].Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(result)) {
            error = Failure("Sharing a Direct3D 12 texture", result);
            break;
        }
        Import(request, handle, images[slot], memories[slot], error);
        CloseHandle(handle);
    }
    if (!abandoned && error.empty() && request.make_commands) {
        // The command buffers and fences of the copies, once.
        VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        create.queueFamilyIndex = request.family;
        VkResult result = vk.vkCreateCommandPool(request.device, &create, nullptr, &pool);
        if (result == VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = MirrorRelay::kSlots;
            result = vk.vkAllocateCommandBuffers(request.device, &allocate, commands);
        } else {
            pool = VK_NULL_HANDLE;
        }
        for (int slot = 0; slot < MirrorRelay::kSlots && result == VK_SUCCESS; ++slot) {
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            result = vk.vkCreateFence(request.device, &fence, nullptr, &fences[slot]);
        }
        if (result != VK_SUCCESS) error = VkFailure("Creating the mirror's Vulkan command buffers", result);
    }
    std::scoped_lock lock(shared->mutex);
    shared->busy = false;
    if (shared->device_gone) {
        shared->failed = true;
        shared->error = "abandoned";
        return;
    }
    if (shared->abandoned || !error.empty()) {
        // Nobody takes them: let go of here.
        DestroyImages(vk, request.device, images, memories);
        DestroyCommands(vk, request.device, pool, fences);
        shared->failed = true;
        shared->error = error.empty() ? "abandoned" : std::move(error);
        return;
    }
    for (int slot = 0; slot < MirrorRelay::kSlots; ++slot) {
        shared->textures[slot] = std::move(textures[slot]);
        shared->images[slot] = images[slot];
        shared->memories[slot] = memories[slot];
        shared->commands[slot] = commands[slot];
        shared->fences[slot] = fences[slot];
    }
    shared->pool = pool;
}

class VulkanImages final : public SubmittedImageGpu {
  public:
    static std::unique_ptr<VulkanImages> Make(const SubmittedImageBinding& binding, std::string& error) {
        auto images = std::unique_ptr<VulkanImages>(new VulkanImages);
        if (!images->Begin(binding, error)) return nullptr;
        return images;
    }

    ~VulkanImages() override {
        Reset();
        if (pool_) vk_.vkDestroyCommandPool(device_, pool_, nullptr);
        if (loader_) FreeLibrary(loader_);
    }

    const char* Name() const override { return "Vulkan"; }
    SubmittedImageApi Api() const override { return SubmittedImageApi::kVulkan; }
    XrSwapchainUsageFlags MirrorSourceUsage() const override { return XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; }
    bool SupportsReadback() const override { return false; }

    bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader* images, std::uint32_t count,
               std::string& error) override {
        if (!images || count == 0 || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
            error = "not Vulkan swapchain images";
            return false;
        }
        const auto* vulkan = reinterpret_cast<const XrSwapchainImageVulkanKHR*>(images);
        std::vector<VkImage> handles;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (vulkan[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR || vulkan[i].image == VK_NULL_HANDLE) {
                error = "a swapchain image has no Vulkan image";
                return false;
            }
            handles.push_back(vulkan[i].image);
        }
        Forget(swapchain);
        swapchains_[swapchain] = std::move(handles);
        return true;
    }

    // The runtime may destroy the images next: a copy the game's queue has
    // not run yet reads one of them, so it is waited for (bounded; this is
    // the game's xrDestroySwapchain, not a frame).
    void Forget(XrSwapchain swapchain) override {
        const int copying = relay_.Copying();
        if (copying >= 0 && slots_[copying].source == swapchain) WaitForCopy();
        swapchains_.erase(swapchain);
    }
    bool Tracked(XrSwapchain swapchain) const override { return swapchains_.count(swapchain) != 0; }

    std::uint64_t Readback(const SubmittedView&, std::string& error) override {
        error = "Vulkan images cannot be read back yet";
        return 0;
    }
    ReadbackState Poll(std::uint64_t, SubmittedPixels&, std::string&) override { return ReadbackState::kNone; }
    void Finish(std::uint64_t) override {}

    bool SupportsMirror() const override { return true; }
    MirrorDrawResult DrawMirror(const SubmittedView& view, const MirrorDraw& draw, std::string& error) override;
    void CloseMirror() override {
        if (mirror_) mirror_->Close();
    }
    void SetQueueMutex(std::mutex* mutex) override { queue_mutex_ = mutex; }
    void Abandon() override {
        // As a lost device: Reset then waits for nothing of it, and frees
        // nothing through it.
        lost_ = true;
        Reset();
    }
    void Reset() override;

    // Tests.
    ID3D12Device* HostDevice() const { return host_device_.Get(); }
    ID3D12CommandQueue* HostQueue() const { return host_queue_.Get(); }
    void SetTarget(ID3D12Resource* target) { test_target_ = target; }
    void SetTiming(bool on) { timing_wanted_ = on; }
    void SetFullCopy(bool on) { full_copy_ = on; }
    double CopyGpuMs() const { return copy_gpu_ms_; }
    std::uint64_t Copies() const { return copies_; }

  private:
    struct Slot {
        ComPtr<ID3D12Resource> texture;
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkCommandBuffer commands{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        // The picture it holds (or is being given).
        XrSwapchain source{XR_NULL_HANDLE};
        XrExtent2Di extent{};
        MirrorHomography homography{};
        std::chrono::steady_clock::time_point copied{};
        bool timed{false};
    };

    VulkanImages() = default;
    bool Begin(const SubmittedImageBinding& binding, std::string& error);
    bool Run(const MirrorDraw& draw, std::function<void()> task) const {
        if (draw.on_window_thread && draw.on_window_thread(task)) return true;
        task();
        return false;
    }
    // Whether the game's device scales images of the format with a linear blit.
    bool Blittable(VkFormat format);
    // The size the eye's rectangle is copied at: its own, or smaller where
    // the window's picture needs no more of it and the format can be blitted.
    XrExtent2Di PictureExtent(VkFormat format, const SubmittedView& view, const MirrorDraw& draw);
    void FreeImages();
    void AbandonShared();
    // Polls the copy under way. False: the game's device is lost.
    bool PollCopy();
    void WaitForCopy();
    bool Copy(Slot& slot, VkImage source, const SubmittedView& view, const XrExtent2Di& picture,
              std::string& error);

    Vulkan vk_;
    HMODULE loader_{nullptr};
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    // What the session's other users of the game's queue lock around their
    // calls on it (SetQueueMutex); null: none to lock.
    std::mutex* queue_mutex_{nullptr};
    std::uint32_t family_{0};
    VkPhysicalDeviceMemoryProperties memory_{};
    LUID luid_{};
    float timestamp_period_ns_{0.0f};
    VkCommandPool pool_{VK_NULL_HANDLE};
    VkQueryPool queries_{VK_NULL_HANDLE};
    std::unordered_map<XrSwapchain, std::vector<VkImage>> swapchains_;

    std::shared_ptr<Host> host_;
    bool host_failed_{false};
    std::string host_error_;
    ComPtr<ID3D12Device> host_device_;
    ComPtr<ID3D12CommandQueue> host_queue_;
    std::unique_ptr<D3D12Mirror> mirror_;

    std::shared_ptr<Shared> shared_;
    std::uint32_t shared_want_width_{0};
    std::uint32_t shared_want_height_{0};
    VkFormat shared_want_format_{VK_FORMAT_UNDEFINED};
    Slot slots_[MirrorRelay::kSlots];
    MirrorRelay relay_;
    VkFormat format_{VK_FORMAT_UNDEFINED};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    bool lost_{false};
    std::unordered_map<int, bool> blittable_;
    bool full_copy_{false}; // tests: never scaled

    ID3D12Resource* test_target_{nullptr};
    bool timing_wanted_{false};
    double copy_gpu_ms_{-1.0};
    std::uint64_t copies_{0};
};

bool VulkanImages::Begin(const SubmittedImageBinding& binding, std::string& error) {
    instance_ = static_cast<VkInstance>(binding.vulkan_instance);
    physical_ = static_cast<VkPhysicalDevice>(binding.vulkan_physical_device);
    device_ = static_cast<VkDevice>(binding.device);
    family_ = binding.vulkan_queue_family;
    if (!instance_ || !physical_ || !device_) {
        error = "The Vulkan binding names no instance, physical device or device";
        return false;
    }
    vk_.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(binding.vulkan_get_instance_proc_addr);
    if (!vk_.vkGetInstanceProcAddr) {
        // The loader the game loaded.
        HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        if (!loader) loader = loader_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (loader) {
            vk_.vkGetInstanceProcAddr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        }
    }
    if (!vk_.vkGetInstanceProcAddr) {
        error = "No vkGetInstanceProcAddr (the Vulkan loader is not loaded)";
        return false;
    }
    const char* missing = nullptr;
    const auto instance_function = [&](const char* name) { return vk_.vkGetInstanceProcAddr(instance_, name); };
#define VM_LOAD_INSTANCE(name)                                                                                     \
    vk_.name = reinterpret_cast<PFN_##name>(instance_function(#name));                                             \
    if (!vk_.name && !missing) missing = #name;
#define VM_LOAD_INSTANCE_11(name)                                                                                  \
    vk_.name = reinterpret_cast<PFN_##name>(instance_function(#name "KHR"));                                       \
    if (!vk_.name) vk_.name = reinterpret_cast<PFN_##name>(instance_function(#name));                              \
    if (!vk_.name && !missing) missing = #name "KHR";
    VM_INSTANCE_FUNCTIONS(VM_LOAD_INSTANCE)
    VM_INSTANCE_FUNCTIONS_11(VM_LOAD_INSTANCE_11)
#undef VM_LOAD_INSTANCE
#undef VM_LOAD_INSTANCE_11
    if (missing) {
        error = std::string("The game's Vulkan instance lacks ") + missing +
                " (the mirror was turned on after the game started: restart the game)";
        return false;
    }
    const auto device_function = [&](const char* name) { return vk_.vkGetDeviceProcAddr(device_, name); };
    vk_.vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        device_function("vkGetMemoryWin32HandlePropertiesKHR"));
    if (!vk_.vkGetMemoryWin32HandlePropertiesKHR) {
        error = "The game's Vulkan device was made without VK_KHR_external_memory_win32, which the mirror shares its "
                "picture through (the mirror was turned on after the game started: restart the game; or the "
                "graphics driver does not offer it)";
        return false;
    }
#define VM_LOAD_DEVICE(name)                                                                                       \
    vk_.name = reinterpret_cast<PFN_##name>(device_function(#name));                                               \
    if (!vk_.name && !missing) missing = #name;
    VM_DEVICE_FUNCTIONS(VM_LOAD_DEVICE)
#undef VM_LOAD_DEVICE
    if (missing) {
        error = std::string("The game's Vulkan device lacks ") + missing;
        return false;
    }
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id;
    vk_.vkGetPhysicalDeviceProperties2(physical_, &properties);
    if (!id.deviceLUIDValid) {
        error = "The game's Vulkan device has no adapter LUID";
        return false;
    }
    static_assert(sizeof(luid_) == VK_LUID_SIZE);
    std::memcpy(&luid_, id.deviceLUID, VK_LUID_SIZE);
    timestamp_period_ns_ = properties.properties.limits.timestampPeriod;
    vk_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    vk_.vkGetDeviceQueue(device_, family_, binding.vulkan_queue_index, &queue_);
    if (queue_ == VK_NULL_HANDLE) {
        error = "The game's Vulkan device has no such queue";
        return false;
    }
    return true;
}

bool VulkanImages::Blittable(VkFormat format) {
    if (const auto known = blittable_.find(static_cast<int>(format)); known != blittable_.end()) {
        return known->second;
    }
    VkFormatProperties properties{};
    vk_.vkGetPhysicalDeviceFormatProperties(physical_, format, &properties);
    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const bool blittable = (properties.optimalTilingFeatures & kNeeded) == kNeeded;
    blittable_[static_cast<int>(format)] = blittable;
    return blittable;
}

XrExtent2Di VulkanImages::PictureExtent(VkFormat format, const SubmittedView& view, const MirrorDraw& draw) {
    const XrExtent2Di whole = view.rect.extent;
    if (full_copy_ || !Blittable(format)) return whole;
    // How much of the rectangle the window's picture spans (its corners).
    double least_u = 1e9, most_u = -1e9, least_v = 1e9, most_v = -1e9;
    for (const double x : {-1.0, 1.0}) {
        for (const double y : {-1.0, 1.0}) {
            double u = 0.0, v = 0.0;
            if (!MirrorMap(draw.homography, x, y, u, v)) return whole;
            least_u = std::min(least_u, u);
            most_u = std::max(most_u, u);
            least_v = std::min(least_v, v);
            most_v = std::max(most_v, v);
        }
    }
    const double span_u = most_u - least_u;
    const double span_v = most_v - least_v;
    if (!(span_u > 1e-6) || !(span_v > 1e-6)) return whole;
    // A texel of the copy for each pixel of the window, on the axis that
    // needs more; in eighths, so a head that moves does not change the size
    // from frame to frame, and never below half.
    const double needed = std::max(static_cast<double>(draw.width) / (span_u * whole.width),
                                   static_cast<double>(draw.height) / (span_v * whole.height));
    const double scale = std::clamp(std::ceil(needed * 8.0) / 8.0, 0.5, 1.0);
    if (scale >= 1.0) return whole;
    return {std::max(1, static_cast<std::int32_t>(std::ceil(whole.width * scale))),
            std::max(1, static_cast<std::int32_t>(std::ceil(whole.height * scale)))};
}

// Only with nothing of either queue's using them (MirrorRelay::Idle).
void VulkanImages::FreeImages() {
    for (Slot& slot : slots_) {
        if (slot.image) vk_.vkDestroyImage(device_, slot.image, nullptr);
        if (slot.memory) vk_.vkFreeMemory(device_, slot.memory, nullptr);
        slot.image = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        // The mirror forgets its view of the texture: another made later may
        // have its address.
        if (mirror_ && slot.texture) mirror_->Retire(slot.texture);
        slot.texture.Reset();
        slot.source = XR_NULL_HANDLE;
    }
    format_ = VK_FORMAT_UNDEFINED;
    width_ = 0;
    height_ = 0;
}

// One still being made is let go of by the thread making it, when it is
// done; one made and not taken, here.
void VulkanImages::AbandonShared() {
    if (!shared_) return;
    {
        std::scoped_lock lock(shared_->mutex);
        shared_->abandoned = true;
        shared_->device_gone = lost_;
        if (lost_) {
            // Made on a device that is gone, or still being made: left to it.
            for (ComPtr<ID3D12Resource>& texture : shared_->textures) texture.Reset();
        } else if (!shared_->busy && !shared_->failed) {
            DestroyImages(vk_, device_, shared_->images, shared_->memories);
            DestroyCommands(vk_, device_, shared_->pool, shared_->fences);
            for (ComPtr<ID3D12Resource>& texture : shared_->textures) texture.Reset();
        }
    }
    shared_.reset();
}

bool VulkanImages::PollCopy() {
    const int copying = relay_.Copying();
    if (copying < 0) return true;
    Slot& slot = slots_[copying];
    const VkResult status = vk_.vkGetFenceStatus(device_, slot.fence);
    if (status == VK_NOT_READY) return true;
    if (status != VK_SUCCESS) {
        lost_ = true;
        return false;
    }
    relay_.CopyDone(copying);
    if (slot.timed && queries_) {
        std::uint64_t stamps[2]{};
        if (vk_.vkGetQueryPoolResults(device_, queries_, 0, 2, sizeof(stamps), stamps, sizeof(std::uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            copy_gpu_ms_ = static_cast<double>(stamps[1] - stamps[0]) * timestamp_period_ns_ / 1e6;
        }
    }
    return true;
}

void VulkanImages::WaitForCopy() {
    const int copying = relay_.Copying();
    if (copying < 0 || lost_) return;
    const VkResult status = vk_.vkWaitForFences(device_, 1, &slots_[copying].fence, VK_TRUE, kCopyWaitNs);
    if (status == VK_SUCCESS) {
        relay_.CopyDone(copying);
    } else if (status != VK_TIMEOUT) {
        lost_ = true;
    }
}

// On the game's queue, inside its xrEndFrame: the view's rectangle of its
// array slice into the shared image's top-left corner, at `picture`'s size
// (scaled down where that is smaller). The game's image is
// in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable: how a colour image is
// released) and is put back; the shared image is taken with its content
// undefined and handed to Direct3D 12 (the external queue family), as the
// Vulkan bridge hands over the images it resolves into.
bool VulkanImages::Copy(Slot& slot, VkImage source, const SubmittedView& view, const XrExtent2Di& picture,
                        std::string& error) {
    VkResult result = vk_.vkResetFences(device_, 1, &slot.fence);
    if (result == VK_SUCCESS) result = vk_.vkResetCommandBuffer(slot.commands, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (result == VK_SUCCESS) result = vk_.vkBeginCommandBuffer(slot.commands, &begin);
    if (result != VK_SUCCESS) {
        error = VkFailure("Starting the mirror's copy", result);
        return false;
    }
    const VkCommandBuffer commands = slot.commands;
    slot.timed = timing_wanted_ && queries_ != VK_NULL_HANDLE;
    if (slot.timed) vk_.vkCmdResetQueryPool(commands, queries_, 0, 2);
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = source;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, view.array_slice, 1};
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = slot.image;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 2, barriers);
    if (slot.timed) vk_.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 0);
    if (picture.width == view.rect.extent.width && picture.height == view.rect.extent.height) {
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, view.array_slice, 1};
        region.srcOffset = {view.rect.offset.x, view.rect.offset.y, 0};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {static_cast<std::uint32_t>(view.rect.extent.width),
                         static_cast<std::uint32_t>(view.rect.extent.height), 1};
        vk_.vkCmdCopyImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, view.array_slice, 1};
        region.srcOffsets[0] = {view.rect.offset.x, view.rect.offset.y, 0};
        region.srcOffsets[1] = {view.rect.offset.x + view.rect.extent.width,
                                view.rect.offset.y + view.rect.extent.height, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {picture.width, picture.height, 1};
        vk_.vkCmdBlitImage(commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
    }
    if (slot.timed) vk_.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcQueueFamilyIndex = family_;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    vk_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 2, barriers);
    result = vk_.vkEndCommandBuffer(commands);
    if (result != VK_SUCCESS) {
        error = VkFailure("Recording the mirror's copy", result);
        return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    {
        // A VkQueue is externally synchronized: the mutex of everything else
        // here that uses the game's queue (the OSD's and the fill's uploads,
        // Frame Generation, the runtime's frame calls), for the submit alone.
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        if (g_submit_hook) g_submit_hook();
        result = vk_.vkQueueSubmit(queue_, 1, &submit, slot.fence);
    }
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) lost_ = true;
        error = VkFailure("Submitting the mirror's copy to the game's queue", result);
        return false;
    }
    ++copies_;
    return true;
}

MirrorDrawResult VulkanImages::DrawMirror(const SubmittedView& view, const MirrorDraw& draw, std::string& error) {
    if (lost_) {
        error = "the game's Vulkan device is lost";
        return MirrorDrawResult::kFailed;
    }
    if (host_failed_) {
        error = host_error_;
        return MirrorDrawResult::kFailed;
    }
    const auto found = swapchains_.find(view.swapchain);
    if (found == swapchains_.end() || view.image_index >= found->second.size()) {
        error = "the swapchain's images are not known";
        return MirrorDrawResult::kFailed;
    }
    const auto format = static_cast<VkFormat>(view.format);
    const DXGI_FORMAT dxgi = DxgiOf(format);
    const SubmittedFormat shared_format = ClassifyFormat(SubmittedImageApi::kD3D12, dxgi);
    if (dxgi == DXGI_FORMAT_UNKNOWN || shared_format.kind == SubmittedPixelKind::kUnknown) {
        error = "format " + std::to_string(view.format) + " is not one that can be shown";
        return MirrorDrawResult::kFailed;
    }
    if (view.sample_count > 1) {
        error = "multisampled images are not shown";
        return MirrorDrawResult::kFailed;
    }
    if ((view.usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) == 0) {
        error = "the game's images were made without the usage their copy needs (the mirror was turned on after "
                "the game made its swapchains, or the runtime refused the usage): restart the game";
        return MirrorDrawResult::kFailed;
    }
    if (view.array_slice >= view.array_size) {
        error = "array slice " + std::to_string(view.array_slice) + " is outside the image's " +
                std::to_string(view.array_size);
        return MirrorDrawResult::kFailed;
    }
    const XrRect2Di& rect = view.rect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
        static_cast<std::uint64_t>(rect.offset.x) + static_cast<std::uint64_t>(rect.extent.width) > view.width ||
        static_cast<std::uint64_t>(rect.offset.y) + static_cast<std::uint64_t>(rect.extent.height) > view.height) {
        error = "the rectangle is empty or outside the image";
        return MirrorDrawResult::kFailed;
    }
    if (draw.width == 0 || draw.height == 0) {
        error = "the window has no picture";
        return MirrorDrawResult::kFailed;
    }

    // The Direct3D 12 device the window is drawn with: made once, on the
    // window's thread.
    if (!host_device_) {
        if (!host_) {
            host_ = std::make_shared<Host>();
            Run(draw, [host = host_, luid = luid_] { MakeHost(host, luid); });
        }
        std::unique_lock lock(host_->mutex);
        if (host_->busy) return MirrorDrawResult::kSkipped;
        if (host_->failed) {
            host_failed_ = true;
            host_error_ = host_->error;
            error = host_error_;
            lock.unlock();
            host_.reset();
            return MirrorDrawResult::kFailed;
        }
        host_device_ = std::move(host_->device);
        host_queue_ = std::move(host_->queue);
        lock.unlock();
        host_.reset();
        mirror_ = std::make_unique<D3D12Mirror>(host_device_.Get(), host_queue_.Get());
    }
    if (!draw.window && !test_target_) {
        error = "no window to draw into";
        return MirrorDrawResult::kFailed;
    }

    // What each queue has finished since the last call.
    if (mirror_->Idle()) relay_.ShowsDone();
    if (!PollCopy()) {
        error = "the game's Vulkan device is lost";
        return MirrorDrawResult::kFailed;
    }

    if (timing_wanted_ && !queries_) {
        VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queries.queryCount = 2;
        if (vk_.vkCreateQueryPool(device_, &queries, nullptr, &queries_) != VK_SUCCESS) queries_ = VK_NULL_HANDLE;
    }

    // The shared images: of the rectangle's size at first; outgrown, made
    // again of the size a rectangle can reach at most on the axis that grew
    // (a game that changes its render size from frame to frame).
    // (Their size is the copy's: the rectangle's, or scaled down for the window.)
    const XrExtent2Di picture = PictureExtent(format, view, draw);
    const auto rect_width = static_cast<std::uint32_t>(picture.width);
    const auto rect_height = static_cast<std::uint32_t>(picture.height);
    const auto scaled = [](std::uint32_t full, std::int32_t of, std::int32_t to) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(full) * to + of - 1) / of);
    };
    if (format_ != format || rect_width > width_ || rect_height > height_) {
        relay_.DropReady();
        if (!relay_.Idle()) return MirrorDrawResult::kSkipped;
        std::uint32_t want_width = rect_width;
        std::uint32_t want_height = rect_height;
        if (format_ == format && width_ != 0) {
            want_width = rect_width > width_
                             ? std::max(scaled(view.width, rect.extent.width, picture.width), rect_width)
                             : width_;
            want_height = rect_height > height_
                              ? std::max(scaled(view.height, rect.extent.height, picture.height), rect_height)
                              : height_;
        }
        if (shared_ && (shared_want_format_ != format || shared_want_width_ < rect_width ||
                        shared_want_height_ < rect_height)) {
            AbandonShared();
        }
        if (!shared_) {
            // Made, imported into the game's device and (the first time)
            // given their command buffers and fences on the window's
            // thread, where the images they replace are let go of too:
            // none of it is this thread's time.
            SharedRequest request;
            request.vk = vk_;
            request.physical = physical_;
            request.device = device_;
            request.memory = memory_;
            request.family = family_;
            request.host = host_device_;
            request.format = format;
            request.dxgi = dxgi;
            request.width = want_width;
            request.height = want_height;
            request.make_commands = pool_ == VK_NULL_HANDLE;
            for (int i = 0; i < MirrorRelay::kSlots; ++i) {
                Slot& slot = slots_[i];
                // The mirror forgets its view of the texture: another made
                // later may have its address.
                if (mirror_ && slot.texture) mirror_->Retire(slot.texture);
                request.old_textures[i] = std::move(slot.texture);
                request.old_images[i] = slot.image;
                request.old_memories[i] = slot.memory;
                slot.image = VK_NULL_HANDLE;
                slot.memory = VK_NULL_HANDLE;
                slot.source = XR_NULL_HANDLE;
            }
            format_ = VK_FORMAT_UNDEFINED;
            width_ = 0;
            height_ = 0;
            shared_ = std::make_shared<Shared>();
            shared_want_format_ = format;
            shared_want_width_ = want_width;
            shared_want_height_ = want_height;
            Run(draw, [shared = shared_, request = std::move(request)]() mutable {
                MakeShared(shared, std::move(request));
            });
        }
        std::unique_lock lock(shared_->mutex);
        if (shared_->busy) return MirrorDrawResult::kSkipped;
        if (shared_->failed) {
            error = shared_->error;
            lock.unlock();
            shared_.reset();
            return MirrorDrawResult::kFailed;
        }
        for (int i = 0; i < MirrorRelay::kSlots; ++i) {
            slots_[i].texture = std::move(shared_->textures[i]);
            slots_[i].image = shared_->images[i];
            slots_[i].memory = shared_->memories[i];
            if (shared_->pool) {
                slots_[i].commands = shared_->commands[i];
                slots_[i].fence = shared_->fences[i];
            }
        }
        if (shared_->pool) pool_ = shared_->pool;
        lock.unlock();
        shared_.reset();
        format_ = format;
        width_ = shared_want_width_;
        height_ = shared_want_height_;
        relay_.Reset();
    }

    // The newest finished copy into the window.
    bool shown = false;
    const auto now = std::chrono::steady_clock::now();
    if (const int ready = relay_.ToShow(); ready >= 0 && now - slots_[ready].copied > kStalePicture) {
        relay_.DropReady();
    }
    if (const int ready = relay_.ToShow(); ready >= 0) {
        const Slot& slot = slots_[ready];
        SubmittedView shared_view;
        shared_view.resolved = true;
        shared_view.format = dxgi;
        shared_view.rect = {{0, 0}, slot.extent};
        shared_view.width = width_;
        shared_view.height = height_;
        MirrorDraw shared_draw = draw;
        shared_draw.homography = slot.homography;
        switch (mirror_->Draw(slot.texture.Get(), shared_view, shared_format, test_target_, shared_draw, error,
                              D3D12_RESOURCE_STATE_COMMON)) {
        case MirrorDrawResult::kShown:
            relay_.ShowStarted(ready);
            shown = true;
            break;
        case MirrorDrawResult::kSkipped:
            break;
        case MirrorDrawResult::kFailed:
            return MirrorDrawResult::kFailed;
        }
    }

    // This frame's eye, for a later call to show.
    if (const int free = relay_.ToCopy(); free >= 0) {
        Slot& slot = slots_[free];
        if (!Copy(slot, found->second[view.image_index], view, picture, error)) return MirrorDrawResult::kFailed;
        relay_.CopyStarted(free);
        slot.source = view.swapchain;
        slot.extent = picture;
        slot.homography = draw.homography;
        slot.copied = now;
    }
    return shown ? MirrorDrawResult::kShown : MirrorDrawResult::kSkipped;
}

void VulkanImages::Reset() {
    WaitForCopy();
    const bool copy_done = relay_.Copying() < 0;
    if (mirror_) mirror_->Reset();
    mirror_.reset();
    // Images being made on the window's thread use the game's device: waited
    // for, bounded. The window's thread runs every task it took before it
    // ends (MirrorWindow), so this waits only for one that is running now.
    // (A device that is gone is not waited for: the task is told, and
    // touches nothing of it.)
    for (int wait = 0; shared_ && !lost_ && wait < 2000; ++wait) {
        {
            std::scoped_lock lock(shared_->mutex);
            if (!shared_->busy) break;
        }
        Sleep(1);
    }
    AbandonShared();
    if (host_) host_.reset();
    if (copy_done && !lost_) {
        FreeImages();
        for (Slot& slot : slots_) {
            if (slot.fence) vk_.vkDestroyFence(device_, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
            slot.commands = VK_NULL_HANDLE;
        }
        if (queries_) vk_.vkDestroyQueryPool(device_, queries_, nullptr);
        if (pool_) vk_.vkDestroyCommandPool(device_, pool_, nullptr);
    } else {
        // Never freed under the game's queue: left to the game's device.
        for (Slot& slot : slots_) {
            slot.texture.Detach();
            slot = {};
        }
        format_ = VK_FORMAT_UNDEFINED;
        width_ = 0;
        height_ = 0;
    }
    queries_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    relay_.Reset();
    swapchains_.clear();
    host_device_.Reset();
    host_queue_.Reset();
    host_failed_ = false;
    host_error_.clear();
    test_target_ = nullptr;
}

VulkanImages* AsVulkan(SubmittedImageGpu& gpu) {
    return gpu.Api() == SubmittedImageApi::kVulkan ? dynamic_cast<VulkanImages*>(&gpu) : nullptr;
}

} // namespace

std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageVulkan(const SubmittedImageBinding& binding,
                                                              std::string& error) {
    return VulkanImages::Make(binding, error);
}

bool CaptureVulkanBinding(const void* entry, SubmittedImageBinding& captured) {
    const auto* base = static_cast<const XrBaseInStructure*>(entry);
    if (!base || base->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) return false;
    const auto& binding = *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(base);
    captured.type = base->type;
    captured.device = binding.device;
    captured.vulkan_instance = binding.instance;
    captured.vulkan_physical_device = binding.physicalDevice;
    captured.vulkan_queue_family = binding.queueFamilyIndex;
    captured.vulkan_queue_index = binding.queueIndex;
    return true;
}

bool TrackSubmittedSwapchainVulkan(
    SubmittedImageGpu& gpu, XrSwapchain swapchain, std::uint32_t count,
    const std::function<XrResult(XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*)>& enumerate,
    std::string& error) {
    std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    const XrResult result =
        enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) {
        error = "xrEnumerateSwapchainImages failed (" + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    return gpu.Track(swapchain, reinterpret_cast<const XrSwapchainImageBaseHeader*>(images.data()), count, error);
}

void* MirrorVulkanHostDeviceForTests(SubmittedImageGpu& gpu) {
    const VulkanImages* vulkan = AsVulkan(gpu);
    return vulkan ? vulkan->HostDevice() : nullptr;
}

void* MirrorVulkanHostQueueForTests(SubmittedImageGpu& gpu) {
    const VulkanImages* vulkan = AsVulkan(gpu);
    return vulkan ? vulkan->HostQueue() : nullptr;
}

void SetMirrorVulkanTargetForTests(SubmittedImageGpu& gpu, void* d3d12_resource) {
    if (VulkanImages* vulkan = AsVulkan(gpu)) vulkan->SetTarget(static_cast<ID3D12Resource*>(d3d12_resource));
}

void SetMirrorVulkanSubmitHookForTests(void (*hook)()) {
    g_submit_hook = hook;
}

void SetMirrorVulkanImportDelayForTests(std::uint32_t milliseconds) {
    g_import_delay_ms.store(milliseconds, std::memory_order_release);
}

void SetMirrorVulkanFullCopyForTests(SubmittedImageGpu& gpu, bool on) {
    if (VulkanImages* vulkan = AsVulkan(gpu)) vulkan->SetFullCopy(on);
}

void SetMirrorVulkanTimingForTests(SubmittedImageGpu& gpu, bool on) {
    if (VulkanImages* vulkan = AsVulkan(gpu)) vulkan->SetTiming(on);
}

double MirrorVulkanCopyGpuMsForTests(SubmittedImageGpu& gpu) {
    const VulkanImages* vulkan = AsVulkan(gpu);
    return vulkan ? vulkan->CopyGpuMs() : -1.0;
}

std::uint64_t MirrorVulkanCopiesForTests(SubmittedImageGpu& gpu) {
    const VulkanImages* vulkan = AsVulkan(gpu);
    return vulkan ? vulkan->Copies() : 0;
}

} // namespace depthxr
