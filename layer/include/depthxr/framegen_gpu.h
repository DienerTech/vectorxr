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
    // Per view (Direct3D 12, from FrameGenEngineMode::kAssist): the game's
    // motion vectors were merged into the flow (its kEngineLabels written).
    std::vector<bool> merged;
    // Direct3D 12, FrameGenEngineMode::kLead (stage 6): the game's vectors led
    // the frame on every view (FrameGenEngineLeads): its measured passes were
    // predicated on its verdict, and the optical flow hardware left idle.
    bool lead{false};
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

// One analysis (Execute with FrameGenTuning::analyze_only) on the GPU, on
// std::chrono::steady_clock ({} unknown).
struct FrameGenAnalysisTiming {
    std::uint64_t submission{0};                            // FrameGenExecuteResult::submission
    std::chrono::steady_clock::time_point capture_end{};    // the newest capture it waited for had run
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point hardware_start{}; // waiting for the optical flow hardware ({} without it)
    std::chrono::steady_clock::time_point hardware_end{};
    std::chrono::steady_clock::time_point end{};
    double hardware_call_ms{0.0}; // CPU time the optical flow hardware's execute calls took
};

struct FrameGenMemoryBudget {
    std::uint64_t usage_bytes{0};
    std::uint64_t budget_bytes{0};
};

// Engine motion vectors (FrameGenGpu::CaptureMotion): one view's motion
// vector images to read for a frame.
struct FrameGenMotionJob {
    std::size_t view{0};
    // The swapchain and region the game named (XR_FB_space_warp,
    // XR_EXT_frame_synthesis); image_index is the image the frame's vectors
    // are thought to be in (FrameGenEngineImageTracker), which a dump copies
    // without `census_all`'s.
    FrameGenImage vectors;
    std::uint32_t image_count{0}; // 0: the swapchain's own, up to kFrameGenEngineImages
    std::optional<std::uint32_t> released; // a game following the specification released this one
    // The images (bits) the game acquired and has not released yet: in a
    // state of its own until their release, so neither read nor moved out of
    // it (their census says they were not read). None for a game that never
    // acquires its motion vector images (MSFS 2024).
    std::uint32_t acquired{0};
    std::uint32_t color_index{0};          // the images the frame's color and depth swapchains released
    std::uint32_t depth_index{0};
    std::uint64_t frame{0};                // the layer's count of frames with a projection layer
    FrameGenMotionEncoding encoding;
    bool informative{false};
    // Stage 0: every image is fingerprinted, and the sparse samples taken.
    // Without census_all only vectors.image_index is.
    bool census_all{false};
    bool samples{false};
    // Taken when the frame's depth was released, before its xrEndFrame:
    // whether the images are written by then (census only).
    bool early{false};
    // Stage 2: vectors.image_index is also resampled to the view's
    // half-resolution grid (kEngineCapture), paired with the view's color
    // capture: its latest, or with `pair_next` (the game released its vectors
    // before its color this frame) the next one.
    bool capture{false};
    bool pair_next{false};
};

// The census's grids: the hash grid, 64 x 64 texels at fixed places of the
// whole image, and the statistics grid, 32 x 32 texels inside the region.
inline constexpr std::uint32_t kFrameGenEngineHashTexels = 64 * 64;
inline constexpr std::uint32_t kFrameGenEngineStatsTexels = 32 * 32;

// One image's census (kEngineCensus, see FG_PASS_ENGINE_CENSUS).
struct FrameGenEngineImageCensus {
    std::uint32_t xor_hash{0};
    std::uint32_t sum_hash{0};
    std::uint32_t zeros{0};     // exact zero vectors on the hash grid (of kFrameGenEngineHashTexels)
    std::uint32_t non_finite{0}; // ... not finite
    std::uint32_t large{0};      // longer than 4 on the statistics grid (of kFrameGenEngineStatsTexels)
    float sum_abs{0.0f};
    float max_abs{0.0f};
    std::uint32_t finite{0};     // finite on the statistics grid
    std::array<float, 4> sums{}; // r, g, b, a summed over those
    std::uint32_t tag{0};
    std::uint32_t image{0};
    std::uint32_t rect_width{0};
    std::uint32_t rect_height{0};
    std::uint64_t Hash() const { return (static_cast<std::uint64_t>(xor_hash) << 32) | sum_hash; }
};

