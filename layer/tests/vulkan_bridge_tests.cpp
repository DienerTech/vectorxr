// Frame Generation's Vulkan bridge: the pure parts (formats, the runtime's
// create info, extension lists) everywhere; the shared images and fences on a
// GPU whose Vulkan device and Direct3D 12 adapter are the same (77 without
// one: there is no software Vulkan device that shares memory with Direct3D 12).
//
// The GPU cases run a Vulkan "game" against a stand-in runtime whose
// swapchain images are Direct3D 12 textures on the bridge's device, with the
// Khronos validation layer (vulkan_test_validation.h) and the Direct3D 12
// debug layer checking every case. The bridge resolves its Vulkan functions
// through a getter of the test's, which records the bridge's barriers and
// queue calls (and fails submissions on demand), so the hand-over protocol is
// checked structurally as well as by readbacks. Only the fixture decides a
// skip; with VECTORXR_TESTS_REQUIRE_HW=1 a skip fails.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_vulkan_bridge.h"
#include "vulkan_test_validation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using depthxr::VulkanBridge;
using depthxr::VulkanBridgeGame;
using Microsoft::WRL::ComPtr;

constexpr int kSkip = 77;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::fflush(stderr);
        std::exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A figure from the bridge's summary ("name=value").
std::uint64_t SummaryValue(const std::string& summary, const std::string& name) {
    const std::size_t at = summary.find(name + "=");
    Check(at != std::string::npos, "the summary has " + name + ": " + summary);
    return std::stoull(summary.substr(at + name.size() + 1));
}

// A pair of figures from the bridge's summary ("name=first/second").
std::pair<std::uint64_t, std::uint64_t> SummaryPair(const std::string& summary, const std::string& name) {
    const std::size_t at = summary.find(name + "=");
    Check(at != std::string::npos, "the summary has " + name + ": " + summary);
    const std::size_t value = at + name.size() + 1;
    const std::size_t slash = summary.find('/', value);
    Check(slash != std::string::npos, "the summary's " + name + " is a pair: " + summary);
    return {std::stoull(summary.substr(value)), std::stoull(summary.substr(slash + 1))};
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// The pure parts.

void TestFormats() {
    const std::pair<VkFormat, DXGI_FORMAT> pairs[] = {
        {VK_FORMAT_R8G8B8A8_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
        {VK_FORMAT_B8G8R8A8_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM},
        {VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT},
        {VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM},
        {VK_FORMAT_B10G11R11_UFLOAT_PACK32, DXGI_FORMAT_R11G11B10_FLOAT},
        {VK_FORMAT_R32G32B32A32_SFLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT},
        {VK_FORMAT_D32_SFLOAT, DXGI_FORMAT_D32_FLOAT},
        {VK_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT},
        {VK_FORMAT_D16_UNORM, DXGI_FORMAT_D16_UNORM},
        {VK_FORMAT_D32_SFLOAT_S8_UINT, DXGI_FORMAT_D32_FLOAT_S8X24_UINT},
    };
    for (const auto& [vulkan, dxgi] : pairs) {
        Check(VulkanBridge::RuntimeFormat(vulkan) == dxgi, "VkFormat " + std::to_string(vulkan) + " maps to DXGI");
        Check(VulkanBridge::GameFormat(dxgi) == vulkan, "DXGI " + std::to_string(dxgi) + " maps back");
    }
    Check(VulkanBridge::RuntimeFormat(VK_FORMAT_R8G8B8_UNORM) == 0, "a format Direct3D 12 lacks maps to 0");
    Check(VulkanBridge::GameFormat(DXGI_FORMAT_B8G8R8X8_UNORM) == 0, "a format Vulkan lacks maps to 0");
    Check(VulkanBridge::GameFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS) == 0, "typeless formats are not offered");

    const std::vector<std::int64_t> runtime{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM,
                                            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                            DXGI_FORMAT_D32_FLOAT};
    const std::vector<std::int64_t> game = VulkanBridge::TranslatedFormats(runtime);
    const std::vector<std::int64_t> expected{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_D32_SFLOAT};
    Check(game == expected, "the game's formats keep the runtime's order, without untranslatable or repeated ones");
}

void TestRuntimeCreateInfo() {
    XrSwapchainCreateInfo color{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    color.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    color.format = VK_FORMAT_R8G8B8A8_SRGB;
    color.sampleCount = 4;
    color.width = 64;
    color.height = 48;
    color.faceCount = 1;
    color.arraySize = 2;
    color.mipCount = 1;
    XrSwapchainCreateInfo runtime{};
    std::string reason;
    Check(VulkanBridge::RuntimeCreateInfo(color, runtime, reason), "a colour swapchain translates: " + reason);
    Check(runtime.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "colour format is DXGI");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) != 0, "the application's usage is kept");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0, "colour is a render target");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, "the bridge's copies write colour");
    Check(runtime.sampleCount == 1, "multisampled colour is resolved for the runtime");
    Check(runtime.width == 64 && runtime.height == 48 && runtime.arraySize == 2, "size and layers are kept");

    XrSwapchainCreateInfo depth = color;
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth.sampleCount = 1;
    Check(VulkanBridge::RuntimeCreateInfo(depth, runtime, reason), "a depth swapchain translates: " + reason);
    Check(runtime.format == DXGI_FORMAT_D32_FLOAT, "depth format is DXGI");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0, "depth is no render target");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, "the bridge's copies write depth");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0, "depth is a depth target");
    for (const XrSwapchainUsageFlags usage : {XrSwapchainUsageFlags{0}, XrSwapchainUsageFlags{XR_SWAPCHAIN_USAGE_SAMPLED_BIT}}) {
        XrSwapchainCreateInfo bare = depth;
        bare.usageFlags = usage;
        Check(VulkanBridge::RuntimeCreateInfo(bare, runtime, reason), "depth by format alone translates: " + reason);
        Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0 &&
                  (runtime.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                  (runtime.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) == 0,
              "a depth format gets depth target and transfer usage, never colour");
    }

    XrSwapchainCreateInfo odd = color;
    odd.format = VK_FORMAT_R8G8B8_UNORM;
    Check(!VulkanBridge::RuntimeCreateInfo(odd, runtime, reason) && !reason.empty(),
          "a format without a DXGI equivalent is refused with a reason");

    // Vulkan-only structures leading the chain stay with the game; others go on.
    const VkFormat view_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    XrBaseInStructure application{static_cast<XrStructureType>(1000999000), nullptr};
    XrVulkanSwapchainFormatListCreateInfoKHR list{XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
    list.next = &application;
    list.viewFormatCount = 2;
    list.viewFormats = view_formats;
    XrSwapchainCreateInfo listed = color;
    listed.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    listed.next = &list;
    Check(VulkanBridge::RuntimeCreateInfo(listed, runtime, reason), "a listed swapchain translates: " + reason);
    Check(runtime.next == &application, "the Vulkan format list is left out of the runtime's chain");
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) != 0, "mutable format is kept");
}

void TestExtensions() {
    const auto& instance = VulkanBridge::InstanceExtensions();
    const auto& device = VulkanBridge::DeviceExtensions();
    Check(!instance.empty() && !device.empty(), "the bridge names its extensions");
    const std::string device_list = VulkanBridge::WithExtensions("VK_KHR_swapchain VK_KHR_external_memory", device);
    Check(device_list.rfind("VK_KHR_swapchain VK_KHR_external_memory ", 0) == 0, "the runtime's names come first");
    Check(device_list.find("VK_KHR_external_memory_win32") != std::string::npos, "win32 memory is added");
    Check(device_list.find("VK_KHR_external_semaphore_win32") != std::string::npos, "win32 semaphores are added");
    std::size_t count = 0;
    for (std::size_t at = device_list.find("VK_KHR_external_memory "); at != std::string::npos;
         at = device_list.find("VK_KHR_external_memory ", at + 1)) {
        ++count;
    }
    Check(count == 1, "a name the runtime already lists is not repeated");
    Check(VulkanBridge::WithExtensions("", {"A", "B"}) == "A B", "an empty list gets the names");
    Check(VulkanBridge::WithExtensions("  A   B ", {"B"}) == "A B", "spacing is normalized");
}

// ---------------------------------------------------------------------------
// The Vulkan game.

HMODULE g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_get_instance_proc_addr = nullptr;
bool g_validation = false;
bool g_d3d12_debug = false;
vulkan_test::Messages g_messages; // every instance's messenger reports here

#define GAME_INSTANCE_FUNCTIONS(X)                                                                                 \
    X(vkDestroyInstance)                                                                                           \
    X(vkEnumeratePhysicalDevices)                                                                                  \
    X(vkGetPhysicalDeviceProperties)                                                                               \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                    \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                         \
    X(vkCreateDevice)                                                                                              \
    X(vkGetDeviceProcAddr)
// Core 1.1, or their KHR names (null on a Vulkan 1.0 instance without them).
#define GAME_INSTANCE_FUNCTIONS_11(X)                                                                              \
    X(vkGetPhysicalDeviceProperties2)                                                                              \
    X(vkGetPhysicalDeviceImageFormatProperties2)                                                                   \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties)
#define GAME_DEBUG_FUNCTIONS(X)                                                                                    \
    X(vkCreateDebugUtilsMessengerEXT)                                                                              \
    X(vkDestroyDebugUtilsMessengerEXT)
#define GAME_DEVICE_FUNCTIONS(X)                                                                                   \
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
    X(vkCmdClearDepthStencilImage)                                                                                 \
    X(vkCmdCopyBufferToImage)                                                                                      \
    X(vkQueueSubmit)                                                                                               \
    X(vkQueueWaitIdle)                                                                                             \
    X(vkCreateSemaphore)                                                                                           \
    X(vkDestroySemaphore)                                                                                          \
    X(vkCreateFence)                                                                                               \
    X(vkDestroyFence)                                                                                              \
    X(vkWaitForFences)
#define GAME_DEVICE_OPTIONAL(X) X(vkImportSemaphoreWin32HandleKHR)

struct Game {
#define GAME_DECLARE(name) PFN_##name name{nullptr};
    GAME_INSTANCE_FUNCTIONS(GAME_DECLARE)
    GAME_INSTANCE_FUNCTIONS_11(GAME_DECLARE)
    GAME_DEBUG_FUNCTIONS(GAME_DECLARE)
    GAME_DEVICE_FUNCTIONS(GAME_DECLARE)
    GAME_DEVICE_OPTIONAL(GAME_DECLARE)
#undef GAME_DECLARE
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t family{0};
    std::uint32_t queue_index{0};
    VkCommandPool pool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> pending; // submitted, freed at GameIdle
    VkPhysicalDeviceMemoryProperties memory{};
    LUID luid{};
    bool timeline{false}; // the device has timeline semaphores
    // Busy work: a large image cleared over and over.
    VkImage scratch{VK_NULL_HANDLE};
    VkDeviceMemory scratch_memory{VK_NULL_HANDLE};
};

PFN_vkVoidFunction InstanceFunction(VkInstance instance, const char* name) {
    return g_get_instance_proc_addr(instance, name);
}

