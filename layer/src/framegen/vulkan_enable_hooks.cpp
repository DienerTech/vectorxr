#include "depthxr/vulkan_enable_hooks.h"

// Interface stub: every name resolves to the runtime until the hooks land.

namespace depthxr {

void ResetVulkanEnableHooks(XrInstance, PFN_xrGetInstanceProcAddr, bool, std::function<void(const std::string&)>) {}

PFN_xrVoidFunction VulkanEnableHook(XrInstance, const char*) {
    return nullptr;
}

VulkanBridgeGame VulkanGameFor(std::uint64_t) {
    return {};
}

void RequeryVulkanGraphicsRequirements(PFN_xrGetInstanceProcAddr, XrInstance, XrSystemId, int) {}

std::string VulkanEnableHookSummary() {
    return {};
}

} // namespace depthxr