// What a job's census found, once read back (a few frames later).
struct FrameGenEngineCensus {
    std::uint64_t sequence{0}; // in the order read, from 1
    std::uint64_t frame{0};
    std::size_t view{0};
    bool early{false};
    std::uint32_t images{0};
    std::uint32_t censused{~0u}; // the images read (bits): `image` holds only theirs
    std::uint32_t color_index{0};
    std::uint32_t depth_index{0};
    std::uint64_t handle{0}; // the motion vector swapchain
    std::array<std::int32_t, 4> rect{};
    std::array<FrameGenEngineImageCensus, kFrameGenEngineImages> image{};
    // 16x16 samples of 4 texels: the three images' raw texels, then the
    // linear depth, the color image index and the half-resolution texel.
    std::vector<std::array<float, 4>> samples;
    double gpu_ms{-1.0}; // the census submission's time on the GPU (stage 2: its capture's too)
};

// The backend's engine motion state: counts, and the censuses read back
// lately (the newest kept few; callers take those past the last sequence
// they saw).
struct FrameGenEngineStatus {
    std::uint64_t submitted{0};
    std::uint64_t busy{0};        // calls skipped: every motion slot still on the GPU (never waited for)
    std::uint64_t refused{0};     // images that cannot be read (multisampled, integer, not shader-readable)
    std::uint64_t failed{0};
    std::uint64_t dumped{0};      // frames whose images were copied for a research capture
    // Stage 2: views' vectors resampled (kEngineCapture), in how many
    // dispatches (one for eyes of one size); views whose capture had the
    // game's queue wait for the analysis that last read the field it writes
    // (the CPU saw that analysis unfinished); views left out (not set up yet,
    // or no color capture to pair with); and views analysed with their own
    // frame's field (what a later stage reads).
    std::uint64_t captured{0};
    std::uint64_t dispatches{0};
    std::uint64_t waits{0};
    std::uint64_t unready{0};
    std::uint64_t paired{0};
    // Stage 3: frames whose evidence was taken (every view's), and what the
    // validator makes of it.
    std::uint64_t evidence{0};
    FrameGenEngineValidation validation;
    bool stopped{false};          // the device was removed: nothing more is read
    // Each motion vector image's description (resource, heap), once each,
    // and debug-layer messages about their states (VECTORXR_FRAMEGEN_ENGINE_MV_DEBUG).
    std::vector<std::string> notes;
    std::vector<FrameGenEngineCensus> census;
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
    // Engine motion vectors (Direct3D 12): reads each job's motion vector
    // images on the game's queue, as they are at the call (a game may never
    // release them; those it holds acquired are left alone), and keeps what
    // the census finds for EngineStatus a few frames later. `color`
    // describes each view's color image, by view.
    // Jobs with `capture` (stage 2) also resample their image into the
    // view's raw field (kEngineRaw), once Execute set the view up with
    // FrameGenTuning::engine_mode from kCapture; `resampled`, when given,
    // says per job whether it was (a view not set up, or with no color
    // capture to pair with, is left out while its census is still taken).
    // From kObserve, Execute takes the evidence on the fields
    // (FrameGenViewRequest::engine_motion) and judges their convention
    // (FrameGenEngineValidator, in EngineStatus); a validator that disabled
    // them leaves every view out.
    // Never waits: with every motion slot still on the GPU the call is
    // skipped (false, an empty error).
    virtual bool SupportsEngineMotion() const { return false; }
    virtual bool CaptureMotion(const std::vector<FrameGenMotionJob>& /*jobs*/,
                               const std::vector<FrameGenViewRequest>& /*color*/, std::string& error,
                               std::vector<bool>* resampled = nullptr) {
        error.clear();
        if (resampled) resampled->clear();
        return false;
    }
    // With the censuses read back since sequence `after` (FrameGenEngineCensus::sequence).
    virtual FrameGenEngineStatus EngineStatus(std::uint64_t /*after*/ = 0) const { return {}; }
    // Stage 3: the convention the game is expected to use
    // (FrameGenEnginePriorFor), for the validator (one batch adopts it).
    virtual void SetEnginePrior(const FrameGenEngineConvention& /*convention*/) {}
    // Research: the next CaptureMotion (not an early one) also copies the
    // images into `directory` (which must exist), every image or only each
    // job's vectors.image_index; files are written after the GPU, by a
    // thread of their own.
    virtual bool DumpMotion(const std::string& /*directory*/, bool /*all_images*/) { return false; }
    // How the game's depth is being read after checking it against the
    // other eye, with the evidence; counts decisions so callers log each once.
    virtual FrameGenDepthStatus DepthStatus() const { return {}; }
    // Stage 4: the world's motion from the application's movement
    // (VECTORXR_FRAMEGEN_ENGINE_DELTA; mode off where the backend has none):
    // its calibration against the world fit, with decisions counted so
    // callers log each once.
    virtual FrameGenWorldDeltaStatus WorldDeltaStatus() const { return {}; }
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
    // Tests: the draws of generated frames into the output images recorded
    // so far (DrawsOutputs; frames drawn together count once).
    virtual std::uint64_t DrawsForTest() const { return 0; }
    // FrameGenTuning::hardware_flow: empty until asked for, then "active",
    // or why the hardware cannot be used (block matching measures motion).
    virtual std::string HardwareFlowStatus() const { return "this graphics API has no optical flow hardware support"; }
    // Tests: the next optical flow hardware work is never handed to the
    // hardware, as if it had stopped responding.
    virtual void StallHardwareForTest() {}
    // Tests: fail the ordinal-th following working-texture allocation once
    // (one-based; zero cancels). Direct3D 12 only.
    virtual void FailAllocationForTest(std::uint32_t /*ordinal*/) {}
    // Several views match on one optical flow atlas (the default) or on a
    // session each (tests, research; VECTORXR_FRAMEGEN_HARDWARE_ATLAS=0).
    virtual void SetHardwareAtlas(bool /*on*/) {}
    // Tests: the last analysis used the optional multi-view hardware atlas,
    // rather than the supported per-view fallback.
    virtual bool UsesHardwareAtlasForTest() const { return false; }
    // Tests: the engine motion vectors' convention locked from now on
    // (FrameGenEngineValidator::LockForTest). Direct3D 12 only.
    virtual void LockEngineForTest(const FrameGenEngineConvention& /*convention*/) {}
    // Tests and replays: the depth's reading, swap and scale locked from now
    // on (FrameGenDepthValidator::LockForTest). Direct3D 12 only.
    virtual void LockDepthForTest(FrameGenDepthReading /*reading*/, bool /*swapped*/, float /*scale*/) {}
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
    // Timings of analyses the GPU has finished since the last call, oldest
    // first; never waits. Under the caller's GPU lock.
    virtual std::vector<FrameGenAnalysisTiming> TakeTimings() { return {}; }
    // The process's local video memory use (the application's included) and
    // the OS budget for it; absent when the backend cannot query it.
    virtual std::optional<FrameGenMemoryBudget> MemoryBudget() const { return std::nullopt; }
    // Video memory of the backend's own images for all views (history, flow,
    // depth, trust, eye-shift maps, active and cached optical-flow atlases),
    // as allocated; 0 when unknown. Output images belong to the runtime and
    // are not included. Driver-retained resources detached during teardown
    // are reflected in MemoryBudget's process usage, not this live-image sum.
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
    // (the last FrameGenHistoryRing real frames are kept) or the GPU
    // failed. Never waits on the CPU; the session's queue waits for the
    // frame on the GPU, so the outputs can be released right after.
    virtual bool Synthesize(std::uint64_t /*frame*/, FrameGenSynthesisMode /*mode*/, float /*fraction*/,
                            const std::vector<FrameGenImage>& /*outputs*/, std::string& error) {
        error = "this graphics API cannot write generated frames one at a time";
        return false;
    }
    // Extrapolating just in time, a real frame whose own motion is not
    // measured yet, moved by the previous real frame's (`motion`, analyzed
    // right before it and measured): written like Synthesize from the
    // frame's images and `motion`'s flow (read where the content was in
    // that frame). Where the frame uses depth, it is shown from where the
    // eye is at its time, as Synthesize's are, by `motion`'s resolved depth.
    // Needs only the frame captured; false (nothing written) when either
    // frame's images are gone, `motion` is not the frame's previous one or
    // is not measured, a view using depth has none of `motion`'s, or the
    // backend cannot.
    virtual bool SynthesizeStale(std::uint64_t /*frame*/, std::uint64_t /*motion*/, float /*fraction*/,
                                 const std::vector<FrameGenImage>& /*outputs*/, std::string& error) {
        error = "this graphics API cannot move a frame by the previous one's motion";
        return false;
    }
    // SynthesizeStale for quad views composed per eye (SynthesizeComposed).
    virtual bool SynthesizeComposedStale(std::uint64_t /*frame*/, std::uint64_t /*motion*/, float /*fraction*/,
                                         const FrameGenComposition& /*composition*/,
                                         const std::vector<FrameGenImage>& /*outputs*/, std::string& error) {
        error = "this graphics API cannot move a frame by the previous one's motion";
        return false;
    }
    // Quad views: frames written just in time from views of their own
    // (FrameGenComposition), composed per eye.
    virtual bool SupportsComposition() const { return false; }
    // Like Synthesize, but each analyzed view is written at `fraction` into
    // an image of the backend's own (or read as captured, for its exact copy
    // or a view without motion), then each eye of `composition` is composed
    // from its peripheral and focus views into `outputs[eye]` (acquired and
    // waited, composition.output in size). Never waits on the CPU. False as
    // Synthesize.
    virtual bool SynthesizeComposed(std::uint64_t /*frame*/, FrameGenSynthesisMode /*mode*/, float /*fraction*/,
                                    const FrameGenComposition& /*composition*/,
                                    const std::vector<FrameGenImage>& /*outputs*/, std::string& error) {
        error = "this graphics API cannot compose quad views";
        return false;
    }
    // Whether analyzed real frame `frame`'s motion is measured on the GPU
    // by now: frames moved by it written before then wait for it, and with
    // them anything submitted after them to the same queue. (Its exact copy
    // only needs its capture.)
    virtual bool Measured(std::uint64_t /*frame*/) const { return true; }
    // Whether its images' capture has run on the GPU by now: its exact copy
    // written before then waits for that (and the game's frame before it).
    virtual bool Captured(std::uint64_t /*frame*/) const { return true; }
    // Whether Synthesize/SynthesizeComposed can still write analyzed real
    // frame `frame`: as its exact copy (`exact`, its newest images), or moved
    // (its previous images too), before newer captures replace them in the
    // history ring. Never waits; under the caller's GPU lock.
    virtual bool Writable(std::uint64_t /*frame*/, bool /*exact*/) const { return true; }
    // GPU-aware pacing: identifies every capture submitted so far (0: none).
    // Under the caller's GPU lock, after Capture.
    virtual std::uint64_t CaptureMark() const { return 0; }
    // Whether every capture up to `mark` has run on the GPU. Thread-safe:
    // callable without the caller's GPU lock, from any thread, while the
    // backend exists (not during Reset).
    virtual bool CaptureReached(std::uint64_t /*mark*/) const { return true; }
    // Blocks the calling thread until CaptureReached(mark) or `timeout`;
    // false on timeout. Thread-safe like CaptureReached; one caller at a time.
    virtual bool WaitCapture(std::uint64_t /*mark*/, std::chrono::milliseconds /*timeout*/) const { return true; }
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
// Synthesis as a draw (Direct3D 12): the vertex shader, the pixel shader
// writing 1 to kFrameGenMaxGenerated generated frames, then the quad views
// compose pixel shader (empty while the build has none).
inline constexpr std::size_t kDrawShaderCount = 2 + kFrameGenMaxGenerated;
inline constexpr std::size_t kComposeDrawShader = kDrawShaderCount - 1;
extern const std::array<Blob, kDrawShaderCount> kDxbcDraw;
} // namespace framegen_shaders

} // namespace depthxr
