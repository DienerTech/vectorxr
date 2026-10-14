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
#include <utility>
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
    // Between periodic refreshes, on the clock (effectiveRefreshHz).
    OsdMetric refresh_interval;
    std::uint64_t submitted{}, refreshed{}, no_render{}, layer_limit{}, image_timeouts{}, upload_busy{}, worker_busy{}, failures{}, frame_rejections{};
    // Frames a world-anchored panel went out head-locked: its head pose was not located.
    std::uint64_t anchor_fallbacks{};
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
    PFN_xrLocateSpace locate{};
};

struct OsdSnapshot {
    std::string application, runtime, turbo, modules, restart;
    bool experimental{false};
    std::string pivot;
    std::string compact_pivot;
    // Snap View line; empty (and not drawn) for apps without Snap Views.
    std::string pivot_secondary;
    std::string compact_pivot_secondary;
    std::string framegen{"Disabled"};
    std::string compact_framegen{"Disabled"};
    // A second, smaller line under the expanded row (what the game's depth
    // does), empty for none.
    std::string framegen_detail;
    // The expanded row is shown only in sessions with Frame Generation on.
    bool framegen_session{false};
    // Beside the FPS counter in those sessions: Frame Generation's state (ON,
    // STBY, WAIT, PAUSED, OFF or N/A) and, while it generates, the frames per
    // second the headset shows (0 otherwise).
    std::string framegen_badge;
    double framegen_fps{0.0};
    // Diagnostic modes left on (debug logging, a Frame Generation diagnostic
    // view), named in a tag after the brand; empty for none. While there is
    // one, the header row stays even with every header item hidden.
    std::string diagnostics;
};

struct OsdPresentation {
    bool compact{false}, shown{false};
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
    // The renderer's raster generation when it was requested (OsdRenderer).
    std::uint64_t generation{};
    // An empty compact panel (CompactOsdLayout's empty, the diagnostics tag
    // included): every pixel is 0, and it is not submitted.
    bool empty{false};
    bool Valid() const { return content_width>2 && content_width<=width && height>2 && height<=texture_height && pixels.size()==static_cast<std::size_t>(width)*height; }
};

struct OsdCompactLayout {
    int width{240}, height{}, header_y{-1}, metrics_y{-1}, status_y{-1}, turbo_y{-1}, framegen_y{-1}, pivot_y{-1};
    int status_value_x{128};
    int frame_time_x{262};
    std::array<int,4> header_x{-1,-1,-1,-1};
    // Each header field's column width (narrower beside the diagnostics tag).
    std::array<int,4> header_width{155,260,297,142};
    // The diagnostics tag's slot in the header row (-1: none).
    int diagnostics_x{-1}, diagnostics_width{0};
    // framegen_badge: Frame Generation's badge sits beside the FPS value.
    bool fps{}, frame_time{}, empty{}, framegen_badge{};
};
// What a compact layout must make room for beyond the settings. Named fields,
// not bare bools: 0.19 (the Pivot snap line) and Frame Generation (the FPS
// badge) each added one, and a positional bool compiled with either meaning.
struct OsdCompactLayoutOptions {
    // The second Pivot row used when Snap Views exist.
    bool pivot_snap_line{false};
    // The session started with Frame Generation on, so the FPS value has room
    // for its badge when settings show it.
    bool framegen_session{false};
    // The diagnostics tag's width in pixels (OsdSnapshot::diagnostics), 0 for
    // none: it takes a slot after the brand in the header row.
    int diagnostics_width{0};
};
OsdCompactLayout CompactOsdLayout(const OsdSettings&, const OsdCompactLayoutOptions& options = {});

