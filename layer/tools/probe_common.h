#pragma once

// vectorxr_hardware_probe: what the OpenXR frame loop (pivot_space_probe.cpp) shares with the GPU load
// (probe_gpu_load.cpp) and the two rendering backends, Direct3D 11 (probe_d3d11.cpp) and Vulkan (probe_vulkan.cpp).
// The probe is a developer tool, so errors are exceptions that end the run with a message.

#include <Windows.h>
#include <d3d11.h>
#include <vulkan/vulkan_core.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace probe {

// Quad views (--quad-views): peripheral left/right, then focus left/right, as
// DCS renders them.
constexpr uint32_t kMaximumViews = 4;
constexpr float kNearZ = 0.05f;
constexpr float kFarZ = 100.0f;
constexpr uint32_t kMaximumEyeDimension = 1280;
// --quad-layer: X-Plane's UI panel, a 2048x2048 quad layer swapchain.
constexpr uint32_t kQuadLayerSize = 2048;

enum class ProbeMode {
    SourceExact,
    CrossSpace,
    CrossSpaceCached,
};

enum class GraphicsApi {
    D3D11,
    Vulkan,
};

struct Options {
    ProbeMode mode{ProbeMode::CrossSpace};
    double seconds{20.0};
    bool threaded_wait{false};
    bool quad_views{false};
    bool full_size{false};
    bool pattern{false};
    bool depth{false};
    double recreate_seconds{0.0};
    int sessions{1};
    bool d3d11_debug{false};
    double hitch_every{0.0}; // seconds
    double hitch_ms{300.0};
    double frame_ms{0.0};
    double gpu_ms{0.0};       // GPU work per frame (0: none)
    int gpu_frames_ahead{2};  // frames whose GPU work may be queued
    bool gpu_ms_fixed{false}; // keep the calibrated work instead of re-measuring
    bool gpu_calibrate{false};
    GraphicsApi graphics{GraphicsApi::D3D11};
    int vulkan_enable{2};                    // XR_KHR_vulkan_enable2, or 1: XR_KHR_vulkan_enable
    uint32_t vulkan_api{VK_API_VERSION_1_2}; // the Vulkan instance's apiVersion
    bool vulkan_validation{false};
    bool vk_late_submit{false};  // submit the frame's rendering after its last release
    bool side_by_side{false};    // one double-wide colour swapchain, each eye an imageRect
    bool quad_layer{false};      // a quad layer released every frame, after the projection
    bool minimal_usage{false};   // colour swapchains with usage 0
    bool render_check{false};    // no OpenXR: draw the shapes into the probe's own images and check their pixels
    bool self_test{false};
    bool help{false};
};

const char* ModeName(ProbeMode mode);

void ThrowIfFailed(HRESULT result, std::string_view operation);
void CheckXr(XrInstance instance, XrResult result, std::string_view operation);
void Expect(bool condition, std::string_view what); // --self-test

// The first of `preferences` the runtime offers in `available`.
std::optional<int64_t> ChooseFormat(const std::vector<int64_t>& available, const std::vector<int64_t>& preferences);

// A rectangle of pixels, right and bottom exclusive.
struct PixelRect {
    int32_t left{0};
    int32_t top{0};
    int32_t right{0};
    int32_t bottom{0};
};

enum class PatternKind {
    Peripheral, // white bar, green grid
    Focus,      // yellow bar, magenta grid (quad views 2-3)
    Quad,       // --quad-layer: orange bar, cyan grid on a translucent panel
};

// --pattern in a view's own pixels: a grid of 7x7 squares and a frame, and a bar sweeping across once every four
// seconds (`seconds`: any steady clock). Rects are clamped to the view; empty ones are dropped.
struct Pattern {
    std::vector<PixelRect> grid;
    PixelRect bar{};
    std::array<float, 4> grid_color{};
    std::array<float, 4> bar_color{};
};
Pattern PatternRects(uint32_t width, uint32_t height, PatternKind kind, double seconds);

