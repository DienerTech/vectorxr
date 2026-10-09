#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/vulkan_enable_hooks.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace depthxr {
namespace {

// What a game's VkDevice was made with (XR_KHR_vulkan_enable2).
struct DeviceRecord {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{nullptr};
    bool timeline_semaphores{false};
};

// The hooks' state, for the one OpenXR instance there is. The lock is never
// held across a call into the runtime or Vulkan: the runtime calls the
// layer's vkCreateInstance / vkCreateDevice from inside the hooks.
struct Hooks {
    std::mutex mutex;
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrGetInstanceProcAddr next{nullptr};
    bool wanted{false}; // latched when the instance was made
    std::function<void(const std::string&)> log;
    // The game's vkGetInstanceProcAddr, which the layer's own forwards to.
    // Kept past the instance: a runtime may hold on to the layer's.
    PFN_vkGetInstanceProcAddr game_get_instance_proc_addr{nullptr};
    // The VkInstance the runtime resolved vkCreateDevice for. Kept until
    // the next lookup (or instance): a runtime may hold on to the layer's
    // vkCreateDevice and call it without looking it up again.
    VkInstance device_instance{VK_NULL_HANDLE};
    // The VkInstance made through the layer last (when no lookup named one).
    VkInstance last_instance{VK_NULL_HANDLE};
    // The API version each VkInstance made through the layer asked for.
    std::unordered_map<std::uint64_t, std::uint32_t> instance_versions;
    std::unordered_map<std::uint64_t, DeviceRecord> devices;
    // Devices the runtime made in xrCreateVulkanDeviceKHR without the
    // layer's vkCreateDevice.
    std::unordered_set<std::uint64_t> unshimmed_devices;
    // How many times the layer's vkCreateInstance / vkCreateDevice ran.
    std::uint64_t instances_made{0};
    std::uint64_t devices_made{0};
    bool instance_list_logged{false};
    bool device_list_logged{false};
    std::string summary;
};

Hooks& State() {
    static Hooks hooks;
    return hooks;
}

std::string Joined(const std::vector<std::string>& names) {
    std::string text;
    for (const std::string& name : names) {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

void Report(const std::string& summary, const std::string& message) {
    std::function<void(const std::string&)> log;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        if (!summary.empty()) {
            hooks.summary += (hooks.summary.empty() ? "" : "; ") + summary;
        }
        log = hooks.log;
    }
    if (log && !message.empty()) {
        log(message);
    }
}

bool Listed(const char* const* names, std::uint32_t count, std::string_view name) {
    for (std::uint32_t i = 0; names && i < count; ++i) {
        if (names[i] && name == names[i]) return true;
    }
    return false;
}

template <typename T>
T Resolve(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance, const char* name) {
    return get_instance_proc_addr ? reinterpret_cast<T>(get_instance_proc_addr(instance, name)) : nullptr;
}

// --- The layer's vkGetInstanceProcAddr, given to the runtime ----------------

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* info,
                                                   const VkAllocationCallbacks* allocator, VkInstance* instance);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkDevice* device);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(VkInstance instance, const char* name) {
    PFN_vkGetInstanceProcAddr game = nullptr;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        game = hooks.game_get_instance_proc_addr;
    }
    if (!game || !name) {
        return nullptr;
    }
    const PFN_vkVoidFunction function = game(instance, name);
    const std::string_view requested(name);
    if (!function) {
        return nullptr;
    }
    if (requested == "vkGetInstanceProcAddr") {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetInstanceProcAddr);
    }
    if (requested == "vkCreateInstance") {
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateInstance);
    }
    if (requested == "vkCreateDevice") {
        // The runtime makes the device on this instance.
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        hooks.device_instance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateDevice);
    }
    return function;
}

