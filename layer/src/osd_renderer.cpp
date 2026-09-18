#include <windows.h>
#include <d3d11.h>
#include "depthxr/osd_renderer.h"
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace depthxr {
using Clock = std::chrono::steady_clock;

void OsdTelemetry::Tick(Clock::time_point now) {
    if (last) {
        const double ms = std::chrono::duration<double, std::milli>(now - *last).count();
        // A pause is not an application frame. Restart the window on resume.
        if (ms > 1000 || ms <= 0) samples.clear();
        else { samples.push_back(ms); if (samples.size() > 120) samples.pop_front(); }
    }
    last = now;
}
double OsdTelemetry::Mean() const {
    return samples.empty() ? 0 : std::accumulate(samples.begin(), samples.end(), 0.0) / samples.size();
}
double OsdTelemetry::Percentile95() const {
    if (samples.empty()) return 0;
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted[static_cast<std::size_t>(std::ceil(sorted.size() * .95)) - 1];
}

namespace {
bool SameInput(const InputBinding& a, const InputBinding& b) {
    return a.type==b.type && a.chord==b.chord && a.device_guid==b.device_guid && a.input_path==b.input_path && a.product_guid==b.product_guid;
}
std::wstring Wide(const std::string& s) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(size, L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size);
    return result;
}
std::string Number(double value, int precision = 1) {
    if (value <= 0) return "--";
    std::ostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str();
}
// GDI owns only this private memory DC; never touches the application's pipeline.
struct Canvas {
    HDC dc{CreateCompatibleDC(nullptr)};
    HBITMAP bitmap{};
    HGDIOBJ old{};
    std::uint32_t* bits{};
    Canvas(int w, int h) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (bitmap && dc) { old = SelectObject(dc, bitmap); SetBkMode(dc, TRANSPARENT); }
    }
    ~Canvas() { if (old) SelectObject(dc, old); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); }
    void Rect(int x, int y, int w, int h, COLORREF color) {
        const auto brush = CreateSolidBrush(color); RECT r{x,y,x+w,y+h}; FillRect(dc,&r,brush); DeleteObject(brush);
    }
    void Text(int x, int y, int width, int size, const std::string& text, COLORREF color, bool bold=false) {
        const auto font = CreateFontW(-size,0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto previous = SelectObject(dc,font); SetTextColor(dc,color);
        RECT r{x,y,x+width,y+size+12}; const auto wide=Wide(text);
        DrawTextW(dc,wide.c_str(),static_cast<int>(wide.size()),&r,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,previous); DeleteObject(font);
    }
};
}

