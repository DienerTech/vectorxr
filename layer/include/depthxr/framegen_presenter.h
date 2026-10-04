#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/settings.h"

namespace depthxr {

class Logger;

// Downstream calls the presenter makes for the frames it submits. `submit`
// is the layer's normal final submission path (OSD overlay included), so
// generated frames carry every layer a real frame would.
struct FrameGenRuntimeCalls {
    std::function<XrResult(XrSession, const XrFrameWaitInfo*, XrFrameState*)> wait;
    std::function<XrResult(XrSession, const XrFrameBeginInfo*)> begin;
    std::function<XrResult(XrSession, const XrFrameEndInfo*)> submit;
};

struct FrameGenSwapchainDispatch {
    PFN_xrEnumerateSwapchainFormats formats{};
    PFN_xrCreateSwapchain create{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
};

enum class FrameGenState {
    kOff,         // not enabled for this session, or toggled off in-session
    kWaiting,     // enabled, waiting for a steady renderable cadence
    kActive,      // generating frames
    kPaused,      // stood down after a runtime or threading problem; the toggle retries
    kUnavailable, // this session cannot generate frames (API, format, GPU setup)
};

const char* ToString(FrameGenState state);

struct FrameGenStatus {
    FrameGenState state{FrameGenState::kOff};
    std::string reason;
    FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
    double app_fps{0.0};
    double output_fps{0.0};
    double gpu_ms{-1.0};
    std::uint64_t generated{0};
    std::uint64_t repeated{0};
    std::uint64_t failures{0};
};

// Frame Generation pacing for one OpenXR session.
//
// While engaged, every application frame owns two runtime frames. The app's
// xrWaitFrame takes the runtime's slot k but reports the following slot,
// k + 1, with a doubled period, so the app renders for the time its frame is
// really shown. Inside the app's xrEndFrame the presenter submits slot k with
// a generated frame (interpolated between the previous and newest real frames,
// or extrapolated past the previous real frame), then runs one extra runtime
// wait/begin/end for slot k + 1 with the app's own layers. The app is thereby
// held at exactly half the runtime rate. An app frame is never begun while
// another is in progress: a wait arriving early is held until the previous
// frame's extra submission has finished.
class FrameGenPresenter {
  public:
    using GpuFactory = std::function<std::unique_ptr<FrameGenGpu>(const void*, std::string&)>;

    ~FrameGenPresenter();

    // Latches Frame Generation for the session. A failure leaves the
    // presenter Unavailable (it then never engages, and Turbo may run).
    void Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
               Logger* logger, std::mutex* queue_mutex, GpuFactory factory = CreateFrameGenGpu);
    void Stop();
    // Started with a working GPU backend; such a session takes frame pacing
    // over from Turbo even while toggled off, so toggling never races Turbo.
    bool OwnsPacing() const;
    bool Started() const;

    void Configure(const FrameGenResolvedSettings& settings);
    // Steady renderable frames and session age required before engaging.
    void SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age);
    // In-session A/B toggle; also retries after a pause. Returns the new
    // user-enabled state.
    bool Toggle();

    XrSwapchainUsageFlags SourceUsage(const XrSwapchainCreateInfo& info) const;
    void TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count);
    void ForgetSwapchain(XrSwapchain swapchain);
    void NoteRelease(XrSwapchain swapchain, std::uint32_t image_index);

    XrResult WaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state, const FrameGenRuntimeCalls& calls);
    XrResult BeginFrame(XrSession session, const XrFrameBeginInfo* info, const FrameGenRuntimeCalls& calls);
    XrResult EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls);

    FrameGenStatus Status() const;
    std::string OsdText() const;
    std::string CompactOsdText() const;

  private:
    struct SwapchainRecord {
        std::int64_t format{0};
        std::uint32_t sample_count{1};
        std::optional<std::uint32_t> released;
    };

    struct PendingFrame {
        XrTime app_time{0};
        XrTime slot_time{0};
        XrDuration period{0};
        bool should_render{false};
        bool engaged{false};
        std::uint64_t sequence{0};
        std::chrono::steady_clock::time_point wait_return{};
    };

    struct ViewRecord {
        FrameGenExtent extent;
        FrameGenViewGeometry geometry;
    };

    struct RealFrame {
        bool valid{false};
        std::uint64_t sequence{0};
        XrTime display_time{0};
        XrSpace space{XR_NULL_HANDLE};
        std::vector<ViewRecord> views;
    };

    struct Output {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        FrameGenExtent extent;
        std::int64_t format{0};
        FrameGenOutputKind kind{FrameGenOutputKind::kSrgb8};
        bool acquired{false};
        std::uint32_t index{0};
    };

    // Generated projection views waiting for their slot (extrapolation).
    struct Generated {
        bool valid{false};
        XrSpace space{XR_NULL_HANDLE};
        XrCompositionLayerFlags flags{0};
        std::vector<XrCompositionLayerProjectionView> views;
    };

    bool WantsEngagementLocked() const;
    void PauseLocked(const std::string& reason);
    void UnavailableLocked(const std::string& reason);
    void SetStateLocked(FrameGenState state, const std::string& reason);
    void NoteCadenceLocked(bool renderable);

    XrResult SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time, const FrameGenRuntimeCalls& calls);
    XrResult SubmitSecond(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                          const FrameGenRuntimeCalls& calls);
    XrResult RunPair(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                     const FrameGenRuntimeCalls& calls);
    // Captures the newest real frame and generates into the output swapchains.
    // On success `views` holds the generated projection views.
    bool Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame, float fraction,
                  std::vector<XrCompositionLayerProjectionView>& views, bool& synthesized);
    bool EnsureOutputs(const XrCompositionLayerProjection& projection, std::vector<FrameGenOutputKind>& kinds,
                       std::string& error);
    void DestroyOutputs();
    void ReleaseOutputs();
    void RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time, std::uint64_t sequence);
    void NoteSubmissionLocked();

    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    XrSession session_{XR_NULL_HANDLE};
    Logger* logger_{nullptr};
    std::mutex* queue_mutex_{nullptr};
    FrameGenSwapchainDispatch xr_{};
    std::unique_ptr<FrameGenGpu> gpu_;
    bool started_{false};
    bool user_enabled_{true};
    FrameGenState state_{FrameGenState::kOff};
    std::string reason_;
    FrameGenResolvedSettings settings_;
    std::chrono::steady_clock::time_point started_at_{};
    std::uint32_t healthy_streak_{0};
    std::uint32_t engage_streak_frames_{45};
    std::chrono::milliseconds engage_min_age_{3000};

    std::unordered_map<XrSwapchain, SwapchainRecord> swapchains_;
    std::vector<std::int64_t> runtime_formats_;
    std::deque<PendingFrame> frames_;
    std::uint64_t wait_sequence_{0};
    XrTime last_returned_time_{0};

    // Touched only by the frame thread inside EndFrame (one at a time).
    RealFrame last_real_;
    std::vector<Output> outputs_;
    Generated extrapolated_;
    std::uint32_t consecutive_failures_{0};

    std::uint64_t generated_{0};
    std::uint64_t repeated_{0};
    std::uint64_t failures_{0};
    mutable std::deque<std::chrono::steady_clock::time_point> app_frames_;
    mutable std::deque<std::chrono::steady_clock::time_point> submissions_;
    std::chrono::steady_clock::time_point last_summary_{};
    bool logged_first_generation_{false};
    void* timer_{nullptr}; // high-resolution waitable timer, frame thread only
};

} // namespace depthxr
