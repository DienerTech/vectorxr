// Frame Generation's Vulkan bridge through the layer itself: the real hooks,
// from xrCreateApiLayerInstance on (XR_KHR_vulkan_enable and _enable2 among
// them), with a real Vulkan game on the GPU against a stand-in runtime whose
// Direct3D 12 sessions' swapchain images are Direct3D 12 textures on the
// device of the binding it is given (and whose Vulkan sessions' images are
// VkImages on the game's device). It makes the game's Vulkan instance and
// device as runtimes do for XR_KHR_vulkan_enable2, through the
// vkGetInstanceProcAddr it is handed, so the create infos that finally reach
// Vulkan are seen. The stand-in reads back what each frame shows on the
// session's queue, after the images' releases there, as a compositor reads
// what it shows. Frame Generation runs on a stand-in backend (it owns frame
// pacing, as with a real one, and does no GPU work), or on the real Direct3D
// 12 backend where a case says so. A native Direct3D 12 game (shaped like
// MSFS) runs too, before and after a bridged session. The host is left
// alone: settings, logs and the layer's other files go to a temporary
// directory. The Khronos validation layer checks the game's Vulkan work and
// the Direct3D 12 debug layer the Direct3D 12 work, after every case.
//
// Exit code 77: no vulkan-1.dll, no GPU whose Vulkan device and Direct3D 12
// adapter are the same (with FG_TEST_ADAPTER, test_adapter.h: none it names),
// or a driver that imports no Direct3D 12 images or fences (with
// VECTORXR_TESTS_REQUIRE_HW=1 that fails instead, as does running without
// validation or the Direct3D 12 debug layer). Past that, every bridge refusal
// fails. `unbridged` runs only the cases that need no working bridge (the
// hooks, the instance, fallbacks the bridge never sees, native Direct3D 12).
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/openxr_layer.h"
#include "depthxr/openxr_loader_api_layer.h"
#include "depthxr/vulkan_enable_hooks.h"
#include "test_adapter.h"
#include "test_gates.h"
#include "vulkan_test_validation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The layer's loader entry point (layer_entry.cpp).
extern "C" XrResult XRAPI_CALL xrCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                                        const depthxr::XrApiLayerCreateInfo* layer_info,
                                                        XrInstance* instance);

namespace depthxr {

// Test-only access to the singleton (DEPTHXR_TESTING), for what an
// application cannot reach through OpenXR.
class VulkanBridgeLayerTestPeer {
  public:
    // Frame Generation starts sessions with `factory`'s backend.
    static void UseFrameGenGpu(FrameGenPresenter::GpuFactory factory) {
        OpenXrLayer::Instance().framegen_gpu_factory_ = std::move(factory);
    }
    static VulkanBridge& Bridge() {
        return OpenXrLayer::Instance().vulkan_bridge_;
    }
    static bool QueueSubstituted() {
        return OpenXrLayer::Instance().framegen_queue_.Active();
    }
    static bool FrameGenOwnsPacing() {
        return OpenXrLayer::Instance().framegen_owns_pacing_.load();
    }
    static std::string GraphicsApi() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.graphics_api_;
    }
    // The instance's latched decision for the XR_KHR_vulkan_enable(2) hooks.
    static bool HooksAddForBridge() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.vulkan_bridge_extensions_;
    }
    // Nothing of an instance, a session, either bridge or Frame Generation is left.
    static bool Idle() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        return layer.instance_ == XR_NULL_HANDLE && layer.active_session_ == XR_NULL_HANDLE &&
               !layer.vulkan_bridge_.Active() && !layer.d3d11_bridge_.Active() && !layer.framegen_queue_.Active() &&
               !layer.framegen_.Started();
    }
    // The layer's log so far: an error reaches the file at once, with
    // everything logged before it.
    static std::string Log() {
        static int reads = 0;
        OpenXrLayer& layer = OpenXrLayer::Instance();
        layer.logger_.Error("Vulkan bridge layer tests: log read " + std::to_string(++reads) + ".");
        std::ifstream stream(layer.logger_.ActiveLogPath(), std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return text.str();
    }
    static std::filesystem::path LogPath() {
        return OpenXrLayer::Instance().logger_.ActiveLogPath();
    }
    // Lets the log file go (nothing is logged after the last instance).
    static void ReleaseLog(const std::filesystem::path& path) {
        OpenXrLayer::Instance().logger_.Initialize(path);
    }
    // Frame Generation engages after `steady_frames` steady frames once its
    // session is `minimum_age` old (kept across sessions).
    static void SetFrameGenEngagement(std::uint32_t steady_frames, std::chrono::milliseconds minimum_age) {
        OpenXrLayer::Instance().framegen_.SetEngagementPolicy(steady_frames, minimum_age);
    }
    // Frame Generation's state and counters (until its session ends).
    static FrameGenStatus FrameGen() {
        return OpenXrLayer::Instance().framegen_.Status();
    }
    // The settings file is read again now (as the layer's watcher does).
    static void ReloadConfig() {
        OpenXrLayer::Instance().PollConfigFile();
    }
    // The bridge's 30 s summary is logged at the next xrEndFrame.
    static void ExpireBridgeSummary() {
        OpenXrLayer& layer = OpenXrLayer::Instance();
        std::scoped_lock lock(layer.mutex_);
        layer.vulkan_bridge_last_summary_ = std::chrono::steady_clock::now() - std::chrono::seconds(31);
    }
};

} // namespace depthxr

namespace {

using Microsoft::WRL::ComPtr;
using Peer = depthxr::VulkanBridgeLayerTestPeer;
using Color = std::array<std::uint8_t, 4>;
using Names = std::vector<std::string>;

constexpr int kSkip = 77;
constexpr const char* kVulkanExtension = "XR_KHR_vulkan_enable";
constexpr const char* kVulkan2Extension = "XR_KHR_vulkan_enable2";
constexpr const char* kD3D12Extension = "XR_KHR_D3D12_enable";
constexpr const char* kD3D11Extension = "XR_KHR_D3D11_enable";
constexpr XrSystemId kSystem = 7;
constexpr XrDuration kPeriod = 11'111'111;
constexpr std::uint32_t kImages = 3;
constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
// Frame Generation's own cases measure motion: their images are this size.
constexpr std::uint32_t kFrameGenSize = 256;
// An application structure of no known type, chained after the ones the
// layer reads (it must reach the runtime as it was).
constexpr XrStructureType kApplicationStructType = static_cast<XrStructureType>(1000999000);
// The stand-in runtime's Direct3D 12 formats, in its order of preference:
// one Vulkan has no format for (B8G8R8X8) among them.
constexpr std::int64_t kRuntimeFormats12[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                              DXGI_FORMAT_R8G8B8A8_UNORM,      DXGI_FORMAT_B8G8R8X8_UNORM,
                                              DXGI_FORMAT_R16G16B16A16_FLOAT,  DXGI_FORMAT_D32_FLOAT};
constexpr std::int64_t kRuntimeFormatsVulkan[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                                  VK_FORMAT_D32_SFLOAT};

std::filesystem::path g_root; // the test's own directory for everything the layer writes
std::uint32_t g_image_width = kWidth; // the case's swapchains' size
std::uint32_t g_image_height = kHeight;
bool g_unbridged_only = false; // `unbridged`: the cases that need no working bridge

// Without static destructors: a failure inside a layer call (in the stand-in
// runtime) holds the layer's lock, which its teardown takes.
void Check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << "\n(layer log: " << Peer::LogPath().string() << ")" << std::endl;
        test_gates::OpenAllBeforeExit();
        std::_Exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// How many times `part` appears in `text`.
std::size_t Count(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

// The log's line holding `part` (for failure messages), or "none".
std::string LineWith(const std::string& log, const std::string& part) {
    const std::size_t at = log.rfind(part);
    if (at == std::string::npos) return "none";
    const std::size_t begin = log.rfind('\n', at);
    const std::size_t end = log.find('\n', at);
    return log.substr(begin == std::string::npos ? 0 : begin + 1,
                      end == std::string::npos ? std::string::npos : end - (begin == std::string::npos ? 0 : begin + 1));
}

// Logged once for each session the bridge makes Direct3D 12.
constexpr const char* kBridgedSessionLine =
    "Frame Generation's Vulkan bridge: the session is Direct3D 12 and the game renders into images shared with it";
// Logged for each Vulkan session Frame Generation does not run.
constexpr const char* kUnbridgedLine = "Frame Generation is off for this Vulkan session: ";
// The session's graphics lines (session_graphics.h): the game's Vulkan device,
// and the Direct3D 12 device of a bridged session.
constexpr const char* kGameGraphicsLine = "Session graphics: api=Vulkan, adapter=\"";
constexpr const char* kBridgeGraphicsLine =
    "Session graphics: api=Direct3D 12 (Frame Generation's Vulkan bridge), adapter=\"";

// The game's graphics line is logged once and names Vulkan's driver version
// and DXGI's, so DXGI's adapter was found (by the device's LUID, or by its PCI
// vendor and device ID when the LUID cannot be read).
void CheckGameGraphicsLine(const std::string& log) {
    const std::string line = LineWith(log, kGameGraphicsLine);
    Check(Count(log, kGameGraphicsLine) == 1 && Contains(line, ", vulkanDriver=") && Contains(line, ", driver=") &&
              !Contains(line, ", driver=unknown"),
          "the log names the hardware of the game's Vulkan device once, with both driver versions: " + line);
}

bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

template <typename T>
T Handle(std::uintptr_t value) {
    return reinterpret_cast<T>(value);
}

std::string Text(const Color& texel) {
    return std::to_string(texel[0]) + "," + std::to_string(texel[1]) + "," + std::to_string(texel[2]) + "," +
           std::to_string(texel[3]);
}

std::string Text(const std::optional<Color>& texel) {
    return texel ? Text(*texel) : "nothing";
}

std::string Text(const Names& names) {
    std::string text = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
        text += (i ? " " : "") + names[i];
    }
    return text + "]";
}

// Each attempt's extensions, in order.
std::string Text(const std::vector<Names>& attempts) {
    std::string text;
    for (const Names& names : attempts) {
        text += (text.empty() ? "" : " then ") + Text(names);
    }
    return text.empty() ? "no attempt" : text;
}

bool Near(const std::optional<Color>& texel, const Color& expected) {
    if (!texel) return false;
    for (int i = 0; i < 4; ++i) {
        if (std::abs(static_cast<int>((*texel)[i]) - static_cast<int>(expected[i])) > 1) return false;
    }
    return true;
}

bool Has(const Names& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

Names Split(const std::string& list) {
    Names names;
    std::istringstream words(list);
    for (std::string word; words >> word;) {
        names.push_back(word);
    }
    return names;
}

// Each frame's colour differs from those of the frames before it in the same image.
Color FrameColor(int frame, int eye) {
    return {static_cast<std::uint8_t>(16 + 37 * (frame % 6)), static_cast<std::uint8_t>(200 - 29 * (frame % 6)),
            static_cast<std::uint8_t>(eye ? 220 : 60), 255};
}

void SetEnv(const char* name, const std::string& value) {
    _putenv_s(name, value.c_str());
}

// --- Vulkan -----------------------------------------------------------------

#define TEST_VK_GLOBAL(X)                                                                                            \
    X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)
#define TEST_VK_INSTANCE(X)                                                                                          \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                              \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties)                                    \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceImageFormatProperties2)                              \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                       \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)
#define TEST_VK_DEVICE(X)                                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)        \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateSemaphore) X(vkDestroySemaphore)              \
    X(vkImportSemaphoreWin32HandleKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)    \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier)                    \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)   \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences)

#define TEST_VK_DECLARE(name) PFN_##name name = nullptr;
// The loader's functions, for the game's instance and device (one game at a time).
struct Vk {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    TEST_VK_GLOBAL(TEST_VK_DECLARE)
    TEST_VK_INSTANCE(TEST_VK_DECLARE)
    TEST_VK_DEVICE(TEST_VK_DECLARE)
};
Vk g_vk;
vulkan_test::Messages g_vk_messages; // what validation reported
bool g_validation = false;           // the game's instances enable the validation layer

void LoadInstanceFunctions(VkInstance instance) {
#define TEST_VK_LOAD_INSTANCE(name) g_vk.name = reinterpret_cast<PFN_##name>(g_vk.vkGetInstanceProcAddr(instance, #name));
    TEST_VK_INSTANCE(TEST_VK_LOAD_INSTANCE)
}

void LoadDeviceFunctions(VkDevice device) {
#define TEST_VK_LOAD_DEVICE(name) g_vk.name = reinterpret_cast<PFN_##name>(g_vk.vkGetDeviceProcAddr(device, #name));
    TEST_VK_DEVICE(TEST_VK_LOAD_DEVICE)
}

void CheckVk(VkResult result, const std::string& what) {
    Check(result == VK_SUCCESS, what + ": VkResult " + std::to_string(static_cast<int>(result)));
}

// The validation layer reported nothing since the last check.
void CheckVulkanValidation(const std::string& when) {
    const std::vector<std::string> messages = g_vk_messages.Take();
    std::string text;
    for (const std::string& message : messages) {
        text += message + "\n";
    }
    Check(messages.empty(), when + ": Vulkan validation reported\n" + text);
}

// What the game's own vkGetInstanceProcAddr saw: the create infos that
// finally reach vkCreateInstance and vkCreateDevice (after the runtime's and
// the layer's additions).
struct FinalCreates {
    int instances{0};
    int devices{0};
    Names instance_extensions;
    std::uint32_t instance_api{0};
    Names device_extensions;
    const void* device_next{nullptr}; // the final chain's first structure
    // The first structure was timeline semaphore features: its value, and what followed it.
    std::optional<VkBool32> timeline_first;
    const void* after_timeline{nullptr};
};
FinalCreates g_final;
VkInstance g_game_instance = VK_NULL_HANDLE; // the instance vkCreateDevice was resolved for

Names NamesOf(const char* const* names, std::uint32_t count) {
    Names result;
    for (std::uint32_t i = 0; names && i < count; ++i) {
        result.emplace_back(names[i]);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GameCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                                  VkInstance* instance) {
    ++g_final.instances;
    g_final.instance_extensions = NamesOf(info->ppEnabledExtensionNames, info->enabledExtensionCount);
    g_final.instance_api = info->pApplicationInfo ? info->pApplicationInfo->apiVersion : 0;
    return g_vk.vkCreateInstance(info, allocator, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL GameCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                                const VkAllocationCallbacks* allocator, VkDevice* device) {
    ++g_final.devices;
    g_final.device_extensions = NamesOf(info->ppEnabledExtensionNames, info->enabledExtensionCount);
    g_final.device_next = info->pNext;
    g_final.timeline_first.reset();
    g_final.after_timeline = nullptr;
    if (const auto* first = static_cast<const VkBaseInStructure*>(info->pNext);
        first && first->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
        g_final.timeline_first = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(first)->timelineSemaphore;
        g_final.after_timeline = first->pNext;
    }
    const auto create =
        reinterpret_cast<PFN_vkCreateDevice>(g_vk.vkGetInstanceProcAddr(g_game_instance, "vkCreateDevice"));
    return create ? create(physical, info, allocator, device) : VK_ERROR_INITIALIZATION_FAILED;
}

// The game's vkGetInstanceProcAddr (XR_KHR_vulkan_enable2's create infos):
// the loader's, watching instance and device creation (vkCreateDevice only
// where the loader has it: never for a null instance).
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GameGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (std::strcmp(name, "vkCreateInstance") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(GameCreateInstance);
    }
    if (std::strcmp(name, "vkCreateDevice") == 0) {
        if (!g_vk.vkGetInstanceProcAddr(instance, name)) return nullptr;
        g_game_instance = instance;
        return reinterpret_cast<PFN_vkVoidFunction>(GameCreateDevice);
    }
    return g_vk.vkGetInstanceProcAddr(instance, name);
}

// sRGB's encoding: the linear value whose 8-bit sRGB encoding is `byte`.
float Linear(std::uint8_t byte) {
    const float value = byte / 255.0f;
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

std::uint32_t MemoryType(VkPhysicalDevice physical, std::uint32_t bits) {
    VkPhysicalDeviceMemoryProperties properties{};
    g_vk.vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool IsDepth(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// The Vulkan game: its instance, device and queue, and command buffers it
// renders with (each reused once the GPU is done with it).
struct VulkanGame {
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t family{0};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    VkCommandPool pool{VK_NULL_HANDLE};
    std::array<VkCommandBuffer, 16> commands{};
    std::array<VkFence, 16> fences{};
    std::uint32_t next{0};
};

void FinishGame(VulkanGame& game) {
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = game.family;
    CheckVk(g_vk.vkCreateCommandPool(game.device, &pool, nullptr, &game.pool), "the game's command pool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = game.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = static_cast<std::uint32_t>(game.commands.size());
    CheckVk(g_vk.vkAllocateCommandBuffers(game.device, &allocate, game.commands.data()), "the game's command buffers");
    for (VkFence& fence : game.fences) {
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        CheckVk(g_vk.vkCreateFence(game.device, &info, nullptr, &fence), "a fence of the game's");
    }
    g_vk.vkGetDeviceQueue(game.device, game.family, 0, &game.queue);
}

std::uint32_t GraphicsFamily(VkPhysicalDevice physical) {
    std::uint32_t count = 0;
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    g_vk.vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    }
    return 0;
}

void MakeMessenger(VulkanGame& game) {
    if (!g_validation || !g_vk.vkCreateDebugUtilsMessengerEXT) return;
    const VkDebugUtilsMessengerCreateInfoEXT info = vulkan_test::MessengerInfo(&g_vk_messages);
    CheckVk(g_vk.vkCreateDebugUtilsMessengerEXT(game.instance, &info, nullptr, &game.messenger), "the messenger");
}

void DestroyGame(VulkanGame& game) {
    if (game.device) {
        CheckVk(g_vk.vkDeviceWaitIdle(game.device), "the game's device idles");
        for (VkFence fence : game.fences) {
            if (fence) g_vk.vkDestroyFence(game.device, fence, nullptr);
        }
        if (game.pool) g_vk.vkDestroyCommandPool(game.device, game.pool, nullptr);
        g_vk.vkDestroyDevice(game.device, nullptr);
    }
    if (game.messenger) g_vk.vkDestroyDebugUtilsMessengerEXT(game.instance, game.messenger, nullptr);
    if (game.instance) g_vk.vkDestroyInstance(game.instance, nullptr);
    game = VulkanGame{};
}

VkCommandBuffer BeginCommands(VulkanGame& game, std::uint32_t& slot) {
    slot = game.next++ % static_cast<std::uint32_t>(game.commands.size());
    Check(g_vk.vkWaitForFences(game.device, 1, &game.fences[slot], VK_TRUE, 10'000'000'000ull) == VK_SUCCESS,
          "the game's earlier command buffer finished");
    CheckVk(g_vk.vkResetFences(game.device, 1, &game.fences[slot]), "a fence reset");
    CheckVk(g_vk.vkResetCommandBuffer(game.commands[slot], 0), "a command buffer reset");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(g_vk.vkBeginCommandBuffer(game.commands[slot], &begin), "a command buffer begins");
    return game.commands[slot];
}

void SubmitCommands(VulkanGame& game, std::uint32_t slot) {
    CheckVk(g_vk.vkEndCommandBuffer(game.commands[slot]), "a command buffer ends");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &game.commands[slot];
    CheckVk(g_vk.vkQueueSubmit(game.queue, 1, &submit, game.fences[slot]), "the game's submission");
}

// The game's rendering of an image it was handed in the attachment layout
// (and gives back in it): a clear of every layer, through a transfer.
void ClearVulkan(VulkanGame& game, VkImage image, std::uint32_t layers, const Color& color, bool srgb) {
    std::uint32_t slot = 0;
    const VkCommandBuffer commands = BeginCommands(game, slot);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    g_vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue clear{};
    for (int i = 0; i < 3; ++i) {
        clear.float32[i] = srgb ? Linear(color[i]) : color[i] / 255.0f;
    }
    clear.float32[3] = color[3] / 255.0f;
    g_vk.vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    std::swap(barrier.oldLayout, barrier.newLayout);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    g_vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                              0, nullptr, 0, nullptr, 1, &barrier);
    SubmitCommands(game, slot);
}

void ClearVulkanDepth(VulkanGame& game, VkImage image, std::uint32_t layers, float depth) {
    std::uint32_t slot = 0;
    const VkCommandBuffer commands = BeginCommands(game, slot);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    const VkPipelineStageFlags depth_stages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    g_vk.vkCmdPipelineBarrier(commands, depth_stages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    const VkClearDepthStencilValue clear{depth, 0};
    g_vk.vkCmdClearDepthStencilImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    std::swap(barrier.oldLayout, barrier.newLayout);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    g_vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, depth_stages, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    SubmitCommands(game, slot);
}

// The game's queue still runs: an empty submission's fence signals.
bool GameRuns(VulkanGame& game) {
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (g_vk.vkCreateFence(game.device, &info, nullptr, &fence) != VK_SUCCESS) return false;
    const bool ran = g_vk.vkQueueSubmit(game.queue, 0, nullptr, fence) == VK_SUCCESS &&
                     g_vk.vkWaitForFences(game.device, 1, &fence, VK_TRUE, 10'000'000'000ull) == VK_SUCCESS;
    g_vk.vkDestroyFence(game.device, fence, nullptr);
    return ran;
}

// --- Direct3D 12 ------------------------------------------------------------

// Only errors and corruption are stored: nothing the checks look for is
// crowded out by the message limit.
void StoreErrorsOnly(ID3D12InfoQueue* queue) {
    D3D12_MESSAGE_SEVERITY deny[] = {D3D12_MESSAGE_SEVERITY_WARNING, D3D12_MESSAGE_SEVERITY_INFO,
                                     D3D12_MESSAGE_SEVERITY_MESSAGE};
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = static_cast<UINT>(std::size(deny));
    filter.DenyList.pSeverityList = deny;
    queue->PushStorageFilter(&filter);
}

// Every error and corruption message the Direct3D 12 debug layer stored
// since the last check (messages it had to drop counted as errors).
std::string ValidationMessages12(ID3D12Device* device) {
    std::string errors;
    ComPtr<ID3D12InfoQueue> queue;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        if (const UINT64 dropped = queue->GetNumMessagesDiscardedByMessageCountLimit()) {
            errors += "[D3D12] " + std::to_string(dropped) + " messages dropped at the message limit\n";
        }
        for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                errors += std::string("[D3D12 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue->ClearStoredMessages();
    }
    return errors;
}

bool WaitFence(ID3D12Fence* fence, UINT64 value, DWORD timeout_ms) {
    if (fence->GetCompletedValue() >= value) return true;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event && SUCCEEDED(fence->SetEventOnCompletion(value, event))) {
        WaitForSingleObject(event, timeout_ms);
    }
    if (event) CloseHandle(event);
    return fence->GetCompletedValue() >= value;
}

// A fence the test opens: the game's Vulkan queue (or a Direct3D 12 queue)
// held back until Open. On Vulkan it is the fence imported as a semaphore,
// as the bridge imports its own.
struct Gate {
    ComPtr<ID3D12Fence> fence;
    VulkanGame* game{nullptr};
    VkSemaphore semaphore{VK_NULL_HANDLE};
    Gate(ID3D12Device* device, VulkanGame* vulkan) : game(vulkan) {
        HANDLE handle = nullptr;
        Check(device && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) &&
                  SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)),
              "a gate fence");
        if (game) {
            VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            CheckVk(g_vk.vkCreateSemaphore(game->device, &info, nullptr, &semaphore), "the gate's semaphore");
            Check(g_vk.vkImportSemaphoreWin32HandleKHR != nullptr, "the game's device imports semaphores");
            VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
            import.semaphore = semaphore;
            import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
            import.handle = handle;
            CheckVk(g_vk.vkImportSemaphoreWin32HandleKHR(game->device, &import), "the gate opened on the game's device");
        }
        CloseHandle(handle);
        test_gates::Add(fence.Get());
    }
    ~Gate() {
        test_gates::Remove(fence.Get());
        Open();
        if (game && semaphore) {
            g_vk.vkQueueWaitIdle(game->queue);
            g_vk.vkDestroySemaphore(game->device, semaphore, nullptr);
        }
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    // The game's queue runs nothing submitted after this until Open.
    void HoldGame() {
        const UINT64 value = 1;
        VkD3D12FenceSubmitInfoKHR values{VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR};
        values.waitSemaphoreValuesCount = 1;
        values.pWaitSemaphoreValues = &value;
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.pNext = &values;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &stage;
        CheckVk(g_vk.vkQueueSubmit(game->queue, 1, &submit, VK_NULL_HANDLE), "the game's queue held at the gate");
    }
    void HoldQueue(ID3D12CommandQueue* queue) { queue->Wait(fence.Get(), 1); }
    void Open() { fence->Signal(1); }
    // Opened from another thread after `ms`, while the layer waits for what it holds.
    std::thread OpenLater(DWORD ms) {
        return std::thread([this, ms] {
            Sleep(ms);
            Open();
        });
    }
};

// A native Direct3D 12 game (shaped like MSFS): it renders into the
// runtime's images on a queue of its own, each clear waited for.
struct D3D12Game {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtv;
    ComPtr<ID3D12DescriptorHeap> dsv;
    ComPtr<ID3D12Fence> fence;
    UINT64 value{0};
};

bool MakeD3D12Game(const LUID& luid, D3D12Game& game) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&game.device)))) {
        return false;
    }
    const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8};
    D3D12_DESCRIPTOR_HEAP_DESC dsv{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8};
    return SUCCEEDED(game.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&game.queue))) &&
           SUCCEEDED(game.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&game.allocator))) &&
           SUCCEEDED(game.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, game.allocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&game.list))) &&
           SUCCEEDED(game.list->Close()) && SUCCEEDED(game.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&game.rtv))) &&
           SUCCEEDED(game.device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(&game.dsv))) &&
           SUCCEEDED(game.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&game.fence)));
}

