#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/framegen_plan.h"

namespace depthxr {

// One swapchain image region, identified by its OpenXR swapchain. The
// backend resolves native images itself through Track().
struct FrameGenImage {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t image_index{0};
    std::uint32_t array_index{0};
    XrRect2Di rect{};
    std::int64_t format{0}; // swapchain creation format, native to the graphics API
};

struct FrameGenViewSubmission {
    FrameGenImage source; // newest real frame, already released by the app
    // Acquired and waited images of layer-owned output swapchains, one per
    // generated frame (FrameGenTuning::generated).
    std::vector<FrameGenImage> outputs;
    FrameGenViewRequest request;
};

struct FrameGenExecuteResult {
    std::vector<bool> synthesized; // per view; false = the newest frame was written unchanged
    std::vector<bool> depth;       // per view; the newest frame's depth was used
    // Execute failed only because a view's image was not captured when the
    // application released it (SetCopyAfterRelease): nothing was generated,
    // and the views are now set up so the next releases are captured.
    bool uncaptured{false};
    // Identifies this submission in LastGpuWindow; 0 = not timed.
    std::uint64_t submission{0};
    // FrameGenTuning::analyze_only: the real frame Synthesize can now write
    // generated frames from (0: none, Execute failed).
    std::uint64_t frame{0};
};

// When one submission ran on the GPU, on std::chrono::steady_clock.
struct FrameGenGpuWindow {
    std::uint64_t submission{0};
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point end{};
};

struct FrameGenMemoryBudget {
    std::uint64_t usage_bytes{0};
    std::uint64_t budget_bytes{0};
};

class FrameGenGpu {
  public:
    virtual ~FrameGenGpu() = default;
    virtual const char* Name() const = 0;
    // Output kind for an application's color format; nullopt = unsupported.
    virtual std::optional<FrameGenOutputKind> Classify(std::int64_t format) const = 0;
    // Preferred layer-owned output swapchain formats, best first.
    virtual std::vector<std::int64_t> OutputFormats(FrameGenOutputKind kind) const = 0;
    // Usage the application's color swapchains need so their images can be
    // captured (Vulkan copies need TRANSFER_SRC; Direct3D needs nothing).
    virtual XrSwapchainUsageFlags RequiredSourceUsage() const = 0;
    virtual XrSwapchainUsageFlags OutputUsage() const = 0;
    virtual bool Track(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages enumerate, std::string& error) = 0;
    virtual void Forget(XrSwapchain swapchain) = 0;
    // Copies an application image into `view`'s history while the
    // application still owns it: called from xrReleaseSwapchainImage before
    // the release is forwarded, so the image is in its documented
    // pre-release state and no runtime work can touch it yet. Execute then
    // skips its own copy for that view when the image matches. Capture never
    // reallocates: if the view's textures do not match the request yet it
    // returns false with an empty error and Execute copies instead.
    virtual bool Capture(std::size_t view, const FrameGenImage& source, const FrameGenViewRequest& request,
                         FrameGenFlowQuality quality, std::string& error) = 0;
    // Like Capture, for the depth image the application submitted with
    // `view` (XrCompositionLayerDepthInfoKHR): converted to linear depth
    // for the view's next Execute, which then sets request.depth's mapping
    // if the request asks for it. `request` describes the color view. False
    // with an empty error when the backend has no depth path or the view is
    // not set up yet.
    virtual bool CaptureDepth(std::size_t /*view*/, const FrameGenImage& /*depth*/,
                              const FrameGenDepthEncoding& /*encoding*/, const FrameGenViewRequest& /*request*/,
                              std::string& error) {
        error.clear();
        return false;
    }
    // Usage the application's depth swapchains need for CaptureDepth (none
    // when the backend has no depth path).
    virtual XrSwapchainUsageFlags RequiredDepthUsage() const { return 0; }
    virtual bool SupportsDepth() const { return false; }
    // How the game's depth is being read after checking it against the
    // other eye, with the evidence; counts decisions so callers log each once.
    virtual FrameGenDepthStatus DepthStatus() const { return {}; }
    // Benchmarks only: while on, Execute stamps the GPU time after every
    // pass and waits for the GPU, and PassMilliseconds() reports the last
    // Execute's time per pass (kFrameGenPassCount entries, then source
    // copies, then copies into the output swapchains). Empty where the
    // backend cannot profile.
    virtual void SetProfiling(bool /*on*/) {}
    virtual std::vector<double> PassMilliseconds() const { return {}; }
    // Direct3D 12 draws a view's generated frames straight into the output
    // images when it can (on by default); off writes them by compute and
    // copies them, as the other backends do. For tests.
    virtual void SetDirectOutput(bool /*on*/) {}
    // Whether generated frames are drawn straight into the output images
    // (no intermediate image per view).
    virtual bool DrawsOutputs() const { return false; }
    // FrameGenTuning::hardware_flow: empty until asked for, then "active",
    // or why the hardware cannot be used (block matching measures motion).
    virtual std::string HardwareFlowStatus() const { return "this graphics API has no optical flow hardware support"; }
    // Tests: the next optical flow hardware work is never handed to the
    // hardware, as if it had stopped responding.
    virtual void StallHardwareForTest() {}
    // Tests and research: the flow the last Execute measured for `view`
    // (the filtered field synthesis used) in full-resolution pixels, x and
    // y per cell row by row; `cells` receives the field's size. Waits for the
    // GPU. Direct3D 12 only.
    virtual bool ReadFlow(std::size_t /*view*/, std::vector<float>& /*xy*/, FrameGenExtent& /*cells*/) { return false; }
    // Research: one of `view`'s working textures as the last Execute left
    // it ("newest" names what that Execute wrote), as floats, `channels` per
    // texel row by row (packed half pairs, such as the eye-shift map, as
    // two). Waits for the GPU. Direct3D 12 only.
    virtual bool ReadTexture(std::size_t /*view*/, FrameGenTexture /*texture*/, std::vector<float>& /*values*/,
                             FrameGenExtent& /*extent*/, std::uint32_t& /*channels*/) {
        return false;
    }
    // Research: the next Execute also writes what it generates from into
    // `directory` (which must exist): each view's newest image and linear
    // depth before any trust decision, as raw texels, and frame.txt with the
    // requests and tuning, for replaying real frames offline
    // (`depthxr_framegen_tests replay`). That Execute waits for the GPU.
    // Direct3D 12 only.
    virtual bool DumpNextExecute(const std::string& /*directory*/) { return false; }
    // Tests and research only: Execute copies a view's image itself when
    // Capture did not, though by then the application has released it to
    // the runtime. Off (the default), such a frame is not generated from
    // (FrameGenExecuteResult::uncaptured).
    virtual void SetCopyAfterRelease(bool /*on*/) {}
    // Records and submits generation for every view on the session's graphics
    // queue from the images Capture copied at release. Never waits on the
    // CPU for this submission.
    virtual bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                         FrameGenExecuteResult& result, std::string& error) = 0;
    // Blocks until every submitted command list has finished; required before
    // freeing anything a recent submission may still use. False when the GPU
    // did not finish in time: callers must then keep resources alive.
    virtual bool WaitIdle() = 0;
    // Forget history (discontinuity); textures stay allocated.
    virtual void ResetHistory() = 0;
    // Teardown only: waits for outstanding work and frees every resource.
    virtual void Reset() = 0;
    // Most recent GPU duration of a whole submission, or a negative value.
    virtual double LastGpuMilliseconds() const { return -1.0; }
    // When the newest submission whose timing has been read (a few
    // submissions after it) started and ended on the GPU.
    virtual std::optional<FrameGenGpuWindow> LastGpuWindow() const { return std::nullopt; }
    // The process's local video memory use (the application's included) and
    // the OS budget for it; absent when the backend cannot query it.
    virtual std::optional<FrameGenMemoryBudget> MemoryBudget() const { return std::nullopt; }
    // Video memory of the backend's own images for all views (history, flow,
    // depth, trust, eye-shift maps), as allocated; 0 when unknown. Output
    // images belong to the runtime and are not included.
    virtual std::uint64_t WorkingMemoryBytes() const { return 0; }
    // The runtime may then be called from a presenter thread while the
    // application renders: the API's queue is free-threaded (Direct3D 12), so
    // runtime work on it cannot race the application's own submissions.
    virtual bool SupportsThreadedPresentation() const { return false; }
    // Just-in-time synthesis (Direct3D 12): Execute with
    // FrameGenTuning::analyze_only measures a real frame's motion once, and
    // Synthesize then writes any generated frame from it, one at a time,
    // right before the refresh that shows it. Generated frames need one
    // output image per view instead of one per generated frame, and a frame
    // the next real one is late for is made when that is known.
    virtual bool SupportsJustInTime() const { return false; }
    // Writes one generated frame per view from analyzed real frame `frame`
    // (FrameGenExecuteResult::frame) at `fraction` in `mode`: between it and
    // the real frame before it, or past it (from a frame analyzed for
    // interpolation, with FrameGenTuning::cover). `outputs` are acquired and
    // waited output images, one per view, in Execute's view order. False
    // when the frame's images have since been replaced by newer captures
    // (the last kFrameGenHistoryRingMax real frames are kept) or the GPU
    // failed. Never waits on the CPU; the session's queue waits for the
    // frame on the GPU, so the outputs can be released right after.
    virtual bool Synthesize(std::uint64_t /*frame*/, FrameGenSynthesisMode /*mode*/, float /*fraction*/,
                            const std::vector<FrameGenImage>& /*outputs*/, std::string& error) {
        error = "this graphics API cannot write generated frames one at a time";
        return false;
    }
};