// The game's instance as the runtime finally makes it, with the bridge's
// instance extensions the loader offers added.
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(const VkInstanceCreateInfo* info,
                                                   const VkAllocationCallbacks* allocator, VkInstance* instance) {
    PFN_vkGetInstanceProcAddr game = nullptr;
    bool wanted = false;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        game = hooks.game_get_instance_proc_addr;
        wanted = hooks.wanted;
        ++hooks.instances_made;
    }
    const auto create = Resolve<PFN_vkCreateInstance>(game, VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!wanted || !info || !instance) {
        return create(info, allocator, instance);
    }

    std::vector<VkExtensionProperties> offered;
    if (const auto enumerate = Resolve<PFN_vkEnumerateInstanceExtensionProperties>(
            game, VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties")) {
        std::uint32_t count = 0;
        if (enumerate(nullptr, &count, nullptr) == VK_SUCCESS && count) {
            offered.resize(count);
            if (enumerate(nullptr, &count, offered.data()) < VK_SUCCESS) offered.clear();
            offered.resize(std::min<std::size_t>(offered.size(), count));
        }
    }
    std::vector<const char*> names(info->ppEnabledExtensionNames,
                                   info->ppEnabledExtensionNames + (info->ppEnabledExtensionNames ? info->enabledExtensionCount : 0));
    std::vector<std::string> added;
    std::vector<std::string> missing;
    for (const char* name : VulkanBridge::InstanceExtensions()) {
        const bool available = std::any_of(offered.begin(), offered.end(), [&](const VkExtensionProperties& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
        if (Listed(names.data(), static_cast<std::uint32_t>(names.size()), name)) continue;
        (available ? added : missing).emplace_back(name);
        if (available) names.push_back(name);
    }
    VkInstanceCreateInfo augmented = *info;
    augmented.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    augmented.ppEnabledExtensionNames = names.empty() ? nullptr : names.data();
    VkResult result = create(&augmented, allocator, instance);
    if (result < VK_SUCCESS && !added.empty()) {
        // The game's instance never fails for the bridge's sake.
        Report("vkCreateInstance(refused " + Joined(added) + ")",
               "Frame Generation's Vulkan bridge: vkCreateInstance refused the bridge's extensions (result " +
                   std::to_string(static_cast<int>(result)) + "); the game's Vulkan instance is made without them.");
        added.clear();
        result = create(info, allocator, instance);
    }
    if (result >= VK_SUCCESS && *instance != VK_NULL_HANDLE) {
        {
            Hooks& hooks = State();
            std::scoped_lock lock(hooks.mutex);
            hooks.instance_versions[reinterpret_cast<std::uint64_t>(*instance)] =
                info->pApplicationInfo ? info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
            hooks.last_instance = *instance;
        }
        Report("vkCreateInstance(+" + std::to_string(added.size()) + (missing.empty() ? "" : ", lacks " + Joined(missing)) +
                   ")",
               (added.empty() ? std::string("Frame Generation's Vulkan bridge: nothing added to the game's Vulkan "
                                            "instance")
                              : "Frame Generation's Vulkan bridge: added " + Joined(added) +
                                    " to the game's Vulkan instance") +
                   (missing.empty() ? std::string(".") : "; the loader does not offer " + Joined(missing) + "."));
    }
    return result;
}

// What a device create info's chain says of timeline semaphores: nothing,
// enabled or left off.
enum class TimelineChain { kUnnamed, kEnabled, kOff };

TimelineChain TimelineIn(const void* chain) {
    TimelineChain found = TimelineChain::kUnnamed;
    for (const auto* next = static_cast<const VkBaseInStructure*>(chain); next; next = next->pNext) {
        VkBool32 timeline = VK_FALSE;
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            timeline = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(next)->timelineSemaphore;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            timeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(next)->timelineSemaphore;
        } else {
            continue;
        }
        if (timeline) return TimelineChain::kEnabled;
        found = TimelineChain::kOff;
    }
    return found;
}

// The game's device as the runtime finally makes it: the bridge's device
// extensions the physical device offers added, and timeline semaphores when
// the chain names neither them nor Vulkan 1.2's features and the device has
// them. The game's structures are never written.
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkDevice* device) {
    PFN_vkGetInstanceProcAddr game = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    bool wanted = false;
    std::uint32_t instance_version = 0; // 0: not made through the layer
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        game = hooks.game_get_instance_proc_addr;
        // The instance vkCreateDevice was looked up for last; a runtime
        // that never looked it up gets the instance made last.
        instance = hooks.device_instance != VK_NULL_HANDLE ? hooks.device_instance : hooks.last_instance;
        wanted = hooks.wanted;
        ++hooks.devices_made;
        const auto it = hooks.instance_versions.find(reinterpret_cast<std::uint64_t>(instance));
        if (it != hooks.instance_versions.end()) instance_version = std::max<std::uint32_t>(it->second, VK_API_VERSION_1_0);
    }
    const auto create = Resolve<PFN_vkCreateDevice>(game, instance, "vkCreateDevice");
    if (!create) {
        Report("vkCreateDevice(unresolved)",
               "Frame Generation's Vulkan bridge: the game's vkGetInstanceProcAddr has no vkCreateDevice for the "
               "game's Vulkan instance; the game's Vulkan device cannot be made.");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!wanted || !info || !device || !physical_device) {
        return create(physical_device, info, allocator, device);
    }

    std::vector<VkExtensionProperties> offered;
    if (const auto enumerate = Resolve<PFN_vkEnumerateDeviceExtensionProperties>(
            game, instance, "vkEnumerateDeviceExtensionProperties")) {
        std::uint32_t count = 0;
        if (enumerate(physical_device, nullptr, &count, nullptr) == VK_SUCCESS && count) {
            offered.resize(count);
            if (enumerate(physical_device, nullptr, &count, offered.data()) < VK_SUCCESS) offered.clear();
            offered.resize(std::min<std::size_t>(offered.size(), count));
        }
    }
    const auto offers = [&](const char* name) {
        return std::any_of(offered.begin(), offered.end(), [&](const VkExtensionProperties& property) {
            return std::strcmp(property.extensionName, name) == 0;
        });
    };
    std::vector<const char*> names(info->ppEnabledExtensionNames,
                                   info->ppEnabledExtensionNames + (info->ppEnabledExtensionNames ? info->enabledExtensionCount : 0));
    std::vector<std::string> added;
    std::vector<std::string> missing;
    for (const char* name : VulkanBridge::DeviceExtensions()) {
        if (Listed(names.data(), static_cast<std::uint32_t>(names.size()), name)) continue;
        (offers(name) ? added : missing).emplace_back(name);
        if (offers(name)) names.push_back(name);
    }

    // Timeline semaphores, as the game's chain has them, or added.
    VkDeviceCreateInfo augmented = *info;
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    const TimelineChain chain = TimelineIn(info->pNext);
    bool timeline = chain == TimelineChain::kEnabled;
    bool timeline_added = false;
    if (chain == TimelineChain::kUnnamed && instance_version != 0) {
        VkPhysicalDeviceProperties properties{};
        if (const auto get_properties =
                Resolve<PFN_vkGetPhysicalDeviceProperties>(game, instance, "vkGetPhysicalDeviceProperties")) {
            get_properties(physical_device, &properties);
        }
        auto get_features = Resolve<PFN_vkGetPhysicalDeviceFeatures2>(game, instance, "vkGetPhysicalDeviceFeatures2KHR");
        if (!get_features && instance_version >= VK_API_VERSION_1_1) {
            get_features = Resolve<PFN_vkGetPhysicalDeviceFeatures2>(game, instance, "vkGetPhysicalDeviceFeatures2");
        }
        VkPhysicalDeviceTimelineSemaphoreFeatures supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &supported;
        if (get_features) {
            get_features(physical_device, &features);
        }
        // Below Vulkan 1.2 (the instance's or the device's) the feature is an extension's.
        const bool core = std::min(instance_version, properties.apiVersion) >= VK_API_VERSION_1_2;
        const bool extension_needed = !core && !Listed(names.data(), static_cast<std::uint32_t>(names.size()),
                                                       VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        if (supported.timelineSemaphore && (!extension_needed || offers(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))) {
            if (extension_needed) {
                names.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
                added.emplace_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
            }
            timeline_features.timelineSemaphore = VK_TRUE;
            timeline_features.pNext = const_cast<void*>(info->pNext);
            augmented.pNext = &timeline_features;
            timeline = timeline_added = true;
        }
    }
    augmented.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    augmented.ppEnabledExtensionNames = names.empty() ? nullptr : names.data();
    VkResult result = create(physical_device, &augmented, allocator, device);
    if (result < VK_SUCCESS && (!added.empty() || timeline_added)) {
        // The game's device never fails for the bridge's sake.
        Report("vkCreateDevice(refused " + Joined(added) + (timeline_added ? " timeline" : "") + ")",
               "Frame Generation's Vulkan bridge: vkCreateDevice refused the bridge's additions (result " +
                   std::to_string(static_cast<int>(result)) +
                   "); the game's Vulkan device is made without them, and Frame Generation will be off.");
        added.clear();
        timeline_added = false;
        timeline = chain == TimelineChain::kEnabled;
        result = create(physical_device, info, allocator, device);
    }
    if (result >= VK_SUCCESS && *device != VK_NULL_HANDLE) {
        {
            Hooks& hooks = State();
            std::scoped_lock lock(hooks.mutex);
            hooks.devices[reinterpret_cast<std::uint64_t>(*device)] = {game, timeline};
        }
        const std::string semaphores = timeline_added ? "timeline semaphores added"
                                       : timeline     ? "timeline semaphores (the game's)"
                                       : chain == TimelineChain::kOff ? "binary semaphores (the game left timeline "
                                                                        "semaphores off)"
                                                                      : "binary semaphores";
        Report("vkCreateDevice(+" + std::to_string(added.size()) + (timeline_added ? ", timeline added" : "") +
                   (timeline ? ", timeline" : ", binary") + (missing.empty() ? "" : ", lacks " + Joined(missing)) + ")",
               (added.empty() ? std::string("Frame Generation's Vulkan bridge: nothing added to the game's Vulkan "
                                            "device")
                              : "Frame Generation's Vulkan bridge: added " + Joined(added) +
                                    " to the game's Vulkan device") +
                   (missing.empty() ? std::string() : "; the device does not offer " + Joined(missing)) + "; " +
                   semaphores + ".");
    }
    return result;
}

