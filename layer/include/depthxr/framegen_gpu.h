#pragma once

#include <array>
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
    // Records and submits generation for every view on the session's graphics
    // queue, copying a view's source first unless Capture already did. Never
    // waits on the CPU for this submission.
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
    // The runtime may then be called from a presenter thread while the
    // application renders: the API's queue is free-threaded (Direct3D 12), so
    // runtime work on it cannot race the application's own submissions.
    virtual bool SupportsThreadedPresentation() const { return false; }
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
inline constexpr std::size_t kDrawShaderCount = 4;
extern const std::array<Blob, kDrawShaderCount> kDxbcDraw;
} // namespace framegen_shaders

} // namespace depthxr
