// vectorxr_hardware_probe --graphics vulkan: a Vulkan game for the OpenXR runtime. With XR_KHR_vulkan_enable2 the
// runtime creates the VkInstance and VkDevice from the probe's create infos; with XR_KHR_vulkan_enable the probe
// creates them with the extensions the runtime names. Views are cleared and the pattern drawn with
// vkCmdClearAttachments in a render pass (there is no line scene); the GPU load is a SPIR-V compute shader. Only
// Vulkan 1.0 entry points are used, so every --vulkan-api runs the same code.
#include "probe_common.h"

#ifdef VECTORXR_PROBE_SPIRV
#include "probe_vulkan_load.h" // kProbeLoadSpirv: probe_vulkan_load.hlsl compiled by DXC (-Fh)
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace probe {
namespace {

#define PROBE_VK_GLOBAL_FUNCTIONS(X) X(CreateInstance) X(EnumerateInstanceLayerProperties)
#define PROBE_VK_INSTANCE_FUNCTIONS(X)                                                                              \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties)                                  \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice)                 \
    X(GetDeviceProcAddr)
#define PROBE_VK_DEVICE_FUNCTIONS(X)                                                                                \
    X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(QueueWaitIdle) X(CreateCommandPool) X(DestroyCommandPool)  \
    X(ResetCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateFence)         \
    X(DestroyFence) X(ResetFences) X(GetFenceStatus) X(WaitForFences) X(CreateImageView) X(DestroyImageView)       \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CmdBeginRenderPass)      \
    X(CmdEndRenderPass) X(CmdClearAttachments) X(CmdPipelineBarrier) X(CreateBuffer) X(DestroyBuffer)              \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(CmdFillBuffer)            \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)        \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline)                  \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets)             \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CreateQueryPool)              \
    X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults) X(CreateImage)            \
    X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(CmdCopyImageToBuffer) X(MapMemory)          \
    X(UnmapMemory)

// The probe links no Vulkan library: vulkan-1.dll is loaded at run time and every function resolved through it.
struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr{};
#define PROBE_VK_DECLARE(name) PFN_vk##name name{};
    PROBE_VK_GLOBAL_FUNCTIONS(PROBE_VK_DECLARE)
    PROBE_VK_INSTANCE_FUNCTIONS(PROBE_VK_DECLARE)
    PROBE_VK_DEVICE_FUNCTIONS(PROBE_VK_DECLARE)
#undef PROBE_VK_DECLARE
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2{}; // Vulkan 1.1 instances only (the LUID)
    PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT{};
    PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT{};
};

const char* VkResultName(VkResult result) {
    switch (result) {
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    default:
        return nullptr;
    }
}

void CheckVk(VkResult result, std::string_view operation) {
    if (result == VK_SUCCESS) {
        return;
    }
    std::ostringstream stream;
    stream << operation << " failed: VkResult " << static_cast<int32_t>(result);
    if (const char* name = VkResultName(result)) {
        stream << " (" << name << ")";
    }
    throw std::runtime_error(stream.str());
}

// An XrVersion's major.minor as a Vulkan API version (an API version has no patch).
uint32_t VulkanApiFromXr(XrVersion version) {
    return VK_MAKE_API_VERSION(0, XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version), 0);
}

// Whether `api`'s major.minor is at least `minimum`'s.
bool ApiAtLeast(uint32_t api, uint32_t minimum) {
    return VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), 0) >=
           VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(minimum), VK_API_VERSION_MINOR(minimum), 0);
}

std::string ApiText(uint32_t api) {
    return std::to_string(VK_API_VERSION_MAJOR(api)) + "." + std::to_string(VK_API_VERSION_MINOR(api));
}

// XR_KHR_vulkan_enable's extension lists: names separated by spaces.
std::vector<std::string> SplitNames(std::string_view text) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(' ', start), text.size());
        if (end > start && text[start] != '\0') {
            names.emplace_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return names;
}

std::string JoinNames(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        joined += (joined.empty() ? "" : " ") + name;
    }
    return joined.empty() ? "none" : joined;
}