// --- The four OpenXR functions ------------------------------------------------

template <typename T>
T Next(XrInstance instance, const char* name, bool& wanted) {
    PFN_xrGetInstanceProcAddr next = nullptr;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        next = hooks.next;
        wanted = hooks.wanted;
    }
    PFN_xrVoidFunction function = nullptr;
    if (!next || XR_FAILED(next(instance, name, &function))) {
        return nullptr;
    }
    return reinterpret_cast<T>(function);
}

// XR_KHR_vulkan_enable's space-separated lists with `extra` appended, under
// the two-call idiom (the count includes the terminating NUL).
template <typename F>
XrResult WithBridgeExtensions(F runtime, XrInstance instance, XrSystemId system, std::uint32_t capacity,
                              std::uint32_t* count, char* buffer, const std::vector<const char*>& extra,
                              const char* what, bool& logged) {
    std::uint32_t size = 0;
    XrResult result = runtime(instance, system, 0, &size, nullptr);
    if (XR_FAILED(result)) {
        return result;
    }
    std::string list(size, '\0');
    if (size) {
        result = runtime(instance, system, size, &size, list.data());
        if (XR_FAILED(result)) {
            return result;
        }
    }
    list.resize(std::strlen(list.c_str()));
    const std::string extended = VulkanBridge::WithExtensions(list, extra);
    if (!count) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *count = static_cast<std::uint32_t>(extended.size() + 1);
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < *count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    if (!buffer) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::memcpy(buffer, extended.c_str(), extended.size() + 1);
    bool first = false;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        first = !logged;
        logged = true;
    }
    if (first) {
        const std::string runtime_list = VulkanBridge::WithExtensions(list, {});
        const std::size_t added = std::count(extended.begin(), extended.end(), ' ') + (extended.empty() ? 0 : 1) -
                                  (std::count(runtime_list.begin(), runtime_list.end(), ' ') + (runtime_list.empty() ? 0 : 1));
        Report(std::string(what) + "(+" + std::to_string(added) + ")",
               "Frame Generation's Vulkan bridge: the game's " + std::string(what) + " list is \"" + extended +
                   "\" (" + std::to_string(added) + " of the bridge's names added to the runtime's).");
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL HookGetVulkanInstanceExtensions(XrInstance instance, XrSystemId system, std::uint32_t capacity,
                                                    std::uint32_t* count, char* buffer) {
    bool wanted = false;
    const auto runtime =
        Next<PFN_xrGetVulkanInstanceExtensionsKHR>(instance, "xrGetVulkanInstanceExtensionsKHR", wanted);
    if (!runtime) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!wanted) {
        return runtime(instance, system, capacity, count, buffer);
    }
    return WithBridgeExtensions(runtime, instance, system, capacity, count, buffer, VulkanBridge::InstanceExtensions(),
                                "Vulkan instance extension", State().instance_list_logged);
}

