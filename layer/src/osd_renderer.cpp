#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include "depthxr/osd_renderer.h"
#include "depthxr/logger.h"
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace depthxr {
using Clock = std::chrono::steady_clock;

namespace {
const char* XrResultName(XrResult r){switch(r){
case XR_SUCCESS:return "XR_SUCCESS";case XR_ERROR_FUNCTION_UNSUPPORTED:return "XR_ERROR_FUNCTION_UNSUPPORTED";
case XR_ERROR_RUNTIME_FAILURE:return "XR_ERROR_RUNTIME_FAILURE";case XR_ERROR_OUT_OF_MEMORY:return "XR_ERROR_OUT_OF_MEMORY";
case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED:return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";case XR_ERROR_GRAPHICS_DEVICE_INVALID:return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
case XR_ERROR_LAYER_INVALID:return "XR_ERROR_LAYER_INVALID";case XR_ERROR_LAYER_LIMIT_EXCEEDED:return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
case XR_ERROR_SESSION_LOST:return "XR_ERROR_SESSION_LOST";case XR_ERROR_CALL_ORDER_INVALID:return "XR_ERROR_CALL_ORDER_INVALID";
case XR_ERROR_TIME_INVALID:return "XR_ERROR_TIME_INVALID";
default:return "see OpenXR result code";}}
double Milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct MetricTimer { OsdMetric& metric; Clock::time_point start; ~MetricTimer(){metric.Add(Milliseconds(start));} };
}
void OsdMetric::Add(double ms){if(!std::isfinite(ms)||ms<0)return;++count;sum+=ms;maximum=std::max(maximum,ms);++histogram[std::min<std::size_t>(255,static_cast<std::size_t>(ms<10?ms/.05:200+(ms-10)/2))];}
double OsdMetric::P95Upper() const {if(!count)return 0;std::uint64_t total{};for(std::size_t i=0;i<256;++i){total+=histogram[i];if(total>=static_cast<std::uint64_t>(std::ceil(count*.95)))return i==255?maximum:std::min(maximum,(i<200?(i+1)*.05:10+(i-199)*2.0));}return maximum;}
std::size_t OsdRenderer::Mode() const {return !settings_.enabled?0:visible_?2:1;}
void OsdRenderer::Event(bool error,std::string text){if(events_.size()>=64){++dropped_events_;events_.pop_front();}events_.emplace_back(error,"OSD "+identity_+" "+std::move(text));}
void OsdRenderer::RecordPrepare(double ms){std::scoped_lock lock(mutex_);auto mode=Mode();auto now=Clock::now();auto& stats=measurements_[mode];stats.prepare.Add(ms);
    if(cadence_mode_==mode&&cadence_last_!=Clock::time_point{}){double delta=std::chrono::duration<double,std::milli>(now-cadence_last_).count();if(delta>0&&delta<=1000)stats.cadence.Add(delta);}
    cadence_last_=now;cadence_mode_=mode;
}
void OsdRenderer::RecordSubmit(double ms){std::scoped_lock lock(mutex_);measurements_[Mode()].submit.Add(ms);}
namespace {
void WriteDiagnostics(Logger* logger,const std::vector<std::pair<LogLevel,std::string>>& lines){
    if(!logger)return;for(const auto& [level,text]:lines){if(level==LogLevel::Error)logger->Error(text);else if(level==LogLevel::Debug)logger->Debug(text);else logger->Info(text);}
}
}
void OsdRenderer::ReportDiagnostics(bool final){
    std::vector<std::pair<LogLevel,std::string>> lines;Logger* logger;
    {std::scoped_lock lock(mutex_);logger=logger_;lines=CollectDiagnosticsLocked(final);}
    WriteDiagnostics(logger,lines);
}
std::vector<std::pair<LogLevel,std::string>> OsdRenderer::CollectDiagnosticsLocked(bool final){
    std::vector<std::pair<LogLevel,std::string>> lines;
    if(!logger_){events_.clear();return lines;}
    for(auto& [error,text]:events_)lines.emplace_back(error?LogLevel::Error:LogLevel::Info,std::move(text));events_.clear();
    if(dropped_events_){lines.emplace_back(LogLevel::Info,"OSD diagnostic event overflow="+std::to_string(dropped_events_));dropped_events_=0;}
    const auto now=Clock::now();const double seconds=std::chrono::duration<double>(now-report_start_).count();
    if(!final&&seconds<30)return lines;
    auto metric=[](const char* name,const OsdMetric& m){std::ostringstream out;out<<std::fixed<<std::setprecision(4)<<" "<<name<<"{n="<<m.count<<",avgMs="<<(m.count?m.sum/m.count:0)<<",p95UpperMs="<<m.P95Upper()<<",maxMs="<<m.maximum<<"}";return out.str();};
    for(std::size_t i=0;i<3;++i){const auto& m=measurements_[i];if(!m.prepare.count&&!m.append.count&&!m.failures)continue;
        std::ostringstream out;out<<"OSD summary "<<identity_<<" api="<<(graphics_?graphics_->Name():"Unsupported")<<" mode="<<(i==0?"disabled":i==1?"hidden":"visible")<<" final="<<final<<" windowSeconds="<<seconds
            <<" submitted="<<m.submitted<<" refreshed="<<m.refreshed<<" activeSeconds="<<m.cadence.sum/1000.0<<" effectiveRefreshHz="<<(m.cadence.sum>0?m.refreshed*1000.0/m.cadence.sum:0)<<" noRender="<<m.no_render<<" layerLimit="<<m.layer_limit<<" imageTimeouts="<<m.image_timeouts<<" uploadBusy="<<m.upload_busy<<" workerBusy="<<m.worker_busy<<" failures="<<m.failures
            <<" frameRejections="<<m.frame_rejections<<metric("prepareCpu",m.prepare)<<metric("appendCpu",m.append)<<metric("appCadence",m.cadence)<<metric("runtimeSubmitCpu",m.submit);
        lines.emplace_back(LogLevel::Info,out.str());
        if(logger_->IsDebugEnabled())lines.emplace_back(LogLevel::Debug,"OSD detail "+identity_+" mode="+(i==0?std::string("disabled"):i==1?"hidden":"visible")+metric("rasterWorkerCpu",m.raster)+metric("acquireCpu",m.acquire)+metric("waitCpu",m.wait)+metric("uploadEnqueueCpu",m.upload)+metric("releaseCpu",m.release)+metric("imageAge",m.age)+"; CPU timings do not measure GPU/compositor execution; p95 bins: 0.05ms below 10ms, 2ms below 120ms, overflow=max");
    }
    measurements_={};report_start_=now;return lines;
}

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

OsdCompactLayout CompactOsdLayout(const OsdSettings& settings) {
    OsdCompactLayout layout;
    const std::array<bool,4> headers{settings.compact_show_brand,settings.compact_show_app,settings.compact_show_runtime,settings.compact_show_clock};
    constexpr std::array<int,4> widths{155,260,297,142};
    int x=28, count=0, y=18;
    for (int i=0;i<4;++i) if(headers[i]) { layout.header_x[i]=x; x+=widths[i]+16; ++count; }
    layout.fps=settings.compact_metrics=="all" || settings.compact_metrics=="fps";
    layout.frame_time=settings.compact_metrics=="all" || settings.compact_metrics=="frameTime";
    const bool status=settings.compact_show_turbo || settings.compact_show_pivot;
    layout.empty=!count && !layout.fps && !layout.frame_time && !status;
    layout.width=std::max({240,count?x+12:0,
        (layout.fps || layout.frame_time)?56+(layout.fps?210:0)+(layout.frame_time?300:0)+(layout.fps&&layout.frame_time?24:0):0,
        status?520:0});
    if(count) { layout.header_y=y; y+=44; }
    if(layout.fps || layout.frame_time) { layout.metrics_y=y; y+=68; }
    if(settings.compact_show_turbo) { layout.turbo_y=y; y+=48; }
    if(settings.compact_show_pivot) { layout.pivot_y=y; y+=48; }
    layout.status_y=layout.turbo_y>=0?layout.turbo_y:layout.pivot_y;
    layout.height=layout.empty?80:y+6;
    return layout;
}

OsdBitmap RasterizeOsd(const OsdSettings& settings, bool compact, const OsdSnapshot& snapshot,
                       const OsdTelemetry& telemetry, bool rgba) {
    constexpr auto white=RGB(233,240,248), muted=RGB(148,165,184), bg=RGB(13,20,31), inset=RGB(20,31,45);
    COLORREF accent=RGB(81,221,189);
    if (settings.accent=="copper") accent=RGB(245,174,120);
    else if (settings.accent=="blue") accent=RGB(111,172,255);
    else if (settings.accent=="violet") accent=RGB(189,154,255);
    else if (settings.accent=="rose") accent=RGB(255,145,178);
    else if (settings.accent=="custom" && settings.custom_color.size()==7) {
        const auto rgb=std::strtoul(settings.custom_color.c_str()+1,nullptr,16);
        accent=RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255);
    }
    const auto layout=CompactOsdLayout(settings);
    const int panel_width=compact?layout.width:OsdBitmap::width;
    const int header_shift=(settings.show_brand || settings.show_app || settings.show_runtime || settings.show_clock)?0:40;
    const int height=compact?layout.height:210-header_shift+(settings.show_graph?156:0)+
        (settings.show_turbo?52:0)+(settings.show_pivot?52:0)+(settings.show_modules?52:0)+(!snapshot.restart.empty()?48:0);
    OsdBitmap output; output.height=height; output.content_width=panel_width;
    Canvas canvas(output.width,height);
    if (!canvas.bits || !canvas.dc) return output;
    canvas.Rect(0,0,output.width,height,bg);
    SYSTEMTIME time; GetLocalTime(&time);
    char clock_label[24];
    if (settings.clock_format=="12") snprintf(clock_label,sizeof(clock_label),"%u:%02u %s",time.wHour%12?time.wHour%12:12,time.wMinute,time.wHour<12?"AM":"PM");
    else snprintf(clock_label,sizeof(clock_label),"%02u:%02u",time.wHour,time.wMinute);
    const auto mean=telemetry.Mean();
    if (compact) {
        constexpr std::array<int,4> widths{155,260,297,142};
        const std::array<std::string,4> values{"VECTORXR",snapshot.application,snapshot.runtime,clock_label};
        for(int i=0;i<4;++i) if(layout.header_x[i]>=0)
            canvas.Text(layout.header_x[i],layout.header_y,widths[i],i==0?22:21,values[i],i==0?accent:muted,i==0);
        if(layout.fps) canvas.Text(28,layout.metrics_y,210,40,Number(mean>0?1000/mean:0,0)+" fps",white,true);
        if(layout.frame_time) canvas.Text(layout.fps?262:28,layout.metrics_y,300,40,Number(mean)+" ms avg",white,true);
        if(layout.status_y>=0) {
            const int y=layout.status_y;
            if(y>18) canvas.Rect(28,y-8,panel_width-56,1,inset);
            if(settings.compact_show_turbo) {
                canvas.Text(30,layout.turbo_y+2,90,20,"TURBO",muted);
                canvas.Text(128,layout.turbo_y,panel_width-156,24,snapshot.turbo+(snapshot.experimental?" / EXP":""),accent,true);
            }
            if(settings.compact_show_pivot) {
                canvas.Text(30,layout.pivot_y+2,80,20,"PIVOT",muted);
                canvas.Text(128,layout.pivot_y,panel_width-156,24,snapshot.compact_pivot.empty()?snapshot.pivot:snapshot.compact_pivot,white,true);
            }
        }
    } else {
        if(settings.show_brand) canvas.Text(28,18,155,22,"VECTORXR",accent,true);
        const int clock_x=settings.show_clock?790:932;
        const int app_x=settings.show_brand?200:28;
        const int runtime_x=settings.show_runtime?(settings.show_app?(app_x+clock_x)/2:app_x):clock_x;
        if(settings.show_app) canvas.Text(app_x,18,runtime_x-app_x-16,21,snapshot.application,muted);
        if(settings.show_runtime) canvas.Text(runtime_x,18,clock_x-runtime_x-18,21,snapshot.runtime,muted);
        if(settings.show_clock) canvas.Text(clock_x,18,142,21,clock_label,muted);
        canvas.Text(28,57-header_shift,265,68,Number(mean>0?1000/mean:0,0),white,true);
        canvas.Text(325,57-header_shift,280,68,Number(mean)+" ms",white,true);
        canvas.Text(655,57-header_shift,276,68,Number(telemetry.Percentile95())+" ms",accent,true);
        canvas.Text(30,141-header_shift,270,20,"APP FPS",muted);
        canvas.Text(327,141-header_shift,280,20,"APP FRAME / AVG",muted);
        canvas.Text(657,141-header_shift,275,20,"APP FRAME / P95",muted);
    }
    if (!compact) {
        int y=190-header_shift;
        for (const auto& row:settings.body_order) {
            if (row=="graph" && settings.show_graph) {
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
            } else if ((row=="turbo" && settings.show_turbo) || (row=="pivot" && settings.show_pivot) || (row=="modules" && settings.show_modules)) {
                const auto label=row=="turbo"?"TURBO":row=="pivot"?"PIVOT":"ENHANCEMENTS";
                const auto value=row=="turbo"?snapshot.turbo+(snapshot.experimental?" / EXPERIMENTAL":""):row=="pivot"?snapshot.pivot:snapshot.modules;
                canvas.Text(30,y,210,20,label,muted);
                canvas.Text(242,y,680,24,value,row=="turbo"?accent:white,true); y+=52;
            }
        }
        if (!snapshot.restart.empty()) canvas.Text(30,y,902,20,"RESTART PENDING  /  "+snapshot.restart,RGB(245,185,108));
    }
    // Complete batched GDI writes before reading the DIB directly.
    GdiFlush();
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
        // Transparent black surrounds every edge, including the straight sides.
        // The submitted rect is inset one texel below, so filtering stays inside
        // initialized pixels even when the unused swapchain rows contain old data.
        constexpr int padding=3;
        const double dx=std::max({18.0+padding-x,0.0,x-(panel_width-19.0-padding)});
        const double dy=std::max({18.0+padding-y,0.0,y-(height-19.0-padding)});
        const double edge=std::min({x-padding+.0,panel_width-1.0-padding-x,y-padding+.0,height-1.0-padding-y,
            (dx>0 && dy>0)?18.0-std::hypot(dx,dy):18.0});
        const double ramp=std::clamp(edge/6.0,0.0,1.0);
        const double coverage=ramp*ramp*(3-2*ramp);
        const auto alpha=static_cast<unsigned>(std::lround(255*settings.opacity/100.0*coverage*(compact&&layout.empty?0:1)));
        // Neutral feathering softens the silhouette. Headset comparisons with
        // hard and opaque edges did not eliminate the reported color fringe.
        const double interior=std::clamp((edge-6)/6.0,0.0,1.0);
        const auto source=canvas.bits[y*output.width+x];
        const auto channel=[&](unsigned shift){return static_cast<unsigned>(std::lround(16+(((source>>shift)&255)-16.0)*interior));};
        const auto pixel=interior==1.0?source:(channel(16)<<16)|(channel(8)<<8)|channel(0);
        const unsigned b=lut[alpha][pixel&255],g=lut[alpha][(pixel>>8)&255],r=lut[alpha][(pixel>>16)&255];
        output.pixels[y*output.width+x]=(alpha<<24)|(g<<8)|(rgba?(b<<16)|r:(r<<16)|b);
    }
    return output;
}