// Every slice of a runtime image cleared: colour images are render targets
// and depth images depth targets whenever the game holds them.
void ClearD3D12(D3D12Game& game, ID3D12Resource* image, const Color& color, float depth) {
    Check(SUCCEEDED(game.allocator->Reset()) && SUCCEEDED(game.list->Reset(game.allocator.Get(), nullptr)),
          "the Direct3D 12 game's command list");
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    const bool is_depth = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    for (UINT slice = 0; slice < desc.DepthOrArraySize && slice < 8; ++slice) {
        if (is_depth) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = game.dsv->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += slice * game.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            D3D12_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_D32_FLOAT;
            view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.ArraySize = 1;
            game.device->CreateDepthStencilView(image, &view, handle);
            game.list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
        } else {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = game.rtv->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += slice * game.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.ArraySize = 1;
            game.device->CreateRenderTargetView(image, &view, handle);
            const float rgba[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
            game.list->ClearRenderTargetView(handle, rgba, 0, nullptr);
        }
    }
    Check(SUCCEEDED(game.list->Close()), "the Direct3D 12 game's command list closes");
    ID3D12CommandList* lists[] = {game.list.Get()};
    game.queue->ExecuteCommandLists(1, lists);
    game.queue->Signal(game.fence.Get(), ++game.value);
    Check(WaitFence(game.fence.Get(), game.value, 10000), "the Direct3D 12 game's clear ran");
}

// --- The stand-in runtime ---------------------------------------------------

struct StandInSwapchain {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    std::vector<ComPtr<ID3D12Resource>> images12; // a Direct3D 12 session's
    std::vector<VkImage> images_vk;               // a Vulkan session's, on the game's device
    std::vector<VkDeviceMemory> memory_vk;
    std::deque<std::uint32_t> acquired; // oldest first
    std::uint32_t next{0};
    std::optional<std::uint32_t> released; // the image released last
    int acquires{0};
    int waits{0};
    int releases{0};
    std::size_t Count() const { return images12.empty() ? images_vk.size() : images12.size(); }
};

// What the stand-in showed for one projection view.
struct ShownView {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t slice{0};
    XrStructureType next_type{XR_TYPE_UNKNOWN}; // the first structure of the view's chain
    bool depth{false};                          // the chain had XrCompositionLayerDepthInfoKHR
    XrSwapchain depth_swapchain{XR_NULL_HANDLE};
    std::optional<Color> texel;       // the centre of the image released last
    std::optional<float> depth_value; // the centre of its depth (StandIn::read_depth)
};

struct ShownFrame {
    XrTime time{0};
    std::vector<ShownView> views;
    std::vector<XrCompositionLayerFlags> projection_flags; // each projection layer's
    std::vector<XrSwapchain> quads;                        // each quad layer's swapchain
    std::vector<XrCompositionLayerFlags> quad_flags;
    bool held{false}; // the readback still waited while the game's rendering was held back
    std::vector<Color> unordered; // read meanwhile on a queue ordered after nothing (the control)
};

// The stand-in runtime: what it does, and what it saw.
struct StandIn {
    std::vector<std::string> extensions{kVulkanExtension, kVulkan2Extension, kD3D12Extension}; // listed before the instance
    bool enumeration_offered{true};
    bool vulkan_offered{true}; // XR_KHR_vulkan_enable(2)'s functions
    bool refuse_d3d12_instance{false}; // XR_ERROR_EXTENSION_NOT_PRESENT for instances with XR_KHR_D3D12_enable
    bool refuse_d3d12_session{false};  // XR_ERROR_GRAPHICS_DEVICE_INVALID for Direct3D 12 sessions
    bool refuse_destroy_session{false}; // xrDestroySession fails (the session stays)
    bool refuse_transfer_dst{false};    // swapchains with TRANSFER_DST are refused
    bool ignore_getter{false}; // makes the game's Vulkan instance and device with the loader's own functions
    // Looks vkCreateDevice up through the getter in xrCreateVulkanInstanceKHR
    // (for the instance it made) and calls that in xrCreateVulkanDeviceKHR
    // without looking it up again.
    bool cache_create_device{false};
    PFN_vkCreateDevice cached_create_device{nullptr};
    bool skip_readback{false}; // shown frames are not read back
    std::uint32_t width_error{0}; // images this much wider than asked for
    bool read_depth{false};       // shown views' depth is read back too
    LUID luid{};                  // the adapter it composes on
    // XR_KHR_vulkan_enable's lists, and what XR_KHR_vulkan_enable2's
    // creation adds of its own, as runtimes do (the fence capabilities among
    // them: the validation layer takes them alone for a Vulkan 1.0
    // instance's VkPhysicalDeviceIDProperties, which the runtime reads).
    std::string vulkan_instance_extensions{"VK_KHR_external_memory_capabilities VK_KHR_get_physical_device_properties2"};
    std::string vulkan_device_extensions{"VK_KHR_external_memory VK_KHR_dedicated_allocation"};
    std::vector<const char*> vulkan_instance_additions{"VK_KHR_external_memory_capabilities",
                                                       "VK_KHR_external_fence_capabilities"};
    std::vector<const char*> vulkan_device_additions{"VK_KHR_external_memory"};
    bool paced{false}; // xrWaitFrame returns at the next refresh, as a runtime's does (else at once)
    // Direct3D 12 held back behind it: the next readback is checked to wait
    // for the game's rendering, then the gate is opened.
    Gate* hold{nullptr};

    std::vector<Names> instance_attempts; // each attempt's extensions
    std::vector<std::string> calls;
    XrInstance instance{XR_NULL_HANDLE};
    XrSession session{XR_NULL_HANDLE};
    XrStructureType binding{XR_TYPE_UNKNOWN}; // the session's graphics binding
    const void* binding_next{nullptr};        // what followed it
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue; // the session's queue
    VkDevice vk_device{VK_NULL_HANDLE};
    VkPhysicalDevice vk_physical{VK_NULL_HANDLE};
    VkInstance vk_instance{VK_NULL_HANDLE}; // the one xrGetVulkanGraphicsDevice2KHR was asked about
    PFN_vkGetInstanceProcAddr given_get_instance_proc_addr{nullptr}; // XR_KHR_vulkan_enable2's, as received
    ComPtr<ID3D12Fence> readback;
    UINT64 readback_value{0};
    ComPtr<ID3D12CommandQueue> control_queue; // the stand-in's own, for the control readbacks
    ComPtr<ID3D12Fence> control;
    UINT64 control_value{0};
    std::map<XrSwapchain, StandInSwapchain> swapchains;
    std::vector<XrSwapchainCreateInfo> swapchain_requests; // as received
    std::vector<XrSwapchain> created;
    std::vector<XrSwapchain> destroyed;
    std::vector<ShownFrame> frames;
    std::function<void(XrSwapchain)> on_destroy_swapchain;
    std::function<void()> on_destroy_session;
    std::function<void()> on_destroy_instance; // before a session still alive goes with the instance
    XrTime time{1'000'000'000};
    std::chrono::steady_clock::time_point refresh{}; // paced: the refresh the last wait returned at
    std::uintptr_t handles{0x1000};

    // The index of the first `call` at or after `from`, or -1.
    std::ptrdiff_t Position(const std::string& call, std::ptrdiff_t from = 0) const {
        for (std::size_t i = static_cast<std::size_t>(std::max<std::ptrdiff_t>(from, 0)); i < calls.size(); ++i) {
            if (calls[i] == call) return static_cast<std::ptrdiff_t>(i);
        }
        return -1;
    }
};

StandIn g_runtime;
// Over g_runtime while a case runs: Frame Generation's presenter thread calls
// the runtime as the application's thread does. The test reads what the
// stand-in saw under it too (Shown).
std::recursive_mutex g_runtime_mutex;

// The frames the stand-in showed from the `from`th on.
std::vector<ShownFrame> Shown(std::size_t from = 0) {
    std::scoped_lock lock(g_runtime_mutex);
    const std::vector<ShownFrame>& frames = g_runtime.frames;
    return {frames.begin() + static_cast<std::ptrdiff_t>(std::min(from, frames.size())), frames.end()};
}

std::size_t ShownCount() {
    std::scoped_lock lock(g_runtime_mutex);
    return g_runtime.frames.size();
}

// Until `until`, on a high-resolution timer (a runtime's wait wakes at the refresh).
void SleepUntil(std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();
    if (until <= now) return;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<LONGLONG>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count() / 100); // relative, 100 ns
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        WaitForSingleObject(timer, INFINITE);
    } else {
        std::this_thread::sleep_until(until);
    }
    if (timer) CloseHandle(timer);
}

StandInSwapchain* FindSwapchain(XrSwapchain swapchain) {
    const auto it = g_runtime.swapchains.find(swapchain);
    return it == g_runtime.swapchains.end() ? nullptr : &it->second;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// A texel to read: an image's array slice (its centre). Depth images read
// their whole slice (Direct3D 12 copies depth by subresource only).
struct Source {
    ID3D12Resource* image{nullptr};
    std::uint32_t slice{0};
};
using Sources = std::vector<Source>;

bool IsDepthImage(ID3D12Resource* image) {
    return (image->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
}

// A copy of each source, submitted to a queue: its centre texel (colour) or
// its slice (depth), each at a placement of its own.
struct Readback {
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ID3D12Fence* fence{nullptr};
    UINT64 value{0};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> placements;
    std::vector<bool> depth;
};

bool SubmitReadback(ID3D12CommandQueue* queue, ID3D12Fence* fence, UINT64& fence_value, const Sources& sources,
                    Readback& readback) {
    ID3D12Device* device = g_runtime.device12.Get();
    UINT64 offset = 0;
    for (const Source& source : sources) {
        const D3D12_RESOURCE_DESC desc = source.image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT placement{};
        UINT64 bytes = 0;
        if (IsDepthImage(source.image)) {
            device->GetCopyableFootprints(&desc, source.slice * desc.MipLevels, 1, offset, &placement, nullptr,
                                          nullptr, &bytes);
        } else {
            placement.Offset = offset;
            placement.Footprint = {desc.Format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
            bytes = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        }
        readback.placements.push_back(placement);
        readback.depth.push_back(IsDepthImage(source.image));
        offset = (offset + bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                 D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    }
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<UINT64>(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&readback.buffer))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&readback.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, readback.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&readback.list)))) {
        return false;
    }
    // Colour images are render targets and depth images depth targets
    // whenever the runtime holds them.
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    for (const Source& texel : sources) {
        if (std::none_of(barriers.begin(), barriers.end(), [&](const D3D12_RESOURCE_BARRIER& barrier) {
                return barrier.Transition.pResource == texel.image;
            })) {
            barriers.push_back(Transition(texel.image,
                                          IsDepthImage(texel.image) ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                                    : D3D12_RESOURCE_STATE_RENDER_TARGET,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE));
        }
    }
    readback.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const D3D12_RESOURCE_DESC image = sources[i].image->GetDesc();
        D3D12_TEXTURE_COPY_LOCATION source{sources[i].image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = sources[i].slice * image.MipLevels;
        D3D12_TEXTURE_COPY_LOCATION destination{readback.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = readback.placements[i];
        if (readback.depth[i]) {
            readback.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        } else {
            const UINT x = static_cast<UINT>(image.Width / 2);
            const UINT y = image.Height / 2;
            const D3D12_BOX box{x, y, 0, x + 1, y + 1, 1};
            readback.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        }
    }
    for (D3D12_RESOURCE_BARRIER& barrier : barriers) {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    }
    readback.list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    if (FAILED(readback.list->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {readback.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    readback.fence = fence;
    readback.value = ++fence_value;
    queue->Signal(fence, readback.value);
    return true;
}

// Each source's centre: colour texels, and depth values (as the red
// channel's float, in `depth`).
bool FinishReadback(Readback& readback, std::vector<Color>& texels, std::vector<float>* depth = nullptr) {
    if (!WaitFence(readback.fence, readback.value, 10000)) {
        return false;
    }
    void* mapped = nullptr;
    if (FAILED(readback.buffer->Map(0, nullptr, &mapped))) {
        return false;
    }
    texels.clear();
    if (depth) depth->clear();
    for (std::size_t i = 0; i < readback.placements.size(); ++i) {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& placement = readback.placements[i];
        const auto* base = static_cast<const std::uint8_t*>(mapped) + placement.Offset;
        if (readback.depth[i]) {
            const auto* row = base + placement.Footprint.RowPitch * (placement.Footprint.Height / 2);
            float value = 0.0f;
            std::memcpy(&value, row + 4 * (placement.Footprint.Width / 2), sizeof(value));
            if (depth) depth->push_back(value);
            texels.push_back({0, 0, 0, 0});
        } else {
            texels.push_back({base[0], base[1], base[2], base[3]});
            if (depth) depth->push_back(0.0f);
        }
    }
    const D3D12_RANGE written{0, 0};
    readback.buffer->Unmap(0, &written);
    return true;
}

// Reads each source on the session's queue, after everything queued there
// before (the images' releases among it), as a compositor reads what it
// shows. With the game held back (StandIn::hold), the readback must still be
// waiting a while later, nothing ordered after the game's rendering having
// run; as a control, the same images read meanwhile on a queue of the
// stand-in's own, ordered after nothing, do not hold the frame yet.
bool ReadBack12(const Sources& sources, ShownFrame& frame, std::vector<Color>& texels, std::vector<float>& depth) {
    StandIn& rt = g_runtime;
    Readback shown;
    if (!SubmitReadback(rt.queue.Get(), rt.readback.Get(), rt.readback_value, sources, shown)) {
        return false;
    }
    if (rt.hold) {
        Sleep(50);
        frame.held = rt.readback->GetCompletedValue() < shown.value;
        if (!rt.control_queue) {
            const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
            rt.device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&rt.control_queue));
            rt.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.control));
        }
        Readback control;
        if (rt.control_queue && rt.control &&
            SubmitReadback(rt.control_queue.Get(), rt.control.Get(), rt.control_value, sources, control)) {
            FinishReadback(control, frame.unordered);
        }
        rt.hold->Open();
        rt.hold = nullptr;
    }
    return FinishReadback(shown, texels, &depth);
}

// What one image of a runtime swapchain holds now, read on a queue ordered
// after nothing (the stand-in's own).
std::optional<Color> ReadNow(ID3D12Resource* image) {
    StandIn& rt = g_runtime;
    if (!rt.control_queue) {
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
        rt.device12->CreateCommandQueue(&desc, IID_PPV_ARGS(&rt.control_queue));
        rt.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.control));
    }
    Readback readback;
    std::vector<Color> texels;
    if (!rt.control_queue || !SubmitReadback(rt.control_queue.Get(), rt.control.Get(), rt.control_value, {{image, 0}},
                                             readback) ||
        !FinishReadback(readback, texels) || texels.empty()) {
        return std::nullopt;
    }
    return texels[0];
}