XrResult XRAPI_CALL HookGetVulkanDeviceExtensions(XrInstance instance, XrSystemId system, std::uint32_t capacity,
                                                  std::uint32_t* count, char* buffer) {
    bool wanted = false;
    const auto runtime = Next<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR", wanted);
    if (!runtime) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!wanted) {
        return runtime(instance, system, capacity, count, buffer);
    }
    return WithBridgeExtensions(runtime, instance, system, capacity, count, buffer, VulkanBridge::DeviceExtensions(),
                                "Vulkan device extension", State().device_list_logged);
}

XrResult XRAPI_CALL HookCreateVulkanInstance(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* info,
                                             VkInstance* vulkan_instance, VkResult* vulkan_result) {
    bool wanted = false;
    const auto runtime = Next<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR", wanted);
    if (!runtime) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!wanted || !info || !info->pfnGetInstanceProcAddr) {
        return runtime(instance, info, vulkan_instance, vulkan_result);
    }
    std::uint64_t made = 0;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        hooks.game_get_instance_proc_addr = info->pfnGetInstanceProcAddr;
        made = hooks.instances_made;
    }
    XrVulkanInstanceCreateInfoKHR forwarded = *info;
    forwarded.pfnGetInstanceProcAddr = LayerGetInstanceProcAddr;
    const XrResult result = runtime(instance, &forwarded, vulkan_instance, vulkan_result);
    bool ran = false;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        ran = hooks.instances_made != made;
    }
    if (XR_SUCCEEDED(result) && !ran) {
        Report("xrCreateVulkanInstanceKHR(not seen)",
               "Frame Generation's Vulkan bridge: the runtime made the game's Vulkan instance without the layer's "
               "vkCreateInstance; the bridge's extensions could not be added.");
    }
    return result;
}

