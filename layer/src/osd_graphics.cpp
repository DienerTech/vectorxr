#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>
#include "depthxr/osd_graphics.h"
#include "depthxr/osd_renderer.h"
#include <algorithm>
#include <cstring>
#include <array>
#include <limits>

namespace depthxr {
using Microsoft::WRL::ComPtr;
namespace {
std::string Result(const char* operation, std::int64_t code) { return std::string(operation)+" result="+std::to_string(code); }
// Every row of a swapchain image below the bitmap's height must stay 0, so
// filtering never meets old pixels. An image's rows are 0 below the height
// last uploaded to it (all rows count until its first upload), so an upload
// copies the bitmap's rows and clears only the rows the image's last, taller
// panel left, not the whole 960x768 texture.
class D3D11Graphics final : public OsdGraphics {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<int> image_rows;
    std::vector<std::uint32_t> zeros;
public:
    explicit D3D11Graphics(ID3D11Device* d) : device(d) { d->GetImmediateContext(&context); }
    const char* Name() const override { return "D3D11"; }
    std::vector<std::int64_t> Formats() const override { return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}; }
    bool Rgba(std::int64_t f) const override { return f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
    bool Images(XrSwapchain s,const OsdDispatch& api,std::string& error) override {
        zeros.assign(OsdBitmap::width*OsdBitmap::texture_height,0);
        std::uint32_t count{}; auto r=api.images(s,0,&count,nullptr);
        if(XR_FAILED(r)||!count){error=Result("xrEnumerateSwapchainImages count",r);return false;}
        images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        image_rows.assign(count,OsdBitmap::texture_height);
        r=api.images(s,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if(XR_FAILED(r)){error=Result("xrEnumerateSwapchainImages",r);return false;} return true;
    }
    OsdUpload Upload(std::uint32_t index,const OsdBitmap& b,std::string& error) override {
        if(!b.Valid()){error="Invalid OSD bitmap bounds";return OsdUpload::Failed;}
        if(index>=images.size()||!images[index].texture){error="Invalid D3D11 image index";return OsdUpload::Failed;}
        const auto rows=static_cast<UINT>(b.height), stale=static_cast<UINT>(image_rows[index]);
        D3D11_BOX box{0,0,0,OsdBitmap::width,rows,1};
        context->UpdateSubresource(images[index].texture,0,&box,b.pixels.data(),OsdBitmap::width*4,0);
        if(stale>rows) {
            box={0,rows,0,OsdBitmap::width,stale,1};
            context->UpdateSubresource(images[index].texture,0,&box,zeros.data(),OsdBitmap::width*4,0);
        }
        image_rows[index]=b.height;
        const auto hr=device->GetDeviceRemovedReason();
        if(FAILED(hr)){error=Result("D3D11 device removed",hr);return OsdUpload::Failed;} return OsdUpload::Complete;
    }
    void Reset() override { images.clear(); image_rows.clear(); zeros.clear(); }
};

class D3D12Graphics final : public OsdGraphics {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Resource> staging;
    ComPtr<ID3D12Fence> fence;
    std::vector<XrSwapchainImageD3D12KHR> images;
    // As in D3D11Graphics: each image's rows are 0 below image_rows, and the
    // staging buffer's below staging_rows.
    std::vector<int> image_rows;
    int staging_rows{};
    HANDLE event{};
    UINT64 serial{};
    bool signal_pending{};
    std::byte* mapped{};
public:
    explicit D3D12Graphics(const XrGraphicsBindingD3D12KHR& b):device(b.device),queue(b.queue){}
    ~D3D12Graphics() override { Reset(); }
    const char* Name() const override { return "D3D12"; }
    std::vector<std::int64_t> Formats() const override { return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}; }
    bool Rgba(std::int64_t f) const override { return f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
    void Reset() override {
        // ExecuteCommandLists has no result. If signaling failed, retry before
        // waiting: an unqueued fence value can never complete on a healthy device.
        if(signal_pending && SUCCEEDED(device->GetDeviceRemovedReason())) {
            if(FAILED(queue->Signal(fence.Get(),serial)) && SUCCEEDED(device->GetDeviceRemovedReason())) {
                // Exceptional live-device failure: retain in-flight resources
                // rather than hang or free memory the GPU may still be using.
                for(const auto& image:images)if(image.texture)image.texture->AddRef();
                commands.Detach();allocator.Detach();staging.Detach();fence.Detach();
                mapped=nullptr;
            }
        }
        if(fence && serial && fence->GetCompletedValue()<serial && SUCCEEDED(device->GetDeviceRemovedReason())) {
            if(SUCCEEDED(fence->SetEventOnCompletion(serial,event))) WaitForSingleObject(event,INFINITE);
        }
        if(mapped && staging) staging->Unmap(0,nullptr);
        mapped=nullptr;commands.Reset();allocator.Reset();staging.Reset();fence.Reset();images.clear();image_rows.clear();staging_rows=0;serial=0;signal_pending=false;
        if(event)CloseHandle(event);event=nullptr;
    }
    bool Images(XrSwapchain s,const OsdDispatch& api,std::string& error) override {
        std::uint32_t count{};auto r=api.images(s,0,&count,nullptr);
        if(XR_FAILED(r)||!count){error=Result("xrEnumerateSwapchainImages count",r);return false;}
        images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        image_rows.assign(count,OsdBitmap::texture_height);staging_rows=OsdBitmap::texture_height;
        r=api.images(s,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if(XR_FAILED(r)){error=Result("xrEnumerateSwapchainImages",r);return false;}
        auto check=[&](HRESULT hr,const char* op){if(FAILED(hr)){error=Result(op,hr);return false;}return true;};
        if(!check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"D3D12 allocator")||
           !check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"D3D12 command list")||
           !check(commands->Close(),"D3D12 initial close")||!check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"D3D12 fence"))return false;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event){error="D3D12 fence event failed";return false;}
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=OsdBitmap::width*4*768;
        desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(!check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&staging)),"D3D12 staging"))return false;
        D3D12_RANGE read{0,0};return check(staging->Map(0,&read,reinterpret_cast<void**>(&mapped)),"D3D12 map");
    }
    OsdUpload Upload(std::uint32_t i,const OsdBitmap& b,std::string& error) override {
        if(!b.Valid()){error="Invalid OSD bitmap bounds";return OsdUpload::Failed;}
        if(i>=images.size()||!images[i].texture){error="Invalid D3D12 image index";return OsdUpload::Failed;}
        auto hr=device->GetDeviceRemovedReason();if(FAILED(hr)){error=Result("D3D12 device removed",hr);return OsdUpload::Failed;}
        if(fence->GetCompletedValue()<serial)return OsdUpload::Busy;
        hr=allocator->Reset();if(SUCCEEDED(hr))hr=commands->Reset(allocator.Get(),nullptr);
        if(FAILED(hr)){error=Result("D3D12 reset commands",hr);return OsdUpload::Failed;}
        // 960 * 4 is already aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
        static_assert(OsdBitmap::width*4 % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT==0);
        constexpr std::size_t pitch=OsdBitmap::width*4;
        std::memcpy(mapped,b.pixels.data(),b.pixels.size()*4);
        if(staging_rows>b.height)std::memset(mapped+b.height*pitch,0,(staging_rows-b.height)*pitch);
        staging_rows=b.height;
        const auto rows=static_cast<UINT>(std::max(b.height,image_rows[i]));
        image_rows[i]=b.height;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={images[i].texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST};
        commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=images[i].texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=staging.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint={images[i].texture->GetDesc().Format,OsdBitmap::width,OsdBitmap::texture_height,1,OsdBitmap::width*4};
        const D3D12_BOX box{0,0,0,OsdBitmap::width,rows,1};
        commands->CopyTextureRegion(&dst,0,0,0,&src,&box);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);commands->ResourceBarrier(1,&barrier);
        hr=commands->Close();if(FAILED(hr)){error=Result("D3D12 close commands",hr);return OsdUpload::Failed;}
        ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);
        hr=queue->Signal(fence.Get(),++serial);
        signal_pending=FAILED(hr);
        if(FAILED(hr)){error=Result("D3D12 queue signal",hr);return OsdUpload::Failed;}return OsdUpload::Complete;
    }
};
}

std::unique_ptr<OsdGraphics> CreateOsdD3D11(ID3D11Device* device) { return device?std::make_unique<D3D11Graphics>(device):nullptr; }
std::unique_ptr<OsdGraphics> CreateOsdVulkan(const void*,std::string&);
std::unique_ptr<OsdGraphics> CreateOsdGraphics(const void* chain,std::string& error) {
    for(auto* b=static_cast<const XrBaseInStructure*>(chain);b;b=b->next){
        if(b->type==XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)return CreateOsdD3D11(reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(b)->device);
        if(b->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR){auto& binding=*reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(b);if(binding.device&&binding.queue&&binding.queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT)return std::make_unique<D3D12Graphics>(binding);error="Invalid D3D12 device or direct queue";return nullptr;}
        if(b->type==XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR)return CreateOsdVulkan(b,error);
    }
    error="OSD requires Direct3D 11, Direct3D 12, or Vulkan";return nullptr;
}
}