XrResult XRAPI_CALL RtEnumerateInstanceExtensions(const char*, std::uint32_t capacity, std::uint32_t* count,
                                                   XrExtensionProperties* properties) {
    std::scoped_lock lock(g_runtime_mutex);
    const std::vector<std::string>& names = g_runtime.extensions;
    *count = static_cast<std::uint32_t>(names.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < names.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::strncpy(properties[i].extensionName, names[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                             const depthxr::XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    Names extensions;
    for (std::uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        extensions.emplace_back(info->enabledExtensionNames[i]);
    }
    rt.instance_attempts.push_back(extensions);
    rt.calls.push_back("xrCreateApiLayerInstance");
    // The stand-in is the last of the chain: nothing follows it.
    Check(layer_info && layer_info->nextInfo == nullptr, "the layer hands the rest of the chain on");
    if (rt.refuse_d3d12_instance && Has(extensions, kD3D12Extension)) {
        return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
    *instance = rt.instance = Handle<XrInstance>(++rt.handles);
    return XR_SUCCESS;
}

void FreeVulkanImages(StandInSwapchain& chain) {
    for (std::size_t i = 0; i < chain.images_vk.size(); ++i) {
        g_vk.vkDestroyImage(g_runtime.vk_device, chain.images_vk[i], nullptr);
        g_vk.vkFreeMemory(g_runtime.vk_device, chain.memory_vk[i], nullptr);
    }
    chain.images_vk.clear();
    chain.memory_vk.clear();
}

// The session's swapchains (their images) and everything of its device go.
void DropSession(StandIn& rt) {
    for (auto& [handle, chain] : rt.swapchains) {
        FreeVulkanImages(chain);
    }
    rt.swapchains.clear();
    rt.device12.Reset();
    rt.queue.Reset();
    rt.vk_device = VK_NULL_HANDLE;
    rt.readback.Reset();
    rt.control_queue.Reset();
    rt.control.Reset();
    rt.session = XR_NULL_HANDLE;
}

XrResult XRAPI_CALL RtDestroyInstance(XrInstance instance) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroyInstance");
    Check(instance == rt.instance, "the runtime's instance is destroyed");
    if (rt.on_destroy_instance) {
        const auto hook = std::move(rt.on_destroy_instance);
        rt.on_destroy_instance = nullptr;
        hook();
    }
    // A session the application did not destroy goes with its instance.
    DropSession(rt);
    rt.instance = XR_NULL_HANDLE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetInstanceProperties(XrInstance, XrInstanceProperties* properties) {
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    std::strncpy(properties->runtimeName, "VectorXR stand-in runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* system) {
    *system = kSystem;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* properties) {
    properties->systemId = kSystem;
    std::strncpy(properties->systemName, "Stand-in headset", XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->graphicsProperties.maxLayerCount = 16;
    properties->graphicsProperties.maxSwapchainImageWidth = 4096;
    properties->graphicsProperties.maxSwapchainImageHeight = 4096;
    properties->trackingProperties.orientationTracking = XR_TRUE;
    properties->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetD3D12Requirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* requirements) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetD3D12GraphicsRequirementsKHR");
    requirements->adapterLuid = g_runtime.luid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetVulkanRequirements(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR* requirements) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanGraphicsRequirementsKHR");
    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetVulkanRequirements2(XrInstance, XrSystemId, XrGraphicsRequirementsVulkan2KHR* requirements) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanGraphicsRequirements2KHR");
    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
}

// XR_KHR_vulkan_enable's space-separated lists, with the two-call idiom.
XrResult ExtensionList(const std::string& list, std::uint32_t capacity, std::uint32_t* count, char* buffer) {
    *count = static_cast<std::uint32_t>(list.size() + 1);
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    std::memcpy(buffer, list.c_str(), list.size() + 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetVulkanInstanceExtensions(XrInstance, XrSystemId, std::uint32_t capacity, std::uint32_t* count,
                                                   char* buffer) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanInstanceExtensionsKHR");
    return ExtensionList(g_runtime.vulkan_instance_extensions, capacity, count, buffer);
}

XrResult XRAPI_CALL RtGetVulkanDeviceExtensions(XrInstance, XrSystemId, std::uint32_t capacity, std::uint32_t* count,
                                                 char* buffer) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanDeviceExtensionsKHR");
    return ExtensionList(g_runtime.vulkan_device_extensions, capacity, count, buffer);
}

// The Vulkan device of the adapter the stand-in composes on.
VkPhysicalDevice RuntimeDevice(VkInstance instance) {
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        g_vk.vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    // As a runtime asks a Vulkan 1.0 instance: by the extension's name first.
    auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        g_vk.vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (!properties2) {
        properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            g_vk.vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    }
    std::uint32_t count = 0;
    if (!enumerate || !properties2 || enumerate(instance, &count, nullptr) != VK_SUCCESS) return VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> devices(count);
    enumerate(instance, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        properties2(device, &properties);
        if (id.deviceLUIDValid && std::memcmp(id.deviceLUID, &g_runtime.luid, VK_LUID_SIZE) == 0) return device;
    }
    return VK_NULL_HANDLE;
}

XrResult XRAPI_CALL RtGetVulkanGraphicsDevice(XrInstance, XrSystemId, VkInstance instance, VkPhysicalDevice* device) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanGraphicsDeviceKHR");
    *device = RuntimeDevice(instance);
    return *device ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL RtGetVulkanGraphicsDevice2(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR* info,
                                               VkPhysicalDevice* device) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrGetVulkanGraphicsDevice2KHR");
    g_runtime.vk_instance = info->vulkanInstance;
    *device = RuntimeDevice(info->vulkanInstance);
    return *device ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
}

// `names` with `additions` it lacks appended.
std::vector<const char*> WithAdditions(const char* const* names, std::uint32_t count,
                                       const std::vector<const char*>& additions) {
    std::vector<const char*> result(names, names + (names ? count : 0));
    for (const char* name : additions) {
        if (std::none_of(result.begin(), result.end(), [&](const char* have) { return std::strcmp(have, name) == 0; })) {
            result.push_back(name);
        }
    }
    return result;
}

// As runtimes make the game's instance: its create info with the runtime's
// own extensions added, through the vkGetInstanceProcAddr it was given.
XrResult XRAPI_CALL RtCreateVulkanInstance(XrInstance, const XrVulkanInstanceCreateInfoKHR* info, VkInstance* instance,
                                           VkResult* result) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrCreateVulkanInstanceKHR");
    rt.given_get_instance_proc_addr = info->pfnGetInstanceProcAddr;
    VkInstanceCreateInfo create = *info->vulkanCreateInfo;
    const std::vector<const char*> names =
        WithAdditions(create.ppEnabledExtensionNames, create.enabledExtensionCount, rt.vulkan_instance_additions);
    create.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    create.ppEnabledExtensionNames = names.data();
    const auto make = rt.ignore_getter ? g_vk.vkCreateInstance
                                       : reinterpret_cast<PFN_vkCreateInstance>(
                                             info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    *result = make ? make(&create, info->vulkanAllocator, instance) : VK_ERROR_INITIALIZATION_FAILED;
    if (rt.cache_create_device && *result == VK_SUCCESS) {
        rt.cached_create_device =
            reinterpret_cast<PFN_vkCreateDevice>(info->pfnGetInstanceProcAddr(*instance, "vkCreateDevice"));
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateVulkanDevice(XrInstance, const XrVulkanDeviceCreateInfoKHR* info, VkDevice* device,
                                         VkResult* result) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrCreateVulkanDeviceKHR");
    rt.given_get_instance_proc_addr = info->pfnGetInstanceProcAddr;
    VkDeviceCreateInfo create = *info->vulkanCreateInfo;
    const std::vector<const char*> names =
        WithAdditions(create.ppEnabledExtensionNames, create.enabledExtensionCount, rt.vulkan_device_additions);
    create.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
    create.ppEnabledExtensionNames = names.data();
    PFN_vkCreateDevice make = nullptr;
    if (rt.ignore_getter) {
        g_game_instance = rt.vk_instance;
        make = GameCreateDevice; // the loader's, watched by the test
    } else if (rt.cache_create_device) {
        make = rt.cached_create_device;
    } else {
        make = reinterpret_cast<PFN_vkCreateDevice>(info->pfnGetInstanceProcAddr(rt.vk_instance, "vkCreateDevice"));
    }
    *result = make ? make(info->vulkanPhysicalDevice, &create, info->vulkanAllocator, device)
                   : VK_ERROR_INITIALIZATION_FAILED;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateViewConfigurations(XrInstance, XrSystemId, std::uint32_t capacity,
                                                   std::uint32_t* count, XrViewConfigurationType* types) {
    *count = 1;
    if (capacity) types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType type,
                                                      XrViewConfigurationProperties* properties) {
    properties->viewConfigurationType = type;
    properties->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                                       std::uint32_t capacity, std::uint32_t* count,
                                                       XrViewConfigurationView* views) {
    *count = 2;
    for (std::uint32_t i = 0; i < std::min(capacity, 2u); ++i) {
        views[i].recommendedImageRectWidth = kWidth;
        views[i].recommendedImageRectHeight = kHeight;
        views[i].maxImageRectWidth = 4096;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, std::uint32_t capacity,
                                          std::uint32_t* count, XrEnvironmentBlendMode* modes) {
    *count = 1;
    if (capacity) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtPollEvent(XrInstance, XrEventDataBuffer*) {
    return XR_EVENT_UNAVAILABLE;
}

XrResult XRAPI_CALL RtCreateSession(XrInstance, const XrSessionCreateInfo* info, XrSession* session) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    // The graphics binding, wherever the chain has it.
    const auto* first = info ? static_cast<const XrBaseInStructure*>(info->next) : nullptr;
    while (first && first->type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
           first->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        first = first->next;
    }
    if (first && first->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        rt.calls.push_back("xrCreateSession:D3D12");
        const auto& binding = *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
        if (rt.refuse_d3d12_session || !binding.device || !binding.queue ||
            FAILED(binding.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rt.readback)))) {
            rt.readback.Reset();
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        rt.device12 = binding.device;
        rt.queue = binding.queue;
        rt.binding_next = binding.next;
    } else if (first && first->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        rt.calls.push_back("xrCreateSession:Vulkan");
        const auto& binding = *reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(first);
        if (!binding.device || !binding.physicalDevice || !binding.instance) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        rt.vk_device = binding.device;
        rt.vk_physical = binding.physicalDevice;
        rt.binding_next = binding.next;
    } else {
        rt.calls.push_back("xrCreateSession:other");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    rt.binding = first->type;
    *session = rt.session = Handle<XrSession>(++rt.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySession(XrSession session) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroySession");
    Check(session == rt.session, "the runtime's session is destroyed");
    if (rt.refuse_destroy_session) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    if (rt.on_destroy_session) {
        const auto hook = std::move(rt.on_destroy_session);
        rt.on_destroy_session = nullptr;
        hook();
    }
    // Its swapchains go with it.
    DropSession(rt);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtBeginSession(XrSession, const XrSessionBeginInfo* info) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrBeginSession");
    Check(info && info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
          "the runtime's session begins with the stereo view configuration");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEndSession(XrSession) {
    std::scoped_lock lock(g_runtime_mutex);
    g_runtime.calls.push_back("xrEndSession");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtAttachActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtSyncActions(XrSession, const XrActionsSyncInfo*) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateReferenceSpaces(XrSession, std::uint32_t capacity, std::uint32_t* count,
                                                XrReferenceSpaceType* spaces) {
    constexpr XrReferenceSpaceType kSpaces[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                XR_REFERENCE_SPACE_TYPE_STAGE};
    *count = 3;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(std::begin(kSpaces), std::end(kSpaces), spaces);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtGetReferenceSpaceBoundsRect(XrSession, XrReferenceSpaceType, XrExtent2Df* bounds) {
    *bounds = {};
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XrResult XRAPI_CALL RtCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* space) {
    std::scoped_lock lock(g_runtime_mutex);
    *space = Handle<XrSpace>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) {
    std::scoped_lock lock(g_runtime_mutex);
    *space = Handle<XrSpace>(++g_runtime.handles);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySpace(XrSpace) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    location->pose = {{0, 0, 0, 1}, {0, 0, 0}};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* state, std::uint32_t capacity,
                                  std::uint32_t* count, XrView* views) {
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT;
    for (std::uint32_t i = 0; i < std::min(capacity, 2u); ++i) {
        views[i].pose = {{0, 0, 0, 1}, {i ? 0.032f : -0.032f, 0, 0}};
        views[i].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    std::optional<std::chrono::steady_clock::time_point> until;
    XrTime time = 0;
    {
        std::scoped_lock lock(g_runtime_mutex);
        StandIn& rt = g_runtime;
        XrDuration refreshes = 1;
        if (rt.paced) {
            // The next refresh, or the first one after a wait that comes late.
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::nanoseconds period(kPeriod);
            if (rt.refresh == std::chrono::steady_clock::time_point{}) {
                rt.refresh = now;
            } else {
                rt.refresh += period;
                for (; rt.refresh < now; rt.refresh += period) {
                    ++refreshes;
                }
                until = rt.refresh;
            }
        }
        rt.time += kPeriod * refreshes;
        time = rt.time;
    }
    if (until) SleepUntil(*until);
    state->predictedDisplayTime = time;
    state->predictedDisplayPeriod = kPeriod;
    state->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtBeginFrame(XrSession, const XrFrameBeginInfo*) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEndFrame(XrSession, const XrFrameEndInfo* info) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrEndFrame");
    ShownFrame frame;
    frame.time = info->displayTime;
    for (std::uint32_t i = 0; i < info->layerCount; ++i) {
        const XrCompositionLayerBaseHeader* base = info->layers[i];
        if (base && base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            frame.quads.push_back(reinterpret_cast<const XrCompositionLayerQuad*>(base)->subImage.swapchain);
            frame.quad_flags.push_back(base->layerFlags);
        }
        if (!base || base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(base);
        frame.projection_flags.push_back(projection.layerFlags);
        Check(projection.viewCount == 2,
              "the runtime gets projections of two views, not " + std::to_string(projection.viewCount));
        for (std::uint32_t v = 0; v < projection.viewCount; ++v) {
            const XrCompositionLayerProjectionView& view = projection.views[v];
            ShownView shown;
            shown.swapchain = view.subImage.swapchain;
            shown.slice = view.subImage.imageArrayIndex;
            const auto* chain = static_cast<const XrBaseInStructure*>(view.next);
            shown.next_type = chain ? chain->type : XR_TYPE_UNKNOWN;
            for (; chain; chain = chain->next) {
                if (chain->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    shown.depth = true;
                    shown.depth_swapchain =
                        reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(chain)->subImage.swapchain;
                }
            }
            frame.views.push_back(shown);
        }
    }
    // A Direct3D 12 session's frames are read back (nothing is read on a
    // removed device: its fences read as done at once).
    if (rt.device12 && !rt.skip_readback && SUCCEEDED(rt.device12->GetDeviceRemovedReason())) {
        Sources sources;
        std::vector<int> depth_at(frame.views.size(), -1);
        for (std::size_t v = 0; v < frame.views.size(); ++v) {
            const ShownView& view = frame.views[v];
            const StandInSwapchain* chain = FindSwapchain(view.swapchain);
            if (!chain || !chain->released || chain->images12.empty()) {
                sources.clear();
                break;
            }
            sources.push_back({chain->images12[*chain->released].Get(), view.slice});
            if (rt.read_depth && view.depth) {
                const StandInSwapchain* depth = FindSwapchain(view.depth_swapchain);
                if (depth && depth->released && !depth->images12.empty()) {
                    depth_at[v] = static_cast<int>(sources.size());
                    sources.push_back({depth->images12[*depth->released].Get(), static_cast<std::uint32_t>(v)});
                }
            }
        }
        std::vector<Color> texels;
        std::vector<float> depth;
        if (!sources.empty() && ReadBack12(sources, frame, texels, depth)) {
            std::size_t at = 0;
            for (std::size_t v = 0; v < frame.views.size(); ++v) {
                frame.views[v].texel = texels[at++];
                if (depth_at[v] >= 0) {
                    frame.views[v].depth_value = depth[at++];
                }
            }
        }
    }
    rt.frames.push_back(frame);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateSwapchainFormats(XrSession, std::uint32_t capacity, std::uint32_t* count,
                                                 std::int64_t* formats) {
    std::scoped_lock lock(g_runtime_mutex);
    const bool vulkan = g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    const std::int64_t* list = vulkan ? kRuntimeFormatsVulkan : kRuntimeFormats12;
    const std::uint32_t size =
        static_cast<std::uint32_t>(vulkan ? std::size(kRuntimeFormatsVulkan) : std::size(kRuntimeFormats12));
    *count = size;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < size) return XR_ERROR_SIZE_INSUFFICIENT;
    std::copy(list, list + size, formats);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrCreateSwapchain");
    rt.swapchain_requests.push_back(*info);
    if (session != rt.session) return XR_ERROR_HANDLE_INVALID;
    if (rt.refuse_transfer_dst && (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) {
        return XR_ERROR_FEATURE_UNSUPPORTED;
    }
    const bool depth = (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    StandInSwapchain chain;
    chain.info = *info;
    chain.info.next = nullptr;
    const std::uint32_t array_size = std::max(1u, info->arraySize) * std::max(1u, info->faceCount);
    for (std::uint32_t i = 0; i < kImages; ++i) {
        if (rt.device12) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = info->width + rt.width_error;
            desc.Height = info->height;
            desc.DepthOrArraySize = static_cast<UINT16>(array_size);
            desc.MipLevels = static_cast<UINT16>(std::max(1u, info->mipCount));
            desc.Format = static_cast<DXGI_FORMAT>(info->format);
            desc.SampleDesc.Count = std::max(1u, info->sampleCount);
            desc.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            // Handed out as runtimes hand them out: colour images as render
            // targets, depth images as depth targets.
            ComPtr<ID3D12Resource> image;
            if (FAILED(rt.device12->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                    IID_PPV_ARGS(&image)))) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            image->SetName(L"VectorXR stand-in runtime swapchain image");
            chain.images12.push_back(image);
        } else if (rt.vk_device) {
            VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            create.imageType = VK_IMAGE_TYPE_2D;
            create.format = static_cast<VkFormat>(info->format);
            create.extent = {info->width, info->height, 1};
            create.mipLevels = std::max(1u, info->mipCount);
            create.arrayLayers = array_size;
            create.samples = static_cast<VkSampleCountFlagBits>(std::max(1u, info->sampleCount));
            create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                           ((info->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
                           ((info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0) |
                           ((info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
            create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage image = VK_NULL_HANDLE;
            if (g_vk.vkCreateImage(rt.vk_device, &create, nullptr, &image) != VK_SUCCESS) {
                FreeVulkanImages(chain);
                return XR_ERROR_RUNTIME_FAILURE;
            }
            VkMemoryRequirements requirements{};
            g_vk.vkGetImageMemoryRequirements(rt.vk_device, image, &requirements);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = MemoryType(rt.vk_physical, requirements.memoryTypeBits);
            VkDeviceMemory memory = VK_NULL_HANDLE;
            if (g_vk.vkAllocateMemory(rt.vk_device, &allocate, nullptr, &memory) != VK_SUCCESS ||
                g_vk.vkBindImageMemory(rt.vk_device, image, memory, 0) != VK_SUCCESS) {
                g_vk.vkDestroyImage(rt.vk_device, image, nullptr);
                if (memory) g_vk.vkFreeMemory(rt.vk_device, memory, nullptr);
                FreeVulkanImages(chain);
                return XR_ERROR_RUNTIME_FAILURE;
            }
            chain.images_vk.push_back(image);
            chain.memory_vk.push_back(memory);
        } else {
            return XR_ERROR_HANDLE_INVALID;
        }
    }
    *swapchain = Handle<XrSwapchain>(++rt.handles);
    rt.swapchains[*swapchain] = std::move(chain);
    rt.created.push_back(*swapchain);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtDestroySwapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(g_runtime_mutex);
    StandIn& rt = g_runtime;
    rt.calls.push_back("xrDestroySwapchain");
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (rt.on_destroy_swapchain) rt.on_destroy_swapchain(swapchain);
    FreeVulkanImages(*chain);
    rt.destroyed.push_back(swapchain);
    rt.swapchains.erase(swapchain);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtEnumerateSwapchainImages(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t* count,
                                                XrSwapchainImageBaseHeader* images) {
    std::scoped_lock lock(g_runtime_mutex);
    const StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    *count = static_cast<std::uint32_t>(chain->Count());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < chain->Count()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!chain->images12.empty()) {
        auto* out = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (std::size_t i = 0; i < chain->images12.size(); ++i) {
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
            out[i].texture = chain->images12[i].Get();
        }
    } else {
        auto* out = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (std::size_t i = 0; i < chain->images_vk.size(); ++i) {
            if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
            out[i].image = chain->images_vk[i];
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtAcquire(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    std::scoped_lock lock(g_runtime_mutex);
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (chain->acquired.size() >= chain->Count()) return XR_ERROR_CALL_ORDER_INVALID;
    *index = chain->next;
    chain->next = (chain->next + 1) % static_cast<std::uint32_t>(chain->Count());
    chain->acquired.push_back(*index);
    ++chain->acquires;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtWait(XrSwapchain swapchain, const XrSwapchainImageWaitInfo*) {
    std::scoped_lock lock(g_runtime_mutex);
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (chain->waits >= chain->acquires) return XR_ERROR_CALL_ORDER_INVALID;
    ++chain->waits;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL RtRelease(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) {
    std::scoped_lock lock(g_runtime_mutex);
    StandInSwapchain* chain = FindSwapchain(swapchain);
    if (!chain) return XR_ERROR_HANDLE_INVALID;
    if (chain->acquired.empty() || chain->releases >= chain->waits) return XR_ERROR_CALL_ORDER_INVALID;
    chain->released = chain->acquired.front();
    chain->acquired.pop_front();
    ++chain->releases;
    return XR_SUCCESS;
}

template <typename F>
PFN_xrVoidFunction Function(F* function) {
    return reinterpret_cast<PFN_xrVoidFunction>(function);
}

XrResult XRAPI_CALL RtGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    static const std::map<std::string, PFN_xrVoidFunction> kFunctions = {
        {"xrDestroyInstance", Function(RtDestroyInstance)},
        {"xrGetInstanceProperties", Function(RtGetInstanceProperties)},
        {"xrGetSystem", Function(RtGetSystem)},
        {"xrGetSystemProperties", Function(RtGetSystemProperties)},
        {"xrGetD3D12GraphicsRequirementsKHR", Function(RtGetD3D12Requirements)},
        {"xrEnumerateViewConfigurations", Function(RtEnumerateViewConfigurations)},
        {"xrGetViewConfigurationProperties", Function(RtGetViewConfigurationProperties)},
        {"xrEnumerateViewConfigurationViews", Function(RtEnumerateViewConfigurationViews)},
        {"xrEnumerateEnvironmentBlendModes", Function(RtEnumerateBlendModes)},
        {"xrPollEvent", Function(RtPollEvent)},
        {"xrCreateSession", Function(RtCreateSession)},
        {"xrDestroySession", Function(RtDestroySession)},
        {"xrBeginSession", Function(RtBeginSession)},
        {"xrEndSession", Function(RtEndSession)},
        {"xrAttachSessionActionSets", Function(RtAttachActionSets)},
        {"xrSyncActions", Function(RtSyncActions)},
        {"xrEnumerateReferenceSpaces", Function(RtEnumerateReferenceSpaces)},
        {"xrGetReferenceSpaceBoundsRect", Function(RtGetReferenceSpaceBoundsRect)},
        {"xrCreateReferenceSpace", Function(RtCreateReferenceSpace)},
        {"xrCreateActionSpace", Function(RtCreateActionSpace)},
        {"xrDestroySpace", Function(RtDestroySpace)},
        {"xrLocateSpace", Function(RtLocateSpace)},
        {"xrLocateViews", Function(RtLocateViews)},
        {"xrWaitFrame", Function(RtWaitFrame)},
        {"xrBeginFrame", Function(RtBeginFrame)},
        {"xrEndFrame", Function(RtEndFrame)},
        {"xrEnumerateSwapchainFormats", Function(RtEnumerateSwapchainFormats)},
        {"xrCreateSwapchain", Function(RtCreateSwapchain)},
        {"xrDestroySwapchain", Function(RtDestroySwapchain)},
        {"xrEnumerateSwapchainImages", Function(RtEnumerateSwapchainImages)},
        {"xrAcquireSwapchainImage", Function(RtAcquire)},
        {"xrWaitSwapchainImage", Function(RtWait)},
        {"xrReleaseSwapchainImage", Function(RtRelease)},
    };
    static const std::map<std::string, PFN_xrVoidFunction> kVulkanFunctions = {
        {"xrGetVulkanGraphicsRequirementsKHR", Function(RtGetVulkanRequirements)},
        {"xrGetVulkanGraphicsRequirements2KHR", Function(RtGetVulkanRequirements2)},
        {"xrGetVulkanInstanceExtensionsKHR", Function(RtGetVulkanInstanceExtensions)},
        {"xrGetVulkanDeviceExtensionsKHR", Function(RtGetVulkanDeviceExtensions)},
        {"xrGetVulkanGraphicsDeviceKHR", Function(RtGetVulkanGraphicsDevice)},
        {"xrGetVulkanGraphicsDevice2KHR", Function(RtGetVulkanGraphicsDevice2)},
        {"xrCreateVulkanInstanceKHR", Function(RtCreateVulkanInstance)},
        {"xrCreateVulkanDeviceKHR", Function(RtCreateVulkanDevice)},
    };
    *function = nullptr;
    std::scoped_lock lock(g_runtime_mutex);
    if (instance == XR_NULL_HANDLE) {
        // Before the instance: the loader's extension scan.
        if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0 && g_runtime.enumeration_offered) {
            *function = Function(RtEnumerateInstanceExtensions);
            return XR_SUCCESS;
        }
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (const auto it = kFunctions.find(name); it != kFunctions.end()) {
        *function = it->second;
        return XR_SUCCESS;
    }
    if (const auto it = kVulkanFunctions.find(name); it != kVulkanFunctions.end() && g_runtime.vulkan_offered) {
        *function = it->second;
        return XR_SUCCESS;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// --- Frame Generation's stand-in backend ------------------------------------

// The chain Frame Generation's backend was made from.
struct FrameGenChain {
    XrStructureType first{XR_TYPE_UNKNOWN};
    void* queue{nullptr};      // FrameGenRuntimeQueue::d3d12_queue
    const void* next{nullptr}; // the chain after it (or the chain itself)
    int made{0};               // backends made
    bool vulkan{false};        // a backend was ever asked for with a Vulkan binding in its chain
};
FrameGenChain g_framegen_chain;

class StandInGpu final : public depthxr::FrameGenGpu {
  public:
    const char* Name() const override { return "Stand-in"; }
    std::optional<depthxr::FrameGenOutputKind> Classify(std::int64_t) const override {
        return depthxr::FrameGenOutputKind::kSrgb8;
    }
    std::vector<std::int64_t> OutputFormats(depthxr::FrameGenOutputKind) const override {
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    }
    XrSwapchainUsageFlags RequiredSourceUsage() const override { return 0; }
    XrSwapchainUsageFlags OutputUsage() const override { return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; }
    bool Track(XrSwapchain, PFN_xrEnumerateSwapchainImages, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Capture(std::size_t, const depthxr::FrameGenImage&, const depthxr::FrameGenViewRequest&,
                 depthxr::FrameGenFlowQuality, std::string&) override {
        return true;
    }
    bool WaitIdle() override { return true; }
    bool Execute(const std::vector<depthxr::FrameGenViewSubmission>& views, const depthxr::FrameGenTuning&,
                 depthxr::FrameGenExecuteResult& result, std::string&) override {
        result.synthesized.assign(views.size(), true);
        return true;
    }
    void ResetHistory() override {}
    void Reset() override {}
};

void NoteChain(const void* chain) {
    g_framegen_chain = {XR_TYPE_UNKNOWN, nullptr, nullptr, g_framegen_chain.made + 1, g_framegen_chain.vulkan};
    for (auto* next = static_cast<const XrBaseInStructure*>(chain); next; next = next->next) {
        g_framegen_chain.vulkan = g_framegen_chain.vulkan || next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    }
    if (const auto* first = static_cast<const XrBaseInStructure*>(chain)) {
        g_framegen_chain.first = first->type;
        g_framegen_chain.next = chain;
        if (first->type == depthxr::kFrameGenRuntimeQueueType) {
            const auto* marker = static_cast<const depthxr::FrameGenRuntimeQueue*>(chain);
            g_framegen_chain.queue = marker->d3d12_queue;
            g_framegen_chain.next = marker->next;
        }
    }
}

std::unique_ptr<depthxr::FrameGenGpu> MakeStandInGpu(const void* chain, std::string&) {
    NoteChain(chain);
    return std::make_unique<StandInGpu>();
}

// The real backend, noting its chain.
std::unique_ptr<depthxr::FrameGenGpu> MakeRealGpu(const void* chain, std::string& error) {
    NoteChain(chain);
    return depthxr::CreateFrameGenGpu(chain, error);
}

// --- The application --------------------------------------------------------

// The application's OpenXR, from the layer's xrGetInstanceProcAddr (the
// extensions' functions are null where the layer hands none out).
struct Api {
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrGetSystem get_system{};
    PFN_xrGetVulkanGraphicsRequirementsKHR vulkan_requirements{};
    PFN_xrGetVulkanGraphicsRequirements2KHR vulkan_requirements2{};
    PFN_xrGetVulkanInstanceExtensionsKHR vulkan_instance_extensions{};
    PFN_xrGetVulkanDeviceExtensionsKHR vulkan_device_extensions{};
    PFN_xrGetVulkanGraphicsDeviceKHR vulkan_device{};
    PFN_xrGetVulkanGraphicsDevice2KHR vulkan_device2{};
    PFN_xrCreateVulkanInstanceKHR create_vulkan_instance{};
    PFN_xrCreateVulkanDeviceKHR create_vulkan_device{};
    PFN_xrGetD3D12GraphicsRequirementsKHR d3d12_requirements{};
    PFN_xrPollEvent poll_event{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrCreateReferenceSpace create_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrEnumerateSwapchainFormats enumerate_formats{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrLocateViews locate_views{};
};

template <typename T>
T LayerFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    Check(XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function)) && function,
          std::string("the layer hands out ") + name);
    return reinterpret_cast<T>(function);
}

template <typename T>
T OptionalLayerFunction(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function)) ? reinterpret_cast<T>(function) : nullptr;
}

Api Load(XrInstance instance) {
    Api api;
    api.instance = instance;
    api.destroy_instance = LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance");
    api.get_system = LayerFunction<PFN_xrGetSystem>(instance, "xrGetSystem");
    api.vulkan_requirements =
        OptionalLayerFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(instance, "xrGetVulkanGraphicsRequirementsKHR");
    api.vulkan_requirements2 =
        OptionalLayerFunction<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
    api.vulkan_instance_extensions =
        OptionalLayerFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(instance, "xrGetVulkanInstanceExtensionsKHR");
    api.vulkan_device_extensions =
        OptionalLayerFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR");
    api.vulkan_device = OptionalLayerFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(instance, "xrGetVulkanGraphicsDeviceKHR");
    api.vulkan_device2 =
        OptionalLayerFunction<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
    api.create_vulkan_instance = OptionalLayerFunction<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
    api.create_vulkan_device = OptionalLayerFunction<PFN_xrCreateVulkanDeviceKHR>(instance, "xrCreateVulkanDeviceKHR");
    api.d3d12_requirements =
        OptionalLayerFunction<PFN_xrGetD3D12GraphicsRequirementsKHR>(instance, "xrGetD3D12GraphicsRequirementsKHR");
    api.poll_event = LayerFunction<PFN_xrPollEvent>(instance, "xrPollEvent");
    api.create_session = LayerFunction<PFN_xrCreateSession>(instance, "xrCreateSession");
    api.destroy_session = LayerFunction<PFN_xrDestroySession>(instance, "xrDestroySession");
    api.begin_session = LayerFunction<PFN_xrBeginSession>(instance, "xrBeginSession");
    api.end_session = LayerFunction<PFN_xrEndSession>(instance, "xrEndSession");
    api.create_space = LayerFunction<PFN_xrCreateReferenceSpace>(instance, "xrCreateReferenceSpace");
    api.destroy_space = LayerFunction<PFN_xrDestroySpace>(instance, "xrDestroySpace");
    api.enumerate_formats = LayerFunction<PFN_xrEnumerateSwapchainFormats>(instance, "xrEnumerateSwapchainFormats");
    api.create_swapchain = LayerFunction<PFN_xrCreateSwapchain>(instance, "xrCreateSwapchain");
    api.destroy_swapchain = LayerFunction<PFN_xrDestroySwapchain>(instance, "xrDestroySwapchain");
    api.enumerate_images = LayerFunction<PFN_xrEnumerateSwapchainImages>(instance, "xrEnumerateSwapchainImages");
    api.acquire = LayerFunction<PFN_xrAcquireSwapchainImage>(instance, "xrAcquireSwapchainImage");
    api.wait = LayerFunction<PFN_xrWaitSwapchainImage>(instance, "xrWaitSwapchainImage");
    api.release = LayerFunction<PFN_xrReleaseSwapchainImage>(instance, "xrReleaseSwapchainImage");
    api.wait_frame = LayerFunction<PFN_xrWaitFrame>(instance, "xrWaitFrame");
    api.begin_frame = LayerFunction<PFN_xrBeginFrame>(instance, "xrBeginFrame");
    api.end_frame = LayerFunction<PFN_xrEndFrame>(instance, "xrEndFrame");
    api.locate_views = LayerFunction<PFN_xrLocateViews>(instance, "xrLocateViews");
    return api;
}

// xrCreateInstance as the loader makes it with the layer enabled: the
// stand-in runtime is the next (and last) of the chain.
XrResult CreateInstance(const std::vector<const char*>& extensions, XrInstance& instance) {
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(info.applicationInfo.applicationName, "VectorXR Vulkan bridge test", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "Vulkan", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.enabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    depthxr::XrApiLayerNextInfo next{};
    next.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next.structVersion = depthxr::XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next.structSize = sizeof(next);
    std::strncpy(next.layerName, "XR_APILAYER_TEST_stand_in_runtime", XR_MAX_API_LAYER_NAME_SIZE - 1);
    next.nextGetInstanceProcAddr = RtGetInstanceProcAddr;
    next.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
    depthxr::XrApiLayerCreateInfo create{};
    create.structType = depthxr::XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    create.structVersion = depthxr::XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    create.structSize = sizeof(create);
    create.nextInfo = &next;
    instance = XR_NULL_HANDLE;
    return xrCreateApiLayerInstance(&info, &create, &instance);
}

struct App {
    Api api;
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};
};

App StartApp(const std::vector<const char*>& extensions) {
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult result = CreateInstance(extensions, instance);
    Check(result == XR_SUCCESS && instance != XR_NULL_HANDLE,
          "xrCreateInstance through the layer: " + std::to_string(result));
    App app;
    app.api = Load(instance);
    XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
    get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    Check(app.api.get_system(instance, &get, &app.system) == XR_SUCCESS, "xrGetSystem");
    Check(Contains(Peer::Log(), "Loaded config from"), "the test's settings were loaded");
    return app;
}

void BeginSession(App& app) {
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1;
    Check(app.api.create_space(app.session, &space, &app.space) == XR_SUCCESS, "xrCreateReferenceSpace");
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    Check(app.api.poll_event(app.api.instance, &event) == XR_EVENT_UNAVAILABLE, "xrPollEvent");
    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    Check(app.api.begin_session(app.session, &begin) == XR_SUCCESS, "xrBeginSession");
}

// Its session (when it has one) and instance destroyed.
void StopApp(App& app) {
    if (app.space != XR_NULL_HANDLE) {
        Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
        app.space = XR_NULL_HANDLE;
    }
    if (app.session != XR_NULL_HANDLE) {
        Check(app.api.destroy_session(app.session) == XR_SUCCESS, "xrDestroySession");
        app.session = XR_NULL_HANDLE;
    }
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
}

// How the game makes its Vulkan instance and device.
struct GameOptions {
    int enable{2};                      // XR_KHR_vulkan_enable's version
    std::uint32_t api{VK_API_VERSION_1_3};
    const void* device_chain{nullptr};  // the application's VkDeviceCreateInfo chain
};

// The application's VkDeviceCreateInfo as it made it, to check nothing of
// it was written.
struct DeviceCreateSnapshot {
    VkDeviceCreateInfo info{};
    std::vector<const char*> names;
};

// The game's Vulkan instance and device, made as XR_KHR_vulkan_enable(2)
// says (enable2: by the runtime, through the layer; enable: by the game, with
// the extension lists the layer hands out).
VulkanGame MakeVulkanGame(App& app, const GameOptions& options = {}) {
    VulkanGame game;
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    std::vector<const char*> layers;
    std::vector<const char*> instance_extensions;
    if (g_validation) {
        layers.push_back(vulkan_test::kValidationLayer);
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    Names listed_instance; // enable: the layer's lists
    Names listed_device;
    const auto list = [](auto function, XrInstance instance, XrSystemId system, const char* what) {
        Check(function != nullptr, std::string("the layer hands out ") + what);
        std::uint32_t size = 0;
        Check(function(instance, system, 0, &size, nullptr) == XR_SUCCESS && size > 0, std::string(what) + " counted");
        std::string text(size, '\0');
        Check(function(instance, system, size, &size, text.data()) == XR_SUCCESS, what);
        text.resize(std::strlen(text.c_str()));
        return Split(text);
    };
    if (options.enable == 1) {
        Check(app.api.vulkan_requirements &&
                  app.api.vulkan_requirements(app.api.instance, app.system, &requirements) == XR_SUCCESS,
              "xrGetVulkanGraphicsRequirementsKHR");
        listed_instance = list(app.api.vulkan_instance_extensions, app.api.instance, app.system,
                               "xrGetVulkanInstanceExtensionsKHR");
    } else {
        Check(app.api.vulkan_requirements2 &&
                  app.api.vulkan_requirements2(app.api.instance, app.system, &requirements) == XR_SUCCESS,
              "xrGetVulkanGraphicsRequirements2KHR");
    }
    std::vector<const char*> instance_names = instance_extensions;
    for (const std::string& name : listed_instance) instance_names.push_back(name.c_str());

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "VectorXR Vulkan bridge test";
    application.apiVersion = options.api;
    VkDebugUtilsMessengerCreateInfoEXT messenger = vulkan_test::MessengerInfo(&g_vk_messages);
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pNext = g_validation ? &messenger : nullptr;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    instance_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_names.size());
    instance_info.ppEnabledExtensionNames = instance_names.empty() ? nullptr : instance_names.data();
    if (options.enable == 1) {
        CheckVk(g_vk.vkCreateInstance(&instance_info, nullptr, &game.instance), "the game's vkCreateInstance");
    } else {
        Check(app.api.create_vulkan_instance != nullptr, "the layer hands out xrCreateVulkanInstanceKHR");
        XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        create.systemId = app.system;
        create.pfnGetInstanceProcAddr = GameGetInstanceProcAddr;
        create.vulkanCreateInfo = &instance_info;
        VkResult result = VK_ERROR_UNKNOWN;
        Check(app.api.create_vulkan_instance(app.api.instance, &create, &game.instance, &result) == XR_SUCCESS,
              "xrCreateVulkanInstanceKHR");
        CheckVk(result, "the game's instance made by the runtime");
    }
    LoadInstanceFunctions(game.instance);
    MakeMessenger(game);

    if (options.enable == 1) {
        Check(app.api.vulkan_device &&
                  app.api.vulkan_device(app.api.instance, app.system, game.instance, &game.physical) == XR_SUCCESS,
              "xrGetVulkanGraphicsDeviceKHR");
        listed_device =
            list(app.api.vulkan_device_extensions, app.api.instance, app.system, "xrGetVulkanDeviceExtensionsKHR");
    } else {
        XrVulkanGraphicsDeviceGetInfoKHR get{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        get.systemId = app.system;
        get.vulkanInstance = game.instance;
        Check(app.api.vulkan_device2 &&
                  app.api.vulkan_device2(app.api.instance, &get, &game.physical) == XR_SUCCESS,
              "xrGetVulkanGraphicsDevice2KHR");
    }
    game.family = GraphicsFamily(game.physical);
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = game.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    std::vector<const char*> device_names;
    for (const std::string& name : listed_device) device_names.push_back(name.c_str());
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = options.device_chain;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue;
    device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_names.size());
    device_info.ppEnabledExtensionNames = device_names.empty() ? nullptr : device_names.data();
    const DeviceCreateSnapshot before{device_info, device_names};
    if (options.enable == 1) {
        CheckVk(g_vk.vkCreateDevice(game.physical, &device_info, nullptr, &game.device), "the game's vkCreateDevice");
    } else {
        Check(app.api.create_vulkan_device != nullptr, "the layer hands out xrCreateVulkanDeviceKHR");
        XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        create.systemId = app.system;
        create.pfnGetInstanceProcAddr = GameGetInstanceProcAddr;
        create.vulkanPhysicalDevice = game.physical;
        create.vulkanCreateInfo = &device_info;
        VkResult result = VK_ERROR_UNKNOWN;
        Check(app.api.create_vulkan_device(app.api.instance, &create, &game.device, &result) == XR_SUCCESS,
              "xrCreateVulkanDeviceKHR");
        CheckVk(result, "the game's device made by the runtime");
    }
    Check(std::memcmp(&before.info, &device_info, sizeof(device_info)) == 0 && before.names == device_names,
          "the application's VkDeviceCreateInfo is not written");
    LoadDeviceFunctions(game.device);
    FinishGame(game);
    return game;
}

// The application's session create info: its Vulkan binding first, a
// structure of its own after it.
struct SessionCreate {
    XrBaseInStructure application_struct{kApplicationStructType};
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    SessionCreate(const VulkanGame& game, XrSystemId system) {
        binding.next = &application_struct;
        binding.instance = game.instance;
        binding.physicalDevice = game.physical;
        binding.device = game.device;
        binding.queueFamilyIndex = game.family;
        binding.queueIndex = 0;
        info.next = &binding;
        info.systemId = system;
    }
    SessionCreate(const SessionCreate&) = delete;
    SessionCreate& operator=(const SessionCreate&) = delete;
};

XrSwapchainCreateInfo ColorInfo(std::int64_t format = VK_FORMAT_R8G8B8A8_SRGB, std::uint32_t samples = 1,
                                std::uint32_t array_size = 1) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // The game clears its images by transfer.
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = samples;
    info.width = g_image_width;
    info.height = g_image_height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    return info;
}

XrSwapchainCreateInfo DepthInfo(std::int64_t format, std::uint32_t array_size, std::uint32_t samples = 1) {
    XrSwapchainCreateInfo info = ColorInfo(format, samples, array_size);
    info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    return info;
}

// A swapchain the application renders into, with its images: VkImage
// handles (Vulkan) or ID3D12Resource pointers (Direct3D 12).
struct Target {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<std::uint64_t> images;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
};

template <typename Image>
std::vector<std::uint64_t> Images(App& app, XrSwapchain swapchain, XrStructureType type) {
    std::uint32_t count = 0;
    const XrResult result = app.api.enumerate_images(swapchain, 0, &count, nullptr);
    Check(result == XR_SUCCESS && count == kImages,
          "xrEnumerateSwapchainImages counts the images: " + std::to_string(count));
    std::vector<Image> images(count, {type});
    Check(app.api.enumerate_images(swapchain, count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) == XR_SUCCESS,
          "xrEnumerateSwapchainImages");
    std::vector<std::uint64_t> handles;
    for (const Image& image : images) {
        if constexpr (std::is_same_v<Image, XrSwapchainImageVulkanKHR>) {
            Check(image.image != VK_NULL_HANDLE, "every image is a VkImage");
            handles.push_back(reinterpret_cast<std::uint64_t>(image.image));
        } else {
            Check(image.texture != nullptr, "every image is a texture");
            handles.push_back(reinterpret_cast<std::uint64_t>(image.texture));
        }
    }
    return handles;
}

Target MakeTarget(App& app, const XrSwapchainCreateInfo& info, const std::string& what, bool d3d12 = false) {
    Target target;
    target.info = info;
    const XrResult result = app.api.create_swapchain(app.session, &info, &target.swapchain);
    Check(result == XR_SUCCESS && target.swapchain != XR_NULL_HANDLE,
          what + ": xrCreateSwapchain " + std::to_string(result));
    target.images = d3d12 ? Images<XrSwapchainImageD3D12KHR>(app, target.swapchain, XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)
                          : Images<XrSwapchainImageVulkanKHR>(app, target.swapchain, XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR);
    return target;
}

std::uint32_t Acquire(App& app, const Target& target) {
    std::uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    Check(app.api.acquire(target.swapchain, &acquire, &index) == XR_SUCCESS && index < target.images.size(),
          "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    Check(app.api.wait(target.swapchain, &wait) == XR_SUCCESS, "xrWaitSwapchainImage");
    return index;
}

void Release(App& app, const Target& target) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(app.api.release(target.swapchain, &release) == XR_SUCCESS, "xrReleaseSwapchainImage");
}

// The game's rendering of an image it holds: a Vulkan game's or a Direct3D 12
// game's (or none: a game whose images the test leaves alone).
struct Renderer {
    VulkanGame* vulkan{nullptr};
    D3D12Game* d3d12{nullptr};
    void Clear(const Target& target, std::uint32_t index, const Color& color) const {
        const std::uint32_t layers = std::max(1u, target.info.arraySize);
        if (vulkan) {
            ClearVulkan(*vulkan, reinterpret_cast<VkImage>(target.images[index]), layers, color,
                        target.info.format == VK_FORMAT_R8G8B8A8_SRGB || target.info.format == VK_FORMAT_B8G8R8A8_SRGB);
        } else if (d3d12) {
            ClearD3D12(*d3d12, reinterpret_cast<ID3D12Resource*>(target.images[index]), color, 0.0f);
        }
    }
    void ClearDepth(const Target& target, std::uint32_t index, float depth) const {
        const std::uint32_t layers = std::max(1u, target.info.arraySize);
        if (vulkan) {
            ClearVulkanDepth(*vulkan, reinterpret_cast<VkImage>(target.images[index]), layers, depth);
        } else if (d3d12) {
            ClearD3D12(*d3d12, reinterpret_cast<ID3D12Resource*>(target.images[index]), {}, depth);
        }
    }
};

struct Eye {
    const Target* target{nullptr};
    std::uint32_t slice{0};
};

// What Frame does beyond rendering and submitting a frame.
struct FrameOptions {
    const Target* depth{nullptr}; // a depth swapchain (two slices), cleared to `depth_value` and chained to the views
    float depth_value{0.5f};
    // The game's queue waits for the gate from just before the LAST colour
    // target's release on (after the others' releases): that image alone is
    // not written yet. The stand-in opens it once its readback waits all the same.
    Gate* hold{nullptr};
    // As `hold`, but nothing opens it: the frame ends with the game's queue held.
    Gate* stall{nullptr};
    XrTime* display_time{nullptr};
    // A quad layer over the projection (a game's interface): its swapchain
    // acquired, cleared to `quad_color` and released after the colour
    // targets, every frame.
    const Target* quad{nullptr};
    Color quad_color{255, 255, 255, 255};
    XrCompositionLayerFlags layer_flags{0}; // the projection's and the quad's
};

// One application frame: each colour target acquired, waited, rendered in
// its colour and released, in the order given; then a stereo projection of
// `eyes`, each view chaining its slice of the depth target (and a structure
// of the application's after it) when there is one.
XrResult Frame(App& app, const Renderer& renderer, const std::vector<const Target*>& targets,
               const std::vector<Color>& colors, const std::array<Eye, 2>& eyes, const FrameOptions& options = {}) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Check(app.api.wait_frame(app.session, &wait_info, &state) == XR_SUCCESS, "xrWaitFrame");
    if (options.display_time) *options.display_time = state.predictedDisplayTime;
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    Check(app.api.begin_frame(app.session, &begin_info) == XR_SUCCESS, "xrBeginFrame");
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = state.predictedDisplayTime;
    locate.space = app.space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> located{};
    for (XrView& view : located) view.type = XR_TYPE_VIEW;
    std::uint32_t located_count = 0;
    Check(app.api.locate_views(app.session, &locate, &view_state, 2, &located_count, located.data()) == XR_SUCCESS &&
              located_count == 2,
          "xrLocateViews");

    std::vector<std::uint32_t> indices;
    for (const Target* target : targets) {
        indices.push_back(Acquire(app, *target));
    }
    const Target* depth = options.depth;
    const std::uint32_t depth_index = depth ? Acquire(app, *depth) : 0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        renderer.Clear(*targets[i], indices[i], colors[i]);
    }
    if (options.quad) {
        renderer.Clear(*options.quad, Acquire(app, *options.quad), options.quad_color);
    }
    if (depth) {
        renderer.ClearDepth(*depth, depth_index, options.depth_value);
    }
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if ((options.hold || options.stall) && i + 1 == targets.size()) {
            (options.hold ? options.hold : options.stall)->HoldGame();
        }
        Release(app, *targets[i]);
    }
    if (depth) {
        Release(app, *depth);
    }
    if (options.quad) {
        Release(app, *options.quad);
    }

    XrBaseInStructure after_depth{kApplicationStructType};
    std::array<XrCompositionLayerDepthInfoKHR, 2> depth_infos{};
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (std::uint32_t eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose = located[eye].pose;
        views[eye].fov = located[eye].fov;
        views[eye].subImage.swapchain = eyes[eye].target->swapchain;
        views[eye].subImage.imageRect = {{0, 0},
                                         {static_cast<std::int32_t>(eyes[eye].target->info.width),
                                          static_cast<std::int32_t>(eyes[eye].target->info.height)}};
        views[eye].subImage.imageArrayIndex = eyes[eye].slice;
        if (depth) {
            depth_infos[eye] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
            depth_infos[eye].next = &after_depth;
            depth_infos[eye].subImage.swapchain = depth->swapchain;
            depth_infos[eye].subImage.imageRect = views[eye].subImage.imageRect;
            depth_infos[eye].subImage.imageArrayIndex = eye;
            depth_infos[eye].minDepth = 0.0f;
            depth_infos[eye].maxDepth = 1.0f;
            depth_infos[eye].nearZ = 0.05f;
            depth_infos[eye].farZ = 1000.0f;
            views[eye].next = &depth_infos[eye];
        }
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.layerFlags = options.layer_flags;
    projection.space = app.space;
    projection.viewCount = 2;
    projection.views = views.data();
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (options.quad) {
        quad.layerFlags = options.layer_flags;
        quad.space = app.space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = options.quad->swapchain;
        quad.subImage.imageRect = {{0, 0},
                                   {static_cast<std::int32_t>(options.quad->info.width),
                                    static_cast<std::int32_t>(options.quad->info.height)}};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position.z = -1.0f;
        quad.size = {1.0f, 0.5f};
    }
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = options.quad ? 2 : 1;
    end.layers = layers;
    {
        std::scoped_lock lock(g_runtime_mutex);
        g_runtime.hold = options.hold;
    }
    const XrResult result = app.api.end_frame(app.session, &end);
    std::scoped_lock lock(g_runtime_mutex);
    if (g_runtime.hold) {
        // The stand-in never read the frame back: let the game run on.
        g_runtime.hold->Open();
        g_runtime.hold = nullptr;
        Check(!options.hold || result != XR_SUCCESS, "the runtime read back a frame the game was held back for");
    }
    return result;
}

// An image rendered and released outside a frame.
void Render(App& app, const Renderer& renderer, const Target& target, const Color& color) {
    renderer.Clear(target, Acquire(app, target), color);
    Release(app, target);
}

// --- The cases --------------------------------------------------------------

// The GPU the cases run on: a hardware Direct3D 12 adapter with a Vulkan
// device of the same LUID.
struct Gpu {
    LUID luid{};
    std::string name;
};
Gpu g_gpu;

// The test's own reference to the Direct3D 12 device on the game's adapter,
// from the start of each case to its end. Direct3D 12 makes one device per
// adapter in a process, so the bridge's device (the session's) is this one,
// and what its debug layer reported is still there to check once the case's
// teardown has let everything of the layer's go.
ComPtr<ID3D12Device> g_device12;
std::string g_case; // begun, and its debug layers not yet checked
bool g_d3d12_debug = false;

ComPtr<ID3D12Device> MakeDevice12() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(g_gpu.luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        return nullptr;
    }
    ComPtr<ID3D12InfoQueue> queue;
    if (SUCCEEDED(device.As(&queue))) {
        StoreErrorsOnly(queue.Get());
    }
    return device;
}

bool SameObject(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> identity_a;
    ComPtr<IUnknown> identity_b;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&identity_a))) &&
           SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&identity_b))) && identity_a.Get() == identity_b.Get();
}

// Neither validation reported an error since the last check.
void CheckValidation(const std::string& when) {
    const std::string errors = ValidationMessages12(g_device12 ? g_device12.Get() : g_runtime.device12.Get());
    Check(errors.empty(), g_case + " (" + when + "): the Direct3D 12 debug layer reported\n" + errors);
    CheckVulkanValidation(g_case + " (" + when + ")");
}

// What a case's settings turn on beyond the defaults.
struct CaseSettings {
    bool framegen{true};
    bool debug_log{false};
    // Frame Generation's (its default when empty). The cases were written when
    // Auto was the default, so they keep it unless they choose another.
    std::string frame_multiplier{"auto"};
    std::string synthesis_mode;   // Frame Generation's (its default when empty)
};

void WriteSettings(const CaseSettings& case_settings) {
    std::ofstream settings(g_root / "settings.json", std::ios::trunc);
    settings << R"({"version": 3, "core": {"enabled": true, "logLevel": ")"
             << (case_settings.debug_log ? "debug" : "info")
             << R"(", "trackSeenApps": false}, "applications": [], "modules": {"depthxr": {"enabled": false}, )"
             << R"("pivotxr": {"enabled": false}, "quadviews": {"enabled": false}, )"
             << R"("turbo": {"enabled": false}, "framegen": {"enabled": )" << (case_settings.framegen ? "true" : "false");
    if (!case_settings.frame_multiplier.empty() || !case_settings.synthesis_mode.empty()) {
        settings << R"(, "defaults": {)";
        if (!case_settings.frame_multiplier.empty()) {
            settings << R"("frameMultiplier": ")" << case_settings.frame_multiplier << R"(")"
                     << (case_settings.synthesis_mode.empty() ? "" : ", ");
        }
        if (!case_settings.synthesis_mode.empty()) {
            settings << R"("synthesisMode": ")" << case_settings.synthesis_mode << R"(")";
        }
        settings << "}";
    }
    settings << "}}}\n";
}

// The settings changed while the game runs, seen by the layer at once.
void ChangeSettings(const CaseSettings& case_settings) {
    WriteSettings(case_settings);
    Peer::ReloadConfig();
}

// A fresh stand-in runtime, a log of the case's own, and the test's settings.
// Every case ends with EndCase.
void BeginCase(const std::string& name, const CaseSettings& case_settings = {}) {
    Check(g_case.empty(), name + ": the case before it (" + g_case + ") ended with its validation checked");
    Check(Peer::Idle(), name + ": nothing is left of the cases before it");
    std::cout << "case " << name << std::endl;
    g_case = name;
    g_device12 = MakeDevice12();
    // The runtime's clock runs on across sessions, as a runtime's does.
    const XrTime time = g_runtime.time;
    g_runtime = StandIn{};
    g_runtime.time = time;
    g_runtime.luid = g_gpu.luid;
    g_framegen_chain = {};
    g_final = {};
    g_image_width = kWidth;
    g_image_height = kHeight;
    SetEnv("VECTORXR_LOG_PATH", (g_root / "logs" / name / "vectorxr.log").string());
    WriteSettings(case_settings);
}

void BeginCase(const std::string& name, bool framegen) {
    CaseSettings settings;
    settings.framegen = framegen;
    BeginCase(name, settings);
}

// After the case's teardown: neither validation reported an error during
// the case. `device_removed`: the case removed the Direct3D 12 device, and
// what its debug layer reported since the last check is the removal's.
void EndCase(bool device_removed = false) {
    Check(Peer::Idle(), g_case + ": nothing is left of the case");
    const std::string errors = ValidationMessages12(g_device12 ? g_device12.Get() : nullptr);
    Check(device_removed || errors.empty(), g_case + " (after its teardown): the Direct3D 12 debug layer reported\n" + errors);
    CheckVulkanValidation(g_case + " (after its teardown)");
    g_device12.Reset();
    g_case.clear();
}

const XrGraphicsBindingD3D12KHR* BridgeBinding() {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(Peer::Bridge().Binding());
}

// Why the layer left the case's Vulkan session unbridged (for failure messages).
std::string Unbridged(const std::string& log) {
    return LineWith(log, kUnbridgedLine);
}

// One instance of an application asking for `extensions`: the result and the
// extensions the runtime was asked for, attempt by attempt. A created
// instance is destroyed again.
XrResult InstanceAttempts(const std::vector<const char*>& extensions, std::vector<Names>& attempts, std::string& log) {
    XrInstance instance = XR_NULL_HANDLE;
    const XrResult result = CreateInstance(extensions, instance);
    attempts = g_runtime.instance_attempts;
    log = Peer::Log();
    if (XR_SUCCEEDED(result)) {
        Check(LayerFunction<PFN_xrDestroyInstance>(instance, "xrDestroyInstance")(instance) == XR_SUCCESS,
              "xrDestroyInstance");
    }
    Check(Peer::Idle(), "nothing is left of the instance");
    return result;
}

// 1. XR_KHR_D3D12_enable for a Vulkan application at xrCreateInstance.
void TestInstanceExtensions() {
    std::vector<Names> attempts;
    std::string log;
    const std::string added = "Frame Generation's Vulkan bridge: XR_KHR_D3D12_enable added to the instance's extensions";

    BeginCase("instance-enable2");
    XrResult result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension, kD3D12Extension} &&
              Contains(log, added + "."),
          "added for XR_KHR_vulkan_enable2 when the runtime offers it: " + Text(attempts));
    EndCase();

    BeginCase("instance-enable");
    result = InstanceAttempts({kVulkanExtension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkanExtension, kD3D12Extension} &&
              Contains(log, added + "."),
          "added for XR_KHR_vulkan_enable: " + Text(attempts));
    EndCase();

    BeginCase("instance-untrusted-list");
    g_runtime.extensions = {"XR_KHR_composition_layer_depth"};
    result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension, kD3D12Extension} &&
              Contains(log, added + " (the runtime's extension list before the instance was not believable)."),
          "added when the runtime's list lacks the application's own extension: " + Text(attempts));
    EndCase();

    BeginCase("instance-framegen-off", false);
    result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension} &&
              !Contains(log, "Frame Generation's Vulkan bridge"),
          "not added without Frame Generation: " + Text(attempts));
    EndCase();

    BeginCase("instance-bridge-off");
    SetEnv("VECTORXR_VULKAN_BRIDGE", "off");
    result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    SetEnv("VECTORXR_VULKAN_BRIDGE", "");
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension} &&
              !Contains(log, "Frame Generation's Vulkan bridge"),
          "not added with VECTORXR_VULKAN_BRIDGE=off: " + Text(attempts));
    EndCase();

    BeginCase("instance-d3d11-bridge-off");
    SetEnv("VECTORXR_D3D11_BRIDGE", "off");
    result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    SetEnv("VECTORXR_D3D11_BRIDGE", "");
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension, kD3D12Extension},
          "the Direct3D 11 bridge's switch is not the Vulkan bridge's: " + Text(attempts));
    EndCase();

    BeginCase("instance-no-d3d12");
    g_runtime.extensions = {kVulkanExtension, kVulkan2Extension};
    result = InstanceAttempts({kVulkan2Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension} &&
              Contains(log, "Frame Generation's Vulkan bridge is unavailable: the runtime does not offer "
                            "XR_KHR_D3D12_enable; Frame Generation is off for Vulkan sessions."),
          "not added when a believable list lacks it: " + Text(attempts));
    EndCase();

    BeginCase("instance-own-d3d12");
    result = InstanceAttempts({kVulkan2Extension, kD3D12Extension}, attempts, log);
    Check(result == XR_SUCCESS && attempts.size() == 1 && attempts[0] == Names{kVulkan2Extension, kD3D12Extension},
          "not added twice: " + Text(attempts));
    EndCase();
}