bool MakeInstance(std::uint32_t api, bool bridge_extensions, Game& game, std::string& error) {
    std::vector<const char*> layers;
    std::vector<const char*> extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    if (g_validation) layers.push_back(vulkan_test::kValidationLayer);
    if (bridge_extensions) {
        for (const char* name : VulkanBridge::InstanceExtensions()) extensions.push_back(name);
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "depthxr_vulkan_bridge_tests";
    application.apiVersion = api;
    VkDebugUtilsMessengerCreateInfoEXT messenger_info = vulkan_test::MessengerInfo(&g_messages);
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pNext = &messenger_info;
    create.pApplicationInfo = &application;
    create.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create.ppEnabledLayerNames = layers.data();
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(InstanceFunction(nullptr, "vkCreateInstance"));
    const VkResult result =
        create_instance ? create_instance(&create, nullptr, &game.instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS) {
        error = "vkCreateInstance failed (" + std::to_string(result) + ")";
        return false;
    }
#define GAME_LOAD(name) game.name = reinterpret_cast<PFN_##name>(InstanceFunction(game.instance, #name));
#define GAME_LOAD_11(name)                                                                                         \
    game.name = reinterpret_cast<PFN_##name>(                                                                      \
        InstanceFunction(game.instance, api >= VK_API_VERSION_1_1 ? #name : #name "KHR"));
    GAME_INSTANCE_FUNCTIONS(GAME_LOAD)
    GAME_INSTANCE_FUNCTIONS_11(GAME_LOAD_11)
    GAME_DEBUG_FUNCTIONS(GAME_LOAD)
#undef GAME_LOAD
#undef GAME_LOAD_11
    if (game.vkCreateDebugUtilsMessengerEXT) {
        game.vkCreateDebugUtilsMessengerEXT(game.instance, &messenger_info, nullptr, &game.messenger);
    }
    return true;
}

// The physical device whose LUID is a hardware DXGI adapter's (or, with
// `like` set, the one with the same name and IDs).
bool FindPhysical(Game& game, const Game* like, std::string& error) {
    std::uint32_t count = 0;
    game.vkEnumeratePhysicalDevices(game.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    game.vkEnumeratePhysicalDevices(game.instance, &count, devices.data());
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        game.vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (like) {
            VkPhysicalDeviceProperties wanted{};
            like->vkGetPhysicalDeviceProperties(like->physical, &wanted);
            if (properties.vendorID == wanted.vendorID && properties.deviceID == wanted.deviceID &&
                std::strcmp(properties.deviceName, wanted.deviceName) == 0) {
                game.physical = device;
                game.luid = like->luid;
                break;
            }
            continue;
        }
        if (!game.vkGetPhysicalDeviceProperties2) continue;
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &id;
        game.vkGetPhysicalDeviceProperties2(device, &properties2);
        if (!id.deviceLUIDValid) continue;
        LUID luid{};
        std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (!factory || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        game.physical = device;
        game.luid = luid;
        break;
    }
    if (!game.physical) {
        error = "no Vulkan device whose adapter is a hardware Direct3D 12 adapter";
        return false;
    }
    game.vkGetPhysicalDeviceMemoryProperties(game.physical, &game.memory);
    return true;
}

// What sharing needs, as the fixture's own queries see it.
bool SharingWorks(const Game& game, std::string& error) {
    for (const VkFormat format : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_D32_SFLOAT}) {
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &external;
        info.format = format;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     (format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        properties.pNext = &external_properties;
        if (game.vkGetPhysicalDeviceImageFormatProperties2(game.physical, &info, &properties) != VK_SUCCESS ||
            !(external_properties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
            error = "VkFormat " + std::to_string(format) + " is not importable from Direct3D 12";
            return false;
        }
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo semaphore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    semaphore.pNext = &type;
    semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    game.vkGetPhysicalDeviceExternalSemaphoreProperties(game.physical, &semaphore, &properties);
    if (!(properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
        error = "Direct3D 12 fences are not importable as timeline semaphores";
        return false;
    }
    return true;
}

bool MakeDevice(Game& game, bool bridge_extensions, bool timeline, std::uint32_t queue_index, std::string& error) {
    std::uint32_t count = 0;
    game.vkGetPhysicalDeviceQueueFamilyProperties(game.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    game.vkGetPhysicalDeviceQueueFamilyProperties(game.physical, &count, families.data());
    game.family = UINT32_MAX;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            game.family = i;
            break;
        }
    }
    if (game.family == UINT32_MAX) {
        error = "no graphics queue family";
        return false;
    }
    game.queue_index = std::min(queue_index, families[game.family].queueCount - 1);
    const std::vector<float> priorities(game.queue_index + 1, 1.0f);
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = game.family;
    queue.queueCount = game.queue_index + 1;
    queue.pQueuePriorities = priorities.data();
    std::vector<const char*> extensions;
    if (bridge_extensions) {
        for (const char* name : VulkanBridge::DeviceExtensions()) extensions.push_back(name);
    }
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create.pNext = timeline ? &features12 : nullptr;
    create.queueCreateInfoCount = 1;
    create.pQueueCreateInfos = &queue;
    create.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create.ppEnabledExtensionNames = extensions.data();
    const VkResult result = game.vkCreateDevice(game.physical, &create, nullptr, &game.device);
    if (result != VK_SUCCESS) {
        error = "vkCreateDevice failed (" + std::to_string(result) + ")";
        return false;
    }
    game.timeline = timeline;
    bool complete = true;
#define GAME_LOAD(name)                                                                                            \
    game.name = reinterpret_cast<PFN_##name>(game.vkGetDeviceProcAddr(game.device, #name));                        \
    complete = complete && game.name != nullptr;
    GAME_DEVICE_FUNCTIONS(GAME_LOAD)
#undef GAME_LOAD
    game.vkImportSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        game.vkGetDeviceProcAddr(game.device, "vkImportSemaphoreWin32HandleKHR"));
    if (!complete) {
        error = "a Vulkan 1.0 device function is missing";
        return false;
    }
    game.vkGetDeviceQueue(game.device, game.family, game.queue_index, &game.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = game.family;
    if (game.vkCreateCommandPool(game.device, &pool, nullptr, &game.pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return false;
    }
    return true;
}

// Waits for the game's queue and lets its recorded work go.
void GameIdle(Game& game) {
    Check(game.vkQueueWaitIdle(game.queue) == VK_SUCCESS, "the game's queue idles");
    if (!game.pending.empty()) {
        game.vkFreeCommandBuffers(game.device, game.pool, static_cast<std::uint32_t>(game.pending.size()),
                                  game.pending.data());
        game.pending.clear();
    }
}

void DestroyDevice(Game& game) {
    if (!game.device) return;
    GameIdle(game);
    if (game.scratch) game.vkDestroyImage(game.device, game.scratch, nullptr);
    if (game.scratch_memory) game.vkFreeMemory(game.device, game.scratch_memory, nullptr);
    game.scratch = VK_NULL_HANDLE;
    game.scratch_memory = VK_NULL_HANDLE;
    game.vkDestroyCommandPool(game.device, game.pool, nullptr);
    game.vkDestroyDevice(game.device, nullptr);
    game.device = VK_NULL_HANDLE;
}

void DestroyInstance(Game& game) {
    DestroyDevice(game);
    if (game.messenger) game.vkDestroyDebugUtilsMessengerEXT(game.instance, game.messenger, nullptr);
    if (game.instance) game.vkDestroyInstance(game.instance, nullptr);
    game.messenger = VK_NULL_HANDLE;
    game.instance = VK_NULL_HANDLE;
}

std::uint32_t MemoryType(const Game& game, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < game.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (game.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    Check(false, "a memory type");
    return 0;
}

// Records work into a command buffer of the game's and submits it.
void GameSubmit(Game& game, const std::function<void(VkCommandBuffer)>& record, VkFence fence = VK_NULL_HANDLE) {
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = game.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    Check(game.vkAllocateCommandBuffers(game.device, &allocate, &commands) == VK_SUCCESS, "a game command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    game.vkBeginCommandBuffer(commands, &begin);
    record(commands);
    game.vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    Check(game.vkQueueSubmit(game.queue, 1, &submit, fence) == VK_SUCCESS, "the game's submission");
    game.pending.push_back(commands);
}

VkImageMemoryBarrier GameBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                 std::uint32_t mip, std::uint32_t mips, std::uint32_t layer, std::uint32_t layers) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, mip, mips, layer, layers};
    return barrier;
}

// The game's rendering: a clear of `image` (layer `layer`), taken from and
// left in the layout OpenXR hands images out in, as an application does (the
// validation layer checks the bridge handed it over in that layout).
void Clear(Game& game, VkImage image, const std::array<float, 4>& color, std::uint32_t layer = 0) {
    GameSubmit(game, [&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier = GameBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
                                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, layer, 1);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
        const VkClearColorValue value{{color[0], color[1], color[2], color[3]}};
        game.vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                  &barrier.subresourceRange);
        std::swap(barrier.oldLayout, barrier.newLayout);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
    });
}

void ClearDepth(Game& game, VkImage image, VkImageAspectFlags aspect, float depth, std::uint32_t stencil,
                std::uint32_t layer = 0, std::uint32_t mip = 0) {
    GameSubmit(game, [&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier = GameBarrier(image, aspect, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip, 1, layer, 1);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
        const VkClearDepthStencilValue value{depth, stencil};
        game.vkCmdClearDepthStencilImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                         &barrier.subresourceRange);
        std::swap(barrier.oldLayout, barrier.newLayout);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
    });
}

// GPU time on the game's queue: clears of a 4096x4096 RGBA16F image.
void GameBusy(Game& game, int clears) {
    if (!game.scratch) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        create.extent = {4096, 4096, 1};
        create.mipLevels = 1;
        create.arrayLayers = 1;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(game.vkCreateImage(game.device, &create, nullptr, &game.scratch) == VK_SUCCESS, "scratch image");
        VkMemoryRequirements requirements{};
        game.vkGetImageMemoryRequirements(game.device, game.scratch, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            MemoryType(game, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(game.vkAllocateMemory(game.device, &allocate, nullptr, &game.scratch_memory) == VK_SUCCESS &&
                  game.vkBindImageMemory(game.device, game.scratch, game.scratch_memory, 0) == VK_SUCCESS,
              "scratch memory");
    }
    GameSubmit(game, [&](VkCommandBuffer commands) {
        VkImageMemoryBarrier barrier = GameBarrier(game.scratch, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &barrier);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        for (int i = 0; i < clears; ++i) {
            const VkClearColorValue value{{static_cast<float>(i) / 64.0f, 0.5f, 0.25f, 1.0f}};
            game.vkCmdClearColorImage(commands, game.scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                      &barrier.subresourceRange);
            game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                      nullptr, 0, nullptr, 1, &barrier);
        }
    });
}

// The game's queue still works: an empty fenced submission completes.
bool GameRuns(Game& game) {
    VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (game.vkCreateFence(game.device, &create, nullptr, &fence) != VK_SUCCESS) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    bool ran = game.vkQueueSubmit(game.queue, 1, &submit, fence) == VK_SUCCESS &&
               game.vkWaitForFences(game.device, 1, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
    game.vkDestroyFence(game.device, fence, nullptr);
    return ran;
}

// ---------------------------------------------------------------------------
// The getter the bridge is given: it records the bridge's barriers, resolves
// and queue calls, and fails submissions or imports on demand.

struct Command {
    bool resolve{false};
    VkPipelineStageFlags source_stages{0};
    VkPipelineStageFlags destination_stages{0};
    std::vector<VkImageMemoryBarrier> barriers;
    VkImage resolve_source{VK_NULL_HANDLE};
    VkImage resolve_destination{VK_NULL_HANDLE};
    VkImageLayout resolve_source_layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout resolve_destination_layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

struct Submission {
    std::vector<VkCommandBuffer> commands;
    std::uint32_t waits{0};
    std::uint32_t signals{0};
    std::uint64_t wait_value{0};
    std::uint64_t signal_value{0};
    bool timeline_values{false};
    bool fence_values{false};
};

struct Recorder {
    std::mutex mutex;
    PFN_vkGetDeviceProcAddr real_get_device_proc_addr{nullptr};
    PFN_vkCmdPipelineBarrier real_barrier{nullptr};
    PFN_vkCmdResolveImage real_resolve{nullptr};
    PFN_vkBeginCommandBuffer real_begin{nullptr};
    PFN_vkQueueSubmit real_submit{nullptr};
    PFN_vkQueueWaitIdle real_queue_wait_idle{nullptr};
    PFN_vkDeviceWaitIdle real_device_wait_idle{nullptr};
    PFN_vkImportSemaphoreWin32HandleKHR real_import{nullptr};
    PFN_vkGetPhysicalDeviceProperties2KHR real_properties2_khr{nullptr};
    std::unordered_map<VkCommandBuffer, std::vector<Command>> recorded;
    std::vector<Submission> submissions;
    std::atomic<bool> in_call{false}; // inside Acquired or Released
    std::atomic<int> queue_calls_outside{0};
    int fail_submits{0}; // the next ones fail with `fail_result`
    VkResult fail_result{VK_SUCCESS};
    bool fail_import{false};
    std::uint64_t device_functions{0}; // device functions the bridge resolved through the getter
    std::atomic<int> properties2_khr_calls{0}; // calls through the KHR name (a Vulkan 1.0 game has only that)

    std::vector<Submission> Take() {
        std::scoped_lock lock(mutex);
        std::vector<Submission> taken;
        taken.swap(submissions);
        return taken;
    }
    std::vector<Command> Commands(VkCommandBuffer commands) {
        std::scoped_lock lock(mutex);
        const auto it = recorded.find(commands);
        return it == recorded.end() ? std::vector<Command>{} : it->second;
    }
};
Recorder g_recorder;

VKAPI_ATTR void VKAPI_CALL RecordingBarrier(VkCommandBuffer commands, VkPipelineStageFlags source,
                                            VkPipelineStageFlags destination, VkDependencyFlags flags,
                                            std::uint32_t memory_count, const VkMemoryBarrier* memory,
                                            std::uint32_t buffer_count, const VkBufferMemoryBarrier* buffers,
                                            std::uint32_t image_count, const VkImageMemoryBarrier* images) {
    {
        std::scoped_lock lock(g_recorder.mutex);
        Command command;
        command.source_stages = source;
        command.destination_stages = destination;
        command.barriers.assign(images, images + image_count);
        g_recorder.recorded[commands].push_back(command);
    }
    g_recorder.real_barrier(commands, source, destination, flags, memory_count, memory, buffer_count, buffers,
                            image_count, images);
}

VKAPI_ATTR void VKAPI_CALL RecordingResolve(VkCommandBuffer commands, VkImage source, VkImageLayout source_layout,
                                            VkImage destination, VkImageLayout destination_layout,
                                            std::uint32_t count, const VkImageResolve* regions) {
    {
        std::scoped_lock lock(g_recorder.mutex);
        Command command;
        command.resolve = true;
        command.resolve_source = source;
        command.resolve_destination = destination;
        command.resolve_source_layout = source_layout;
        command.resolve_destination_layout = destination_layout;
        g_recorder.recorded[commands].push_back(command);
    }
    g_recorder.real_resolve(commands, source, source_layout, destination, destination_layout, count, regions);
}

VKAPI_ATTR VkResult VKAPI_CALL RecordingBegin(VkCommandBuffer commands, const VkCommandBufferBeginInfo* info) {
    {
        std::scoped_lock lock(g_recorder.mutex);
        g_recorder.recorded[commands].clear();
    }
    return g_recorder.real_begin(commands, info);
}

VKAPI_ATTR VkResult VKAPI_CALL RecordingSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits,
                                               VkFence fence) {
    {
        std::scoped_lock lock(g_recorder.mutex);
        if (!g_recorder.in_call) ++g_recorder.queue_calls_outside;
        if (g_recorder.fail_submits > 0) {
            --g_recorder.fail_submits;
            return g_recorder.fail_result;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            Submission submission;
            submission.commands.assign(submits[i].pCommandBuffers,
                                       submits[i].pCommandBuffers + submits[i].commandBufferCount);
            submission.waits = submits[i].waitSemaphoreCount;
            submission.signals = submits[i].signalSemaphoreCount;
            for (auto* next = static_cast<const VkBaseInStructure*>(submits[i].pNext); next; next = next->pNext) {
                if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    const auto& values = *reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
                    submission.timeline_values = true;
                    if (values.waitSemaphoreValueCount) submission.wait_value = values.pWaitSemaphoreValues[0];
                    if (values.signalSemaphoreValueCount) submission.signal_value = values.pSignalSemaphoreValues[0];
                } else if (next->sType == VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR) {
                    const auto& values = *reinterpret_cast<const VkD3D12FenceSubmitInfoKHR*>(next);
                    submission.fence_values = true;
                    if (values.waitSemaphoreValuesCount) submission.wait_value = values.pWaitSemaphoreValues[0];
                    if (values.signalSemaphoreValuesCount) submission.signal_value = values.pSignalSemaphoreValues[0];
                }
            }
            g_recorder.submissions.push_back(submission);
        }
    }
    return g_recorder.real_submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL RecordingQueueWaitIdle(VkQueue queue) {
    if (!g_recorder.in_call) ++g_recorder.queue_calls_outside;
    return g_recorder.real_queue_wait_idle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL RecordingDeviceWaitIdle(VkDevice device) {
    if (!g_recorder.in_call) ++g_recorder.queue_calls_outside;
    return g_recorder.real_device_wait_idle(device);
}

VKAPI_ATTR VkResult VKAPI_CALL RecordingImport(VkDevice device, const VkImportSemaphoreWin32HandleInfoKHR* info) {
    if (g_recorder.fail_import) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    return g_recorder.real_import(device, info);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RecordingGetDeviceProcAddr(VkDevice device, const char* name) {
    const PFN_vkVoidFunction real = g_recorder.real_get_device_proc_addr(device, name);
    ++g_recorder.device_functions;
    if (!real) return nullptr;
    const std::string function(name);
#define RECORD(field, wrapper, type)                                                                               \
    g_recorder.field = reinterpret_cast<type>(real);                                                               \
    return reinterpret_cast<PFN_vkVoidFunction>(&wrapper);
    if (function == "vkCmdPipelineBarrier") { RECORD(real_barrier, RecordingBarrier, PFN_vkCmdPipelineBarrier) }
    if (function == "vkCmdResolveImage") { RECORD(real_resolve, RecordingResolve, PFN_vkCmdResolveImage) }
    if (function == "vkBeginCommandBuffer") { RECORD(real_begin, RecordingBegin, PFN_vkBeginCommandBuffer) }
    if (function == "vkQueueSubmit") { RECORD(real_submit, RecordingSubmit, PFN_vkQueueSubmit) }
    if (function == "vkQueueWaitIdle") { RECORD(real_queue_wait_idle, RecordingQueueWaitIdle, PFN_vkQueueWaitIdle) }
    if (function == "vkDeviceWaitIdle") { RECORD(real_device_wait_idle, RecordingDeviceWaitIdle, PFN_vkDeviceWaitIdle) }
    if (function == "vkImportSemaphoreWin32HandleKHR") {
        RECORD(real_import, RecordingImport, PFN_vkImportSemaphoreWin32HandleKHR)
    }
#undef RECORD
    return real;
}

VKAPI_ATTR void VKAPI_CALL RecordingProperties2Khr(VkPhysicalDevice physical, VkPhysicalDeviceProperties2* properties) {
    ++g_recorder.properties2_khr_calls;
    g_recorder.real_properties2_khr(physical, properties);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL RecordingGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2KHR") == 0) {
        g_recorder.real_properties2_khr =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(g_get_instance_proc_addr(instance, name));
        return g_recorder.real_properties2_khr ? reinterpret_cast<PFN_vkVoidFunction>(&RecordingProperties2Khr)
                                               : nullptr;
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        g_recorder.real_get_device_proc_addr =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_get_instance_proc_addr(instance, name));
        return reinterpret_cast<PFN_vkVoidFunction>(&RecordingGetDeviceProcAddr);
    }
    return g_get_instance_proc_addr(instance, name);
}

VulkanBridgeGame Recording(bool timeline) {
    VulkanBridgeGame game;
    game.get_instance_proc_addr = reinterpret_cast<PFN_xrVoidFunction>(&RecordingGetInstanceProcAddr);
    game.timeline_semaphores = timeline;
    return game;
}

// The bridge's calls on the game's queue are allowed only inside these.
void Acquire(VulkanBridge& bridge, XrSwapchain swapchain, std::uint32_t index) {
    g_recorder.in_call = true;
    bridge.Acquired(swapchain, index);
    g_recorder.in_call = false;
}

bool Release(VulkanBridge& bridge, XrSwapchain swapchain) {
    g_recorder.in_call = true;
    const bool copied = bridge.Released(swapchain);
    g_recorder.in_call = false;
    return copied;
}

// ---------------------------------------------------------------------------
// The stand-in runtime.

LUID g_runtime_luid{};
bool g_requirements_offered = true;
std::vector<ComPtr<ID3D12Resource>> g_runtime_images;
std::vector<std::string> g_log; // what the bridge reported

XrResult XRAPI_CALL FakeGetRequirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* requirements) {
    requirements->adapterLuid = g_runtime_luid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL FakeGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
    if (g_requirements_offered && std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(&FakeGetRequirements);
        return XR_SUCCESS;
    }
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL FakeEnumerate(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = static_cast<uint32_t>(g_runtime_images.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < g_runtime_images.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (std::size_t i = 0; i < g_runtime_images.size(); ++i) {
        Check(d3d12_images[i].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, "the bridge enumerates Direct3D 12 images");
        d3d12_images[i].texture = g_runtime_images[i].Get();
    }
    return XR_SUCCESS;
}

enum class RuntimeKind { kRenderTarget, kDepth, kPlain };

D3D12_RESOURCE_STATES RuntimeState(RuntimeKind kind) {
    return kind == RuntimeKind::kRenderTarget ? D3D12_RESOURCE_STATE_RENDER_TARGET
           : kind == RuntimeKind::kDepth      ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                              : D3D12_RESOURCE_STATE_COMMON;
}

// The stand-in runtime's swapchain: images in the state a runtime hands them
// out in (colour as render targets, depth as depth targets).
std::vector<ComPtr<ID3D12Resource>> MakeImages(ID3D12Device* device, std::uint32_t count, std::uint32_t width,
                                                std::uint32_t height, DXGI_FORMAT format, std::uint32_t array_size = 1,
                                                RuntimeKind kind = RuntimeKind::kRenderTarget,
                                                std::uint32_t mips = 1) {
    std::vector<ComPtr<ID3D12Resource>> images;
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = static_cast<UINT16>(array_size);
        desc.MipLevels = static_cast<UINT16>(mips);
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = kind == RuntimeKind::kRenderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                     : kind == RuntimeKind::kDepth      ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                                        : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> image;
        Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, RuntimeState(kind), nullptr,
                                                        IID_PPV_ARGS(&image))),
              "the stand-in runtime makes its images");
        images.push_back(image);
    }
    return images;
}