const std::vector<int64_t>& VulkanColorFormats() {
    static const std::vector<int64_t> formats{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                                              VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    return formats;
}

VkRect2D ToVulkan(const PixelRect& rect, int32_t x, int32_t y) {
    return {{rect.left + x, rect.top + y},
            {static_cast<uint32_t>(rect.right - rect.left), static_cast<uint32_t>(rect.bottom - rect.top)}};
}

// --vulkan-validation: the debug-utils messenger's errors and its validation and performance warnings (the first
// few, and how many); the loader's general warnings (which layers it skipped) are left out. The runtime's own Vulkan
// calls on the probe's device are validated too.
struct VulkanMessages {
    std::mutex mutex;
    uint64_t errors{0};
    uint64_t warnings{0};
    std::vector<std::pair<bool, std::string>> first; // error?, message
    // After the summary (VulkanDevice::Report): the counts it printed, and the first few messages since.
    bool reported{false};
    uint64_t reported_errors{0};
    uint64_t reported_warnings{0};
    std::vector<std::pair<bool, std::string>> late;
};

VKAPI_ATTR VkBool32 VKAPI_CALL OnVulkanMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                               VkDebugUtilsMessageTypeFlagsEXT types,
                                               const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* messages = static_cast<VulkanMessages*>(user);
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (!error && (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    std::scoped_lock lock(messages->mutex);
    ++(error ? messages->errors : messages->warnings);
    auto& kept = messages->reported ? messages->late : messages->first;
    if (kept.size() < 10) {
        kept.emplace_back(error, data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

// --render-check and --gpu-calibrate without OpenXR: the first discrete GPU, else the first that is not a CPU
// implementation; nullopt when there is none (the check cannot run here).
std::optional<std::size_t> StandaloneDeviceIndex(const std::vector<VkPhysicalDeviceType>& types) {
    std::optional<std::size_t> chosen;
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (types[index] == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            return index;
        }
        if (!chosen && types[index] != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            chosen = index;
        }
    }
    return chosen;
}

// The probe's Vulkan instance, device and queue, and its frames' command buffers. The instance and the device are
// made by the probe (XR_KHR_vulkan_enable, --gpu-calibrate) or by the runtime from the probe's create infos
// (XR_KHR_vulkan_enable2); either way the probe destroys them, after the session that used them.
class VulkanDevice {
public:
    explicit VulkanDevice(const Options& options) : options_(options) {
        if (options_.vulkan_validation) {
            UseValidationLayerPath();
        }
        loader_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (loader_ == nullptr) {
            throw Unavailable("vulkan-1.dll is not installed (no Vulkan driver)");
        }
        vk_.GetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader_, "vkGetInstanceProcAddr"));
        if (vk_.GetInstanceProcAddr == nullptr) {
            throw std::runtime_error("vulkan-1.dll has no vkGetInstanceProcAddr");
        }
#define PROBE_VK_LOAD(name)                                                                                         \
    vk_.name = reinterpret_cast<PFN_vk##name>(vk_.GetInstanceProcAddr(nullptr, "vk" #name));                       \
    if (vk_.name == nullptr) {                                                                                      \
        throw std::runtime_error("the Vulkan loader lacks vk" #name);                                              \
    }
        PROBE_VK_GLOBAL_FUNCTIONS(PROBE_VK_LOAD)
#undef PROBE_VK_LOAD
    }

    // A queue that never went idle keeps everything: the device, the instance and the loader.
    ~VulkanDevice() {
        DestroyDevice();
        if (!idle_stuck_) {
            if (messenger_ != VK_NULL_HANDLE) {
                vk_.DestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            }
            if (instance_ != VK_NULL_HANDLE) {
                vk_.DestroyInstance(instance_, nullptr);
            }
        }
        ReportLate();
        if (loader_ != nullptr && !idle_stuck_) {
            FreeLibrary(loader_);
        }
    }

    // Waits for the queue (bounded), then destroys the device's objects and the device, while the messenger still
    // reports what vkDestroyDevice finds (objects never destroyed). After a wait that timed out nothing is destroyed:
    // the stuck queue may still use it. Idempotent; never throws.
    void DestroyDevice() {
        if (device_ == VK_NULL_HANDLE || !WaitIdle("the Vulkan device is destroyed")) {
            return;
        }
        if (idle_fence_ != VK_NULL_HANDLE) {
            vk_.DestroyFence(device_, idle_fence_, nullptr);
        }
        if (once_fence_ != VK_NULL_HANDLE) {
            vk_.DestroyFence(device_, once_fence_, nullptr);
        }
        if (once_pool_ != VK_NULL_HANDLE) {
            vk_.DestroyCommandPool(device_, once_pool_, nullptr);
        }
        for (Slot& slot : slots_) {
            if (slot.fence != VK_NULL_HANDLE) {
                vk_.DestroyFence(device_, slot.fence, nullptr);
            }
            if (slot.pool != VK_NULL_HANDLE) {
                vk_.DestroyCommandPool(device_, slot.pool, nullptr);
            }
            slot = Slot{};
        }
        if (options_.vk_test_leak) {
            const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence leaked = VK_NULL_HANDLE;
            if (vk_.CreateFence(device_, &fence_info, nullptr, &leaked) == VK_SUCCESS) {
                std::cout << "Vulkan: --vk-test-leak leaves a VkFence undestroyed\n";
            }
        }
        vk_.DestroyDevice(device_, nullptr);
        idle_fence_ = VK_NULL_HANDLE;
        once_fence_ = VK_NULL_HANDLE;
        once_pool_ = VK_NULL_HANDLE;
        once_buffer_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
    }

    // Whether a wait for the queue timed out: what it may still use is kept, not destroyed.
    bool Stuck() const {
        return idle_stuck_;
    }

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    const VulkanFunctions& vk() const {
        return vk_;
    }

    // The probe's VkInstanceCreateInfo: --vulkan-api, `extensions` (the runtime's, with XR_KHR_vulkan_enable), and
    // with --vulkan-validation the Khronos layer and a messenger for the instance's own creation; without OpenXR
    // (`offline`: --gpu-calibrate, --render-check) also synchronization validation, which would report the runtime's
    // own use of the queue in a session. Valid until the next call.
    const VkInstanceCreateInfo& InstanceCreateInfo(const std::vector<std::string>& extensions, bool offline = false) {
        instance_extensions_ = extensions;
        layers_.clear();
        const auto add_extension = [&](const char* name) {
            if (std::find(instance_extensions_.begin(), instance_extensions_.end(), name) ==
                instance_extensions_.end()) {
                instance_extensions_.push_back(name);
            }
        };
        if (options_.vulkan_validation) {
            RequireValidationLayer();
            layers_.push_back("VK_LAYER_KHRONOS_validation");
            add_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (offline) {
                add_extension(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            }
        }
        instance_extension_names_.clear();
        for (const std::string& name : instance_extensions_) {
            instance_extension_names_.push_back(name.c_str());
        }
        application_ = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application_.pApplicationName = "VectorXR Hardware Probe";
        application_.applicationVersion = 1;
        application_.pEngineName = "VectorXR Probe";
        application_.engineVersion = 1;
        application_.apiVersion = options_.vulkan_api;
        messenger_info_ = MessengerInfo();
        validation_features_ = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation_features_.pNext = &messenger_info_;
        validation_features_.enabledValidationFeatureCount = 1;
        validation_features_.pEnabledValidationFeatures = &kSynchronizationValidation;
        instance_info_ = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        if (options_.vulkan_validation) {
            instance_info_.pNext = offline ? static_cast<const void*>(&validation_features_) : &messenger_info_;
        }
        instance_info_.pApplicationInfo = &application_;
        instance_info_.enabledLayerCount = static_cast<uint32_t>(layers_.size());
        instance_info_.ppEnabledLayerNames = layers_.data();
        instance_info_.enabledExtensionCount = static_cast<uint32_t>(instance_extension_names_.size());
        instance_info_.ppEnabledExtensionNames = instance_extension_names_.data();
        return instance_info_;
    }

    void SetInstance(VkInstance instance) {
        instance_ = instance;
#define PROBE_VK_LOAD(name)                                                                                         \
    vk_.name = reinterpret_cast<PFN_vk##name>(vk_.GetInstanceProcAddr(instance_, "vk" #name));                     \
    if (vk_.name == nullptr) {                                                                                      \
        throw std::runtime_error("the Vulkan instance lacks vk" #name);                                            \
    }
        PROBE_VK_INSTANCE_FUNCTIONS(PROBE_VK_LOAD)
#undef PROBE_VK_LOAD
        if (ApiAtLeast(options_.vulkan_api, VK_API_VERSION_1_1)) {
            vk_.GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vk_.GetInstanceProcAddr(instance_, "vkGetPhysicalDeviceProperties2"));
        }
        if (options_.vulkan_validation) {
            vk_.CreateDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vk_.GetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
            vk_.DestroyDebugUtilsMessengerEXT = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vk_.GetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (vk_.CreateDebugUtilsMessengerEXT == nullptr || vk_.DestroyDebugUtilsMessengerEXT == nullptr) {
                throw std::runtime_error("--vulkan-validation: the instance lacks VK_EXT_debug_utils");
            }
            const VkDebugUtilsMessengerCreateInfoEXT info = MessengerInfo();
            CheckVk(vk_.CreateDebugUtilsMessengerEXT(instance_, &info, nullptr, &messenger_),
                    "vkCreateDebugUtilsMessengerEXT");
        }
    }

    // The runtime's (or the first discrete) GPU: prints it, checks --vulkan-api, picks a graphics and compute queue.
    void SetPhysicalDevice(VkPhysicalDevice physical) {
        physical_ = physical;
        VkPhysicalDeviceProperties properties{};
        vk_.GetPhysicalDeviceProperties(physical_, &properties);
        std::ostringstream line;
        line << "Vulkan device: " << properties.deviceName << ", API " << ApiText(properties.apiVersion) << "."
             << VK_API_VERSION_PATCH(properties.apiVersion) << ", driver 0x" << std::hex << properties.driverVersion
             << std::dec;
        if (vk_.GetPhysicalDeviceProperties2 != nullptr && ApiAtLeast(properties.apiVersion, VK_API_VERSION_1_1)) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties2.pNext = &id;
            vk_.GetPhysicalDeviceProperties2(physical_, &properties2);
            if (id.deviceLUIDValid) {
                LUID luid{};
                std::memcpy(&luid, id.deviceLUID, sizeof(luid));
                line << ", LUID " << std::hex << std::setfill('0') << std::setw(8)
                     << static_cast<uint32_t>(luid.HighPart) << ":" << std::setw(8) << luid.LowPart << std::dec;
            }
        }
        std::cout << line.str() << "\n";
        if (!ApiAtLeast(properties.apiVersion, options_.vulkan_api)) {
            throw std::runtime_error("the Vulkan device offers API " + ApiText(properties.apiVersion) +
                                     ", below --vulkan-api " + ApiText(options_.vulkan_api));
        }
        timestamp_period_ = properties.limits.timestampPeriod;

        uint32_t count = 0;
        vk_.GetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vk_.GetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
        constexpr VkQueueFlags kWanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        family_ = UINT32_MAX;
        for (uint32_t family = 0; family < count; ++family) {
            if ((families[family].queueFlags & kWanted) == kWanted && families[family].queueCount > 0) {
                family_ = family;
                timestamp_bits_ = families[family].timestampValidBits;
                break;
            }
        }
        if (family_ == UINT32_MAX) {
            throw std::runtime_error("the Vulkan device has no graphics and compute queue family");
        }
        vk_.GetPhysicalDeviceMemoryProperties(physical_, &memory_);
    }

    // One queue of the chosen family, `extensions` (the runtime's, with XR_KHR_vulkan_enable). Valid until the next
    // call.
    const VkDeviceCreateInfo& DeviceCreateInfo(const std::vector<std::string>& extensions) {
        device_extensions_ = extensions;
        device_extension_names_.clear();
        for (const std::string& name : device_extensions_) {
            device_extension_names_.push_back(name.c_str());
        }
        queue_info_ = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info_.queueFamilyIndex = family_;
        queue_info_.queueCount = 1;
        queue_info_.pQueuePriorities = &queue_priority_;
        device_info_ = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info_.queueCreateInfoCount = 1;
        device_info_.pQueueCreateInfos = &queue_info_;
        device_info_.enabledExtensionCount = static_cast<uint32_t>(device_extension_names_.size());
        device_info_.ppEnabledExtensionNames = device_extension_names_.data();
        if (options_.vk_test_stuck_queue) {
            // A timeline semaphore may be waited for before anything signals it (Vulkan 1.2).
            timeline_features_ = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            timeline_features_.timelineSemaphore = VK_TRUE;
            device_info_.pNext = &timeline_features_;
        }
        return device_info_;
    }

    void SetDevice(VkDevice device) {
        device_ = device;
#define PROBE_VK_LOAD(name)                                                                                         \
    vk_.name = reinterpret_cast<PFN_vk##name>(vk_.GetDeviceProcAddr(device_, "vk" #name));                         \
    if (vk_.name == nullptr) {                                                                                      \
        throw std::runtime_error("the Vulkan device lacks vk" #name);                                              \
    }
        PROBE_VK_DEVICE_FUNCTIONS(PROBE_VK_LOAD)
#undef PROBE_VK_LOAD
        vk_.GetDeviceQueue(device_, family_, 0, &queue_);
        for (Slot& slot : slots_) {
            VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = family_;
            CheckVk(vk_.CreateCommandPool(device_, &pool_info, nullptr, &slot.pool), "vkCreateCommandPool");
            const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            CheckVk(vk_.CreateFence(device_, &fence_info, nullptr, &slot.fence), "vkCreateFence");
        }
    }

    VkInstance Instance() const {
        return instance_;
    }
    VkPhysicalDevice PhysicalDevice() const {
        return physical_;
    }
    VkDevice Device() const {
        return device_;
    }
    VkQueue Queue() const {
        return queue_;
    }
    uint32_t QueueFamily() const {
        return family_;
    }
    uint32_t TimestampBits() const {
        return timestamp_bits_;
    }
    float TimestampPeriod() const {
        return timestamp_period_;
    }

    // A memory type in `type_bits`, one with all of `preferred` if there is one.
    uint32_t MemoryType(uint32_t type_bits, VkMemoryPropertyFlags preferred) const {
        for (uint32_t type = 0; type < memory_.memoryTypeCount; ++type) {
            if ((type_bits & (1u << type)) != 0 && (memory_.memoryTypes[type].propertyFlags & preferred) == preferred) {
                return type;
            }
        }
        for (uint32_t type = 0; type < memory_.memoryTypeCount; ++type) {
            if ((type_bits & (1u << type)) != 0) {
                return type;
            }
        }
        throw std::runtime_error("no Vulkan memory type fits");
    }

    void Submit(const std::vector<VkCommandBuffer>& buffers, VkFence fence) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = static_cast<uint32_t>(buffers.size());
        submit.pCommandBuffers = buffers.data();
        // No command buffers: only the fence, signalled once everything submitted before has run.
        CheckVk(vk_.QueueSubmit(queue_, buffers.empty() ? 0 : 1, buffers.empty() ? nullptr : &submit, fence),
                "vkQueueSubmit");
        if (!buffers.empty()) {
            ++submissions_;
        }
    }

    // Records with `record` into a command buffer of its own, submits it and waits (set-up, calibration, the
    // render check's transitions and read-backs). Not within a frame.
    template <typename Recorder>
    void RunOnce(const Recorder& record) {
        if (once_pool_ == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = family_;
            CheckVk(vk_.CreateCommandPool(device_, &pool_info, nullptr, &once_pool_), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo buffer_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            buffer_info.commandPool = once_pool_;
            buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffer_info.commandBufferCount = 1;
            CheckVk(vk_.AllocateCommandBuffers(device_, &buffer_info, &once_buffer_), "vkAllocateCommandBuffers");
            const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            CheckVk(vk_.CreateFence(device_, &fence_info, nullptr, &once_fence_), "vkCreateFence");
        }
        CheckVk(vk_.ResetCommandPool(device_, once_pool_, 0), "vkResetCommandPool");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(vk_.BeginCommandBuffer(once_buffer_, &begin), "vkBeginCommandBuffer");
        record(once_buffer_);
        CheckVk(vk_.EndCommandBuffer(once_buffer_), "vkEndCommandBuffer");
        CheckVk(vk_.ResetFences(device_, 1, &once_fence_), "vkResetFences");
        Submit({once_buffer_}, once_fence_);
        WaitFence(once_fence_, "a one-off submission");
    }

    // Without OpenXR (--gpu-calibrate, --render-check): an instance, the first discrete GPU (else the first one that
    // is not a CPU implementation) and a device of the probe's own.
    void CreateStandalone() {
        VkInstance instance = VK_NULL_HANDLE;
        const VkResult created = vk_.CreateInstance(&InstanceCreateInfo({}, true), nullptr, &instance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER) {
            throw Unavailable("vkCreateInstance: VK_ERROR_INCOMPATIBLE_DRIVER (no Vulkan driver)");
        }
        CheckVk(created, "vkCreateInstance");
        SetInstance(instance);
        uint32_t count = 0;
        CheckVk(vk_.EnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> physicals(count);
        CheckVk(vk_.EnumeratePhysicalDevices(instance_, &count, physicals.data()), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDeviceType> types;
        for (const VkPhysicalDevice candidate : physicals) {
            VkPhysicalDeviceProperties properties{};
            vk_.GetPhysicalDeviceProperties(candidate, &properties);
            types.push_back(properties.deviceType);
        }
        const std::optional<std::size_t> chosen = StandaloneDeviceIndex(types);
        if (!chosen) {
            throw Unavailable("no Vulkan device that is not a CPU implementation");
        }
        SetPhysicalDevice(physicals[*chosen]);
        VkDevice device = VK_NULL_HANDLE;
        CheckVk(vk_.CreateDevice(physical_, &DeviceCreateInfo({}), nullptr, &device), "vkCreateDevice");
        SetDevice(device);
    }

    void WaitFence(VkFence fence, std::string_view what) {
        constexpr uint64_t kTimeoutNs = 10'000'000'000ull;
        const VkResult result = vk_.WaitForFences(device_, 1, &fence, VK_TRUE, kTimeoutNs);
        if (result == VK_TIMEOUT) {
            throw std::runtime_error(std::string(what) + " did not finish on the GPU within 10 s");
        }
        CheckVk(result, "vkWaitForFences");
    }

    // A frame begins: its slot's command buffers are reused once the GPU has run them (a wait is counted).
    void BeginFrame() {
        Slot& slot = slots_[frame_ % slots_.size()];
        if (slot.fenced) {
            if (vk_.GetFenceStatus(device_, slot.fence) != VK_SUCCESS) {
                const auto begun = std::chrono::steady_clock::now();
                WaitFence(slot.fence, "a frame's rendering");
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
                ++slot_waits_;
                slot_wait_max_ms_ = std::max(slot_wait_max_ms_, ms);
            }
            CheckVk(vk_.ResetFences(device_, 1, &slot.fence), "vkResetFences");
            slot.fenced = false;
        }
        CheckVk(vk_.ResetCommandPool(device_, slot.pool, 0), "vkResetCommandPool");
        slot.used = 0;
        in_frame_ = true;
    }

    // The command buffer the frame records into now (the GPU load's share, then the image's views).
    VkCommandBuffer Recording() {
        if (!in_frame_) {
            throw std::runtime_error("Vulkan rendering recorded outside a frame");
        }
        if (recording_ != VK_NULL_HANDLE) {
            return recording_;
        }
        Slot& slot = slots_[frame_ % slots_.size()];
        if (slot.used == slot.buffers.size()) {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = slot.pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VkCommandBuffer buffer = VK_NULL_HANDLE;
            CheckVk(vk_.AllocateCommandBuffers(device_, &info, &buffer), "vkAllocateCommandBuffers");
            slot.buffers.push_back(buffer);
        }
        recording_ = slot.buffers[slot.used++];
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CheckVk(vk_.BeginCommandBuffer(recording_, &begin), "vkBeginCommandBuffer");
        return recording_;
    }

    // The image's views were recorded and it is about to be released: its rendering is submitted now, before the
    // release (OpenXR's rule), or with --vk-late-submit only at the end of the frame, after every release.
    void FinishImage() {
        if (recording_ == VK_NULL_HANDLE) {
            return;
        }
        CheckVk(vk_.EndCommandBuffer(recording_), "vkEndCommandBuffer");
        if (options_.vk_late_submit) {
            late_.push_back(recording_);
        } else {
            Submit({recording_}, VK_NULL_HANDLE);
        }
        recording_ = VK_NULL_HANDLE;
    }

    // Everything recorded so far reaches the queue.
    void Flush() {
        if (recording_ != VK_NULL_HANDLE) {
            CheckVk(vk_.EndCommandBuffer(recording_), "vkEndCommandBuffer");
            late_.push_back(recording_);
            recording_ = VK_NULL_HANDLE;
        }
        if (!late_.empty()) {
            Submit(late_, VK_NULL_HANDLE);
            late_.clear();
        }
    }

    // After the frame's last release, before xrEndFrame: the late rendering, then the slot's fence.
    void EndFrame() {
        Flush();
        Slot& slot = slots_[frame_ % slots_.size()];
        Submit({}, slot.fence);
        slot.fenced = true;
        ++frame_;
        in_frame_ = false;
    }

    // Waits until the queue has run everything (the runtime's work on it included), at most 10 s: an empty batch's
    // fence covers all earlier work on the queue. Called on the render thread between frames, when no OpenXR call
    // that may use the queue is in flight, and from destructors: it never throws. False when the queue did not
    // finish (a wait on the bridge's 'done' semaphore that never ends): the caller keeps what the queue may still
    // use, and every later wait returns false at once.
    bool WaitIdle(std::string_view before) {
        if (queue_ == VK_NULL_HANDLE) {
            return true;
        }
        if (idle_stuck_) {
            return false;
        }
        if (idle_fence_ == VK_NULL_HANDLE) {
            const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vk_.CreateFence(device_, &fence_info, nullptr, &idle_fence_) != VK_SUCCESS) {
                idle_fence_ = VK_NULL_HANDLE;
                return vk_.QueueWaitIdle(queue_) == VK_SUCCESS;
            }
        }
        constexpr uint64_t kTimeoutNs = 10'000'000'000ull;
        VkResult result = vk_.ResetFences(device_, 1, &idle_fence_);
        if (result == VK_SUCCESS) {
            result = vk_.QueueSubmit(queue_, 0, nullptr, idle_fence_);
        }
        if (result == VK_SUCCESS) {
            result = vk_.WaitForFences(device_, 1, &idle_fence_, VK_TRUE, kTimeoutNs);
        }
        if (result == VK_TIMEOUT) {
            std::cout << "Vulkan: the queue did not finish within 10 s before " << before
                      << " (a wait on the bridge's 'done' semaphore that never ended?); what it may still use is "
                         "kept\n";
            idle_stuck_ = true;
            return false;
        }
        if (result != VK_SUCCESS) {
            std::cout << "Vulkan: waiting for the queue to go idle failed: VkResult " << static_cast<int32_t>(result)
                      << "\n";
        }
        return true;
    }

    // Returns validation's errors and warnings (0 without --vulkan-validation), and 1 for a queue that never went
    // idle. Messages after it (the instance's destruction) are printed by the destructor.
    uint64_t Report() {
        std::ostringstream line;
        line << std::fixed << std::setprecision(2) << "Vulkan: " << submissions_ << " submissions"
             << (options_.vk_late_submit ? " (each frame's after its last release)" : "") << ", " << slot_waits_
             << " waits for a frame's command buffers";
        if (slot_waits_ > 0) {
            line << " (max " << slot_wait_max_ms_ << " ms)";
        }
        std::cout << line.str() << "\n";
        const uint64_t stuck = idle_stuck_ ? 1 : 0;
        if (idle_stuck_) {
            std::cout << "Vulkan: the queue never went idle; the objects it may still use were not destroyed\n";
        }
        if (!options_.vulkan_validation) {
            return stuck;
        }
        std::scoped_lock lock(messages_.mutex);
        for (const auto& [error, message] : messages_.first) {
            std::cout << "Vulkan validation [" << (error ? "error" : "warning") << "]: " << message << "\n";
        }
        std::cout << "Vulkan validation: " << messages_.errors << " errors, " << messages_.warnings << " warnings"
                  << (layer_path_.empty() ? std::string() : " (layer from " + layer_path_ + ")") << "\n";
        messages_.reported = true;
        messages_.reported_errors = messages_.errors;
        messages_.reported_warnings = messages_.warnings;
        return messages_.errors + messages_.warnings + stuck;
    }

private:
    struct Slot {
        VkCommandPool pool{VK_NULL_HANDLE};
        std::vector<VkCommandBuffer> buffers;
        std::size_t used{0};
        VkFence fence{VK_NULL_HANDLE};
        bool fenced{false}; // submitted; signalled once the slot's frame has run
    };

    // The validation layer's manifest: VK_ADD_LAYER_PATH when set, else the one the build writes beside the tests.
    void UseValidationLayerPath() {
        std::array<char, 4096> existing{};
        const DWORD length =
            GetEnvironmentVariableA("VK_ADD_LAYER_PATH", existing.data(), static_cast<DWORD>(existing.size()));
        if (length > 0 && length < existing.size()) {
            layer_path_ = existing.data();
            return;
        }
#ifdef VECTORXR_PROBE_VULKAN_LAYER_DIR
        const std::string directory = VECTORXR_PROBE_VULKAN_LAYER_DIR;
        if (!directory.empty() &&
            GetFileAttributesA((directory + "\\VkLayer_khronos_validation.json").c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetEnvironmentVariableA("VK_ADD_LAYER_PATH", directory.c_str());
            layer_path_ = directory;
        }
#endif
    }

    // Validation's messages after Report (the instance's destruction), so that none goes unseen.
    void ReportLate() {
        if (!options_.vulkan_validation) {
            return;
        }
        std::scoped_lock lock(messages_.mutex);
        if (!messages_.reported ||
            (messages_.errors == messages_.reported_errors && messages_.warnings == messages_.reported_warnings)) {
            return;
        }
        for (const auto& [error, message] : messages_.late) {
            std::cout << "Vulkan validation [" << (error ? "error" : "warning") << "] (teardown): " << message << "\n";
        }
        std::cout << "Vulkan validation during teardown, after the summary: "
                  << messages_.errors - messages_.reported_errors << " errors, "
                  << messages_.warnings - messages_.reported_warnings << " warnings\n";
    }

    void RequireValidationLayer() {
        uint32_t count = 0;
        CheckVk(vk_.EnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties");
        std::vector<VkLayerProperties> layers(count);
        CheckVk(vk_.EnumerateInstanceLayerProperties(&count, layers.data()), "vkEnumerateInstanceLayerProperties");
        const bool found = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (!found) {
            throw Unavailable("--vulkan-validation: VK_LAYER_KHRONOS_validation was not found; set "
                              "VK_ADD_LAYER_PATH to the folder of its manifest (VK_ADD_LAYER_PATH=" +
                              (layer_path_.empty() ? std::string("unset") : layer_path_) + ")");
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT MessengerInfo() {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = OnVulkanMessage;
        info.pUserData = &messages_;
        return info;
    }

    Options options_;
    HMODULE loader_{nullptr};
    VulkanFunctions vk_;
    std::string layer_path_;
    VulkanMessages messages_; // outlives the instance: the instance's own messenger reports its destruction
    std::vector<std::string> instance_extensions_;
    std::vector<const char*> instance_extension_names_;
    std::vector<const char*> layers_;
    static constexpr VkValidationFeatureEnableEXT kSynchronizationValidation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkApplicationInfo application_{};
    VkDebugUtilsMessengerCreateInfoEXT messenger_info_{};
    VkValidationFeaturesEXT validation_features_{};
    VkInstanceCreateInfo instance_info_{};
    std::vector<std::string> device_extensions_;
    std::vector<const char*> device_extension_names_;
    float queue_priority_{1.0f};
    VkDeviceQueueCreateInfo queue_info_{};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features_{}; // --vk-test-stuck-queue
    VkDeviceCreateInfo device_info_{};
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
    VkPhysicalDevice physical_{VK_NULL_HANDLE};
    VkPhysicalDeviceMemoryProperties memory_{};
    uint32_t family_{0};
    uint32_t timestamp_bits_{0};
    float timestamp_period_{1.0f};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    // Frames' command buffers, reused once the slot's fence says the GPU has run them.
    std::array<Slot, 8> slots_{};
    uint64_t frame_{0};
    bool in_frame_{false};
    VkCommandBuffer recording_{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> late_; // recorded, not yet submitted (--vk-late-submit)
    VkCommandPool once_pool_{VK_NULL_HANDLE};
    VkCommandBuffer once_buffer_{VK_NULL_HANDLE};
    VkFence once_fence_{VK_NULL_HANDLE};
    VkFence idle_fence_{VK_NULL_HANDLE}; // WaitIdle's
    bool idle_stuck_{false};             // a WaitIdle timed out
    uint64_t submissions_{0};
    uint64_t slot_waits_{0};
    double slot_wait_max_ms_{0.0};
};

// The GPU load on Vulkan: the SPIR-V compute shader, a storage buffer, timestamp queries and a fence per ring slot.
// A view's share is recorded into the command buffer the view renders with, so it is submitted with the view (before
// its release, or after the frame's last one with --vk-late-submit).
class VulkanGpuLoadBackend final : public GpuLoadBackend {
public:
    explicit VulkanGpuLoadBackend(VulkanDevice& device) : device_(device), vk_(device.vk()) {}

    ~VulkanGpuLoadBackend() override {
        const VkDevice device = device_.Device();
        if (!device_.WaitIdle("the GPU load is destroyed")) {
            return; // the stuck queue may still use it
        }
        for (const VkFence fence : fences_) {
            if (fence != VK_NULL_HANDLE) {
                vk_.DestroyFence(device, fence, nullptr);
            }
        }
        if (queries_ != VK_NULL_HANDLE) {
            vk_.DestroyQueryPool(device, queries_, nullptr);
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vk_.DestroyPipeline(device, pipeline_, nullptr);
        }
        if (pipeline_layout_ != VK_NULL_HANDLE) {
            vk_.DestroyPipelineLayout(device, pipeline_layout_, nullptr);
        }
        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vk_.DestroyDescriptorPool(device, descriptor_pool_, nullptr);
        }
        if (set_layout_ != VK_NULL_HANDLE) {
            vk_.DestroyDescriptorSetLayout(device, set_layout_, nullptr);
        }
        if (shader_ != VK_NULL_HANDLE) {
            vk_.DestroyShaderModule(device, shader_, nullptr);
        }
        if (values_ != VK_NULL_HANDLE) {
            vk_.DestroyBuffer(device, values_, nullptr);
        }
        if (memory_ != VK_NULL_HANDLE) {
            vk_.FreeMemory(device, memory_, nullptr);
        }
    }

    // Separate from the constructor, so that what was made is destroyed when a later step fails.
    void Initialize() {
#ifndef VECTORXR_PROBE_SPIRV
        throw std::runtime_error("--gpu-ms with --graphics vulkan needs the probe built with SPIR-V (the pinned DXC)");
#else
        if (device_.TimestampBits() == 0) {
            throw std::runtime_error("--gpu-ms: the Vulkan queue family has no timestamps");
        }
        const VkDevice device = device_.Device();

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = kLoadThreads * sizeof(float);
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVk(vk_.CreateBuffer(device, &buffer_info, nullptr, &values_), "vkCreateBuffer(GPU load)");
        VkMemoryRequirements requirements{};
        vk_.GetBufferMemoryRequirements(device, values_, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = device_.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        CheckVk(vk_.AllocateMemory(device, &allocate, nullptr, &memory_), "vkAllocateMemory(GPU load)");
        CheckVk(vk_.BindBufferMemory(device, values_, memory_, 0), "vkBindBufferMemory(GPU load)");

        // DXC's array is bytes: copied into words for pCode's alignment.
        std::vector<uint32_t> words((sizeof(kProbeLoadSpirv) + 3) / 4);
        std::memcpy(words.data(), kProbeLoadSpirv, sizeof(kProbeLoadSpirv));
        VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader_info.codeSize = sizeof(kProbeLoadSpirv);
        shader_info.pCode = words.data();
        CheckVk(vk_.CreateShaderModule(device, &shader_info, nullptr, &shader_), "vkCreateShaderModule(GPU load)");

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo set_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_layout_info.bindingCount = 1;
        set_layout_info.pBindings = &binding;
        CheckVk(vk_.CreateDescriptorSetLayout(device, &set_layout_info, nullptr, &set_layout_),
                "vkCreateDescriptorSetLayout(GPU load)");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        CheckVk(vk_.CreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_),
                "vkCreatePipelineLayout(GPU load)");
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = shader_;
        pipeline_info.stage.pName = "CSMain";
        pipeline_info.layout = pipeline_layout_;
        CheckVk(vk_.CreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_),
                "vkCreateComputePipelines(GPU load)");

        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        CheckVk(vk_.CreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_),
                "vkCreateDescriptorPool(GPU load)");
        VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_info.descriptorPool = descriptor_pool_;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &set_layout_;
        CheckVk(vk_.AllocateDescriptorSets(device, &set_info, &set_), "vkAllocateDescriptorSets(GPU load)");
        const VkDescriptorBufferInfo described{values_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set_;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &described;
        vk_.UpdateDescriptorSets(device, 1, &write, 0, nullptr);

        VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = static_cast<uint32_t>((kRing + 1) * kMaximumViews * 2);
        CheckVk(vk_.CreateQueryPool(device, &query_info, nullptr, &queries_), "vkCreateQueryPool(GPU load)");

        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (VkFence& fence : fences_) {
            CheckVk(vk_.CreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence(GPU load)");
        }

        // FLT_MAX everywhere: the first dispatch seeds every value (1 + id * 1e-6), as Direct3D 11's initial data.
        device_.RunOnce([&](VkCommandBuffer commands) {
            vk_.CmdFillBuffer(commands, values_, 0, VK_WHOLE_SIZE, 0x7F7FFFFFu);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vk_.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                   &barrier, 0, nullptr, 0, nullptr);
        });
#endif
    }

    void Warm(uint32_t iterations, int count) override {
        device_.RunOnce([&](VkCommandBuffer commands) {
            for (int i = 0; i < count; ++i) {
                Record(commands, iterations, 1, std::nullopt);
            }
        });
    }

    std::optional<double> TimeViews(uint32_t views, uint32_t iterations, uint32_t chunks) override {
        views = std::min(views, kMaximumViews);
        device_.RunOnce([&](VkCommandBuffer commands) {
            for (uint32_t view = 0; view < views; ++view) {
                Record(commands, iterations, chunks, Query(kRing, view));
            }
        });
        double ms = 0.0;
        for (uint32_t view = 0; view < views; ++view) {
            ms += ReadQuery(Query(kRing, view));
        }
        return ms;
    }

    void BeginSlot(std::size_t) override {}

    void RecordTimed(std::size_t slot, uint32_t view, uint32_t iterations, uint32_t chunks) override {
        Record(device_.Recording(), iterations, chunks, Query(slot, view));
    }

    void SignalFrame(std::size_t slot) override {
        device_.Flush();
        if (fenced_[slot]) {
            CheckVk(vk_.ResetFences(device_.Device(), 1, &fences_[slot]), "vkResetFences(GPU load)");
        }
        device_.Submit({}, fences_[slot]);
        fenced_[slot] = true;
    }

    bool FrameDone(std::size_t slot, bool) override {
        if (!fenced_[slot]) {
            return true;
        }
        const VkResult result = vk_.GetFenceStatus(device_.Device(), fences_[slot]);
        if (result == VK_NOT_READY) {
            return false;
        }
        CheckVk(result, "vkGetFenceStatus(GPU load)");
        return true;
    }

    void WaitFrame(std::size_t slot) override {
        device_.WaitFence(fences_[slot], "the GPU load's frame");
    }

    std::optional<double> ReadView(std::size_t slot, uint32_t view) override {
        return ReadQuery(Query(slot, view));
    }

private:
    static uint32_t Query(std::size_t slot, uint32_t view) {
        return static_cast<uint32_t>((slot * kMaximumViews + view) * 2);
    }

    // `chunks` dispatches of `iterations` each (--gpu-chunks), timed together by queries `query` and `query + 1`
    // when given.
    void Record(VkCommandBuffer commands, uint32_t iterations, uint32_t chunks, std::optional<uint32_t> query) {
        if (query) {
            vk_.CmdResetQueryPool(commands, queries_, *query, 2);
        }
        // The values carry over from dispatch to dispatch (Direct3D 11 orders such dispatches itself). Waiting for
        // all earlier work also starts the timer when the dispatch can start.
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vk_.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               1, &barrier, 0, nullptr, 0, nullptr);
        if (query) {
            // Bottom of pipe: written once all earlier commands have completed.
            vk_.CmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, *query);
        }
        vk_.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
        vk_.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &set_, 0,
                                  nullptr);
        vk_.CmdPushConstants(commands, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(iterations),
                             &iterations);
        for (uint32_t chunk = 0; chunk < chunks; ++chunk) {
            if (chunk > 0) {
                // Each chunk continues from the previous one's values.
                vk_.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
            }
            vk_.CmdDispatch(commands, kLoadGroups, 1, 1);
        }
        if (query) {
            vk_.CmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, *query + 1);
        }
    }

    // A timed dispatch's span in ms (its work has run).
    double ReadQuery(uint32_t query) {
        std::array<uint64_t, 2> ticks{};
        CheckVk(vk_.GetQueryPoolResults(device_.Device(), queries_, query, 2, sizeof(ticks), ticks.data(),
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "vkGetQueryPoolResults(GPU load)");
        const uint32_t bits = device_.TimestampBits();
        const uint64_t mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        const uint64_t span = (ticks[1] - ticks[0]) & mask;
        return static_cast<double>(span) * static_cast<double>(device_.TimestampPeriod()) / 1.0e6;
    }

    VulkanDevice& device_;
    const VulkanFunctions& vk_;
    VkBuffer values_{VK_NULL_HANDLE};
    VkDeviceMemory memory_{VK_NULL_HANDLE};
    VkShaderModule shader_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_{VK_NULL_HANDLE};
    VkDescriptorSet set_{VK_NULL_HANDLE};
    VkQueryPool queries_{VK_NULL_HANDLE}; // per ring slot and view, then calibration's views
    std::array<VkFence, kRing> fences_{};
    std::array<bool, kRing> fenced_{};
};

std::unique_ptr<GpuLoad> CreateVulkanGpuLoad(VulkanDevice& device, const Options& options) {
    auto backend = std::make_unique<VulkanGpuLoadBackend>(device);
    backend->Initialize();
    return std::make_unique<GpuLoad>(std::move(backend), options);
}

template <typename Function>
Function XrFunction(XrInstance instance, const char* name) {
    Function function = nullptr;
    CheckXr(instance, xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function)),
            std::string("xrGetInstanceProcAddr(") + name + ")");
    return function;
}

class VulkanGraphics final : public ProbeGraphics {
public:
    explicit VulkanGraphics(const Options& options)
        : options_(options), device_(std::make_unique<VulkanDevice>(options)) {}

    ~VulkanGraphics() override {
        RemoveSwapchains();
        DestroyRenderPasses();
    }

    std::string Describe() const override {
        std::string text = std::string("Vulkan (") + InstanceExtension() + ", API " + ApiText(options_.vulkan_api);
        if (options_.vulkan_validation) {
            text += ", validation";
        }
        if (options_.vk_late_submit) {
            text += ", rendering submitted after the frame's last release";
        }
        return text + ")";
    }

    const char* InstanceExtension() const override {
        return options_.vulkan_enable == 1 ? XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
                                           : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    }

    void CreateDevice(XrInstance instance, XrSystemId system) override {
        if (options_.vulkan_enable == 1) {
            CreateDeviceEnable1(instance, system);
        } else {
            CreateDeviceEnable2(instance, system);
        }
        binding_.type = options_.vulkan_enable == 1 ? XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR
                                                    : XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
        binding_.instance = device_->Instance();
        binding_.physicalDevice = device_->PhysicalDevice();
        binding_.device = device_->Device();
        binding_.queueFamilyIndex = device_->QueueFamily();
        binding_.queueIndex = 0;
    }

    const void* Binding() const override {
        return &binding_;
    }

    std::vector<int64_t> ColorFormats() const override {
        return VulkanColorFormats();
    }

    int64_t DepthFormat() const override {
        return VK_FORMAT_D32_SFLOAT;
    }

    std::unique_ptr<GpuLoad> CreateGpuLoad(const Options& options) override {
        return CreateVulkanGpuLoad(*device_, options);
    }

    uint32_t AddSwapchain(XrInstance instance, XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                          SwapchainRole role) override {
        uint32_t image_count = 0;
        CheckXr(instance, xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr),
                "xrEnumerateSwapchainImages(count)");
        std::vector<XrSwapchainImageVulkanKHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        CheckXr(instance,
                xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                "xrEnumerateSwapchainImages");
        std::vector<uint64_t> handles;
        for (const XrSwapchainImageVulkanKHR& image : images) {
            handles.push_back(reinterpret_cast<uint64_t>(image.image));
        }
        return AddImages(handles, info, role);
    }

    uint32_t AddImages(const std::vector<uint64_t>& images, const XrSwapchainCreateInfo& info,
                       SwapchainRole role) override {
        sets_.push_back({});
        ImageSet& set = sets_.back();
        set.width = info.width;
        set.height = info.height;
        set.format = static_cast<VkFormat>(info.format);
        for (const uint64_t image : images) {
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = reinterpret_cast<VkImage>(image);
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = set.format;
            view_info.subresourceRange = {static_cast<VkImageAspectFlags>(role == SwapchainRole::Depth
                                                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                              : VK_IMAGE_ASPECT_COLOR_BIT),
                                          0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            CheckVk(device_->vk().CreateImageView(device_->Device(), &view_info, nullptr, &view),
                    "vkCreateImageView(swapchain image)");
            set.views.push_back(view);
        }
        return static_cast<uint32_t>(sets_.size() - 1);
    }

    uint32_t ImageCount(uint32_t id) const override {
        return static_cast<uint32_t>(sets_[id].views.size());
    }

    // The queue runs dry first: the views and framebuffers may still be in use by submitted frames. A queue that
    // does not finish within 10 s keeps them (they leak), so that xrDestroySwapchain and xrDestroySession still run.
    void RemoveSwapchains() override {
        if (sets_.empty() && framebuffers_.empty()) {
            return;
        }
        if (!device_->WaitIdle("the swapchains are destroyed")) {
            framebuffers_.clear();
            sets_.clear();
            return;
        }
        for (const auto& [key, framebuffer] : framebuffers_) {
            device_->vk().DestroyFramebuffer(device_->Device(), framebuffer, nullptr);
        }
        framebuffers_.clear();
        for (const ImageSet& set : sets_) {
            for (const VkImageView view : set.views) {
                device_->vk().DestroyImageView(device_->Device(), view, nullptr);
            }
        }
        sets_.clear();
    }

    void BeginFrame() override {
        device_->BeginFrame();
    }

    // A render pass over the view's rect: its load clears the view (and its depth), then the pattern's rects are
    // cleared in their colours (and, with a depth swapchain, at 2 m in depth).
    void Draw(const ViewDraw& draw) override {
        const VulkanFunctions& vk = device_->vk();
        const ImageSet& color = sets_[draw.color];
        const ImageSet* depth = draw.depth ? &sets_[*draw.depth] : nullptr;
        const VkRenderPass render_pass = RenderPass(color.format, depth ? depth->format : VK_FORMAT_UNDEFINED);
        const VkFramebuffer framebuffer = Framebuffer(draw, render_pass);
        const VkCommandBuffer commands = device_->Recording();

        std::array<VkClearValue, 2> clears{};
        const std::array<float, 4> clear_color = draw.quad ? kQuadBackground : ClearColor(draw.view % 2);
        std::copy(clear_color.begin(), clear_color.end(), clears[0].color.float32);
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = render_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea = ToVulkan(draw.rect, 0, 0);
        begin.clearValueCount = depth ? 2u : 1u;
        begin.pClearValues = clears.data();
        vk.CmdBeginRenderPass(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        if (draw.pattern || draw.quad) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const PatternKind kind =
                draw.quad ? PatternKind::Quad : (draw.view >= 2 ? PatternKind::Focus : PatternKind::Peripheral);
            const Pattern pattern = PatternRects(static_cast<uint32_t>(draw.rect.right - draw.rect.left),
                                                 static_cast<uint32_t>(draw.rect.bottom - draw.rect.top), kind, seconds);
            std::vector<PixelRect> all = pattern.grid;
            ClearRects(commands, VK_IMAGE_ASPECT_COLOR_BIT, pattern.grid_color, 0.0f, pattern.grid, draw.rect);
            ClearRects(commands, VK_IMAGE_ASPECT_COLOR_BIT, pattern.bar_color, 0.0f, {pattern.bar}, draw.rect);
            if (depth) {
                all.push_back(pattern.bar);
                ClearRects(commands, VK_IMAGE_ASPECT_DEPTH_BIT, {}, DepthAt(2.0f), all, draw.rect);
            }
        }
        vk.CmdEndRenderPass(commands);
    }

    void ImageDone() override {
        device_->FinishImage();
    }

    void EndFrame() override {
        device_->EndFrame();
    }

    void DestroyDevice() override {
        RemoveSwapchains();
        DestroyRenderPasses();
        device_->DestroyDevice();
    }

    uint64_t Report() override {
        return device_->Report();
    }

    // --render-check: a device of the probe's own, without OpenXR.
    VulkanDevice& CreateStandaloneDevice() {
        device_->CreateStandalone();
        return *device_;
    }

private:
    struct ImageSet {
        uint32_t width{0};
        uint32_t height{0};
        VkFormat format{VK_FORMAT_UNDEFINED};
        std::vector<VkImageView> views;
    };

    // Kept (leaked) when the queue never went idle: its frames may still use them.
    void DestroyRenderPasses() {
        if (!device_->Stuck() && device_->Device() != VK_NULL_HANDLE) {
            for (const auto& [formats, render_pass] : render_passes_) {
                device_->vk().DestroyRenderPass(device_->Device(), render_pass, nullptr);
            }
        }
        render_passes_.clear();
    }

    void CheckRequirements(const XrGraphicsRequirementsVulkanKHR& requirements) const {
        const uint32_t minimum = VulkanApiFromXr(requirements.minApiVersionSupported);
        const uint32_t maximum = VulkanApiFromXr(requirements.maxApiVersionSupported);
        std::cout << "Vulkan API " << ApiText(options_.vulkan_api) << " (the runtime supports " << ApiText(minimum)
                  << " to " << ApiText(maximum)
                  << (ApiAtLeast(maximum, options_.vulkan_api) ? "" : "; above its maximum") << ")\n";
        if (!ApiAtLeast(options_.vulkan_api, minimum)) {
            throw std::runtime_error("--vulkan-api " + ApiText(options_.vulkan_api) + " is below the runtime's minimum " +
                                     ApiText(minimum));
        }
    }

    // XR_KHR_vulkan_enable2: the runtime creates the instance and the device from the probe's create infos, adding
    // what it needs.
    void CreateDeviceEnable2(XrInstance instance, XrSystemId system) {
        XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        CheckXr(instance,
                XrFunction<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR")(
                    instance, system, &requirements),
                "xrGetVulkanGraphicsRequirements2KHR");
        CheckRequirements(requirements);

        XrVulkanInstanceCreateInfoKHR instance_info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        instance_info.systemId = system;
        instance_info.pfnGetInstanceProcAddr = device_->vk().GetInstanceProcAddr;
        instance_info.vulkanCreateInfo = &device_->InstanceCreateInfo({});
        VkInstance vulkan_instance = VK_NULL_HANDLE;
        VkResult vulkan_result = VK_SUCCESS;
        CheckXr(instance,
                XrFunction<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR")(
                    instance, &instance_info, &vulkan_instance, &vulkan_result),
                "xrCreateVulkanInstanceKHR");
        CheckVk(vulkan_result, "vkCreateInstance (xrCreateVulkanInstanceKHR)");
        device_->SetInstance(vulkan_instance);

        XrVulkanGraphicsDeviceGetInfoKHR get_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        get_info.systemId = system;
        get_info.vulkanInstance = vulkan_instance;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        CheckXr(instance,
                XrFunction<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR")(
                    instance, &get_info, &physical),
                "xrGetVulkanGraphicsDevice2KHR");
        device_->SetPhysicalDevice(physical);

        XrVulkanDeviceCreateInfoKHR device_info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        device_info.systemId = system;
        device_info.pfnGetInstanceProcAddr = device_->vk().GetInstanceProcAddr;
        device_info.vulkanPhysicalDevice = physical;
        device_info.vulkanCreateInfo = &device_->DeviceCreateInfo({});
        VkDevice vulkan_device = VK_NULL_HANDLE;
        CheckXr(instance,
                XrFunction<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR")(
                    instance, &device_info, &vulkan_device, &vulkan_result),
                "xrCreateVulkanDeviceKHR");
        CheckVk(vulkan_result, "vkCreateDevice (xrCreateVulkanDeviceKHR)");
        device_->SetDevice(vulkan_device);
    }

    // XR_KHR_vulkan_enable: the probe creates the instance and the device with the extensions the runtime names.
    void CreateDeviceEnable1(XrInstance instance, XrSystemId system) {
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        CheckXr(instance,
                XrFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(instance, "xrGetVulkanGraphicsRequirementsKHR")(
                    instance, system, &requirements),
                "xrGetVulkanGraphicsRequirementsKHR");
        CheckRequirements(requirements);

        const std::vector<std::string> instance_extensions = RuntimeExtensions(
            instance, system,
            XrFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(instance, "xrGetVulkanInstanceExtensionsKHR"),
            "xrGetVulkanInstanceExtensionsKHR");
        std::cout << "Vulkan instance extensions the runtime asks for: " << JoinNames(instance_extensions) << "\n";
        VkInstance vulkan_instance = VK_NULL_HANDLE;
        CheckVk(device_->vk().CreateInstance(&device_->InstanceCreateInfo(instance_extensions), nullptr,
                                             &vulkan_instance),
                "vkCreateInstance");
        device_->SetInstance(vulkan_instance);

        VkPhysicalDevice physical = VK_NULL_HANDLE;
        CheckXr(instance,
                XrFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(instance, "xrGetVulkanGraphicsDeviceKHR")(
                    instance, system, vulkan_instance, &physical),
                "xrGetVulkanGraphicsDeviceKHR");
        device_->SetPhysicalDevice(physical);

        const std::vector<std::string> device_extensions = RuntimeExtensions(
            instance, system, XrFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR"),
            "xrGetVulkanDeviceExtensionsKHR");
        std::cout << "Vulkan device extensions the runtime asks for: " << JoinNames(device_extensions) << "\n";
        VkDevice vulkan_device = VK_NULL_HANDLE;
        CheckVk(device_->vk().CreateDevice(physical, &device_->DeviceCreateInfo(device_extensions), nullptr,
                                           &vulkan_device),
                "vkCreateDevice");
        device_->SetDevice(vulkan_device);
    }

    static std::vector<std::string> RuntimeExtensions(XrInstance instance, XrSystemId system,
                                                      PFN_xrGetVulkanInstanceExtensionsKHR get, const char* name) {
        uint32_t size = 0;
        CheckXr(instance, get(instance, system, 0, &size, nullptr), std::string(name) + "(size)");
        std::string text(size, '\0');
        CheckXr(instance, get(instance, system, size, &size, text.data()), name);
        text.resize(std::strlen(text.c_str()));
        return SplitNames(text);
    }

    // Swapchain images stay in the layout OpenXR hands them over in (attachment-optimal), so the render pass neither
    // transitions them nor discards the other half of a side-by-side image. The dependency orders this view's
    // writes after the earlier ones to the same image (the other eye's, or a previous frame's).
    VkRenderPass RenderPass(VkFormat color, VkFormat depth) {
        const auto key = std::make_pair(color, depth);
        if (const auto found = render_passes_.find(key); found != render_passes_.end()) {
            return found->second;
        }
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = color;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1] = attachments[0];
        attachments[1].format = depth;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        subpass.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &depth_reference : nullptr;
        constexpr VkPipelineStageFlags kStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = kStages;
        dependency.dstStageMask = kStages;
        dependency.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = depth != VK_FORMAT_UNDEFINED ? 2u : 1u;
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        VkRenderPass render_pass = VK_NULL_HANDLE;
        CheckVk(device_->vk().CreateRenderPass(device_->Device(), &info, nullptr, &render_pass), "vkCreateRenderPass");
        render_passes_.emplace(key, render_pass);
        return render_pass;
    }

    // One framebuffer per colour image and depth image pair, made on first use.
    VkFramebuffer Framebuffer(const ViewDraw& draw, VkRenderPass render_pass) {
        const auto key = std::make_tuple(draw.color, draw.color_image, draw.depth.value_or(UINT32_MAX),
                                         draw.depth ? draw.depth_image : 0u);
        if (const auto found = framebuffers_.find(key); found != framebuffers_.end()) {
            return found->second;
        }
        const ImageSet& color = sets_[draw.color];
        std::array<VkImageView, 2> views{color.views[draw.color_image], VK_NULL_HANDLE};
        if (draw.depth) {
            views[1] = sets_[*draw.depth].views[draw.depth_image];
        }
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = render_pass;
        info.attachmentCount = draw.depth ? 2u : 1u;
        info.pAttachments = views.data();
        info.width = color.width;
        info.height = color.height;
        info.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        CheckVk(device_->vk().CreateFramebuffer(device_->Device(), &info, nullptr, &framebuffer),
                "vkCreateFramebuffer");
        framebuffers_.emplace(key, framebuffer);
        return framebuffer;
    }

    // vkCmdClearAttachments over `rects` (in the view's own pixels; `view` places them in the image).
    void ClearRects(VkCommandBuffer commands, VkImageAspectFlags aspect, const std::array<float, 4>& color,
                    float depth, const std::vector<PixelRect>& rects, const PixelRect& view) {
        if (rects.empty()) {
            return;
        }
        VkClearAttachment attachment{};
        attachment.aspectMask = aspect;
        attachment.colorAttachment = 0;
        if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            std::copy(color.begin(), color.end(), attachment.clearValue.color.float32);
        } else {
            attachment.clearValue.depthStencil = {depth, 0};
        }
        std::vector<VkClearRect> clear_rects;
        for (const PixelRect& rect : rects) {
            clear_rects.push_back({ToVulkan(rect, view.left, view.top), 0, 1});
        }
        device_->vk().CmdClearAttachments(commands, 1, &attachment, static_cast<uint32_t>(clear_rects.size()),
                                          clear_rects.data());
    }

    Options options_;
    std::unique_ptr<VulkanDevice> device_;
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    std::vector<ImageSet> sets_;
    std::map<std::pair<VkFormat, VkFormat>, VkRenderPass> render_passes_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, VkFramebuffer> framebuffers_;
};

} // namespace