OsdBitmap RasterizeOsd(const OsdSettings& settings, bool compact, const OsdSnapshot& snapshot,
                       const OsdTelemetry& telemetry, bool rgba) {
    constexpr auto white=RGB(233,240,248), muted=RGB(148,165,184), bg=RGB(13,20,31), inset=RGB(20,31,45);
    const auto accent=settings.accent=="copper"?RGB(245,174,120):settings.accent=="blue"?RGB(111,172,255):RGB(81,221,189);
    const int height=compact?180:260+(settings.show_graph?156:0)+(settings.show_runtime?64:0)+
        (settings.show_turbo?52:0)+(settings.show_modules?52:0)+(!snapshot.restart.empty()?48:0);
    OsdBitmap output; output.height=height;
    Canvas canvas(output.width,height);
    if (!canvas.bits || !canvas.dc) return output;
    canvas.Rect(0,0,output.width,height,bg);
    canvas.Rect(0,0,6,height,accent);
    canvas.Text(28,18,260,22,"VECTORXR",accent,true);
    canvas.Text(290,18,460,21,snapshot.application,muted);
    if (settings.show_clock) {
        SYSTEMTIME time; GetLocalTime(&time);
        char label[16]; snprintf(label,sizeof(label),"%02u:%02u",time.wHour,time.wMinute);
        canvas.Text(826,18,108,21,label,muted);
    }
    const auto mean=telemetry.Mean();
    canvas.Text(28,57,265,compact?48:68,Number(mean>0?1000/mean:0,0),white,true);
    canvas.Text(325,57,280,compact?48:68,Number(mean)+" ms",white,true);
    canvas.Text(655,57,276,compact?48:68,Number(telemetry.Percentile95())+" ms",accent,true);
    const int label_y=compact?121:141;
    canvas.Text(30,label_y,270,20,"APP FPS",muted);
    canvas.Text(327,label_y,280,20,"APP FRAME / AVG",muted);
    canvas.Text(657,label_y,275,20,"APP FRAME / P95",muted);
    if (!compact) {
        int y=190;
        if (settings.show_graph) {
            canvas.Rect(28,y,904,130,inset);
            double ceiling=22.2;
            for (auto v:telemetry.samples) ceiling=std::max(ceiling,v*1.1);
            canvas.Text(40,y+4,380,18,"FRAME TIME  /  LAST 120 FRAMES",muted);
            canvas.Text(780,y+4,142,18,Number(ceiling)+" ms",muted);
            const auto pen=CreatePen(PS_SOLID,3,accent); auto old=SelectObject(canvas.dc,pen);
            std::size_t index=120-telemetry.samples.size(); bool first=true;
            for (auto v:telemetry.samples) {
                const int x=40+static_cast<int>(index++*880/119);
                const int py=y+118-static_cast<int>(std::min(1.0,v/ceiling)*80);
                if (first) { MoveToEx(canvas.dc,x,py,nullptr); first=false; } else LineTo(canvas.dc,x,py);
            }
            SelectObject(canvas.dc,old); DeleteObject(pen); y+=156;
        }
        if (settings.show_runtime) {
            canvas.Text(30,y,210,20,"RUNTIME",muted);
            canvas.Text(242,y,680,24,snapshot.runtime,white,true); y+=64;
        }
        if (settings.show_turbo) {
            canvas.Text(30,y,210,20,"TURBO",muted);
            canvas.Text(242,y,680,24,snapshot.turbo+(snapshot.experimental?"  /  EXPERIMENTAL":""),accent,true); y+=52;
        }
        if (settings.show_modules) {
            canvas.Text(30,y,210,20,"ENHANCEMENTS",muted);
            canvas.Text(242,y,680,24,snapshot.modules,white,true); y+=52;
        }
        if (!snapshot.restart.empty()) {
            canvas.Text(30,y,902,20,"RESTART PENDING  /  "+snapshot.restart,RGB(245,185,108)); y+=48;
        }
        canvas.Text(30,y+8,902,18,"Application cadence. Not GPU or compositor FPS.",muted);
    }
    output.pixels.resize(static_cast<std::size_t>(output.width)*height);
    // OpenXR premultiplies alpha in linear color space. Encode linear*alpha back
    // to sRGB; multiplying encoded RGB directly creates dark translucent edges.
    static const auto lut=[] {
    std::array<std::array<unsigned char,256>,256> table{};
    for (int a=0;a<256;++a) for (int c=0;c<256;++c) {
        const double s=c/255.0, linear=s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);
        const double p=linear*a/255.0, encoded=p<=.0031308?12.92*p:1.055*std::pow(p,1/2.4)-.055;
        table[a][c]=static_cast<unsigned char>(std::clamp(std::lround(encoded*255),0l,255l));
    }
    return table;
    }();
    for(int y=0;y<height;++y) for(int x=0;x<output.width;++x) {
        const double dx=std::max({18.0-x,0.0,x-(output.width-19.0)});
        const double dy=std::max({18.0-y,0.0,y-(height-19.0)});
        const double coverage=(dx==0 || dy==0)?1.0:std::clamp(18.5-std::hypot(dx,dy),0.0,1.0);
        const auto alpha=static_cast<unsigned>(std::lround(255*settings.opacity/100.0*coverage));
        const auto pixel=canvas.bits[y*output.width+x];
        const unsigned b=lut[alpha][pixel&255],g=lut[alpha][(pixel>>8)&255],r=lut[alpha][(pixel>>16)&255];
        output.pixels[y*output.width+x]=(alpha<<24)|(g<<8)|(rgba?(b<<16)|r:(r<<16)|b);
    }
    return output;
}

