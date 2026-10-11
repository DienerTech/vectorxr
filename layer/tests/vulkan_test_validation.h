#pragma once

// Khronos validation for the Vulkan tests. The build looks for
// VkLayer_khronos_validation.dll (the Vulkan SDK's, or a copy it was pointed
// at), copies it into the build tree with a manifest and passes the directory
// as DEPTHXR_VULKAN_LAYER_DIR. Tests call ConfigureVulkanLoader() before they
// load vulkan-1.dll, enable the layer when the loader lists it, and fail on
// any error or validation warning the messenger collects. Without the layer
// they run with a warning: validation is never a reason to skip.

#include <windows.h>
#include <vulkan/vulkan_core.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vulkan_test {

#ifdef DEPTHXR_VULKAN_LAYER_DIR
inline constexpr const char* kValidationLayerDir = DEPTHXR_VULKAN_LAYER_DIR;
#else
inline constexpr const char* kValidationLayerDir = "";
#endif
inline constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// With VECTORXR_TESTS_REQUIRE_HW=1 (CMake's DEPTHXR_REQUIRE_HW_TESTS) the suite
// must run on the GPU with validation and the Direct3D 12 debug layer: what
// would skip (77) or warn fails instead.
inline bool RequireHardware() {
    char* value = nullptr;
    std::size_t length = 0;
    const bool required = _dupenv_s(&value, &length, "VECTORXR_TESTS_REQUIRE_HW") == 0 && value &&
                          std::strcmp(value, "1") == 0;
    std::free(value);
    return required;
}

// Before vulkan-1.dll is loaded: the validation layer's directory joins the
// loader's search path, and implicit layers (overlays, capture hooks) stay out
// of the test, unless the environment already says otherwise.
inline void ConfigureVulkanLoader() {
    char* existing = nullptr;
    std::size_t length = 0;
    if (kValidationLayerDir[0] && _dupenv_s(&existing, &length, "VK_ADD_LAYER_PATH") == 0 && !existing) {
        _putenv_s("VK_ADD_LAYER_PATH", kValidationLayerDir);
    }
    std::free(existing);
    existing = nullptr;
    if (_dupenv_s(&existing, &length, "VK_LOADER_LAYERS_DISABLE") == 0 && !existing) {
        _putenv_s("VK_LOADER_LAYERS_DISABLE", "~implicit~");
    }
    std::free(existing);
}

// Whether the loader lists the validation layer.
inline bool ValidationAvailable(PFN_vkEnumerateInstanceLayerProperties enumerate) {
    if (!enumerate) return false;
    std::uint32_t count = 0;
    if (enumerate(&count, nullptr) != VK_SUCCESS || count == 0) return false;
    std::vector<VkLayerProperties> layers(count);
    if (enumerate(&count, layers.data()) != VK_SUCCESS) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (std::strcmp(layers[i].layerName, kValidationLayer) == 0) return true;
    }
    return false;
}

// What the debug-utils messenger reported: errors, and warnings of the
// validation type. Callbacks come from any thread.
struct Messages {
    std::mutex mutex;
    std::vector<std::string> collected;

    void Add(const std::string& message) {
        std::scoped_lock lock(mutex);
        collected.push_back(message);
    }
    std::vector<std::string> Take() {
        std::scoped_lock lock(mutex);
        std::vector<std::string> taken;
        taken.swap(collected);
        return taken;
    }
};

inline VKAPI_ATTR VkBool32 VKAPI_CALL CollectMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void* user_data) {
    const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    const bool validation_warning = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
                                    (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
    if ((error || validation_warning) && user_data && data) {
        std::string text = error ? "[vulkan error] " : "[vulkan warning] ";
        if (data->pMessageIdName) text += std::string(data->pMessageIdName) + ": ";
        if (data->pMessage) text += data->pMessage;
        static_cast<Messages*>(user_data)->Add(text);
    }
    return VK_FALSE;
}

// The messenger's create info (also chained into VkInstanceCreateInfo, so
// instance creation and destruction are checked too).
inline VkDebugUtilsMessengerCreateInfoEXT MessengerInfo(Messages* messages) {
    VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = CollectMessage;
    info.pUserData = messages;
    return info;
}

inline void PrintValidationState(bool on) {
    if (on) {
        std::printf("Vulkan validation: on (%s from %s)\n", kValidationLayer,
                    kValidationLayerDir[0] ? kValidationLayerDir : "the loader's search path");
    } else {
        std::printf("WARNING: Vulkan validation is off (no %s): Vulkan usage is checked by behaviour only.\n",
                    kValidationLayer);
    }
}

} // namespace vulkan_test