void MakeRuntimeImages(ID3D12Device* device, std::uint32_t count, std::uint32_t width, std::uint32_t height,
                       DXGI_FORMAT format, std::uint32_t array_size = 1, RuntimeKind kind = RuntimeKind::kRenderTarget,
                       std::uint32_t mips = 1) {
    g_runtime_images = MakeImages(device, count, width, height, format, array_size, kind, mips);
}

std::uint32_t BytesPerTexel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UINT:
        return 1;
    default:
        return 4;
    }
}

// Reads the centre texel of subresource `subresource` of `image` (in `state`)
// on a queue of the stand-in runtime's own.
std::vector<std::uint8_t> ReadTexel(ID3D12Device* device, ID3D12Resource* image, UINT subresource,
                                    D3D12_RESOURCE_STATES state) {
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &rows, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))) &&
              SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "readback objects");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (state != D3D12_RESOURCE_STATE_COMMON) list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    if (state != D3D12_RESOURCE_STATE_COMMON) list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "the readback finishes");
    CloseHandle(event);
    void* mapped = nullptr;
    Check(SUCCEEDED(buffer->Map(0, nullptr, &mapped)), "map the readback");
    const std::uint32_t texel_bytes = BytesPerTexel(footprint.Footprint.Format);
    const auto* texel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                        static_cast<std::size_t>(footprint.Footprint.Height / 2) * footprint.Footprint.RowPitch +
                        static_cast<std::size_t>(footprint.Footprint.Width / 2) * texel_bytes;
    std::vector<std::uint8_t> result(texel, texel + texel_bytes);
    buffer->Unmap(0, nullptr);
    return result;
}

std::vector<std::uint8_t> ReadRuntimeImage(ID3D12Device* device, std::uint32_t index, std::uint32_t slice = 0,
                                           RuntimeKind kind = RuntimeKind::kRenderTarget, std::uint32_t mip = 0,
                                           std::uint32_t plane = 0) {
    ID3D12Resource* image = g_runtime_images[index].Get();
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const UINT subresource = mip + slice * desc.MipLevels + plane * desc.MipLevels * desc.DepthOrArraySize;
    return ReadTexel(device, image, subresource, RuntimeState(kind));
}

std::uint8_t Unorm8(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::uint8_t Srgb8(float value) {
    const float encoded = value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    return Unorm8(encoded);
}

bool Near(int value, int expected, int tolerance = 1) {
    return std::abs(value - expected) <= tolerance;
}

std::string Bytes(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    for (const std::uint8_t byte : bytes) {
        if (!text.empty()) text += ",";
        text += std::to_string(byte);
    }
    return text;
}

// An RGBA8 texel against a clear colour (UNORM 0.5 rounds either way).
bool NearRgba8(const std::vector<std::uint8_t>& texel, const std::array<float, 4>& color, bool srgb = false) {
    if (texel.size() < 4) return false;
    for (int c = 0; c < 4; ++c) {
        const std::uint8_t expected = srgb && c < 3 ? Srgb8(color[c]) : Unorm8(color[c]);
        if (!Near(texel[c], expected)) return false;
    }
    return true;
}

float HalfToFloat(std::uint16_t half) {
    const int exponent = (half >> 10) & 0x1f;
    const int mantissa = half & 0x3ff;
    const float sign = (half & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) return sign * std::ldexp(static_cast<float>(mantissa), -24);
    return sign * std::ldexp(1.0f + mantissa / 1024.0f, exponent - 15);
}

// An unsigned small float (11 or 10 bits: 5 exponent bits, 6 or 5 mantissa bits).
float SmallFloat(std::uint32_t bits, int mantissa_bits) {
    const std::uint32_t mantissa = bits & ((1u << mantissa_bits) - 1);
    const int exponent = static_cast<int>(bits >> mantissa_bits) & 0x1f;
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - mantissa_bits);
    return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits), exponent - 15);
}

// The runtime's texel (DXGI layout) against what the game cleared with.
bool TexelMatches(VkFormat format, const std::vector<std::uint8_t>& texel, const std::array<float, 4>& color) {
    std::uint32_t packed = 0;
    if (texel.size() >= 4) std::memcpy(&packed, texel.data(), 4);
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return NearRgba8(texel, color);
    case VK_FORMAT_R8G8B8A8_SRGB:
        return NearRgba8(texel, color, true);
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: {
        const bool srgb = format == VK_FORMAT_B8G8R8A8_SRGB;
        const auto encode = [&](float value) { return srgb ? Srgb8(value) : Unorm8(value); };
        return texel.size() == 4 && Near(texel[0], encode(color[2])) && Near(texel[1], encode(color[1])) &&
               Near(texel[2], encode(color[0])) && Near(texel[3], Unorm8(color[3]));
    }
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
        if (texel.size() != 8) return false;
        for (int c = 0; c < 4; ++c) {
            std::uint16_t half = 0;
            std::memcpy(&half, texel.data() + 2 * c, 2);
            if (HalfToFloat(half) != color[c]) return false;
        }
        return true;
    }
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return Near(packed & 0x3ff, std::lround(color[0] * 1023)) &&
               Near((packed >> 10) & 0x3ff, std::lround(color[1] * 1023)) &&
               Near((packed >> 20) & 0x3ff, std::lround(color[2] * 1023)) &&
               static_cast<long>(packed >> 30) == std::lround(color[3] * 3);
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return SmallFloat(packed & 0x7ff, 6) == color[0] && SmallFloat((packed >> 11) & 0x7ff, 6) == color[1] &&
               SmallFloat(packed >> 22, 5) == color[2];
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
        if (texel.size() != 16) return false;
        float values[4]{};
        std::memcpy(values, texel.data(), 16);
        return values[0] == color[0] && values[1] == color[1] && values[2] == color[2] && values[3] == color[3];
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Direct3D 12 helpers.

// Every error and corruption message the Direct3D 12 debug layer stored since the last check.
std::string D3D12Errors(ID3D12Device* device) {
    std::string errors;
    ComPtr<ID3D12InfoQueue> queue12;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue12)))) {
        for (UINT64 i = 0; i < queue12->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue12->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue12->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                errors += std::string("[D3D12 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue12->ClearStoredMessages();
    }
    return errors;
}

ID3D12Device* BridgeDevice(const VulkanBridge& bridge) {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(bridge.Binding())->device;
}

ID3D12CommandQueue* BridgeQueue(const VulkanBridge& bridge) {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(bridge.Binding())->queue;
}

// A fence the test opens: the game's queue or a Direct3D 12 queue held back
// until Open. The game's side is the fence imported as a timeline semaphore.
struct Gate {
    ComPtr<ID3D12Fence> fence;
    Game& game;
    VkSemaphore semaphore{VK_NULL_HANDLE};
    Gate(ID3D12Device* device, Game& game_) : game(game_) {
        HANDLE handle = nullptr;
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) &&
                  SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)),
              "a gate fence");
        if (game.timeline && game.vkImportSemaphoreWin32HandleKHR) {
            VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            create.pNext = &type;
            Check(game.vkCreateSemaphore(game.device, &create, nullptr, &semaphore) == VK_SUCCESS, "a gate semaphore");
            VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
            import.semaphore = semaphore;
            import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            import.handle = handle;
            Check(game.vkImportSemaphoreWin32HandleKHR(game.device, &import) == VK_SUCCESS,
                  "the gate imported into the game's device");
        }
        CloseHandle(handle);
    }
    // Everything the game submits from here on waits for Open.
    void HoldGame() {
        const std::uint64_t value = 1;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &value;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &values;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &stage;
        Check(game.vkQueueSubmit(game.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS, "the game is held");
    }
    void HoldQueue(ID3D12CommandQueue* queue) { queue->Wait(fence.Get(), 1); }
    void Open() { fence->Signal(1); }
    ~Gate() {
        Open();
        if (semaphore) {
            GameIdle(game);
            game.vkDestroySemaphore(game.device, semaphore, nullptr);
        }
    }
};

struct Session {
    XrBaseInStructure application_struct{static_cast<XrStructureType>(1000999000)}; // after the binding
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    explicit Session(const Game& game) {
        binding.next = &application_struct;
        binding.instance = game.instance;
        binding.physicalDevice = game.physical;
        binding.device = game.device;
        binding.queueFamilyIndex = game.family;
        binding.queueIndex = game.queue_index;
        info.next = &binding;
        info.systemId = 1;
    }
};

XrSwapchainCreateInfo ColorInfo(std::uint32_t width, std::uint32_t height, std::uint32_t samples = 1,
                                std::uint32_t array_size = 1, VkFormat format = VK_FORMAT_R8G8B8A8_SRGB) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = samples;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    return info;
}

XrSwapchainCreateInfo DepthInfo(std::uint32_t width, std::uint32_t height, VkFormat format = VK_FORMAT_D32_SFLOAT,
                                std::uint32_t array_size = 1, std::uint32_t mips = 1, std::uint32_t samples = 1) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = samples;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = mips;
    return info;
}

XrSwapchain Handle(std::uintptr_t value) {
    return reinterpret_cast<XrSwapchain>(value);
}

VkImage AsImage(std::uint64_t handle) {
    return reinterpret_cast<VkImage>(handle);
}

std::vector<VkImage> GameImages(const VulkanBridge& bridge, XrSwapchain swapchain) {
    std::vector<VkImage> images;
    for (const std::uint64_t handle : bridge.Images(swapchain)) images.push_back(AsImage(handle));
    return images;
}

void BeginBridge(Game& game, VulkanBridge& bridge, Session& session, bool timeline = true) {
    std::string reason;
    std::string description;
    g_runtime_luid = game.luid;
    g_log.clear();
    bridge.SetLog([](const std::string& message) { g_log.push_back(message); });
    Check(bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(timeline), reason,
                       description) != nullptr,
          "bridged: " + reason);
    Check(Contains(description, timeline ? "semaphores=timeline" : "semaphores=binary"),
          "the semaphore mode is described: " + description);
    bridge.SessionCreated();
}

// The case's GPU work passed both validators, the bridge reported nothing and
// used the game's queue only inside Acquired and Released.
void CheckClean(const VulkanBridge* bridge, const std::string& test) {
    if (bridge && bridge->Active()) {
        const std::string errors = D3D12Errors(BridgeDevice(*bridge));
        Check(errors.empty(), test + ": the Direct3D 12 debug layer reported\n" + errors);
    }
    std::string messages;
    for (const std::string& message : g_messages.Take()) messages += message + "\n";
    Check(messages.empty(), test + ": the Vulkan validation layer reported\n" + messages);
    // Frames queued without waiting may put the game's queue far enough
    // behind for more fences: reported, and no fault.
    g_log.erase(std::remove_if(g_log.begin(), g_log.end(),
                               [](const std::string& line) { return Contains(line, "more fences are made"); }),
                g_log.end());
    std::string log;
    for (const std::string& message : g_log) log += message + "\n";
    Check(g_log.empty(), test + ": the bridge reported\n" + log);
    Check(g_recorder.queue_calls_outside == 0,
          test + ": the bridge used the game's queue outside the application's acquire and release (" +
              std::to_string(g_recorder.queue_calls_outside.load()) + " calls)");
}

std::array<float, 4> FrameColor(int frame) {
    return {static_cast<float>((frame * 37 + 11) % 251) / 255.0f, static_cast<float>((frame * 11 + 3) % 247) / 255.0f,
            static_cast<float>((frame * 5 + 7) % 241) / 255.0f, 1.0f};
}

std::string Text(const std::array<float, 4>& color) {
    return std::to_string(color[0]) + "," + std::to_string(color[1]) + "," + std::to_string(color[2]) + "," +
           std::to_string(color[3]);
}

// ---------------------------------------------------------------------------
// Cases.

