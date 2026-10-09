#include <windows.h>
#include <dxgiformat.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr.h>

#include "depthxr/framegen_vulkan_bridge.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

// The Vulkan bridge's pure parts: formats, the runtime's create info and the
// extensions it needs. Its GPU parts are in framegen_vulkan_bridge.cpp.

namespace depthxr {
namespace {

// VkFormat <-> DXGI_FORMAT for swapchains. The runtime's session is Direct3D 12,
// so its formats are DXGI; a Vulkan game asks for and gets VkFormats.
constexpr std::array<std::pair<VkFormat, DXGI_FORMAT>, 12> kFormats{{
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
}};

// Structures only a Vulkan session's swapchains understand.
bool VulkanOnly(XrStructureType type) {
    return type == XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR ||
           type == XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META;
}

bool IsDepthVulkanFormat(std::int64_t format) {
    switch (static_cast<VkFormat>(format)) {
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D16_UNORM_S8_UINT:
        return true;
    default:
        return false;
    }
}

} // namespace

const std::vector<const char*>& VulkanBridge::InstanceExtensions() {
    // Core in Vulkan 1.1; named so a Vulkan 1.0 game's instance has them too.
    static const std::vector<const char*> extensions{
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    };
    return extensions;
}

const std::vector<const char*>& VulkanBridge::DeviceExtensions() {
    // The Win32 ones import the bridge's Direct3D 12 images and fences; the
    // others are core in Vulkan 1.1 and named for a Vulkan 1.0 device.
    static const std::vector<const char*> extensions{
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        "VK_KHR_external_memory_win32",
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        "VK_KHR_external_semaphore_win32",
    };
    return extensions;
}

std::string VulkanBridge::WithExtensions(const std::string& list, const std::vector<const char*>& extra) {
    std::vector<std::string> names;
    std::istringstream words(list);
    for (std::string word; words >> word;) {
        names.push_back(word);
    }
    std::string result;
    for (const std::string& name : names) {
        if (!result.empty()) result += ' ';
        result += name;
    }
    for (const char* name : extra) {
        if (!name || std::find(names.begin(), names.end(), name) != names.end()) continue;
        names.emplace_back(name);
        if (!result.empty()) result += ' ';
        result += name;
    }
    return result;
}

std::int64_t VulkanBridge::RuntimeFormat(std::int64_t vulkan_format) {
    for (const auto& [vulkan, dxgi] : kFormats) {
        if (vulkan == static_cast<VkFormat>(vulkan_format)) return static_cast<std::int64_t>(dxgi);
    }
    return 0;
}

std::int64_t VulkanBridge::GameFormat(std::int64_t dxgi_format) {
    for (const auto& [vulkan, dxgi] : kFormats) {
        if (dxgi == static_cast<DXGI_FORMAT>(dxgi_format)) return static_cast<std::int64_t>(vulkan);
    }
    return 0;
}

std::vector<std::int64_t> VulkanBridge::TranslatedFormats(const std::vector<std::int64_t>& runtime_formats) {
    std::vector<std::int64_t> formats;
    for (const std::int64_t runtime : runtime_formats) {
        const std::int64_t format = GameFormat(runtime);
        if (format != 0 && std::find(formats.begin(), formats.end(), format) == formats.end()) {
            formats.push_back(format);
        }
    }
    return formats;
}

bool VulkanBridge::RuntimeCreateInfo(const XrSwapchainCreateInfo& info, XrSwapchainCreateInfo& runtime,
                                     std::string& reason) {
    reason.clear();
    runtime = info;
    const std::int64_t format = RuntimeFormat(info.format);
    if (format == 0) {
        reason = "VkFormat " + std::to_string(info.format) + " has no Direct3D 12 equivalent";
        return false;
    }
    runtime.format = format;
    const bool depth =
        IsDepthVulkanFormat(info.format) || (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    runtime.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                          (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    runtime.sampleCount = 1;
    const auto* next = static_cast<const XrBaseInStructure*>(info.next);
    while (next && VulkanOnly(next->type)) {
        next = next->next;
    }
    runtime.next = next;
    return true;
}

} // namespace depthxr