// 1b. The runtime refuses the instance with XR_KHR_D3D12_enable: the next
// attempt drops it, and the Vulkan session that follows has no Frame
// Generation (and gets nothing of the bridge's).
void TestRefusedInstance() {
    BeginCase("instance-refused");
    g_runtime.refuse_d3d12_instance = true;
    App app = StartApp({kVulkan2Extension});
    const std::vector<Names> attempts = g_runtime.instance_attempts;
    Check(attempts.size() == 2 && attempts[0] == Names{kVulkan2Extension, kD3D12Extension} &&
              attempts[1] == Names{kVulkan2Extension},
          "the retry drops XR_KHR_D3D12_enable: " + Text(attempts));
    Check(Contains(Peer::Log(), "Frame Generation's Vulkan bridge: XR_KHR_D3D12_enable left out after the runtime "
                                "refused an extension") &&
              Contains(Peer::Log(), "Frame Generation is off for Vulkan sessions."),
          "the refusal is logged");
    Check(!Peer::HooksAddForBridge(), "the hooks add nothing for the bridge on this instance");
    VulkanGame game = MakeVulkanGame(app);
    Check(g_runtime.given_get_instance_proc_addr == GameGetInstanceProcAddr,
          "the runtime gets the game's own vkGetInstanceProcAddr");
    Check(!Has(g_final.device_extensions, "VK_KHR_external_memory_win32"), "nothing is added to the game's device");
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR &&
              g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0 && !Peer::Bridge().Active(),
          "the session is the game's Vulkan one");
    const std::string log = Peer::Log();
    Check(Contains(log, std::string(kUnbridgedLine) + "the instance has no XR_KHR_D3D12_enable"),
          "the log tells why: " + Unbridged(log));
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.state == depthxr::FrameGenState::kUnavailable && Contains(status.reason, "Vulkan bridge"),
          "Frame Generation is unavailable: " + status.reason);
    StopApp(app);
    DestroyGame(game);
    EndCase();
}