void TestBegin(Game& game, Game& bare_game) {
    VulkanBridge bridge;
    std::string reason;
    std::string description;
    Session session(game);
    g_runtime_luid = game.luid;

    // Not a Vulkan binding first.
    XrSessionCreateInfo other{XR_TYPE_SESSION_CREATE_INFO};
    other.next = &session.application_struct;
    Check(!bridge.Begin(XR_NULL_HANDLE, &other, FakeGetInstanceProcAddr, Recording(true), reason, description) &&
              Contains(reason, "first structure") && !bridge.QueriedD3D12Requirements(),
          "only a Vulkan binding in first place is bridged: " + reason);

    // The runtime offers no Direct3D 12 requirements (XR_KHR_D3D12_enable not enabled).
    g_requirements_offered = false;
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(true), reason, description) &&
              Contains(reason, "xrGetD3D12GraphicsRequirementsKHR") && !bridge.QueriedD3D12Requirements(),
          "no bridge without the runtime's requirements: " + reason);
    g_requirements_offered = true;

    // The runtime composes on another adapter (its requirements were asked for).
    g_runtime_luid.LowPart ^= 0x5a5a;
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(true), reason, description) &&
              Contains(reason, "adapter") && bridge.QueriedD3D12Requirements(),
          "no bridge across adapters: " + reason);
    g_runtime_luid = game.luid;
    bridge.End();
    Check(!bridge.QueriedD3D12Requirements(), "End forgets that the Direct3D 12 requirements were asked for");

    // The kill switch.
    _putenv_s("VECTORXR_VULKAN_BRIDGE", "off");
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(true), reason, description) &&
              Contains(reason, "VECTORXR_VULKAN_BRIDGE"),
          "VECTORXR_VULKAN_BRIDGE=off keeps Vulkan: " + reason);
    _putenv_s("VECTORXR_VULKAN_BRIDGE", "");

    // A device made without the bridge's extensions (a game that ignored the lists).
    Session bare_session(bare_game);
    Check(!bridge.Begin(XR_NULL_HANDLE, &bare_session.info, FakeGetInstanceProcAddr, Recording(true), reason,
                        description) &&
              Contains(reason, "lacks the bridge's extensions"),
          "a device without the extensions is refused: " + reason);

    const std::uint64_t resolved_before = g_recorder.device_functions;
    const XrSessionCreateInfo* bridged =
        bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(true), reason, description);
    Check(bridged != nullptr, "the session is bridged: " + reason);
    Check(g_recorder.device_functions > resolved_before, "device functions are resolved through the game's getter");
    Check(bridge.Active(), "active after Begin");
    Check(bridged->systemId == 1 && bridged->createFlags == session.info.createFlags, "the create info is kept");
    const auto* binding = static_cast<const XrGraphicsBindingD3D12KHR*>(bridged->next);
    Check(binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && binding->device && binding->queue,
          "the runtime gets a Direct3D 12 binding");
    const D3D12_COMMAND_QUEUE_DESC queue = binding->queue->GetDesc();
    Check(queue.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && queue.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
          "the bridge's queue is a high-priority direct queue");
    Check(binding->next == &session.application_struct, "the application's other structures follow the binding");
    Check(bridge.Binding() == binding, "Binding() is the session's binding");
    Check(Contains(description, "vulkanDevice=") && Contains(description, "videoMemory=") &&
              Contains(description, "semaphores=timeline") &&
              Contains(description, "queueIndex=" + std::to_string(game.queue_index)),
          "the description names the Vulkan device, queue, semaphores and video memory: " + description);
    std::cout << "bridged session: " << description << '\n';
    bridge.SessionCreated();
    Check(binding->next == nullptr, "after xrCreateSession the application's structures are not followed");
    Check(bridge.Drain(), "an idle bridge drains at once");
    Check(bridge.Escape(), "an idle bridge escapes at once");
    std::string lost;
    Check(!bridge.Lost(lost), "the device is not lost");
    Check(Contains(bridge.Summary(), "semaphores=timeline"), "the summary names the semaphores");
    bridge.End();
    Check(!bridge.Active() && bridge.Binding() == nullptr, "inactive after End");
    Check(GameRuns(game), "the game's device works after End");

    // The game's own loader when the hooks recorded no getter, and binary
    // semaphores when the device has no timeline semaphores.
    VulkanBridgeGame loader_game;
    Check(bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, loader_game, reason, description) &&
              Contains(description, "semaphores=binary"),
          "bridged through the loader the game loaded, with binary semaphores: " + reason + description);
    bridge.End();
    CheckClean(nullptr, "begin");
}

// A failure after Begin made Vulkan objects (an import refused): Begin lets
// them go, so the device is destroyed without leaks (the validation layer
// checks vkDestroyDevice).
void TestBeginFailureCleanup(Game& game) {
    Game device_game = game;
    device_game.device = VK_NULL_HANDLE;
    device_game.pending.clear();
    device_game.scratch = VK_NULL_HANDLE;
    device_game.scratch_memory = VK_NULL_HANDLE;
    std::string error;
    Check(MakeDevice(device_game, true, true, 0, error), "a second device: " + error);
    VulkanBridge bridge;
    Session session(device_game);
    std::string reason;
    std::string description;
    g_recorder.fail_import = true;
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, Recording(true), reason, description) &&
              Contains(reason, "Importing"),
          "an import refused fails Begin: " + reason);
    g_recorder.fail_import = false;
    DestroyDevice(device_game);
    CheckClean(nullptr, "begin failure cleanup");
}

void TestSwapchainChecks(Game& game) {
    VulkanBridge bridge;
    XrResult refusal = XR_SUCCESS;
    std::string reason;
    Check(!bridge.CheckSwapchain(ColorInfo(32, 32), refusal, reason) && refusal == XR_ERROR_RUNTIME_FAILURE,
          "an unbridged session checks nothing");
    Session session(game);
    BeginBridge(game, bridge, session);

    const std::vector<std::int64_t> runtime{
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8A8_UNORM,      DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R10G10B10A2_UNORM,   DXGI_FORMAT_R11G11B10_FLOAT,     DXGI_FORMAT_R32G32B32A32_FLOAT,
        DXGI_FORMAT_D32_FLOAT,           DXGI_FORMAT_D24_UNORM_S8_UINT,   DXGI_FORMAT_D16_UNORM,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    const std::vector<std::int64_t> formats = bridge.GameFormats(runtime);
    const std::vector<std::int64_t> translated = VulkanBridge::TranslatedFormats(runtime);
    std::string listed;
    for (const std::int64_t format : formats) listed += std::to_string(format) + " ";
    std::cout << "importable formats offered: " << listed << '\n';
    Check(formats == translated, "every mapped format imports on this GPU and is offered in order: " + listed);

    const auto check = [&](const XrSwapchainCreateInfo& info, XrResult expected, const std::string& what) {
        const bool accepted = bridge.CheckSwapchain(info, refusal, reason);
        Check(expected == XR_SUCCESS ? accepted : (!accepted && refusal == expected && !reason.empty()),
              what + ": " + (accepted ? std::string("accepted") : std::to_string(refusal) + " " + reason));
    };
    check(ColorInfo(64, 64), XR_SUCCESS, "plain sRGB colour");
    check(ColorInfo(64, 64, 4), XR_SUCCESS, "multisampled colour");
    check(DepthInfo(64, 64), XR_SUCCESS, "depth");
    check(DepthInfo(64, 64, VK_FORMAT_D24_UNORM_S8_UINT, 2), XR_SUCCESS, "depth-stencil array");
    check(DepthInfo(64, 64, VK_FORMAT_D32_SFLOAT, 1, 2), XR_SUCCESS, "depth with mips");
    check(DepthInfo(64, 64, VK_FORMAT_D24_UNORM_S8_UINT, 1, 2), XR_ERROR_FEATURE_UNSUPPORTED,
          "a mipmapped depth-stencil image");
    check(DepthInfo(64, 64, VK_FORMAT_D32_SFLOAT, 2, 2), XR_ERROR_FEATURE_UNSUPPORTED,
          "an array of mipmapped images");
    check(DepthInfo(64, 64, VK_FORMAT_D32_SFLOAT, 1, 1, 4), XR_SUCCESS, "multisampled depth");
    XrSwapchainCreateInfo info = ColorInfo(64, 64);
    info.format = VK_FORMAT_R8G8B8_UNORM;
    check(info, XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "a format without a DXGI equivalent");
    info = ColorInfo(64, 64);
    info.faceCount = 6;
    check(info, XR_ERROR_FEATURE_UNSUPPORTED, "a cube");
    info = ColorInfo(64, 64);
    info.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    check(info, XR_ERROR_FEATURE_UNSUPPORTED, "depth usage on colour");
    check(ColorInfo(1u << 17, 64), XR_ERROR_FEATURE_UNSUPPORTED, "an image too wide");
    info = ColorInfo(64, 64);
    info.usageFlags = XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    // sRGB images have no storage views; UNORM ones need Vulkan 1.2, and the
    // game's API version is not known.
    check(info, XR_ERROR_FEATURE_UNSUPPORTED, "sRGB with storage");
    info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    check(info, XR_ERROR_FEATURE_UNSUPPORTED, "sRGB with storage, mutable");
    info = ColorInfo(64, 64, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    info.usageFlags = XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    check(info, XR_SUCCESS, "UNORM with storage");
    const VkFormat view_formats[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
    XrVulkanSwapchainFormatListCreateInfoKHR list{XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
    list.viewFormatCount = 2;
    list.viewFormats = view_formats;
    info = ColorInfo(64, 64);
    info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    info.next = &list;
    check(info, XR_SUCCESS, "a mutable swapchain with a format list");
    XrVulkanSwapchainCreateInfoMETA meta{XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
    meta.additionalCreateFlags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    info = ColorInfo(64, 64);
    info.next = &meta;
    check(info, XR_ERROR_FEATURE_UNSUPPORTED, "flags the import cannot provide");
    CheckClean(&bridge, "swapchain checks");
    bridge.End();
}

// Frames through a three-image swapchain: each release reaches its runtime
// image, with no wait between frames (copies of an image may still be queued
// when it is released again). The game's images and the runtime's are typed,
// or the runtime's typeless as runtimes' usually are.
void TestTwinFrames(Game& game, bool timeline, bool typeless) {
    const std::string test = std::string("twin frames (") + (timeline ? "timeline" : "binary") +
                             (typeless ? ", typeless runtime images)" : ")");
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 3, 64, 48, typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x10);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(64, 48), FakeEnumerate, reason), "bridged swapchain: " + reason);
    Check(bridge.Bridged(swapchain) && !bridge.Unmirrored(swapchain) && !bridge.HasUnmirrored(),
          "a colour swapchain is mirrored");
    const std::vector<VkImage> images = GameImages(bridge, swapchain);
    Check(images.size() == 3, "one twin per runtime image");
    Check(Contains(bridge.Describe(swapchain), "mode=twin,"), "described: " + bridge.Describe(swapchain));

    // The application's xrEnumerateSwapchainImages: two calls, Vulkan structures.
    std::uint32_t count = 0;
    Check(bridge.EnumerateImages(swapchain, 0, &count, nullptr) == XR_SUCCESS && count == 3, "the count call");
    std::vector<XrSwapchainImageVulkanKHR> enumerated(3, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    Check(bridge.EnumerateImages(swapchain, 2, &count,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(enumerated.data())) ==
              XR_ERROR_SIZE_INSUFFICIENT,
          "too small a capacity");
    std::vector<XrSwapchainImageD3D12KHR> wrong(3, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    Check(bridge.EnumerateImages(swapchain, 3, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(wrong.data())) ==
              XR_ERROR_VALIDATION_FAILURE,
          "a structure of another graphics API");
    Check(bridge.EnumerateImages(swapchain, 3, &count,
                                 reinterpret_cast<XrSwapchainImageBaseHeader*>(enumerated.data())) == XR_SUCCESS,
          "the fill call");
    for (std::uint32_t i = 0; i < 3; ++i) {
        Check(enumerated[i].image == images[i], "enumerated image " + std::to_string(i));
    }
    Check(bridge.EnumerateImages(Handle(0x11), 0, &count, nullptr) == XR_ERROR_HANDLE_INVALID, "not bridged");

    constexpr int kFrames = 30;
    std::array<std::array<float, 4>, 3> last{};
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::uint32_t index = frame % 3;
        const std::array<float, 4> color = FrameColor(frame);
        Acquire(bridge, swapchain, index);
        Clear(game, images[index], color);
        Check(Release(bridge, swapchain), "frame " + std::to_string(frame) + " is copied");
        last[index] = color;
        bridge.FrameEnded();
    }
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    for (std::uint32_t index = 0; index < 3; ++index) {
        const auto texel = ReadRuntimeImage(device, index);
        Check(NearRgba8(texel, last[index], true), test + ": runtime image " + std::to_string(index) +
                                                       " holds its last release: " + Bytes(texel) + ", expected " +
                                                       Text(last[index]));
    }
    const std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "copies") == kFrames && SummaryValue(summary, "skippedCopies") == 0 &&
              SummaryValue(summary, "submits") == 2 * kFrames && SummaryValue(summary, "writtenValue") == kFrames,
          test + ": every frame copied, two submissions each: " + summary);
    std::cout << test << ": " << summary << '\n';
    GameIdle(game);
    CheckClean(&bridge, test);
    Check(bridge.RemoveSwapchain(swapchain), "the copies into a removed swapchain finished");
    Check(!bridge.Bridged(swapchain) && bridge.Images(swapchain).empty(), "removed");
    Check(Contains(bridge.Summary(), "(0 live, 0 kept)"), "summary: " + bridge.Summary());
    bridge.End();
    Check(GameRuns(game), "the game's device works after End");
    CheckClean(nullptr, test + " teardown");
}

// Every colour format the bridge maps, with channel-distinct values each
// format holds exactly, through two frames (the first and the later hand-over).
void TestColorFormats(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    const VkFormat formats[] = {VK_FORMAT_R8G8B8A8_SRGB,           VK_FORMAT_B8G8R8A8_SRGB,
                                VK_FORMAT_R8G8B8A8_UNORM,          VK_FORMAT_B8G8R8A8_UNORM,
                                VK_FORMAT_R16G16B16A16_SFLOAT,     VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R32G32B32A32_SFLOAT};
    const std::array<std::array<float, 4>, 2> colors{{{0.25f, 0.5f, 0.75f, 1.0f}, {0.75f, 0.125f, 0.5f, 0.0f}}};
    std::uintptr_t handle = 0x200;
    for (const VkFormat format : formats) {
        MakeRuntimeImages(device, 1, 16, 16, static_cast<DXGI_FORMAT>(VulkanBridge::RuntimeFormat(format)));
        const XrSwapchain swapchain = Handle(handle++);
        std::string reason;
        Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16, 1, 1, format), FakeEnumerate, reason),
              "VkFormat " + std::to_string(format) + " is bridged: " + reason);
        const VkImage image = GameImages(bridge, swapchain)[0];
        for (const auto& color : colors) {
            Acquire(bridge, swapchain, 0);
            Clear(game, image, color);
            Check(Release(bridge, swapchain), "copied");
            Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
            const auto texel = ReadRuntimeImage(device, 0);
            Check(TexelMatches(format, texel, color), "VkFormat " + std::to_string(format) +
                                                          " reaches the runtime: " + Bytes(texel) + " for " +
                                                          Text(color));
        }
        GameIdle(game);
        Check(bridge.RemoveSwapchain(swapchain), "removed");
    }
    CheckClean(&bridge, "colour formats");
    bridge.End();
}