// RasterizeOsd's last step: feathers the panel's edges to transparent black and
// premultiplies GDI's `bits` into `pixels`, both OsdBitmap::width wide and
// `height` rows. `hidden` (an empty compact panel) leaves every pixel 0.
void FeatherOsdPixels(const std::uint32_t* bits, std::uint32_t* pixels, int panel_width, int height, int opacity,
                      bool hidden, bool rgba);
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
    // Called every frame. A nonzero settings_revision that matches the last
    // call skips copying unchanged settings. Pass a snapshot only when
    // SnapshotDue(): the worker reads it at the refresh rate, not per frame,
    // and not at all while the panel is hidden.
    // Returns the presentation before and after this call's bindings/settings.
    std::pair<OsdPresentation, OsdPresentation> Prepare(const OsdSettings&, std::optional<OsdSnapshot>,
        bool toggle_down, bool cycle_down, std::uint64_t settings_revision = 0);
    bool SnapshotDue() const;
    // The returned layer and its image remain valid until the next Append/Shutdown.
    // Called for every runtime xrEndFrame, after game-layer transforms: on the
    // application's end-frame thread, or Frame Generation's presenter thread.
    // Makes runtime calls (swapchain images; a world-anchored panel's space and
    // head location) under the renderer's lock.
    const XrCompositionLayerBaseHeader* Append(const XrFrameEndInfo&, bool should_render);
    OsdStatus Status() const;
    // Places the panel in the world at each frame's head pose, located at
    // that frame's own display time, instead of in the VIEW space. A runtime
    // that draws a VIEW quad at the pose its frame carries, then turns the
    // whole image to the display pose, moves a head-locked panel by the turn
    // in between; generated frames carry their real frame's pose. Reset by
    // InitializeGraphics.
    void SetWorldAnchored(bool);
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
    // LOCAL, created by Append once the panel is world-anchored.
    XrSpace anchor_space_{XR_NULL_HANDLE};
    bool world_anchored_{false}, anchor_failed_{false}, anchor_logged_{false};
    std::unique_ptr<OsdGraphics> graphics_;
    OsdDispatch api_;
    std::uint32_t max_layers_{};
    std::optional<std::uint32_t> acquired_;
    bool acquired_waited_{};
    bool rgba_{false}, ready_{false}, failed_{false}, configured_{false};
    bool visible_{true}, compact_{false}, toggle_down_{false}, cycle_down_{false}, primed_{false};
    OsdSettings settings_;
    OsdSnapshot snapshot_;
    // The panel was shown without a snapshot (none are taken while hidden):
    // the next raster waits for one.
    bool snapshot_needed_{false};
    // When Prepare last received a snapshot.
    std::chrono::steady_clock::time_point snapshot_at_{};
    // The last periodic refresh drew a snapshot from before it fell due
    // (Frame Generation's presenter found it due before the app's frame took
    // one): the app's next frame takes one for the next refresh.
    bool snapshot_wanted_{false};
    OsdTelemetry telemetry_;
    OsdStatus status_;
    std::uint64_t settings_revision_{};
    int image_height_{};
    int image_width_{OsdBitmap::width};
    // The image up is an empty compact panel (OsdBitmap::empty).
    bool image_empty_{false};
    // When the last refresh fell due (it is requested on the first frame
    // after); zero refreshes on the next frame.
    std::chrono::steady_clock::time_point updated_{};
    std::chrono::steady_clock::time_point image_updated_{};
    // The last periodic refresh's upload (zero after a change).
    std::chrono::steady_clock::time_point refresh_last_{};
    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // This worker touches only CPU pixels. OpenXR and graphics uploads remain on the
    // thread that ends frames. No frame waits for text drawing or font setup.
    struct RasterRequest {
        OsdSettings settings;
        bool compact{}, rgba{};
        OsdSnapshot snapshot;
        OsdTelemetry telemetry;
        std::uint64_t generation{};
    };
    // Advanced by each change that needs a new raster (a show, a layout
    // cycle, new settings): a raster requested before it is dropped.
    std::uint64_t raster_generation_{};
    std::thread raster_thread_;
    std::mutex raster_mutex_;
    std::condition_variable raster_cv_;
    bool raster_stop_{false}, raster_requested_{false};
    std::optional<RasterRequest> raster_request_;
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