// 2. XR_KHR_vulkan_enable2: the layer's vkGetInstanceProcAddr sees the create
// infos the runtime finally passes to Vulkan, and adds what the bridge needs
// there, whatever the runtime added, never writing the game's memory.
void TestEnable2CreateInfos() {
    const std::vector<std::string> bridge_instance(depthxr::VulkanBridge::InstanceExtensions().begin(),
                                                   depthxr::VulkanBridge::InstanceExtensions().end());
    const std::vector<std::string> bridge_device(depthxr::VulkanBridge::DeviceExtensions().begin(),
                                                 depthxr::VulkanBridge::DeviceExtensions().end());
    const auto once = [](const Names& names, const std::string& name) {
        return std::count(names.begin(), names.end(), name) == 1;
    };

    {
        // Nothing in the game's chain: timeline semaphores are added, ahead
        // of the game's own structures.
        BeginCase("enable2-create-infos");
        App app = StartApp({kVulkan2Extension});
        Check(Peer::HooksAddForBridge(), "the hooks add for the bridge on this instance");
        VkPhysicalDevice16BitStorageFeatures storage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
        GameOptions options;
        options.device_chain = &storage;
        VulkanGame game = MakeVulkanGame(app, options);
        Check(g_runtime.given_get_instance_proc_addr != GameGetInstanceProcAddr,
              "the runtime gets the layer's vkGetInstanceProcAddr, not the game's");
        Check(g_final.instances == 1 && g_final.devices == 1, "the game's own vkCreateInstance and vkCreateDevice ran once");
        for (const std::string& name : bridge_instance) {
            Check(once(g_final.instance_extensions, name), "the final instance has " + name + " once: " +
                                                              Text(g_final.instance_extensions));
        }
        Check(!g_validation || g_final.instance_extensions.front() == VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
              "the game's own instance extensions come first: " + Text(g_final.instance_extensions));
        for (const std::string& name : bridge_device) {
            Check(once(g_final.device_extensions, name),
                  "the final device has " + name + " once: " + Text(g_final.device_extensions));
        }
        Check(g_final.device_extensions.front() == "VK_KHR_external_memory",
              "the runtime's addition comes before the layer's: " + Text(g_final.device_extensions));
        Check(g_final.timeline_first && *g_final.timeline_first == VK_TRUE && g_final.after_timeline == &storage,
              "the final device chain starts with timeline semaphores, the game's chain after them");
        const depthxr::VulkanBridgeGame known = depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device));
        Check(known.timeline_semaphores &&
                  known.get_instance_proc_addr == reinterpret_cast<PFN_xrVoidFunction>(GameGetInstanceProcAddr),
              "the hooks know the game's device: its vkGetInstanceProcAddr and timeline semaphores");
        const std::string log = Peer::Log();
        Check(Contains(log, "Frame Generation's Vulkan bridge: added ") &&
                  Contains(log, " to the game's Vulkan device; timeline semaphores added."),
              "the log tells what was added: " + LineWith(log, "to the game's Vulkan device"));
        const std::string summary = depthxr::VulkanEnableHookSummary();
        Check(Contains(summary, "vkCreateInstance(") && Contains(summary, "vkCreateDevice(") &&
                  Contains(summary, "timeline added"),
              "the hooks' summary: " + summary);
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // Vulkan 1.2's features with timeline semaphores off: binary, and the
        // chain reaches Vulkan exactly as the game made it.
        BeginCase("enable2-vulkan12-off");
        App app = StartApp({kVulkan2Extension});
        VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features.timelineSemaphore = VK_FALSE;
        const VkPhysicalDeviceVulkan12Features copy = features;
        GameOptions options;
        options.device_chain = &features;
        VulkanGame game = MakeVulkanGame(app, options);
        Check(g_final.device_next == &features && !g_final.timeline_first &&
                  std::memcmp(&copy, &features, sizeof(features)) == 0,
              "the game's Vulkan 1.2 features reach vkCreateDevice as they were");
        for (const std::string& name : bridge_device) {
            Check(once(g_final.device_extensions, name), "the extensions are still added: " + name);
        }
        Check(!depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device)).timeline_semaphores,
              "the bridge is told binary semaphores");
        Check(Contains(Peer::Log(), "binary semaphores (the game left timeline semaphores off)"), "the log tells so");
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // ...and with them on: the game's own timeline semaphores.
        BeginCase("enable2-vulkan12-on");
        App app = StartApp({kVulkan2Extension});
        VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features.timelineSemaphore = VK_TRUE;
        GameOptions options;
        options.device_chain = &features;
        VulkanGame game = MakeVulkanGame(app, options);
        Check(g_final.device_next == &features && !g_final.timeline_first,
              "the game's chain reaches vkCreateDevice as it was");
        Check(depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device)).timeline_semaphores,
              "the bridge is told timeline semaphores");
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // A Vulkan 1.0 game: timeline semaphores are an extension's there.
        BeginCase("enable2-vulkan10");
        App app = StartApp({kVulkan2Extension});
        GameOptions options;
        options.api = VK_API_VERSION_1_0;
        VulkanGame game = MakeVulkanGame(app, options);
        Check(g_final.instance_api == VK_API_VERSION_1_0, "the game's instance is Vulkan 1.0");
        for (const std::string& name : bridge_instance) {
            Check(once(g_final.instance_extensions, name), "the 1.0 instance gets " + name);
        }
        Check(g_final.timeline_first && *g_final.timeline_first == VK_TRUE &&
                  once(g_final.device_extensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME),
              "timeline semaphores come with their extension: " + Text(g_final.device_extensions));
        Check(depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device)).timeline_semaphores,
              "the bridge is told timeline semaphores");
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // Frame Generation off when the instance was made: the runtime gets
        // the game's own vkGetInstanceProcAddr, and nothing is added.
        BeginCase("enable2-framegen-off", false);
        App app = StartApp({kVulkan2Extension});
        Check(!Peer::HooksAddForBridge(), "the hooks add nothing on this instance");
        VulkanGame game = MakeVulkanGame(app);
        Check(g_runtime.given_get_instance_proc_addr == GameGetInstanceProcAddr,
              "the runtime gets the game's own vkGetInstanceProcAddr");
        Check(!Has(g_final.device_extensions, "VK_KHR_external_memory_win32") && !g_final.timeline_first &&
                  g_final.device_next == nullptr,
              "nothing reaches the game's device: " + Text(g_final.device_extensions));
        Check(!depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device)).get_instance_proc_addr,
              "the hooks know nothing of the device");
        // Turned on before the session: too late for this game.
        ChangeSettings({});
        SessionCreate create(game, app.system);
        Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
        const std::string log = Peer::Log();
        Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && !Peer::Bridge().Active() &&
                  Contains(log, std::string(kUnbridgedLine) + "the instance has no XR_KHR_D3D12_enable") &&
                  Contains(log, "restart the game to use it"),
              "Frame Generation turned on after the game started is off for its session: " + Unbridged(log));
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // The game enabled XR_KHR_D3D12_enable itself, Frame Generation off
        // at the start: the latch says no, whatever the settings say later.
        BeginCase("enable2-latched-off", false);
        App app = StartApp({kVulkan2Extension, kD3D12Extension});
        ChangeSettings({});
        VulkanGame game = MakeVulkanGame(app);
        Check(!Peer::HooksAddForBridge() && g_runtime.given_get_instance_proc_addr == GameGetInstanceProcAddr,
              "the decision was made with the instance");
        SessionCreate create(game, app.system);
        Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
        const std::string log = Peer::Log();
        Check(!Peer::Bridge().Active() &&
                  Contains(log, std::string(kUnbridgedLine) +
                                    "Frame Generation was enabled after the game started; restart the game to use it"),
              "the log tells why: " + Unbridged(log));
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // A runtime that ignores the getter it is given: nothing is added,
        // and Frame Generation is off for the session.
        BeginCase("enable2-getter-ignored");
        g_runtime.ignore_getter = true;
        App app = StartApp({kVulkan2Extension});
        VulkanGame game = MakeVulkanGame(app);
        Check(!Has(g_final.device_extensions, "VK_KHR_external_memory_win32"), "nothing reaches the game's device");
        std::string log = Peer::Log();
        Check(Contains(log, "the runtime made the game's Vulkan device without the layer's vkCreateDevice"),
              "the log tells the hook did not run");
        SessionCreate create(game, app.system);
        Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
        log = Peer::Log();
        Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && !Peer::Bridge().Active() &&
                  g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0 &&
                  Contains(log, std::string(kUnbridgedLine) + "the runtime made the game's Vulkan device without"),
              "the session is the game's: " + Unbridged(log));
        Check(Peer::FrameGen().state == depthxr::FrameGenState::kUnavailable, "Frame Generation is unavailable");
        StopApp(app);
        DestroyGame(game);
        EndCase();
    }
    {
        // A runtime without the extensions: the application gets the
        // runtime's answer, never a function of the layer's.
        BeginCase("enable2-not-offered");
        g_runtime.vulkan_offered = false;
        App app = StartApp({kVulkan2Extension});
        PFN_xrVoidFunction function = nullptr;
        for (const char* name : {"xrCreateVulkanInstanceKHR", "xrCreateVulkanDeviceKHR", "xrGetVulkanInstanceExtensionsKHR",
                                 "xrGetVulkanDeviceExtensionsKHR"}) {
            Check(xrGetInstanceProcAddr(app.api.instance, name, &function) == XR_ERROR_FUNCTION_UNSUPPORTED &&
                      !function,
                  std::string(name) + " is unsupported, as the runtime says");
        }
        StopApp(app);
        EndCase();
    }
}

void CheckRouting(SessionCreate& create);

// 2b. A runtime that looks vkCreateDevice up while it makes the game's
// instance and calls what it got in xrCreateVulkanDeviceKHR without looking
// it up again: the layer's vkCreateDevice still makes the game's device on
// that instance, with what the bridge needs, and the session is bridged.
void TestCachedCreateDevice() {
    BeginCase("enable2-cached-create-device");
    g_runtime.cache_create_device = true;
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app); // the device is made (VK_SUCCESS)
    Check(g_runtime.cached_create_device != nullptr && g_final.devices == 1,
          "the runtime's vkCreateDevice, looked up with the instance, made the game's device");
    for (const char* name : depthxr::VulkanBridge::DeviceExtensions()) {
        Check(std::count(g_final.device_extensions.begin(), g_final.device_extensions.end(), name) == 1,
              std::string("the final device has ") + name + " once: " + Text(g_final.device_extensions));
    }
    Check(g_final.timeline_first && *g_final.timeline_first == VK_TRUE,
          "timeline semaphores are added (the layer knew the game's instance and its version)");
    const depthxr::VulkanBridgeGame known = depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device));
    Check(known.timeline_semaphores &&
              known.get_instance_proc_addr == reinterpret_cast<PFN_xrVoidFunction>(GameGetInstanceProcAddr),
          "the hooks know the game's device");
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    CheckRouting(create);
    Check(Contains(Peer::Log(), "semaphores=timeline"), "the bridge runs with timeline semaphores");
    StopApp(app);
    DestroyGame(game);
    EndCase();
}

