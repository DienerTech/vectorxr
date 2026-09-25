#pragma once

#include <atomic>
#include <array>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <openxr/openxr.h>
#include "depthxr/settings.h"
#include "depthxr/osd_graphics.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace depthxr {
class Logger;

struct OsdMetric {
    std::uint64_t count{};
    double sum{}, maximum{};
    std::array<std::uint64_t, 256> histogram{};
    void Add(double ms);
    double P95Upper() const;
};
struct OsdMeasurements {
    OsdMetric prepare, append, raster, acquire, wait, upload, release, submit, cadence, age;
    std::uint64_t submitted{}, refreshed{}, no_render{}, layer_limit{}, image_timeouts{}, upload_busy{}, worker_busy{}, failures{}, frame_rejections{};
};

struct OsdDispatch {
    PFN_xrEnumerateSwapchainFormats formats{};
    PFN_xrCreateSwapchain create{};
    PFN_xrEnumerateSwapchainImages images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrCreateReferenceSpace create_space{};
    PFN_xrDestroySpace destroy_space{};
};

struct OsdSnapshot {
    std::string application, runtime, turbo, modules, restart;
    bool experimental{false};
    std::string pivot;
    std::string compact_pivot;
};

struct OsdStatus {
    bool available{false}, visible{false}, compact{false}, shown{false};
    std::string message{"No VR session"};
};

// Submission cadence is deliberately separate from compositor/GPU frame rate.
struct OsdTelemetry {
    std::deque<double> samples;
    std::optional<std::chrono::steady_clock::time_point> last;
    void Tick(std::chrono::steady_clock::time_point now);
    double Mean() const;
    double Percentile95() const;
};

struct OsdBitmap {
    static constexpr int width = 960;
    static constexpr int texture_height = 768;
    int content_width{width};
    int height{};
    std::vector<std::uint32_t> pixels;
    double raster_ms{};
    bool Valid() const { return content_width>2 && content_width<=width && height>2 && height<=texture_height && pixels.size()==static_cast<std::size_t>(width)*height; }
};

struct OsdCompactLayout {
    int width{240}, height{}, header_y{-1}, metrics_y{-1}, status_y{-1}, turbo_y{-1}, pivot_y{-1};
    std::array<int,4> header_x{-1,-1,-1,-1};
    bool fps{}, frame_time{}, empty{};
};
OsdCompactLayout CompactOsdLayout(const OsdSettings&);

// Produces premultiplied sRGB pixels for an sRGB OpenXR swapchain.
OsdBitmap RasterizeOsd(const OsdSettings&, bool compact, const OsdSnapshot&, const OsdTelemetry&, bool rgba);

class OsdRenderer {
  public:
    ~OsdRenderer();
    void Initialize(XrSession, ID3D11Device*, std::uint32_t max_layers, OsdDispatch);
    void InitializeGraphics(XrSession, const void* binding_chain, std::uint32_t max_layers, OsdDispatch, Logger*, std::string identity);
    std::string GraphicsApi() const;
    void RecordPrepare(double ms);
    void RecordSubmit(double ms);
    void ReportDiagnostics(bool final = false);
    void Shutdown();
    void ResetPresentation();
    void SubmissionFailed(XrResult);
    void Prepare(const OsdSettings&, OsdSnapshot, bool toggle_down, bool cycle_down);
    // The returned layer and its image remain valid until the next Append/Shutdown.
    // Only called on the application's end-frame thread, after game-layer transforms.
    const XrCompositionLayerBaseHeader* Append(const XrFrameEndInfo&, bool should_render);
    OsdStatus Status() const;
    std::shared_ptr<const OsdBitmap> PresentedBitmap(bool& rgba) const;
  private:
    void ShutdownLocked();
    bool CreateResources();
    void Fail(const char* operation, XrResult result);
    void RasterWorker();
    void Event(bool error, std::string message);
    std::vector<std::pair<LogLevel,std::string>> CollectDiagnosticsLocked(bool final);
    std::size_t Mode() const;
    mutable std::mutex mutex_;
    XrSession session_{XR_NULL_HANDLE};
    XrSwapchain swapchain_{XR_NULL_HANDLE};
    XrSpace space_{XR_NULL_HANDLE};
    std::unique_ptr<OsdGraphics> graphics_;
    OsdDispatch api_;
    std::uint32_t max_layers_{};
    std::optional<std::uint32_t> acquired_;
    bool acquired_waited_{};
    bool rgba_{false}, ready_{false}, failed_{false}, configured_{false};
    bool visible_{true}, compact_{false}, toggle_down_{false}, cycle_down_{false}, primed_{false};
    OsdSettings settings_;
    OsdSnapshot snapshot_;
    OsdTelemetry telemetry_;
    OsdStatus status_;
    int image_height_{};
    int image_width_{OsdBitmap::width};
    std::chrono::steady_clock::time_point updated_{};
    std::chrono::steady_clock::time_point image_updated_{};
    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // This worker touches only CPU pixels. OpenXR and graphics uploads remain on the
    // application thread. No frame waits for text drawing or font setup.
    struct RasterRequest {
        OsdSettings settings;
        bool compact{}, rgba{};
        OsdSnapshot snapshot;
        OsdTelemetry telemetry;
    };
    std::thread raster_thread_;
    std::mutex raster_mutex_;
    std::condition_variable raster_cv_;
    bool raster_stop_{false}, raster_requested_{false};
    std::optional<RasterRequest> raster_request_;
    std::shared_ptr<const OsdBitmap> presented_bitmap_;
    std::optional<OsdBitmap> raster_completed_, upload_;
    Logger* logger_{};
    std::string identity_;
    std::array<OsdMeasurements, 3> measurements_{};
    std::deque<std::pair<bool,std::string>> events_;
    std::uint64_t dropped_events_{};
    std::chrono::steady_clock::time_point report_start_{}, cadence_last_{};
    std::size_t cadence_mode_{3};
};
} // namespace depthxr
