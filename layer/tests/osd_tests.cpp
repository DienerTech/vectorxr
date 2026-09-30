#include <windows.h>
#include <d3d11.h>
#include "depthxr/osd_renderer.h"
#include "depthxr/logger.h"
#include <openxr/openxr_platform.h>
#include <cmath>
#include <algorithm>
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
std::vector<std::int64_t> format_override;
XrResult XRAPI_PTR Formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    if(!format_override.empty()) {
        *count=static_cast<std::uint32_t>(format_override.size());
        if(capacity) { Check(capacity>=*count,"Insufficient format enumeration capacity");std::copy(format_override.begin(),format_override.end(),formats); }
        return XR_SUCCESS;
    }
    *count=1; if (capacity) formats[0]=unsupported?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; return XR_SUCCESS;
}
XrResult XRAPI_PTR Create(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* swapchain) {
    ++creates;
    Check(info->width==OsdBitmap::width && info->height==OsdBitmap::texture_height && info->arraySize==1 && info->faceCount==1,"Invalid swapchain geometry");
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
void CheckUnusedTexturePixels(int width,int height) {
    D3D11_TEXTURE2D_DESC desc{};textures[0]->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback{};
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"D3D11 readback allocation failed");
    context->CopyResource(readback,textures[0]);D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped)),"D3D11 readback map failed");
    for(int y=0;y<OsdBitmap::texture_height;++y)for(int x=0;x<OsdBitmap::width;++x)
        if(x>=width || y>=height)
            Check(reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)[x]==0,"D3D11 stale pixels outside the OSD content");
    context->Unmap(readback,0);readback->Release();
}
void CheckUploadedRaster(const OsdBitmap& expected,DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};textures[0]->GetDesc(&desc);
    Check(desc.Format==format,"OSD ignored the runtime's supported format preference");
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback{};
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)),"D3D11 format readback allocation failed");
    context->CopyResource(readback,textures[0]);D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped)),"D3D11 format readback map failed");
    for(int y=0;y<expected.height;++y)for(int x=0;x<OsdBitmap::width;++x)
        Check(reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)[x]==expected.pixels[y*OsdBitmap::width+x],
            "OSD raster channels do not match the selected swapchain format");
    context->Unmap(readback,0);readback->Release();
}
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
        auto settings=OsdSettings{}; settings.opacity=100; settings.show_clock=false; settings.compact_show_clock=false;
        const auto start=std::chrono::steady_clock::now();
        const auto bitmap=RasterizeOsd(settings,false,Snapshot(),stats,false);
        Check(bitmap.height<=768 && bitmap.pixels.size()==static_cast<std::size_t>(960*bitmap.height),"Invalid raster bounds");
        Check((bitmap.pixels[0]>>24)==0 && (bitmap.pixels[100*960+100]>>24)==255,"Rounded panel alpha incorrect");
        const auto rgba=RasterizeOsd(settings,false,Snapshot(),stats,true);
        const auto bgraPixel=bitmap.pixels[90*960+10], rgbaPixel=rgba.pixels[90*960+10];
        Check((bgraPixel&255)==((rgbaPixel>>16)&255) && ((bgraPixel>>16)&255)==(rgbaPixel&255),"RGBA channel conversion incorrect");
        const auto compact_before=RasterizeOsd(settings,true,Snapshot(),stats,false);
        for(int headers=0;headers<16;++headers) {
            auto choices=settings;
            choices.show_brand=headers&1;choices.show_app=headers&2;choices.show_runtime=headers&4;choices.show_clock=headers&8;
            const auto selected=RasterizeOsd(choices,false,Snapshot(),stats,false);
            Check(selected.Valid() && selected.height==bitmap.height-(headers?0:40),"Expanded header did not collapse only when all slots were off");
            if(!headers) {
                // All performance values and body rows move up together, preserving their rendering.
                for(int y=25;y<selected.height-30;++y) for(int x=28;x<932;++x)
                    Check(selected.pixels[y*960+x]==bitmap.pixels[(y+40)*960+x],"Hiding the header clipped or changed expanded content");
                if(argc>4) WriteBitmap(argv[4],selected);
            }
            Check(RasterizeOsd(choices,true,Snapshot(),stats,false).pixels==compact_before.pixels,"Expanded header choices changed compact output");
        }
        if(argc>1) WriteBitmap(argv[1],bitmap);
        settings.accent="custom"; settings.custom_color="#123456";
        settings.body_order={"pivot","modules","turbo","graph"};
        const auto reordered=RasterizeOsd(settings,false,Snapshot(),stats,false);
        bool custom_accent=false;
        for(const auto pixel:reordered.pixels)custom_accent|=(pixel&0xffffff)==0x123456;
        Check(custom_accent,"Custom accent did not reach the raster");
        Check((reordered.pixels[90*960+20]&0xffffff)==0x0d141f,"Colored accent stripe remains at the panel edge");
        // Every compositor sampling boundary must see transparent black in both
        // channel orders, both layouts, and translucent panels.
        for(bool compact:{false,true})for(bool use_rgba:{false,true})for(int opacity:{25,90,100}) {
            settings.opacity=opacity;
            const auto edged=RasterizeOsd(settings,compact,Snapshot(),stats,use_rgba);
            const auto feather=edged.pixels[90*960+6];
            Check((feather&255)==((feather>>8)&255) && (feather&255)==((feather>>16)&255),"OSD feather has a color tint");
            Check((feather>>24)>0 && (feather>>24)<static_cast<unsigned>(255*opacity/100),"OSD alpha edge is not feathered");
            for(int d=0;d<16;++d) {
                const auto top=edged.pixels[d*960+edged.content_width/2];
                const auto bottom=edged.pixels[(edged.height-1-d)*960+edged.content_width/2];
                const auto left=edged.pixels[(edged.height/2)*960+d];
                const auto right=edged.pixels[(edged.height/2)*960+edged.content_width-1-d];
                Check(top==bottom && top==left && top==right,"OSD raster has different colors/alpha on opposing edges");
            }
            for(int y=0;y<edged.height;++y)for(int x=0;x<OsdBitmap::width;++x)
                if(x<3 || y<3 || x>=edged.content_width-3 || y>=edged.height-3)
                    Check(edged.pixels[y*OsdBitmap::width+x]==0,"OSD edge contains color or alpha that can bleed into the scene");
        }
        settings.opacity=100;
        Check(reordered.height==bitmap.height,"Reordering changed panel proportions");
        Check((reordered.pixels[195*960+31]&0xffffff)!=(bitmap.pixels[195*960+31]&0xffffff),"Body order did not move the graph");
        auto long_names=Snapshot(); long_names.application=std::string(150,'W'); long_names.runtime=std::string(150,'W');
        const auto header=RasterizeOsd(settings,true,long_names,stats,false);
        Check(header.height==136,"Runtime name must fit within compact header");
        settings.compact_show_clock=false;
        const auto expanded=RasterizeOsd(settings,false,Snapshot(),stats,false);
        const auto plain=RasterizeOsd(settings,true,Snapshot(),stats,false);
        for (bool turbo:{false,true}) for (bool pivot:{false,true}) {
            settings.compact_show_turbo=turbo; settings.compact_show_pivot=pivot;
            auto states=Snapshot(); states.turbo="Recovery blocked / Analyzing"; states.pivot=std::string(150,'W'); states.compact_pivot="Quick view + Nudge";
            const auto compact=RasterizeOsd(settings,true,states,stats,false);
            Check(compact.height==136+48*static_cast<int>(turbo)+48*static_cast<int>(pivot),"Compact states did not get separate rows");
            for(int y=18;y<100;++y) for(int x=28;x<600;++x)
                Check(plain.pixels[y*960+x]==compact.pixels[y*960+x],"Compact states changed the performance header");
            Check(RasterizeOsd(settings,false,Snapshot(),stats,false).pixels==expanded.pixels,"Compact options changed expanded output");
            for (int y=0;y<compact.height;++y) for (int x=0;x<960;++x)
                if (x<3 || x>=compact.content_width-3 || y<3 || y>=compact.height-3) Check(compact.pixels[y*960+x]==0,"Compact footer spilled outside the panel");
            if(argc>2 && turbo && pivot) WriteBitmap(argv[2],compact);
        }
        auto micro_settings=OsdSettings{};
        micro_settings.compact_show_brand=micro_settings.compact_show_app=micro_settings.compact_show_runtime=micro_settings.compact_show_clock=false;
        micro_settings.compact_metrics="fps";
        const auto micro=RasterizeOsd(micro_settings,true,Snapshot(),stats,false);
        Check(micro.Valid() && micro.content_width==266 && micro.height==92,"FPS-only display did not shrink");
        for(int y=0;y<micro.height;++y) for(int x=micro.content_width-3;x<960;++x)
            Check(micro.pixels[y*960+x]==0,"Micro display left pixels outside its cropped width");
        if(argc>3) WriteBitmap(argv[3],micro);
        micro_settings.compact_metrics="none";
        const auto empty=RasterizeOsd(micro_settings,true,Snapshot(),stats,false);
        Check(std::all_of(empty.pixels.begin(),empty.pixels.end(),[](auto p){return p==0;}),"Empty compact selection rendered a panel");
        for(const auto* metrics:{"all","fps","frameTime","none"}) for(int flags=0;flags<64;++flags) {
            auto options=OsdSettings{};options.compact_metrics=metrics;
            options.compact_show_brand=flags&1;options.compact_show_app=flags&2;
            options.compact_show_runtime=flags&4;options.compact_show_clock=flags&8;
            options.compact_show_turbo=flags&16;options.compact_show_pivot=flags&32;
            const auto layout=CompactOsdLayout(options);
            Check(layout.width>=240 && layout.width<=960 && layout.height>=68 && layout.height<=232,"Compact geometry escaped texture bounds");
            if(options.compact_show_turbo && options.compact_show_pivot) {
                options.compact_show_pivot=false;
                const auto turbo_only=CompactOsdLayout(options);
                Check(layout.pivot_y==layout.turbo_y+48 && layout.width==turbo_only.width && layout.height==turbo_only.height+48,
                    "Adding compact Pivot widened the panel instead of adding a row");
            }
            Check(layout.empty==(flags==0 && options.compact_metrics=="none"),"Empty compact detection failed");
        }
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
        Check(quad->subImage.imageRect.offset.x==1 && quad->subImage.imageRect.offset.y==1 &&
            quad->subImage.imageRect.extent.width==958 && quad->subImage.imageRect.extent.height>0 &&
            quad->subImage.imageRect.extent.height+2<OsdBitmap::texture_height,"OSD sampling rect is missing its initialized guard pixels");
        CheckUnusedTexturePixels(960,quad->subImage.imageRect.extent.height+2);
        Check(frame.layerCount==1 && frame.layers==&original,"Application submission was modified");
        Check(renderer.Append(frame,true) && acquires==1,"Unchanged panel was uploaded again before refresh deadline");
        // A refresh whose image is not ready yet must keep presenting the last
        // released image instead of blinking the panel off for a frame.
        settings.update_hz=20; renderer.Prepare(settings,Snapshot(),true,false); // binding still held
        timeouts=1000;
        bool presented=true;
        const auto refresh_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(acquires==1 && std::chrono::steady_clock::now()<refresh_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); presented=renderer.Append(frame,true)!=nullptr && presented;
        }
        Check(acquires==2 && releases==1,"Refresh did not acquire a new image");
        Check(presented && renderer.Append(frame,true)!=nullptr && renderer.Status().visible,
              "OSD blinked off while its refreshed image was not ready");
        timeouts=0;
        Check(renderer.Append(frame,true) && releases==2 && acquires==2,"Deferred refresh did not complete once the image was ready");
        settings.update_hz=5; renderer.Prepare(settings,Snapshot(),true,false); // binding still held
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
        settings.scale=25; settings.distance_meters=2;
        renderer.Prepare(settings,Snapshot(),false,false);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && std::abs(quad->size.width/quad->subImage.imageRect.extent.width-.275f/958)<.000001f,"25 percent angular size was not applied");
        const auto full_settings=settings;
        const auto before_micro=creates;
        settings.compact=true;settings.compact_metrics="fps";
        settings.compact_show_brand=settings.compact_show_app=settings.compact_show_runtime=settings.compact_show_clock=false;
        renderer.Prepare(settings,Snapshot(),false,false);
        const auto micro_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        do {
            quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
            if(quad && quad->subImage.imageRect.extent.width==264) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(std::chrono::steady_clock::now()<micro_deadline);
        Check(quad && quad->subImage.imageRect.extent.width==264 && quad->subImage.imageRect.extent.height==90,"Micro content was not cropped at submission");
        Check(std::abs(quad->size.width/quad->subImage.imageRect.extent.width-.275f/958)<.000001f && creates==before_micro,"Micro mode stretched text or recreated its swapchain");
        CheckUnusedTexturePixels(266,92);
        settings.compact_metrics="none";renderer.Prepare(settings,Snapshot(),false,false);
        Check(!renderer.Append(frame,true),"Empty compact selection submitted a layer");
        settings=full_settings;renderer.Prepare(settings,Snapshot(),false,false);
        const auto resources_before_rejection=creates;
        for(const auto rejected:{XR_ERROR_CALL_ORDER_INVALID,XR_ERROR_TIME_INVALID}) {
            renderer.SubmissionFailed(rejected);
            Check(renderer.Append(frame,true)!=nullptr,"Transient frame rejection permanently disabled the OSD");
            Check(creates==resources_before_rejection,"Transient frame rejection recreated healthy OSD resources");
        }
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
    for(const auto preferred:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        // Unsupported higher-ranked formats must be skipped, but the runtime's
        // ordering of the two supported formats must win over backend ordering.
        format_override={DXGI_FORMAT_R16G16B16A16_FLOAT,preferred,
            preferred==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        OsdRenderer renderer;unsupported=false;renderer.Initialize(session,device,4,dispatch);
        auto settings=OsdSettings{};settings.enabled=true;settings.show_clock=false;settings.opacity=90;
        renderer.Prepare(settings,Snapshot(),false,false);
        XrCompositionLayerProjection game{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto* original=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.layerCount=1;frame.layers=&original;
        Check(WaitVisible(renderer,frame),"Runtime-preferred format did not produce an OSD");
        const auto expected=RasterizeOsd(settings,false,Snapshot(),OsdTelemetry{},preferred==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        CheckUploadedRaster(expected,preferred);
        CheckUnusedTexturePixels(expected.content_width,expected.height);
    }
    format_override.clear();
    for(auto level:{LogLevel::Info,LogLevel::Debug}) {
        std::filesystem::path log;
        {
            Logger logger;logger.Initialize((std::filesystem::temp_directory_path()/"vectorxr-osd-tests")/(level==LogLevel::Info?"vectorxr-osd-info.log":"vectorxr-osd-debug.log"));logger.SetLevel(level);log=logger.ActiveLogPath();
            OsdRenderer renderer;XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=device;
            renderer.InitializeGraphics(session,&binding,4,dispatch,&logger,"test-session");
            auto settings=OsdSettings{};renderer.Prepare(settings,Snapshot(),false,false);renderer.RecordPrepare(.1);
            settings.enabled=true;renderer.Prepare(settings,Snapshot(),false,false);renderer.RecordPrepare(.2);renderer.RecordSubmit(.3);
            unsupported=false;
            const XrCompositionLayerProjection diagnostic_projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const auto* diagnostic_layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&diagnostic_projection);
            XrFrameEndInfo diagnostic_frame{XR_TYPE_FRAME_END_INFO};
            diagnostic_frame.layerCount=1;diagnostic_frame.layers=&diagnostic_layer;
            Check(WaitVisible(renderer,diagnostic_frame),"Diagnostic image was not produced");
            renderer.SubmissionFailed(XR_ERROR_CALL_ORDER_INVALID);
            renderer.SubmissionFailed(XR_ERROR_TIME_INVALID);
            renderer.SubmissionFailed(XR_ERROR_LAYER_INVALID);renderer.ReportDiagnostics(true);
        }
        std::ifstream file(log);std::string text((std::istreambuf_iterator<char>(file)),{});
        Check(text.find("OSD initialize")==std::string::npos && text.find("test-session initialize")!=std::string::npos,"OSD session identity missing");
        Check(text.find("XR_ERROR_LAYER_INVALID")!=std::string::npos && text.find("OSD summary")!=std::string::npos,"OSD failure or summary missing");
        Check(text.find("preserving OSD for subsequent frames")!=std::string::npos && text.find("frameRejections=2")!=std::string::npos,"Transient OSD rejection diagnostics missing");
        Check(text.find("mode=disabled")!=std::string::npos && text.find("mode=visible")!=std::string::npos,"OSD baselines not separated");
        Check((text.find("OSD detail")!=std::string::npos)==(level==LogLevel::Debug),"OSD detail log level incorrect");
        Check((text.find("OSD source edges")!=std::string::npos)==(level==LogLevel::Debug),"OSD source diagnostic log level incorrect");
        if(level==LogLevel::Debug)Check(text.find("nonzeroGuardPixels=0")!=std::string::npos &&
            text.find("top[")!=std::string::npos && text.find("bottom[")!=std::string::npos &&
            text.find("left[")!=std::string::npos && text.find("right[")!=std::string::npos,
            "OSD source edge strips or transparent guard evidence missing");
    }
    { OsdMetric metric; for(int i=0;i<100;++i)metric.Add(i<95?.11:20);Check(metric.count==100&&std::abs(metric.P95Upper()-.15)<.001&&metric.maximum==20,"OSD bounded metric histogram incorrect"); }
    context->Release(); device->Release();
    std::cout<<"OSD telemetry, rasterization, bindings, live settings, and swapchain lifecycle passed.\n";
}