XrResult XRAPI_CALL HookCreateVulkanDevice(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* info,
                                           VkDevice* vulkan_device, VkResult* vulkan_result) {
    bool wanted = false;
    const auto runtime = Next<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR", wanted);
    if (!runtime) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (!wanted || !info || !info->pfnGetInstanceProcAddr) {
        return runtime(instance, info, vulkan_device, vulkan_result);
    }
    std::uint64_t made = 0;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        hooks.game_get_instance_proc_addr = info->pfnGetInstanceProcAddr;
        made = hooks.devices_made;
    }
    XrVulkanDeviceCreateInfoKHR forwarded = *info;
    forwarded.pfnGetInstanceProcAddr = LayerGetInstanceProcAddr;
    const XrResult result = runtime(instance, &forwarded, vulkan_device, vulkan_result);
    bool ran = false;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        ran = hooks.devices_made != made;
        if (XR_SUCCEEDED(result) && !ran && vulkan_device && *vulkan_device) {
            hooks.unshimmed_devices.insert(reinterpret_cast<std::uint64_t>(*vulkan_device));
        }
    }
    if (XR_SUCCEEDED(result) && !ran) {
        Report("xrCreateVulkanDeviceKHR(not seen)",
               "Frame Generation's Vulkan bridge: the runtime made the game's Vulkan device without the layer's "
               "vkCreateDevice; the bridge's extensions could not be added, and Frame Generation will be off.");
    }
    return result;
}

} // namespace

void ResetVulkanEnableHooks(XrInstance instance, PFN_xrGetInstanceProcAddr next_get_instance_proc_addr,
                            bool bridge_wanted, std::function<void(const std::string&)> log) {
    Hooks& hooks = State();
    std::scoped_lock lock(hooks.mutex);
    hooks.instance = instance;
    hooks.next = instance != XR_NULL_HANDLE ? next_get_instance_proc_addr : nullptr;
    hooks.wanted = instance != XR_NULL_HANDLE && bridge_wanted;
    hooks.log = instance != XR_NULL_HANDLE ? std::move(log) : nullptr;
    // Devices of an instance before are not this one's.
    hooks.device_instance = VK_NULL_HANDLE;
    hooks.last_instance = VK_NULL_HANDLE;
    hooks.instance_versions.clear();
    hooks.devices.clear();
    hooks.unshimmed_devices.clear();
    hooks.instance_list_logged = false;
    hooks.device_list_logged = false;
    hooks.summary.clear();
}

