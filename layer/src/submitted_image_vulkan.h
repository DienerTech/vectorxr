#pragma once

// The Vulkan half of submitted_image_gpu.cpp, in a source of its own: only
// submitted_image_vulkan.cpp is compiled with OpenXR's Vulkan types. Internal
// to the imaging library.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "depthxr/submitted_image.h"

namespace depthxr {

// A Vulkan session's object: the stabilized mirror window only. Null with
// `error` when the game's Vulkan device cannot share an image with Direct3D
// 12 (made without VK_KHR_external_memory_win32).
std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageVulkan(const SubmittedImageBinding& binding,
                                                              std::string& error);
// `entry`: a structure of a session's create info chain. True, with the
// binding's fields in `captured`, when it is the Vulkan graphics binding.
bool CaptureVulkanBinding(const void* entry, SubmittedImageBinding& captured);
// TrackSubmittedSwapchain for a Vulkan object: the swapchain's `count` images
// from the runtime's enumeration.
bool TrackSubmittedSwapchainVulkan(
    SubmittedImageGpu& gpu, XrSwapchain swapchain, std::uint32_t count,
    const std::function<XrResult(XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*)>& enumerate,
    std::string& error);

} // namespace depthxr
