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
    FrameGenImage output; // acquired and waited image of a layer-owned output swapchain
    FrameGenViewRequest request;
};

struct FrameGenExecuteResult {
    std::vector<bool> synthesized; // per view; false = the newest frame was written unchanged
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
    // skips its own copy for that view when the image matches.
    virtual bool Capture(std::size_t view, const FrameGenImage& source, const FrameGenViewRequest& request,
                         FrameGenFlowQuality quality, std::string& error) = 0;
    // Records and submits generation for every view on the session's graphics
    // queue, copying a view's source first unless Capture already did. Never
    // waits on the CPU for this submission.
    virtual bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                         FrameGenExecuteResult& result, std::string& error) = 0;
    // Blocks until every submitted command list has finished; required before
    // freeing anything a recent submission may still use.
    virtual void WaitIdle() = 0;
    // Forget history (discontinuity); textures stay allocated.
    virtual void ResetHistory() = 0;
    // Teardown only: waits for outstanding work and frees every resource.
    virtual void Reset() = 0;
    // Most recent GPU duration of a whole submission, or a negative value.
    virtual double LastGpuMilliseconds() const { return -1.0; }
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
} // namespace framegen_shaders

} // namespace depthxr
