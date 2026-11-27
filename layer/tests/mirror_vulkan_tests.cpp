// The stabilized mirror window for a Vulkan game (submitted_image_vulkan.cpp),
// on a GPU whose Vulkan device shares memory with Direct3D 12 (77 without
// one: there is no software Vulkan device that does). A Vulkan "game" (an
// instance and device of the test's, made with the extensions VectorXR asks
// a game's device for through XR_KHR_vulkan_enable) owns images that stand
// in for a runtime's swapchain images; the object under test copies them on
// the game's queue and draws them with its own Direct3D 12 device, into a
// render target the test reads back or into a window that is never put on
// the desktop. The Khronos validation layer (vulkan_test_validation.h) and
// the Direct3D 12 debug layer check every case. With
// VECTORXR_TESTS_REQUIRE_HW=1 a skip fails. `measure`: the costs on the real
// adapter, by hand, through the GPU lock.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "depthxr/mirror.h"
#include "depthxr/mirror_window.h"
#include "depthxr/submitted_image.h"
#include "test_adapter.h"
#include "vulkan_test_validation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace depthxr;
using Microsoft::WRL::ComPtr;

constexpr int kSkip = 77;
int checks = 0;
bool g_validation = false;
vulkan_test::Messages g_messages;

void Expect(bool ok, const std::string& why) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", why.c_str());
        std::fflush(stderr);
        std::exit(1);
    }
}

#define GAME_FUNCTIONS(X)                                                                                          \
    X(vkCreateInstance)                                                                                            \
    X(vkEnumerateInstanceLayerProperties)                                                                          \
    X(vkGetInstanceProcAddr)                                                                                       \
    X(vkDestroyInstance)                                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                                               \
    X(vkGetPhysicalDeviceProperties2)                                                                              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                    \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
    X(vkEnumerateDeviceExtensionProperties)                                                                        \
    X(vkCreateDevice)                                                                                              \
    X(vkDestroyDevice)                                                                                             \
    X(vkGetDeviceQueue)                                                                                            \
    X(vkCreateImage)                                                                                               \
    X(vkDestroyImage)                                                                                              \
    X(vkGetImageMemoryRequirements)                                                                                \
    X(vkAllocateMemory)                                                                                            \
    X(vkFreeMemory)                                                                                                \
    X(vkBindImageMemory)                                                                                           \
    X(vkCreateBuffer)                                                                                              \
    X(vkDestroyBuffer)                                                                                             \
    X(vkGetBufferMemoryRequirements)                                                                               \
    X(vkBindBufferMemory)                                                                                          \
    X(vkMapMemory)                                                                                                 \
    X(vkUnmapMemory)                                                                                               \
    X(vkCreateCommandPool)                                                                                         \
    X(vkDestroyCommandPool)                                                                                        \
    X(vkAllocateCommandBuffers)                                                                                    \
    X(vkFreeCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                        \
    X(vkEndCommandBuffer)                                                                                          \
    X(vkCmdPipelineBarrier)                                                                                        \
    X(vkCmdClearColorImage)                                                                                        \
    X(vkCmdCopyBufferToImage)                                                                                      \
    X(vkQueueSubmit)                                                                                               \
    X(vkQueueWaitIdle)

// The loader's own exports (its trampolines dispatch by handle).
struct Loader {
#define GAME_DECLARE(name) PFN_##name name{nullptr};
    GAME_FUNCTIONS(GAME_DECLARE)
#undef GAME_DECLARE
    HMODULE module{nullptr};
};
Loader vk;

bool LoadVulkan() {
    vulkan_test::ConfigureVulkanLoader();
    vk.module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!vk.module) return false;
    bool all = true;
#define GAME_LOAD(name)                                                                                            \
    vk.name = reinterpret_cast<PFN_##name>(GetProcAddress(vk.module, #name));                                      \
    all = all && vk.name;
    GAME_FUNCTIONS(GAME_LOAD)
#undef GAME_LOAD
    return all;
}

