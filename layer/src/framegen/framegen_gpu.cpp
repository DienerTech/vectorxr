#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

namespace depthxr {

std::unique_ptr<FrameGenGpu> CreateFrameGenD3D11(const void* binding, std::string& error);
std::unique_ptr<FrameGenGpu> CreateFrameGenD3D12(const void* binding, std::string& error);
std::unique_ptr<FrameGenGpu> CreateFrameGenVulkan(const void* binding, std::string& error);

std::unique_ptr<FrameGenGpu> CreateFrameGenGpu(const void* binding_chain, std::string& error) {
    for (auto* base = static_cast<const XrBaseInStructure*>(binding_chain); base; base = base->next) {
        switch (base->type) {
        case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR:
            return CreateFrameGenD3D11(base, error);
        case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
            return CreateFrameGenD3D12(base, error);
        case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
            return CreateFrameGenVulkan(base, error);
        default:
            break;
        }
    }
    error = "Frame Generation requires Direct3D 11, Direct3D 12, or Vulkan";
    return nullptr;
}

} // namespace depthxr