// 3. XR_KHR_vulkan_enable: the lists the game asks for get the bridge's
// names, the two-call idiom standing across a change of settings between its
// calls (decided with the instance).
void TestEnableLists() {
    BeginCase("enable-lists");
    App app = StartApp({kVulkanExtension});
    std::uint32_t size = 0;
    Check(app.api.vulkan_device_extensions(app.api.instance, app.system, 0, &size, nullptr) == XR_SUCCESS && size > 0,
          "the device list is counted");
    ChangeSettings({false}); // Frame Generation turned off between the calls
    std::string text(size, '\0');
    std::uint32_t written = 0;
    Check(app.api.vulkan_device_extensions(app.api.instance, app.system, size - 1, &written, text.data()) ==
                  XR_ERROR_SIZE_INSUFFICIENT &&
              written == size,
          "too short a buffer is refused, with the size");
    Check(app.api.vulkan_device_extensions(app.api.instance, app.system, size, &written, text.data()) == XR_SUCCESS &&
              written == size && text[size - 1] == '\0' && std::strlen(text.c_str()) + 1 == size,
          "the second call fills the size the first counted");
    const Names device = Split(text.c_str());
    Check(device.size() >= 2 && device[0] == "VK_KHR_external_memory" && device[1] == "VK_KHR_dedicated_allocation",
          "the runtime's names come first: " + Text(device));
    for (const char* name : depthxr::VulkanBridge::DeviceExtensions()) {
        Check(std::count(device.begin(), device.end(), name) == 1, std::string("the device list has ") + name + " once");
    }
    size = 0;
    Check(app.api.vulkan_instance_extensions(app.api.instance, app.system, 0, &size, nullptr) == XR_SUCCESS &&
              size > 0,
          "the instance list is counted");
    std::string instance_text(size, '\0');
    Check(app.api.vulkan_instance_extensions(app.api.instance, app.system, size, &size, instance_text.data()) ==
              XR_SUCCESS,
          "the instance list");
    const Names instance = Split(instance_text.c_str());
    for (const char* name : depthxr::VulkanBridge::InstanceExtensions()) {
        Check(std::count(instance.begin(), instance.end(), name) == 1,
              std::string("the instance list has ") + name + " once: " + Text(instance));
    }
    // The game makes its device with them (the test's own vkCreateDevice).
    ChangeSettings({});
    GameOptions options;
    options.enable = 1;
    VulkanGame game = MakeVulkanGame(app, options);
    Check(!depthxr::VulkanGameFor(reinterpret_cast<std::uint64_t>(game.device)).get_instance_proc_addr,
          "a game's own device is unknown to the hooks (the bridge uses the loader)");
    StopApp(app);
    DestroyGame(game);
    EndCase();

    BeginCase("enable-lists-framegen-off", false);
    app = StartApp({kVulkanExtension});
    size = 0;
    Check(app.api.vulkan_device_extensions(app.api.instance, app.system, 0, &size, nullptr) == XR_SUCCESS,
          "the device list is counted");
    std::string plain(size, '\0');
    Check(app.api.vulkan_device_extensions(app.api.instance, app.system, size, &size, plain.data()) == XR_SUCCESS &&
              std::string(plain.c_str()) == g_runtime.vulkan_device_extensions,
          "without the bridge the runtime's list is the game's: " + std::string(plain.c_str()));
    StopApp(app);
    EndCase();
}

// What a session the bridge does not take must look like: the game's own
// Vulkan session, Frame Generation unavailable for it, its images the
// runtime's, frames reaching the runtime with its swapchains.
void CheckGameSession(App& app, VulkanGame& game, SessionCreate& create, const std::string& why) {
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && g_runtime.vk_device == game.device &&
              g_runtime.binding_next == &create.application_struct,
          "the runtime gets the application's Vulkan binding");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(), "no bridge, no substitution");
    const std::string log = Peer::Log();
    Check(Count(log, std::string(kUnbridgedLine) + why) == 1, "the log tells why, once: " + Unbridged(log));
    Check(!Contains(log, kBridgedSessionLine), "the log does not tell of a bridged session");
    CheckGameGraphicsLine(log);
    Check(!Contains(log, kBridgeGraphicsLine),
          "the log names no bridge's hardware: " + LineWith(log, kBridgeGraphicsLine));
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.state == depthxr::FrameGenState::kUnavailable && Contains(status.reason, why) &&
              !Peer::FrameGenOwnsPacing() && g_framegen_chain.made == 0,
          "Frame Generation is unavailable, no backend made: " + status.reason);
    BeginSession(app);
    // Formats as the runtime has them (Vulkan's).
    std::uint32_t count = 0;
    std::array<std::int64_t, 8> formats{};
    Check(app.api.enumerate_formats(app.session, static_cast<std::uint32_t>(formats.size()), &count, formats.data()) ==
                  XR_SUCCESS &&
              std::equal(formats.begin(), formats.begin() + count, std::begin(kRuntimeFormatsVulkan),
                         std::end(kRuntimeFormatsVulkan)),
          "the runtime's Vulkan formats reach the game as they are");
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    for (const Target* target : {&left, &right}) {
        const StandInSwapchain* runtime = FindSwapchain(target->swapchain);
        Check(runtime && runtime->images_vk.size() == kImages, "the runtime made Vulkan images");
        for (std::size_t i = 0; i < kImages; ++i) {
            Check(target->images[i] == reinterpret_cast<std::uint64_t>(runtime->images_vk[i]),
                  "the application renders into the runtime's images");
        }
    }
    const Renderer none;
    for (int frame = 0; frame < 3; ++frame) {
        Check(Frame(app, none, {&left, &right}, {FrameColor(frame, 0), FrameColor(frame, 1)},
                    {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS,
              "a Vulkan frame");
        Check(g_runtime.frames.back().views.size() == 2 && g_runtime.frames.back().views[0].swapchain == left.swapchain &&
                  g_runtime.frames.back().views[1].swapchain == right.swapchain,
              "the frame reaches the runtime with the game's swapchains");
    }
    Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS &&
              app.api.destroy_swapchain(right.swapchain) == XR_SUCCESS,
          "the swapchains are destroyed");
}

// 4. VECTORXR_VULKAN_BRIDGE=off: the session is the game's Vulkan one.
void TestBridgeOffSession() {
    BeginCase("session-bridge-off");
    SetEnv("VECTORXR_VULKAN_BRIDGE", "off");
    App app = StartApp({kVulkan2Extension});
    Check(g_runtime.instance_attempts.size() == 1 && g_runtime.instance_attempts[0] == Names{kVulkan2Extension},
          "the instance gets no XR_KHR_D3D12_enable: " + Text(g_runtime.instance_attempts));
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0 && g_runtime.Position("xrCreateSession:D3D12") < 0,
          "nothing of Direct3D 12 reaches the runtime");
    CheckGameSession(app, game, create, "VECTORXR_VULKAN_BRIDGE=off");
    StopApp(app);
    SetEnv("VECTORXR_VULKAN_BRIDGE", "");
    Check(GameRuns(game), "the game's device works");
    DestroyGame(game);
    EndCase();
}

// 4b. VECTORXR_VULKAN_BRIDGE=off for an application that enables
// XR_KHR_D3D11_enable too (the instance's XR_KHR_D3D12_enable is then the
// Direct3D 11 bridge's): nothing of the game's Vulkan is changed.
void TestBridgeOffWithD3D11() {
    BeginCase("session-bridge-off-with-d3d11");
    SetEnv("VECTORXR_VULKAN_BRIDGE", "off");
    App app = StartApp({kD3D11Extension, kVulkanExtension, kVulkan2Extension});
    Check(g_runtime.instance_attempts.size() == 1 && Has(g_runtime.instance_attempts[0], kD3D12Extension),
          "the Direct3D 11 bridge's XR_KHR_D3D12_enable is added: " + Text(g_runtime.instance_attempts));
    Check(!Peer::HooksAddForBridge(), "the hooks add nothing for the Vulkan bridge on this instance");
    const auto list = [&](auto function, const char* what) {
        std::uint32_t size = 0;
        Check(function && function(app.api.instance, app.system, 0, &size, nullptr) == XR_SUCCESS && size > 0,
              std::string(what) + " counted");
        std::string text(size, '\0');
        Check(function(app.api.instance, app.system, size, &size, text.data()) == XR_SUCCESS, what);
        return std::string(text.c_str());
    };
    const std::string instance_list = list(app.api.vulkan_instance_extensions, "xrGetVulkanInstanceExtensionsKHR");
    const std::string device_list = list(app.api.vulkan_device_extensions, "xrGetVulkanDeviceExtensionsKHR");
    Check(instance_list == g_runtime.vulkan_instance_extensions && device_list == g_runtime.vulkan_device_extensions,
          "the extension lists are the runtime's: \"" + instance_list + "\", \"" + device_list + "\"");
    VulkanGame game = MakeVulkanGame(app);
    Check(g_runtime.given_get_instance_proc_addr == GameGetInstanceProcAddr,
          "the runtime gets the game's own vkGetInstanceProcAddr");
    Names instance_expected;
    if (g_validation) instance_expected.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    for (const char* name : g_runtime.vulkan_instance_additions) instance_expected.emplace_back(name);
    Names device_expected;
    for (const char* name : g_runtime.vulkan_device_additions) device_expected.emplace_back(name);
    Check(g_final.instance_extensions == instance_expected,
          "the game's instance is made with its own and the runtime's extensions only: " +
              Text(g_final.instance_extensions));
    Check(g_final.device_extensions == device_expected && g_final.device_next == nullptr && !g_final.timeline_first,
          "the game's device is made with the runtime's extensions only, its chain as it was: " +
              Text(g_final.device_extensions));
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    CheckGameSession(app, game, create, "VECTORXR_VULKAN_BRIDGE=off");
    StopApp(app);
    SetEnv("VECTORXR_VULKAN_BRIDGE", "");
    Check(GameRuns(game), "the game's device works");
    DestroyGame(game);
    EndCase();
}

// 4c. A Vulkan binding after another structure of the application's (the
// runtime finds it all the same): no bridge takes the session, Frame
// Generation is off for it, and no backend is made from its Vulkan chain.
void TestVulkanBindingNotFirst() {
    BeginCase("vulkan-binding-not-first");
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    XrBaseInStructure ahead{kApplicationStructType};
    ahead.next = reinterpret_cast<const XrBaseInStructure*>(&create.binding);
    create.info.next = &ahead;
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && g_runtime.vk_device == game.device &&
              g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR") < 0,
          "the runtime gets the game's Vulkan session");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(), "no bridge, no substitution");
    Check(g_framegen_chain.made == 0 && !g_framegen_chain.vulkan,
          "no Frame Generation backend is made from a chain with a Vulkan binding (made=" +
              std::to_string(g_framegen_chain.made) + ", vulkan=" + std::to_string(g_framegen_chain.vulkan) + ")");
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.state == depthxr::FrameGenState::kUnavailable &&
              Contains(status.reason, "Vulkan sessions need the Vulkan bridge") && !Peer::FrameGenOwnsPacing(),
          "Frame Generation is unavailable: " + status.reason);
    const std::string log = Peer::Log();
    Check(Count(log, std::string(kUnbridgedLine) +
                         "its Vulkan binding is not the first structure of the session's create info") == 1,
          "the log tells why, once: " + Unbridged(log));
    StopApp(app);
    DestroyGame(game);
    EndCase();
}

// 5. A bridged session: the runtime gets a Direct3D 12 session on the game's
// adapter, with the queue substitution's queue, never the bridge's; Frame
// Generation is made from the bridge's binding.
void CheckRouting(SessionCreate& create) {
    const std::string log = Peer::Log();
    const XrGraphicsBindingD3D12KHR* bridge = BridgeBinding();
    Check(bridge && Peer::Bridge().Active(), "the session is bridged (" + Unbridged(log) + ")");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,
          "the runtime gets a Direct3D 12 binding first: " + std::to_string(g_runtime.binding));
    Check(SameLuid(g_runtime.device12->GetAdapterLuid(), g_gpu.luid), "the session's device is on the game's adapter");
    Check(g_runtime.device12.Get() == bridge->device, "the session's device is the bridge's");
    Check(!g_device12 || SameObject(g_device12.Get(), bridge->device),
          "the bridge's device is the one the test holds for its debug-layer checks");
    Check(g_runtime.queue.Get() != bridge->queue, "the runtime is never given the bridge's queue");
    const D3D12_COMMAND_QUEUE_DESC queue = g_runtime.queue->GetDesc();
    Check(queue.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && queue.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH &&
              Peer::QueueSubstituted(),
          "the runtime's queue is the substitution's high-priority direct queue");
    const std::ptrdiff_t requirements = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
    const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:D3D12");
    Check(requirements >= 0 && requirements < session,
          "the Direct3D 12 requirements are asked for before the session is made");
    Check(g_runtime.Position("xrGetVulkanGraphicsRequirements2KHR", requirements) < 0 &&
              g_runtime.Position("xrGetVulkanGraphicsRequirementsKHR", requirements) < 0,
          "the Vulkan requirements are not asked for again: the session is Direct3D 12");
    Check(g_runtime.binding_next == &create.application_struct, "the application's other structures follow the binding");
    Check(Peer::FrameGenOwnsPacing(), "Frame Generation runs the bridged session");
    Check(g_framegen_chain.first == depthxr::kFrameGenRuntimeQueueType &&
              g_framegen_chain.queue == g_runtime.queue.Get() && g_framegen_chain.next == bridge,
          "Frame Generation is made from the bridge's binding, with the runtime's queue");
    Check(Peer::GraphicsApi() == "Vulkan-on-D3D12", "the session's API is " + Peer::GraphicsApi());
    Check(Contains(log, "Frame Generation's Vulkan bridge: XR_KHR_D3D12_enable added to the instance's extensions.") &&
              Contains(log, "Frame Generation gives the runtime a Direct3D 12 queue of its own"),
          "the log tells the session's bridge and queue");
    Check(Count(log, kBridgedSessionLine) == 1, "the log tells once that the session is bridged");
    CheckGameGraphicsLine(log);
    Check(Count(log, kBridgeGraphicsLine) == 1,
          "the log names the hardware of the bridge's Direct3D 12 device once: " + LineWith(log, kBridgeGraphicsLine));
}

// 6. Swapchains on a bridged session: formats translated, the runtime's
// swapchains made for the bridge's copies, refusals before the runtime is
// asked.
void CheckSwapchains(App& app, VulkanGame& game) {
    // Formats: the runtime's DXGI ones as VkFormats, in its order, none Vulkan lacks.
    std::uint32_t count = 0;
    Check(app.api.enumerate_formats(app.session, 0, &count, nullptr) == XR_SUCCESS && count > 0, "formats counted");
    std::vector<std::int64_t> formats(count);
    if (count > 1) {
        std::uint32_t short_count = 0;
        Check(app.api.enumerate_formats(app.session, count - 1, &short_count, formats.data()) ==
                      XR_ERROR_SIZE_INSUFFICIENT &&
                  short_count == count,
              "too short an array of formats is refused, with the count");
    }
    Check(app.api.enumerate_formats(app.session, count, &count, formats.data()) == XR_SUCCESS, "the formats");
    const std::vector<std::int64_t> translated = depthxr::VulkanBridge::TranslatedFormats(
        std::vector<std::int64_t>(std::begin(kRuntimeFormats12), std::end(kRuntimeFormats12)));
    std::size_t at = 0;
    for (const std::int64_t format : formats) {
        while (at < translated.size() && translated[at] != format) ++at;
        Check(at < translated.size(), "format " + std::to_string(format) + " is one of the runtime's, in its order");
    }
    Check(std::find(formats.begin(), formats.end(), VK_FORMAT_R8G8B8A8_SRGB) != formats.end() &&
              std::find(formats.begin(), formats.end(), VK_FORMAT_D32_SFLOAT) != formats.end(),
          "sRGB colour and D32 depth are offered");

    // Refused before the runtime is asked: a format Direct3D 12 lacks, and
    // more layers than the game's device can share.
    std::size_t requests = g_runtime.swapchain_requests.size();
    XrSwapchain refused = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo odd = ColorInfo(VK_FORMAT_R8G8B8_UNORM);
    Check(app.api.create_swapchain(app.session, &odd, &refused) == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED &&
              refused == XR_NULL_HANDLE,
          "a format without a Direct3D 12 twin is unsupported");
    const XrSwapchainCreateInfo layered = ColorInfo(VK_FORMAT_R8G8B8A8_SRGB, 1, 4096);
    const XrResult layered_result = app.api.create_swapchain(app.session, &layered, &refused);
    Check((layered_result == XR_ERROR_FEATURE_UNSUPPORTED || layered_result == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED) &&
              refused == XR_NULL_HANDLE,
          "4096 layers are refused: " + std::to_string(layered_result));
    Check(g_runtime.swapchain_requests.size() == requests, "the runtime was not asked for either");
    Check(Contains(Peer::Log(), "Frame Generation's Vulkan bridge refuses a swapchain it cannot share"),
          "the refusals are logged");

    // A multisampled one: the runtime's are single-sampled copy destinations,
    // in Direct3D 12's format.
    const XrSwapchainCreateInfo msaa_info = ColorInfo(VK_FORMAT_R8G8B8A8_SRGB, 4);
    Target msaa = MakeTarget(app, msaa_info, "a multisampled swapchain");
    Check(g_runtime.swapchain_requests.size() == requests + 1, "one runtime swapchain for it");
    const XrSwapchainCreateInfo& asked = g_runtime.swapchain_requests.back();
    Check((asked.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
              (asked.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) && asked.sampleCount == 1 &&
              asked.width == kWidth && asked.height == kHeight && asked.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
          "the runtime's swapchain is a single-sampled colour attachment and copy destination");
    // The two-call idiom, on the bridge's images.
    count = 0;
    std::vector<XrSwapchainImageVulkanKHR> short_array(kImages - 1, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    Check(app.api.enumerate_images(msaa.swapchain, kImages - 1, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(short_array.data())) ==
                  XR_ERROR_SIZE_INSUFFICIENT &&
              count == kImages,
          "too short an array of images is refused, with the count");
    std::vector<XrSwapchainImageD3D12KHR> d3d12_array(kImages, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    Check(app.api.enumerate_images(msaa.swapchain, kImages, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d12_array.data())) ==
              XR_ERROR_VALIDATION_FAILURE,
          "Direct3D 12 images are refused: the application's are Vulkan's");
    Check(Peer::Bridge().Images(msaa.swapchain) == msaa.images, "the images are the bridge's");
    const Renderer vulkan{&game, nullptr};
    const Color color{90, 180, 30, 255};
    Check(Frame(app, vulkan, {&msaa}, {color}, {{{&msaa, 0}, {&msaa, 0}}}) == XR_SUCCESS,
          "a frame of the multisampled swapchain");
    for (const ShownView& view : g_runtime.frames.back().views) {
        Check(Near(view.texel, color), "the resolved frame reaches the runtime: " + Text(view.texel));
    }
    Check(app.api.destroy_swapchain(msaa.swapchain) == XR_SUCCESS && g_runtime.destroyed.back() == msaa.swapchain,
          "the multisampled swapchain is destroyed");

    // Usage 0 (as X-Plane asks): the runtime's are still colour attachments
    // and copy destinations, and the frame reaches it.
    XrSwapchainCreateInfo bare_info = ColorInfo();
    bare_info.usageFlags = 0;
    Target bare = MakeTarget(app, bare_info, "a swapchain without usage");
    const XrSwapchainCreateInfo& bare_asked = g_runtime.swapchain_requests.back();
    Check((bare_asked.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
              (bare_asked.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
          "the runtime's swapchain is a colour attachment and copy destination");
    const Color bare_color{30, 60, 200, 255};
    Check(Frame(app, vulkan, {&bare}, {bare_color}, {{{&bare, 0}, {&bare, 0}}}) == XR_SUCCESS, "a frame of it");
    Check(Near(g_runtime.frames.back().views[0].texel, bare_color),
          "it reaches the runtime: " + Text(g_runtime.frames.back().views[0].texel));
    Check(app.api.destroy_swapchain(bare.swapchain) == XR_SUCCESS, "it is destroyed");

    // A runtime refusing the copy usage is asked again without it.
    g_runtime.refuse_transfer_dst = true;
    requests = g_runtime.swapchain_requests.size();
    XrSwapchain plain = XR_NULL_HANDLE;
    XrSwapchainCreateInfo plain_info = ColorInfo();
    plain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    Check(app.api.create_swapchain(app.session, &plain_info, &plain) == XR_SUCCESS && plain != XR_NULL_HANDLE,
          "a swapchain the runtime makes without the copy usage");
    Check(g_runtime.swapchain_requests.size() == requests + 2 &&
              (g_runtime.swapchain_requests[requests].usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) &&
              !(g_runtime.swapchain_requests[requests + 1].usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
          "the second request drops TRANSFER_DST");
    Check(Peer::Bridge().Bridged(plain), "it is bridged all the same");
    Check(app.api.destroy_swapchain(plain) == XR_SUCCESS, "it is destroyed");
    g_runtime.refuse_transfer_dst = false;

    // Runtime images that do not match are refused, and the runtime's swapchain goes.
    g_runtime.width_error = 2;
    XrSwapchain mismatched = XR_NULL_HANDLE;
    const XrSwapchainCreateInfo mismatched_info = ColorInfo();
    const XrResult result = app.api.create_swapchain(app.session, &mismatched_info, &mismatched);
    g_runtime.width_error = 0;
    Check(result == XR_ERROR_RUNTIME_FAILURE && mismatched == XR_NULL_HANDLE,
          "images the bridge cannot share fail the swapchain: " + std::to_string(result));
    Check(!g_runtime.created.empty() && g_runtime.destroyed.back() == g_runtime.created.back(),
          "the runtime's swapchain is destroyed again");
}

// 7. Frames: what the runtime shows on its queue is what the game rendered
// that frame, the runtime's queue ordered after the bridge's copies (through
// the queue substitution). In two frames the game's queue is held back from
// just before the last eye's release until the runtime's readback is seen
// waiting for it.
void CheckFrames(App& app, VulkanGame& game, Target& left, Target& right) {
    const Renderer vulkan{&game, nullptr};
    constexpr int kFrames = 10;
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::vector<Color> colors{FrameColor(frame, 0), FrameColor(frame, 1)};
        std::optional<Gate> gate;
        if (frame == 2 || frame == 6) {
            gate.emplace(g_runtime.device12.Get(), &game);
        }
        // Frame 6 releases the right eye first: the left is the one held.
        const bool right_first = frame == 6;
        const int held_eye = right_first ? 0 : 1;
        const std::vector<const Target*> order =
            right_first ? std::vector<const Target*>{&right, &left} : std::vector<const Target*>{&left, &right};
        const std::vector<Color> order_colors = right_first ? std::vector<Color>{colors[1], colors[0]} : colors;
        FrameOptions options;
        options.hold = gate ? &*gate : nullptr;
        const XrResult result = Frame(app, vulkan, order, order_colors, {{{&left, 0}, {&right, 0}}}, options);
        Check(result == XR_SUCCESS, "frame " + std::to_string(frame) + ": xrEndFrame " + std::to_string(result));
        const ShownFrame& shown = g_runtime.frames.back();
        Check(shown.views.size() == 2, "frame " + std::to_string(frame) + ": two views");
        if (gate) {
            Check(shown.held, "frame " + std::to_string(frame) + ": the runtime's readback still waits for eye " +
                                  std::to_string(held_eye) + ", released last and held back");
            Check(shown.unordered.size() == 2 && !Near(shown.unordered[held_eye], colors[held_eye]),
                  "frame " + std::to_string(frame) + ": read on a queue ordered after nothing meanwhile, eye " +
                      std::to_string(held_eye) + "'s image does not hold the frame yet");
        }
        for (int eye = 0; eye < 2; ++eye) {
            const ShownView& view = shown.views[eye];
            Check(view.swapchain == (eye ? right : left).swapchain && !view.depth,
                  "frame " + std::to_string(frame) + ": the eye's view as submitted");
            Check(Near(view.texel, colors[eye]), "frame " + std::to_string(frame) + " eye " + std::to_string(eye) +
                                                     ": the runtime shows " + Text(view.texel) + ", rendered " +
                                                     Text(colors[eye]));
        }
    }
    for (const Target* target : {&left, &right}) {
        const std::string describe = Peer::Bridge().Describe(target->swapchain);
        Check(Contains(describe, "copies=" + std::to_string(kFrames)) && Contains(describe, "skippedCopies=0"),
              "a copy per frame: " + describe);
    }
    Peer::ExpireBridgeSummary();
    Check(Frame(app, vulkan, {&left, &right}, {FrameColor(0, 0), FrameColor(0, 1)}, {{{&left, 0}, {&right, 0}}}) ==
              XR_SUCCESS,
          "a frame after the summary expired");
    Check(Contains(Peer::Log(), "Frame Generation's Vulkan bridge: "), "the bridge's summary is logged");
}

// 8. Depth on a bridged session is mirrored: the runtime gets the game's
// depth, in its views and its images.
void CheckDepth(App& app, VulkanGame& game, Target& left, Target& right) {
    const std::size_t requests = g_runtime.swapchain_requests.size();
    Target depth = MakeTarget(app, DepthInfo(VK_FORMAT_D32_SFLOAT, 2), "depth");
    const XrSwapchainCreateInfo& asked = g_runtime.swapchain_requests[requests];
    Check(asked.format == DXGI_FORMAT_D32_FLOAT && (asked.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
              (asked.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) &&
              !(asked.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) && asked.arraySize == 2,
          "the runtime's depth swapchain is D32, a depth target and copy destination");
    Check(Peer::Bridge().Bridged(depth.swapchain) && !Peer::Bridge().Unmirrored(depth.swapchain),
          "the depth swapchain is mirrored");
    const Renderer vulkan{&game, nullptr};
    g_runtime.read_depth = true;
    constexpr int kFrames = 8;
    for (int frame = 0; frame < kFrames; ++frame) {
        const std::vector<Color> colors{FrameColor(frame + 10, 0), FrameColor(frame + 10, 1)};
        FrameOptions options;
        options.depth = &depth;
        options.depth_value = frame % 2 ? 0.25f : 0.75f;
        Check(Frame(app, vulkan, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}, options) == XR_SUCCESS,
              "depth frame " + std::to_string(frame));
        const ShownFrame& shown = g_runtime.frames.back();
        for (int eye = 0; eye < 2; ++eye) {
            const ShownView& view = shown.views[eye];
            Check(view.depth && view.depth_swapchain == depth.swapchain,
                  "the runtime gets the game's depth in the views");
            Check(view.depth_value && std::abs(*view.depth_value - options.depth_value) < 1e-6f,
                  "depth frame " + std::to_string(frame) + " eye " + std::to_string(eye) + ": the runtime's depth image holds " +
                      (view.depth_value ? std::to_string(*view.depth_value) : "nothing") + ", the game wrote " +
                      std::to_string(options.depth_value));
            Check(Near(view.texel, colors[eye]), "depth frame: the runtime shows " + Text(view.texel));
        }
    }
    g_runtime.read_depth = false;
    Check(!Contains(Peer::Log(), "keeps the game's depth to itself"), "no depth is left out");
    Check(app.api.destroy_swapchain(depth.swapchain) == XR_SUCCESS, "the depth swapchain is destroyed");
}

// 9. Teardown: the runtime frees nothing the bridge's queue still writes.
void CheckTeardown(App& app, VulkanGame& game, Target& left, Target& right) {
    const Renderer vulkan{&game, nullptr};
    ID3D12Device* device = g_runtime.device12.Get();
    ID3D12CommandQueue* bridge_queue = BridgeBinding()->queue;
    {
        // The left eye's last copy waits behind a gate the layer's wait outlasts.
        Gate gate(device, nullptr);
        gate.HoldQueue(bridge_queue);
        const Color last{1, 2, 3, 255};
        Render(app, vulkan, left, last);
        const std::uint32_t index = *FindSwapchain(left.swapchain)->released;
        bool destroyed = false;
        std::optional<Color> held;
        g_runtime.on_destroy_swapchain = [&](XrSwapchain swapchain) {
            if (swapchain == left.swapchain) {
                destroyed = true;
                held = ReadNow(FindSwapchain(swapchain)->images12[index].Get());
            }
        };
        std::thread opener = gate.OpenLater(300);
        const XrResult result = app.api.destroy_swapchain(left.swapchain);
        opener.join();
        g_runtime.on_destroy_swapchain = nullptr;
        Check(result == XR_SUCCESS && destroyed, "the runtime's swapchain is destroyed with the application's");
        Check(Near(held, last), "the bridge's last copy was done when the runtime destroyed its swapchain: " + Text(held));
        Check(!Peer::Bridge().Bridged(left.swapchain), "the bridge let the swapchain go");
    }

    Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
    app.space = XR_NULL_HANDLE;
    Check(app.api.end_session(app.session) == XR_SUCCESS, "xrEndSession");
    // The right eye is left to the session; its last copy waits behind a
    // gate, and a marker after everything on the bridge's queue.
    Gate gate(device, nullptr);
    gate.HoldQueue(bridge_queue);
    Render(app, vulkan, right, {4, 5, 6, 255});
    ComPtr<ID3D12Fence> marker;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))), "a marker fence");
    bridge_queue->Signal(marker.Get(), 1);
    bool marker_done = false;
    bool idle = false;
    g_runtime.on_destroy_session = [&] {
        marker_done = marker->GetCompletedValue() >= 1;
        for (int poll = 0; poll < 50 && !idle; ++poll) {
            idle = Peer::Bridge().WaitIdle(poll ? 1 : 0);
        }
    };
    std::thread opener = gate.OpenLater(300);
    const XrResult result = app.api.destroy_session(app.session);
    opener.join();
    app.session = XR_NULL_HANDLE;
    Check(result == XR_SUCCESS && g_runtime.Position("xrDestroySession") >= 0, "xrDestroySession");
    Check(marker_done, "the bridge's queue had run everything queued on it when the runtime destroyed the session");
    Check(idle, "the bridge's queue was idle when the runtime destroyed the session");
    Check(!Peer::Bridge().Active() && !Peer::QueueSubstituted(), "the bridge and the substitution end with the session");
    const std::string log = Peer::Log();
    Check(Contains(log, "Frame Generation's Vulkan bridge at session end: "), "the bridge's summary is logged");
    Check(!Contains(log, "did not finish") && !Contains(log, "kept until its session ends"),
          "nothing was stalled or kept");
    Check(Count(log, kBridgedSessionLine) == 1, "the session's bridge was logged once, for the whole session");
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
    Check(GameRuns(game), "the game's device works after teardown");
}

