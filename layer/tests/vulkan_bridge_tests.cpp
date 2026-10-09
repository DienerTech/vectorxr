// Frame Generation's Vulkan bridge: the pure parts (formats, the runtime's
// create info, extension lists) everywhere; the shared images and fences on a
// GPU whose Vulkan device and Direct3D 12 adapter are the same (77 without
// one: there is no software Vulkan device that shares memory with Direct3D 12).

#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_vulkan_bridge.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using depthxr::VulkanBridge;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

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

} // namespace

int main() {
    TestFormats();
    TestRuntimeCreateInfo();
    TestExtensions();
    std::printf("Vulkan bridge pure tests passed\n");
    return 0;
}
