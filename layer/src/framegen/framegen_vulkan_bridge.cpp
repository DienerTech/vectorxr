#include "depthxr/framegen_vulkan_bridge.h"

// Interface stub: every session stays as the game made it until the bridge's
// GPU parts land. The pure parts are in framegen_vulkan_formats.cpp.

namespace depthxr {

struct VulkanBridge::Impl {};

VulkanBridge::VulkanBridge() = default;
VulkanBridge::~VulkanBridge() = default;

void VulkanBridge::SetLog(std::function<void(const std::string&)> log) {
    log_ = std::move(log);
}

const XrSessionCreateInfo* VulkanBridge::Begin(XrInstance, const XrSessionCreateInfo*, PFN_xrGetInstanceProcAddr,
                                               const VulkanBridgeGame&, std::string& reason, std::string& description) {
    End();
    queried_d3d12_requirements_ = false;
    description.clear();
    reason = "this build has no Vulkan bridge";
    return nullptr;
}

bool VulkanBridge::QueriedD3D12Requirements() const {
    return queried_d3d12_requirements_;
}

bool VulkanBridge::Active() const {
    return impl_ != nullptr;
}

const void* VulkanBridge::Binding() const {
    return nullptr;
}

void VulkanBridge::SessionCreated() {}

bool VulkanBridge::Escape() {
    return true;
}

bool VulkanBridge::Drain() {
    return true;
}

void VulkanBridge::End() {
    impl_.reset();
}

bool VulkanBridge::Lost(std::string& reason) const {
    reason.clear();
    return false;
}

void VulkanBridge::FrameEnded() {}

std::vector<std::int64_t> VulkanBridge::GameFormats(const std::vector<std::int64_t>& runtime_formats) const {
    return TranslatedFormats(runtime_formats);
}

bool VulkanBridge::CheckSwapchain(const XrSwapchainCreateInfo&, XrResult& refusal, std::string& reason) const {
    refusal = XR_ERROR_RUNTIME_FAILURE;
    reason = "the session is not bridged";
    return false;
}

bool VulkanBridge::AddSwapchain(XrSwapchain, const XrSwapchainCreateInfo&, PFN_xrEnumerateSwapchainImages,
                                std::string& reason) {
    reason = "the session is not bridged";
    return false;
}

bool VulkanBridge::Bridged(XrSwapchain) const {
    return false;
}

bool VulkanBridge::Unmirrored(XrSwapchain) const {
    return false;
}

bool VulkanBridge::HasUnmirrored() const {
    return false;
}

std::vector<std::uint64_t> VulkanBridge::Images(XrSwapchain) const {
    return {};
}

XrResult VulkanBridge::EnumerateImages(XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*) const {
    return XR_ERROR_HANDLE_INVALID;
}

void VulkanBridge::Acquired(XrSwapchain, std::uint32_t) {}

bool VulkanBridge::Released(XrSwapchain) {
    return true;
}

bool VulkanBridge::RemoveSwapchain(XrSwapchain) {
    return true;
}

bool VulkanBridge::WaitIdle(std::uint32_t) {
    return true;
}

std::string VulkanBridge::Describe(XrSwapchain) const {
    return {};
}

std::string VulkanBridge::Summary() const {
    return {};
}

} // namespace depthxr