// Cases 5 to 9 in one session, for XR_KHR_vulkan_enable2 (or the frames of
// one, for XR_KHR_vulkan_enable).
void TestBridgedSession(int enable) {
    BeginCase(enable == 2 ? "bridged-session-enable2" : "bridged-session-enable");
    App app = StartApp({enable == 2 ? kVulkan2Extension : kVulkanExtension});
    GameOptions options;
    options.enable = enable;
    VulkanGame game = MakeVulkanGame(app, options);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
    CheckRouting(create);
    BeginSession(app);
    if (enable == 2) {
        CheckSwapchains(app, game);
    }
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    CheckFrames(app, game, left, right);
    if (enable == 2) {
        CheckDepth(app, game, left, right);
    }
    CheckValidation("cases 5 to 8");
    CheckTeardown(app, game, left, right);
    DestroyGame(game);
    EndCase();
}

// 10. Fallbacks the bridge sees: VECTORXR_FRAMEGEN_QUEUE=game (the runtime
// would get the bridge's queue) and a runtime refusing the Direct3D 12
// session. Both end with the game's Vulkan session, the Vulkan requirements
// asked for again after the Direct3D 12 ones.
void TestBridgedFallbacks() {
    for (const bool refused : {false, true}) {
        BeginCase(refused ? "refused-d3d12-session" : "game-queue");
        if (refused) {
            g_runtime.refuse_d3d12_session = true;
        } else {
            SetEnv("VECTORXR_FRAMEGEN_QUEUE", "game");
        }
        App app = StartApp({kVulkan2Extension});
        VulkanGame game = MakeVulkanGame(app);
        SessionCreate create(game, app.system);
        Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
        const std::ptrdiff_t d3d12 = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
        const std::ptrdiff_t refusal = g_runtime.Position("xrCreateSession:D3D12");
        const std::ptrdiff_t vulkan =
            g_runtime.Position("xrGetVulkanGraphicsRequirements2KHR", std::max(d3d12, refusal) + 1);
        const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:Vulkan");
        Check(d3d12 >= 0 && (refused ? refusal > d3d12 : refusal < 0) && vulkan > std::max(d3d12, refusal) &&
                  session > vulkan,
              "the Vulkan requirements are asked for again after the Direct3D 12 ones, before the Vulkan session (" +
                  Unbridged(Peer::Log()) + ")");
        if (refused) {
            Check(Contains(Peer::Log(), "xrCreateSession refused the bridged Direct3D 12 session"), "the retry is logged");
        }
        CheckGameSession(app, game, create,
                         refused ? "the runtime refused the bridged Direct3D 12 session"
                                 : "the runtime would be given the bridge's queue");
        StopApp(app);
        SetEnv("VECTORXR_FRAMEGEN_QUEUE", "");
        Check(GameRuns(game), "the game's device works");
        DestroyGame(game);
        EndCase();
    }
}

// 10b. An application enabling both XR_KHR_vulkan_enable and _enable2 that
// makes its device itself, with the lists the layer hands out (as
// XR_KHR_vulkan_enable says): bridged as such a game is (the loader's
// functions, binary semaphores). When the runtime refuses the Direct3D 12
// session, XR_KHR_vulkan_enable's requirements are the ones asked for again.
void TestBothEnablesOwnDevice() {
    for (const bool refused : {false, true}) {
        BeginCase(refused ? "both-enables-refused-d3d12-session" : "both-enables-own-device");
        g_runtime.refuse_d3d12_session = refused;
        App app = StartApp({kVulkanExtension, kVulkan2Extension});
        Check(Peer::HooksAddForBridge(), "the hooks add for the bridge on this instance");
        GameOptions options;
        options.enable = 1;
        VulkanGame game = MakeVulkanGame(app, options);
        Check(depthxr::VulkanDeviceOriginOf(reinterpret_cast<std::uint64_t>(game.device)) ==
                  depthxr::VulkanDeviceOrigin::kGame,
              "the game made its device itself");
        SessionCreate create(game, app.system);
        Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS, "xrCreateSession");
        if (!refused) {
            CheckRouting(create);
            Check(Contains(Peer::Log(), "semaphores=binary"),
                  "the bridge runs with binary semaphores: " + LineWith(Peer::Log(), kBridgedSessionLine));
            BeginSession(app);
            const Renderer vulkan{&game, nullptr};
            Target left = MakeTarget(app, ColorInfo(), "the left eye");
            Target right = MakeTarget(app, ColorInfo(), "the right eye");
            for (int frame = 0; frame < 3; ++frame) {
                const std::vector<Color> colors{FrameColor(frame, 0), FrameColor(frame, 1)};
                Check(Frame(app, vulkan, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS,
                      "a bridged frame");
                for (int eye = 0; eye < 2; ++eye) {
                    Check(Near(g_runtime.frames.back().views[eye].texel, colors[eye]),
                          "the runtime shows what the game rendered: " + Text(g_runtime.frames.back().views[eye].texel));
                }
            }
            Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS &&
                      app.api.destroy_swapchain(right.swapchain) == XR_SUCCESS,
                  "the swapchains are destroyed");
        } else {
            const std::ptrdiff_t d3d12 = g_runtime.Position("xrGetD3D12GraphicsRequirementsKHR");
            const std::ptrdiff_t refusal = g_runtime.Position("xrCreateSession:D3D12");
            const std::ptrdiff_t vulkan = g_runtime.Position("xrGetVulkanGraphicsRequirementsKHR", refusal + 1);
            const std::ptrdiff_t session = g_runtime.Position("xrCreateSession:Vulkan");
            Check(d3d12 >= 0 && refusal > d3d12 && vulkan > refusal && session > vulkan &&
                      g_runtime.Position("xrGetVulkanGraphicsRequirements2KHR", d3d12) < 0,
                  "XR_KHR_vulkan_enable's requirements are asked for again before the Vulkan session, not "
                  "XR_KHR_vulkan_enable2's");
            CheckGameSession(app, game, create, "the runtime refused the bridged Direct3D 12 session");
        }
        StopApp(app);
        Check(GameRuns(game), "the game's device works");
        DestroyGame(game);
        EndCase();
    }
}

// 11. The instance destroyed with its bridged session still alive (as a game
// that quits may): the bridge's copies into the session's images are done
// before the runtime frees them with the instance.
void TestDestroyInstanceWithSession() {
    BeginCase("instance-with-session");
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session (" + Unbridged(Peer::Log()) + ")");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    const std::vector<Color> colors{FrameColor(3, 0), FrameColor(3, 1)};
    Check(Frame(app, vulkan, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS &&
              Near(g_runtime.frames.back().views[0].texel, colors[0]) &&
              Near(g_runtime.frames.back().views[1].texel, colors[1]),
          "a frame of the bridged session");
    ID3D12CommandQueue* bridge_queue = BridgeBinding()->queue;
    Gate gate(g_runtime.device12.Get(), nullptr);
    gate.HoldQueue(bridge_queue);
    Render(app, vulkan, left, {7, 8, 9, 255});
    ComPtr<ID3D12Fence> marker;
    Check(SUCCEEDED(g_runtime.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))), "a marker fence");
    bridge_queue->Signal(marker.Get(), 1);
    bool destroyed = false;
    bool marker_done = false;
    g_runtime.on_destroy_instance = [&] {
        destroyed = true;
        marker_done = marker->GetCompletedValue() >= 1;
    };
    std::thread opener = gate.OpenLater(300);
    const XrResult result = app.api.destroy_instance(app.api.instance);
    opener.join();
    Check(result == XR_SUCCESS && destroyed, "xrDestroyInstance with the session alive");
    Check(marker_done, "the bridge's queue had run everything queued on it when the runtime destroyed the instance");
    Check(g_runtime.Position("xrDestroySession") < 0, "the session went with the instance, not on its own");
    Check(Peer::Idle(), "nothing is left of the instance and its session");
    const std::string log = Peer::Log();
    Check(Count(log, kBridgedSessionLine) == 1 && Contains(log, "Frame Generation's Vulkan bridge at session end: "),
          "the session's bridge was logged once, and its summary");
    Check(GameRuns(game), "the game's device works after its instance went with the session");
    DestroyGame(game);
    EndCase();
}

// 12. A runtime failing xrDestroySession: the Vulkan half of the bridge
// ends all the same (the game may destroy its device right after).
void TestRefusedDestroySession() {
    BeginCase("destroy-session-refused");
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session (" + Unbridged(Peer::Log()) + ")");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Check(Frame(app, vulkan, {&left}, {FrameColor(1, 0)}, {{{&left, 0}, {&left, 0}}}) == XR_SUCCESS, "a frame");
    Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
    app.space = XR_NULL_HANDLE;
    g_runtime.refuse_destroy_session = true;
    Check(app.api.destroy_session(app.session) == XR_ERROR_RUNTIME_FAILURE, "xrDestroySession fails in the runtime");
    Check(!Peer::Bridge().Active(), "the bridge ended all the same");
    g_runtime.refuse_destroy_session = false;
    Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    Check(Peer::Idle(), "nothing is left of the instance");
    Check(GameRuns(game), "the game's device works");
    DestroyGame(game);
    EndCase();
}

// What Frame Generation's frames looked like: shown frames since `from`.
struct FrameGenShown {
    std::size_t own{0}; // frames showing Frame Generation's own outputs
    bool increasing{true};
};

FrameGenShown CheckFrameGenShown(std::size_t from, const std::vector<XrSwapchain>& game_swapchains, const std::string& at) {
    FrameGenShown result;
    XrTime last = 0;
    for (const ShownFrame& shown : Shown(from)) {
        Check(shown.views.size() == 2, at + ": the runtime gets two views");
        if (last != 0 && shown.time <= last) result.increasing = false;
        last = shown.time;
        const bool own = std::none_of(game_swapchains.begin(), game_swapchains.end(),
                                      [&](XrSwapchain swapchain) { return swapchain == shown.views[0].swapchain; });
        for (const ShownView& view : shown.views) {
            Check(view.texel && *view.texel != Color{0, 0, 0, 255} && *view.texel != Color{0, 0, 0, 0},
                  at + ": every frame shown is read back and not black: " + Text(view.texel));
        }
        result.own += own ? 1 : 0;
    }
    return result;
}

// Frames of a game with Frame Generation on the real Direct3D 12 backend at
// 2x (just in time, on its own thread): frames it generates reach the
// runtime, display times strictly increasing.
void RunFrameGen(App& app, const Renderer& renderer, Target& left, Target& right, const Target* depth,
                 const std::string& at) {
    const std::vector<XrSwapchain> game_swapchains{left.swapchain, right.swapchain};
    const std::size_t from = ShownCount();
    int frame = 0;
    do {
        for (int i = 0; i < 24; ++i, ++frame) {
            FrameOptions options;
            options.depth = depth;
            options.depth_value = 0.5f;
            Check(Frame(app, renderer, {&left, &right}, {FrameColor(frame, 0), FrameColor(frame, 1)},
                        {{{&left, 0}, {&right, 0}}}, options) == XR_SUCCESS,
                  at + ": frame " + std::to_string(frame));
        }
    } while (Peer::FrameGen().generated == 0 && frame < 144);
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.threaded && status.multiplier == 2,
          at + ": Frame Generation presents from its own thread at 2x (threaded=" + std::to_string(status.threaded) +
              ", multiplier=" + std::to_string(status.multiplier) + ", state " + depthxr::ToString(status.state) + ": " +
              status.reason + ")");
    Check(status.generated > 0,
          at + ": frames are generated (generated=" + std::to_string(status.generated) +
              ", captured=" + std::to_string(status.captured) + ", repeated=" + std::to_string(status.repeated) +
              ", unmeasuredCopies=" + std::to_string(status.unmeasured_copies) +
              ", copiesSkipped=" + std::to_string(status.copies_skipped) + ", unwritten=" +
              std::to_string(status.unwritten) + ", unready=" + std::to_string(status.unready_capture) +
              ", failures=" + std::to_string(status.failures) + ", frames=" + std::to_string(frame) +
              ", shown=" + std::to_string(ShownCount() - from) + ")");
    const FrameGenShown shown = CheckFrameGenShown(from, game_swapchains, at);
    Check(shown.own > 0, at + ": frames of Frame Generation's own outputs reach the runtime");
    Check(shown.increasing, at + ": the runtime's frames have strictly increasing display times");
}

// 13. Frame Generation on the real Direct3D 12 backend for a bridged Vulkan game.
void TestBridgedFrameGen(const std::string& mode) {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = mode;
    BeginCase("bridged-framegen-" + mode, settings);
    g_runtime.paced = true;
    g_image_width = g_image_height = kFrameGenSize;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(MakeRealGpu);
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session (" + Unbridged(Peer::Log()) + ")");
    Check(Peer::FrameGenOwnsPacing() && g_framegen_chain.next == BridgeBinding(),
          "Frame Generation runs on the bridge's device");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    Target depth = MakeTarget(app, DepthInfo(VK_FORMAT_D32_SFLOAT, 2), "depth");
    RunFrameGen(app, vulkan, left, right, &depth, "bridged " + mode);
    CheckValidation("generating");
    StopApp(app);
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    DestroyGame(game);
    EndCase();
}

// 13c. Star Citizen's shapes (its log, 2026-10-09): XR_KHR_vulkan_enable2, an
// sRGB swapchain per eye the game only copies into (TRANSFER_DST alone), and
// its interface on a quad swapchain (TRANSFER_DST alone too) shown over the
// projection every frame, both layers with source alpha. Frame Generation
// (the real Direct3D 12 backend) generates with the quad shown lazily, which
// is on for Vulkan games: frames of its own reach the runtime with the quad
// and the layers' flags, and the quad's releases go lazily, so its frames
// never wait for the game's work.
void TestStarCitizenShapes() {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    BeginCase("star-citizen-shapes", settings);
    SetEnv("VECTORXR_FRAMEGEN_SHOWN_LAYERS", "");
    g_runtime.paced = true;
    g_image_width = g_image_height = kFrameGenSize;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(MakeRealGpu);
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "star citizen: a bridged session (" + Unbridged(Peer::Log()) + ")");
    Check(Contains(Peer::Log(), "Frame Generation keeps releases of layers it does not capture lazy "
                                "(VECTORXR_FRAMEGEN_SHOWN_LAYERS=off turns this off)"),
          "star citizen: a Vulkan game's shown layers are lazy by default");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    XrSwapchainCreateInfo eye_info = ColorInfo();
    eye_info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    Target left = MakeTarget(app, eye_info, "star citizen's left eye (copied into)");
    Target right = MakeTarget(app, eye_info, "star citizen's right eye (copied into)");
    const XrSwapchainCreateInfo& eye_asked = g_runtime.swapchain_requests.back();
    Check((eye_asked.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
              (eye_asked.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) &&
              eye_asked.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
          "star citizen: the runtime's eye swapchains are sRGB colour attachments and copy destinations");
    XrSwapchainCreateInfo ui_info = ColorInfo();
    ui_info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ui_info.height = kFrameGenSize / 2;
    Target ui = MakeTarget(app, ui_info, "star citizen's interface quad");
    Check(Peer::Bridge().Bridged(left.swapchain) && Peer::Bridge().Bridged(right.swapchain) &&
              Peer::Bridge().Bridged(ui.swapchain),
          "star citizen: its swapchains are bridged");
    const Color ui_color{240, 200, 40, 128};
    const XrCompositionLayerFlags alpha = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    const std::size_t from = ShownCount();
    int frame = 0;
    do {
        for (int i = 0; i < 24; ++i, ++frame) {
            FrameOptions options;
            options.quad = &ui;
            options.quad_color = ui_color;
            options.layer_flags = alpha;
            Check(Frame(app, vulkan, {&left, &right}, {FrameColor(frame, 0), FrameColor(frame, 1)},
                        {{{&left, 0}, {&right, 0}}}, options) == XR_SUCCESS,
                  "star citizen: frame " + std::to_string(frame));
        }
    } while (Peer::FrameGen().generated == 0 && frame < 144);
    const depthxr::FrameGenStatus status = Peer::FrameGen();
    Check(status.threaded && status.multiplier == 2 && status.generated > 0,
          "star citizen: Frame Generation generates at 2x from its own thread (generated=" +
              std::to_string(status.generated) + ", state " + depthxr::ToString(status.state) + ": " + status.reason + ")");
    const FrameGenShown shown = CheckFrameGenShown(from, {left.swapchain, right.swapchain}, "star citizen");
    Check(shown.own > 0 && shown.increasing,
          "star citizen: Frame Generation's own frames reach the runtime, in display-time order");
    {
        std::scoped_lock lock(g_runtime_mutex);
        std::size_t with_projection = 0;
        for (std::size_t i = from; i < g_runtime.frames.size(); ++i) {
            const ShownFrame& shown_frame = g_runtime.frames[i];
            if (shown_frame.views.empty()) continue;
            ++with_projection;
            Check(shown_frame.projection_flags.size() == 1 && shown_frame.projection_flags[0] == alpha,
                  "star citizen: frame " + std::to_string(i) + " keeps the projection's source alpha");
            Check(shown_frame.quads.size() == 1 && shown_frame.quads[0] == ui.swapchain && shown_frame.quad_flags[0] == alpha,
                  "star citizen: frame " + std::to_string(i) + " shows the interface quad over the projection");
        }
        Check(with_projection > 0, "star citizen: the runtime showed frames");
    }
    CheckValidation("star citizen");
    StopApp(app);
    const std::string log = Peer::Log();
    const std::string summary = LineWith(log, "Frame Generation's shown layers at session end (lazy): lazyReleases=");
    const std::size_t at = summary.find("lazyReleases=");
    const unsigned long lazy_releases =
        at == std::string::npos ? 0 : std::stoul(summary.substr(at + std::string("lazyReleases=").size()));
    Check(lazy_releases > 0, "star citizen: the interface quad's releases went lazily (" + summary + ")");
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    DestroyGame(game);
    EndCase();
}