std::unique_ptr<ProbeGraphics> CreateVulkanGraphics(const Options& options) {
    return std::make_unique<VulkanGraphics>(options);
}

// With --vulkan-validation it also fails on validation's errors and warnings (synchronization validation included).
bool RunVulkanGpuCalibration(const Options& options) {
    VulkanDevice device(options);
    device.CreateStandalone();
    bool within = false;
    {
        const std::unique_ptr<GpuLoad> load = CreateVulkanGpuLoad(device, options);
        within = RunGpuCalibrationLoop(*load, options, [&] { device.BeginFrame(); }, [&] { device.EndFrame(); });
    }
    device.DestroyDevice(); // validation reports what is left at vkDestroyDevice before the summary
    return device.Report() == 0 && within;
}

namespace {

// --render-check on Vulkan: images as a runtime would hand them over (attachment-optimal, owned by the queue's
// family), read back through a buffer copy.
class VulkanCheckImages final : public CheckImages {
public:
    explicit VulkanCheckImages(const Options& options)
        : graphics_(options), stall_queue_(options.vk_test_stuck_queue) {
        device_ = &graphics_.CreateStandaloneDevice();
    }

    ~VulkanCheckImages() override {
        Remove();
        ReleaseStuckQueue();
    }

    ProbeGraphics& Graphics() override {
        return graphics_;
    }

