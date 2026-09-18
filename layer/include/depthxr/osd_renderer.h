#pragma once

#include <atomic>
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

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace depthxr {

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
    int height{};
    std::vector<std::uint32_t> pixels;
};

// Produces premultiplied sRGB pixels for an sRGB OpenXR swapchain.
OsdBitmap RasterizeOsd(const OsdSettings&, bool compact, const OsdSnapshot&, const OsdTelemetry&, bool rgba);

class OsdRenderer {
  public:
    ~OsdRenderer();
    void Initialize(XrSession, ID3D11Device*, std::uint32_t max_layers, OsdDispatch);
    void Shutdown();
    void ResetPresentation();
    void SubmissionFailed(XrResult);
    void Prepare(const OsdSettings&, OsdSnapshot, bool toggle_down, bool cycle_down);
    // The returned layer and its image remain valid until the next Append/Shutdown.
    // Only called on the application's end-frame thread, after game-layer transforms.
    const XrCompositionLayerBaseHeader* Append(const XrFrameEndInfo&, bool should_render);
    OsdStatus Status() const;
  private:
    void ShutdownLocked();
    bool CreateResources();
    void Fail(const char* operation, XrResult result);
    void RasterWorker();
    mutable std::mutex mutex_;
    XrSession session_{XR_NULL_HANDLE};
    XrSwapchain swapchain_{XR_NULL_HANDLE};
    XrSpace space_{XR_NULL_HANDLE};
    ID3D11Device* device_{};
    ID3D11DeviceContext* context_{};
    OsdDispatch api_;
    std::uint32_t max_layers_{};
    std::vector<ID3D11Texture2D*> images_;
    std::optional<std::uint32_t> acquired_;
    bool rgba_{false}, ready_{false}, failed_{false}, configured_{false};
    bool visible_{true}, compact_{false}, toggle_down_{false}, cycle_down_{false}, primed_{false};
    OsdSettings settings_;
    OsdSnapshot snapshot_;
    OsdTelemetry telemetry_;
    OsdStatus status_;
    int image_height_{};
    std::chrono::steady_clock::time_point updated_{};
    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // This worker touches only CPU pixels. OpenXR and D3D11 remain on the
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
    std::optional<OsdBitmap> raster_completed_, upload_;
};
} // namespace depthxr