// The clear colour of view `view` (left and right eyes slightly different), and the quad layer's translucent panel
// (premultiplied alpha).
std::array<float, 4> ClearColor(uint32_t view);
constexpr std::array<float, 4> kQuadBackground{0.02f, 0.02f, 0.03f, 0.55f};

// The depth buffer value of a point `meters` in front of the eye in the probe's projection (0 near, 1 far).
float DepthAt(float meters);

// --gpu-ms: a compute dispatch of 1024 groups of 64 threads, each looping a dependent sequence `iterations` times, so
// its time grows linearly with the iterations and hardly with the GPU's width.
constexpr uint32_t kLoadGroups = 1024;
constexpr uint32_t kLoadThreads = kLoadGroups * 64;
constexpr uint32_t kLoadFirstIterations = 4096;
constexpr uint32_t kLoadMaximumIterations = 1u << 30;
constexpr int kLoadCalibrationRounds = 8;
constexpr int kLoadCalibrationRuns = 5;
constexpr double kLoadTolerance = 0.03;
constexpr int kLoadRecalibrationFrames = 30;
constexpr int kLoadCalibrationFrames = 200; // --gpu-calibrate

// The value at `fraction` of `sorted` (as the probe's other percentiles); 0 without samples.
double Percentile(const std::vector<double>& sorted, double fraction);
uint32_t ScaleIterations(uint32_t iterations, double factor);
// Calibration's next scale, bounded so that a first measurement near the timer's resolution cannot ask for seconds of
// work in one dispatch.
double CalibrationFactor(double target_ms, double measured_ms);
// Recalibration while running: at most 10% a step, so one disturbed window cannot swing the load.
double RecalibrationFactor(double target_ms, double measured_ms);
// One view's share of a frame's iterations: none without a share, else at least one.
uint32_t ViewIterations(uint32_t iterations, double share);

// What the GPU load needs from a graphics API: dispatches, timestamps and a signal per frame. Frames use the slots of
// a ring in turn; a slot is reused only after its previous frame was collected.
class GpuLoadBackend {
public:
    static constexpr std::size_t kRing = 8;
    static_assert(kRing > 4 + 1, "the ring must hold more frames than --gpu-frames-ahead lets queue");

    virtual ~GpuLoadBackend() = default;
    // `count` untimed dispatches of `iterations` (calibration's warm-up).
    virtual void Warm(uint32_t iterations, int count) = 0;
    // One calibration run: `views` timed dispatches of `iterations` each, waited for; their summed time in ms, or
    // nullopt when one straddled a clock change.
    virtual std::optional<double> TimeViews(uint32_t views, uint32_t iterations) = 0;
    // A frame opens `slot`.
    virtual void BeginSlot(std::size_t slot) = 0;
    // A timed dispatch for the frame in `slot`, as the frame's view `view` (0-3) is about to render.
    virtual void RecordTimed(std::size_t slot, uint32_t view, uint32_t iterations) = 0;
    // The frame in `slot` recorded its last view: its GPU work gets a completion signal.
    virtual void SignalFrame(std::size_t slot) = 0;
    // Whether the frame in `slot` has run; only the first poll of a wait may `flush` queued work to the GPU.
    virtual bool FrameDone(std::size_t slot, bool flush) = 0;
    // Blocks until the frame in `slot` has run.
    virtual void WaitFrame(std::size_t slot) = 0;
    // A timed dispatch's span in ms (the frame has run); nullopt when it straddled a clock change.
    virtual std::optional<double> ReadView(std::size_t slot, uint32_t view) = 0;
};

// --gpu-ms: GPU work that gives each of the probe's frames a game's GPU time. Calibrated with timestamp queries before
// the first frame. Each view's share is recorded right before the view renders, so the view's release (and anything a
// layer queues behind it) waits for it. A signal per frame holds the probe until its frame --gpu-frames-ahead before
// has run on the GPU, as a game's own frame queue would.
class GpuLoad {
public:
    GpuLoad(std::unique_ptr<GpuLoadBackend> backend, const Options& options);