    uint32_t Add(const XrSwapchainCreateInfo& info, SwapchainRole role, uint32_t count) override {
        const VulkanFunctions& vk = device_->vk();
        const bool depth = role == SwapchainRole::Depth;
        Set set;
        set.width = info.width;
        set.height = info.height;
        for (uint32_t index = 0; index < count; ++index) {
            VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = static_cast<VkFormat>(info.format);
            image_info.extent = {info.width, info.height, 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                      : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            Image image;
            CheckVk(vk.CreateImage(device_->Device(), &image_info, nullptr, &image.image), "vkCreateImage(check)");
            set.images.push_back(image);
            VkMemoryRequirements requirements{};
            vk.GetImageMemoryRequirements(device_->Device(), image.image, &requirements);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex =
                device_->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            CheckVk(vk.AllocateMemory(device_->Device(), &allocate, nullptr, &set.images.back().memory),
                    "vkAllocateMemory(check)");
            CheckVk(vk.BindImageMemory(device_->Device(), image.image, set.images.back().memory, 0),
                    "vkBindImageMemory(check)");
        }
        // Into the layout xrWaitSwapchainImage hands images over in.
        device_->RunOnce([&](VkCommandBuffer commands) {
            for (const Image& image : set.images) {
                Barrier(commands, image.image, depth, VK_IMAGE_LAYOUT_UNDEFINED, AttachmentLayout(depth), 0);
            }
        });
        std::vector<uint64_t> handles;
        for (const Image& image : set.images) {
            handles.push_back(reinterpret_cast<uint64_t>(image.image));
        }
        set.depth = depth;
        const uint32_t id = graphics_.AddImages(handles, info, role);
        if (sets_.size() <= id) {
            sets_.resize(id + 1);
        }
        sets_[id] = std::move(set);
        return id;
    }

    std::vector<uint8_t> ColorPixels(uint32_t id, uint32_t image) override {
        return Read(id, image);
    }

    std::optional<std::vector<float>> DepthPixels(uint32_t id, uint32_t image) override {
        const std::vector<uint8_t> bytes = Read(id, image);
        std::vector<float> depth(bytes.size() / sizeof(float));
        std::memcpy(depth.data(), bytes.data(), depth.size() * sizeof(float));
        return depth;
    }

    void Remove() override {
        if (stall_queue_ && !sets_.empty()) {
            stall_queue_ = false;
            StallQueue();
        }
        graphics_.RemoveSwapchains();
        if (!device_->Stuck()) {
            for (const Set& set : sets_) {
                for (const Image& image : set.images) {
                    device_->vk().DestroyImage(device_->Device(), image.image, nullptr);
                    device_->vk().FreeMemory(device_->Device(), image.memory, nullptr);
                }
            }
        }
        sets_.clear();
    }

private:
    struct Image {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
    };

    struct Set {
        uint32_t width{0};
        uint32_t height{0};
        bool depth{false};
        std::vector<Image> images;
    };

    // --vk-test-stuck-queue: the queue waits for a timeline semaphore value that is never signalled, as it does for a
    // bridge whose D3D12 'done' fence never signals; the semaphore leaks with the queue.
    void StallQueue() {
        const auto create_semaphore = reinterpret_cast<PFN_vkCreateSemaphore>(
            device_->vk().GetDeviceProcAddr(device_->Device(), "vkCreateSemaphore"));
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        VkSemaphore never = VK_NULL_HANDLE;
        CheckVk(create_semaphore(device_->Device(), &info, nullptr, &never), "vkCreateSemaphore(stuck queue)");
        const uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo values{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        values.waitSemaphoreValueCount = 1;
        values.pWaitSemaphoreValues = &value;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &values;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &never;
        submit.pWaitDstStageMask = &stage;
        CheckVk(device_->vk().QueueSubmit(device_->Queue(), 1, &submit, VK_NULL_HANDLE),
                "vkQueueSubmit(stuck queue)");
        stuck_ = never;
        std::cout << "Render check: --vk-test-stuck-queue holds the queue on a semaphore that is never signalled\n";
    }

    // Once the run has shown its bounded waits, the semaphore is signalled from the CPU and the queue runs on: the
    // process never exits with a queue waiting on the GPU (what is left is freed with the process). Never throws.
    void ReleaseStuckQueue() {
        if (stuck_ == VK_NULL_HANDLE) {
            return;
        }
        const auto signal = reinterpret_cast<PFN_vkSignalSemaphore>(
            device_->vk().GetDeviceProcAddr(device_->Device(), "vkSignalSemaphore"));
        VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        info.semaphore = stuck_;
        info.value = 1;
        if (signal != nullptr && signal(device_->Device(), &info) == VK_SUCCESS &&
            device_->vk().QueueWaitIdle(device_->Queue()) == VK_SUCCESS) {
            std::cout << "Render check: --vk-test-stuck-queue's semaphore signalled from the CPU; the queue ran on\n";
        }
        stuck_ = VK_NULL_HANDLE;
    }

    static VkImageLayout AttachmentLayout(bool depth) {
        return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    void Barrier(VkCommandBuffer commands, VkImage image, bool depth, VkImageLayout from, VkImageLayout to,
                 VkAccessFlags source_access) const {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                          : VK_IMAGE_ASPECT_COLOR_BIT),
                                    0, 1, 0, 1};
        device_->vk().CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // The image's texels (4 bytes each: RGBA8 or D32), copied out once the queue is idle; the image goes back to its
    // attachment layout.
    std::vector<uint8_t> Read(uint32_t id, uint32_t index) {
        const VulkanFunctions& vk = device_->vk();
        const Set& set = sets_[id];
        const VkImage image = set.images[index].image;
        const VkDeviceSize size = static_cast<VkDeviceSize>(set.width) * set.height * 4;
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        CheckVk(vk.CreateBuffer(device_->Device(), &buffer_info, nullptr, &buffer), "vkCreateBuffer(read-back)");
        VkMemoryRequirements requirements{};
        vk.GetBufferMemoryRequirements(device_->Device(), buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = device_->MemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
        VkResult result = vk.AllocateMemory(device_->Device(), &allocate, nullptr, &memory);
        if (result == VK_SUCCESS) {
            result = vk.BindBufferMemory(device_->Device(), buffer, memory, 0);
        }
        if (result == VK_SUCCESS) {
            device_->RunOnce([&](VkCommandBuffer commands) {
                const VkAccessFlags written = set.depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                                        : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                Barrier(commands, image, set.depth, AttachmentLayout(set.depth), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        written);
                VkBufferImageCopy region{};
                region.imageSubresource = {static_cast<VkImageAspectFlags>(set.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                     : VK_IMAGE_ASPECT_COLOR_BIT),
                                           0, 0, 1};
                region.imageExtent = {set.width, set.height, 1};
                vk.CmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
                Barrier(commands, image, set.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, AttachmentLayout(set.depth),
                        VK_ACCESS_TRANSFER_READ_BIT);
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vk.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
                                      0, nullptr, 0, nullptr);
            });
            void* mapped = nullptr;
            result = vk.MapMemory(device_->Device(), memory, 0, size, 0, &mapped);
            if (result == VK_SUCCESS) {
                std::memcpy(bytes.data(), mapped, bytes.size());
                vk.UnmapMemory(device_->Device(), memory);
            }
        }
        vk.DestroyBuffer(device_->Device(), buffer, nullptr);
        if (memory != VK_NULL_HANDLE) {
            vk.FreeMemory(device_->Device(), memory, nullptr);
        }
        CheckVk(result, "the render check's read-back");
        return bytes;
    }

    VulkanGraphics graphics_;
    VulkanDevice* device_{nullptr};
    std::vector<Set> sets_;
    bool stall_queue_{false}; // --vk-test-stuck-queue, until the first removal
    VkSemaphore stuck_{VK_NULL_HANDLE}; // the semaphore the stalled queue waits for, until released
};

} // namespace

std::unique_ptr<CheckImages> CreateVulkanCheckImages(const Options& options) {
    return std::make_unique<VulkanCheckImages>(options);
}

void RunVulkanSelfTest() {
    Expect(VulkanApiFromXr(XR_MAKE_VERSION(1, 3, 290)) == VK_API_VERSION_1_3 &&
               VulkanApiFromXr(XR_MAKE_VERSION(1, 0, 0)) == VK_API_VERSION_1_0,
           "an XrVersion becomes a Vulkan API version without its patch");
    Expect(ApiAtLeast(VK_API_VERSION_1_2, VK_API_VERSION_1_0) && ApiAtLeast(VK_API_VERSION_1_2, VK_API_VERSION_1_2) &&
               !ApiAtLeast(VK_API_VERSION_1_0, VK_API_VERSION_1_1) &&
               ApiAtLeast(VK_MAKE_API_VERSION(0, 1, 3, 0), VK_MAKE_API_VERSION(0, 1, 3, 280)),
           "Vulkan API versions compare by major.minor");
    Expect(ApiText(VK_MAKE_API_VERSION(0, 1, 3, 280)) == "1.3", "Vulkan API text");
    Expect(SplitNames("VK_KHR_a  VK_KHR_b ") == std::vector<std::string>{"VK_KHR_a", "VK_KHR_b"} &&
               SplitNames("").empty() && SplitNames(std::string_view("VK_KHR_c\0", 9)).size() == 1,
           "XR_KHR_vulkan_enable's extension lists split on spaces");
    Expect(ChooseFormat({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
                        VulkanColorFormats()) == VK_FORMAT_B8G8R8A8_SRGB &&
               ChooseFormat({VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}, VulkanColorFormats()) ==
                   VK_FORMAT_R8G8B8A8_UNORM &&
               !ChooseFormat({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT}, VulkanColorFormats()),
           "Vulkan colour formats prefer sRGB, then RGBA over BGRA");
    const VkRect2D placed = ToVulkan({10, 20, 110, 70}, 1000, 5);
    Expect(placed.offset.x == 1010 && placed.offset.y == 25 && placed.extent.width == 100 && placed.extent.height == 50,
           "pattern rects are placed in a side-by-side image");
    using Types = std::vector<VkPhysicalDeviceType>;
    Expect(StandaloneDeviceIndex(Types{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU}) ==
                   1u &&
               StandaloneDeviceIndex(Types{VK_PHYSICAL_DEVICE_TYPE_CPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU}) ==
                   1u &&
               StandaloneDeviceIndex(Types{VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU}) == 0u,
           "without OpenXR the first discrete GPU, else the first that is not a CPU");
    Expect(!StandaloneDeviceIndex(Types{}) && !StandaloneDeviceIndex(Types{VK_PHYSICAL_DEVICE_TYPE_CPU}),
           "only CPU Vulkan devices (or none): the check cannot run here");
#ifdef VECTORXR_PROBE_SPIRV
    uint32_t magic = 0;
    std::memcpy(&magic, kProbeLoadSpirv, sizeof(magic));
    Expect(sizeof(kProbeLoadSpirv) % 4 == 0 && sizeof(kProbeLoadSpirv) > 20 && magic == 0x07230203u,
           "the GPU load's SPIR-V");
#else
    std::cout << "(built without SPIR-V: --gpu-ms is unavailable with --graphics vulkan)\n";
#endif
}

} // namespace probe
