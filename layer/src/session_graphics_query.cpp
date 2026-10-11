#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/session_graphics.h"
#include "depthxr/vulkan_enable_hooks.h"

#include <cstring>
#include <utility>

// QuerySessionGraphics (session_graphics.h): the session's adapters, found
// through the application's own device objects and DXGI. Its formatting is in
// session_graphics.cpp.

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

std::string Utf8(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), bytes, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(bytes - 1));
    return result;
}

// What DXGI tells of an adapter: its name, PCI vendor, user-mode driver
// version, dedicated video memory, and the process's budget on it.
void DescribeAdapter(IDXGIAdapter* adapter, SessionGraphics& graphics) {
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
        graphics.adapter = Utf8(desc.Description);
        graphics.vendor_id = desc.VendorId;
        graphics.dedicated_video_memory = desc.DedicatedVideoMemory;
    }
    // The interface only names what DXGI is asked about: the answer is the
    // version of the driver's user-mode part.
    LARGE_INTEGER version{};
    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
        graphics.driver_version = static_cast<std::uint64_t>(version.QuadPart);
    }
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
        graphics.budget = memory.Budget;
        graphics.usage = memory.CurrentUsage;
    }
}

// DXGI's adapter with a LUID (Direct3D 12 and Vulkan devices name theirs so),
// from a factory made at the first lookup.
class AdapterByLuid {
  public:
    ComPtr<IDXGIAdapter> Find(const LUID& luid) {
        if (!tried_) {
            tried_ = true;
            CreateDXGIFactory1(IID_PPV_ARGS(&factory_));
        }
        ComPtr<IDXGIAdapter> adapter;
        if (factory_ && FAILED(factory_->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
            adapter.Reset();
        }
        return adapter;
    }

  private:
    ComPtr<IDXGIFactory4> factory_;
    bool tried_{false};
};

SessionGraphics D3D11Graphics(ID3D11Device* device) {
    SessionGraphics graphics;
    graphics.api = "Direct3D 11";
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    if (!device) {
        graphics.problem = "the binding names no device";
    } else if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) ||
               FAILED(dxgi_device->GetAdapter(&adapter))) {
        graphics.problem = "the device names no DXGI adapter";
    } else {
        DescribeAdapter(adapter.Get(), graphics);
    }
    return graphics;
}

SessionGraphics D3D12Graphics(ID3D12Device* device, std::string api, AdapterByLuid& adapters) {
    SessionGraphics graphics;
    graphics.api = std::move(api);
    if (!device) {
        graphics.problem = "the binding names no device";
    } else if (const ComPtr<IDXGIAdapter> adapter = adapters.Find(device->GetAdapterLuid())) {
        DescribeAdapter(adapter.Get(), graphics);
    } else {
        graphics.problem = "DXGI has no adapter with the device's LUID";
    }
    return graphics;
}

SessionGraphics VulkanGraphics(const XrGraphicsBindingVulkanKHR& binding, AdapterByLuid& adapters) {
    SessionGraphics graphics;
    graphics.api = "Vulkan";
    if (!binding.instance || !binding.physicalDevice) {
        graphics.problem = "the binding names no instance or physical device";
        return graphics;
    }
    // The game's vkGetInstanceProcAddr when the layer's hooks saw its device
    // made (XR_KHR_vulkan_enable2), else that of the loader the game loaded.
    auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        VulkanGameFor(reinterpret_cast<std::uint64_t>(binding.device)).get_instance_proc_addr);
    if (!get_instance_proc_addr) {
        if (const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll")) {
            get_instance_proc_addr =
                reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        }
    }
    if (!get_instance_proc_addr) {
        graphics.problem = "no vkGetInstanceProcAddr (the Vulkan loader is not loaded)";
        return graphics;
    }
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        get_instance_proc_addr(binding.instance, "vkGetPhysicalDeviceProperties"));
    if (!get_properties) {
        graphics.problem = "the Vulkan instance has no vkGetPhysicalDeviceProperties";
        return graphics;
    }
    VkPhysicalDeviceProperties properties{};
    get_properties(binding.physicalDevice, &properties);
    graphics.adapter = properties.deviceName;
    graphics.vendor_id = properties.vendorID;
    graphics.vulkan_driver_version = properties.driverVersion;

    // The adapter's LUID, for DXGI's figures: VK_KHR_get_physical_device_properties2
    // when the instance has it, else Vulkan 1.1's (the device's version is
    // checked; the instance's is not known here).
    auto get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        get_instance_proc_addr(binding.instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (!get_properties2 && properties.apiVersion >= VK_API_VERSION_1_1) {
        get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            get_instance_proc_addr(binding.instance, "vkGetPhysicalDeviceProperties2"));
    }
    if (!get_properties2) {
        graphics.problem = "the Vulkan instance has no vkGetPhysicalDeviceProperties2 to name the adapter's LUID";
        return graphics;
    }
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &id;
    get_properties2(binding.physicalDevice, &properties2);
    if (!id.deviceLUIDValid) {
        graphics.problem = "the Vulkan device names no adapter LUID";
        return graphics;
    }
    LUID luid{};
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    std::memcpy(&luid, id.deviceLUID, VK_LUID_SIZE);
    if (const ComPtr<IDXGIAdapter> adapter = adapters.Find(luid)) {
        DescribeAdapter(adapter.Get(), graphics);
    } else {
        graphics.problem = "DXGI has no adapter with the Vulkan device's LUID";
    }
    return graphics;
}

} // namespace

std::vector<SessionGraphics> QuerySessionGraphics(const void* create_info_next, const void* bridge_binding,
                                                  std::string_view bridge_api) {
    std::vector<SessionGraphics> result;
    AdapterByLuid adapters;
    const auto* chain = static_cast<const XrBaseInStructure*>(create_info_next);
    const XrBaseInStructure* binding = chain;
    while (binding && binding->type != XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
           binding->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
           binding->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        binding = binding->next;
    }
    if (!binding) {
        bool opengl = false;
        for (const XrBaseInStructure* next = chain; next; next = next->next) {
            opengl = opengl || next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
        }
        SessionGraphics graphics;
        graphics.api = opengl ? "OpenGL" : "none";
        graphics.problem = opengl ? "an OpenGL session's adapter is not looked up"
                                  : "the session has no Direct3D 11, Direct3D 12 or Vulkan binding";
        result.push_back(std::move(graphics));
    } else if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
        result.push_back(D3D11Graphics(reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(binding)->device));
    } else if (binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        ID3D12Device* device = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding)->device;
        result.push_back(D3D12Graphics(device, "Direct3D 12", adapters));
    } else {
        result.push_back(VulkanGraphics(*reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(binding), adapters));
    }
    if (bridge_binding) {
        result.push_back(D3D12Graphics(static_cast<const XrGraphicsBindingD3D12KHR*>(bridge_binding)->device,
                                       std::string(bridge_api), adapters));
    }
    return result;
}

} // namespace depthxr