struct Game {
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t family{0};
    VkCommandPool pool{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory{};
    LUID luid{};
    std::string name;
};

bool MakeInstance(Game& game) {
    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    g_validation = vulkan_test::ValidationAvailable(vk.vkEnumerateInstanceLayerProperties);
    if (g_validation) {
        layers.push_back(vulkan_test::kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mirror_vulkan_tests";
    app.apiVersion = VK_API_VERSION_1_1;
    VkDebugUtilsMessengerCreateInfoEXT messenger = vulkan_test::MessengerInfo(&g_messages);
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (g_validation) create.pNext = &messenger;
    create.pApplicationInfo = &app;
    create.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create.ppEnabledLayerNames = layers.data();
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    if (vk.vkCreateInstance(&create, nullptr, &game.instance) != VK_SUCCESS) return false;
    if (g_validation) {
        const auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(game.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (make) make(game.instance, &messenger, nullptr, &game.messenger);
    }
    return true;
}

// The first hardware device (FG_TEST_ADAPTER's, when set) that offers
// VK_KHR_external_memory_win32 and has an adapter LUID.
bool FindPhysical(Game& game, std::string& why) {
    std::uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(game.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk.vkEnumeratePhysicalDevices(game.instance, &count, devices.data());
    const std::optional<std::string> wanted = test_adapter::Wanted();
    for (const VkPhysicalDevice device : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        vk.vkGetPhysicalDeviceProperties2(device, &properties);
        if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || !id.deviceLUIDValid) continue;
        if (wanted && !test_adapter::NameMatches(properties.properties.deviceName, *wanted)) continue;
        std::uint32_t extension_count = 0;
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> extensions(extension_count);
        vk.vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data());
        const bool shares = std::any_of(extensions.begin(), extensions.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, "VK_KHR_external_memory_win32") == 0;
        });
        if (!shares) continue;
        std::uint32_t families = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
        std::vector<VkQueueFamilyProperties> family_properties(families);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(device, &families, family_properties.data());
        for (std::uint32_t family = 0; family < families; ++family) {
            if (family_properties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                game.physical = device;
                game.family = family;
                game.name = properties.properties.deviceName;
                std::memcpy(&game.luid, id.deviceLUID, VK_LUID_SIZE);
                vk.vkGetPhysicalDeviceMemoryProperties(device, &game.memory);
                return true;
            }
        }
    }
    why = "no Vulkan GPU that shares memory with Direct3D 12 (VK_KHR_external_memory_win32)";
    return false;
}

// `sharing`: with the extension the mirror needs, as VectorXR adds it to a
// game's device; without, the device of a game started with the mirror off.
bool MakeDevice(Game& game, bool sharing) {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = game.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    std::vector<const char*> extensions;
    if (sharing) extensions.push_back("VK_KHR_external_memory_win32");
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    if (vk.vkCreateDevice(game.physical, &create, nullptr, &game.device) != VK_SUCCESS) return false;
    vk.vkGetDeviceQueue(game.device, game.family, 0, &game.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = game.family;
    return vk.vkCreateCommandPool(game.device, &pool, nullptr, &game.pool) == VK_SUCCESS;
}

void DestroyDevice(Game& game) {
    if (!game.device) return;
    vk.vkQueueWaitIdle(game.queue);
    if (game.pool) vk.vkDestroyCommandPool(game.device, game.pool, nullptr);
    vk.vkDestroyDevice(game.device, nullptr);
    game.device = VK_NULL_HANDLE;
    game.pool = VK_NULL_HANDLE;
}

std::uint32_t MemoryType(const Game& game, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < game.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (game.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    Expect(false, "The game's device has a memory type for the test");
    return 0;
}

// The game's work: recorded, submitted to its queue, and (`wait`) finished.
void GameSubmit(Game& game, const std::function<void(VkCommandBuffer)>& record, bool wait = true) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = game.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Expect(vk.vkAllocateCommandBuffers(game.device, &allocate, &commands) == VK_SUCCESS, "A command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(commands, &begin);
    record(commands);
    vk.vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    Expect(vk.vkQueueSubmit(game.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS, "The game's work is submitted");
    if (wait) {
        vk.vkQueueWaitIdle(game.queue);
        vk.vkFreeCommandBuffers(game.device, game.pool, 1, &commands);
    }
}

// What stands in for a runtime's swapchain: images of the game's device,
// resting in COLOR_ATTACHMENT_OPTIMAL as XR_KHR_vulkan_enable releases them.
struct Swapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t layers{1};
    XrSwapchainUsageFlags usage{0};
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memories;
};

std::uintptr_t g_next_swapchain = 0x5000;

Swapchain MakeSwapchain(Game& game, VkFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                        std::uint32_t count, bool transfer_source = true) {
    Swapchain chain;
    chain.handle = reinterpret_cast<XrSwapchain>(g_next_swapchain++);
    chain.format = format;
    chain.width = width;
    chain.height = height;
    chain.layers = layers;
    chain.usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                  (transfer_source ? XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0u);
    for (std::uint32_t i = 0; i < count; ++i) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = format;
        create.extent = {width, height, 1};
        create.mipLevels = 1;
        create.arrayLayers = layers;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       (transfer_source ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u);
        create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        Expect(vk.vkCreateImage(game.device, &create, nullptr, &image) == VK_SUCCESS, "A swapchain image is made");
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(game.device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            MemoryType(game, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Expect(vk.vkAllocateMemory(game.device, &allocate, nullptr, &memory) == VK_SUCCESS &&
                   vk.vkBindImageMemory(game.device, image, memory, 0) == VK_SUCCESS,
               "A swapchain image has memory");
        chain.images.push_back(image);
        chain.memories.push_back(memory);
        GameSubmit(game, [&](VkCommandBuffer commands) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
            vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                    &barrier);
        });
    }
    return chain;
}

void DestroySwapchain(Game& game, Swapchain& chain) {
    vk.vkQueueWaitIdle(game.queue);
    for (const VkImage image : chain.images) vk.vkDestroyImage(game.device, image, nullptr);
    for (const VkDeviceMemory memory : chain.memories) vk.vkFreeMemory(game.device, memory, nullptr);
    chain.images.clear();
    chain.memories.clear();
}

// The game renders: `bytes` (width * height texels of `texel_bytes`) into a
// layer of an image, which is left in COLOR_ATTACHMENT_OPTIMAL.
void GameFill(Game& game, const Swapchain& chain, std::uint32_t index, std::uint32_t layer, const void* bytes,
              std::size_t texel_bytes) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(chain.width) * chain.height * texel_bytes;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create.size = size;
    create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    Expect(vk.vkCreateBuffer(game.device, &create, nullptr, &buffer) == VK_SUCCESS, "A staging buffer is made");
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(game.device, buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        MemoryType(game, requirements.memoryTypeBits,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    Expect(vk.vkAllocateMemory(game.device, &allocate, nullptr, &memory) == VK_SUCCESS &&
               vk.vkBindBufferMemory(game.device, buffer, memory, 0) == VK_SUCCESS &&
               vk.vkMapMemory(game.device, memory, 0, size, 0, &mapped) == VK_SUCCESS,
           "The staging buffer has memory");
    std::memcpy(mapped, bytes, static_cast<std::size_t>(size));
    vk.vkUnmapMemory(game.device, memory);
    const VkImage image = chain.images[index];
    GameSubmit(game, [&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        copy.imageExtent = {chain.width, chain.height, 1};
        vk.vkCmdCopyBufferToImage(commands, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
        std::swap(barrier.oldLayout, barrier.newLayout);
        vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                &barrier);
    });
    vk.vkDestroyBuffer(game.device, buffer, nullptr);
    vk.vkFreeMemory(game.device, memory, nullptr);
}

// GPU work that takes the game's queue a while: a large image cleared over
// and over, not waited for.
struct Busy {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
};

Busy MakeBusy(Game& game) {
    Swapchain large = MakeSwapchain(game, VK_FORMAT_R16G16B16A16_SFLOAT, 8192, 8192, 1, 1);
    return {large.images[0], large.memories[0]};
}

void GameBusy(Game& game, const Busy& busy, int clears) {
    GameSubmit(
        game,
        [&](VkCommandBuffer commands) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = busy.image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            for (int i = 0; i < clears; ++i) {
                const VkClearColorValue color{{i * 0.001f, 0.5f, 0.25f, 1.0f}};
                vk.vkCmdClearColorImage(commands, busy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                        &barrier.subresourceRange);
            }
            std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
            std::swap(barrier.oldLayout, barrier.newLayout);
            vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                    &barrier);
        },
        false);
}

std::uint8_t Pattern(std::uint32_t x, std::uint32_t y, std::uint32_t seed, std::uint32_t channel) {
    return static_cast<std::uint8_t>((x * (3 + channel * 2) + y * (5 + channel) + seed * 37 + channel * 61) & 0xff);
}

std::vector<std::uint8_t> PatternImage(std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                data[(static_cast<std::size_t>(y) * width + x) * 4 + c] = Pattern(x, y, seed, c);
            }
        }
    }
    return data;
}

std::unique_ptr<SubmittedImageGpu> MakeGpu(const Game& game, std::string& error) {
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = game.instance;
    binding.physicalDevice = game.physical;
    binding.device = game.device;
    binding.queueFamilyIndex = game.family;
    binding.queueIndex = 0;
    return CreateSubmittedImageGpu(&binding, error);
}

void Track(SubmittedImageGpu& gpu, const Swapchain& chain) {
    std::vector<XrSwapchainImageVulkanKHR> images(chain.images.size(), {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    for (std::size_t i = 0; i < images.size(); ++i) images[i].image = chain.images[i];
    std::string error;
    Expect(gpu.Track(chain.handle, reinterpret_cast<const XrSwapchainImageBaseHeader*>(images.data()),
                     static_cast<std::uint32_t>(images.size()), error) &&
               gpu.Tracked(chain.handle),
           "The swapchain's Vulkan images are tracked: " + error);
}

SubmittedView View(const Swapchain& chain, std::uint32_t index, std::uint32_t slice, const XrRect2Di& rect) {
    SubmittedView view;
    view.resolved = true;
    view.swapchain = chain.handle;
    view.image_index = index;
    view.array_slice = slice;
    view.rect = rect;
    view.format = chain.format;
    view.width = chain.width;
    view.height = chain.height;
    view.array_size = chain.layers;
    view.sample_count = 1;
    view.usage = chain.usage;
    return view;
}

// The whole rectangle, texel for texel, in a picture of the rectangle's size.
MirrorHomography Whole() {
    const XrFovf fov{-0.7f, 0.7f, 0.6f, -0.6f};
    MirrorCrop crop;
    crop.center_x = 0.5 * (std::tan(fov.angleLeft) + std::tan(fov.angleRight));
    crop.center_y = 0.5 * (std::tan(fov.angleUp) + std::tan(fov.angleDown));
    crop.half_width = 0.5 * (std::tan(fov.angleRight) - std::tan(fov.angleLeft));
    crop.half_height = 0.5 * (std::tan(fov.angleUp) - std::tan(fov.angleDown));
    return MirrorHomographyFor(fov, crop, MirrorQuat{});
}

// The Direct3D 12 side the test reads the picture back with: the mirror's
// own device and queue.
struct Host {
    ID3D12Device* device{nullptr};
    ID3D12CommandQueue* queue{nullptr};
    ComPtr<ID3D12Fence> fence;
    std::uint64_t serial{0};

    void Idle() {
        if (!fence) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        queue->Signal(fence.Get(), ++serial);
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(serial, event);
        Expect(WaitForSingleObject(event, 20000) == WAIT_OBJECT_0, "The mirror's queue finishes");
        CloseHandle(event);
    }
    ComPtr<ID3D12Resource> Target(std::uint32_t width, std::uint32_t height) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> target;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                                         IID_PPV_ARGS(&target))),
               "A render target of the mirror's device is made");
        return target;
    }
    std::vector<std::uint8_t> Read(ID3D12Resource* target) {
        Idle();
        const D3D12_RESOURCE_DESC desc = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                         IID_PPV_ARGS(&readback))) &&
                   SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&list))),
               "The readback is set up");
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = target;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        Idle();
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        std::uint8_t* mapped = nullptr;
        Expect(SUCCEEDED(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "The readback is mapped");
        for (UINT y = 0; y < desc.Height; ++y) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4,
                        mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                        static_cast<std::size_t>(desc.Width) * 4);
        }
        readback->Unmap(0, nullptr);
        return pixels;
    }
};

// Errors and corruption the Direct3D 12 debug layer stored for the device.
std::string D3D12Errors(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> queue;
    std::string errors;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return errors;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size))) continue;
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            errors += std::string(message->pDescription) + "\n";
        }
    }
    queue->ClearStoredMessages();
    return errors;
}