// 13b. The game's queue stalls before its last release of a frame while
// Frame Generation (the real Direct3D 12 backend) generates, and the game
// destroys its session (or its instance, the session alive): the bridge lets
// its queue through before Frame Generation stops, so Frame Generation's work
// queued behind the stalled release finishes and its output swapchains go
// before the session does, which reaches the runtime after about the
// bridge's 2 s bound. The game's queue runs on from there (else the bridge
// would keep its Vulkan objects for the process, as it does for a queue that
// never finishes).
void TestStalledTeardown(bool with_instance) {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = "interpolate";
    const std::string name = with_instance ? "stalled-teardown-instance" : "stalled-teardown-session";
    BeginCase(name, settings);
    g_runtime.paced = true;
    g_image_width = g_image_height = kFrameGenSize;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(MakeRealGpu);
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session (" + Unbridged(Peer::Log()) + ")");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    RunFrameGen(app, vulkan, left, right, nullptr, name);
    CheckValidation("generating");
    std::vector<XrSwapchain> outputs; // Frame Generation's own
    {
        std::scoped_lock lock(g_runtime_mutex);
        for (const XrSwapchain swapchain : g_runtime.created) {
            if (swapchain != left.swapchain && swapchain != right.swapchain) outputs.push_back(swapchain);
        }
        // Nothing waits for the game's held work on the runtime's side.
        g_runtime.skip_readback = true;
    }
    Check(!outputs.empty(), "Frame Generation made output swapchains");
    {
        Gate gate(g_runtime.device12.Get(), &game);
        FrameOptions options;
        options.stall = &gate;
        Check(Frame(app, vulkan, {&left, &right}, {FrameColor(1, 0), FrameColor(1, 1)}, {{{&left, 0}, {&right, 0}}},
                    options) == XR_SUCCESS,
              "a frame whose last release the game's queue never reaches");
        Check(app.api.destroy_space(app.space) == XR_SUCCESS, "xrDestroySpace");
        app.space = XR_NULL_HANDLE;
        // What the runtime had destroyed when the session went, and when.
        std::optional<std::vector<XrSwapchain>> destroyed_before;
        std::chrono::steady_clock::time_point reached{};
        const auto note = [&] {
            reached = std::chrono::steady_clock::now();
            destroyed_before = g_runtime.destroyed;
            gate.Open();
        };
        if (with_instance) {
            g_runtime.on_destroy_instance = note;
        } else {
            g_runtime.on_destroy_session = note;
        }
        const auto start = std::chrono::steady_clock::now();
        const XrResult result = with_instance ? app.api.destroy_instance(app.api.instance)
                                              : app.api.destroy_session(app.session);
        Check(result == XR_SUCCESS && destroyed_before, with_instance ? "xrDestroyInstance" : "xrDestroySession");
        const double seconds = std::chrono::duration<double>(reached - start).count();
        if (!with_instance) app.session = XR_NULL_HANDLE;
        for (const XrSwapchain output : outputs) {
            Check(std::find(destroyed_before->begin(), destroyed_before->end(), output) != destroyed_before->end(),
                  "Frame Generation's output swapchains were destroyed before the session went");
        }
        const std::string log = Peer::Log();
        Check(!Contains(log, "did not finish its work on a swapchain being destroyed"),
              "Frame Generation's work finished: " + LineWith(log, "did not finish its work"));
        Check(Contains(log, "the game's queue did not reach its last release in time at teardown"),
              "the bridge let its queue through at teardown");
        Check(seconds < 3.5, "the session reached the runtime after " + std::to_string(seconds) +
                                 " s (the bridge waits 2 s for the game's queue)");
    }
    if (!with_instance) {
        Check(app.api.destroy_instance(app.api.instance) == XR_SUCCESS, "xrDestroyInstance");
    }
    Check(Peer::Idle(), "nothing is left of the instance");
    Check(GameRuns(game), "the game's device works after teardown");
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    DestroyGame(game);
    EndCase();
}

// 14. A native Direct3D 12 game (shaped like MSFS) with Frame Generation on
// the real backend: nothing of the Vulkan bridge, its formats as the runtime
// has them, its own images, its depth kept.
void TestNativeD3D12(const std::string& name) {
    CaseSettings settings;
    settings.frame_multiplier = "2x";
    settings.synthesis_mode = "extrapolate";
    BeginCase(name, settings);
    g_runtime.paced = true;
    g_image_width = g_image_height = kFrameGenSize;
    Peer::SetFrameGenEngagement(2, std::chrono::milliseconds(0));
    Peer::UseFrameGenGpu(MakeRealGpu);
    App app = StartApp({kD3D12Extension});
    Check(g_runtime.instance_attempts.size() == 1 && g_runtime.instance_attempts[0] == Names{kD3D12Extension},
          "the instance is as the game asked: " + Text(g_runtime.instance_attempts));
    XrGraphicsRequirementsD3D12KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    Check(app.api.d3d12_requirements &&
              app.api.d3d12_requirements(app.api.instance, app.system, &requirements) == XR_SUCCESS,
          "xrGetD3D12GraphicsRequirementsKHR");
    D3D12Game d3d12;
    Check(MakeD3D12Game(g_gpu.luid, d3d12), "the Direct3D 12 game's device and queue");
    XrBaseInStructure application_struct{kApplicationStructType};
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.next = &application_struct;
    binding.device = d3d12.device.Get();
    binding.queue = d3d12.queue.Get();
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = app.system;
    Check(app.api.create_session(app.api.instance, &info, &app.session) == XR_SUCCESS, "xrCreateSession");
    Check(g_runtime.binding == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && g_runtime.device12.Get() == d3d12.device.Get() &&
              g_runtime.queue.Get() != d3d12.queue.Get() && Peer::QueueSubstituted(),
          "the runtime gets the game's device with the substitution's queue");
    Check(!Peer::Bridge().Active() && Peer::GraphicsApi() == "D3D12" && Peer::FrameGenOwnsPacing(),
          "no bridge; Frame Generation runs on Direct3D 12 (" + Peer::GraphicsApi() + ")");
    BeginSession(app);
    std::uint32_t count = 0;
    std::array<std::int64_t, 8> formats{};
    Check(app.api.enumerate_formats(app.session, static_cast<std::uint32_t>(formats.size()), &count, formats.data()) ==
                  XR_SUCCESS &&
              std::equal(formats.begin(), formats.begin() + count, std::begin(kRuntimeFormats12),
                         std::end(kRuntimeFormats12)),
          "the runtime's formats reach the game as they are");
    XrSwapchainCreateInfo color = ColorInfo(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    XrSwapchainCreateInfo depth_info = DepthInfo(DXGI_FORMAT_D32_FLOAT, 2);
    const std::size_t requests = g_runtime.swapchain_requests.size();
    Target left = MakeTarget(app, color, "the left eye", true);
    Target right = MakeTarget(app, color, "the right eye", true);
    Target depth = MakeTarget(app, depth_info, "depth", true);
    // (Frame Generation adds the usage it captures depth with.)
    Check(g_runtime.swapchain_requests[requests + 2].format == DXGI_FORMAT_D32_FLOAT &&
              (g_runtime.swapchain_requests[requests + 2].usageFlags & depth_info.usageFlags) == depth_info.usageFlags,
          "the depth swapchain reaches the runtime as asked");
    for (const Target* target : {&left, &right, &depth}) {
        const StandInSwapchain* runtime = FindSwapchain(target->swapchain);
        Check(runtime && runtime->images12.size() == kImages, "the runtime made Direct3D 12 images");
        for (std::size_t i = 0; i < kImages; ++i) {
            Check(target->images[i] == reinterpret_cast<std::uint64_t>(runtime->images12[i].Get()),
                  "the game renders into the runtime's images");
        }
    }
    // Before Frame Generation engages, the game's own frames, with their depth.
    const Renderer renderer{nullptr, &d3d12};
    FrameOptions options;
    options.depth = &depth;
    Check(Frame(app, renderer, {&left, &right}, {FrameColor(0, 0), FrameColor(0, 1)}, {{{&left, 0}, {&right, 0}}},
                options) == XR_SUCCESS,
          "a first frame");
    const ShownFrame first = Shown().back();
    Check(first.views.size() == 2 && first.views[0].swapchain == left.swapchain && first.views[0].depth &&
              first.views[0].depth_swapchain == depth.swapchain,
          "the game's frame reaches the runtime with its depth");
    RunFrameGen(app, renderer, left, right, &depth, name);
    CheckValidation("generating");
    StopApp(app);
    Peer::UseFrameGenGpu(MakeStandInGpu);
    Peer::SetFrameGenEngagement(45, std::chrono::milliseconds(3000)); // the presenter's own
    EndCase();
}

// 15. The bridge's device removed: the session is lost. Last: Direct3D 12
// makes one device per adapter in a process.
void TestDeviceLost() {
    BeginCase("device-lost");
    App app = StartApp({kVulkan2Extension});
    VulkanGame game = MakeVulkanGame(app);
    SessionCreate create(game, app.system);
    Check(app.api.create_session(app.api.instance, &create.info, &app.session) == XR_SUCCESS && Peer::Bridge().Active(),
          "a bridged session (" + Unbridged(Peer::Log()) + ")");
    BeginSession(app);
    const Renderer vulkan{&game, nullptr};
    Target left = MakeTarget(app, ColorInfo(), "the left eye");
    Target right = MakeTarget(app, ColorInfo(), "the right eye");
    const std::vector<Color> colors{FrameColor(0, 0), FrameColor(0, 1)};
    Check(Frame(app, vulkan, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}}) == XR_SUCCESS &&
              Near(g_runtime.frames.back().views[0].texel, colors[0]),
          "a frame before the loss");
    ComPtr<ID3D12Device5> device5;
    Check(SUCCEEDED(BridgeBinding()->device->QueryInterface(IID_PPV_ARGS(&device5))), "ID3D12Device5");
    CheckValidation("before the device's removal");
    device5->RemoveDevice();
    device5.Reset();
    const std::size_t shown = g_runtime.frames.size();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const XrResult result = Frame(app, vulkan, {&left, &right}, colors, {{{&left, 0}, {&right, 0}}});
        Check(result == XR_ERROR_SESSION_LOST,
              "xrEndFrame " + std::to_string(attempt + 1) + " after the removal loses the session: " +
                  std::to_string(result));
        Check(g_runtime.frames.size() == shown, "the frame does not reach the runtime");
    }
    const std::string log = Peer::Log();
    Check(Count(log, "the session is lost") == 1, "the loss is logged once, for two frames");
    const auto start = std::chrono::steady_clock::now();
    Check(app.api.destroy_swapchain(left.swapchain) == XR_SUCCESS, "xrDestroySwapchain on a lost session");
    StopApp(app);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Check(seconds < 10.0, "teardown does not wait on a removed device: " + std::to_string(seconds) + " s");
    Check(GameRuns(game), "the game's Vulkan device runs on");
    DestroyGame(game);
    EndCase(true);
}

// Everything the layer reads and writes on the host goes to the test's own directory.
void IsolateHost() {
    wchar_t temp[MAX_PATH + 1]{};
    GetTempPathW(MAX_PATH, temp);
    g_root = std::filesystem::path(temp) /
             ("vectorxr-vulkan-bridge-layer-tests-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(g_root, error);
    std::filesystem::create_directories(g_root);
    SetEnv("VECTORXR_CONFIG_PATH", (g_root / "settings.json").string());
    SetEnv("VECTORXR_LOG_PATH", (g_root / "logs" / "vectorxr.log").string());
    SetEnv("VECTORXR_SEEN_APPS_PATH", (g_root / "seen-apps.json").string());
    SetEnv("VECTORXR_RUNTIME_RELAY_PATH", (g_root / "runtime").string());
    SetEnv("VECTORXR_RUNTIME_PACING_PATH", (g_root / "runtime-pacing.json").string());
    SetEnv("VECTORXR_TURBO_METRICS_PATH", (g_root / "turbo-metrics.json").string());
    SetEnv("XR_RUNTIME_JSON", (g_root / "no-runtime.json").string());
    // Whatever the host sets for the bridges does not apply.
    SetEnv("VECTORXR_FRAMEGEN_QUEUE", "");
    SetEnv("VECTORXR_D3D11_BRIDGE", "");
    SetEnv("VECTORXR_VULKAN_BRIDGE", "");
    SetEnv("VECTORXR_VULKAN_BRIDGE_DEBUG", "");
}

// The fixture: vulkan-1.dll, a hardware adapter whose Vulkan device has its
// LUID and imports Direct3D 12 images and fences, and (when the layer is
// there) a validation layer that reports what it should. 0, or why not.
std::string FindGpu() {
    vulkan_test::ConfigureVulkanLoader();
    const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader) return "no vulkan-1.dll";
    g_vk.vkGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
    if (!g_vk.vkGetInstanceProcAddr) return "vulkan-1.dll has no vkGetInstanceProcAddr";
#define TEST_VK_LOAD_GLOBAL(name) g_vk.name = reinterpret_cast<PFN_##name>(g_vk.vkGetInstanceProcAddr(nullptr, #name));
    TEST_VK_GLOBAL(TEST_VK_LOAD_GLOBAL)
    g_validation = vulkan_test::ValidationAvailable(g_vk.vkEnumerateInstanceLayerProperties);

    VulkanGame fixture;
    std::vector<const char*> layers;
    std::vector<const char*> extensions;
    if (g_validation) {
        layers.push_back(vulkan_test::kValidationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    VkResult created = g_vk.vkCreateInstance(&info, nullptr, &fixture.instance);
    if (created == VK_ERROR_INCOMPATIBLE_DRIVER) return "no Vulkan driver";
    if (created != VK_SUCCESS && g_validation) {
        // The loader lists the validation layer, but it does not load.
        std::cout << "WARNING: " << vulkan_test::kValidationLayer << " is listed but the instance failed with it ("
                  << static_cast<int>(created) << "); running without validation\n";
        g_validation = false;
        info.enabledLayerCount = 0;
        info.ppEnabledLayerNames = nullptr;
        info.enabledExtensionCount = 0;
        info.ppEnabledExtensionNames = nullptr;
        created = g_vk.vkCreateInstance(&info, nullptr, &fixture.instance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER) return "no Vulkan driver";
    }
    // Any other failure is the suite's, never a reason to skip.
    CheckVk(created, "the fixture's Vulkan instance");
    LoadInstanceFunctions(fixture.instance);
    MakeMessenger(fixture);

    std::uint32_t count = 0;
    g_vk.vkEnumeratePhysicalDevices(fixture.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    g_vk.vkEnumeratePhysicalDevices(fixture.instance, &count, devices.data());
    ComPtr<IDXGIFactory4> factory;
    std::string why = "no Vulkan device whose LUID matches a hardware Direct3D 12 adapter";
    // FG_TEST_ADAPTER: only the devices whose names contain it.
    const std::optional<std::string> wanted = test_adapter::Wanted();
    std::vector<test_adapter::Adapter> adapters;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &id;
        g_vk.vkGetPhysicalDeviceProperties2(device, &properties);
        adapters.push_back(
            {properties.properties.deviceName, properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU});
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        LUID luid{};
        std::memcpy(&luid, id.deviceLUID, sizeof(luid));
        if (wanted && !test_adapter::NameMatches(properties.properties.deviceName, *wanted)) continue;
        if (!id.deviceLUIDValid || properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ||
            (!factory && FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        // The driver's own answers: Direct3D 12 images and fences import.
        VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkPhysicalDeviceImageFormatInfo2 format{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        format.pNext = &external;
        format.format = VK_FORMAT_R8G8B8A8_SRGB;
        format.type = VK_IMAGE_TYPE_2D;
        format.tiling = VK_IMAGE_TILING_OPTIMAL;
        format.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkExternalImageFormatProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 format_properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        format_properties.pNext = &external_properties;
        VkPhysicalDeviceExternalSemaphoreInfo semaphore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
        semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        VkExternalSemaphoreProperties semaphore_properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        g_vk.vkGetPhysicalDeviceExternalSemaphoreProperties(device, &semaphore, &semaphore_properties);
        if (g_vk.vkGetPhysicalDeviceImageFormatProperties2(device, &format, &format_properties) != VK_SUCCESS ||
            !(external_properties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            !(semaphore_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
            why = std::string(properties.properties.deviceName) + " imports no Direct3D 12 images or fences";
            continue;
        }
        g_gpu.luid = luid;
        g_gpu.name = properties.properties.deviceName;
        fixture.physical = device;
        why.clear();
        break;
    }
    if (!why.empty() && wanted && !test_adapter::FirstMatch(adapters, *wanted)) {
        why = test_adapter::NoMatch(*wanted, adapters);
    }
    if (why.empty() && g_validation) {
        // The canary: a barrier from a layout the image is not in must be reported.
        fixture.family = GraphicsFamily(fixture.physical);
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = fixture.family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue;
        CheckVk(g_vk.vkCreateDevice(fixture.physical, &device_info, nullptr, &fixture.device), "the canary's device");
        LoadDeviceFunctions(fixture.device);
        FinishGame(fixture);
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {4, 4, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkImage image = VK_NULL_HANDLE;
        CheckVk(g_vk.vkCreateImage(fixture.device, &image_info, nullptr, &image), "the canary's image");
        VkMemoryRequirements requirements{};
        g_vk.vkGetImageMemoryRequirements(fixture.device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = MemoryType(fixture.physical, requirements.memoryTypeBits);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        CheckVk(g_vk.vkAllocateMemory(fixture.device, &allocate, nullptr, &memory), "the canary's memory");
        CheckVk(g_vk.vkBindImageMemory(fixture.device, image, memory, 0), "the canary's memory bound");
        // Nothing before the canary reported anything: what it reports is its own.
        CheckVulkanValidation("the GPU search");
        std::uint32_t slot = 0;
        const VkCommandBuffer commands = BeginCommands(fixture, slot);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; // it is UNDEFINED
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        g_vk.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &barrier);
        SubmitCommands(fixture, slot);
        g_vk.vkQueueWaitIdle(fixture.queue);
        const std::vector<std::string> reported = g_vk_messages.Take();
        std::string text;
        for (const std::string& message : reported) {
            text += message + "\n";
        }
        Check(std::any_of(reported.begin(), reported.end(),
                          [](const std::string& message) {
                              return Contains(message, "[vulkan error]") && Contains(message, "layout");
                          }),
              "the validation canary (a barrier from a layout the image is not in) is reported as a layout error:\n" +
                  text);
        g_vk.vkDestroyImage(fixture.device, image, nullptr);
        g_vk.vkFreeMemory(fixture.device, memory, nullptr);
    }
    DestroyGame(fixture);
    CheckVulkanValidation("the GPU search's teardown");
    return why;
}

} // namespace

int main(int argc, char** argv) {
    g_unbridged_only = argc > 1 && std::string(argv[1]) == "unbridged";
    const bool require = vulkan_test::RequireHardware();
    const auto skip = [&](const std::string& why) {
        std::cout << (require ? "FAILED (VECTORXR_TESTS_REQUIRE_HW=1): " : "skipped: ") << why << "\n";
        return require ? 1 : kSkip;
    };
    // Before any Direct3D 12 device exists.
    ComPtr<ID3D12Debug> debug;
    g_d3d12_debug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (g_d3d12_debug) {
        debug->EnableDebugLayer();
    } else {
        std::cout << "WARNING: no Direct3D 12 debug layer: the bridge's Direct3D 12 work is NOT validated\n";
    }
    const std::string why = FindGpu();
    if (!why.empty()) {
        return skip(why);
    }
    vulkan_test::PrintValidationState(g_validation);
    if (require && (!g_validation || !g_d3d12_debug)) {
        std::cout << "FAILED (VECTORXR_TESTS_REQUIRE_HW=1): validation and the Direct3D 12 debug layer must be on\n";
        return 1;
    }
    std::cout << "GPU: " << g_gpu.name << "\n" << std::flush;
    IsolateHost();
    Peer::UseFrameGenGpu(MakeStandInGpu);

    // The cases that need no working bridge.
    TestInstanceExtensions();
    TestRefusedInstance();
    TestEnable2CreateInfos();
    TestEnableLists();
    TestBridgeOffSession();
    TestBridgeOffWithD3D11();
    TestVulkanBindingNotFirst();
    TestNativeD3D12("native-d3d12");
    if (!g_unbridged_only) {
        // The bridge's.
        TestCachedCreateDevice();
        TestBridgedSession(2);
        TestBridgedSession(1);
        TestBridgedFallbacks();
        TestBothEnablesOwnDevice();
        TestDestroyInstanceWithSession();
        TestRefusedDestroySession();
        TestBridgedFrameGen("interpolate");
        TestBridgedFrameGen("extrapolate");
        TestStarCitizenShapes();
        TestStalledTeardown(false);
        TestStalledTeardown(true);
        TestNativeD3D12("native-d3d12-after-bridged");
        TestDeviceLost();
    }
    g_runtime = StandIn{};
    Peer::ReleaseLog(g_root / "released" / "vectorxr.log");
    std::error_code error;
    std::filesystem::remove_all(g_root, error);
    std::cout << "vulkan bridge layer tests passed" << (g_unbridged_only ? " (the cases that need no bridge)" : "")
              << ": validation=" << (g_validation ? "on" : "OFF") << " (" << vulkan_test::kValidationLayer
              << "), D3D12 debug layer=" << (g_d3d12_debug ? "on" : "OFF") << "\n";
    return 0;
}
