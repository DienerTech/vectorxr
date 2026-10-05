#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

#include <array>
#include <limits>
#include <unordered_map>

namespace depthxr {
namespace {

constexpr std::size_t kFramesInFlight = 3;
constexpr std::size_t kCaptureSlots = 8; // views x frames in flight
constexpr std::uint32_t kSetsPerFrame = 256;

#define FG_VK_FUNCTIONS(X)                                                                                          \
    X(GetDeviceQueue) X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout)                     \
    X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines)        \
    X(DestroyPipeline) X(CreateSampler) X(DestroySampler) X(CreateDescriptorPool) X(DestroyDescriptorPool)           \
    X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateImage) X(DestroyImage)          \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateImageView)              \
    X(DestroyImageView) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool)     \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImage) X(CmdBindPipeline)               \
    X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CreateFence) X(DestroyFence) X(GetFenceStatus)     \
    X(ResetFences) X(WaitForFences) X(QueueSubmit) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool)       \
    X(CmdWriteTimestamp) X(GetQueryPoolResults) X(DeviceWaitIdle)

struct Image {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    bool initialized{false}; // transitioned out of UNDEFINED into GENERAL
};

// Identifies the image a view's history was captured from at release time.
struct CaptureKey {
    bool valid{false};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t image_index{0};
    std::uint32_t array_index{0};
    XrRect2Di rect{};

    bool Matches(const FrameGenImage& image) const {
        return valid && swapchain == image.swapchain && image_index == image.image_index &&
               array_index == image.array_index && rect.offset.x == image.rect.offset.x &&
               rect.offset.y == image.rect.offset.y && rect.extent.width == image.rect.extent.width &&
               rect.extent.height == image.rect.extent.height;
    }
};

struct ViewResources {
    FrameGenHistoryState history_state;
    CaptureKey capture;
    std::array<Image, 2> history;
    std::array<Image, 2> filtered;
    std::array<Image, kFrameGenPyramidLevels + 1> luma_newest;
    std::array<Image, kFrameGenPyramidLevels + 1> luma_previous;
    std::array<Image, kFrameGenPyramidLevels + 1> flow;
    Image output;
};

struct FrameSlot {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer commands{VK_NULL_HANDLE};
    VkFence fence{VK_NULL_HANDLE};
    VkDescriptorPool descriptors{VK_NULL_HANDLE};
    bool pending{false};
    bool timing_pending{false};
};

class VulkanFrameGen final : public FrameGenGpu {
  public:
    ~VulkanFrameGen() override {
        Reset();
        DestroyPipelineObjects();
        if (loader_) FreeLibrary(loader_);
    }

    bool Initialize(const XrGraphicsBindingVulkanKHR& binding, std::string& error) {
        binding_ = binding;
        if (!binding.instance || !binding.physicalDevice || !binding.device) {
            error = "Invalid Vulkan graphics binding";
            return false;
        }
        if (!framegen_shaders::kSpirv[0].size) {
            error = "this build has no Vulkan Frame Generation shaders";
            return false;
        }
        loader_ = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!loader_) {
            error = "Vulkan loader unavailable";
            return false;
        }
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader_, "vkGetInstanceProcAddr"));
        if (!gipa) {
            error = "vkGetInstanceProcAddr unavailable";
            return false;
        }
        auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(binding.instance, "vkGetDeviceProcAddr"));
        memory_properties_ = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa(binding.instance, "vkGetPhysicalDeviceMemoryProperties"));
        auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa(binding.instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            gipa(binding.instance, "vkGetPhysicalDeviceProperties"));
        if (!gdpa || !memory_properties_ || !families || !properties) {
            error = "Vulkan instance dispatch unavailable";
            return false;
        }
#define FG_LOAD(name)                                                                       \
    vk##name = reinterpret_cast<PFN_vk##name>(gdpa(binding.device, "vk" #name));            \
    if (!vk##name) {                                                                        \
        error = "vk" #name " unavailable";                                                  \
        return false;                                                                       \
    }
        FG_VK_FUNCTIONS(FG_LOAD)
