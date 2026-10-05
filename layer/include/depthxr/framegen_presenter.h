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
    std::uint32_t multiplier{2}; // headset frames per real frame in use
    bool automatic{false};       // the multiplier is chosen automatically
    bool full_rate{false};       // automatic and the app holds full refresh
    bool threaded{false};        // presenting from the presenter thread
};

// Frame Generation pacing for one OpenXR session.
//
// While engaged at multiplier N, every application frame owns N runtime
// frames. The app's xrWaitFrame takes the runtime's slot k but reports slot
// k + N - 1 with an N-fold period, so the app renders for the time its frame
// is really shown. Inside the app's xrEndFrame the presenter submits slots k
// to k + N - 2 with generated frames (interpolated between the previous and
// newest real frames, or extrapolated past the previous real frame), each
// after one extra runtime wait/begin, then slot k + N - 1 with the app's own
// layers. The app is thereby held at 1/N of the runtime rate. Consecutive
// submissions are kept most of a refresh apart for runtimes that do not
// pace xrWaitFrame themselves.
//
// Runtime frame calls must never overlap, so a wait arriving while an engaged
// frame is in progress is held until that frame has ended. An engaged frame
// whose end has not started yet can instead be demoted to a single
// submission: immediately when the same thread waits again before ending it,
// or after a bounded hold when another thread does.
//
// That inline cycle holds the application's frame thread for the generated
// slots, so a game whose frame takes longer than the runtime's lead time, or
// that waits for its next frame before ending the current one (MSFS), loses
// frame rate instead of gaining it. Where the graphics queue is free-threaded
// (Direct3D 12) and the mode is Extrapolate, a presenter thread instead owns
// the runtime frame loop: one runtime wait/begin/end per refresh, filled with
// extrapolated frames, while the application's waits are paced to every Nth
// refresh without reaching the runtime. Each real frame is submitted, from
// its own xrEndFrame, in the first slot at or after the one it was predicted
// for; the frames generated from it fill the slots until the next one.
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
    // Timing of the automatic multiplier (tests shorten it): how long a change
    // settles before it is judged, how long the app must hold its cadence
    // before probing one step down, how long a probe must hold, and the first
    // wait between probes (doubling after each failed probe).
    void SetAutoPolicy(std::chrono::milliseconds settle, std::chrono::milliseconds steady,
                       std::chrono::milliseconds probe_confirm, std::chrono::milliseconds probe_backoff);
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
    // Call before forwarding xrDestroySpace: nothing the presenter thread
    // repeats may refer to the space afterwards.
    void ForgetSpace(XrSpace space);
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
        bool depth{false};            // a depth swapchain
        bool depth_capturable{false}; // created with the usage depth capture needs
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
        bool begun{false};
        bool engaged{false};
        bool demoted{false}; // falls back to one submission in slot k
        bool claimed{false}; // its end has started; it will run its extra frames
        std::uint32_t multiplier{2};
        XrDuration reported_period{0}; // the period the app was given
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
        std::uint32_t image_count{0};
    };

    // Generated projection views waiting for their slots (extrapolation),
    // one set of views per generated frame.
    struct Generated {
        bool valid{false};
        XrSpace space{XR_NULL_HANDLE};
        XrCompositionLayerFlags flags{0};
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
    };

    // A deep copy of a frame's composition layers that outlives the app's
    // xrEndFrame, for slots the presenter thread fills between real frames.
    struct FrameCopy {
        struct Layer {
            XrStructureType type{XR_TYPE_UNKNOWN};
            XrCompositionLayerProjection projection{};
            std::vector<XrCompositionLayerProjectionView> views;
            std::vector<XrCompositionLayerDepthInfoKHR> depth;
            std::vector<std::uint8_t> has_depth;
            XrCompositionLayerQuad quad{};
            XrCompositionLayerCylinderKHR cylinder{};
            XrCompositionLayerEquirectKHR equirect{};
            XrCompositionLayerEquirect2KHR equirect2{};
            XrCompositionLayerCubeKHR cube{};
        };
        bool valid{false};
        bool has_projection{false};
        bool projection_stale{false}; // its swapchain was destroyed: never repeat it
        std::uint32_t projection_index{0};
        XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        std::vector<Layer> layers;
    };

    // An application frame the presenter thread presents.
    struct ThreadedFrame {
        std::uint64_t id{0};
        XrTime predicted{0};           // the display time returned to the app
        XrTime target_time{0};         // the display time of target_slot
        XrDuration reported_period{0}; // the period returned to the app
        std::uint64_t target_slot{0};  // the slot the frame is predicted to reach
        std::uint32_t multiplier{2};
        std::uint32_t lead{0};         // refreshes between its release and target_slot
        bool begun{false};
        bool claimed{false}; // its end has started
        std::chrono::steady_clock::time_point wait_return{};
    };

    struct SlotRequest {
        std::uint64_t frame_id{0};
        std::uint64_t earliest_slot{0};
    };

    struct SlotOffer {
        std::uint64_t frame_id{0};
        std::uint64_t slot{0};
        XrTime time{0};
        bool should_render{true};
    };

    // A projection view's source region, captured when its swapchain releases.
    struct CaptureTarget {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::uint32_t array_index{0};
        XrRect2Di rect{};
        std::size_t view{0};
        // A view's depth image (XrCompositionLayerDepthInfoKHR) rather than
        // its color image.
        bool depth{false};
        FrameGenDepthEncoding encoding;
        FrameGenExtent color_extent; // the color view the depth belongs to
    };

    bool WantsEngagementLocked() const;
    bool RunnableLocked() const;
    std::uint32_t EffectiveMultiplierLocked() const;
    void UpdateAutoLocked(std::chrono::steady_clock::time_point now, const std::optional<PendingFrame>& frame,
                          bool started_late = false);
    void MaybeRetryLocked();
    void PauseLocked(const std::string& reason);
    void UnavailableLocked(const std::string& reason);
    void SetStateLocked(FrameGenState state, const std::string& reason);
    void RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection);
    std::optional<FrameGenOutputKind> OutputKindLocked(std::size_t view, std::int64_t source_format) const;

    XrResult SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time, const FrameGenRuntimeCalls& calls);
    void NotePhaseLocked(std::chrono::steady_clock::time_point called, std::chrono::steady_clock::time_point returned,
                         XrTime display_time, XrDuration period);
    // Returns true when the slot was already more than half a refresh past
    // the time the runtime would have released its wait.
    bool PaceSubmission(XrTime display_time, XrDuration period);
    XrResult NextSlot(XrSession session, const FrameGenRuntimeCalls& calls, XrDuration period, XrFrameState& state);
    XrResult RunCycle(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                      const FrameGenRuntimeCalls& calls);
    // Generates one frame per fraction into the output swapchain sets from
    // the newest real frame. On success `sets` holds the generated
    // projection views, one set per fraction.
    bool Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                  const std::vector<float>& fractions,
                  std::vector<std::vector<XrCompositionLayerProjectionView>>& sets);
    bool EnsureOutputs(const XrCompositionLayerProjection& projection, const std::vector<FrameGenOutputKind>& kinds,
                       std::size_t sets, std::string& error);
    // Acquires and waits the images of the first `sets` output sets.
    bool AcquireOutputs(std::size_t sets, std::string& error, bool& timed_out);
    void DestroyOutputs();
    void ReleaseOutputs();
    void RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time, std::uint64_t sequence);
    void InvalidateContinuityLocked();
    void NoteFailureLocked(const std::string& error);
    void NoteSubmissionLocked();

    // Threaded presentation.
    bool ThreadedEligibleLocked() const;
    // Running threaded presentation continues while this holds; unlike
    // engagement it does not need a fresh run of steady frames.
    bool StayThreadedLocked() const;
    void DropUnclaimedLocked();
    void UpdateLeadLocked(bool late, std::optional<double> slack_ms, const ThreadedFrame& frame);
    // Wakes the presenter thread and app threads waiting on slots.
    void WakeSlots();
    // Every runtime frame call goes through these, so the presenter knows the
    // runtime's frame state (waited, begun, ended) whatever its own frame
    // bookkeeping says, and never makes a call out of order.
    XrResult RuntimeWait(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameWaitInfo* info,
                         XrFrameState* state);
    XrResult RuntimeBegin(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameBeginInfo* info);
    XrResult RuntimeEnd(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameEndInfo* info);
    bool RuntimeFrameOpenLocked() const { return runtime_ends_ < runtime_begins_; }
    bool RuntimeWaitPendingLocked() const { return runtime_waits_ > runtime_begins_; }
    // Some apps end a frame with a display time other than the one they were
    // given (MSFS adds one period and renders for that). The offset is
    // measured in periods and taken off what engaged frames are told, so the
    // content is rendered for when it is shown.
    void NoteDisplayOffsetLocked(XrTime submitted, XrTime returned, XrDuration period);
    XrDuration DisplayOffsetLocked(XrDuration period) const;
    static void StripSpace(FrameCopy& copy, XrSpace space);
    // Presenter thread only: waits until `ready` or the deadline with the
    // high-resolution timer (condition variables round to the system tick).
    bool CompositorWait(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline,
                        const std::function<bool()>& ready);
    // Removes a destroyed swapchain from a copy so it is never submitted.
    static void StripSwapchain(FrameCopy& copy, XrSwapchain swapchain);
    // Returns nothing when the presenter thread is not running (the caller
    // then takes the inline path).
    std::optional<XrResult> ThreadedWaitLocked(std::unique_lock<std::mutex>& lock, XrFrameState* state,
                                               const FrameGenRuntimeCalls& calls);
    XrResult ThreadedEnd(XrSession session, const XrFrameEndInfo& info, const FrameGenRuntimeCalls& calls,
                         const ThreadedFrame& frame, std::chrono::steady_clock::time_point entered);
    void StartCompositorLocked(const FrameGenRuntimeCalls& calls);
    // Lets the presenter thread present every frame in flight, then stops it.
    void ExitThreadedLocked(std::unique_lock<std::mutex>& lock);
    // Stops the presenter thread at its next slot; frames still in flight are
    // dropped.
    void StopCompositorLocked(std::unique_lock<std::mutex>& lock);
    void JoinCompositorLocked(std::unique_lock<std::mutex>& lock);
    void CompositorLoop();
    static bool CopyFrame(const XrFrameEndInfo& info, FrameCopy& copy, std::string& unsupported);
    // Points `pointers` at `copy`'s layers, replacing its primary projection's
    // views with `generated` when given.
    static XrFrameEndInfo BuildFrame(FrameCopy& copy, const std::vector<XrCompositionLayerProjectionView>* generated,
                                     XrSpace generated_space, XrCompositionLayerFlags generated_flags,
                                     std::vector<const XrCompositionLayerBaseHeader*>& pointers);

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
    std::chrono::milliseconds auto_settle_{2000};
    std::chrono::milliseconds auto_steady_{5000};
    std::chrono::milliseconds auto_probe_confirm_{15000};
    std::chrono::milliseconds auto_initial_backoff_{10000};
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
    std::vector<std::vector<Output>> output_sets_; // [generated frame][view]
    Generated extrapolated_;
    std::chrono::steady_clock::time_point last_runtime_submit_{};
    XrDuration last_period_{0};
    // The runtime's own cadence: when a runtime wait blocked and returned for
    // display time phase_xr_ at phase_wall_, the wait for display time t
    // would return at phase_wall_ + (t - phase_xr_).
    std::chrono::steady_clock::time_point phase_wall_{};
    XrTime phase_xr_{0};

    // Automatic multiplier: steps up while the app misses its cadence, probes
    // one step down after a steady stretch, backing off when a probe fails.
    std::uint32_t auto_multiplier_{1}; // 1 = full rate, no generation; Auto starts there
    bool full_rate_{false};
    struct Cadence {
        std::chrono::steady_clock::time_point time;
        bool late{false};
        bool started_late{false}; // inline: the cycle's first slot was already past due
    };
    std::deque<Cadence> cadence_;
    std::optional<std::chrono::steady_clock::time_point> last_app_end_;
    std::chrono::steady_clock::time_point multiplier_changed_at_{};
    std::chrono::steady_clock::time_point probe_not_before_{};
    std::chrono::milliseconds probe_backoff_{10000};
    bool probing_{false};
    std::uint32_t probe_from_{0};
    std::size_t auto_late_percent_{0}; // for the log

    std::uint64_t generated_{0};
    std::uint64_t repeated_{0};
    std::uint64_t failures_{0};
    std::uint64_t demoted_{0};
    std::uint64_t captured_{0};
    mutable std::deque<std::chrono::steady_clock::time_point> app_frames_;
    mutable std::deque<std::chrono::steady_clock::time_point> submissions_;
    std::chrono::steady_clock::time_point last_summary_{};
    // Per summary window: time the app spends between its wait returning and
    // its end, and engaged cycles whose first slot was already late.
    double app_work_ms_{0.0};
    std::uint64_t app_work_frames_{0};
    std::uint64_t cycles_{0};
    std::uint64_t late_cycles_{0};
    bool last_cycle_started_late_{false};
    std::optional<std::chrono::steady_clock::time_point> inline_hold_until_; // auto stays at full rate
    bool logged_first_generation_{false};
    bool logged_depth_{false};
    bool logged_capture_failure_{false};
    void* timer_{nullptr}; // high-resolution waitable timer, frame thread only

    // Threaded presentation. The presenter thread owns the runtime frame loop
    // while compositor_running_; app threads hand it real frames through
    // slot_request_/slot_offer_. Lock order: submit_mutex_, queue,
    // gpu_mutex_, mutex_. GPU work never holds mutex_, which the presenter
    // thread needs every refresh.
    std::thread compositor_;
    void* compositor_timer_{nullptr};
    void* compositor_wake_{nullptr}; // auto-reset event behind slot_cv_ for the presenter thread
    FrameGenRuntimeCalls compositor_calls_;
    bool compositor_running_{false};
    bool compositor_stop_{false};  // close the current slot and exit
    bool compositor_drain_{false}; // exit once no application frame is in flight
    bool threaded_disabled_{false};
    bool logged_threaded_{false};
    std::condition_variable slot_cv_;
    std::uint64_t slot_index_{0}; // runtime slots the presenter thread has waited
    XrTime slot_time_{0};
    XrDuration slot_period_{0};
    bool slot_should_render_{true};
    std::deque<ThreadedFrame> threaded_frames_;
    std::uint64_t threaded_id_{1ull << 48}; // apart from inline sequences, so continuity never spans modes
    std::optional<std::uint64_t> last_release_slot_;
    double app_latency_ms_{0.0}; // smoothed time from a wait returning to its end (log)
    // Refreshes between a wait's release and the slot its frame is predicted
    // for: one more after two late frames among the last ten, one fewer
    // after seven of ten arriving more than a refresh before their deadline,
    // so a single hitch does not add latency for good. Only frames
    // predicted with the current lead move it, so frames in flight do not
    // move it twice. An early frame is held until its slot, which delays the
    // app's next frame; a lead that is too long therefore slows the app, not
    // just adds latency.
    std::uint32_t lead_slots_{0};
    std::deque<std::uint8_t> lead_history_; // last frames at the current lead: 0 on time, 1 late, 2 slack
    std::uint32_t dropped_unbegun_{0}; // dropped frames whose begin is still to come
    std::uint32_t dropped_unended_{0}; // dropped frames whose end is still to come
    // Runtime frame calls that succeeded this session.
    std::uint64_t runtime_waits_{0};
    std::uint64_t runtime_begins_{0};
    std::uint64_t runtime_ends_{0};
    bool logged_order_guard_{false};
    double display_offset_periods_{0.0};
    std::uint32_t display_offset_samples_{0};
    const char* compositor_phase_{"stopped"}; // for the watchdog log
    bool logged_watchdog_{false};
    bool logged_swallowed_{false};
    std::optional<SlotRequest> slot_request_;
    std::optional<SlotOffer> slot_offer_;
    bool offer_done_{false};
    FrameCopy newest_copy_;              // the newest real frame's layers
    FrameCopy passthrough_copy_;         // the last frame passed straight through
    bool generation_pending_{false};     // frames are being generated from newest_copy_
    bool generated_ready_{false};        // extrapolated_ belongs to newest_copy_
    std::size_t next_generated_set_{0};
    std::uint64_t threaded_late_{0};     // real frames shown after their predicted slot
    std::mutex submit_mutex_;            // presenter-thread submissions vs output swapchain calls
    std::mutex gpu_mutex_;               // the GPU backend (Track, Capture, Execute, WaitIdle, Forget, Reset)
    double gpu_ms_{-1.0};                // last generation GPU time, read without the backend
};

} // namespace depthxr