// Direct3D 12 queue substitution (FrameGenQueueSubstitution): the runtime was
// given this queue in place of the game's. Chained ahead of the game's own
// graphics binding in the chain CreateFrameGenGpu is given (never passed to
// the runtime): Frame Generation's work then runs on it, so frames it writes
// are synchronized with the runtime as soon as they are written.
inline constexpr XrStructureType kFrameGenRuntimeQueueType = static_cast<XrStructureType>(0x56584651);
struct FrameGenRuntimeQueue {
    XrStructureType type{kFrameGenRuntimeQueueType};
    const void* next{nullptr};
    void* d3d12_queue{nullptr}; // ID3D12CommandQueue*
};

// Builds the backend matching the session's graphics binding (D3D11, D3D12
// or Vulkan). Returns nullptr with a reason for other APIs or failures.
std::unique_ptr<FrameGenGpu> CreateFrameGenGpu(const void* binding_chain, std::string& error);

namespace framegen_shaders {
struct Blob {
    const void* data;
    std::size_t size;
};
// Indexed by FrameGenPass. SPIR-V blobs are empty when the build had no
// SPIR-V compiler; the Vulkan backend then reports itself unavailable.
extern const std::array<Blob, kFrameGenPassCount> kDxbc;
extern const std::array<Blob, kFrameGenPassCount> kSpirv;
// Synthesis as a draw (Direct3D 12): the vertex shader, then the pixel
// shader writing 1, 2 and 3 generated frames.
inline constexpr std::size_t kDrawShaderCount = 1 + kFrameGenMaxGenerated;
extern const std::array<Blob, kDrawShaderCount> kDxbcDraw;
} // namespace framegen_shaders

} // namespace depthxr
