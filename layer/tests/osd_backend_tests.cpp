#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>
#include "depthxr/osd_renderer.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace depthxr;
using Microsoft::WRL::ComPtr;
namespace {
void Check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
ID3D12Resource* dxImage{};VkImage vkImage{};
XrResult XRAPI_PTR Images(XrSwapchain,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* images){
    *count=1;if(capacity){if(images->type==XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)->texture=dxImage;
    else if(images->type==XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)reinterpret_cast<XrSwapchainImageVulkanKHR*>(images)->image=vkImage;else return XR_ERROR_VALIDATION_FAILURE;}return XR_SUCCESS;
}
OsdBitmap Bitmap(){OsdBitmap b;b.height=210;b.pixels.resize(960*210);for(std::size_t i=0;i<b.pixels.size();++i)b.pixels[i]=0xff000000u|static_cast<std::uint32_t>(i);return b;}
void Upload(OsdGraphics& graphics,const OsdBitmap& b){std::string error;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;){auto r=graphics.Upload(0,b,error);if(r==OsdUpload::Complete)return;if(r==OsdUpload::Failed){std::cerr<<error<<'\n';std::exit(1);}Check(std::chrono::steady_clock::now()<deadline,"Upload busy forever");std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
void SeedOldPixels(OsdGraphics& graphics){OsdBitmap old;old.height=OsdBitmap::texture_height;old.pixels.assign(OsdBitmap::width*old.height,0xffff00ff);Upload(graphics,old);}
int D3D12Test(DXGI_FORMAT format){
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"DXGI factory");Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),"WARP adapter");
    Check(SUCCEEDED(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"D3D12 WARP device");
    D3D12_COMMAND_QUEUE_DESC q{};Check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"D3D12 queue");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=960;desc.Height=768;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> texture;Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture))),"D3D12 texture");dxImage=texture.Get();
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};binding.device=device.Get();binding.queue=queue.Get();std::string error;auto graphics=CreateOsdGraphics(&binding,error);Check(!!graphics,"D3D12 binding dispatch");
    OsdDispatch api{};api.images=Images;Check(graphics->Images(reinterpret_cast<XrSwapchain>(1),api,error),error.c_str());auto bitmap=Bitmap();SeedOldPixels(*graphics);Upload(*graphics,bitmap);Upload(*graphics,bitmap);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;Check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"D3D12 readback");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> cmd;ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"D3D12 test allocator");Check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd))),"D3D12 test list");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};cmd->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=texture.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);cmd->ResourceBarrier(1,&barrier);Check(SUCCEEDED(cmd->Close()),"D3D12 test close");ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);
    Check(SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"D3D12 test fence");auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(event&&SUCCEEDED(queue->Signal(fence.Get(),1))&&SUCCEEDED(fence->SetEventOnCompletion(1,event)),"D3D12 fence signal");Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"D3D12 GPU timeout");CloseHandle(event);
    void* data{};D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};Check(SUCCEEDED(readback->Map(0,&range,&data)),"D3D12 readback map");
    for(int y=0;y<bitmap.height;++y)Check(std::memcmp(static_cast<const char*>(data)+y*footprint.Footprint.RowPitch,bitmap.pixels.data()+y*960,960*4)==0,"D3D12 pixel mismatch");
    for(int y=bitmap.height;y<OsdBitmap::texture_height;++y)for(int x=0;x<OsdBitmap::width;++x)Check(reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(data)+y*footprint.Footprint.RowPitch)[x]==0,"D3D12 stale pixels outside the OSD rect");
    readback->Unmap(0,nullptr);Check(graphics->Upload(1,bitmap,error)==OsdUpload::Failed,"D3D12 invalid index accepted");graphics->Reset();std::cout<<"D3D12 format="<<format<<" upload, reuse, bounds and GPU readback passed\n";return 0;
}
#define VK_TEST_FUNCTIONS(X) X(CreateInstance) X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(DestroyDevice) X(GetDeviceQueue) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) X(QueueSubmit)
int VulkanTest(VkFormat format){
    auto loader=LoadLibraryExW(L"vulkan-1.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!loader){std::cout<<"Vulkan loader absent\n";return 77;}
#define LOAD(name) auto vk##name=reinterpret_cast<PFN_vk##name>(GetProcAddress(loader,"vk" #name));Check(!!vk##name,"Vulkan test entry " #name);
    VK_TEST_FUNCTIONS(LOAD)
#undef LOAD
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="VectorXR OSD backend test";app.apiVersion=VK_API_VERSION_1_0;VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&app;VkInstance instance{};
    if(vkCreateInstance(&ic,nullptr,&instance)!=VK_SUCCESS){FreeLibrary(loader);return 77;}
    std::uint32_t count{};Check(vkEnumeratePhysicalDevices(instance,&count,nullptr)==VK_SUCCESS,"Vulkan device enumeration");if(!count){vkDestroyInstance(instance,nullptr);FreeLibrary(loader);return 77;}
    std::vector<VkPhysicalDevice> devices(count);Check(vkEnumeratePhysicalDevices(instance,&count,devices.data())==VK_SUCCESS,"Vulkan enumerate devices");auto physical=devices.front();vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    std::uint32_t family=0;while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;Check(family<count,"Vulkan graphics family");float priority=1;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;VkDevice device{};Check(vkCreateDevice(physical,&dc,nullptr,&device)==VK_SUCCESS,"Vulkan create device");VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical,&mem);
    auto memoryType=[&](std::uint32_t bits,VkMemoryPropertyFlags flags){for(std::uint32_t i=0;i<mem.memoryTypeCount;++i)if((bits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&flags)==flags)return i;Check(false,"Vulkan memory type");return 0u;};
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=format;image.extent={960,768,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Check(vkCreateImage(device,&image,nullptr,&vkImage)==VK_SUCCESS,"Vulkan test image");VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,vkImage,&req);VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,0);VkDeviceMemory imageMem{};Check(vkAllocateMemory(device,&alloc,nullptr,&imageMem)==VK_SUCCESS&&vkBindImageMemory(device,vkImage,imageMem,0)==VK_SUCCESS,"Vulkan image memory");
    auto bitmap=Bitmap();VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=OsdBitmap::width*OsdBitmap::texture_height*4;bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;VkBuffer buffer{};Check(vkCreateBuffer(device,&bc,nullptr,&buffer)==VK_SUCCESS,"Vulkan readback buffer");vkGetBufferMemoryRequirements(device,buffer,&req);alloc.allocationSize=req.size;alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);VkDeviceMemory bufferMem{};Check(vkAllocateMemory(device,&alloc,nullptr,&bufferMem)==VK_SUCCESS&&vkBindBufferMemory(device,buffer,bufferMem,0)==VK_SUCCESS,"Vulkan readback memory");
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;VkCommandPool pool{};Check(vkCreateCommandPool(device,&pc,nullptr,&pool)==VK_SUCCESS,"Vulkan test pool");VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkCommandBuffer cmd{};Check(vkAllocateCommandBuffers(device,&ca,&cmd)==VK_SUCCESS,"Vulkan test command buffer");VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(vkCreateFence(device,&fc,nullptr,&fence)==VK_SUCCESS,"Vulkan test fence");
    auto begin=[&]{Check(vkResetCommandPool(device,pool,0)==VK_SUCCESS,"Vulkan test reset pool");VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&bi)==VK_SUCCESS,"Vulkan test begin");};
    auto submit=[&]{Check(vkEndCommandBuffer(cmd)==VK_SUCCESS,"Vulkan test end");Check(vkResetFences(device,1,&fence)==VK_SUCCESS,"Vulkan test reset fence");VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&si,fence)==VK_SUCCESS&&vkWaitForFences(device,1,&fence,VK_TRUE,10000000000ull)==VK_SUCCESS,"Vulkan test submit/wait");};
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=vkImage;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    begin();vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);submit();
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};binding.instance=instance;binding.physicalDevice=physical;binding.device=device;binding.queueFamilyIndex=family;std::string error;auto graphics=CreateOsdGraphics(&binding,error);Check(!!graphics,error.c_str());OsdDispatch api{};api.images=Images;Check(graphics->Images(reinterpret_cast<XrSwapchain>(1),api,error),error.c_str());SeedOldPixels(*graphics);Upload(*graphics,bitmap);Upload(*graphics,bitmap);
    begin();barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={960,OsdBitmap::texture_height,1};vkCmdCopyImageToBuffer(cmd,vkImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);submit();
    void* data{};Check(vkMapMemory(device,bufferMem,0,VK_WHOLE_SIZE,0,&data)==VK_SUCCESS,"Vulkan readback map");VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=bufferMem;range.size=VK_WHOLE_SIZE;Check(vkInvalidateMappedMemoryRanges(device,1,&range)==VK_SUCCESS,"Vulkan readback invalidate");Check(std::memcmp(data,bitmap.pixels.data(),bitmap.pixels.size()*4)==0,"Vulkan pixel mismatch");for(std::size_t i=bitmap.pixels.size();i<OsdBitmap::width*OsdBitmap::texture_height;++i)Check(static_cast<const std::uint32_t*>(data)[i]==0,"Vulkan stale pixels outside the OSD rect");vkUnmapMemory(device,bufferMem);
    Check(graphics->Upload(1,bitmap,error)==OsdUpload::Failed,"Vulkan invalid index accepted");graphics.reset();vkDestroyFence(device,fence,nullptr);vkDestroyCommandPool(device,pool,nullptr);vkDestroyBuffer(device,buffer,nullptr);vkFreeMemory(device,bufferMem,nullptr);vkDestroyImage(device,vkImage,nullptr);vkFreeMemory(device,imageMem,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);FreeLibrary(loader);
    std::cout<<"Vulkan format="<<format<<" GPU upload, reuse, bounds and readback passed\n";return 0;
}
}
int main(int argc,char** argv){
    if(argc>1&&std::string(argv[1])=="vulkan") {
        const auto first=VulkanTest(VK_FORMAT_R8G8B8A8_SRGB);
        return first?first:VulkanTest(VK_FORMAT_B8G8R8A8_SRGB);
    }
    const auto first=D3D12Test(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    return first?first:D3D12Test(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
}