#undef FG_LOAD
        std::uint32_t family_count = 0;
        families(binding.physicalDevice, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> family_properties(family_count);
        families(binding.physicalDevice, &family_count, family_properties.data());
        if (binding.queueFamilyIndex >= family_count ||
            binding.queueIndex >= family_properties[binding.queueFamilyIndex].queueCount ||
            !(family_properties[binding.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            error = "Vulkan session queue does not support compute";
            return false;
        }
        vkGetDeviceQueue(binding.device, binding.queueFamilyIndex, binding.queueIndex, &queue_);
        if (!queue_) {
            error = "Vulkan session queue unavailable";
            return false;
        }
        VkPhysicalDeviceProperties device_properties{};
        properties(binding.physicalDevice, &device_properties);
        timestamp_period_ = device_properties.limits.timestampPeriod;
        const bool timestamps = family_properties[binding.queueFamilyIndex].timestampValidBits > 0 &&
                                device_properties.limits.timestampComputeAndGraphics;
        return CreatePipelineObjects(timestamps, error);
    }

    const char* Name() const override { return "Vulkan"; }

    std::optional<FrameGenOutputKind> Classify(std::int64_t format) const override {
        switch (static_cast<VkFormat>(format)) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return FrameGenOutputKind::kSrgb8;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return FrameGenOutputKind::kUnorm8;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return FrameGenOutputKind::kFloat16;
        default:
            return std::nullopt;
        }
    }

    std::vector<std::int64_t> OutputFormats(FrameGenOutputKind kind) const override {
        switch (kind) {
        case FrameGenOutputKind::kSrgb8:
            return {VK_FORMAT_R8G8B8A8_SRGB};
        case FrameGenOutputKind::kUnorm8:
            return {VK_FORMAT_R8G8B8A8_UNORM};
        default:
            return {VK_FORMAT_R16G16B16A16_SFLOAT};
        }
    }

    XrSwapchainUsageFlags RequiredSourceUsage() const override { return XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; }
    XrSwapchainUsageFlags OutputUsage() const override {
        return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }

    bool Track(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages enumerate, std::string& error) override {
        if (images_.count(swapchain)) {
            return true;
        }
        std::uint32_t count = 0;
        XrResult result = enumerate(swapchain, 0, &count, nullptr);
        if (XR_FAILED(result) || count == 0) {
            error = "xrEnumerateSwapchainImages count result=" + std::to_string(result);
            return false;
        }
        std::vector<XrSwapchainImageVulkanKHR> native(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(native.data()));
        if (XR_FAILED(result)) {
            error = "xrEnumerateSwapchainImages result=" + std::to_string(result);
            return false;
        }
        auto& images = images_[swapchain];
        for (const auto& image : native) {
            images.push_back(image.image);
        }
        return true;
    }

    void Forget(XrSwapchain swapchain) override { images_.erase(swapchain); }

    bool Capture(std::size_t index, const FrameGenImage& source, const FrameGenViewRequest& request,
                 FrameGenFlowQuality quality, std::string& error) override {
        const auto it = images_.find(source.swapchain);
        if (it == images_.end() || source.image_index >= it->second.size()) {
            error = "Frame Generation capture image is not tracked";
            return false;
        }
        if (views_.size() <= index) {
            views_.resize(index + 1);
        }
        ViewResources& view = views_[index];
        // Capture never reallocates; Execute sets the view up first.
        if (!view.output.image ||
            !view.history_state.Matches(request.extent, source.format, request.output, FrameGenFinalFlowLevel(quality))) {
            error.clear();
            return false;
        }
        FrameSlot& slot = capture_slots_[capture_count_ % capture_slots_.size()];
        if (slot.pending) {
            VkResult status = vkGetFenceStatus(binding_.device, slot.fence);
            if (status == VK_NOT_READY) {
                status = vkWaitForFences(binding_.device, 1, &slot.fence, VK_TRUE, 100'000'000ull);
            }
            if (status != VK_SUCCESS) {
                error = "Vulkan Frame Generation capture work did not finish in time (VkResult=" + std::to_string(status) + ")";
                return false;
            }
            slot.pending = false;
        }
        if (!Check(vkResetCommandPool(binding_.device, slot.pool, 0), "vkResetCommandPool", error)) {
            return false;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(vkBeginCommandBuffer(slot.commands, &begin), "vkBeginCommandBuffer", error)) {
            return false;
        }
        commands_ = slot.commands;
        CopySource(view, source, it->second[source.image_index]);
        if (!Check(vkEndCommandBuffer(slot.commands), "vkEndCommandBuffer", error)) {
            // Nothing was submitted; rebuild the view on the next Execute.
            view.history_state = {};
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.commands;
        if (!Check(vkResetFences(binding_.device, 1, &slot.fence), "vkResetFences", error) ||
            !Check(vkQueueSubmit(queue_, 1, &submit, slot.fence), "vkQueueSubmit", error)) {
            return false;
        }
        slot.pending = true;
        ++capture_count_;
        view.capture = {true, source.swapchain, source.image_index, source.array_index, source.rect};
        return true;
    }

    bool WaitIdle() override {
        if (!binding_.device || !vkWaitForFences) {
            return true;
        }
        bool idle = true;
        auto wait = [&](FrameSlot& slot) {
            if (slot.pending && slot.fence) {
                if (vkWaitForFences(binding_.device, 1, &slot.fence, VK_TRUE, 2'000'000'000ull) == VK_SUCCESS) {
                    slot.pending = false;
                } else {
                    idle = false;
                }
            }
        };
        for (auto& slot : slots_) wait(slot);
        for (auto& slot : capture_slots_) wait(slot);
        return idle;
    }

    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        result.synthesized.assign(views.size(), false);
        const std::size_t slot_index = static_cast<std::size_t>(submission_ % kFramesInFlight);
        FrameSlot& slot = slots_[slot_index];
        if (slot.pending) {
            // Three submissions ago; only a stalled GPU waits here.
            VkResult status = vkGetFenceStatus(binding_.device, slot.fence);
            if (status == VK_NOT_READY) {
                status = vkWaitForFences(binding_.device, 1, &slot.fence, VK_TRUE, 100'000'000ull);
            }
            if (status != VK_SUCCESS) {
                error = "Vulkan Frame Generation GPU work did not finish in time (VkResult=" + std::to_string(status) + ")";
                return false;
            }
            slot.pending = false;
            ReadTiming(slot, slot_index);
        }
        if (!Check(vkResetCommandPool(binding_.device, slot.pool, 0), "vkResetCommandPool", error) ||
            !Check(vkResetDescriptorPool(binding_.device, slot.descriptors, 0), "vkResetDescriptorPool", error)) {
            return false;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(vkBeginCommandBuffer(slot.commands, &begin), "vkBeginCommandBuffer", error)) {
            return false;
        }
        commands_ = slot.commands;
        descriptors_ = slot.descriptors;
        const bool timed = query_pool_ != VK_NULL_HANDLE;
        if (timed) {
            vkCmdResetQueryPool(commands_, query_pool_, static_cast<std::uint32_t>(slot_index * 2), 2);
            vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool_,
                                static_cast<std::uint32_t>(slot_index * 2));
        }
        if (!dummy_.initialized) {
            ToGeneral(dummy_);
        }
        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        bool ok = true;
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            ok = RecordView(views[i], tuning, views_[i], synthesized, error);
            result.synthesized[i] = synthesized;
        }
        if (timed) {
            vkCmdWriteTimestamp(commands_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool_,
                                static_cast<std::uint32_t>(slot_index * 2 + 1));
        }
        ok = Check(vkEndCommandBuffer(commands_), "vkEndCommandBuffer", error) && ok;
        if (!ok) {
            // Nothing was submitted; recorded layout transitions never ran.
            for (std::size_t i = 0; i < views.size() && i < views_.size(); ++i) {
                DestroyView(views_[i]);
            }
            dummy_.initialized = false;
            return false;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands_;
        if (!Check(vkResetFences(binding_.device, 1, &slot.fence), "vkResetFences", error) ||
            !Check(vkQueueSubmit(queue_, 1, &submit, slot.fence), "vkQueueSubmit", error)) {
            return false;
        }
        slot.pending = true;
        slot.timing_pending = timed;
        ++submission_;
        for (std::size_t i = 0; i < views.size(); ++i) {
            views_[i].history_state.Commit(result.synthesized[i]);
            views_[i].capture.valid = false;
        }
        return true;
    }

    void ResetHistory() override {
        for (auto& view : views_) {
            view.history_state.Invalidate();
        }
    }

    void Reset() override {
        if (!binding_.device) {
            return;
        }
        WaitIdle();
        for (auto& view : views_) {
            DestroyView(view);
        }
        views_.clear();
        images_.clear();
    }

    double LastGpuMilliseconds() const override { return last_gpu_ms_; }

  private:
    static bool Check(VkResult result, const char* operation, std::string& error) {
        if (result != VK_SUCCESS) {
            error = std::string(operation) + " failed: VkResult=" + std::to_string(result);
            return false;
        }
        return true;
    }

    bool CreatePipelineObjects(bool timestamps, std::string& error) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        if (!Check(vkCreateSampler(binding_.device, &sampler, nullptr, &sampler_), "vkCreateSampler", error)) {
            return false;
        }
        std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
        for (std::uint32_t i = 0; i < 7; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                : i < 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
        }
        bindings[6].pImmutableSamplers = &sampler_;
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout.pBindings = bindings.data();
        if (!Check(vkCreateDescriptorSetLayout(binding_.device, &layout, nullptr, &set_layout_),
                   "vkCreateDescriptorSetLayout", error)) {
            return false;
        }
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FrameGenConstants)};
        VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout.setLayoutCount = 1;
        pipeline_layout.pSetLayouts = &set_layout_;
        pipeline_layout.pushConstantRangeCount = 1;
        pipeline_layout.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(binding_.device, &pipeline_layout, nullptr, &pipeline_layout_),
                   "vkCreatePipelineLayout", error)) {
            return false;
        }
        for (std::size_t pass = 0; pass < kFrameGenPassCount; ++pass) {
            const auto& blob = framegen_shaders::kSpirv[pass];
            VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module_info.codeSize = blob.size;
            module_info.pCode = static_cast<const std::uint32_t*>(blob.data);
            VkShaderModule module = VK_NULL_HANDLE;
            if (!Check(vkCreateShaderModule(binding_.device, &module_info, nullptr, &module), "vkCreateShaderModule", error)) {
                return false;
            }
            VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipeline.stage.module = module;
            pipeline.stage.pName = "CSMain";
            pipeline.layout = pipeline_layout_;
            const VkResult created =
                vkCreateComputePipelines(binding_.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipelines_[pass]);
            vkDestroyShaderModule(binding_.device, module, nullptr);
            if (!Check(created, "vkCreateComputePipelines", error)) {
                return false;
            }
        }
        std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerFrame * 4},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerFrame * 2},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerFrame},
        }};
        for (auto& slot : capture_slots_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = binding_.queueFamilyIndex;
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (!Check(vkCreateCommandPool(binding_.device, &pool, nullptr, &slot.pool), "vkCreateCommandPool", error)) {
                return false;
            }
            VkCommandBufferAllocateInfo buffer{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            buffer.commandPool = slot.pool;
            buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffer.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(binding_.device, &buffer, &slot.commands), "vkAllocateCommandBuffers", error) ||
                !Check(vkCreateFence(binding_.device, &fence, nullptr, &slot.fence), "vkCreateFence", error)) {
                return false;
            }
        }
        for (auto& slot : slots_) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool.queueFamilyIndex = binding_.queueFamilyIndex;
            VkCommandBufferAllocateInfo buffer{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            descriptors.maxSets = kSetsPerFrame;
            descriptors.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
            descriptors.pPoolSizes = sizes.data();
            if (!Check(vkCreateCommandPool(binding_.device, &pool, nullptr, &slot.pool), "vkCreateCommandPool", error)) {
                return false;
            }
            buffer.commandPool = slot.pool;
            buffer.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffer.commandBufferCount = 1;
            if (!Check(vkAllocateCommandBuffers(binding_.device, &buffer, &slot.commands), "vkAllocateCommandBuffers", error) ||
                !Check(vkCreateFence(binding_.device, &fence, nullptr, &slot.fence), "vkCreateFence", error) ||
                !Check(vkCreateDescriptorPool(binding_.device, &descriptors, nullptr, &slot.descriptors),
                       "vkCreateDescriptorPool", error)) {
                return false;
            }
        }
        if (timestamps) {
            VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = static_cast<std::uint32_t>(kFramesInFlight * 2);
            if (vkCreateQueryPool(binding_.device, &queries, nullptr, &query_pool_) != VK_SUCCESS) {
                query_pool_ = VK_NULL_HANDLE;
            }
        }
        // Placeholder for sampled bindings a pass declares but does not use
        // this dispatch: Vulkan requires every statically used descriptor to
        // be valid even when the shader skips it at run time.
        return CreateImage(dummy_, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, error);
    }

    void DestroyPipelineObjects() {
        if (!binding_.device || !vkDestroyPipeline) {
            return;
        }
        DestroyImage(dummy_);
        auto destroy = [&](FrameSlot& slot) {
            if (slot.descriptors) vkDestroyDescriptorPool(binding_.device, slot.descriptors, nullptr);
            if (slot.fence) vkDestroyFence(binding_.device, slot.fence, nullptr);
            if (slot.pool) vkDestroyCommandPool(binding_.device, slot.pool, nullptr);
            slot = {};
        };
        for (auto& slot : slots_) destroy(slot);
        for (auto& slot : capture_slots_) destroy(slot);
        if (query_pool_) vkDestroyQueryPool(binding_.device, query_pool_, nullptr);
        for (auto& pipeline : pipelines_) {
            if (pipeline) vkDestroyPipeline(binding_.device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        if (pipeline_layout_) vkDestroyPipelineLayout(binding_.device, pipeline_layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(binding_.device, set_layout_, nullptr);
        if (sampler_) vkDestroySampler(binding_.device, sampler_, nullptr);
        query_pool_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        set_layout_ = VK_NULL_HANDLE;
        sampler_ = VK_NULL_HANDLE;
    }

    void ReadTiming(FrameSlot& slot, std::size_t slot_index) {
        if (!slot.timing_pending || !query_pool_) {
            return;
        }
        slot.timing_pending = false;
        std::array<std::uint64_t, 2> stamps{};
        if (vkGetQueryPoolResults(binding_.device, query_pool_, static_cast<std::uint32_t>(slot_index * 2), 2,
                                  sizeof(stamps), stamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            stamps[1] >= stamps[0]) {
            last_gpu_ms_ = static_cast<double>(stamps[1] - stamps[0]) * timestamp_period_ / 1'000'000.0;
        }
    }

    bool CreateImage(Image& image, FrameGenExtent extent, VkFormat format, VkImageUsageFlags usage, std::string& error) {
        DestroyImage(image);
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!Check(vkCreateImage(binding_.device, &info, nullptr, &image.image), "vkCreateImage", error)) {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(binding_.device, image.image, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        memory_properties_(binding_.physicalDevice, &properties);
        std::uint32_t type = properties.memoryTypeCount;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        }
        if (type == properties.memoryTypeCount) {
            error = "Vulkan device-local memory unavailable for Frame Generation";
            DestroyImage(image);
            return false;
        }
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkAllocateMemory(binding_.device, &allocate, nullptr, &image.memory), "vkAllocateMemory", error) ||
            !Check(vkBindImageMemory(binding_.device, image.image, image.memory, 0), "vkBindImageMemory", error)) {
            DestroyImage(image);
            return false;
        }
        view.image = image.image;
        if (!Check(vkCreateImageView(binding_.device, &view, nullptr, &image.view), "vkCreateImageView", error)) {
            DestroyImage(image);
            return false;
        }
        image.initialized = false;
        return true;
    }

    void DestroyImage(Image& image) {
        if (!binding_.device || !vkDestroyImage) {
            return;
        }
        if (image.view) vkDestroyImageView(binding_.device, image.view, nullptr);
        if (image.image) vkDestroyImage(binding_.device, image.image, nullptr);
        if (image.memory) vkFreeMemory(binding_.device, image.memory, nullptr);
        image = {};
    }

    void DestroyView(ViewResources& view) {
        // Teardown and failure paths only: wait for every submission first so
        // no recorded command still references these images. A stalled GPU
        // keeps them (a leak) rather than freeing memory still in use.
        if (!WaitIdle()) {
            view.history_state = {};
            return;
        }
        for (auto* group : {&view.history, &view.filtered}) {
            for (auto& image : *group) DestroyImage(image);
        }
        for (auto* group : {&view.luma_newest, &view.luma_previous, &view.flow}) {
            for (auto& image : *group) DestroyImage(image);
        }
        DestroyImage(view.output);
        view.history_state.Invalidate();
        view = {};
    }

    // Our images live in GENERAL for their whole life: legal for sampling,
    // storage and transfers, so dispatches only need memory barriers.
    void ToGeneral(Image& image) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             1, &barrier);
        image.initialized = true;
    }

    void GlobalBarrier(VkPipelineStageFlags source_stage, VkAccessFlags source_access, VkPipelineStageFlags destination_stage,
                       VkAccessFlags destination_access) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        vkCmdPipelineBarrier(commands_, source_stage, destination_stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void SwapchainBarrier(VkImage image, std::uint32_t layer, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access,
                          VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
                          VkPipelineStageFlags destination_stage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source_access;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
        vkCmdPipelineBarrier(commands_, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool EnsureView(ViewResources& view, const FrameGenViewSubmission& submission, const FrameGenTuning& tuning,
                    std::string& error) {
        const FrameGenExtent extent = submission.request.extent;
        const std::uint8_t final_level = FrameGenFinalFlowLevel(tuning.quality);
        const unsigned change = view.history_state.Configure(extent, submission.source.format, submission.request.output,
                                                              final_level);
        const auto levels = FrameGenPyramid(extent);
        constexpr VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (change != FrameGenHistoryState::kNone && !WaitIdle()) {
            // Recent submissions still use the images being replaced.
            view.history_state = {};
            error = "Vulkan Frame Generation GPU work did not finish before reallocation";
            return false;
        }
        if (change & FrameGenHistoryState::kAll || !view.output.image) {
            view.capture.valid = false;
            const auto format = static_cast<VkFormat>(submission.source.format);
            for (auto& history : view.history) {
                if (!CreateImage(history, extent, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, error)) {
                    return false;
                }
            }
            for (std::uint8_t level = 1; level <= kFrameGenPyramidLevels; ++level) {
                if (!CreateImage(view.luma_newest[level], levels[level], VK_FORMAT_R32_SFLOAT, storage, error) ||
                    !CreateImage(view.luma_previous[level], levels[level], VK_FORMAT_R32_SFLOAT, storage, error)) {
                    return false;
                }
                if (level >= 2 && !CreateImage(view.flow[level], levels[level], VK_FORMAT_R16G16B16A16_SFLOAT, storage, error)) {
                    return false;
                }
            }
            const VkFormat intermediate = submission.request.output == FrameGenOutputKind::kFloat16
                ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
            if (!CreateImage(view.output, extent, intermediate, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             error)) {
                return false;
            }
        }
        if (change != FrameGenHistoryState::kNone || !view.filtered[0].image) {
            for (auto& filtered : view.filtered) {
                if (!CreateImage(filtered, levels[final_level], VK_FORMAT_R16G16B16A16_SFLOAT, storage, error)) {
                    return false;
                }
            }
        }
        for (auto* group : {&view.history, &view.filtered}) {
            for (auto& image : *group) {
                if (image.image && !image.initialized) ToGeneral(image);
            }
        }
        for (auto* group : {&view.luma_newest, &view.luma_previous, &view.flow}) {
            for (auto& image : *group) {
                if (image.image && !image.initialized) ToGeneral(image);
            }
        }
        if (!view.output.initialized) ToGeneral(view.output);
        return true;
    }

    // Released application images are in COLOR_ATTACHMENT_OPTIMAL
    // (XR_KHR_vulkan_enable); the copy returns them there.
    void CopySource(ViewResources& view, const FrameGenImage& image, VkImage source) {
        const auto& rect = image.rect;
        SwapchainBarrier(source, image.array_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        GlobalBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.array_index, 1};
        copy.srcOffset = {rect.offset.x, rect.offset.y, 0};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {static_cast<std::uint32_t>(rect.extent.width), static_cast<std::uint32_t>(rect.extent.height), 1};
        vkCmdCopyImage(commands_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, view.history[view.history_state.Newest()].image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        SwapchainBarrier(source, image.array_index, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        GlobalBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    Image* Resolve(ViewResources& view, FrameGenTexture texture) {
        const std::uint8_t newest = view.history_state.Newest();
        const std::uint8_t previous = view.history_state.Previous();
        switch (texture.kind) {
        case FrameGenTextureKind::kHistoryNewest: return &view.history[newest];
        case FrameGenTextureKind::kHistoryPrevious: return &view.history[previous];
        case FrameGenTextureKind::kLumaNewest: return &view.luma_newest[texture.level];
        case FrameGenTextureKind::kLumaPrevious: return &view.luma_previous[texture.level];
        case FrameGenTextureKind::kFlow: return &view.flow[texture.level];
        case FrameGenTextureKind::kFilteredNewest: return &view.filtered[newest];
        case FrameGenTextureKind::kFilteredPrevious: return &view.filtered[previous];
        case FrameGenTextureKind::kOutput: return &view.output;
        default: return nullptr;
        }
    }

    bool RecordView(const FrameGenViewSubmission& submission, const FrameGenTuning& tuning, ViewResources& view,
                    bool& synthesized, std::string& error) {
        const auto source_it = images_.find(submission.source.swapchain);
        if (source_it == images_.end() || submission.source.image_index >= source_it->second.size() ||
            submission.outputs.size() < tuning.generated) {
            error = "Frame Generation image is not tracked";
            return false;
        }
        std::vector<VkImage> outputs;
        for (const FrameGenImage& image : submission.outputs) {
            const auto output_it = images_.find(image.swapchain);
            if (output_it == images_.end() || image.image_index >= output_it->second.size()) {
                error = "Frame Generation output image is not tracked";
                return false;
            }
            outputs.push_back(output_it->second[image.image_index]);
        }
        const VkImage source = source_it->second[submission.source.image_index];
        if (!EnsureView(view, submission, tuning, error)) {
            return false;
        }

        if (!view.capture.Matches(submission.source)) {
            CopySource(view, submission.source, source);
        }

        const FrameGenViewPlan plan = BuildFrameGenViewPlan(view.history_state.Effective(submission.request), tuning);
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocate.descriptorPool = descriptors_;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &set_layout_;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (!Check(vkAllocateDescriptorSets(binding_.device, &allocate, &set), "vkAllocateDescriptorSets", error)) {
                return false;
            }
            std::array<VkDescriptorImageInfo, 6> infos{};
            std::array<VkWriteDescriptorSet, 6> writes{};
            std::uint32_t count = 0;
            for (std::uint32_t i = 0; i < 6; ++i) {
                const bool input = i < 4;
                Image* image = input ? Resolve(view, dispatch.inputs[i]) : Resolve(view, dispatch.outputs[i - 4]);
                if (!image && !input) {
                    continue; // storage slot not declared by this pass
                }
                infos[count] = {VK_NULL_HANDLE, image ? image->view : dummy_.view, VK_IMAGE_LAYOUT_GENERAL};
                writes[count] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[count].dstSet = set;
                writes[count].dstBinding = i;
                writes[count].descriptorCount = 1;
                writes[count].descriptorType = input ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[count].pImageInfo = &infos[count];
                ++count;
            }
            vkUpdateDescriptorSets(binding_.device, count, writes.data(), 0, nullptr);
            vkCmdBindPipeline(commands_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[static_cast<std::size_t>(dispatch.pass)]);
            vkCmdBindDescriptorSets(commands_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(commands_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FrameGenConstants),
                               &dispatch.constants);
            vkCmdDispatch(commands_, (dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                          (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
            GlobalBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
            bool copied = false;
            for (std::size_t index = 0; index < outputs.size(); ++index) {
                if (!(dispatch.copy_mask & (1u << index))) {
                    continue;
                }
                const VkImage output = outputs[index];
                const FrameGenImage& image = submission.outputs[index];
                SwapchainBarrier(output, image.array_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageCopy out{};
                out.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                out.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.array_index, 1};
                out.dstOffset = {image.rect.offset.x, image.rect.offset.y, 0};
                out.extent = {submission.request.extent.width, submission.request.extent.height, 1};
                vkCmdCopyImage(commands_, view.output.image, VK_IMAGE_LAYOUT_GENERAL, output,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &out);
                SwapchainBarrier(output, image.array_index, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                copied = true;
            }
            if (copied) {
                // The next synthesis (or submission) overwrites the output texture.
                GlobalBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            }
        }
        synthesized = plan.synthesize;
        return true;
    }

    HMODULE loader_{nullptr};
    XrGraphicsBindingVulkanKHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    VkQueue queue_{VK_NULL_HANDLE};
    VkSampler sampler_{VK_NULL_HANDLE};
    VkDescriptorSetLayout set_layout_{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_{VK_NULL_HANDLE};
    std::array<VkPipeline, kFrameGenPassCount> pipelines_{};
    std::array<FrameSlot, kFramesInFlight> slots_{};
    std::array<FrameSlot, kCaptureSlots> capture_slots_{};
    std::uint64_t capture_count_{0};
    VkQueryPool query_pool_{VK_NULL_HANDLE};
    float timestamp_period_{1.0f};
    double last_gpu_ms_{-1.0};
    VkCommandBuffer commands_{VK_NULL_HANDLE};
    VkDescriptorPool descriptors_{VK_NULL_HANDLE};
    std::uint64_t submission_{0};
    Image dummy_;
    std::unordered_map<XrSwapchain, std::vector<VkImage>> images_;
    std::vector<ViewResources> views_;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties_{nullptr};
#define FG_DECLARE(name) PFN_vk##name vk##name{nullptr};
    FG_VK_FUNCTIONS(FG_DECLARE)
#undef FG_DECLARE
};
#undef FG_VK_FUNCTIONS

} // namespace

std::unique_ptr<FrameGenGpu> CreateFrameGenVulkan(const void* binding, std::string& error) {
    auto gpu = std::make_unique<VulkanFrameGen>();
    if (!gpu->Initialize(*static_cast<const XrGraphicsBindingVulkanKHR*>(binding), error)) {
        return nullptr;
    }
    return gpu;
}

} // namespace depthxr