// The copy runs only after the game's rendering: with the game's queue held
// back before it draws, the copy does not run; once released, the runtime's
// image gets the frame.
void TestWrittenOrdering(Game& game, bool timeline) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x18);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(64, 64), FakeEnumerate, reason), "bridged: " + reason);
    const std::array<float, 4> color{0.9f, 0.45f, 0.05f, 1.0f};
    {
        Gate gate(device, game);
        gate.HoldGame();
        Acquire(bridge, swapchain, 1);
        Clear(game, GameImages(bridge, swapchain)[1], color);
        Release(bridge, swapchain);
        Sleep(150);
        const std::string summary = bridge.Summary();
        Check(SummaryValue(summary, "doneCompleted") < SummaryValue(summary, "doneValue"),
              "the copy waits for the game's rendering: " + summary);
        gate.Open();
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    }
    const auto texel = ReadRuntimeImage(device, 1);
    Check(NearRgba8(texel, color, true), "the runtime's image holds the frame drawn after the gate: " + Bytes(texel));
    CheckClean(&bridge, std::string("written ordering ") + (timeline ? "timeline" : "binary"));
    bridge.End();
}

// An image released again while its last copy is still queued gets another
// recording of the copy (D3D12 forbids executing a command list still pending).
void TestPendingRelease(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 1, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x1c);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    const VkImage image = GameImages(bridge, swapchain)[0];
    const std::array<float, 4> first{0.8f, 0.1f, 0.1f, 1.0f};
    const std::array<float, 4> second{0.1f, 0.8f, 0.1f, 1.0f};
    {
        Gate gate(device, game);
        gate.HoldGame();
        Acquire(bridge, swapchain, 0);
        Clear(game, image, first);
        Release(bridge, swapchain); // its copy waits behind the gate
        Acquire(bridge, swapchain, 0);
        Clear(game, image, second);
        Release(bridge, swapchain);
        const std::string summary = bridge.Summary();
        Check(SummaryValue(summary, "copyRecordings") == 2 && SummaryValue(summary, "copies") == 2,
              "the second release used a second recording: " + summary);
    }
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(NearRgba8(ReadRuntimeImage(device, 0), second, true), "the runtime's image holds the later frame");
    CheckClean(&bridge, "pending release");
    bridge.End();
}

// The game renders into an image again while its copy is still queued (the
// bridge's queue held back): the game's queue waits for the copy, so the
// runtime's image holds the released frame, never the next one.
void TestDoneWait(Game& game, bool timeline) {
    const std::string test = std::string("done wait (") + (timeline ? "timeline)" : "binary)");
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 1, 256, 256, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x20);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(256, 256), FakeEnumerate, reason), "bridged: " + reason);
    const VkImage image = GameImages(bridge, swapchain)[0];
    for (int round = 0; round < 3; ++round) {
        const std::array<float, 4> released = FrameColor(2 * round);
        const std::array<float, 4> next = FrameColor(2 * round + 1);
        {
            Gate gate(device, game);
            gate.HoldQueue(BridgeQueue(bridge));
            Acquire(bridge, swapchain, 0);
            Clear(game, image, released);
            Check(Release(bridge, swapchain), "released");
            const std::uint64_t waits = SummaryValue(bridge.Summary(), "doneWaits");
            Acquire(bridge, swapchain, 0); // the copy is held: the game's queue waits for it
            Check(SummaryValue(bridge.Summary(), "doneWaits") == waits + 1, test + ": the acquire waits for the copy");
            Clear(game, image, next);
            Sleep(30);
        }
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        GameIdle(game);
        auto texel = ReadRuntimeImage(device, 0);
        Check(NearRgba8(texel, released, true),
              test + ": the runtime's image holds the released frame: " + Bytes(texel) + ", expected " +
                  Text(released));
        Check(Release(bridge, swapchain), "the next frame is released");
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        texel = ReadRuntimeImage(device, 0);
        Check(NearRgba8(texel, next, true), test + ": then the next one: " + Bytes(texel));
    }
    GameIdle(game);
    CheckClean(&bridge, test);
    bridge.End();
}

// Three frames in flight with both queues racing (the game's slow in the first
// half, the bridge's slow in the second): every frame's copy, read back on the
// bridge's queue right after it, holds that frame.
void TestRace(Game& game, bool timeline) {
    const std::string test = std::string("race (") + (timeline ? "timeline)" : "binary)");
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ID3D12Device* device = BridgeDevice(bridge);
    ID3D12CommandQueue* queue = BridgeQueue(bridge);
    constexpr std::uint32_t kSize = 512;
    constexpr int kFrames = 36;
    MakeRuntimeImages(device, 3, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM);
    const XrSwapchain swapchain = Handle(0x24);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(kSize, kSize, 1, 1, VK_FORMAT_R8G8B8A8_UNORM), FakeEnumerate,
                              reason),
          "bridged: " + reason);
    const std::vector<VkImage> images = GameImages(bridge, swapchain);

    // The bridge's queue's own load and per-frame readbacks (as Frame
    // Generation's captures would be queued after the copy).
    D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 256ull << 20;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> busy_a;
    ComPtr<ID3D12Resource> busy_b;
    ComPtr<ID3D12Resource> readback;
    Check(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&busy_a))) &&
              SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&busy_b))),
          "busy buffers");
    buffer.Width = kFrames * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    Check(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))),
          "the readback buffer");
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators(kFrames);
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists(kFrames);
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::uint32_t index = frame % 3;
        const bool bridge_slow = frame >= kFrames / 2;
        Acquire(bridge, swapchain, index);
        if (!bridge_slow) GameBusy(game, 24);
        Clear(game, images[index], FrameColor(frame));
        Check(Release(bridge, swapchain), "copied");
        Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocators[frame]))) &&
                  SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[frame].Get(),
                                                      nullptr, IID_PPV_ARGS(&lists[frame]))),
              "a readback list");
        ID3D12GraphicsCommandList* list = lists[frame].Get();
        if (bridge_slow) {
            for (int i = 0; i < 6; ++i) list->CopyResource(busy_b.Get(), busy_a.Get());
        }
        ID3D12Resource* runtime = g_runtime_images[index].Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = runtime;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint.Offset = static_cast<UINT64>(frame) * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1,
                                                 D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        D3D12_TEXTURE_COPY_LOCATION source{runtime, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX box{kSize / 2, kSize / 2, 0, kSize / 2 + 1, kSize / 2 + 1, 1};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList* run[] = {list};
        queue->ExecuteCommandLists(1, run);
    }
    Check(bridge.WaitIdle(20000), "the bridge's queue finishes");
    GameIdle(game);
    void* mapped = nullptr;
    Check(SUCCEEDED(readback->Map(0, nullptr, &mapped)), "map the readbacks");
    int wrong = 0;
    std::string first_wrong;
    for (int frame = 0; frame < kFrames; ++frame) {
        const auto* bytes = static_cast<const std::uint8_t*>(mapped) + frame * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        const std::vector<std::uint8_t> texel(bytes, bytes + 4);
        if (!NearRgba8(texel, FrameColor(frame))) {
            if (!wrong) first_wrong = "frame " + std::to_string(frame) + ": " + Bytes(texel);
            ++wrong;
        }
    }
    readback->Unmap(0, nullptr);
    const std::string summary = bridge.Summary();
    std::cout << test << ": " << wrong << " of " << kFrames << " frames wrong, " << summary << '\n';
    Check(wrong == 0, test + ": " + std::to_string(wrong) + " frames read back wrong (" + first_wrong + ")");
    CheckClean(&bridge, test);
    bridge.End();
}

// Multisampled images are rendered privately and resolved into the twin, two
// rounds on every index (the first hand-over and the later ones).
void TestResolve(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x30);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32, 4), FakeEnumerate, reason), "bridged: " + reason);
    Check(Contains(bridge.Describe(swapchain), "mode=resolve") && Contains(bridge.Describe(swapchain), "samples=4"),
          "the game renders four samples: " + bridge.Describe(swapchain));
    const std::vector<VkImage> images = GameImages(bridge, swapchain);
    for (int round = 0; round < 2; ++round) {
        for (std::uint32_t index = 0; index < 2; ++index) {
            const std::array<float, 4> color = FrameColor(round * 2 + static_cast<int>(index));
            Acquire(bridge, swapchain, index);
            Clear(game, images[index], color);
            Check(Release(bridge, swapchain), "resolved and copied");
            Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
            const auto texel = ReadRuntimeImage(device, index);
            Check(NearRgba8(texel, color, true), "round " + std::to_string(round) + ", image " + std::to_string(index) +
                                                     ": the resolved frame reaches the runtime: " + Bytes(texel));
        }
    }
    GameIdle(game);
    CheckClean(&bridge, "resolve");
    bridge.End();
}

// Array images: every slice reaches the runtime.
void TestArray(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2);
    const XrSwapchain swapchain = Handle(0x40);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32, 1, 2), FakeEnumerate, reason), "bridged: " + reason);
    const VkImage image = GameImages(bridge, swapchain)[0];
    const std::array<float, 4> left{0.8f, 0.05f, 0.05f, 1.0f};
    const std::array<float, 4> right{0.05f, 0.05f, 0.8f, 1.0f};
    for (int frame = 0; frame < 2; ++frame) {
        Acquire(bridge, swapchain, 0);
        Clear(game, image, frame ? right : left, 0);
        Clear(game, image, frame ? left : right, 1);
        Check(Release(bridge, swapchain), "copied");
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        Check(NearRgba8(ReadRuntimeImage(device, 0, 0), frame ? right : left, true) &&
                  NearRgba8(ReadRuntimeImage(device, 0, 1), frame ? left : right, true),
              "both slices reach the runtime");
    }
    GameIdle(game);
    CheckClean(&bridge, "array");
    bridge.End();
}

// Depth reaches the runtime's depth images: every mapped depth format, typed
// and typeless runtime images, two layers or two mips, depth and stencil.
// Arrays of mipmapped images and mipmapped depth-stencil images are refused
// (they do not round-trip).
void TestDepthValues(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    XrResult refusal = XR_SUCCESS;
    std::string reason;
    Check(!bridge.CheckSwapchain(DepthInfo(32, 32, VK_FORMAT_D32_SFLOAT, 2, 2), refusal, reason) &&
              refusal == XR_ERROR_FEATURE_UNSUPPORTED,
          "an array of mipmapped depth images is refused: " + reason);
    XrSwapchainCreateInfo mipmapped_array = ColorInfo(32, 32, 1, 2);
    mipmapped_array.mipCount = 2;
    Check(!bridge.CheckSwapchain(mipmapped_array, refusal, reason) && refusal == XR_ERROR_FEATURE_UNSUPPORTED,
          "an array of mipmapped colour images is refused: " + reason);
    struct Case {
        VkFormat format;
        DXGI_FORMAT typeless;
    };
    const Case cases[] = {{VK_FORMAT_D32_SFLOAT, DXGI_FORMAT_R32_TYPELESS},
                          {VK_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24G8_TYPELESS},
                          {VK_FORMAT_D16_UNORM, DXGI_FORMAT_R16_TYPELESS},
                          {VK_FORMAT_D32_SFLOAT_S8_UINT, DXGI_FORMAT_R32G8X24_TYPELESS}};
    struct Shape {
        std::uint32_t layers;
        std::uint32_t mips;
    };
    const Shape shapes[] = {{2, 1}, {1, 2}};
    std::uintptr_t handle = 0x300;
    for (const Case& c : cases) {
        const bool stencil = c.format == VK_FORMAT_D24_UNORM_S8_UINT || c.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        for (const bool typeless : {false, true}) {
            for (const Shape& shape : shapes) {
                if (stencil && shape.mips > 1) {
                    Check(!bridge.CheckSwapchain(DepthInfo(32, 32, c.format, 1, shape.mips), refusal, reason) &&
                              refusal == XR_ERROR_FEATURE_UNSUPPORTED,
                          "a mipmapped depth-stencil image is refused: " + reason);
                    continue;
                }
                const DXGI_FORMAT runtime_format =
                    typeless ? c.typeless : static_cast<DXGI_FORMAT>(VulkanBridge::RuntimeFormat(c.format));
                MakeRuntimeImages(device, 2, 32, 32, runtime_format, shape.layers, RuntimeKind::kDepth, shape.mips);
                const XrSwapchain swapchain = Handle(handle++);
                Check(bridge.AddSwapchain(swapchain, DepthInfo(32, 32, c.format, shape.layers, shape.mips),
                                          FakeEnumerate, reason),
                      "depth VkFormat " + std::to_string(c.format) + " is bridged: " + reason);
                Check(!bridge.Unmirrored(swapchain) && Contains(bridge.Describe(swapchain), "mode=twin-depth"),
                      "depth is mirrored: " + bridge.Describe(swapchain));
                const VkImage image = GameImages(bridge, swapchain)[1];
                const auto value = [&](std::uint32_t layer, std::uint32_t mip, int frame) {
                    return 1 + static_cast<int>(layer * shape.mips + mip) + 2 * frame;
                };
                for (int frame = 0; frame < 2; ++frame) {
                    Acquire(bridge, swapchain, 1);
                    for (std::uint32_t layer = 0; layer < shape.layers; ++layer) {
                        for (std::uint32_t mip = 0; mip < shape.mips; ++mip) {
                            const int k = value(layer, mip, frame);
                            ClearDepth(game, image, aspect, 0.125f * static_cast<float>(k), 16 + k, layer, mip);
                        }
                    }
                    Check(Release(bridge, swapchain), "copied");
                    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
                    for (std::uint32_t layer = 0; layer < shape.layers; ++layer) {
                        for (std::uint32_t mip = 0; mip < shape.mips; ++mip) {
                            const int k = value(layer, mip, frame);
                            const float depth = 0.125f * static_cast<float>(k);
                            const auto texel = ReadRuntimeImage(device, 1, layer, RuntimeKind::kDepth, mip, 0);
                            std::uint32_t raw = 0;
                            std::memcpy(&raw, texel.data(), std::min<std::size_t>(texel.size(), 4));
                            bool matches = false;
                            if (c.format == VK_FORMAT_D16_UNORM) {
                                matches = texel.size() == 2 &&
                                          Near(static_cast<int>(raw & 0xffff), std::lround(depth * 65535));
                            } else if (c.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                                matches = Near(static_cast<int>(raw & 0xffffff), std::lround(depth * 16777215.0));
                            } else {
                                float stored = 0.0f;
                                std::memcpy(&stored, &raw, 4);
                                matches = stored == depth;
                            }
                            const std::string where = "depth VkFormat " + std::to_string(c.format) +
                                                      (typeless ? " (typeless runtime)" : "") + ", frame " +
                                                      std::to_string(frame) + ", layer " + std::to_string(layer) +
                                                      ", mip " + std::to_string(mip);
                            Check(matches, where + ": " + Bytes(texel) + " for " + std::to_string(depth));
                            if (stencil) {
                                const auto stencil_texel =
                                    ReadRuntimeImage(device, 1, layer, RuntimeKind::kDepth, mip, 1);
                                Check(!stencil_texel.empty() && stencil_texel[0] == 16 + k,
                                      where + ": stencil " + Bytes(stencil_texel) + " for " + std::to_string(16 + k));
                            }
                        }
                    }
                }
                GameIdle(game);
                Check(bridge.RemoveSwapchain(swapchain), "removed");
            }
        }
    }
    CheckClean(&bridge, "depth values");
    bridge.End();
}