OsdRenderer::~OsdRenderer() { Shutdown(); }
void OsdRenderer::RasterWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    bool edge_logged=false;
    for (;;) {
        RasterRequest request;
        {
            std::unique_lock lock(raster_mutex_);
            raster_cv_.wait(lock,[&] { return raster_stop_ || raster_request_.has_value(); });
            if (raster_stop_) return;
            request=std::move(*raster_request_); raster_request_.reset();
        }
        OsdBitmap bitmap;
        const auto raster_start=Clock::now();
        try { bitmap=RasterizeOsd(request.settings,request.compact,request.snapshot,request.telemetry,request.rgba); }
        catch (...) { /* Report a failed image through the normal OSD status path. */ }
        bitmap.raster_ms=std::chrono::duration<double,std::milli>(Clock::now()-raster_start).count();
        if(!edge_logged && bitmap.Valid() && logger_ && logger_->IsDebugEnabled()) {
            // Inspect the actual game's CPU image once, off the submission
            // thread. Opposing edge strips distinguish source-image bands
            // from color separation introduced after OpenXR submission.
            std::ostringstream edge;
            edge<<"OSD source edges "<<identity_<<" encoding=premultiplied-linear-sRGB packed="
                <<(request.rgba?"ABGR":"ARGB")<<" alpha=source edgeTreatment=neutral-feather rectInset=1 guard=3 width="
                <<bitmap.content_width<<" height="<<bitmap.height<<" opacity="<<request.settings.opacity;
            std::size_t nonzero_guard=0;
            for(int y=0;y<bitmap.height;++y)for(int x=0;x<OsdBitmap::width;++x)
                if((x<3 || y<3 || x>=bitmap.content_width-3 || y>=bitmap.height-3) &&
                   bitmap.pixels[y*OsdBitmap::width+x]!=0) ++nonzero_guard;
            edge<<" nonzeroGuardPixels="<<nonzero_guard<<std::hex<<std::setfill('0');
            for(int side=0;side<4;++side) {
                edge<<" "<<std::array{"top","bottom","left","right"}[side]<<"[";
                for(int d=0;d<16;++d) {
                    const int x=side<2?bitmap.content_width/2:side==2?d:bitmap.content_width-1-d;
                    const int y=side>=2?bitmap.height/2:side==0?d:bitmap.height-1-d;
                    if(d)edge<<",";
                    edge<<std::setw(8)<<bitmap.pixels[y*OsdBitmap::width+x];
                }
                edge<<"]";
            }
            logger_->Debug(edge.str()); edge_logged=true;
        }
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
    raster_stop_=false; raster_requested_=false; raster_request_.reset(); raster_completed_.reset(); upload_.reset(); presented_bitmap_.reset();
    if(graphics_) graphics_->Reset();
    if (swapchain_ && api_.destroy) api_.destroy(swapchain_);
    if (space_ && api_.destroy_space) api_.destroy_space(space_);
    swapchain_=XR_NULL_HANDLE; space_=XR_NULL_HANDLE; session_=XR_NULL_HANDLE;
    WriteDiagnostics(logger_,CollectDiagnosticsLocked(true)); graphics_.reset();
    acquired_.reset(); acquired_waited_=false; ready_=false; failed_=false; configured_=false; primed_=false;
    telemetry_={}; status_={}; updated_={}; image_updated_={};
}
void OsdRenderer::Shutdown() { std::scoped_lock lock(mutex_); ShutdownLocked(); }
void OsdRenderer::ResetPresentation() {
    std::scoped_lock lock(mutex_);
    configured_=false; primed_=false; telemetry_={}; updated_={};
    status_.visible=false;
}
void OsdRenderer::SubmissionFailed(XrResult result) {
    std::scoped_lock lock(mutex_);
    if(result==XR_ERROR_CALL_ORDER_INVALID || result==XR_ERROR_TIME_INVALID) {
        // A pacing transition can reject the whole frame without invalidating
        // this layer's resources. Never retry that xrEndFrame; let the next
        // application frame present the existing OSD normally.
        auto& count=measurements_[Mode()].frame_rejections;
        if(!count)Event(false,"frame rejected XrResult="+std::to_string(result)+"("+XrResultName(result)+"); preserving OSD for subsequent frames");
        ++count;
        return;
    }
    Fail("OSD frame submission",result);
}
void OsdRenderer::Initialize(XrSession session, ID3D11Device* device, std::uint32_t max_layers, OsdDispatch api) {
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=device;
    InitializeGraphics(session,&binding,max_layers,api,nullptr,{});
}
void OsdRenderer::InitializeGraphics(XrSession session,const void* chain,std::uint32_t max_layers,OsdDispatch api,Logger* logger,std::string identity) {
    std::scoped_lock lock(mutex_);ShutdownLocked();
    session_=session;max_layers_=max_layers;api_=api;logger_=logger;identity_=std::move(identity);
    measurements_={};events_.clear();dropped_events_=0;report_start_=Clock::now();cadence_last_={};cadence_mode_=3;
    std::string error;graphics_=CreateOsdGraphics(chain,error);
    status_.available=graphics_!=nullptr;status_.message=graphics_?"Disabled":error.empty()?"OSD graphics binding unavailable":error;
    Event(false,"initialize api="+std::string(graphics_?graphics_->Name():"Unsupported")+" maxLayers="+std::to_string(max_layers_)+" status="+status_.message);
}
std::string OsdRenderer::GraphicsApi() const { std::scoped_lock lock(mutex_);return graphics_?graphics_->Name():"Other"; }
void OsdRenderer::Prepare(const OsdSettings& settings, OsdSnapshot snapshot, bool toggle, bool cycle) {
    std::scoped_lock lock(mutex_);
    if (!configured_ || (settings.enabled && !settings_.enabled)) {
        if (failed_) {
            if(graphics_) graphics_->Reset();
            if (swapchain_ && api_.destroy) api_.destroy(swapchain_);
            if (space_ && api_.destroy_space) api_.destroy_space(space_);
            swapchain_=XR_NULL_HANDLE; space_=XR_NULL_HANDLE; acquired_.reset(); acquired_waited_=false; ready_=false;
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
    const bool changed=!configured_ || settings.enabled!=settings_.enabled || compact_!=status_.compact ||
        (settings.enabled&&visible_)!=status_.shown || settings.update_hz!=settings_.update_hz ||
        settings.scale!=settings_.scale || settings.opacity!=settings_.opacity || settings.body_order!=settings_.body_order ||
        settings.show_graph!=settings_.show_graph || settings.show_turbo!=settings_.show_turbo || settings.show_pivot!=settings_.show_pivot || settings.show_modules!=settings_.show_modules ||
        settings.show_runtime!=settings_.show_runtime || settings.show_brand!=settings_.show_brand || settings.show_app!=settings_.show_app || settings.show_clock!=settings_.show_clock || settings.clock_format!=settings_.clock_format ||
        settings.compact_metrics!=settings_.compact_metrics || settings.compact_show_brand!=settings_.compact_show_brand || settings.compact_show_app!=settings_.compact_show_app ||
        settings.compact_show_runtime!=settings_.compact_show_runtime || settings.compact_show_clock!=settings_.compact_show_clock ||
        settings.compact_show_turbo!=settings_.compact_show_turbo || settings.compact_show_pivot!=settings_.compact_show_pivot ||
        settings.accent!=settings_.accent || settings.custom_color!=settings_.custom_color || settings.horizontal_degrees!=settings_.horizontal_degrees ||
        settings.vertical_degrees!=settings_.vertical_degrees || settings.distance_meters!=settings_.distance_meters;
    settings_=settings; snapshot_=std::move(snapshot); configured_=true;
    if(changed){
        Event(false,"configuration enabled="+std::to_string(settings.enabled)+" shown="+std::to_string(settings.enabled&&visible_)+" compact="+std::to_string(compact_)+
            " refreshHz="+std::to_string(settings.update_hz)+" scale="+std::to_string(settings.scale)+" opacity="+std::to_string(settings.opacity)+
            " graph="+std::to_string(settings.show_graph)+" turbo="+std::to_string(settings.show_turbo)+" pivot="+std::to_string(settings.show_pivot)+" modules="+std::to_string(settings.show_modules));
        updated_={};
    }
    if (settings.enabled) telemetry_.Tick(Clock::now()); else telemetry_={};
    status_.compact=compact_;
    status_.shown=settings.enabled && visible_;
    if (graphics_ && (!settings.enabled || !visible_)) { status_.visible=false; status_.message=settings.enabled?"Hidden by binding":"Disabled"; }
}
void OsdRenderer::Fail(const char* operation, XrResult result) {
    Event(true,std::string(operation)+" XrResult="+std::to_string(result)+"("+XrResultName(result)+")"+"; OSD disabled until explicit retry");
    ++measurements_[Mode()].failures; failed_=true; status_.visible=false;
    status_.message=std::string(operation)+" failed ("+std::to_string(result)+"). Disable and re-enable OSD to retry.";
}
bool OsdRenderer::CreateResources() {
    const auto initialization_start=Clock::now();
    if (!api_.formats || !api_.create || !api_.images || !api_.acquire || !api_.wait || !api_.release ||
        !api_.destroy || !api_.create_space || !api_.destroy_space) { Fail("OSD dispatch",XR_ERROR_FUNCTION_UNSUPPORTED); return false; }
    // Clean up a failed previous attempt before a user-requested retry.
    if (swapchain_) { api_.destroy(swapchain_); swapchain_=XR_NULL_HANDLE; acquired_.reset(); acquired_waited_=false; ready_=false; }
    if (space_) { api_.destroy_space(space_); space_=XR_NULL_HANDLE; }
    std::uint32_t count=0; auto result=api_.formats(session_,0,&count,nullptr);
    if (XR_FAILED(result) || !count) { Fail("OSD formats",result); return false; }
    std::vector<std::int64_t> formats(count); result=api_.formats(session_,count,&count,formats.data());
    if (XR_FAILED(result)) { Fail("OSD formats",result); return false; }
    std::int64_t format=0;
    // Follow the runtime's preference order, as OpenXR Toolkit does, while
    // restricting selection to formats our raster/upload path can encode.
    const auto supported=graphics_->Formats();
    for(auto candidate:formats)
        if (std::find(supported.begin(),supported.end(),candidate)!=supported.end()) { format=candidate; break; }
    if (!format) { Fail("OSD sRGB format",XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED); return false; }
    rgba_=graphics_->Rgba(format);
    Event(false,"format selection runtimePreferred="+std::to_string(formats.front())+
        " selected="+std::to_string(format)+" channels="+(rgba_?"RGBA":"BGRA"));
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create.format=format; create.sampleCount=1; create.width=OsdBitmap::width; create.height=768;
    create.faceCount=1; create.arraySize=1; create.mipCount=1;
    result=api_.create(session_,&create,&swapchain_);
    if (XR_FAILED(result)) { Fail("OSD swapchain",result); return false; }
    std::string error;
    if(!graphics_->Images(swapchain_,api_,error)){Fail(error.c_str(),XR_ERROR_GRAPHICS_DEVICE_INVALID);return false;}
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW; space.poseInReferenceSpace.orientation.w=1;
    result=api_.create_space(session_,&space,&space_);
    if (XR_FAILED(result)) { Fail("OSD view space",result); return false; }
    Event(false,"resources ready api="+std::string(graphics_->Name())+" format="+std::to_string(format)+" texture=960x768 usage=transfer-dst|color initializationCpuMs="+std::to_string(Milliseconds(initialization_start)));
    return true;
}
const XrCompositionLayerBaseHeader* OsdRenderer::Append(const XrFrameEndInfo& info, bool should_render) {
    const auto entry=Clock::now();
    std::scoped_lock lock(mutex_);
    auto& stats=measurements_[Mode()];
    MetricTimer append_timer(stats.append,entry);
    status_.visible=false;
    if (!settings_.enabled || !visible_ || !graphics_ || !session_ || failed_) return nullptr;
    if (!should_render || info.layerCount==0) { ++stats.no_render; status_.message="Waiting for a rendered frame"; return nullptr; }
    if (info.layerCount>=max_layers_) { if(!stats.layer_limit)Event(false,"composition layer limit reached appLayers="+std::to_string(info.layerCount)+" runtimeMax="+std::to_string(max_layers_)); ++stats.layer_limit; status_.message="Runtime composition layer limit reached"; return nullptr; }
    if (!swapchain_ && !CreateResources()) return nullptr;
    const auto now=Clock::now();
    // Swap only a finished CPU buffer. The worker never holds raster_mutex_
    // while drawing, and the submission path does not wait to acquire it.
    {
        std::unique_lock raster_lock(raster_mutex_,std::try_to_lock);
        if (raster_lock.owns_lock()) {
            if (raster_completed_) {
                stats.raster.Add(raster_completed_->raster_ms); upload_=std::move(raster_completed_); raster_completed_.reset(); raster_requested_=false;
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
        } else ++stats.worker_busy;
    }
    // A refresh that cannot complete this frame (image still in use, previous
    // GPU copy pending) keeps its acquired image and retries next frame. The
    // last released image stays valid, so keep presenting it: returning no
    // layer would blink the panel off for that frame.
    const char* refresh_pending=nullptr;
    if (upload_) {
        if (!acquired_) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; std::uint32_t index{};
            const auto acquire_start=Clock::now();
            const auto result=api_.acquire(swapchain_,&acquire,&index);
            stats.acquire.Add(Milliseconds(acquire_start));
            if (XR_FAILED(result)) { Fail("OSD acquire",result); return nullptr; }
            acquired_=index; acquired_waited_=false;
        }
        if(!acquired_waited_){
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=0;
            const auto wait_start=Clock::now();const auto result=api_.wait(swapchain_,&wait);stats.wait.Add(Milliseconds(wait_start));
            if(result==XR_TIMEOUT_EXPIRED){++stats.image_timeouts;refresh_pending="Waiting for an overlay image";}
            else if(result!=XR_SUCCESS && result!=XR_SESSION_LOSS_PENDING){Fail("OSD wait",result);return nullptr;}
            else acquired_waited_=true;
        }
        if(!refresh_pending) {
            const auto& bitmap=*upload_;
            if(bitmap.pixels.empty()){Fail("OSD rasterization",XR_ERROR_OUT_OF_MEMORY);return nullptr;}
            std::string error;const auto upload_start=Clock::now();const auto uploaded=graphics_->Upload(*acquired_,bitmap,error);stats.upload.Add(Milliseconds(upload_start));
            if(uploaded==OsdUpload::Busy){++stats.upload_busy;refresh_pending="Waiting for previous OSD upload";}
            else if(uploaded==OsdUpload::Failed){Fail(error.c_str(),XR_ERROR_GRAPHICS_DEVICE_INVALID);return nullptr;}
            else {
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                const auto release_start=Clock::now();const auto released=api_.release(swapchain_,&release);stats.release.Add(Milliseconds(release_start));
                if (XR_FAILED(released)) { Fail("OSD release",released); return nullptr; }
                acquired_.reset(); acquired_waited_=false; ++stats.refreshed; ready_=true; image_height_=bitmap.height; image_width_=bitmap.content_width; updated_=now; image_updated_=Clock::now();
                presented_bitmap_=std::make_shared<OsdBitmap>(std::move(*upload_));upload_.reset();
            }
        }
    }
    if (!ready_) { status_.message=refresh_pending?refresh_pending:"Preparing the overlay"; return nullptr; }
    if (compact_ && CompactOsdLayout(settings_).empty) { status_.visible=false; status_.message="No compact values selected"; return nullptr; }
    constexpr double rad=3.14159265358979323846/180;
    const auto yaw=-settings_.horizontal_degrees*rad, pitch=settings_.vertical_degrees*rad;
    const auto sy=std::sin(yaw/2),cy=std::cos(yaw/2),sp=std::sin(pitch/2),cp=std::cos(pitch/2);
    quad_={XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad_.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad_.space=space_; quad_.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain=swapchain_;
    quad_.subImage.imageRect.offset={1,1};
    quad_.subImage.imageRect.extent={image_width_-2,image_height_-2};
    quad_.pose.orientation={static_cast<float>(cy*sp),static_cast<float>(sy*cp),static_cast<float>(-sy*sp),static_cast<float>(cy*cp)};
    quad_.pose.position={static_cast<float>(-std::sin(yaw)*std::cos(pitch)*settings_.distance_meters),
        static_cast<float>(std::sin(pitch)*settings_.distance_meters),static_cast<float>(-std::cos(yaw)*std::cos(pitch)*settings_.distance_meters)};
    // Constant angular size when distance changes; depth is independently comfortable.
    quad_.size.width=static_cast<float>(settings_.distance_meters*.55*settings_.scale/100*(image_width_-2)/(OsdBitmap::width-2));
    quad_.size.height=quad_.size.width*(image_height_-2)/(image_width_-2);
    ++stats.submitted;stats.age.Add(Milliseconds(image_updated_));
    status_.visible=true; status_.message="Visible";
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad_);
}

OsdStatus OsdRenderer::Status() const { std::scoped_lock lock(mutex_); return status_; }
std::shared_ptr<const OsdBitmap> OsdRenderer::PresentedBitmap(bool& rgba) const {
    std::scoped_lock lock(mutex_);rgba=rgba_;return ready_?presented_bitmap_:nullptr;
}
} // namespace depthxr
