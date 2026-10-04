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
#include <thread>
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
    kOff,         // not enabled for this session, toggled off, or turned off in settings
    kWaiting,     // enabled, waiting for a steady renderable cadence
    kActive,      // generating frames
    kPaused,      // stood down after a runtime, GPU or threading problem; retries itself
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
    std::uint64_t demoted{0};
    std::uint64_t captured{0};
    bool retry_pending{false};
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
// held at exactly half the runtime rate.
//
// Runtime frame calls must never overlap, so a wait arriving while an engaged
// frame is in progress is held until that frame has ended. An engaged frame
// whose end has not started yet can instead be demoted to a single
// submission: immediately when the same thread waits again before ending it,
// or after a bounded hold when another thread does.
class FrameGenPresenter {
  public:
    using GpuFactory = std::function<std::unique_ptr<FrameGenGpu>(const void*, std::string&)>;

    ~FrameGenPresenter();

    // Latches Frame Generation for the session. A failure leaves the
    // presenter Unavailable (it then never engages, and Turbo may run).
    void Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
               Logger* logger, std::mutex* queue_mutex, GpuFactory factory = CreateFrameGenGpu);
    void Stop();
    // Started with a working GPU backend; such a session keeps frame pacing
    // away from Turbo even while toggled off, so toggling never races Turbo.
    bool OwnsPacing() const;
    bool Started() const;

    void Configure(const FrameGenResolvedSettings& settings);
    // Settings can turn a running session off (frames pass through) and back
    // on; only starting Frame Generation needs a new session.
    void SetConfigured(bool enabled);
    // Steady renderable frames and session age required before engaging.
    void SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age);
    // Delay before a paused session retries by itself, and how often.
    void SetRetryPolicy(std::chrono::milliseconds delay, std::uint32_t attempts);
    // In-session A/B toggle; also retries after a pause. Returns the new
    // user-enabled state.
    bool Toggle();
    // Session state changes (begin/end session) drop frames in flight and
    // history continuity.
    void ResetFrames();
    // The layer ended an application frame without forwarding it.
    void AbandonFrame(XrTime display_time);

    XrSwapchainUsageFlags SourceUsage(const XrSwapchainCreateInfo& info) const;
    // `usage` is what the swapchain was actually created with; a source
    // without the backend's capture usage is never copied.
    void TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count,
                        XrSwapchainUsageFlags usage);
    void ForgetSwapchain(XrSwapchain swapchain);
    void NoteRelease(XrSwapchain swapchain, std::uint32_t image_index);
    // Call before forwarding a release: captures the image into history while
    // the application still owns it.
    void CaptureOnRelease(XrSwapchain swapchain, std::uint32_t image_index);

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
        bool capturable{true};
        std::optional<std::uint32_t> released;
    };

    // What each output swapchain holds, readable from release threads.
    struct OutputKinds {
        bool valid{false};
        FrameGenOutputKind requested{FrameGenOutputKind::kSrgb8};
        FrameGenOutputKind kind{FrameGenOutputKind::kSrgb8};
    };

    struct PendingFrame {
        XrTime app_time{0};
        XrTime slot_time{0};
        XrDuration period{0};
        bool should_render{false};
        bool engaged{false};
        bool demoted{false}; // falls back to one submission in slot k
        bool claimed{false}; // its end has started; it will run its extra frame
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
        FrameGenOutputKind requested{FrameGenOutputKind::kSrgb8}; // what the source asked for
        FrameGenOutputKind kind{FrameGenOutputKind::kSrgb8};      // what the swapchain format holds
        bool acquired{false};
        bool waited{false};
        std::uint32_t index{0};
    };

    // Generated projection views waiting for their slot (extrapolation).
    struct Generated {
        bool valid{false};
        XrSpace space{XR_NULL_HANDLE};
        XrCompositionLayerFlags flags{0};
        std::vector<XrCompositionLayerProjectionView> views;
    };

    // A projection view's source region, captured when its swapchain releases.
    struct CaptureTarget {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::uint32_t array_index{0};
        XrRect2Di rect{};
        std::size_t view{0};
    };

    bool WantsEngagementLocked() const;
    void MaybeRetryLocked();
    void PauseLocked(const std::string& reason);
    void UnavailableLocked(const std::string& reason);
    void SetStateLocked(FrameGenState state, const std::string& reason);
    void RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection);
    std::optional<FrameGenOutputKind> OutputKindLocked(std::size_t view, std::int64_t source_format) const;

    XrResult SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time, const FrameGenRuntimeCalls& calls);
    XrResult SubmitSecond(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                          const FrameGenRuntimeCalls& calls);
    XrResult RunPair(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                     const FrameGenRuntimeCalls& calls);
    // Generates into the output swapchains from the newest real frame. On
    // success `views` holds the generated projection views.
    bool Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame, float fraction,
                  std::vector<XrCompositionLayerProjectionView>& views);
    bool EnsureOutputs(const XrCompositionLayerProjection& projection, const std::vector<FrameGenOutputKind>& kinds,
                       std::string& error);
    bool AcquireOutputs(std::string& error, bool& timed_out);
    void DestroyOutputs();
    void ReleaseOutputs();
    void RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time, std::uint64_t sequence);
    void InvalidateContinuityLocked();
    void NoteFailureLocked(const std::string& error);
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
    bool configured_enabled_{true};
    FrameGenState state_{FrameGenState::kOff};
    std::string reason_;
    FrameGenResolvedSettings settings_;
    std::chrono::steady_clock::time_point started_at_{};
    std::uint32_t healthy_streak_{0};
    std::uint32_t engage_streak_frames_{45};
    std::chrono::milliseconds engage_min_age_{3000};
    std::chrono::milliseconds retry_delay_{10000};
    std::uint32_t retry_attempts_{3};
    std::optional<std::chrono::steady_clock::time_point> paused_at_;
    std::uint32_t automatic_retries_{0};
    std::uint32_t consecutive_failures_{0};
    std::uint32_t consecutive_demotions_{0};

    std::unordered_map<XrSwapchain, SwapchainRecord> swapchains_;
    std::vector<std::int64_t> runtime_formats_;
    std::deque<PendingFrame> frames_;
    std::uint64_t wait_sequence_{0};
    XrTime last_returned_time_{0};
    std::vector<CaptureTarget> capture_targets_;
    std::vector<OutputKinds> output_kinds_;
    std::thread::id end_thread_; // the thread that last ended a frame
    bool logged_claimed_wait_{false};

    // Touched by the frame thread inside EndFrame (one at a time) and by
    // Stop; mutex_ guards the parts other threads read.
    RealFrame last_real_;
    std::vector<Output> outputs_;
    Generated extrapolated_;

    std::uint64_t generated_{0};
    std::uint64_t repeated_{0};
    std::uint64_t failures_{0};
    std::uint64_t demoted_{0};
    std::uint64_t captured_{0};
    mutable std::deque<std::chrono::steady_clock::time_point> app_frames_;
    mutable std::deque<std::chrono::steady_clock::time_point> submissions_;
    std::chrono::steady_clock::time_point last_summary_{};
    bool logged_first_generation_{false};
    bool logged_capture_failure_{false};
    void* timer_{nullptr}; // high-resolution waitable timer, frame thread only
};

} // namespace depthxr