// The images get their attachment, transfer and sampled usage whatever the
// application asked (OpenXR hands them out in the attachment layout).


void TestUsage(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    struct Case {
        const char* name;
        XrSwapchainCreateInfo info;
    };
    XrSwapchainCreateInfo none = ColorInfo(16, 16);
    none.usageFlags = 0;
    XrSwapchainCreateInfo sampled = ColorInfo(16, 16);
    sampled.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchainCreateInfo transfer = ColorInfo(16, 16);
    transfer.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    XrSwapchainCreateInfo msaa = ColorInfo(16, 16, 4);
    msaa.usageFlags = 0;
    XrSwapchainCreateInfo storage = ColorInfo(16, 16, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    storage.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT;
    XrSwapchainCreateInfo depth = DepthInfo(16, 16);
    depth.usageFlags = 0;
    XrSwapchainCreateInfo depth_sampled = DepthInfo(16, 16);
    depth_sampled.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    const Case cases[] = {{"colour usage 0", none},          {"colour sampled only", sampled},
                          {"colour transfer only", transfer}, {"multisampled colour usage 0", msaa},
                          {"UNORM storage", storage},         {"depth usage 0", depth},
                          {"depth sampled only", depth_sampled}};
    std::uintptr_t handle = 0x400;
    for (const Case& c : cases) {
        const bool is_depth = c.info.format == VK_FORMAT_D32_SFLOAT;
        MakeRuntimeImages(device, 1, 16, 16, static_cast<DXGI_FORMAT>(VulkanBridge::RuntimeFormat(c.info.format)), 1,
                          is_depth ? RuntimeKind::kDepth : RuntimeKind::kRenderTarget);
        const XrSwapchain swapchain = Handle(handle++);
        XrResult refusal = XR_SUCCESS;
        std::string reason;
        Check(bridge.CheckSwapchain(c.info, refusal, reason), std::string(c.name) + " is accepted: " + reason);
        Check(bridge.AddSwapchain(swapchain, c.info, FakeEnumerate, reason),
              std::string(c.name) + " is bridged: " + reason);
        const VkImage image = GameImages(bridge, swapchain)[0];
        for (int frame = 0; frame < 2; ++frame) {
            Acquire(bridge, swapchain, 0);
            if (is_depth) {
                ClearDepth(game, image, VK_IMAGE_ASPECT_DEPTH_BIT, 0.5f, 0);
            } else {
                Clear(game, image, FrameColor(frame));
            }
            Check(Release(bridge, swapchain), "copied");
            Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
            const auto texel =
                ReadRuntimeImage(device, 0, 0, is_depth ? RuntimeKind::kDepth : RuntimeKind::kRenderTarget);
            if (is_depth) {
                float value = 0.0f;
                std::memcpy(&value, texel.data(), 4);
                Check(value == 0.5f, std::string(c.name) + ": depth reaches the runtime");
            } else {
                Check(NearRgba8(texel, FrameColor(frame), c.info.format == VK_FORMAT_R8G8B8A8_SRGB),
                      std::string(c.name) + ": the frame reaches the runtime: " + Bytes(texel));
            }
        }
        GameIdle(game);
        CheckClean(&bridge, c.name);
        Check(bridge.RemoveSwapchain(swapchain), "removed");
    }
    bridge.End();
}

// A runtime image that does not match is refused, with the reason; depth the
// runtime made no depth target of stays private.
void TestMismatch(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 30, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    std::string reason;
    Check(!bridge.AddSwapchain(Handle(0x60), ColorInfo(32, 32), FakeEnumerate, reason) &&
              Contains(reason, "does not match"),
          "a mismatched runtime image is refused: " + reason);
    Check(!bridge.Bridged(Handle(0x60)), "not bridged after a refusal");
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    Check(!bridge.AddSwapchain(Handle(0x62), ColorInfo(32, 32), FakeEnumerate, reason) &&
              Contains(reason, "does not match"),
          "a runtime image of another format family is refused: " + reason);
    // A colour image the runtime did not make a render target: the copy's
    // transitions would be invalid.
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, RuntimeKind::kPlain);
    Check(!bridge.AddSwapchain(Handle(0x61), ColorInfo(32, 32), FakeEnumerate, reason) &&
              Contains(reason, "not a render target"),
          "a runtime image that is no render target is refused: " + reason);
    // Depth images that are no depth targets: the game's depth stays its own.
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R32_TYPELESS, 1, RuntimeKind::kPlain);
    const XrSwapchain depth = Handle(0x63);
    Check(bridge.AddSwapchain(depth, DepthInfo(32, 32), FakeEnumerate, reason), "depth is accepted: " + reason);
    Check(bridge.Unmirrored(depth) && bridge.HasUnmirrored() &&
              Contains(bridge.Describe(depth), "private because the runtime's depth images are not depth targets"),
          "and stays private: " + bridge.Describe(depth));
    Check(g_log.size() == 1 && Contains(g_log[0], "stays private"), "which is reported");
    g_log.clear();
    const VkImage image = GameImages(bridge, depth)[0];
    for (int frame = 0; frame < 2; ++frame) {
        Acquire(bridge, depth, 0);
        ClearDepth(game, image, VK_IMAGE_ASPECT_DEPTH_BIT, 0.25f, 0);
        Check(Release(bridge, depth), "a private release is no skipped copy");
    }
    Check(SummaryValue(bridge.Summary(), "copies") == 0, "no copies of private depth");
    GameIdle(game);
    Check(bridge.RemoveSwapchain(depth), "removed at once");
    Check(!bridge.HasUnmirrored(), "no private depth once removed");
    CheckClean(&bridge, "mismatch");
    bridge.End();
}

// Multisampled depth stays private: no copies, and the caller learns to leave it out.
void TestPrivateDepth(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 32, 32, DXGI_FORMAT_D32_FLOAT, 1, RuntimeKind::kDepth); // never touched
    const XrSwapchain swapchain = Handle(0x50);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, DepthInfo(32, 32, VK_FORMAT_D32_SFLOAT, 1, 1, 4), FakeEnumerate, reason),
          "multisampled depth is accepted: " + reason);
    Check(bridge.Bridged(swapchain) && bridge.Unmirrored(swapchain) && bridge.HasUnmirrored(),
          "multisampled depth is not mirrored");
    const VkImage image = GameImages(bridge, swapchain)[0];
    for (int frame = 0; frame < 2; ++frame) {
        Acquire(bridge, swapchain, 0);
        ClearDepth(game, image, VK_IMAGE_ASPECT_DEPTH_BIT, 0.75f, 0);
        Release(bridge, swapchain);
    }
    Check(SummaryValue(bridge.Summary(), "copies") == 0, "no copies of depth: " + bridge.Summary());
    GameIdle(game);
    Check(bridge.RemoveSwapchain(swapchain), "depth removes at once");
    CheckClean(&bridge, "private depth");
    bridge.End();
}

// Releases without acquires are counted, not copied.
void TestUnmatched(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x80);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    Check(Release(bridge, swapchain), "an unmatched release");
    const std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "unmatchedReleases") == 1 && SummaryValue(summary, "copies") == 0 &&
              SummaryValue(summary, "submits") == 0,
          "a release with nothing acquired is counted, not submitted: " + summary);
    CheckClean(&bridge, "unmatched");
    bridge.End();
}

// A stalled bridge queue: a destroyed swapchain whose copies did not finish
// is kept (the runtime's must be too), and End gives up after its wait, the
// game's queue running on (its wait for the stalled copy ends); once the
// queue resumes, its copies land in the runtime's images (still referenced)
// without removing the device.
void TestStalledQueue(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ComPtr<ID3D12Device> device = BridgeDevice(bridge);
    MakeRuntimeImages(device.Get(), 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x90);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    const std::array<float, 4> color{0.05f, 0.85f, 0.85f, 1.0f};
    Gate gate(device.Get(), game);
    gate.HoldQueue(BridgeQueue(bridge));
    Acquire(bridge, swapchain, 0);
    Clear(game, GameImages(bridge, swapchain)[0], color);
    Release(bridge, swapchain);
    Acquire(bridge, swapchain, 0); // the game's queue waits for the stalled copy
    auto start = std::chrono::steady_clock::now();
    Check(!bridge.RemoveSwapchain(swapchain), "copies into the destroyed swapchain did not finish");
    Check(SecondsSince(start) < 3.0, "RemoveSwapchain gives up after its wait");
    Check(Contains(bridge.Summary(), "(0 live, 1 kept)") && g_log.size() == 1, "the swapchain is kept, and reported");
    start = std::chrono::steady_clock::now();
    bridge.End();
    Check(SecondsSince(start) < 3.0, "End gives up after its wait: " + std::to_string(SecondsSince(start)) + " s");
    Check(g_log.size() == 2 && Contains(g_log.back(), "did not finish at teardown"), "End reports the stalled queue");
    Check(GameRuns(game), "the game's queue runs on (its wait for the stalled copy ended)");
    gate.Open();
    Sleep(200);
    Check(device->GetDeviceRemovedReason() == S_OK, "the queue's late copies remove nothing");
    Check(NearRgba8(ReadRuntimeImage(device.Get(), 0), color, true), "the late copy landed in the runtime's image");
    D3D12Errors(device.Get()); // the stall's own messages
    g_log.clear();
    CheckClean(nullptr, "stalled queue");
}

// The bridge's queue stalled while one image is released again and again:
// once every recording of its copy is queued, a release waits a while, then
// goes uncopied and says so (Frame Generation must not capture it).
void TestSkippedCopy(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 1, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xa8);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    {
        Gate gate(device, game);
        gate.HoldQueue(BridgeQueue(bridge));
        for (int release = 0; release < 4; ++release) {
            Acquire(bridge, swapchain, 0);
            Check(Release(bridge, swapchain),
                  "release " + std::to_string(release) + " is copied (a recording is free)");
        }
        Acquire(bridge, swapchain, 0);
        const auto start = std::chrono::steady_clock::now();
        Check(!Release(bridge, swapchain), "with every recording queued, the release is not copied");
        const double seconds = SecondsSince(start);
        Check(seconds > 0.05 && seconds < 1.0, "after a bounded wait: " + std::to_string(seconds) + " s");
        const std::string summary = bridge.Summary();
        Check(SummaryValue(summary, "skippedCopies") == 1 && SummaryValue(summary, "copyRecordings") == 4,
              "one skipped copy, four recordings: " + summary);
        Check(g_log.size() == 1 && Contains(g_log[0], "stalled"), "the stall is reported");
    }
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const std::array<float, 4> color{0.3f, 0.6f, 0.9f, 1.0f};
    Acquire(bridge, swapchain, 0);
    Clear(game, GameImages(bridge, swapchain)[0], color);
    Check(Release(bridge, swapchain), "copies resume once the queue moves");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(NearRgba8(ReadRuntimeImage(device, 0), color, true), "and the next frame is copied");
    GameIdle(game);
    g_log.clear();
    CheckClean(&bridge, "skipped copy");
    bridge.End();
}

// The game's queue held back over many frames of a stereo colour+depth set:
// no acquire or release waits on the CPU (no fence is waited for: more are
// made, up to the cap, then submissions go unfenced), and once the queue
// moves every copy lands and everything is let go.
void TestRingStall(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    struct Chain {
        XrSwapchain swapchain;
        bool depth;
        std::vector<ComPtr<ID3D12Resource>> runtime;
        std::vector<VkImage> images;
    };
    std::vector<Chain> chains;
    for (std::uintptr_t i = 0; i < 4; ++i) {
        Chain chain{Handle(0xc0 + i), i >= 2, {}, {}};
        chain.runtime =
            MakeImages(device, 3, 32, 32, chain.depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1,
                       chain.depth ? RuntimeKind::kDepth : RuntimeKind::kRenderTarget);
        g_runtime_images = chain.runtime;
        std::string reason;
        Check(bridge.AddSwapchain(chain.swapchain, chain.depth ? DepthInfo(32, 32) : ColorInfo(32, 32), FakeEnumerate,
                                  reason),
              "bridged: " + reason);
        chain.images = GameImages(bridge, chain.swapchain);
        chains.push_back(chain);
    }
    double slowest_acquire = 0.0;
    double slowest_release = 0.0;
    constexpr int kFrames = 12; // 48 acquire/release pairs, 96 submissions
    {
        Gate gate(device, game);
        gate.HoldGame();
        for (int frame = 0; frame < kFrames; ++frame) {
            for (Chain& chain : chains) {
                const std::uint32_t index = frame % 3;
                auto start = std::chrono::steady_clock::now();
                Acquire(bridge, chain.swapchain, index);
                slowest_acquire = std::max(slowest_acquire, SecondsSince(start));
                start = std::chrono::steady_clock::now();
                Check(Release(bridge, chain.swapchain), "copied (a recording is free)");
                slowest_release = std::max(slowest_release, SecondsSince(start));
            }
        }
        const std::string summary = bridge.Summary();
        std::cout << "ring stall: slowest acquire " << slowest_acquire * 1000.0 << " ms, slowest release "
                  << slowest_release * 1000.0 << " ms, " << summary << '\n';
        Check(slowest_acquire < 0.005 && slowest_release < 0.005,
              "no acquire or release waits while the game's queue is held");
        Check(SummaryValue(summary, "liveFences") == 64 && SummaryValue(summary, "fenceGrowth") == 48 &&
                  SummaryValue(summary, "unfencedSubmits") == 2 * kFrames * 4 - 64,
              "fences are made up to the cap, then submissions go unfenced: " + summary);
        Check(g_log.size() == 3, "the growth and the unfenced submissions (at 1 and 10) are reported");
        g_log.clear();
        // A swapchain destroyed while the queue is held is kept, not destroyed.
        const auto start = std::chrono::steady_clock::now();
        Check(!bridge.RemoveSwapchain(chains[0].swapchain), "a swapchain destroyed during the stall is kept");
        Check(SecondsSince(start) < 3.0, "after a bounded wait");
        Check(g_log.size() == 1, "and reported");
        g_log.clear();
        chains.erase(chains.begin());
    }
    GameIdle(game);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    // Frames after the stall are copied as usual.
    for (int frame = 0; frame < 3; ++frame) {
        Chain& chain = chains[0];
        g_runtime_images = chain.runtime;
        Acquire(bridge, chain.swapchain, frame);
        Clear(game, chain.images[frame], FrameColor(frame));
        Check(Release(bridge, chain.swapchain), "copied after the stall");
    }
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    for (int frame = 0; frame < 3; ++frame) {
        Check(NearRgba8(ReadRuntimeImage(device, frame), FrameColor(frame), true), "the frames after the stall");
    }
    GameIdle(game);
    bridge.FrameEnded(); // the fences signalled since are reused
    for (Chain& chain : chains) {
        Check(bridge.RemoveSwapchain(chain.swapchain), "removed once the queue moved");
    }
    CheckClean(&bridge, "ring stall");
    bridge.End();
    CheckClean(nullptr, "ring stall teardown");
}

