#include <windows.h>
#include <d3d11.h>
#include "depthxr/osd_renderer.h"
#include <openxr/openxr_platform.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace depthxr;
namespace {
void Check(bool ok, const char* message) { if (!ok) { std::cerr<<message<<'\n'; std::exit(1); } }
ID3D11Device* device{};
ID3D11DeviceContext* context{};
std::vector<ID3D11Texture2D*> textures;
int creates{}, acquires{}, waits{}, releases{}, destroys{}, spaces{}, timeouts{};
bool unsupported{}, fail_create{};
XrResult XRAPI_PTR Formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    *count=1; if (capacity) formats[0]=unsupported?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    ++creates;
    Check(info->width==960 && info->height==768 && info->arraySize==1 && info->faceCount==1,"Invalid swapchain geometry");
    Check((info->usageFlags&XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)!=0,"Upload usage missing");
    if (fail_create) return XR_ERROR_RUNTIME_FAILURE;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width=info->width; desc.Height=info->height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=static_cast<DXGI_FORMAT>(info->format); desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    for(int i=0;i<3;++i) { ID3D11Texture2D* texture{}; Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&texture)),"WARP texture failed"); textures.push_back(texture); }
    *swapchain=reinterpret_cast<XrSwapchain>(2); return XR_SUCCESS;
}
XrResult XRAPI_PTR Images(XrSwapchain, std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count=static_cast<std::uint32_t>(textures.size());
    if(capacity) for(std::size_t i=0;i<textures.size();++i) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i].texture=textures[i];
    return XR_SUCCESS;
}
XrResult XRAPI_PTR Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) { ++acquires; *index=0; return XR_SUCCESS; }
XrResult XRAPI_PTR Wait(XrSwapchain, const XrSwapchainImageWaitInfo* info) { ++waits; Check(info->timeout==0,"OSD blocked the frame thread"); if(timeouts>0) { --timeouts; return XR_TIMEOUT_EXPIRED; } return XR_SUCCESS; }
XrResult XRAPI_PTR Release(XrSwapchain, const XrSwapchainImageReleaseInfo*) { ++releases; return XR_SUCCESS; }
XrResult XRAPI_PTR Destroy(XrSwapchain) { ++destroys; for(auto* texture:textures) texture->Release(); textures.clear(); return XR_SUCCESS; }
XrResult XRAPI_PTR Space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    Check(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW && info->poseInReferenceSpace.orientation.w==1,"OSD must use an identity VIEW space");
    ++spaces; *space=reinterpret_cast<XrSpace>(3); return XR_SUCCESS;
}
XrResult XRAPI_PTR DestroySpace(XrSpace) { --spaces; return XR_SUCCESS; }
const XrCompositionLayerBaseHeader* WaitVisible(OsdRenderer& renderer, const XrFrameEndInfo& frame) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do { if (const auto* layer=renderer.Append(frame,true)) return layer; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while(std::chrono::steady_clock::now()<deadline);
    return nullptr;
}
const OsdDispatch dispatch{Formats,Create,Images,Acquire,Wait,Release,Destroy,Space,DestroySpace};
const XrSession session=reinterpret_cast<XrSession>(1);
OsdSnapshot Snapshot() { return {"FlightSimulator.exe","SteamVR / OpenXR","Async","Depth  /  Pivot  /  Quadviews","Turbo experiments",true}; }
void WriteBitmap(const char* path, const OsdBitmap& bitmap) {
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(bitmap.pixels.size()*4);
    BITMAPINFOHEADER info{sizeof(info),OsdBitmap::width,-bitmap.height,1,32,BI_RGB};
    std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(&file),sizeof(file));
    out.write(reinterpret_cast<const char*>(&info),sizeof(info)); out.write(reinterpret_cast<const char*>(bitmap.pixels.data()),bitmap.pixels.size()*4);
}
}
int main(int argc, char** argv) {
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"WARP device creation failed");
    {
        OsdTelemetry stats; auto now=std::chrono::steady_clock::now(); stats.Tick(now);
        for(int i=0;i<120;++i) { now+=std::chrono::milliseconds(i<114?10:20); stats.Tick(now); }
        Check(std::abs(stats.Mean()-10.5)<.001 && stats.Percentile95()==10,"Telemetry statistics incorrect");
        auto settings=OsdSettings{}; settings.opacity=100; settings.show_clock=false;
        const auto start=std::chrono::steady_clock::now();
        const auto bitmap=RasterizeOsd(settings,false,Snapshot(),stats,false);
        Check(bitmap.height<=768 && bitmap.pixels.size()==static_cast<std::size_t>(960*bitmap.height),"Invalid raster bounds");
        Check((bitmap.pixels[0]>>24)==0 && (bitmap.pixels[100*960+100]>>24)==255,"Rounded panel alpha incorrect");
        const auto rgba=RasterizeOsd(settings,false,Snapshot(),stats,true);
        const auto bgraPixel=bitmap.pixels[90*960+3], rgbaPixel=rgba.pixels[90*960+3];
        Check((bgraPixel&255)==((rgbaPixel>>16)&255) && ((bgraPixel>>16)&255)==(rgbaPixel&255),"RGBA channel conversion incorrect");
        if(argc>1) WriteBitmap(argv[1],bitmap);
        std::cout<<"Two rasters (includes initial LUT): "<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<" ms\n";
        stats.Tick(now+std::chrono::seconds(2)); Check(stats.samples.empty(),"Paused session polluted frame history");
        for(int i=0;i<140;++i) stats.Tick(now+std::chrono::seconds(3)+std::chrono::milliseconds(i*11));
        Check(stats.samples.size()==120,"History grew past bound");
    }
    {
        OsdRenderer renderer; renderer.Initialize(session,device,4,dispatch);
        OsdSettings settings; settings.show_clock=false;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto* original=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO}; frame.layerCount=1; frame.layers=&original;
        renderer.Prepare(settings,Snapshot(),false,false);
        Check(!renderer.Append(frame,true) && creates==0,"Disabled OSD created resources");
        settings.enabled=true; renderer.Prepare(settings,Snapshot(),true,false);
        timeouts=2;
        Check(!renderer.Append(frame,true),"First frame waited for CPU rasterization");
        const auto raster_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(acquires==0 && std::chrono::steady_clock::now()<raster_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); renderer.Append(frame,true);
        }
        Check(acquires==1 && releases==0,"Timed-out image was used or released");
        Check(!renderer.Append(frame,true) && acquires==1 && waits==2,"Pending acquisition was not retained");
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
        const auto* layer=renderer.Append(frame,true);
        Check(layer && releases==1 && acquires==1,"Successful retry failed to release image");
        D3D11_PRIMITIVE_TOPOLOGY topology; context->IAGetPrimitiveTopology(&topology);
        Check(topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,"OSD changed application graphics state");
        const auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(layer);
        Check(quad->space==reinterpret_cast<XrSpace>(3) && quad->pose.position.x>0 && quad->pose.position.y<0 && quad->pose.position.z<0,"Head-relative pose incorrect");
        Check(quad->layerFlags==XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT && quad->eyeVisibility==XR_EYE_VISIBILITY_BOTH,"Quad blend or eye visibility incorrect");
        Check(frame.layerCount==1 && frame.layers==&original,"Application submission was modified");
        Check(renderer.Append(frame,true) && acquires==1,"Unchanged panel was uploaded again before refresh deadline");
        frame.layerCount=4; Check(!renderer.Append(frame,true),"Exceeded runtime layer count");
        frame.layerCount=0; Check(!renderer.Append(frame,true),"Added OSD to an empty frame");
        frame.layerCount=1; Check(!renderer.Append(frame,false),"OSD ignored shouldRender");
        renderer.Prepare(settings,Snapshot(),true,false); Check(renderer.Append(frame,true),"Held starting binding incorrectly toggled");
        renderer.Prepare(settings,Snapshot(),false,false);
        renderer.Prepare(settings,Snapshot(),true,false); Check(!renderer.Append(frame,true),"Hide binding did not toggle");
        renderer.Prepare(settings,Snapshot(),false,false);
        renderer.Prepare(settings,Snapshot(),true,true); Check(renderer.Append(frame,true),"Show binding did not toggle");
        Check(renderer.Status().compact,"Cycle binding did not switch layout");
        settings.enabled=false; renderer.Prepare(settings,Snapshot(),false,false); Check(!renderer.Append(frame,true),"Live disable failed");
        settings.enabled=true; settings.compact=false; settings.horizontal_degrees=-30; renderer.Prepare(settings,Snapshot(),false,false);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && !renderer.Status().compact && quad->pose.position.x<0,"Live enable did not restore saved layout/position");
        renderer.SubmissionFailed(XR_ERROR_LAYER_INVALID);
        Check(!renderer.Append(frame,true),"Failed submission was repeated");
        renderer.ResetPresentation();
        renderer.Prepare(settings,Snapshot(),false,false);
        Check(WaitVisible(renderer,frame),"Session rebegin did not restore presentation");
        renderer.Shutdown(); Check(textures.empty() && spaces==0,"Resources leaked on teardown");
        renderer.Initialize(session,device,4,dispatch); fail_create=true;
        renderer.Prepare(settings,Snapshot(),false,false); Check(!renderer.Append(frame,true),"Creation failure escaped");
        const auto attempts=creates;
        Check(!renderer.Append(frame,true) && creates==attempts,"Repeated resource failure every frame");
        fail_create=false; settings.enabled=false; renderer.Prepare(settings,Snapshot(),false,false);
        settings.enabled=true; renderer.Prepare(settings,Snapshot(),false,false);
        Check(WaitVisible(renderer,frame),"Explicit retry did not recover");
        renderer.Shutdown(); unsupported=true; renderer.Initialize(session,device,4,dispatch);
        renderer.Prepare(settings,Snapshot(),false,false);
        Check(!renderer.Append(frame,true) && renderer.Status().message.find("sRGB")!=std::string::npos,"Unsupported format was not surfaced");
        renderer.Shutdown(); renderer.Initialize(session,nullptr,4,dispatch);
        renderer.Prepare(settings,Snapshot(),false,false);
        Check(!renderer.Append(frame,true) && !renderer.Status().available,"Unsupported graphics API was not isolated");
    }
    context->Release(); device->Release();
    std::cout<<"OSD telemetry, rasterization, bindings, live settings, and swapchain lifecycle passed.\n";
}
