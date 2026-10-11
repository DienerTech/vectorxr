#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <openxr/openxr.h>

#include "depthxr/framegen_vulkan_bridge.h"

namespace depthxr {

// XR_KHR_vulkan_enable and XR_KHR_vulkan_enable2 for Frame Generation's Vulkan
// bridge (VulkanBridge). The game's Vulkan instance and device are made before
// its session, so what the bridge needs is added there: the layer answers four
// of the extensions' functions itself.
//   xrGetVulkanInstanceExtensionsKHR, xrGetVulkanDeviceExtensionsKHR (enable):
//     the runtime's lists with the bridge's names appended.
//   xrCreateVulkanInstanceKHR, xrCreateVulkanDeviceKHR (enable2): forwarded to
//     the runtime with a vkGetInstanceProcAddr of the layer's own in place of
//     the game's, so the layer sees the create info the runtime finally passes
//     to vkCreateInstance / vkCreateDevice. There it adds the bridge's
//     extensions (device: only those the physical device offers) and, when the
//     chain names no Vulkan 1.2 or timeline semaphore features, timeline
//     semaphores (when the device supports them). The game's memory is never
//     written. The instance, the device and whether it has timeline semaphores
//     are kept for the bridge.
// Names are added only when the bridge was wanted when the instance was made
// (Frame Generation enabled for the executable, the bridge not switched off,
// XR_KHR_D3D12_enable on the instance); otherwise every call goes straight to
// the runtime.
//
// One OpenXR instance at a time (the layer allows no more).

// At xrCreateInstance, after the runtime made `instance` (and at
// xrDestroyInstance, with XR_NULL_HANDLE): the runtime's xrGetInstanceProcAddr,
// whether the bridge is wanted for this instance (decided once, so both calls
// of an extension list's two-call idiom agree), and where what was added is
// reported.
void ResetVulkanEnableHooks(XrInstance instance, PFN_xrGetInstanceProcAddr next_get_instance_proc_addr,
                            bool bridge_wanted, std::function<void(const std::string&)> log);

// For the layer's xrGetInstanceProcAddr: the layer's own function for `name`
// when it is one of the four and the runtime offers it; null otherwise (the
// name then resolves as before).
PFN_xrVoidFunction VulkanEnableHook(XrInstance instance, const char* name);

// What the hooks know about a game's VkDevice (its handle as an integer), for
// VulkanBridge::Begin: the game's vkGetInstanceProcAddr (XR_KHR_vulkan_enable2)
// and whether the device has timeline semaphores. Unknown devices (made by the
// game itself, XR_KHR_vulkan_enable) get the defaults.
VulkanBridgeGame VulkanGameFor(std::uint64_t device);

// How a game's VkDevice (its handle as an integer) was made, as the hooks saw
// it while the bridge was wanted: by the runtime in xrCreateVulkanDeviceKHR,
// with the layer's vkCreateDevice or without it, or otherwise by the game
// itself (XR_KHR_vulkan_enable; also any device the hooks did not see).
enum class VulkanDeviceOrigin { kGame, kRuntimeWithLayer, kRuntimeWithoutLayer };
VulkanDeviceOrigin VulkanDeviceOriginOf(std::uint64_t device);

// The game's VkDevice (its handle as an integer) named by a session create
// info whose first structure is a Vulkan binding; 0 otherwise. For the layer,
// which is built without Vulkan's types.
std::uint64_t VulkanSessionDevice(const XrSessionCreateInfo* info);

// A session made with Vulkan after the bridge asked the runtime for Direct3D
// 12's graphics requirements asks for the Vulkan ones again (`enable_version`
// 1 or 2, the extension the game enabled), as a runtime may expect the
// requirements of the API it was asked about last.
void RequeryVulkanGraphicsRequirements(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr, XrInstance instance,
                                       XrSystemId system_id, int enable_version);

// Which of the four ran and what they added, since the last call (for the
// session's log line and tests).
std::string VulkanEnableHookSummary();

} // namespace depthxr