    // The iterations that take --gpu-ms as frames record them, split evenly into `views` timed dispatches (a split
    // load measures a few percent above one dispatch of the same iterations): 30 warm-up dispatches, then rounds of
    // 5 timed frames, each scaled by target / median, until within 3% (at most 8 rounds).
    void Calibrate(uint32_t views);
    // A frame begins: waits until the frame --gpu-frames-ahead before it has run on the GPU.
    void BeginFrame();
    // One view's share of this frame's load (its pixels / all views' pixels), right before the view renders.
    void DispatchView(double share);
    // The frame's last view was recorded; with --gpu-frames-ahead 0 the frame waits for its own GPU work here.
    void EndFrame();
    // Collects every recorded frame's timings (waits for the GPU).
    void Drain();
    // Whether no frame began with more earlier frames queued on the GPU than --gpu-frames-ahead allows.
    bool QueueBounded() const;
    double MeanMs() const;
    void Report(std::ostream& out) const;

private:
    struct Frame {
        uint32_t views_used{0};
        uint32_t iterations{0}; // the frame's total, before its views' shares
    };

    std::optional<double> ReadFrame(std::size_t slot);
    void WaitForFrame(uint64_t index);
    void Collect(bool wait);
    bool CollectOldest(bool wait);

    std::unique_ptr<GpuLoadBackend> backend_;
    double target_ms_{0.0};
    uint64_t frames_ahead_{0};
    bool fixed_{false};
    std::array<Frame, GpuLoadBackend::kRing> ring_{};
    uint64_t frames_{0};    // frames recorded
    uint64_t collected_{0}; // frames whose timings were read, oldest first
    bool open_{false};      // the current frame has recorded a view
    uint32_t iterations_{kLoadFirstIterations};
    uint32_t calibrated_iterations_{0};
    uint64_t adjustments_{0};
    double window_ms_{0.0};
    int window_frames_{0};
    std::vector<double> frame_ms_;
    uint64_t disjoint_frames_{0};
    uint64_t stalls_{0};
    double stall_ms_{0.0};
    double stall_max_ms_{0.0};
    uint64_t max_queued_{0};
};

// --gpu-calibrate's frames: the load alone, paced as a 90 Hz runtime would (or at --frame-ms when longer) and split
// into two views (four with --quad-views). `begin_frame` and `end_frame` bracket each frame for the backend (a
// release's flush, a frame's command buffers). True when the mean load is within 3% of --gpu-ms and no frame began
// with more frames queued than --gpu-frames-ahead allows.
bool RunGpuCalibrationLoop(GpuLoad& load, const Options& options, const std::function<void()>& begin_frame,
                           const std::function<void()>& end_frame);

// What a swapchain is for, when the backend makes its views.
enum class SwapchainRole {
    Projection,          // colour; the backend keeps a private depth buffer for it
    ProjectionWithDepth, // colour whose depth goes to a depth swapchain (--depth)
    Depth,               // a depth swapchain
    Quad,                // the quad layer's colour (--quad-layer)
};

// One view drawn into an acquired image.
struct ViewDraw {
    uint32_t color{0}; // the colour swapchain (AddSwapchain's id) and its acquired image
    uint32_t color_image{0};
    std::optional<uint32_t> depth; // a depth swapchain and its acquired image (--depth)
    uint32_t depth_image{0};
    PixelRect rect{};          // the view's pixels in the image (--side-by-side: one half)
    bool first_in_image{true}; // the frame's first view in this image: it clears all of its depth
    XrPosef pose{};
    XrFovf fov{};
    uint32_t view{0}; // the view's index: the clear colour's eye, focus views' pattern
    bool pattern{false};
    bool quad{false}; // the quad layer: its panel and pattern instead of a view
};