PFN_xrVoidFunction VulkanEnableHook(XrInstance instance, const char* name) {
    if (!name) {
        return nullptr;
    }
    static const std::pair<std::string_view, PFN_xrVoidFunction> kHooks[] = {
        {"xrGetVulkanInstanceExtensionsKHR", reinterpret_cast<PFN_xrVoidFunction>(HookGetVulkanInstanceExtensions)},
        {"xrGetVulkanDeviceExtensionsKHR", reinterpret_cast<PFN_xrVoidFunction>(HookGetVulkanDeviceExtensions)},
        {"xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction>(HookCreateVulkanInstance)},
        {"xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction>(HookCreateVulkanDevice)},
    };
    const auto hook = std::find_if(std::begin(kHooks), std::end(kHooks),
                                   [&](const auto& entry) { return entry.first == name; });
    if (hook == std::end(kHooks)) {
        return nullptr;
    }
    PFN_xrGetInstanceProcAddr next = nullptr;
    {
        Hooks& hooks = State();
        std::scoped_lock lock(hooks.mutex);
        next = hooks.next;
    }
    // Only what the runtime offers: an application without the extension
    // gets the runtime's answer.
    PFN_xrVoidFunction runtime = nullptr;
    if (!next || XR_FAILED(next(instance, name, &runtime)) || !runtime) {
        return nullptr;
    }
    return hook->second;
}

VulkanBridgeGame VulkanGameFor(std::uint64_t device) {
    Hooks& hooks = State();
    std::scoped_lock lock(hooks.mutex);
    const auto it = hooks.devices.find(device);
    if (it == hooks.devices.end()) {
        return {};
    }
    return {reinterpret_cast<PFN_xrVoidFunction>(it->second.get_instance_proc_addr), it->second.timeline_semaphores};
}

VulkanDeviceOrigin VulkanDeviceOriginOf(std::uint64_t device) {
    Hooks& hooks = State();
    std::scoped_lock lock(hooks.mutex);
    if (hooks.devices.count(device)) {
        return VulkanDeviceOrigin::kRuntimeWithLayer;
    }
    return hooks.unshimmed_devices.count(device) ? VulkanDeviceOrigin::kRuntimeWithoutLayer : VulkanDeviceOrigin::kGame;
}

std::uint64_t VulkanSessionDevice(const XrSessionCreateInfo* info) {
    const auto* first = info ? static_cast<const XrBaseInStructure*>(info->next) : nullptr;
    if (!first || first->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        return 0;
    }
    return reinterpret_cast<std::uint64_t>(reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(first)->device);
}

void RequeryVulkanGraphicsRequirements(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr, XrInstance instance,
                                       XrSystemId system_id, int enable_version) {
    if (!next_get_instance_proc_addr || instance == XR_NULL_HANDLE || (enable_version != 1 && enable_version != 2)) {
        return;
    }
    // Both versions take the same structure (XrGraphicsRequirementsVulkan2KHR
    // is XrGraphicsRequirementsVulkanKHR).
    PFN_xrGetVulkanGraphicsRequirementsKHR get_requirements = nullptr;
    const char* name =
        enable_version == 2 ? "xrGetVulkanGraphicsRequirements2KHR" : "xrGetVulkanGraphicsRequirementsKHR";
    if (XR_SUCCEEDED(next_get_instance_proc_addr(instance, name,
                                                 reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements))) &&
        get_requirements) {
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        get_requirements(instance, system_id, &requirements);
    }
}

std::string VulkanEnableHookSummary() {
    Hooks& hooks = State();
    std::scoped_lock lock(hooks.mutex);
    std::string summary;
    summary.swap(hooks.summary);
    return summary;
}

} // namespace depthxr