void CheckClean(const std::string& where, ID3D12Device* device) {
    std::string all;
    for (const std::string& message : g_messages.Take()) all += message + "\n";
    Expect(all.empty(), where + ": Vulkan validation is silent:\n" + all);
    const std::string errors = D3D12Errors(device);
    Expect(errors.empty(), where + ": the Direct3D 12 debug layer is silent:\n" + errors);
}

bool WaitFor(const std::function<bool()>& condition, int milliseconds = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

MirrorWindow::Options WindowOptions(int width, int height) {
    MirrorWindow::Options options;
    options.title = MirrorWindowTitle("mirror_vulkan_tests.exe");
    options.picture_width = width;
    options.picture_height = height;
    options.aspect = static_cast<double>(width) / height;
    options.setting_width = width;
    options.on_desktop = false; // never shown on the desktop
    return options;
}

double Ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

// Calls until a frame is shown, both queues given their time in between
// (a game's frame goes by).
int UntilShown(Game& game, Host& host, SubmittedImageGpu& gpu, const SubmittedView& view, const MirrorDraw& draw,
               const std::string& what, int limit = 400) {
    for (int call = 1; call <= limit; ++call) {
        std::string error;
        const MirrorDrawResult result = gpu.DrawMirror(view, draw, error);
        Expect(result != MirrorDrawResult::kFailed, what + ": a frame is drawn: " + error);
        if (result == MirrorDrawResult::kShown) return call;
        vk.vkQueueWaitIdle(game.queue);
        if (host.device) host.Idle();
        Sleep(1);
    }
    Expect(false, what + ": a frame is shown");
    return 0;
}

int Tests(Game& game, Game& bare) {
    std::string error;
    // A device made without the sharing extension (the mirror turned on after
    // the game started): no object, and the reason says what to do.
    {
        std::unique_ptr<SubmittedImageGpu> none = MakeGpu(bare, error);
        Expect(!none && error.find("VK_KHR_external_memory_win32") != std::string::npos &&
                   error.find("restart the game") != std::string::npos,
               "A Vulkan device that cannot share is refused with the reason: " + error);
    }
    std::unique_ptr<SubmittedImageGpu> gpu = MakeGpu(game, error);
    Expect(gpu != nullptr, "A Vulkan session's images are taken: " + error);
    Expect(std::string(gpu->Name()) == "Vulkan" && gpu->Api() == SubmittedImageApi::kVulkan && gpu->SupportsMirror() &&
               !gpu->SupportsReadback() &&
               gpu->MirrorSourceUsage() == XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT && gpu->RequiredSourceUsage() == 0,
           "The Vulkan object shows the mirror, asks for the copy usage and has no screenshot readback");

    // Two eyes side by side in a two-layer image; the right eye of layer 1.
    constexpr std::uint32_t kWidth = 192, kHeight = 80, kEyeWidth = 96;
    Swapchain chain = MakeSwapchain(game, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 2, 2);
    const std::vector<std::uint8_t> other = PatternImage(kWidth, kHeight, 3);
    std::vector<std::uint8_t> pattern = PatternImage(kWidth, kHeight, 5);
    for (std::uint32_t index = 0; index < 2; ++index) {
        GameFill(game, chain, index, 0, other.data(), 4);
        GameFill(game, chain, index, 1, index == 1 ? pattern.data() : other.data(), 4);
    }
    Track(*gpu, chain);
    const XrRect2Di right_eye{{static_cast<int>(kEyeWidth), 0}, {static_cast<int>(kEyeWidth), static_cast<int>(kHeight)}};
    const SubmittedView view = View(chain, 1, 1, right_eye);
    {
        std::string why;
        Expect(gpu->Readback(view, why) == 0 && why == "Vulkan images cannot be read back yet",
               "A Vulkan image is not read back: " + why);
    }

    // The mirror's own Direct3D 12 device is made at the first draw (here
    // by the caller: no window's thread is offered).
    MirrorDraw draw;
    draw.width = kEyeWidth;
    draw.height = kHeight;
    draw.homography = Whole();
    Expect(gpu->DrawMirror(view, draw, error) == MirrorDrawResult::kFailed && error == "no window to draw into",
           "With neither a window nor a target nothing is drawn: " + error);
    Host host;
    host.device = static_cast<ID3D12Device*>(MirrorVulkanHostDeviceForTests(*gpu));
    host.queue = static_cast<ID3D12CommandQueue*>(MirrorVulkanHostQueueForTests(*gpu));
    Expect(host.device && host.queue, "The mirror has a Direct3D 12 device and queue of its own");
    {
        // On the game's adapter.
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter1> adapter;
        const LUID luid = host.device->GetAdapterLuid();
        Expect(luid.LowPart == game.luid.LowPart && luid.HighPart == game.luid.HighPart,
               "The mirror's device is on the game's adapter");
    }
    ComPtr<ID3D12Resource> target = host.Target(kEyeWidth, kHeight);
    SetMirrorVulkanTargetForTests(*gpu, target.Get());

    const auto expect_eye = [&](const std::vector<std::uint8_t>& pixels, const std::vector<std::uint8_t>& source,
                                std::uint32_t offset_x, const std::string& what, int tolerance = 0, bool bgra = false) {
        int worst = 0;
        int wrong = 0;
        std::string first;
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kEyeWidth; ++x) {
                const std::uint8_t* got = &pixels[(static_cast<std::size_t>(y) * kEyeWidth + x) * 4];
                const std::uint8_t* want = &source[(static_cast<std::size_t>(y) * kWidth + offset_x + x) * 4];
                int texel = 255 - got[3];
                for (int c = 0; c < 3; ++c) {
                    texel = std::max(texel, std::abs(static_cast<int>(got[c]) - static_cast<int>(want[bgra ? 2 - c : c])));
                }
                if (texel > tolerance && wrong++ == 0) {
                    first = "first at " + std::to_string(x) + "," + std::to_string(y) + ": " + std::to_string(got[0]) +
                            " " + std::to_string(got[1]) + " " + std::to_string(got[2]) + " " + std::to_string(got[3]) +
                            ", wanted " + std::to_string(want[0]) + " " + std::to_string(want[1]) + " " +
                            std::to_string(want[2]);
                }
                worst = std::max(worst, texel);
            }
        }
        Expect(worst <= tolerance, what + " (worst difference " + std::to_string(worst) + ", " +
                                       std::to_string(wrong) + " texels wrong, " + first + ")");
    };

    // The first call only queues the copy: the picture follows a call later.
    const std::uint64_t copies_before = MirrorVulkanCopiesForTests(*gpu);
    Expect(gpu->DrawMirror(view, draw, error) == MirrorDrawResult::kSkipped &&
               MirrorVulkanCopiesForTests(*gpu) == copies_before + 1,
           "The first frame is copied on the game's queue and not shown yet: " + error);
    vk.vkQueueWaitIdle(game.queue);
    Expect(gpu->DrawMirror(view, draw, error) == MirrorDrawResult::kShown, "The next call shows it: " + error);
    expect_eye(host.Read(target.Get()), pattern, kEyeWidth,
               "The window's picture is the right eye's texels of layer 1 exactly, through both graphics APIs");
    CheckClean("the first picture", host.device);

    // Frame after frame: each call shows what the call before copied.
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        const std::uint32_t index = frame % 2;
        const std::vector<std::uint8_t> rendered = PatternImage(kWidth, kHeight, 20 + frame);
        GameFill(game, chain, index, 1, rendered.data(), 4);
        const SubmittedView now = View(chain, index, 1, right_eye);
        // Whatever was copied before is shown (or dropped); this frame is copied.
        const MirrorDrawResult first = gpu->DrawMirror(now, draw, error);
        Expect(first != MirrorDrawResult::kFailed, "A frame is copied: " + error);
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        Expect(gpu->DrawMirror(now, draw, error) == MirrorDrawResult::kShown, "And shown a call later: " + error);
        expect_eye(host.Read(target.Get()), rendered, kEyeWidth,
                   "Frame " + std::to_string(frame) + " is what the window shows a call after its copy");
    }
    CheckClean("frames", host.device);

    // The left eye of layer 0, a rectangle with an offset inside it.
    {
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        const std::vector<std::uint8_t> left = PatternImage(kWidth, kHeight, 77);
        GameFill(game, chain, 0, 0, left.data(), 4);
        const SubmittedView left_view =
            View(chain, 0, 0, {{0, 0}, {static_cast<int>(kEyeWidth), static_cast<int>(kHeight)}});
        gpu->DrawMirror(left_view, draw, error);
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        Expect(gpu->DrawMirror(left_view, draw, error) == MirrorDrawResult::kShown, "The left eye is shown: " + error);
        expect_eye(host.Read(target.Get()), left, 0, "The left eye of layer 0 is shown, nothing of the right or of layer 1");
    }

    // What cannot be shown is refused with a reason; the mirror goes on.
    {
        const auto refused = [&](SubmittedView bad, MirrorDraw bad_draw, const std::string& what,
                                 const std::string& part) {
            std::string why;
            Expect(gpu->DrawMirror(bad, bad_draw, why) == MirrorDrawResult::kFailed &&
                       why.find(part) != std::string::npos,
                   "Refused: " + what + " (" + why + ")");
        };
        SubmittedView bad = view;
        bad.usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        refused(bad, draw, "an image made without the copy usage", "restart the game");
        bad = view;
        bad.sample_count = 4;
        refused(bad, draw, "a multisampled image", "multisampled");
        bad = view;
        bad.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        refused(bad, draw, "a format the mirror does not show", "format");
        bad = view;
        bad.rect.extent.width = static_cast<int>(kWidth);
        refused(bad, draw, "a rectangle outside the image", "rectangle");
        bad = view;
        bad.array_slice = 2;
        refused(bad, draw, "a layer outside the image", "array slice");
        bad = view;
        bad.swapchain = reinterpret_cast<XrSwapchain>(std::uintptr_t{0x99});
        refused(bad, draw, "a swapchain that is not tracked", "not known");
        bad = view;
        bad.image_index = 9;
        refused(bad, draw, "an image outside its swapchain", "not known");
        MirrorDraw bad_draw = draw;
        bad_draw.width = 0;
        refused(view, bad_draw, "a picture of no size", "no picture");
        UntilShown(game, host, *gpu, view, draw, "after refusals");
        CheckClean("refusals", host.device);
    }

    // Other formats: sRGB (linear texels, encoded for the window), blue
    // first, half floats. Each makes the shared images anew.
    {
        Swapchain srgb = MakeSwapchain(game, VK_FORMAT_R8G8B8A8_SRGB, kWidth, kHeight, 1, 1);
        const std::vector<std::uint8_t> stored = PatternImage(kWidth, kHeight, 9);
        GameFill(game, srgb, 0, 0, stored.data(), 4);
        Track(*gpu, srgb);
        UntilShown(game, host, *gpu, View(srgb, 0, 0, right_eye), draw, "sRGB");
        // Stale pictures of the other format are not shown: the newest is this one's.
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, View(srgb, 0, 0, right_eye), draw, "sRGB again");
        expect_eye(host.Read(target.Get()), stored, kEyeWidth,
                   "An sRGB image's stored values reach the window (decoded and encoded again)", 1);

        Swapchain bgra = MakeSwapchain(game, VK_FORMAT_B8G8R8A8_UNORM, kWidth, kHeight, 1, 1);
        GameFill(game, bgra, 0, 0, stored.data(), 4);
        Track(*gpu, bgra);
        UntilShown(game, host, *gpu, View(bgra, 0, 0, right_eye), draw, "blue first");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, View(bgra, 0, 0, right_eye), draw, "blue first again");
        expect_eye(host.Read(target.Get()), stored, kEyeWidth, "A blue-first image is shown with its colours right", 0,
                   true);

        Swapchain half = MakeSwapchain(game, VK_FORMAT_R16G16B16A16_SFLOAT, kWidth, kHeight, 1, 1);
        std::vector<std::uint16_t> halves(static_cast<std::size_t>(kWidth) * kHeight * 4, 0x3800 /* 0.5 */);
        GameFill(game, half, 0, 0, halves.data(), 8);
        Track(*gpu, half);
        UntilShown(game, host, *gpu, View(half, 0, 0, right_eye), draw, "half floats");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, View(half, 0, 0, right_eye), draw, "half floats again");
        const std::vector<std::uint8_t> encoded = host.Read(target.Get());
        Expect(std::abs(encoded[0] - 188) <= 1 && std::abs(encoded[1] - 188) <= 1 && encoded[3] == 255,
               "Half-float 0.5 is shown sRGB-encoded (" + std::to_string(encoded[0]) + ")");
        CheckClean("formats", host.device);
        gpu->Forget(srgb.handle);
        gpu->Forget(bgra.handle);
        gpu->Forget(half.handle);
        Expect(!gpu->Tracked(srgb.handle), "Forgotten swapchains are forgotten");
        DestroySwapchain(game, srgb);
        DestroySwapchain(game, bgra);
        DestroySwapchain(game, half);
    }

    // A game that changes its render size: a smaller rectangle, then the
    // larger again (the shared images are kept, then made anew once).
    {
        const XrRect2Di small_rect{{static_cast<int>(kEyeWidth), 0}, {48, 40}};
        MirrorDraw small_draw = draw;
        small_draw.width = 48;
        small_draw.height = 40;
        ComPtr<ID3D12Resource> small_target = host.Target(48, 40);
        SetMirrorVulkanTargetForTests(*gpu, small_target.Get());
        GameFill(game, chain, 1, 1, pattern.data(), 4);
        UntilShown(game, host, *gpu, View(chain, 1, 1, small_rect), small_draw, "a small rectangle");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, View(chain, 1, 1, small_rect), small_draw, "a small rectangle again");
        const std::vector<std::uint8_t> pixels = host.Read(small_target.Get());
        int worst = 0;
        for (std::uint32_t y = 0; y < 40; ++y) {
            for (std::uint32_t x = 0; x < 48; ++x) {
                for (int c = 0; c < 3; ++c) {
                    worst = std::max(worst, std::abs(static_cast<int>(pixels[(y * 48 + x) * 4 + c]) -
                                                     static_cast<int>(pattern[(static_cast<std::size_t>(y) * kWidth +
                                                                               kEyeWidth + x) * 4 + c])));
                }
            }
        }
        Expect(worst == 0, "A smaller rectangle is shown exactly, nothing of an older and larger picture");
        SetMirrorVulkanTargetForTests(*gpu, target.Get());
        UntilShown(game, host, *gpu, view, draw, "the large rectangle");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, view, draw, "the large rectangle again");
        expect_eye(host.Read(target.Get()), pattern, kEyeWidth, "The rectangle grown back is shown exactly");
        CheckClean("render sizes", host.device);
    }

    // A window smaller than the eye: the game's queue scales the eye down as
    // it copies it (half here: each pixel the mean of two by two texels),
    // and the whole copy is what it was with scaling turned off.
    {
        MirrorDraw half_draw = draw;
        half_draw.width = kEyeWidth / 2;
        half_draw.height = kHeight / 2;
        ComPtr<ID3D12Resource> half_target = host.Target(kEyeWidth / 2, kHeight / 2);
        SetMirrorVulkanTargetForTests(*gpu, half_target.Get());
        // A smooth picture, so a mean is a mean whatever the rounding.
        std::vector<std::uint8_t> smooth(static_cast<std::size_t>(kWidth) * kHeight * 4);
        for (std::uint32_t y = 0; y < kHeight; ++y) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                std::uint8_t* texel = &smooth[(static_cast<std::size_t>(y) * kWidth + x) * 4];
                texel[0] = static_cast<std::uint8_t>(x);
                texel[1] = static_cast<std::uint8_t>(y * 3);
                texel[2] = static_cast<std::uint8_t>(x + y);
                texel[3] = 255;
            }
        }
        GameFill(game, chain, 1, 1, smooth.data(), 4);
        const auto half_picture = [&](const std::string& what) {
            UntilShown(game, host, *gpu, view, half_draw, what);
            vk.vkQueueWaitIdle(game.queue);
            host.Idle();
            UntilShown(game, host, *gpu, view, half_draw, what + " again");
            const std::vector<std::uint8_t> pixels = host.Read(half_target.Get());
            int worst = 0;
            for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
                for (std::uint32_t x = 0; x < kEyeWidth / 2; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        int sum = 0;
                        for (int dy = 0; dy < 2; ++dy) {
                            for (int dx = 0; dx < 2; ++dx) {
                                sum += smooth[(static_cast<std::size_t>(y * 2 + dy) * kWidth + kEyeWidth + x * 2 + dx) * 4 + c];
                            }
                        }
                        worst = std::max(worst, std::abs(static_cast<int>(pixels[(y * (kEyeWidth / 2) + x) * 4 + c]) -
                                                         (sum + 2) / 4));
                    }
                }
            }
            return worst;
        };
        const int scaled = half_picture("a half-size picture, scaled by the game's queue");
        Expect(scaled <= 1, "A picture half the eye's size is the eye scaled down by the copy (worst difference " +
                                std::to_string(scaled) + ")");
        SetMirrorVulkanFullCopyForTests(*gpu, true);
        const int whole = half_picture("a half-size picture from a whole copy");
        Expect(whole <= 2, "And the same picture from a whole copy, scaled by the mirror's draw (worst difference " +
                               std::to_string(whole) + ")");
        SetMirrorVulkanFullCopyForTests(*gpu, false);
        SetMirrorVulkanTargetForTests(*gpu, target.Get());
        GameFill(game, chain, 1, 1, pattern.data(), 4);
        UntilShown(game, host, *gpu, view, draw, "the eye's own size");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, view, draw, "the eye's own size again");
        expect_eye(host.Read(target.Get()), pattern, kEyeWidth, "A picture of the eye's own size is copied texel for texel");
        CheckClean("scaled copies", host.device);
    }

    // The game's queue busy with its own frame: no call waits for it, the
    // copy stays the only one queued, and the picture comes once it is done.
    {
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        gpu->DrawMirror(view, draw, error);
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        const Busy busy = MakeBusy(game);
        GameBusy(game, busy, 400);
        const std::uint64_t before = MirrorVulkanCopiesForTests(*gpu);
        double worst_ms = 0.0;
        int skipped = 0, shown = 0;
        const auto begun = std::chrono::steady_clock::now();
        for (int call = 0; call < 2000 && shown < 2; ++call) {
            const auto start = std::chrono::steady_clock::now();
            const MirrorDrawResult result = gpu->DrawMirror(view, draw, error);
            worst_ms = std::max(worst_ms, Ms(start));
            Expect(result != MirrorDrawResult::kFailed, "A frame while the game's queue is busy: " + error);
            (result == MirrorDrawResult::kShown ? shown : skipped)++;
            if (shown < 2 && MirrorVulkanCopiesForTests(*gpu) > before + 1 && skipped > 0 && shown == 0) {
                Expect(false, "A second copy was queued behind one the game's queue had not run");
            }
            Sleep(1);
        }
        const double busy_ms = Ms(begun);
        Expect(shown >= 1, "The picture comes once the game's queue has run the copy");
        Expect(worst_ms < 50.0, "No call waits for the game's queue (worst " + std::to_string(worst_ms) + " ms)");
        std::cout << "Busy game queue: " << skipped << " calls skipped over " << busy_ms << " ms, worst call "
                  << worst_ms << " ms\n";
        vk.vkQueueWaitIdle(game.queue);
        vk.vkDestroyImage(game.device, busy.image, nullptr);
        vk.vkFreeMemory(game.device, busy.memory, nullptr);
        CheckClean("a busy queue", host.device);
    }

    // A window (never put on the desktop): frames reach it without a wait,
    // through resizes of the window, and its swap chain is the mirror's
    // device's, not the game's.
    {
        SetMirrorVulkanTargetForTests(*gpu, nullptr);
        MirrorWindow window;
        Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The test's window is made: " + error);
        const std::uint32_t made_before = MirrorSwapChainsMadeForTests();
        int shown = 0, skipped = 0;
        double worst_ms = 0.0;
        const auto frame = [&](int number) {
            MirrorDraw window_draw;
            window_draw.window = window.Handle();
            window.OutputSize(window_draw.width, window_draw.height);
            window_draw.homography = Whole();
            window_draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
            const auto start = std::chrono::steady_clock::now();
            const MirrorDrawResult result = gpu->DrawMirror(View(chain, number % 2, 1, right_eye), window_draw, error);
            worst_ms = std::max(worst_ms, Ms(start));
            Expect(result != MirrorDrawResult::kFailed, "A frame is drawn into the window: " + error);
            (result == MirrorDrawResult::kShown ? shown : skipped)++;
        };
        for (int number = 0; number < 120; ++number) {
            frame(number);
            Sleep(shown == 0 ? 5 : 1);
        }
        Expect(shown > 0, "Frames reach the window");
        const int before_resize = shown;
        const HWND handle = static_cast<HWND>(window.Handle());
        for (int step = 0; step < 40; ++step) {
            SetWindowPos(handle, nullptr, 0, 0, 240 + step * 11, 160 + step * 6,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            frame(200 + step);
            Sleep(1);
        }
        Expect(shown > before_resize, "Frames are shown while the window is resized");
        Expect(MirrorSwapChainsMadeForTests() == made_before + 1,
               "One swap chain was made for the window, none for its forty sizes");
        Expect(worst_ms < 250.0, "No frame stalls (worst " + std::to_string(worst_ms) + " ms)");
        std::cout << "Window: " << shown << " frames shown, " << skipped << " skipped, worst call " << worst_ms
                  << " ms\n";
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        gpu->CloseMirror();
        gpu->CloseMirror();
        window.Stop();
        // And the mirror draws on into a target after its window closed.
        SetMirrorVulkanTargetForTests(*gpu, target.Get());
        GameFill(game, chain, 1, 1, pattern.data(), 4);
        UntilShown(game, host, *gpu, view, draw, "after the window");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, view, draw, "after the window again");
        expect_eye(host.Read(target.Get()), pattern, kEyeWidth, "The mirror draws on after its window closed");
        CheckClean("the window", host.device);
    }

    // The shared images are made and imported into the game's device on the
    // window's own thread: however long that takes (made slow here), no draw
    // call waits for it; the frames meanwhile are skipped. The same when
    // another format makes them anew.
    {
        SetMirrorVulkanImportDelayForTests(400);
        std::unique_ptr<SubmittedImageGpu> slow = MakeGpu(game, error);
        Expect(slow != nullptr, "A second Vulkan object is made: " + error);
        Track(*slow, chain);
        Swapchain other_format = MakeSwapchain(game, VK_FORMAT_R8G8B8A8_SRGB, kWidth, kHeight, 1, 1);
        GameFill(game, other_format, 0, 0, pattern.data(), 4);
        Track(*slow, other_format);
        MirrorWindow window;
        Expect(window.Start(WindowOptions(320, 180), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The slow test's window is made: " + error);
        const auto until_shown = [&](const SubmittedView& shown_view, double& worst_ms, double& waited_ms,
                                     int& skipped) {
            worst_ms = 0.0;
            skipped = 0;
            const auto started = std::chrono::steady_clock::now();
            for (;;) {
                MirrorDraw window_draw;
                window_draw.window = window.Handle();
                window.OutputSize(window_draw.width, window_draw.height);
                window_draw.homography = Whole();
                window_draw.on_window_thread = [&window](std::function<void()> task) {
                    return window.Post(std::move(task));
                };
                std::string why;
                const auto begun = std::chrono::steady_clock::now();
                const MirrorDrawResult result = slow->DrawMirror(shown_view, window_draw, why);
                worst_ms = std::max(worst_ms, Ms(begun));
                waited_ms = Ms(started);
                Expect(result != MirrorDrawResult::kFailed, "A frame while the shared images are made: " + why);
                if (result == MirrorDrawResult::kShown) return;
                ++skipped;
                Expect(waited_ms < 20000.0, "The shared images are never there");
                Sleep(2);
            }
        };
        double worst_ms = 0.0, waited_ms = 0.0;
        int skipped = 0;
        until_shown(view, worst_ms, waited_ms, skipped);
        Expect(waited_ms >= 380.0 && skipped >= 5,
               "While the shared images are made and imported the frames are skipped (" + std::to_string(waited_ms) +
                   " ms)");
        Expect(worst_ms < 50.0, "Opening the mirror stalls no draw call (worst " + std::to_string(worst_ms) +
                                    " ms with a 400 ms import)");
        std::cout << "Slow import: opening skipped " << skipped << " calls over " << waited_ms << " ms, worst call "
                  << worst_ms << " ms";
        until_shown(View(other_format, 0, 0, right_eye), worst_ms, waited_ms, skipped);
        Expect(waited_ms >= 380.0 && skipped >= 5 && worst_ms < 50.0,
               "Images made anew for another format stall no draw call either (worst " + std::to_string(worst_ms) +
                   " ms)");
        std::cout << "; another format skipped " << skipped << " calls over " << waited_ms << " ms, worst call "
                  << worst_ms << " ms" << std::endl;
        // Closed and reset while images are being made: bounded, nothing left.
        MirrorDraw window_draw;
        window_draw.window = window.Handle();
        window.OutputSize(window_draw.width, window_draw.height);
        window_draw.homography = Whole();
        window_draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
        vk.vkQueueWaitIdle(game.queue);
        Sleep(20);
        slow->DrawMirror(view, window_draw, error);
        Expect(slow->DrawMirror(view, window_draw, error) == MirrorDrawResult::kSkipped,
               "A frame of the first format again is skipped while its images are made");
        slow->CloseMirror();
        window.Stop();
        const auto begun = std::chrono::steady_clock::now();
        slow.reset();
        Expect(Ms(begun) < 3000.0, "The object goes with images still being made, in bounded time");
        SetMirrorVulkanImportDelayForTests(0);
        slow.reset();
        DestroySwapchain(game, other_format);
        CheckClean("slow import", host.device);
    }

    // A swapchain destroyed right behind its copy: forgetting it waits for
    // the copy (bounded), so the image can go.
    {
        Swapchain brief = MakeSwapchain(game, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, 1);
        GameFill(game, brief, 0, 0, pattern.data(), 4);
        Track(*gpu, brief);
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        const std::uint64_t before = MirrorVulkanCopiesForTests(*gpu);
        gpu->DrawMirror(View(brief, 0, 0, right_eye), draw, error);
        if (MirrorVulkanCopiesForTests(*gpu) == before) {
            vk.vkQueueWaitIdle(game.queue);
            host.Idle();
            gpu->DrawMirror(View(brief, 0, 0, right_eye), draw, error);
        }
        Expect(MirrorVulkanCopiesForTests(*gpu) > before, "A copy from the brief swapchain is queued");
        gpu->Forget(brief.handle);
        Expect(!gpu->Tracked(brief.handle), "The swapchain is forgotten behind its copy");
        // No vkQueueWaitIdle of the test's here: the images go at once.
        for (const VkImage image : brief.images) vk.vkDestroyImage(game.device, image, nullptr);
        for (const VkDeviceMemory memory : brief.memories) vk.vkFreeMemory(game.device, memory, nullptr);
        CheckClean("a swapchain destroyed behind its copy", host.device);
        UntilShown(game, host, *gpu, view, draw, "after a swapchain went");
    }

    // The game's queue is everybody's to synchronize: the mirror submits its
    // copy under the mutex the session's other users of the queue take (the
    // OSD's uploads, Frame Generation, the runtime's frame calls). While one
    // of them holds it, the copy waits; no submit is made beside theirs.
    {
        static std::mutex queue_mutex;
        static std::atomic<int> submits{0};
        gpu->SetQueueMutex(&queue_mutex);
        SetMirrorVulkanSubmitHookForTests([] { ++submits; });
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, view, draw, "under the queue's mutex");
        Expect(submits.load() > 0, "The copies are submitted with the queue's mutex set");
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        const std::uint64_t before = MirrorVulkanCopiesForTests(*gpu);
        const int submits_before = submits.load();
        std::atomic<int> calls{0};
        std::atomic<bool> done{false};
        std::unique_lock held(queue_mutex); // another user of the queue is in its call
        std::thread frames([&] {
            std::string why;
            for (int i = 0; i < 400 && MirrorVulkanCopiesForTests(*gpu) == before; ++i) {
                gpu->DrawMirror(view, draw, why);
                ++calls;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            done = true;
        });
        // The thread comes to its copy and stops there.
        int settled = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            const int seen = calls.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            if (calls.load() == seen && !done) {
                settled = seen;
                break;
            }
        }
        Expect(settled >= 0 && !done && submits.load() == submits_before &&
                   MirrorVulkanCopiesForTests(*gpu) == before,
               "While another user holds the game's queue, the mirror's copy waits for it and submits nothing");
        held.unlock();
        frames.join();
        Expect(MirrorVulkanCopiesForTests(*gpu) > before && submits.load() > submits_before,
               "Once the queue is free the copy is submitted");
        SetMirrorVulkanSubmitHookForTests(nullptr);
        gpu->SetQueueMutex(nullptr);
        vk.vkQueueWaitIdle(game.queue);
        host.Idle();
        UntilShown(game, host, *gpu, view, draw, "after the queue's mutex");
    }

    // The session's end, with a copy and a draw in flight: everything of the
    // mirror's on the game's device goes before the device does.
    gpu->DrawMirror(view, draw, error);
    ComPtr<ID3D12Device> kept = host.device;
    host = {};
    gpu->Reset();
    Expect(!gpu->Tracked(chain.handle) && MirrorVulkanHostDeviceForTests(*gpu) == nullptr,
           "Reset lets go of the swapchains and of the mirror's device");
    // Usable again after a reset (the next session of the same game).
    Track(*gpu, chain);
    gpu->DrawMirror(view, draw, error);
    Host again;
    again.device = static_cast<ID3D12Device*>(MirrorVulkanHostDeviceForTests(*gpu));
    again.queue = static_cast<ID3D12CommandQueue*>(MirrorVulkanHostQueueForTests(*gpu));
    Expect(again.device != nullptr, "After a reset the mirror sets itself up again");
    ComPtr<ID3D12Resource> second_target = again.Target(kEyeWidth, kHeight);
    SetMirrorVulkanTargetForTests(*gpu, second_target.Get());
    GameFill(game, chain, 1, 1, pattern.data(), 4);
    UntilShown(game, again, *gpu, view, draw, "after a reset");
    vk.vkQueueWaitIdle(game.queue);
    again.Idle();
    UntilShown(game, again, *gpu, view, draw, "after a reset again");
    expect_eye(again.Read(second_target.Get()), pattern, kEyeWidth, "After a reset the picture is exact again");
    CheckClean("the end", again.device);
    second_target.Reset();
    again = {};
    // A session whose end was never seen, and whose device the game has
    // destroyed since: the reader is abandoned and makes no call on the
    // device, then or when it is freed. (On a device of its own: what the
    // reader held of it is the device's to free, which validation names
    // when the device goes; nothing else may be said.)
    {
        vk.vkQueueWaitIdle(game.queue);
        {
            std::string all;
            for (const std::string& message : g_messages.Take()) all += message + "\n";
            Expect(all.empty(), "Vulkan validation is silent before the abandoned session:\n" + all);
        }
        Game gone = game;
        gone.device = VK_NULL_HANDLE;
        gone.pool = VK_NULL_HANDLE;
        Expect(MakeDevice(gone, true), "A second Vulkan device is made");
        Swapchain left_chain = MakeSwapchain(gone, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, 1, 1);
        GameFill(gone, left_chain, 0, 0, pattern.data(), 4);
        std::string why;
        std::unique_ptr<SubmittedImageGpu> left = MakeGpu(gone, why);
        Expect(left != nullptr, "A reader of the second device is made: " + why);
        Track(*left, left_chain);
        SubmittedView left_view = View(left_chain, 0, 0, right_eye);
        left->DrawMirror(left_view, draw, why); // makes its Direct3D 12 device
        Host left_host;
        left_host.device = static_cast<ID3D12Device*>(MirrorVulkanHostDeviceForTests(*left));
        left_host.queue = static_cast<ID3D12CommandQueue*>(MirrorVulkanHostQueueForTests(*left));
        Expect(left_host.device != nullptr, "The second reader sets itself up: " + why);
        ComPtr<ID3D12Resource> left_target = left_host.Target(kEyeWidth, kHeight);
        SetMirrorVulkanTargetForTests(*left, left_target.Get());
        for (int i = 0; i < 2000 && MirrorVulkanCopiesForTests(*left) == 0; ++i) {
            left->DrawMirror(left_view, draw, why);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Expect(MirrorVulkanCopiesForTests(*left) > 0, "It copies an eye: " + why);
        left_target.Reset();
        left_host = {};
        static std::atomic<int> late_submits{0};
        SetMirrorVulkanSubmitHookForTests([] { ++late_submits; });
        left->Abandon();
        Expect(!left->Tracked(left_chain.handle) && MirrorVulkanHostDeviceForTests(*left) == nullptr,
               "An abandoned reader holds nothing");
        vk.vkQueueWaitIdle(gone.queue);
        DestroySwapchain(gone, left_chain);
        DestroyDevice(gone);
        // Freed after its device: no call on it.
        left.reset();
        Expect(late_submits.load() == 0, "An abandoned reader submits nothing more");
        SetMirrorVulkanSubmitHookForTests(nullptr);
        std::string other;
        for (const std::string& message : g_messages.Take()) {
            if (message.find("vkDestroyDevice") == std::string::npos) other += message + "\n";
        }
        Expect(other.empty(), "Nothing of a device that is gone is used by an abandoned reader:\n" + other);
    }
    gpu.reset();
    kept.Reset();
    DestroySwapchain(game, chain);
    CheckClean("teardown", nullptr);
    return 0;
}

double Median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
double Worst(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

std::uint64_t LocalVideoMemory(const LUID& luid) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) return 0;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart != luid.LowPart || desc.AdapterLuid.HighPart != luid.HighPart) continue;
        ComPtr<IDXGIAdapter3> adapter3;
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter.As(&adapter3)) ||
            FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            return 0;
        }
        return info.CurrentUsage;
    }
}

