#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>
#include "depthxr/osd_graphics.h"
#include "depthxr/osd_renderer.h"
#include <cstring>
#include <limits>

namespace depthxr {
namespace {
#define OSD_VK_FUNCTIONS(X) \
 X(GetDeviceQueue) X(CreateBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(BindBufferMemory) \
 X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(FreeMemory) X(DestroyBuffer) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
 X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(ResetFences) X(WaitForFences) X(QueueSubmit)
class VulkanGraphics final : public OsdGraphics {
    HMODULE loader{};
    XrGraphicsBindingVulkanKHR binding{};
    VkQueue queue{};
    VkBuffer staging{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkCommandBuffer commands{};
    VkFence fence{};
    void* mapped{};
    bool pending{},coherent{};
    std::vector<XrSwapchainImageVulkanKHR> images;
#define DECLARE(name) PFN_vk##name vk##name{};
    OSD_VK_FUNCTIONS(DECLARE)
#undef DECLARE
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties{};
    bool Check(VkResult r,const char* op,std::string& error){if(r!=VK_SUCCESS){error=std::string(op)+" VkResult="+std::to_string(r);return false;}return true;}
public:
    ~VulkanGraphics() override { Reset();if(loader)FreeLibrary(loader); }
    const char* Name() const override { return "Vulkan"; }
    std::vector<std::int64_t> Formats() const override { return {VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_SRGB}; }
    bool Rgba(std::int64_t f) const override { return f==VK_FORMAT_R8G8B8A8_SRGB; }
    bool Initialize(const XrGraphicsBindingVulkanKHR& b,std::string& error) {
        binding=b;
        if(!b.instance||!b.physicalDevice||!b.device){error="Invalid Vulkan graphics binding";return false;}
        loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!loader){error="Vulkan loader unavailable";return false;}
        auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        if(!gipa){error="vkGetInstanceProcAddr unavailable";return false;}
        auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(b.instance,"vkGetDeviceProcAddr"));
        memoryProperties=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(b.instance,"vkGetPhysicalDeviceMemoryProperties"));
        auto families=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(b.instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
        if(!gdpa||!memoryProperties||!families){error="Vulkan instance dispatch unavailable";return false;}
#define LOAD(name) vk##name=reinterpret_cast<PFN_vk##name>(gdpa(b.device,"vk" #name));if(!vk##name){error="vk" #name " unavailable";return false;}
        OSD_VK_FUNCTIONS(LOAD)
#undef LOAD
        std::uint32_t count{};families(b.physicalDevice,&count,nullptr);
        std::vector<VkQueueFamilyProperties> props(count);families(b.physicalDevice,&count,props.data());
        if(b.queueFamilyIndex>=props.size()||b.queueIndex>=props[b.queueFamilyIndex].queueCount||!(props[b.queueFamilyIndex].queueFlags&VK_QUEUE_GRAPHICS_BIT)){error="Invalid Vulkan graphics queue family/index";return false;}
        vkGetDeviceQueue(b.device,b.queueFamilyIndex,b.queueIndex,&queue);
        if(!queue){error="Vulkan session queue unavailable";return false;}return true;
    }
    void Reset() override {
        if(pending&&fence)vkWaitForFences(binding.device,1,&fence,VK_TRUE,std::numeric_limits<std::uint64_t>::max());
        pending=false;
        if(mapped)vkUnmapMemory(binding.device,memory);mapped=nullptr;
        if(fence)vkDestroyFence(binding.device,fence,nullptr);fence=VK_NULL_HANDLE;
        if(pool)vkDestroyCommandPool(binding.device,pool,nullptr);pool=VK_NULL_HANDLE;commands=VK_NULL_HANDLE;
        if(staging)vkDestroyBuffer(binding.device,staging,nullptr);staging=VK_NULL_HANDLE;
        if(memory)vkFreeMemory(binding.device,memory,nullptr);memory=VK_NULL_HANDLE;images.clear();
    }
    bool Images(XrSwapchain s,const OsdDispatch& api,std::string& error) override {
        std::uint32_t count{};auto xr=api.images(s,0,&count,nullptr);
        if(XR_FAILED(xr)||!count){error="Vulkan image enumeration count XrResult="+std::to_string(xr);return false;}
        images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xr=api.images(s,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if(XR_FAILED(xr)){error="Vulkan image enumeration XrResult="+std::to_string(xr);return false;}
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.size=OsdBitmap::width*4*768;buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(!Check(vkCreateBuffer(binding.device,&buffer,nullptr,&staging),"vkCreateBuffer",error))return false;
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(binding.device,staging,&requirements);
        VkPhysicalDeviceMemoryProperties props{};memoryProperties(binding.physicalDevice,&props);
        std::uint32_t type=props.memoryTypeCount;
        for(std::uint32_t i=0;i<props.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){
            type=i;if(props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)break;
        }
        if(type==props.memoryTypeCount){error="Vulkan host-visible upload memory unavailable";return false;}
        coherent=(props.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=requirements.size;alloc.memoryTypeIndex=type;
        if(!Check(vkAllocateMemory(binding.device,&alloc,nullptr,&memory),"vkAllocateMemory",error)||
           !Check(vkBindBufferMemory(binding.device,staging,memory,0),"vkBindBufferMemory",error)||
           !Check(vkMapMemory(binding.device,memory,0,VK_WHOLE_SIZE,0,&mapped),"vkMapMemory",error))return false;
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};cp.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;cp.queueFamilyIndex=binding.queueFamilyIndex;
        if(!Check(vkCreateCommandPool(binding.device,&cp,nullptr,&pool),"vkCreateCommandPool",error))return false;
        VkCommandBufferAllocateInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cb.commandPool=pool;cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;
        if(!Check(vkAllocateCommandBuffers(binding.device,&cb,&commands),"vkAllocateCommandBuffers",error))return false;
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};return Check(vkCreateFence(binding.device,&fc,nullptr,&fence),"vkCreateFence",error);
    }
    OsdUpload Upload(std::uint32_t index,const OsdBitmap& b,std::string& error) override {
        if(!b.Valid()){error="Invalid OSD bitmap bounds";return OsdUpload::Failed;}
        if(index>=images.size()||!images[index].image){error="Invalid Vulkan image index";return OsdUpload::Failed;}
        if(pending){auto r=vkGetFenceStatus(binding.device,fence);if(r==VK_NOT_READY)return OsdUpload::Busy;if(!Check(r,"vkGetFenceStatus",error))return OsdUpload::Failed;pending=false;}
        if(!Check(vkResetCommandPool(binding.device,pool,0),"vkResetCommandPool",error))return OsdUpload::Failed;
        std::memcpy(mapped,b.pixels.data(),b.pixels.size()*4);
        std::memset(static_cast<std::byte*>(mapped)+b.pixels.size()*4,0,(OsdBitmap::width*OsdBitmap::texture_height-b.pixels.size())*4);
        if(!coherent){VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=memory;range.size=VK_WHOLE_SIZE;if(!Check(vkFlushMappedMemoryRanges(binding.device,1,&range),"vkFlushMappedMemoryRanges",error))return OsdUpload::Failed;}
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(!Check(vkBeginCommandBuffer(commands,&begin),"vkBeginCommandBuffer",error))return OsdUpload::Failed;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=images[index].image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={OsdBitmap::width,OsdBitmap::texture_height,1};
        vkCmdCopyBufferToImage(commands,staging,images[index].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        if(!Check(vkEndCommandBuffer(commands),"vkEndCommandBuffer",error)||!Check(vkResetFences(binding.device,1,&fence),"vkResetFences",error))return OsdUpload::Failed;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&commands;
        if(!Check(vkQueueSubmit(queue,1,&submit,fence),"vkQueueSubmit",error))return OsdUpload::Failed;
        pending=true;return OsdUpload::Complete;
    }
};
#undef OSD_VK_FUNCTIONS
}
std::unique_ptr<OsdGraphics> CreateOsdVulkan(const void* binding,std::string& error){
    auto graphics=std::make_unique<VulkanGraphics>();
    if(!graphics->Initialize(*static_cast<const XrGraphicsBindingVulkanKHR*>(binding),error))return nullptr;
    return graphics;
}
}