// A graphics API for the probe's OpenXR session: its device, its swapchains' images, and its rendering. Calls come
// from the render thread only; the threaded-wait worker calls nothing but xrWaitFrame.
class ProbeGraphics {
public:
    virtual ~ProbeGraphics() = default;
    // For the probe's output: the API and how it reaches OpenXR.
    virtual std::string Describe() const = 0;
    // The OpenXR instance extension the API needs.
    virtual const char* InstanceExtension() const = 0;
    // The device the runtime asks for (after xrGetSystem, before xrCreateSession).
    virtual void CreateDevice(XrInstance instance, XrSystemId system) = 0;
    // The graphics binding for XrSessionCreateInfo::next.
    virtual const void* Binding() const = 0;
    // Colour formats in order of preference (sRGB first), and the depth format.
    virtual std::vector<int64_t> ColorFormats() const = 0;
    virtual int64_t DepthFormat() const = 0;
    // The GPU load on this device (--gpu-ms).
    virtual std::unique_ptr<GpuLoad> CreateGpuLoad(const Options& options) = 0;
    // A new swapchain's images; returns the id draws use.
    virtual uint32_t AddSwapchain(XrInstance instance, XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                                  SwapchainRole role) = 0;
    // The same for images given directly (ID3D11Texture2D* or VkImage, in the layout OpenXR hands images over in):
    // AddSwapchain's second half, and --render-check's way in.
    virtual uint32_t AddImages(const std::vector<uint64_t>& images, const XrSwapchainCreateInfo& info,
                               SwapchainRole role) = 0;
    virtual uint32_t ImageCount(uint32_t id) const = 0;
    // Before the swapchains are destroyed (no image is held): waits until the GPU is done with them, then drops what
    // was made for them.
    virtual void RemoveSwapchains() = 0;
    // A frame begins (after xrBeginFrame).
    virtual void BeginFrame() = 0;
    virtual void Draw(const ViewDraw& draw) = 0;
    // The image's last view was drawn and the image is about to be released.
    virtual void ImageDone() = 0;
    // After the frame's last release, before xrEndFrame.
    virtual void EndFrame() = 0;
    // At the end of the run: the debug layer's or validation's messages, and the backend's own counts. Returns how
    // many errors and warnings the debug layer or validation reported (0 without them).
    virtual uint64_t Report() = 0;
};

std::unique_ptr<ProbeGraphics> CreateD3D11Graphics(const Options& options);
std::unique_ptr<ProbeGraphics> CreateVulkanGraphics(const Options& options);

// --gpu-calibrate without OpenXR, on the default adapter (Direct3D 11) or the first discrete GPU (Vulkan).
bool RunD3D11GpuCalibration(const Options& options);
bool RunVulkanGpuCalibration(const Options& options);

// --render-check: a graphics backend on its own device (no OpenXR) with images the probe makes itself, standing in
// for a runtime's swapchain images.
class CheckImages {
public:
    virtual ~CheckImages() = default;
    virtual ProbeGraphics& Graphics() = 0;
    // `count` images of `info`, in the layout OpenXR hands images over in, registered with Graphics().
    virtual uint32_t Add(const XrSwapchainCreateInfo& info, SwapchainRole role, uint32_t count) = 0;
    // Once the GPU is done: an RGBA8 image's bytes as stored (sRGB-encoded for sRGB formats), rows top down.
    virtual std::vector<uint8_t> ColorPixels(uint32_t id, uint32_t image) = 0;
    // A depth image's values; nullopt when the backend's pattern writes no depth (Direct3D 11).
    virtual std::optional<std::vector<float>> DepthPixels(uint32_t id, uint32_t image) = 0;
    // Graphics().RemoveSwapchains(), then the images go.
    virtual void Remove() = 0;
};
std::unique_ptr<CheckImages> CreateD3D11CheckImages(const Options& options);
std::unique_ptr<CheckImages> CreateVulkanCheckImages(const Options& options);
// Draws the probe's shapes (side by side with a depth swapchain, one swapchain per view, the quad layer) through the
// backend for a few frames, twice with the swapchains recreated in between, and checks the pixels and that the debug
// layer or validation reported nothing.
bool RunRenderCheck(const Options& options);

// The backends' parts of --self-test (no GPU).
void RunD3D11SelfTest();
void RunVulkanSelfTest();

} // namespace probe