// The real adapter: what the mirror costs a Vulkan game. A 3600x3600 sRGB
// eye image; the window never on the desktop.
int Measure(Game& game) {
    constexpr std::uint32_t kEye = 3600;
    std::cout << "Adapter: " << game.name << "\n";
    Swapchain chain = MakeSwapchain(game, VK_FORMAT_R8G8B8A8_SRGB, kEye, kEye, 1, 1);
    {
        const std::vector<std::uint8_t> data = PatternImage(kEye, kEye, 1);
        GameFill(game, chain, 0, 0, data.data(), 4);
    }
    const SubmittedView view = View(chain, 0, 0, {{0, 0}, {static_cast<int>(kEye), static_cast<int>(kEye)}});
    const XrFovf fov{-0.87f, 0.79f, 0.77f, -0.84f};
    for (const auto& [width, height, full_copy] :
         {std::tuple{1280, 720, false}, std::tuple{1920, 1080, false}, std::tuple{2560, 1440, false},
          std::tuple{1920, 1080, true}}) {
        vk.vkQueueWaitIdle(game.queue);
        const std::uint64_t before = LocalVideoMemory(game.luid);
        std::string error;
        std::unique_ptr<SubmittedImageGpu> gpu = MakeGpu(game, error);
        Expect(gpu != nullptr, "The measured object is made: " + error);
        Track(*gpu, chain);
        SetMirrorVulkanTimingForTests(*gpu, true);
        SetMirrorVulkanFullCopyForTests(*gpu, full_copy);
        MirrorWindow window;
        Expect(window.Start(WindowOptions(width, height), error) && WaitFor([&] { return window.Handle() != nullptr; }),
               "The measured window is made: " + error);
        MirrorStabilizer stabilizer;
        MirrorSettings settings;
        std::vector<double> cpu_shown, cpu_skipped, copy_gpu, draw_gpu;
        std::uint64_t during = before;
        const std::uint32_t made_before = MirrorSwapChainsMadeForTests();
        int shown = 0, skipped = 0;
        double first_shown_ms = 0.0;
        const auto begun = std::chrono::steady_clock::now();
        // The mirror's draw on its own queue, between two timestamps.
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> stamps;
        ComPtr<ID3D12CommandAllocator> allocators[2];
        ComPtr<ID3D12GraphicsCommandList> lists[2];
        ComPtr<ID3D12Fence> fence;
        std::uint64_t serial = 0;
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        for (int number = 0; number < 2000 && shown < 120; ++number) {
            if (!device) {
                device = static_cast<ID3D12Device*>(MirrorVulkanHostDeviceForTests(*gpu));
                queue = static_cast<ID3D12CommandQueue*>(MirrorVulkanHostQueueForTests(*gpu));
                if (device) {
                    D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
                    device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap));
                    D3D12_HEAP_PROPERTIES readback{};
                    readback.Type = D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC buffer{};
                    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    buffer.Width = 16;
                    buffer.Height = 1;
                    buffer.DepthOrArraySize = 1;
                    buffer.MipLevels = 1;
                    buffer.SampleDesc.Count = 1;
                    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stamps));
                    for (int i = 0; i < 2; ++i) {
                        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]));
                        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                                  IID_PPV_ARGS(&lists[i]));
                        lists[i]->Close();
                    }
                    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
                }
            }
            MirrorDraw draw;
            draw.window = window.Handle();
            window.OutputSize(draw.width, draw.height);
            draw.on_window_thread = [&window](std::function<void()> task) { return window.Post(std::move(task)); };
            if (shown >= 40 && shown < 60) {
                // Resized on the desktop in the middle, another size at every frame.
                const int wide = width - 16 * (shown - 39);
                SetWindowPos(static_cast<HWND>(window.Handle()), nullptr, 0, 0, wide, wide * height / width,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            const MirrorQuat head{0.0, std::sin(0.004 * std::sin(number * 0.9)), 0.0,
                                  std::cos(0.004 * std::sin(number * 0.9))};
            XrPosef pose{};
            pose.orientation = {static_cast<float>(head.x), static_cast<float>(head.y), static_cast<float>(head.z),
                                static_cast<float>(head.w)};
            draw.homography =
                stabilizer.Update(pose, fov, 1'000'000'000ll + number * 11'111'111ll, settings).homography;
            if (queue) {
                allocators[0]->Reset();
                lists[0]->Reset(allocators[0].Get(), nullptr);
                lists[0]->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                lists[0]->Close();
                ID3D12CommandList* run[] = {lists[0].Get()};
                queue->ExecuteCommandLists(1, run);
            }
            const auto start = std::chrono::steady_clock::now();
            const MirrorDrawResult result = gpu->DrawMirror(view, draw, error);
            const double cpu = Ms(start);
            Expect(result != MirrorDrawResult::kFailed, "A measured frame is drawn: " + error);
            if (queue) {
                allocators[1]->Reset();
                lists[1]->Reset(allocators[1].Get(), nullptr);
                lists[1]->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                lists[1]->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamps.Get(), 0);
                lists[1]->Close();
                ID3D12CommandList* run[] = {lists[1].Get()};
                queue->ExecuteCommandLists(1, run);
                queue->Signal(fence.Get(), ++serial);
            }
            // A game's frame goes by; both queues finish what they have.
            vk.vkQueueWaitIdle(game.queue);
            if (queue) {
                const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                fence->SetEventOnCompletion(serial, event);
                WaitForSingleObject(event, 5000);
                CloseHandle(event);
            }
            if (result == MirrorDrawResult::kShown) {
                if (shown++ == 0) first_shown_ms = Ms(begun);
                cpu_shown.push_back(cpu);
                UINT64 frequency = 0;
                queue->GetTimestampFrequency(&frequency);
                UINT64* mapped = nullptr;
                const D3D12_RANGE range{0, 16};
                if (frequency && SUCCEEDED(stamps->Map(0, &range, reinterpret_cast<void**>(&mapped)))) {
                    draw_gpu.push_back(static_cast<double>(mapped[1] - mapped[0]) * 1000.0 /
                                       static_cast<double>(frequency));
                    const D3D12_RANGE none{0, 0};
                    stamps->Unmap(0, &none);
                }
                during = std::max(during, LocalVideoMemory(game.luid));
            } else {
                ++skipped;
                cpu_skipped.push_back(cpu);
            }
            const double copy = MirrorVulkanCopyGpuMsForTests(*gpu);
            if (copy >= 0.0) copy_gpu.push_back(copy);
            Sleep(3);
        }
        std::cout << "Vulkan, 3600x3600 eye " << (full_copy ? "copied whole" : "scaled down as it is copied")
                  << " on the game's queue, window " << width << "x" << height << ": "
                  << shown << " shown, " << skipped << " skipped (first shown after " << first_shown_ms
                  << " ms); copy on the game's queue, GPU: median " << Median(copy_gpu) << " ms, worst "
                  << Worst(copy_gpu) << " ms; mirror draw on its own queue, GPU: median " << Median(draw_gpu)
                  << " ms, worst " << Worst(draw_gpu) << " ms; CPU in the call on the frame thread, shown: median "
                  << Median(cpu_shown) << " ms, worst " << Worst(cpu_shown) << " ms; not shown: median "
                  << Median(cpu_skipped) << " ms, worst " << Worst(cpu_skipped)
                  << " ms; video memory while open +"
                  << (static_cast<std::int64_t>(during) - static_cast<std::int64_t>(before)) / (1024.0 * 1024.0)
                  << " MB (two whole 3600x3600 images would be " << 2.0 * kEye * kEye * 4 / (1024.0 * 1024.0)
                  << " MB, two " << width << "x" << height << " buffers " << 2.0 * width * height * 4 / (1024.0 * 1024.0)
                  << " MB); swap chains made through 20 window sizes: " << MirrorSwapChainsMadeForTests() - made_before
                  << std::endl;
        heap.Reset();
        stamps.Reset();
        for (int i = 0; i < 2; ++i) {
            lists[i].Reset();
            allocators[i].Reset();
        }
        fence.Reset();
        gpu->CloseMirror();
        window.Stop();
        gpu.reset();
    }
    DestroySwapchain(game, chain);
    return 0;
}