// The game's queue never gets to its release: Drain gets the bridge's queue
// through anyway, before the runtime's images go; Escape does the same before
// Frame Generation stops.
void TestDrainPastGame(Game& game, bool timeline) {
    const std::string test = std::string("drain past the game (") + (timeline ? "timeline)" : "binary)");
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xa0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    {
        Gate gate(device, game);
        gate.HoldGame();
        Acquire(bridge, swapchain, 1);
        Clear(game, GameImages(bridge, swapchain)[1], {0.1f, 0.2f, 0.3f, 1.0f});
        Release(bridge, swapchain);
        auto start = std::chrono::steady_clock::now();
        Check(!bridge.Escape(), "Escape lets the bridge's queue through");
        double seconds = SecondsSince(start);
        Check(seconds > 1.5 && seconds < 5.0, test + ": Escape after its wait: " + std::to_string(seconds) + " s");
        Check(bridge.WaitIdle(1000), test + ": the bridge's queue is past the game's release");
        Check(g_log.size() == 1, "Escape reports it");
        g_log.clear();
        Acquire(bridge, swapchain, 0);
        Clear(game, GameImages(bridge, swapchain)[0], {0.3f, 0.2f, 0.1f, 1.0f});
        Release(bridge, swapchain);
        start = std::chrono::steady_clock::now();
        Check(bridge.Drain(), "Drain gets the bridge's queue through");
        seconds = SecondsSince(start);
        Check(seconds > 1.5 && seconds < 5.0, test + ": Drain after its first wait: " + std::to_string(seconds) + " s");
        Check(g_log.empty(), "nothing to report when the queue finished");
    }
    Check(GameRuns(game), "the game's queue runs on");
    D3D12Errors(device);
    CheckClean(nullptr, test);
    bridge.End();
    CheckClean(nullptr, test + " teardown");
}

// The bridge's submissions refused by the game's queue: a refused release
// hands nothing over (no copy, "written" unchanged, false for Frame
// Generation) and the next frame starts over; a refused acquire makes its
// release submit nothing; a lost device makes the session lost and lets the
// bridge's queue through at once.
void TestRefusedSubmissions(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xd0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    const std::vector<VkImage> images = GameImages(bridge, swapchain);
    const auto frame = [&](std::uint32_t index, int color) {
        Acquire(bridge, swapchain, index);
        Clear(game, images[index], FrameColor(color));
        return Release(bridge, swapchain);
    };
    Check(frame(0, 0), "a frame");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");

    // A refused release.
    Acquire(bridge, swapchain, 0);
    Clear(game, images[0], FrameColor(1));
    const std::uint64_t written = SummaryValue(bridge.Summary(), "writtenValue");
    g_recorder.fail_submits = 1;
    g_recorder.fail_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Check(!Release(bridge, swapchain), "a refused release is not copied");
    Check(SummaryValue(bridge.Summary(), "writtenValue") == written &&
              SummaryValue(bridge.Summary(), "failedSubmits") == 1,
          "nothing was handed over: " + bridge.Summary());
    Check(g_log.size() == 1 && Contains(g_log[0], "refused"), "the refusal is reported");
    g_log.clear();
    Check(frame(0, 2), "the next frame starts over and is copied");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(NearRgba8(ReadRuntimeImage(device, 0), FrameColor(2), true), "the runtime's image holds the next frame");

    // A refused acquire: the image stays Direct3D 12's, its release submits nothing.
    g_recorder.fail_submits = 1;
    Acquire(bridge, swapchain, 0);
    const std::uint64_t submits = SummaryValue(bridge.Summary(), "submits");
    Check(!Release(bridge, swapchain), "the release of a refused acquire is not copied");
    Check(SummaryValue(bridge.Summary(), "submits") == submits, "and submits nothing");
    Check(frame(0, 3), "the next frame is copied");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(NearRgba8(ReadRuntimeImage(device, 0), FrameColor(3), true), "the runtime's image holds it");
    GameIdle(game);
    CheckClean(&bridge, "refused submissions");

    // The game's device lost while a release is queued behind the game's work:
    // "written" is signalled from the CPU, so the bridge's queue gets through.
    {
        Gate gate(device, game);
        gate.HoldGame();
        Acquire(bridge, swapchain, 1);
        Check(Release(bridge, swapchain), "a release behind the held game");
        Check(!bridge.WaitIdle(200), "the bridge's queue waits for the game");
        g_recorder.fail_submits = 1;
        g_recorder.fail_result = VK_ERROR_DEVICE_LOST;
        Acquire(bridge, swapchain, 0);
        std::string lost;
        Check(bridge.Lost(lost) && Contains(lost, "Vulkan device was lost"), "the session is lost: " + lost);
        Check(bridge.WaitIdle(1000), "the bridge's queue got through at once");
        Check(!Release(bridge, swapchain), "nothing more is copied");
        Check(g_log.size() == 1 && Contains(g_log[0], "lost"), "the loss is reported");
        g_log.clear();
    }
    GameIdle(game);
    const auto start = std::chrono::steady_clock::now();
    Check(bridge.Drain(), "a lost bridge drains");
    bridge.End();
    Check(SecondsSince(start) < 2.0, "teardown of a lost bridge does not wait");
    g_recorder.fail_submits = 0;
    CheckClean(nullptr, "lost device");
}

// The hand-overs, as recorded: barriers (families, layouts, stages, accesses,
// aspects, ranges) and where they are submitted, for each kind of image.
void TestQfotRecorded(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    const std::uint32_t family = game.family;
    struct Chain {
        const char* name;
        XrSwapchain swapchain;
        XrSwapchainCreateInfo info;
        DXGI_FORMAT runtime;
        RuntimeKind kind;
    };
    Chain chains[] = {
        {"colour array", Handle(0xe0), ColorInfo(32, 32, 1, 2), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
         RuntimeKind::kRenderTarget},
        {"depth-stencil array", Handle(0xe1), DepthInfo(32, 32, VK_FORMAT_D24_UNORM_S8_UINT, 2),
         DXGI_FORMAT_D24_UNORM_S8_UINT, RuntimeKind::kDepth},
        {"depth with mips", Handle(0xe2), DepthInfo(32, 32, VK_FORMAT_D16_UNORM, 1, 2), DXGI_FORMAT_D16_UNORM,
         RuntimeKind::kDepth},
        {"multisampled colour", Handle(0xe3), ColorInfo(32, 32, 4), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
         RuntimeKind::kRenderTarget},
        {"multisampled depth", Handle(0xe4), DepthInfo(32, 32, VK_FORMAT_D32_SFLOAT, 1, 1, 4), DXGI_FORMAT_D32_FLOAT,
         RuntimeKind::kDepth},
    };
    const auto fail = [](const Chain& chain, const std::string& what) {
        Check(false, std::string("qfot-recorded, ") + chain.name + ": " + what);
    };
    for (const Chain& chain : chains) {
        const bool depth = chain.kind == RuntimeKind::kDepth;
        const bool resolve = !depth && chain.info.sampleCount > 1;
        const bool private_depth = depth && chain.info.sampleCount > 1;
        const VkImageLayout layout =
            depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkImageAspectFlags aspect =
            !depth ? VkImageAspectFlags{VK_IMAGE_ASPECT_COLOR_BIT}
            : chain.info.format == VK_FORMAT_D24_UNORM_S8_UINT || chain.info.format == VK_FORMAT_D32_SFLOAT_S8_UINT
                ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT}
                : VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT};
        const std::uint32_t layers = chain.info.arraySize;
        const std::uint32_t mips = chain.info.mipCount;
        MakeRuntimeImages(device, 2, 32, 32, chain.runtime, layers, chain.kind, mips);
        std::string reason;
        Check(bridge.AddSwapchain(chain.swapchain, chain.info, FakeEnumerate, reason), "bridged: " + reason);
        const std::vector<VkImage> images = GameImages(bridge, chain.swapchain);
        g_recorder.Take();
        const auto covers = [&](const VkImageMemoryBarrier& barrier, VkImageAspectFlags wanted, std::uint32_t levels) {
            return barrier.subresourceRange.aspectMask == wanted && barrier.subresourceRange.baseMipLevel == 0 &&
                   barrier.subresourceRange.levelCount == levels && barrier.subresourceRange.baseArrayLayer == 0 &&
                   barrier.subresourceRange.layerCount == layers;
        };
        // The one barrier of a submission's one command buffer.
        const auto single = [&](const std::vector<Submission>& submissions, Command& command,
                                VkImageMemoryBarrier& barrier, const std::string& what) {
            if (submissions.size() != 1 || submissions[0].commands.size() != 1) fail(chain, what + ": one submission");
            const std::vector<Command> commands = g_recorder.Commands(submissions[0].commands[0]);
            if (commands.size() != 1 || commands[0].resolve || commands[0].barriers.size() != 1) {
                fail(chain, what + ": one barrier");
            }
            command = commands[0];
            barrier = command.barriers[0];
        };
        for (int round = 0; round < 2; ++round) {
            for (std::uint32_t index = 0; index < 2; ++index) {
                const std::string where = "round " + std::to_string(round) + ", image " + std::to_string(index);
                const std::uint64_t waits_before = SummaryValue(bridge.Summary(), "doneWaits");
                Acquire(bridge, chain.swapchain, index);
                std::vector<Submission> acquired = g_recorder.Take();
                const bool waited = SummaryValue(bridge.Summary(), "doneWaits") > waits_before;
                Command command;
                VkImageMemoryBarrier barrier{};
                if (round == 0) {
                    // first_acquire: UNDEFINED to the attachment layout, no ownership transfer.
                    single(acquired, command, barrier, where + " first acquire");
                    if (barrier.image != images[index] || barrier.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
                        barrier.newLayout != layout || barrier.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
                        barrier.dstQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED ||
                        !covers(barrier, aspect, resolve ? 1 : mips) || barrier.dstAccessMask == 0) {
                        fail(chain, where + ": the first acquire's barrier");
                    }
                } else if (private_depth) {
                    if (!acquired.empty()) fail(chain, where + ": private depth needs nothing at a later acquire");
                } else if (resolve) {
                    // Only the done wait, if any; no barrier.
                    for (const Submission& submission : acquired) {
                        for (const VkCommandBuffer commands : submission.commands) {
                            if (!g_recorder.Commands(commands).empty()) {
                                fail(chain, where + ": a resolve acquire has no barrier");
                            }
                        }
                    }
                    if (waited != (!acquired.empty() && acquired[0].waits == 1)) {
                        fail(chain, where + ": the done wait is submitted exactly when issued");
                    }
                } else {
                    // acquire: from the external queue family (GENERAL) to the game's.
                    single(acquired, command, barrier, where + " acquire");
                    if (barrier.image != images[index] || barrier.oldLayout != VK_IMAGE_LAYOUT_GENERAL ||
                        barrier.newLayout != layout || barrier.srcQueueFamilyIndex != VK_QUEUE_FAMILY_EXTERNAL ||
                        barrier.dstQueueFamilyIndex != family || !covers(barrier, aspect, mips) ||
                        (barrier.dstAccessMask & (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)) !=
                            (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) ||
                        !(command.destination_stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) {
                        fail(chain, where + ": the acquire's barrier");
                    }
                    if (waited != (acquired[0].waits == 1) || acquired[0].signals != 0) {
                        fail(chain, where + ": the acquire waits for done exactly when issued");
                    }
                }
                Check(Release(bridge, chain.swapchain), "copied");
                const std::vector<Submission> released = g_recorder.Take();
                if (private_depth) {
                    if (!released.empty()) fail(chain, where + ": private depth submits nothing at release");
                    continue;
                }
                if (released.size() != 1 || released[0].commands.size() != 1 || released[0].signals != 1 ||
                    released[0].waits != 0 || released[0].signal_value == 0) {
                    fail(chain, where + ": the release is one submission signalling written");
                }
                const std::vector<Command> commands = g_recorder.Commands(released[0].commands[0]);
                if (resolve) {
                    // Private to TRANSFER_SRC and the twin taken over to
                    // TRANSFER_DST; the resolve; private back, the twin to
                    // Direct3D 12.
                    if (commands.size() != 3 || commands[0].resolve || !commands[1].resolve || commands[2].resolve ||
                        commands[0].barriers.size() != 2 || commands[2].barriers.size() != 2) {
                        fail(chain, where + ": barrier, resolve, barrier");
                    }
                    const VkImageMemoryBarrier& to_source = commands[0].barriers[0];
                    const VkImageMemoryBarrier& to_twin = commands[0].barriers[1];
                    const VkImageMemoryBarrier& back = commands[2].barriers[0];
                    const VkImageMemoryBarrier& handed = commands[2].barriers[1];
                    const VkImage twin = to_twin.image;
                    if (to_source.image != images[index] || to_source.oldLayout != layout ||
                        to_source.newLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        !(commands[0].source_stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) ||
                        to_twin.oldLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
                        to_twin.newLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                        to_twin.srcQueueFamilyIndex != VK_QUEUE_FAMILY_IGNORED || twin == images[index] ||
                        !covers(to_source, aspect, 1) || !covers(to_twin, aspect, 1)) {
                        fail(chain, where + ": the barriers before the resolve");
                    }
                    if (commands[1].resolve_source != images[index] || commands[1].resolve_destination != twin ||
                        commands[1].resolve_source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        commands[1].resolve_destination_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                        fail(chain, where + ": the resolve");
                    }
                    if (back.image != images[index] || back.oldLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
                        back.newLayout != layout || handed.image != twin ||
                        handed.oldLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ||
                        handed.newLayout != VK_IMAGE_LAYOUT_GENERAL || handed.srcQueueFamilyIndex != family ||
                        handed.dstQueueFamilyIndex != VK_QUEUE_FAMILY_EXTERNAL ||
                        !(handed.srcAccessMask & VK_ACCESS_TRANSFER_WRITE_BIT) || !covers(handed, aspect, 1)) {
                        fail(chain, where + ": the barriers after the resolve");
                    }
                } else {
                    if (commands.size() != 1 || commands[0].barriers.size() != 1) fail(chain, where + ": one barrier");
                    const VkImageMemoryBarrier& handed = commands[0].barriers[0];
                    if (handed.image != images[index] || handed.oldLayout != layout ||
                        handed.newLayout != VK_IMAGE_LAYOUT_GENERAL || handed.srcQueueFamilyIndex != family ||
                        handed.dstQueueFamilyIndex != VK_QUEUE_FAMILY_EXTERNAL || !covers(handed, aspect, mips) ||
                        !(handed.srcAccessMask & VK_ACCESS_MEMORY_WRITE_BIT) ||
                        !(commands[0].source_stages & VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)) {
                        fail(chain, where + ": the release's barrier");
                    }
                }
            }
        }
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        Check(bridge.RemoveSwapchain(chain.swapchain), "removed");
    }
    GameIdle(game);
    CheckClean(&bridge, "qfot-recorded");
    bridge.End();
}

