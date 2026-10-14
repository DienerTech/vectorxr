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
    // A runtime's new images hold whatever was there: every row the OSD does
    // not draw must still read 0.
    const std::vector<std::uint32_t> old(static_cast<std::size_t>(info->width)*info->height,0xffff00ffu);
    const D3D11_SUBRESOURCE_DATA initial{old.data(),info->width*4,0};
    for(int i=0;i<3;++i) { ID3D11Texture2D* texture{}; Check(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&texture)),"WARP texture failed"); textures.push_back(texture); }
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
bool fail_local{};
int local_creates{};
XrResult XRAPI_PTR Space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    Check(info->poseInReferenceSpace.orientation.w==1,"OSD spaces must have an identity pose");
    if (info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL) {
        ++local_creates; if (fail_local) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
        ++spaces; *space=reinterpret_cast<XrSpace>(4); return XR_SUCCESS;
    }
    Check(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW,"OSD must use a VIEW space");
    ++spaces; *space=reinterpret_cast<XrSpace>(3); return XR_SUCCESS;
}
// The head, 90 degrees to the left (about +Y), at (1, 2, 3).
XrPosef head_pose{{0,0.70710678f,0,0.70710678f},{1,2,3}};
bool head_valid{true};
int locates{};
XrTime located_at{};
XrResult XRAPI_PTR Locate(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    Check(space==reinterpret_cast<XrSpace>(3) && base==reinterpret_cast<XrSpace>(4),"The head was not located in the anchor space");
    ++locates; located_at=time;
    location->locationFlags=head_valid?XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT:0;
    location->pose=head_pose; return XR_SUCCESS;
}
XrResult XRAPI_PTR DestroySpace(XrSpace) { --spaces; return XR_SUCCESS; }
const XrCompositionLayerBaseHeader* WaitVisible(OsdRenderer& renderer, const XrFrameEndInfo& frame) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do { if (const auto* layer=renderer.Append(frame,true)) return layer; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while(std::chrono::steady_clock::now()<deadline);
    return nullptr;
}
// Ends frames until `done`, for up to 3 seconds; returns whether it happened.
template<class Done> bool PumpUntil(OsdRenderer& renderer, const XrFrameEndInfo& frame, Done done) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!done()) {
        if (std::chrono::steady_clock::now()>=deadline) return false;
        renderer.Append(frame,true); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
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
// RasterizeOsd's per-pixel loop from before FeatherOsdPixels wrote the panel's
// interior directly, with only its inputs renamed: the faster loop must match
// it exactly.
std::vector<std::uint32_t> ReferenceFeather(const std::vector<std::uint32_t>& bits,int panel_width,int height,int opacity,bool hidden,bool rgba) {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(OsdBitmap::width)*height);
    static const auto lut=[] {
    std::array<std::array<unsigned char,256>,256> table{};
    for (int a=0;a<256;++a) for (int c=0;c<256;++c) {
        const double s=c/255.0, linear=s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);
        const double p=linear*a/255.0, encoded=p<=.0031308?12.92*p:1.055*std::pow(p,1/2.4)-.055;
        table[a][c]=static_cast<unsigned char>(std::clamp(std::lround(encoded*255),0l,255l));
    }
    return table;
    }();
    for(int y=0;y<height;++y) for(int x=0;x<OsdBitmap::width;++x) {
        constexpr int padding=3;
        const double dx=std::max({18.0+padding-x,0.0,x-(panel_width-19.0-padding)});
        const double dy=std::max({18.0+padding-y,0.0,y-(height-19.0-padding)});
        const double edge=std::min({x-padding+.0,panel_width-1.0-padding-x,y-padding+.0,height-1.0-padding-y,
            (dx>0 && dy>0)?18.0-std::hypot(dx,dy):18.0});
        const double ramp=std::clamp(edge/6.0,0.0,1.0);
        const double coverage=ramp*ramp*(3-2*ramp);
        const auto alpha=static_cast<unsigned>(std::lround(255*opacity/100.0*coverage*(hidden?0:1)));
        const double interior=std::clamp((edge-6)/6.0,0.0,1.0);
        const auto source=bits[y*OsdBitmap::width+x];
        const auto channel=[&](unsigned shift){return static_cast<unsigned>(std::lround(16+(((source>>shift)&255)-16.0)*interior));};
        const auto pixel=interior==1.0?source:(channel(16)<<16)|(channel(8)<<8)|channel(0);
        const unsigned b=lut[alpha][pixel&255],g=lut[alpha][(pixel>>8)&255],r=lut[alpha][(pixel>>16)&255];
        pixels[y*OsdBitmap::width+x]=(alpha<<24)|(g<<8)|(rgba?(b<<16)|r:(r<<16)|b);
    }
    return pixels;
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
        {
            // Frame Generation's hint gets a second line under its row.
            auto framegen=Snapshot(); framegen.framegen_session=true;
            framegen.framegen="On  /  Extrapolate  /  Auto 4x";
            const auto one_line=RasterizeOsd(settings,false,framegen,stats,false);
            framegen.framegen_detail="23 \xE2\x86\x92 90 fps  /  GPU 8.0 ms";
            const auto two_lines=RasterizeOsd(settings,false,framegen,stats,false);
            Check(two_lines.Valid() && two_lines.height==one_line.height+28,"Frame Generation's second line did not add its height");

            // Its badge sits beside the FPS value, inside the FPS column, without
            // changing the panel's size or anything else on it.
            auto badged=framegen; badged.framegen_badge="ON"; badged.framegen_fps=90;
            const auto with_badge=RasterizeOsd(settings,false,badged,stats,false);
            Check(with_badge.Valid() && with_badge.height==two_lines.height,"Frame Generation's FPS badge changed the panel height");
            bool in_column=false, elsewhere=false;
            for(int y=0;y<with_badge.height;++y) for(int x=0;x<960;++x)
                if(with_badge.pixels[y*960+x]!=two_lines.pixels[y*960+x]) (x>=60 && x<315 && y>=57 && y<140?in_column:elsewhere)=true;
            Check(in_column && !elsewhere,"Frame Generation's FPS badge was not drawn beside the FPS value alone");
            for(const char* state:{"STBY","WAIT","PAUSED","OFF","N/A"}) {
                auto other=badged; other.framegen_badge=state; other.framegen_fps=0;
                const auto raster=RasterizeOsd(settings,false,other,stats,false);
                Check(raster.pixels!=with_badge.pixels && raster.pixels!=two_lines.pixels,"Frame Generation's FPS badge did not show its state");
            }
            settings.show_framegen_fps=false;
            Check(RasterizeOsd(settings,false,badged,stats,false).pixels==two_lines.pixels,"The FPS badge setting did not hide the badge");
            settings.show_framegen_fps=true;
            auto other_session=badged; other_session.framegen_session=false;
            auto plain_session=framegen; plain_session.framegen_session=false;
            Check(RasterizeOsd(settings,false,other_session,stats,false).pixels==RasterizeOsd(settings,false,plain_session,stats,false).pixels,
                "A session without Frame Generation showed its FPS badge");
            if(const char* path=std::getenv("OSD_FRAMEGEN_BMP")) WriteBitmap(path,with_badge);

            // Compact: the FPS column widens for the badge, only in sessions
            // with Frame Generation and only while FPS is shown.
            auto compact_settings=settings;
            compact_settings.compact_show_brand=compact_settings.compact_show_app=compact_settings.compact_show_runtime=false;
            constexpr OsdCompactLayoutOptions framegen_options{.framegen_session=true};
            const auto plain_layout=CompactOsdLayout(compact_settings), badge_layout=CompactOsdLayout(compact_settings,framegen_options);
            Check(!plain_layout.framegen_badge && badge_layout.framegen_badge && badge_layout.width==plain_layout.width+50 &&
                  badge_layout.frame_time_x==plain_layout.frame_time_x+50 && badge_layout.height==plain_layout.height,
                "The compact FPS column did not make room for Frame Generation's badge");
            const auto compact_badged=RasterizeOsd(compact_settings,true,badged,stats,false);
            Check(compact_badged.Valid() && compact_badged.content_width==badge_layout.width,"Compact badge raster has the wrong width");
            if(const char* path=std::getenv("OSD_FRAMEGEN_COMPACT_BMP")) WriteBitmap(path,compact_badged);
            compact_settings.compact_metrics="frameTime";
            Check(!CompactOsdLayout(compact_settings,framegen_options).framegen_badge,"Compact badge shown without an FPS value");
            compact_settings.compact_metrics="fps"; compact_settings.show_framegen_fps=false;
            Check(!CompactOsdLayout(compact_settings,framegen_options).framegen_badge &&
                  CompactOsdLayout(compact_settings,framegen_options).width==CompactOsdLayout(compact_settings).width,"Compact badge shown while turned off");
        }
        {
            // Diagnostic modes left on name themselves in a tag after the
            // brand: only the header row changes, and it stays with every
            // header item hidden.
            auto tagged=Snapshot(); tagged.diagnostics="DEBUG LOG \xC2\xB7 TINT VIEW";
            const auto plain=RasterizeOsd(settings,false,Snapshot(),stats,false);
            const auto with_tag=RasterizeOsd(settings,false,tagged,stats,false);
            Check(with_tag.Valid() && with_tag.height==plain.height,"The diagnostics tag changed the panel height");
            bool in_header=false, below=false, attention=false;
            for(int y=0;y<plain.height;++y) for(int x=0;x<960;++x) {
                if(with_tag.pixels[y*960+x]!=plain.pixels[y*960+x]) (y<52?in_header:below)=true;
                attention|=y<52 && (with_tag.pixels[y*960+x]&0xffffff)==0xf5b96c;
            }
            Check(in_header && !below && attention,"The diagnostics tag was not drawn in the header row alone");
            auto hidden=settings; hidden.show_brand=hidden.show_app=hidden.show_runtime=hidden.show_clock=false;
            Check(RasterizeOsd(hidden,false,Snapshot(),stats,false).height==plain.height-40 &&
                  RasterizeOsd(hidden,false,tagged,stats,false).height==plain.height,
                  "Hiding every header item hid the diagnostics tag");
            if(const char* path=std::getenv("OSD_DIAGNOSTICS_BMP")) WriteBitmap(path,with_tag);

            // Compact: the tag takes a slot after the brand, and the fields
            // after it move right by its width.
            auto compact_settings=settings; compact_settings.compact_show_brand=compact_settings.compact_show_app=true;
            constexpr OsdCompactLayoutOptions tag_options{.diagnostics_width=200};
            const auto untagged_layout=CompactOsdLayout(compact_settings), tagged_layout=CompactOsdLayout(compact_settings,tag_options);
            Check(untagged_layout.diagnostics_x<0 && tagged_layout.diagnostics_x==28+155+16 && tagged_layout.diagnostics_width==200 &&
                  tagged_layout.header_x[1]==untagged_layout.header_x[1]+216 && tagged_layout.height==untagged_layout.height,
                  "The compact diagnostics tag did not take its slot after the brand");
            compact_settings.compact_show_brand=compact_settings.compact_show_app=compact_settings.compact_show_runtime=
                compact_settings.compact_show_clock=false;
            const auto no_header=CompactOsdLayout(compact_settings), tag_only=CompactOsdLayout(compact_settings,tag_options);
            Check(no_header.header_y<0 && tag_only.header_y>=0 && tag_only.diagnostics_x==28 && tag_only.height==no_header.height+44,
                  "The compact diagnostics tag did not keep the header row");
            compact_settings.compact_show_brand=compact_settings.compact_show_app=true;
            const auto compact_tagged=RasterizeOsd(compact_settings,true,tagged,stats,false);
            Check(compact_tagged.Valid() &&
                  compact_tagged.content_width>RasterizeOsd(compact_settings,true,Snapshot(),stats,false).content_width,
                  "The compact panel did not widen for the diagnostics tag");
            if(const char* path=std::getenv("OSD_DIAGNOSTICS_COMPACT_BMP")) WriteBitmap(path,compact_tagged);
            // With every field shown, the longest tag narrows the runtime's
            // column (and the app's) instead of passing the panel's edge.
            compact_settings.compact_show_runtime=compact_settings.compact_show_clock=true;
            constexpr OsdCompactLayoutOptions long_tag{.diagnostics_width=300};
            const auto full=CompactOsdLayout(compact_settings), crowded=CompactOsdLayout(compact_settings,long_tag);
            Check(full.width<=OsdBitmap::width && crowded.width<=OsdBitmap::width && crowded.header_width[2]<full.header_width[2] &&
                  crowded.header_width[2]>=80 && crowded.header_width[1]>=80 && crowded.header_width[0]==155 && crowded.header_width[3]==142 &&
                  crowded.diagnostics_width==300,
                  "The compact diagnostics tag pushed the header past the panel");
            auto long_snapshot=tagged; long_snapshot.diagnostics="DEBUG LOG \xC2\xB7 CONFIDENCE VIEW";
            const auto crowded_raster=RasterizeOsd(compact_settings,true,long_snapshot,stats,false);
            Check(crowded_raster.Valid() && crowded_raster.content_width<=OsdBitmap::width,"The crowded compact header did not fit the panel");
            if(const char* path=std::getenv("OSD_DIAGNOSTICS_CROWDED_BMP")) WriteBitmap(path,crowded_raster);
        }
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
        {
            // Random source colors, panel widths and heights either side of the
            // interior band's limits, both channel orders and an empty panel.
            std::vector<std::uint32_t> bits(static_cast<std::size_t>(OsdBitmap::width)*OsdBitmap::texture_height);
            std::uint32_t seed=0x2545f491;
            for(auto& pixel:bits) { seed=seed*1664525+1013904223; pixel=seed; }
            for(int panel_width:{30,31,266,520,960}) for(int height:{42,43,136,602})
            for(int opacity:{1,25,90,100}) for(bool hidden:{false,true}) for(bool use_rgba:{false,true}) {
                if(hidden && opacity!=90) continue;
                std::vector<std::uint32_t> pixels(static_cast<std::size_t>(OsdBitmap::width)*height,0xdeadbeef);
                FeatherOsdPixels(bits.data(),pixels.data(),panel_width,height,opacity,hidden,use_rgba);
                Check(pixels==ReferenceFeather(bits,panel_width,height,opacity,hidden,use_rgba),
                      "The OSD's interior fast path changed feathered or premultiplied pixels");
            }
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
        for (bool turbo:{false,true}) for (bool pivot:{false,true}) for (bool snap_line:{false,true}) {
            settings.compact_show_turbo=turbo; settings.compact_show_pivot=pivot;
            auto states=Snapshot(); states.turbo="Recovery blocked / Analyzing"; states.pivot=std::string(150,'W'); states.compact_pivot="Motion: Cont Active + Position";
            if(snap_line) states.compact_pivot_secondary="Snap: Active / Stacked + Nudge";
            const auto compact=RasterizeOsd(settings,true,states,stats,false);
            Check(compact.height==136+48*static_cast<int>(turbo)+(pivot?(snap_line?80:48):0),
                  "Compact Pivot must use one row without Snap Views and two with them");
            for(int y=18;y<100;++y) for(int x=28;x<600;++x)
                Check(plain.pixels[y*960+x]==compact.pixels[y*960+x],"Compact states changed the performance header");
            Check(RasterizeOsd(settings,false,Snapshot(),stats,false).pixels==expanded.pixels,"Compact options changed expanded output");
            for (int y=0;y<compact.height;++y) for (int x=0;x<960;++x)
                if (x<3 || x>=compact.content_width-3 || y<3 || y>=compact.height-3) Check(compact.pixels[y*960+x]==0,"Compact footer spilled outside the panel");
            if(argc>2 && turbo && pivot && snap_line) WriteBitmap(argv[2],compact);
        }
        {
            auto single=Snapshot(); single.pivot="Motion: Active / Continuous / Flight";
            auto stacked=single; stacked.pivot_secondary="Snap: Active / Stacked / Check Six / Views";
            settings.show_pivot=true;
            Check(RasterizeOsd(settings,false,stacked,stats,false).height==RasterizeOsd(settings,false,single,stats,false).height+32,
                  "Expanded Pivot must add its snap line only when Snap Views are configured");
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
        // The tallest compact panel shows the header (18 + 44), metrics (68), the
        // Turbo and Frame Gen rows (48 each), and Pivot with its snap line (80),
        // with 6 below. It must fit the OSD texture.
        constexpr int kWorstCompactHeight=18+44+68+48+48+80+6;
        static_assert(kWorstCompactHeight<=OsdBitmap::texture_height);
        int tallest_compact=0;
        for(const auto* metrics:{"all","fps","frameTime","none"}) for(int flags=0;flags<256;++flags) {
            auto options=OsdSettings{};options.compact_metrics=metrics;
            options.compact_show_brand=flags&1;options.compact_show_app=flags&2;
            options.compact_show_runtime=flags&4;options.compact_show_clock=flags&8;
            options.compact_show_turbo=flags&16;options.compact_show_pivot=flags&32;
            options.compact_show_framegen=flags&128;
            const OsdCompactLayoutOptions layout_options{.framegen_session=(flags&64)!=0};
            auto snap_options=layout_options; snap_options.pivot_snap_line=true;
            const auto layout=CompactOsdLayout(options,layout_options);
            const auto snap_layout=CompactOsdLayout(options,snap_options);
            tallest_compact=std::max(tallest_compact,snap_layout.height);
            Check(layout.width>=240 && layout.width<=960 && layout.height>=68 && snap_layout.height<=kWorstCompactHeight,"Compact geometry escaped texture bounds");
            Check(snap_layout.height==layout.height+(options.compact_show_pivot?32:0) && snap_layout.width==layout.width,
                "Only a shown Pivot row may grow for the snap line");
            if(options.compact_show_turbo && options.compact_show_pivot) {
                options.compact_show_pivot=false;
                const auto turbo_only=CompactOsdLayout(options,layout_options);
                Check(layout.pivot_y==layout.turbo_y+(options.compact_show_framegen?96:48) && layout.width==turbo_only.width &&
                      layout.height==turbo_only.height+48,
                    "Adding compact Pivot widened the panel instead of adding a row");
            }
            Check(layout.empty==((flags&(63|128))==0 && options.compact_metrics=="none"),"Empty compact detection failed");
        }
        Check(tallest_compact==kWorstCompactHeight,"The compact worst case changed; re-derive it against the OSD texture");
        {
            // Everything that grows the panel at once, as a Frame Generation
            // session of an app with Snap Views draws it: rasterized, within
            // the texture, and nothing past the panel's edges.
            OsdSettings all{};
            all.show_brand=all.show_app=all.show_runtime=all.show_clock=true;
            all.show_graph=all.show_turbo=all.show_framegen=all.show_pivot=all.show_modules=true;
            all.compact_show_brand=all.compact_show_app=all.compact_show_runtime=all.compact_show_clock=true;
            all.compact_metrics="all";
            all.compact_show_turbo=all.compact_show_framegen=all.compact_show_pivot=true;
            auto busy=Snapshot();
            busy.framegen_session=true; busy.framegen_badge="ON"; busy.framegen_fps=90;
            busy.framegen="On  /  Interpolate  /  Dynamic"; busy.framegen_detail="45 -> 90 fps  /  GPU 3.1 ms";
            busy.compact_framegen="Dynamic  /  45 -> 90 fps";
            busy.pivot="Motion: Active / Continuous + Position / Flight"; busy.compact_pivot="Motion: Cont Active + Position";
            busy.pivot_secondary="Snap: Active / Stacked + Nudge / Check Six / Views";
            busy.compact_pivot_secondary="Snap: Active / Stacked + Nudge";
            const auto expanded=RasterizeOsd(all,false,busy,stats,false);
            const auto compact=RasterizeOsd(all,true,busy,stats,false);
            // Header and metrics 210, graph 156, Turbo 52, Frame Gen with its
            // detail 80, Pivot with its snap line 84, enhancements 52, restart 48.
            Check(expanded.Valid() && expanded.height==210+156+52+80+84+52+48 && expanded.height<=OsdBitmap::texture_height,
                  "The expanded worst case escaped the OSD texture");
            Check(compact.Valid() && compact.height==kWorstCompactHeight,"The compact worst case rasterized at another height");
            for(const OsdBitmap* raster:{&expanded,&compact})
                for(int y=0;y<raster->height;++y) for(int x=0;x<OsdBitmap::width;++x)
                    if(x<3 || y<3 || x>=raster->content_width-3 || y>=raster->height-3)
                        Check(raster->pixels[y*OsdBitmap::width+x]==0,"The worst-case OSD drew past its panel");
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
        Check(!renderer.SnapshotDue(),"Status snapshot was requested before the refresh deadline");
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
        Check(renderer.SnapshotDue(),"A settings change must refresh the status snapshot");
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
    {
        // A world-anchored panel goes out in LOCAL, at the head pose located
        // at each frame's own display time; head-locked otherwise.
        OsdDispatch anchored=dispatch; anchored.locate=Locate;
        OsdRenderer renderer; unsupported=false; renderer.Initialize(session,device,4,anchored);
        auto settings=OsdSettings{}; settings.enabled=true; settings.show_clock=false; settings.horizontal_degrees=20; settings.vertical_degrees=0;
        renderer.Prepare(settings,Snapshot(),false,false);
        XrCompositionLayerProjection game{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto* original=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO}; frame.layerCount=1; frame.layers=&original; frame.displayTime=1000;
        auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(WaitVisible(renderer,frame));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(3) && locates==0 && local_creates==0,"A head-locked panel located the head");
        const XrPosef panel=quad->pose;
        renderer.SetWorldAnchored(true);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(4) && locates==1 && located_at==1000,"The anchored panel was not placed at its frame's head pose");
        // Turned 90 degrees left about +Y: x' = z, z' = -x.
        Check(std::abs(quad->pose.position.x-(1+panel.position.z))<1e-5f && std::abs(quad->pose.position.y-(2+panel.position.y))<1e-5f &&
              std::abs(quad->pose.position.z-(3-panel.position.x))<1e-5f,"The panel's position did not follow the head");
        // The panel sits 20 degrees right, level (a -20 degree yaw); the head turned
        // 90 degrees left: 70 degrees left of LOCAL's forward in all.
        const double half=35.0*3.14159265358979323846/180;
        const XrQuaternionf& o=quad->pose.orientation;
        Check(std::abs(o.x)<1e-5f && std::abs(o.z)<1e-5f && std::abs(std::abs(o.y)-static_cast<float>(std::sin(half)))<1e-5f &&
              std::abs(std::abs(o.w)-static_cast<float>(std::cos(half)))<1e-5f && o.y*o.w>0,
              "The panel's orientation did not follow the head");
        // Every frame is located at its own display time: none reuses another's.
        frame.displayTime=2000;
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && locates==2 && located_at==2000 && local_creates==1,"A frame reused another frame's head pose");
        // Without a located head the panel stays head-locked for that frame.
        head_valid=false;
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(3) && std::abs(quad->pose.position.x-panel.position.x)<1e-6f,
              "An unlocated head moved the panel");
        head_valid=true;
        renderer.SetWorldAnchored(false);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(3) && locates==3,"Turning anchoring off kept the panel in the world");
        renderer.Shutdown(); Check(spaces==0,"The anchor space leaked on teardown");
        // A runtime without a LOCAL space keeps the panel head-locked, and is
        // asked once.
        renderer.Initialize(session,device,4,anchored); renderer.SetWorldAnchored(true); fail_local=true; local_creates=0;
        renderer.Prepare(settings,Snapshot(),false,false);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(WaitVisible(renderer,frame));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(3),"A missing LOCAL space hid the panel");
        renderer.Append(frame,true);
        Check(local_creates==1,"A missing LOCAL space was asked for every frame");
        // A new session starts head-locked until it is set again.
        renderer.Shutdown(); fail_local=false; local_creates=0; renderer.Initialize(session,device,4,anchored);
        renderer.Prepare(settings,Snapshot(),false,false);
        quad=reinterpret_cast<const XrCompositionLayerQuad*>(WaitVisible(renderer,frame));
        Check(quad && quad->space==reinterpret_cast<XrSpace>(3) && local_creates==0,"A new session kept the last session's anchoring");
        renderer.Shutdown(); Check(spaces==0,"Spaces leaked on teardown");
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
    {
        // A compact panel showing only the app's name: its raster depends on
        // the settings and snapshot alone, not the frame times.
        OsdRenderer renderer; unsupported=false; renderer.Initialize(session,device,4,dispatch);
        auto settings=OsdSettings{}; settings.enabled=true; settings.compact=true; settings.compact_metrics="none";
        settings.compact_show_brand=settings.compact_show_runtime=settings.compact_show_clock=false; settings.update_hz=20;
        XrCompositionLayerProjection game{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const auto* original=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game);
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO}; frame.layerCount=1; frame.layers=&original;
        renderer.Prepare(settings,Snapshot(),false,false);
        Check(WaitVisible(renderer,frame),"The app-name panel was not shown");

        // The refresh interval runs from the request: an upload that waits on
        // its image past the 50 ms interval leaves the next refresh due.
        settings.opacity=80; renderer.Prepare(settings,Snapshot(),false,false);
        timeouts=1000; const int acquired=acquires, released=releases;
        const auto requested=std::chrono::steady_clock::now();
        renderer.Append(frame,true);
        Check(PumpUntil(renderer,frame,[&]{return acquires>acquired && std::chrono::steady_clock::now()-requested>=std::chrono::milliseconds(60);}),
              "The refresh was not acquired");
        timeouts=0;
        Check(renderer.Append(frame,true) && releases==released+1,"The delayed refresh was not uploaded");
        Check(renderer.SnapshotDue(),"The OSD's refresh interval ran from its upload instead of its request");

        // A hidden panel takes no snapshots, however long it stays hidden.
        renderer.Prepare(settings,Snapshot(),true,false);
        Check(!renderer.Append(frame,true),"The toggle did not hide the panel");
        renderer.Prepare(settings,std::nullopt,false,false);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Check(!renderer.SnapshotDue(),"A hidden OSD asked for status snapshots");

        // Shown again, it draws the next snapshot, not the one it held.
        renderer.Prepare(settings,std::nullopt,true,false);
        Check(renderer.SnapshotDue(),"A shown OSD did not ask for a status snapshot");
        const int held=acquires;
        const auto waited=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
        while(std::chrono::steady_clock::now()<waited) { renderer.Append(frame,true); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Check(acquires==held,"A shown OSD drew the snapshot it held while hidden");
        auto shown=Snapshot(); shown.application="Shown.exe";
        renderer.Prepare(settings,shown,false,false);
        const int before_shown=releases;
        Check(PumpUntil(renderer,frame,[&]{return releases>before_shown;}),"The shown panel did not refresh");
        CheckUploadedRaster(RasterizeOsd(settings,true,shown,OsdTelemetry{},false),DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

        // A raster finished before a change is dropped, not uploaded after it.
        settings.opacity=70; renderer.Prepare(settings,shown,false,false);
        renderer.Append(frame,true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto changed=shown; changed.application="Changed.exe"; settings.opacity=60;
        renderer.Prepare(settings,changed,false,false);
        const int before_change=releases;
        Check(PumpUntil(renderer,frame,[&]{return releases>before_change;}),"The changed panel did not refresh");
        CheckUploadedRaster(RasterizeOsd(settings,true,changed,OsdTelemetry{},false),DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

        // Moving or resizing the panel applies to the image already up.
        settings.update_hz=1; renderer.Prepare(settings,changed,false,false);
        const int before_slow=releases;
        Check(PumpUntil(renderer,frame,[&]{return releases>before_slow;}),"The 1 Hz panel did not refresh");
        const int placed=acquires;
        settings.horizontal_degrees=-20; settings.scale=50; renderer.Prepare(settings,changed,false,false);
        const auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(renderer.Append(frame,true));
        Check(quad && quad->pose.position.x<0,"Moving the panel did not apply at once");
        const auto settled=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
        while(std::chrono::steady_clock::now()<settled) { renderer.Append(frame,true); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Check(acquires==placed,"Moving or resizing the panel rasterized it again");

        // A raster that fails (here GDI's per-process handle quota is spent)
        // disables the panel until it is turned off and on, which then
        // rasterizes again.
        std::vector<HBRUSH> brushes;
        while(brushes.size()<100000) { const HBRUSH brush=CreateSolidBrush(RGB(1,2,3)); if(!brush) break; brushes.push_back(brush); }
        if(brushes.size()<100000) {
            settings.opacity=50; renderer.Prepare(settings,changed,false,false);
            const bool failed=PumpUntil(renderer,frame,[&]{return renderer.Status().message.find("rasterization")!=std::string::npos;});
            for(auto brush:brushes) DeleteObject(brush);
            Check(failed,"A failed OSD raster was not reported");
            settings.enabled=false; renderer.Prepare(settings,changed,false,false);
            settings.enabled=true; renderer.Prepare(settings,changed,false,false);
            Check(WaitVisible(renderer,frame),"Turning the OSD off and on did not recover from a failed raster");
        } else {
            for(auto brush:brushes) DeleteObject(brush);
            std::cout<<"GDI handle quota not reached; failed-raster retry not tested\n";
        }

        // The diagnostics tag keeps the header row, and so the panel, in a
        // compact layout with nothing else selected.
        settings.compact_show_app=false;
        auto tagged=changed; tagged.diagnostics="DEBUG LOG";
        Check(RasterizeOsd(settings,true,changed,OsdTelemetry{},false).empty && !RasterizeOsd(settings,true,tagged,OsdTelemetry{},false).empty,
              "An empty compact raster was not marked, or the diagnostics tag's was");
        renderer.Prepare(settings,tagged,false,false);
        Check(WaitVisible(renderer,frame),"The diagnostics tag was hidden in an otherwise empty compact panel");
        renderer.Prepare(settings,changed,false,false);
        Check(!renderer.Append(frame,true),"An empty compact panel was submitted once its diagnostics tag went");
        renderer.Shutdown();
    }
    {
        // Uploads copy the panel's rows and clear only what each image's last,
        // taller panel left: across images and heights, every image matches
        // its bitmap and is 0 below it.
        Check(textures.empty(),"An earlier test left its swapchain");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=OsdBitmap::width; desc.Height=OsdBitmap::texture_height; desc.MipLevels=1; desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        const std::vector<std::uint32_t> old(static_cast<std::size_t>(OsdBitmap::width)*OsdBitmap::texture_height,0xffff00ffu);
        const D3D11_SUBRESOURCE_DATA initial{old.data(),OsdBitmap::width*4,0};
        for(int i=0;i<2;++i) { ID3D11Texture2D* texture{}; Check(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&texture)),"WARP texture failed"); textures.push_back(texture); }
        auto graphics=CreateOsdD3D11(device); std::string error;
        Check(graphics && graphics->Images(reinterpret_cast<XrSwapchain>(2),dispatch,error),"D3D11 OSD images were not enumerated");
        const auto bitmap=[](int height,std::uint32_t seed) {
            OsdBitmap b; b.height=height; b.pixels.resize(static_cast<std::size_t>(OsdBitmap::width)*height);
            for(std::size_t i=0;i<b.pixels.size();++i) b.pixels[i]=0xff000000u|static_cast<std::uint32_t>(i*seed);
            return b;
        };
        const auto check=[&](int image,const OsdBitmap& b) {
            D3D11_TEXTURE2D_DESC staging=desc; staging.Usage=D3D11_USAGE_STAGING; staging.BindFlags=0; staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* readback{};
            Check(SUCCEEDED(device->CreateTexture2D(&staging,nullptr,&readback)),"D3D11 readback allocation failed");
            context->CopyResource(readback,textures[image]); D3D11_MAPPED_SUBRESOURCE mapped{};
            Check(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped)),"D3D11 readback map failed");
            for(int y=0;y<OsdBitmap::texture_height;++y) for(int x=0;x<OsdBitmap::width;++x) {
                const auto pixel=reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch)[x];
                Check(pixel==(y<b.height?b.pixels[y*OsdBitmap::width+x]:0u),"An OSD image kept old rows or lost the panel's");
            }
            context->Unmap(readback,0); readback->Release();
        };
        const std::array<std::pair<int,OsdBitmap>,5> uploads{{{0,bitmap(500,3)},{1,bitmap(100,5)},{0,bitmap(100,7)},{1,bitmap(300,11)},{0,bitmap(766,13)}}};
        for(const auto& [image,b]:uploads) {
            Check(graphics->Upload(static_cast<std::uint32_t>(image),b,error)==OsdUpload::Complete,"D3D11 OSD upload failed");
            check(image,b);
        }
        graphics->Reset();
        for(auto* texture:textures) texture->Release();
        textures.clear();
    }
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
    {
        // effectiveRefreshHz counts refreshes on the clock, not against the
        // app's frames: here no app frame is recorded at all, as when the app
        // stalls while Frame Generation's presenter keeps ending frames.
        std::filesystem::path log;
        {
            Logger logger; logger.Initialize((std::filesystem::temp_directory_path()/"vectorxr-osd-tests")/"vectorxr-osd-rate.log");
            logger.SetLevel(LogLevel::Info); log=logger.ActiveLogPath();
            OsdRenderer renderer; XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device=device;
            renderer.InitializeGraphics(session,&binding,4,dispatch,&logger,"rate-session");
            auto settings=OsdSettings{}; settings.enabled=true; settings.show_clock=false; settings.update_hz=20;
            renderer.Prepare(settings,Snapshot(),false,false);
            const XrCompositionLayerProjection game{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const auto* original=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&game);
            XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO}; frame.layerCount=1; frame.layers=&original;
            Check(WaitVisible(renderer,frame),"The rate test's panel was not shown");
            const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(700);
            while(std::chrono::steady_clock::now()<until) { renderer.Append(frame,true); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            renderer.ReportDiagnostics(true);
        }
        std::ifstream file(log); std::string text((std::istreambuf_iterator<char>(file)),{});
        const auto line=text.find("rate-session api=D3D11 mode=visible");
        const auto rate=text.find("effectiveRefreshHz=",line);
        Check(line!=std::string::npos && rate!=std::string::npos,"The OSD's visible summary is missing");
        const double hz=std::atof(text.c_str()+rate+19);
        Check(hz>8 && hz<=25,"effectiveRefreshHz did not measure the 20 Hz refreshes on the clock");
    }
    { OsdMetric metric; for(int i=0;i<100;++i)metric.Add(i<95?.11:20);Check(metric.count==100&&std::abs(metric.P95Upper()-.15)<.001&&metric.maximum==20,"OSD bounded metric histogram incorrect"); }
    context->Release(); device->Release();
    std::cout<<"OSD telemetry, rasterization, bindings, live settings, and swapchain lifecycle passed.\n";
}