OsdRenderer::~OsdRenderer() { Shutdown(); }
void OsdRenderer::RasterWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        RasterRequest request;
        {
            std::unique_lock lock(raster_mutex_);
            raster_cv_.wait(lock,[&] { return raster_stop_ || raster_request_.has_value(); });
            if (raster_stop_) return;
            request=std::move(*raster_request_); raster_request_.reset();
        }
        OsdBitmap bitmap;
        try { bitmap=RasterizeOsd(request.settings,request.compact,request.snapshot,request.telemetry,request.rgba); }
        catch (...) { /* Report a failed image through the normal OSD status path. */ }
        {
            std::scoped_lock lock(raster_mutex_);
            raster_completed_=std::move(bitmap);
        }
    }
}
void OsdRenderer::ShutdownLocked() {
    {
        std::scoped_lock lock(raster_mutex_); raster_stop_=true;
    }
    raster_cv_.notify_one();
    if (raster_thread_.joinable()) raster_thread_.join();
    raster_stop_=false; raster_requested_=false; raster_request_.reset(); raster_completed_.reset(); upload_.reset();
    if (swapchain_ && api_.destroy) api_.destroy(swapchain_);
    if (space_ && api_.destroy_space) api_.destroy_space(space_);
    swapchain_=XR_NULL_HANDLE; space_=XR_NULL_HANDLE; session_=XR_NULL_HANDLE;
    if (context_) context_->Release(); if (device_) device_->Release(); context_=nullptr; device_=nullptr;
    images_.clear(); acquired_.reset(); ready_=false; failed_=false; configured_=false; primed_=false;
    telemetry_={}; status_={}; updated_={};
}
void OsdRenderer::Shutdown() { std::scoped_lock lock(mutex_); ShutdownLocked(); }
void OsdRenderer::ResetPresentation() {
    std::scoped_lock lock(mutex_);
    configured_=false; primed_=false; telemetry_={}; updated_={};
    status_.visible=false;
}
void OsdRenderer::SubmissionFailed(XrResult result) {
    std::scoped_lock lock(mutex_);
    Fail("OSD frame submission",result);
}
void OsdRenderer::Initialize(XrSession session, ID3D11Device* device, std::uint32_t max_layers, OsdDispatch api) {
    std::scoped_lock lock(mutex_); ShutdownLocked();
    session_=session; device_=device; max_layers_=max_layers; api_=api;
    if (device_) { device_->AddRef(); device_->GetImmediateContext(&context_); }
    status_.available=context_!=nullptr;
    status_.message=context_?"Disabled":"OSD requires a Direct3D 11 application";
}
void OsdRenderer::Prepare(const OsdSettings& settings, OsdSnapshot snapshot, bool toggle, bool cycle) {
    std::scoped_lock lock(mutex_);
    if (!configured_ || (settings.enabled && !settings_.enabled)) {
        if (failed_) {
            if (swapchain_ && api_.destroy) api_.destroy(swapchain_);
            if (space_ && api_.destroy_space) api_.destroy_space(space_);
            swapchain_=XR_NULL_HANDLE; space_=XR_NULL_HANDLE; acquired_.reset(); images_.clear(); ready_=false;
        }
        visible_=settings.visible_on_start; compact_=settings.compact; primed_=false; failed_=false; telemetry_={};
    }
    if (settings.compact!=settings_.compact) compact_=settings.compact;
    if (settings.visible_on_start!=settings_.visible_on_start) visible_=settings.visible_on_start;
    if (!SameInput(settings.toggle_binding,settings_.toggle_binding) || !SameInput(settings.cycle_binding,settings_.cycle_binding)) primed_=false;
    if (primed_ && settings.enabled) {
        if (toggle && !toggle_down_) visible_=!visible_;
        if (cycle && !cycle_down_) { compact_=!compact_; updated_={}; }
    }
    toggle_down_=toggle; cycle_down_=cycle; primed_=true;
    settings_=settings; snapshot_=std::move(snapshot); configured_=true;
    if (settings.enabled) telemetry_.Tick(Clock::now()); else telemetry_={};
    status_.compact=compact_;
    status_.shown=settings.enabled && visible_;
    if (context_ && (!settings.enabled || !visible_)) { status_.visible=false; status_.message=settings.enabled?"Hidden by binding":"Disabled"; }
}
void OsdRenderer::Fail(const char* operation, XrResult result) {
    failed_=true; status_.visible=false;
    status_.message=std::string(operation)+" failed ("+std::to_string(result)+"). Disable and re-enable OSD to retry.";
}
bool OsdRenderer::CreateResources() {
    if (!api_.formats || !api_.create || !api_.images || !api_.acquire || !api_.wait || !api_.release ||
        !api_.destroy || !api_.create_space || !api_.destroy_space) { Fail("OSD dispatch",XR_ERROR_FUNCTION_UNSUPPORTED); return false; }
    // Clean up a failed previous attempt before a user-requested retry.
    if (swapchain_) { api_.destroy(swapchain_); swapchain_=XR_NULL_HANDLE; acquired_.reset(); images_.clear(); ready_=false; }
    if (space_) { api_.destroy_space(space_); space_=XR_NULL_HANDLE; }
    std::uint32_t count=0; auto result=api_.formats(session_,0,&count,nullptr);
    if (XR_FAILED(result) || !count) { Fail("OSD formats",result); return false; }
    std::vector<std::int64_t> formats(count); result=api_.formats(session_,count,&count,formats.data());
    if (XR_FAILED(result)) { Fail("OSD formats",result); return false; }
    std::int64_t format=0;
    for(auto candidate:{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB})
        if (std::find(formats.begin(),formats.end(),candidate)!=formats.end()) { format=candidate; break; }
    if (!format) { Fail("OSD sRGB format",XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED); return false; }
    rgba_=format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create.format=format; create.sampleCount=1; create.width=OsdBitmap::width; create.height=768;
    create.faceCount=1; create.arraySize=1; create.mipCount=1;
    result=api_.create(session_,&create,&swapchain_);
    if (XR_FAILED(result)) { Fail("OSD swapchain",result); return false; }
    result=api_.images(swapchain_,0,&count,nullptr);
    if (XR_FAILED(result) || !count) { Fail("OSD images",result); return false; }
    std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result=api_.images(swapchain_,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    if (XR_FAILED(result)) { Fail("OSD images",result); return false; }
    for (const auto& image:images) images_.push_back(image.texture);
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW; space.poseInReferenceSpace.orientation.w=1;
    result=api_.create_space(session_,&space,&space_);
    if (XR_FAILED(result)) { Fail("OSD view space",result); return false; }
    return true;
}
const XrCompositionLayerBaseHeader* OsdRenderer::Append(const XrFrameEndInfo& info, bool should_render) {
    std::scoped_lock lock(mutex_);
    status_.visible=false;
    if (!settings_.enabled || !visible_ || !context_ || !session_ || failed_) return nullptr;
    if (!should_render || info.layerCount==0) { status_.message="Waiting for a rendered frame"; return nullptr; }
    if (info.layerCount>=max_layers_) { status_.message="Runtime composition layer limit reached"; return nullptr; }
    if (!swapchain_ && !CreateResources()) return nullptr;
    const auto now=Clock::now();
    // Swap only a finished CPU buffer. The worker never holds raster_mutex_
    // while drawing, and the submission path does not wait to acquire it.
    {
        std::unique_lock raster_lock(raster_mutex_,std::try_to_lock);
        if (raster_lock.owns_lock()) {
            if (raster_completed_) {
                upload_=std::move(raster_completed_); raster_completed_.reset(); raster_requested_=false;
            }
            if (!upload_ && !raster_requested_ &&
                (!ready_ || now-updated_>=std::chrono::duration<double>(1.0/settings_.update_hz))) {
                if (!raster_thread_.joinable()) {
                    try { raster_thread_=std::thread([this] { RasterWorker(); }); }
                    catch (...) { Fail("OSD worker",XR_ERROR_OUT_OF_MEMORY); return nullptr; }
                }
                raster_request_=RasterRequest{settings_,compact_,rgba_,snapshot_,telemetry_};
                raster_requested_=true; raster_cv_.notify_one();
            }
        }
    }
    if (upload_) {
        if (!acquired_) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; std::uint32_t index{};
            const auto result=api_.acquire(swapchain_,&acquire,&index);
            if (XR_FAILED(result)) { Fail("OSD acquire",result); return nullptr; }
            acquired_=index;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=0;
        const auto result=api_.wait(swapchain_,&wait);
        // TIMEOUT_EXPIRED is positive but does NOT grant access to the image.
        if (result==XR_TIMEOUT_EXPIRED) { status_.message="Waiting for an overlay image"; return nullptr; }
        if (result!=XR_SUCCESS) { Fail("OSD wait",result); return nullptr; }
        if (*acquired_>=images_.size() || !images_[*acquired_]) { Fail("OSD image index",XR_ERROR_RUNTIME_FAILURE); return nullptr; }
        const auto& bitmap=*upload_;
        if (bitmap.pixels.empty()) { Fail("OSD rasterization",XR_ERROR_OUT_OF_MEMORY); return nullptr; }
        D3D11_BOX box{0,0,0,OsdBitmap::width,static_cast<UINT>(bitmap.height),1};
        context_->UpdateSubresource(images_[*acquired_],0,&box,bitmap.pixels.data(),OsdBitmap::width*4,0);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto released=api_.release(swapchain_,&release);
        if (XR_FAILED(released)) { Fail("OSD release",released); return nullptr; }
        acquired_.reset(); ready_=true; image_height_=bitmap.height; updated_=now; upload_.reset();
    }
    if (!ready_) { status_.message="Preparing the overlay"; return nullptr; }
    constexpr double rad=3.14159265358979323846/180;
    const auto yaw=-settings_.horizontal_degrees*rad, pitch=settings_.vertical_degrees*rad;
    const auto sy=std::sin(yaw/2),cy=std::cos(yaw/2),sp=std::sin(pitch/2),cp=std::cos(pitch/2);
    quad_={XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad_.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad_.space=space_; quad_.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain=swapchain_; quad_.subImage.imageRect.extent={OsdBitmap::width,image_height_};
    quad_.pose.orientation={static_cast<float>(cy*sp),static_cast<float>(sy*cp),static_cast<float>(-sy*sp),static_cast<float>(cy*cp)};
    quad_.pose.position={static_cast<float>(-std::sin(yaw)*std::cos(pitch)*settings_.distance_meters),
        static_cast<float>(std::sin(pitch)*settings_.distance_meters),static_cast<float>(-std::cos(yaw)*std::cos(pitch)*settings_.distance_meters)};
    // Constant angular size when distance changes; depth is independently comfortable.
    quad_.size.width=static_cast<float>(settings_.distance_meters*.55*settings_.scale/100);
    quad_.size.height=quad_.size.width*image_height_/OsdBitmap::width;
    status_.visible=true; status_.message="Visible";
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_);
}
OsdStatus OsdRenderer::Status() const { std::scoped_lock lock(mutex_); return status_; }
} // namespace depthxr