// The bridge uses the game's queue only inside the application's acquire and
// release: nothing at swapchain creation, removal, Drain, Escape, End or
// destruction (a bridge destroyed without End lets nothing go: it is left to
// the process, as at process exit).
void TestQueueOnlyInAcquireRelease(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 3, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xf0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32, 4), FakeEnumerate, reason), "bridged: " + reason);
    for (int frame = 0; frame < 6; ++frame) {
        Acquire(bridge, swapchain, frame % 3);
        Release(bridge, swapchain);
        bridge.FrameEnded();
    }
    Check(!bridge.Summary().empty() && !bridge.Describe(swapchain).empty(), "reports");
    Check(bridge.RemoveSwapchain(swapchain), "removed");
    Check(bridge.Escape() && bridge.Drain(), "escape and drain");
    bridge.End();
    Check(g_recorder.queue_calls_outside == 0, "no queue call outside the acquire and release");

    // A bridge left active to the process.
    MakeRuntimeImages(device, 1, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    {
        Game leaked = game;
        leaked.device = VK_NULL_HANDLE;
        leaked.pending.clear();
        leaked.scratch = VK_NULL_HANDLE;
        leaked.scratch_memory = VK_NULL_HANDLE;
        std::string error;
        Check(MakeDevice(leaked, true, true, 0, error), "a device for a bridge left to the process: " + error);
        VulkanBridge left;
        Session left_session(leaked);
        BeginBridge(leaked, left, left_session);
        Check(left.AddSwapchain(Handle(0xf1), ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
        Acquire(left, Handle(0xf1), 0);
        Release(left, Handle(0xf1));
        Check(left.WaitIdle(10000), "the bridge's queue finishes");
        GameIdle(leaked);
        // The device and the bridge's objects on it are left to the process too.
    }
    Check(g_recorder.queue_calls_outside == 0, "the destructor uses no queue");
    CheckClean(nullptr, "queue only in acquire and release");
}

// A Vulkan 1.0 game: with the bridge's extensions it is bridged (the bridge
// resolves the KHR names of the 1.1 functions), without them refused with a
// named reason.
void TestVulkan10(const Game& like) {
    for (const bool extensions : {true, false}) {
        Game game;
        std::string error;
        Check(MakeInstance(VK_API_VERSION_1_0, extensions, game, error), "a Vulkan 1.0 instance: " + error);
        Check(FindPhysical(game, &like, error), "the GPU in the 1.0 instance: " + error);
        Check(MakeDevice(game, extensions, false, 0, error), "a Vulkan 1.0 device: " + error);
        VulkanBridge bridge;
        Session session(game);
        std::string reason;
        std::string description;
        g_runtime_luid = like.luid;
        g_log.clear();
        bridge.SetLog([](const std::string& message) { g_log.push_back(message); });
        g_recorder.properties2_khr_calls = 0;
        const XrSessionCreateInfo* bridged = bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr,
                                                          Recording(false), reason, description);
        if (!extensions) {
            Check(!bridged && Contains(reason, "lacks"),
                  "a Vulkan 1.0 game without the extensions is refused: " + reason);
            std::cout << "Vulkan 1.0 without the extensions: " << reason << '\n';
        } else {
            Check(bridged != nullptr, "a Vulkan 1.0 game with the extensions is bridged: " + reason);
            Check(g_recorder.properties2_khr_calls > 0,
                  "the bridge calls the 1.1 functions of a Vulkan 1.0 game by their KHR names");
            Check(Contains(description, "semaphores=binary") && Contains(description, "formatLists=no"),
                  "binary semaphores, no format lists: " + description);
            bridge.SessionCreated();
            ID3D12Device* device = BridgeDevice(bridge);
            MakeRuntimeImages(device, 3, 32, 32, DXGI_FORMAT_R8G8B8A8_TYPELESS);
            const XrSwapchain swapchain = Handle(0x110);
            Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
            const std::vector<VkImage> images = GameImages(bridge, swapchain);
            for (int frame = 0; frame < 6; ++frame) {
                Acquire(bridge, swapchain, frame % 3);
                Clear(game, images[frame % 3], FrameColor(frame));
                Check(Release(bridge, swapchain), "copied");
            }
            Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
            for (int frame = 3; frame < 6; ++frame) {
                Check(NearRgba8(ReadRuntimeImage(device, frame % 3), FrameColor(frame), true),
                      "Vulkan 1.0 frames reach the runtime");
            }
            GameIdle(game);
            CheckClean(&bridge, "Vulkan 1.0");
            bridge.End();
        }
        DestroyInstance(game);
        CheckClean(nullptr, std::string("Vulkan 1.0 ") + (extensions ? "with" : "without") + " the extensions");
    }
}

// The copies' cost on the bridge's queue (Direct3D 12 timestamps) and the
// submissions' on the game's thread, with incompressible content.
void TestCopyCost(Game& game) {
    for (const std::uint32_t size : {2048u, 4096u}) {
        VulkanBridge bridge;
        Session session(game);
        BeginBridge(game, bridge, session);
        ID3D12Device* device = BridgeDevice(bridge);
        MakeRuntimeImages(device, 3, size, size, DXGI_FORMAT_R8G8B8A8_UNORM);
        const XrSwapchain swapchain = Handle(0x120);
        std::string reason;
        Check(bridge.AddSwapchain(swapchain, ColorInfo(size, size, 1, 1, VK_FORMAT_R8G8B8A8_UNORM), FakeEnumerate,
                                  reason),
              "bridged: " + reason);
        const std::vector<VkImage> images = GameImages(bridge, swapchain);
        // Noise, uploaded once into every image.
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(size) * size * 4;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        Check(game.vkCreateBuffer(game.device, &buffer_info, nullptr, &staging) == VK_SUCCESS, "a staging buffer");
        VkMemoryRequirements requirements{};
        game.vkGetBufferMemoryRequirements(game.device, staging, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex =
            MemoryType(game, requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        Check(game.vkAllocateMemory(game.device, &allocate, nullptr, &memory) == VK_SUCCESS &&
                  game.vkBindBufferMemory(game.device, staging, memory, 0) == VK_SUCCESS,
              "staging memory");
        void* mapped = nullptr;
        Check(game.vkMapMemory(game.device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS, "map staging");
        std::uint32_t state = 2463534242u;
        auto* words = static_cast<std::uint32_t*>(mapped);
        for (VkDeviceSize i = 0; i < bytes / 4; ++i) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            words[i] = state | 0xff000000u;
        }
        game.vkUnmapMemory(game.device, memory);
        for (std::uint32_t index = 0; index < 3; ++index) {
            Acquire(bridge, swapchain, index);
            GameSubmit(game, [&](VkCommandBuffer commands) {
                VkImageMemoryBarrier barrier = GameBarrier(images[index], VK_IMAGE_ASPECT_COLOR_BIT,
                                                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 0, 1);
                game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {size, size, 1};
                game.vkCmdCopyBufferToImage(commands, staging, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                            &region);
                std::swap(barrier.oldLayout, barrier.newLayout);
                game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            });
            Release(bridge, swapchain);
        }
        GameIdle(game);
        game.vkDestroyBuffer(game.device, staging, nullptr);
        game.vkFreeMemory(game.device, memory, nullptr);
        for (int frame = 0; frame < 60; ++frame) {
            Acquire(bridge, swapchain, frame % 3);
            Release(bridge, swapchain);
        }
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        GameIdle(game);
        const std::string summary = bridge.Summary();
        const auto copy = SummaryPair(summary, "copyGpuUs p50/p99");
        const auto submit = SummaryPair(summary, "submitUs p50/max");
        std::cout << "copy cost " << size << "x" << size << " RGBA8 noise: copyGpuUs p50/p99=" << copy.first << "/"
                  << copy.second << " us, submitUs p50/max=" << submit.first << "/" << submit.second
                  << " us (validation " << (g_validation ? "on" : "off") << ")\n";
        Check(copy.first > 0, "the copies are timed: " + summary);
        CheckClean(&bridge, "copy cost");
        bridge.End();
    }
}

// The game's frames end on one thread and its images are released on
// another: the bridge's submissions stay inside the application's calls (the
// game synchronises its queue around them), so a second thread is only
// reported, once.
void TestSecondThread(Game& game) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x130);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    bridge.FrameEnded(); // the frame's thread
    Acquire(bridge, swapchain, 0);
    Check(g_log.empty(), "one thread: nothing to report");
    std::thread release([&] { Release(bridge, swapchain); });
    release.join();
    Check(g_log.size() == 1 && Contains(g_log[0], "more than one thread"), "a second thread is reported");
    Check(Contains(bridge.Summary(), "secondThread=yes"), "summary: " + bridge.Summary());
    std::thread again([&] {
        Acquire(bridge, swapchain, 1);
        Release(bridge, swapchain);
        bridge.FrameEnded();
    });
    again.join();
    Check(g_log.size() == 1, "reported once");
    g_log.clear();
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    GameIdle(game);
    CheckClean(&bridge, "second thread");
    bridge.End();
}

// The bridge's device removed: the session is lost, and teardown does not wait.
// Last: Direct3D 12 makes one device per adapter in a process.
void TestDeviceLost(Game& game, bool timeline) {
    VulkanBridge bridge;
    Session session(game);
    BeginBridge(game, bridge, session, timeline);
    ComPtr<ID3D12Device5> device5;
    Check(SUCCEEDED(BridgeDevice(bridge)->QueryInterface(IID_PPV_ARGS(&device5))), "ID3D12Device5");
    MakeRuntimeImages(device5.Get(), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xb0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    Acquire(bridge, swapchain, 0);
    Release(bridge, swapchain);
    std::string lost;
    Check(!bridge.Lost(lost), "not lost yet");
    device5->RemoveDevice();
    Check(bridge.Lost(lost) && Contains(lost, "removed"), "the removal is seen: " + lost);
    Acquire(bridge, swapchain, 0); // the done fence reads past every value: no wait
    Release(bridge, swapchain);
    auto start = std::chrono::steady_clock::now();
    Check(bridge.Drain(), "a removed device counts as idle");
    const double drain = SecondsSince(start);
    start = std::chrono::steady_clock::now();
    bridge.End();
    const double end = SecondsSince(start);
    Check(drain < 1.0 && end < 2.0, "teardown does not wait on a removed device: Drain " + std::to_string(drain) +
                                      " s, End " + std::to_string(end) + " s");
    Check(GameRuns(game), "the game's Vulkan device runs on");
    g_log.clear();
    g_messages.Take();
}

// The validation layer is live: a barrier from a layout the image is not in
// is reported.
bool ValidationCanary(Game& game) {
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = VK_FORMAT_R8G8B8A8_UNORM;
    create.extent = {16, 16, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image = VK_NULL_HANDLE;
    Check(game.vkCreateImage(game.device, &create, nullptr, &image) == VK_SUCCESS, "canary image");
    VkMemoryRequirements requirements{};
    game.vkGetImageMemoryRequirements(game.device, image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = MemoryType(game, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    Check(game.vkAllocateMemory(game.device, &allocate, nullptr, &memory) == VK_SUCCESS &&
              game.vkBindImageMemory(game.device, image, memory, 0) == VK_SUCCESS,
          "canary memory");
    GameSubmit(game, [&](VkCommandBuffer commands) {
        const VkImageMemoryBarrier barrier = GameBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT,
                                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                         VK_IMAGE_LAYOUT_GENERAL, 0, 1, 0, 1);
        game.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &barrier);
    });
    GameIdle(game);
    game.vkDestroyImage(game.device, image, nullptr);
    game.vkFreeMemory(game.device, memory, nullptr);
    bool error = false;
    for (const std::string& message : g_messages.Take()) error = error || Contains(message, "[vulkan error]");
    return error;
}

} // namespace

int main(int argc, char** argv) {
    // `binary`: the cases run in both semaphore modes run in binary mode only.
    const bool binary_only = argc > 1 && std::string(argv[1]) == "binary";
    TestFormats();
    TestRuntimeCreateInfo();
    TestExtensions();
    std::printf("Vulkan bridge pure tests passed\n");

    const bool require = vulkan_test::RequireHardware();
    const auto skip = [&](const std::string& why) {
        std::printf("%s: %s\n", require ? "FAIL (VECTORXR_TESTS_REQUIRE_HW=1)" : "skipped", why.c_str());
        return require ? 1 : kSkip;
    };
    // Before any Direct3D 12 device exists.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        g_d3d12_debug = true;
    }
    Check(g_d3d12_debug || !require, "the Direct3D 12 debug layer is available");
    vulkan_test::ConfigureVulkanLoader();
    g_loader = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_loader) return skip("no Vulkan loader");
    g_get_instance_proc_addr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_loader, "vkGetInstanceProcAddr"));
    Check(g_get_instance_proc_addr != nullptr, "the loader's vkGetInstanceProcAddr");
    g_validation = vulkan_test::ValidationAvailable(reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        g_get_instance_proc_addr(nullptr, "vkEnumerateInstanceLayerProperties")));
    vulkan_test::PrintValidationState(g_validation);
    Check(g_validation || !require, "the Khronos validation layer is listed");

    Game game;
    std::string error;
    Check(MakeInstance(VK_API_VERSION_1_2, true, game, error), error);
    if (!FindPhysical(game, nullptr, error)) return skip(error);
    if (!SharingWorks(game, error)) return skip(error);
    // A second queue of the family, so the binding's queue index is not 0.
    Check(MakeDevice(game, true, true, 1, error), error);
    if (g_validation) {
        const bool canary = ValidationCanary(game);
        if (!canary) std::printf("WARNING: the validation canary was not reported\n");
        Check(canary || !require, "the validation layer reports a wrong layout (canary)");
    }
    Game bare_game = game;
    bare_game.device = VK_NULL_HANDLE;
    bare_game.pending.clear();
    Check(MakeDevice(bare_game, false, false, 0, error), "a device without the extensions: " + error);

    TestBegin(game, bare_game);
    DestroyDevice(bare_game);
    CheckClean(nullptr, "device without the extensions destroyed");
    TestBeginFailureCleanup(game);
    TestSwapchainChecks(game);
    for (const bool timeline : {true, false}) {
        if (timeline && binary_only) continue;
        TestTwinFrames(game, timeline, false);
        TestTwinFrames(game, timeline, true);
        TestWrittenOrdering(game, timeline);
        TestDoneWait(game, timeline);
        TestRace(game, timeline);
    }
    TestColorFormats(game);
    TestPendingRelease(game);
    TestResolve(game);
    TestArray(game);
    TestDepthValues(game);
    TestUsage(game);
    TestMismatch(game);
    TestPrivateDepth(game);
    TestUnmatched(game);
    TestQfotRecorded(game);
    TestQueueOnlyInAcquireRelease(game);
    TestRefusedSubmissions(game);
    TestStalledQueue(game);
    TestSkippedCopy(game);
    TestRingStall(game);
    if (!binary_only) TestDrainPastGame(game, true);
    TestDrainPastGame(game, false);
    TestSecondThread(game);
    TestVulkan10(game);
    TestCopyCost(game);
    TestDeviceLost(game, false);
    g_runtime_images.clear();
    // The instance stays: a device of a bridge left to the process is on it.
    DestroyDevice(game);
    CheckClean(nullptr, "teardown");
    std::printf("Vulkan bridge GPU tests passed\n");
    std::printf("validation=%s (%s), D3D12 debug layer=%s\n", g_validation ? "on" : "off",
                vulkan_test::kValidationLayerDir[0] ? vulkan_test::kValidationLayerDir : "loader search path",
                g_d3d12_debug ? "on" : "off");
    return 0;
}