// The whole GPU's memory in use as the driver reports it (nvidia-smi, where
// it is installed; -1 without), in MB: every process, whatever DXGI counts
// for this one.
double DriverMemoryUsedMb() {
    FILE* pipe = _popen("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>nul", "r");
    if (!pipe) return -1.0;
    char line[64]{};
    const bool read = std::fgets(line, sizeof(line), pipe) != nullptr;
    _pclose(pipe);
    return read ? std::atof(line) : -1.0;
}

// Is a Direct3D 12 texture that a Vulkan device imports allocated twice, or
// only counted twice? Eight 3600x3600 shared textures are made by a
// Direct3D 12 device, then imported into the game's Vulkan device, then the
// Vulkan side and the Direct3D 12 side are let go of in turn; after each
// step: this process's video memory as DXGI counts it, and the whole GPU's
// as the driver does.
int VideoMemory(Game& game) {
    constexpr int kImages = 8;
    constexpr std::uint32_t kSide = 3600;
    const double each = kSide * kSide * 4.0 / (1024.0 * 1024.0);
    std::cout << "Adapter: " << game.name << "; " << kImages << " shared " << kSide << "x" << kSide
              << " RGBA8 textures of " << each << " MB each, " << kImages * each << " MB together\n";
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    Expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "A DXGI factory");
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == game.luid.LowPart && desc.AdapterLuid.HighPart == game.luid.HighPart) break;
        adapter.Reset();
    }
    ComPtr<ID3D12Device> device;
    Expect(adapter && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))),
           "A Direct3D 12 device on the game's adapter");
    const auto get_properties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vk.module, "vkGetDeviceProcAddr"))(
            game.device, "vkGetMemoryWin32HandlePropertiesKHR"));
    Expect(get_properties != nullptr, "The game's device has vkGetMemoryWin32HandlePropertiesKHR");
    const auto report = [&](const char* step, std::uint64_t dxgi_base, double driver_base) {
        Sleep(700);
        const double dxgi = (static_cast<double>(LocalVideoMemory(game.luid)) - static_cast<double>(dxgi_base)) /
                            (1024.0 * 1024.0);
        const double driver = DriverMemoryUsedMb();
        std::cout << "  " << step << ": DXGI (this process) +" << dxgi << " MB; driver (whole GPU) ";
        if (driver < 0.0 || driver_base < 0.0) {
            std::cout << "not available";
        } else {
            std::cout << "+" << driver - driver_base << " MB";
        }
        std::cout << std::endl;
    };
    Sleep(700);
    const std::uint64_t dxgi_base = LocalVideoMemory(game.luid);
    const double driver_base = DriverMemoryUsedMb();
    std::vector<ComPtr<ID3D12Resource>> textures(kImages);
    std::vector<HANDLE> handles(kImages);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kSide;
    desc.Height = kSide;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    for (int i = 0; i < kImages; ++i) {
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                         D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                         IID_PPV_ARGS(&textures[i]))) &&
                   SUCCEEDED(device->CreateSharedHandle(textures[i].Get(), nullptr, GENERIC_ALL, nullptr, &handles[i])),
               "A shared Direct3D 12 texture is made");
    }
    report("made by Direct3D 12, not imported", dxgi_base, driver_base);
    std::vector<VkImage> images(kImages);
    std::vector<VkDeviceMemory> memories(kImages);
    for (int i = 0; i < kImages; ++i) {
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.pNext = &external;
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R8G8B8A8_UNORM;
        create.extent = {kSide, kSide, 1};
        create.mipLevels = 1;
        create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Expect(vk.vkCreateImage(game.device, &create, nullptr, &images[i]) == VK_SUCCESS, "A Vulkan image");
        VkMemoryRequirements requirements{};
        vk.vkGetImageMemoryRequirements(game.device, images[i], &requirements);
        VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        Expect(get_properties(game.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handles[i],
                              &properties) == VK_SUCCESS,
               "The handle's memory types");
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = images[i];
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handles[i];
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = &import;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(game, requirements.memoryTypeBits & properties.memoryTypeBits, 0);
        Expect(vk.vkAllocateMemory(game.device, &allocate, nullptr, &memories[i]) == VK_SUCCESS &&
                   vk.vkBindImageMemory(game.device, images[i], memories[i], 0) == VK_SUCCESS,
               "The texture is imported");
        CloseHandle(handles[i]);
    }
    // Written by Vulkan, so the memory is really there on both sides.
    GameSubmit(game, [&](VkCommandBuffer commands) {
        for (int i = 0; i < kImages; ++i) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[i];
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                    nullptr, 0, nullptr, 1, &barrier);
            const VkClearColorValue color{{0.25f, 0.5f, 0.75f, 1.0f}};
            vk.vkCmdClearColorImage(commands, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                                    &barrier.subresourceRange);
        }
    });
    report("imported into Vulkan and written", dxgi_base, driver_base);
    for (int i = 0; i < kImages; ++i) {
        vk.vkDestroyImage(game.device, images[i], nullptr);
        vk.vkFreeMemory(game.device, memories[i], nullptr);
    }
    report("Vulkan's images and memory freed", dxgi_base, driver_base);
    textures.clear();
    report("Direct3D 12's textures released", dxgi_base, driver_base);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool require = vulkan_test::RequireHardware();
    const auto skip = [&](const std::string& why) {
        std::cout << "Vulkan mirror: " << (require ? "FAILED (hardware required): " : "skipped: ") << why << "\n";
        return require ? 1 : kSkip;
    };
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!LoadVulkan()) return skip("no Vulkan loader");
    Game game;
    std::string why;
    if (!MakeInstance(game)) return skip("no Vulkan instance");
    if (!FindPhysical(game, why)) return skip(why);
    if (mode.empty()) {
        vulkan_test::PrintValidationState(g_validation);
        if (require && !g_validation) return skip("the Khronos validation layer is required and not there");
        // The Direct3D 12 debug layer for the mirror's own device.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            std::cout << "Direct3D 12 debug layer: on\n";
        } else if (require) {
            return skip("the Direct3D 12 debug layer is required and not there");
        }
    }
    if (!MakeDevice(game, true)) return skip("the Vulkan device could not be made");
    int result = 0;
    if (mode == "measure") {
        result = Measure(game);
    } else if (mode == "vram") {
        result = VideoMemory(game);
    } else if (mode.empty()) {
        Game bare = game;
        bare.device = VK_NULL_HANDLE;
        bare.pool = VK_NULL_HANDLE;
        Expect(MakeDevice(bare, false), "A Vulkan device without the sharing extension is made");
        result = Tests(game, bare);
        DestroyDevice(bare);
    } else {
        std::cerr << "Unknown mode " << mode << "\n";
        return 2;
    }
    DestroyDevice(game);
    {
        std::string all;
        for (const std::string& message : g_messages.Take()) all += message + "\n";
        Expect(all.empty(), "Vulkan validation is silent at the end:\n" + all);
    }
    if (game.messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vk.vkGetInstanceProcAddr(game.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(game.instance, game.messenger, nullptr);
    }
    vk.vkDestroyInstance(game.instance, nullptr);
    if (result == 0 && mode.empty()) {
        std::cout << "Vulkan mirror window (" << game.name << "): " << checks << " checks passed"
                  << (g_validation ? " (validation on)" : "") << "\n";
    }
    return result;
}
