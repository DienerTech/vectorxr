#pragma once

#include <array>
#include <atomic>
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
#include <tuple>
#include <unordered_map>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/framegen_trace.h"
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

// Whether generated frames use the game's depth, and if not, why.
enum class FrameGenDepthState {
    kUnknown,     // no frames generated yet
    kNotSent,     // the game sends no depth with its views
    kNotReadable, // sent, but this graphics API's depth is not read yet (Direct3D 12 only)
    kChecking,    // being checked against the other eye
    kInUse,
    kEmpty,       // sent but holds nothing near (games that only fill it with their own reprojection on)
    kUnusable,    // does not line the eyes up
};

const char* ToString(FrameGenDepthState state); // "unknown", "not-sent", ... (relay names)

struct FrameGenStatus {
    FrameGenState state{FrameGenState::kOff};
    std::string reason;
    FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
    double app_fps{0.0};
    double output_fps{0.0};   // frames submitted to the runtime per second
    // Of those, frames showing something the frame before did not (a new
    // generated or real frame): repeats of the last image when generation
    // or the game ran late are left out.
    double distinct_fps{0.0};
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
    // Dynamic: the app runs at its own rate and generated frames fill the
    // refreshes between; `multiplier` is then the typical refreshes per real
    // frame.
    bool dynamic{false};
    // Dynamic standing by: the game holds most of the refresh rate, so its
    // frames pass straight through (full_rate is set too).
    bool standby{false};
    std::string dynamic_reason; // why Dynamic last engaged, stood down or stayed standing by
    std::uint64_t dynamic_engagements{0};
    std::uint64_t dynamic_stand_downs{0}; // benefit guard and runtime halving stand-downs (probes not included)
    std::uint64_t dynamic_probes{0};
    // The runtime holds the frames Frame Generation presents to half its
    // refresh rate, or a third or a quarter of it (runtime or headset
    // smoothing, or a frame rate limit, can do that): reported from the
    // presenter thread's waits, and kept until those or, passing frames
    // through, the game's own show every refresh again.
    bool runtime_halving{false};
    // The runtime's display times do not advance with the time that passes
    // (Varjo's after a game paused its frame loop): nothing engages and
    // frames pass straight through until they do.
    bool runtime_clock_displaced{false};
    std::uint64_t runtime_clock_displacements{0};
    bool threaded{false};        // presenting from the presenter thread
    FrameGenDepthState depth{FrameGenDepthState::kUnknown};
    // Session totals for the summaries and tests.
    std::uint32_t lead_slots{0};      // refreshes between a game wait's release and its frame's refresh
    std::uint32_t lead_floor{0};      // the lowest lead known to hold this run (0: none yet)
    std::uint64_t lead_ups{0};        // steps up (late frames)
    std::uint64_t lead_downs{0};      // steps down (frames with a refresh to spare)
    std::uint64_t lead_probes{0};     // steps down after a calm while (FrameGenLeadPolicy)
    std::uint64_t lead_probes_failed{0}; // probes undone by late frames
    std::uint64_t lead_downs_undone{0};  // steps down undone by late frames within FrameGenLeadPolicy::judge
    std::uint64_t late_cpu{0};        // real frames ended after their refresh was due
    std::uint64_t late_gpu{0};        // real frames not captured on the GPU when first chosen for a refresh
    bool gpu_gating{false};           // game waits are held until its GPU catches up
    std::uint64_t gpu_waits{0};       // game waits held for its GPU (over 0.5 ms)
    std::uint64_t gpu_wait_timeouts{0};
    std::uint64_t pauses{0};          // gaps no frame is measured or extrapolated across
    std::uint64_t unready_capture{0}; // refreshes whose real frame was not captured yet
    std::uint64_t unmeasured_copies{0}; // moved frames written as exact copies: motion not measured yet
    std::uint64_t stale_motion{0};      // moved frames moved by the previous real frame's motion instead
    std::uint64_t copies_skipped{0};  // refreshes repeating the exact copy the outputs already hold
    std::uint64_t unwritten{0};       // refreshes that could not be written
    // Refreshes whose real frame's images newer captures had replaced in the
    // backend's history: written as its exact copy, or as the frame before.
    std::uint64_t ring_exhausted{0};
    std::uint64_t analyses_timed{0};  // analyses whose GPU timing was read
    std::uint64_t analyses_late{0};   // of those, ended after their frames were needed
    // Quad views the layer composes into stereo (FrameGenQuadFrame).
    bool quad{false};                    // Frame Generation composes them itself
    std::uint64_t quad_claimed{0};       // frames Frame Generation composed
    std::uint64_t quad_declined{0};      // frames of the quad compose site the layer composed itself
    std::uint64_t quad_setups{0};        // frames the views were set up from (their releases not captured)
    std::uint64_t quad_unanalyzed{0};    // claimed frames whose motion could not be measured (not shown)
    std::uint64_t quad_write_declines{0}; // times composing stopped for a while: frames kept failing to be written
    bool quad_declining{false};          // ...and it has not resumed yet (frames pass straight through meanwhile)
    // Refreshes not submitted, nothing current to show (the runtime shows
    // its last frame again): the frame shown as it is had newer images in
    // its swapchains by then (the runtime shows a swapchain's newest), and no
    // newer frame shown as it is could take its place.
    std::uint64_t stale_skipped{0};
};

// What the On-Screen Display and the desktop app say about a Frame Generation
// status, in one vocabulary: On, Standby, Starting, Paused, Unavailable, Off.
struct FrameGenOsdText {
    std::string row;       // the expanded OSD's Frame Generation row
    std::string compact;   // the compact OSD's one-line status
    std::string detail;    // a second, smaller line under the row; empty for none
    // Beside the OSD's FPS counter: ON, STBY, WAIT, PAUSED, OFF or N/A (empty
    // when the session did not start with Frame Generation on).
    std::string badge;
    double shown_fps{0.0}; // frames per second the headset shows while generating; 0 otherwise
    std::string state;     // the desktop app's state: active, standby, waiting, paused, unavailable or off
    std::string reason;    // the desktop app's status line beside that state; empty for none
};
// `started`: the session started with Frame Generation on.
FrameGenOsdText DescribeFrameGen(const FrameGenStatus& status, bool started);

// Emulated quad views (the layer composes XR_VARJO_quad_views into stereo):
// what happens to one frame from the layer's quad views compose site.
enum class FrameGenQuadRoute : std::uint8_t {
    kDeclined,      // the layer composes it and did not offer it
    kClaimed,       // Frame Generation composes it: the frame's primary projection holds the four source views
    kNotPresenting, // the layer composes it: no just-in-time quad presentation is running
    kUncaptured,    // the layer composes it: a view's latest release was not captured (the views are set up from it)
    kBusy,          // the layer composes it: the frame ending is not a just-in-time frame of its own
};

// Handed to EndFrame with every frame from the emulated quad compose site.
struct FrameGenQuadFrame {
    FrameGenQuadRoute route{FrameGenQuadRoute::kDeclined};
    // kClaimed: each eye's composition (views 0/1 peripheral, 2/3 focus;
    // kUncaptured too, the views are set up for it), and the composed eye
    // views as the runtime gets them (pose, fov; subImage is ignored), made
    // for this frame's own display time.
    FrameGenComposition composition;
    std::array<XrCompositionLayerProjectionView, 2> eyes{};
    // kUncaptured: the four source views (subImage as the application
    // submitted them) and their space, so the views are set up.
    std::array<XrCompositionLayerProjectionView, 4> source_views{};
    XrSpace source_space{XR_NULL_HANDLE};
};

// Dynamic multiplier policy (tests shorten the times).
struct FrameGenDynamicPolicy {
    double engage_below{0.90};                    // standing by: engage once the game runs below this share of the refresh rate...
    std::chrono::milliseconds engage_after{2000}; // ...this long without a break
    double stay_above{0.95};                      // a probe stays standing by only if the game holds this share
    std::chrono::milliseconds settle{2000};       // after any switch, nothing is judged (allocation, first write)
    std::chrono::milliseconds guard_window{2500}; // engaged: the benefit guard's window
    std::chrono::milliseconds hold{10000};        // after a stand-down: no engagement for this long, doubling...
    std::chrono::milliseconds hold_max{160000};   // ...up to this
    std::chrono::milliseconds probe_after{30000}; // engaged without a guard trigger this long: probe standing by, doubling...
    std::chrono::milliseconds probe_max{240000};  // ...up to this after each failed probe
    // Benefit guard: each stands down after two failing windows in a row.
    double distinct_margin_fps{2.0}; // (a) distinct frames per second over the game's own rate before engaging
    double generated_share{0.2};     // (b) generated frames per second over the refreshes the game misses
    double unready_share{0.05};      // (c) refreshes whose real frame was not on the GPU yet, at most
    double late_analyses_share{0.5}; // (d) analyses ending after their frames were needed, at most
};

// How the lead steps back down when nothing late made it grow for a while
// (tests shorten the times). A probe takes one slot off; frames late again
// within `judge` restore it, and that lead is then the lowest known to hold:
// below it is tried only every `explore_after`, doubling after each failure
// there. A floor late frames raised above the lowest lead a probe proved is
// tried again after `explore_after`, not the doubled wait, which is for
// below the proven lead (Star Citizen on Virtual Desktop, 2026-10-14: lead 3
// held about 56 s where 2 was proven). That probe back doubles its own wait
// after each failure; the proven lead and the wait below it stay.
struct FrameGenLeadPolicy {
    std::chrono::milliseconds probe_after{5000};    // no step up this long: one slot less (a probe)
    std::chrono::milliseconds judge{3000};          // a step up this soon after a probe: it failed
    std::chrono::milliseconds explore_after{10000}; // below the lowest lead known to hold: this long, doubling...
    std::chrono::milliseconds explore_max{60000};   // ...up to this
    // Steps down for frames with a refresh to spare are judged as probes too,
    // and frames late at the lowest lead known to hold raise it a refresh
    // (above the proven one at most): below it, once frames were late there,
    // the lead is retried only after explore_after, doubling after each
    // failure (tests compare without).
    bool slack_hysteresis{true};
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
    // At every session's start, Frame Generation on or off: GPU work an
    // earlier session's teardown found unfinished went with that session's
    // swapchains, so a later session's swapchains are never kept for it.
    void BeginSession();
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
    // How long the video memory budget must have room for another set of
    // output images before a lowered ceiling rises again (tests shorten it).
    void SetMemoryRecoveryDelay(std::chrono::milliseconds delay);
    // Timing of the automatic multiplier (tests shorten it): how long a change
    // settles before it is judged, how long the app must hold its cadence
    // before probing one step down, how long a probe must hold, and the first
    // wait between probes (doubling after each failed probe).
    void SetAutoPolicy(std::chrono::milliseconds settle, std::chrono::milliseconds steady,
                       std::chrono::milliseconds probe_confirm, std::chrono::milliseconds probe_backoff);
    // When Dynamic engages, stands down and probes (tests shorten it).
    void SetDynamicPolicy(const FrameGenDynamicPolicy& policy);
    // When the lead steps back down (tests shorten it).
    void SetLeadPolicy(const FrameGenLeadPolicy& policy);
    // How often the session summary is logged (tests shorten it).
    void SetSummaryInterval(std::chrono::milliseconds interval);
    // How long the runtime must hold the frames the presenter thread presents
    // to half its refresh rate (or less) before that is reported, and Dynamic
    // stands down for it; while it is reported, the presenter thread's waits
    // decide again a quarter of it after they start (tests shorten it).
    void SetHalvingWindow(std::chrono::milliseconds window);
    // Quad views: how long the layer composes them after frames kept failing
    // to be written, doubling up to `longest` (tests shorten it).
    void SetQuadDecline(std::chrono::milliseconds first, std::chrono::milliseconds longest);
    // Interpolating, motion is measured on the shader cores even with
    // NVIDIA's optical flow hardware chosen. Beside a game's rendering the
    // hardware's vectors come late on every graphics API (they wait for the
    // busy GPU), while the frames between real frames need them first: DCS,
    // flight 2, measured 8.6 ms against 1.1 on the shader cores, and 6-10% of
    // refreshes repeated a frame; MSFS 2024 (2026-10-09) 9-17 ms, and its lead
    // grew to 11-14 refreshes to make room. Both looked the same interpolated
    // (MSFS, 2026-10-07). Extrapolate keeps the hardware.
    void SetInterpolateOnShaders(bool on);
    // Delay before a paused session retries by itself, and how often.
    void SetRetryPolicy(std::chrono::milliseconds delay, std::uint32_t attempts);
    // In-session A/B toggle; also retries after a pause. Returns the new
    // user-enabled state.
    bool Toggle();
    // Session state changes (begin/end session) drop frames in flight and
    // history continuity. False when the presenter thread did not stop
    // within the teardown wait (it is inside a runtime call): nothing was
    // reset, and the thread is left stopping.
    bool ResetFrames();
    // Teardown never waits on the presenter thread for long: how long it
    // waits for the thread to stop, or for a lock the thread holds over a
    // runtime call (VECTORXR_FRAMEGEN_TEARDOWN_WAIT_MS; tests shorten it).
    void SetTeardownWait(std::chrono::milliseconds wait);
    std::chrono::milliseconds TeardownWait() const;
    // Stops the presenter thread within the teardown wait. False when it is
    // still inside a runtime call: it is left stopping (a runtime may hold
    // a wait until its session ends; ending it lets the thread go).
    bool StopPresenter();
    // A presenter thread that never left a runtime call is left behind at a
    // session's teardown: detached, with everything it uses (its GPU work,
    // output images, the runtime's session) kept for the process's exit.
    // Frame Generation stays off for the rest of the process.
    void AbandonPresenter();
    bool PresenterAbandoned() const;
    // What the presenter thread is doing, and the runtime call it is in and
    // for how long, read without locks: for logs while a teardown waits.
    std::string PresenterActivity() const;
    // The layer ended an application frame without forwarding it.
    void AbandonFrame(XrTime display_time);

    XrSwapchainUsageFlags SourceUsage(const XrSwapchainCreateInfo& info) const;
    // `usage` is what the swapchain was actually created with; a source
    // without the backend's capture usage is never copied.
    void TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count,
                        XrSwapchainUsageFlags usage);
    // False when the GPU did not finish its work in time: work recorded by
    // the layer may still use the swapchain's images.
    bool ForgetSwapchain(XrSwapchain swapchain);
    // Call before forwarding xrDestroySpace: nothing the presenter thread
    // repeats may refer to the space afterwards. False when its submission
    // in flight may still name it (a runtime call past the teardown wait):
    // the space must then be kept, for the runtime to free with the session.
    bool ForgetSpace(XrSpace space);
    void NoteRelease(XrSwapchain swapchain, std::uint32_t image_index);
    // Call before forwarding a release: captures the image into history while
    // the application still owns it.
    void CaptureOnRelease(XrSwapchain swapchain, std::uint32_t image_index);

    XrResult WaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state, const FrameGenRuntimeCalls& calls);
    XrResult BeginFrame(XrSession session, const XrFrameBeginInfo* info, const FrameGenRuntimeCalls& calls);
    // `quad`: the frame came from the layer's emulated quad views compose
    // site (every such frame has one); a kClaimed frame's primary projection
    // holds the four source views and never reaches the runtime as it is.
    XrResult EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls,
                      const FrameGenQuadFrame* quad = nullptr);
    // In xrEndFrame, under the layer's lock, with the four-view projection
    // the layer would submit (views 0/1 peripheral, 2/3 focus; poses times
    // the Pivot reverse delta; fov the compositor's): records the four views
    // as capture targets and, when Frame Generation composes this frame,
    // claims it (it must then reach EndFrame with kClaimed, or AbandonFrame).
    FrameGenQuadRoute ClaimQuadFrame(const XrCompositionLayerProjection& source);

    FrameGenStatus Status() const;
    // The session's frame-by-frame trace (FrameGenTrace), for the layer's
    // own frame calls; it records only while debug logging is on.
    FrameGenTrace& Trace() { return trace_; }
    // The trace records the game's swapchain acquires and releases only while
    // frames are generated or about to be (when releases are captured): while
    // Frame Generation stands by or is off they are every image of every
    // frame and tell nothing about it.
    bool TracesSwapchainCalls() const;
    // The log level changed: a started session's trace follows debug logging
    // (a session starts with it as the level is then).
    void SetTraceEnabled(bool enabled);
    // The runtime shows only images of Frame Generation's own (just in time,
    // once its first frame is written): the game's are captured, never shown.
    bool ShowsOwnImagesOnly() const;
    // Frame Generation captures the images of `swapchain` (a primary
    // projection's color or depth).
    bool CapturesFrom(XrSwapchain swapchain) const;
    // DescribeFrameGen of the current status, and its parts.
    FrameGenOsdText OsdDescription() const;
    std::string OsdText() const;
    std::string CompactOsdText() const;
    // A second OSD line under OsdText (a hint the player can act on), or empty.
    std::string OsdDetailText() const;

  private:
    // Holds a presenter thread "in the runtime" for the layer's teardown tests.
    friend class TurboFrameTestPeer;
    struct SwapchainRecord {
        std::int64_t format{0};
        std::uint32_t sample_count{1};
        bool capturable{true};
        bool depth{false};            // a depth swapchain
        bool depth_capturable{false}; // created with the usage depth capture needs
        std::optional<std::uint32_t> released;
        std::uint64_t releases{0}; // images released so far
        std::uint64_t captured_releases{0}; // `releases` when its latest color capture was submitted
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
        // Presenter thread: when the runtime releases the waits of the slot
        // the real frame is shown in and of the first generated frame's
        // slot ({} when unknown), for the GPU timing in the summary.
        std::chrono::steady_clock::time_point real_due{};
        std::chrono::steady_clock::time_point generated_due{};
        bool spare{false}; // the last fraction is a spare (FrameGenTuning::spare)
    };

    struct ViewRecord {
        FrameGenExtent extent;
        FrameGenViewGeometry geometry;
    };

    struct RealFrame {
        bool valid{false};
        std::uint64_t sequence{0};
        XrTime display_time{0};
        XrDuration interval{0}; // since the real frame before it, 0 when not continuous
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
        // Per set: an exact copy of the real frame it came from (its own pose
        // and image), which alone may carry that frame's depth chain.
        std::vector<std::uint8_t> exact;
        // Changes whenever the sets are replaced (distinct-frame counting).
        std::uint64_t generation{0};
        // The display time each set was made for (presenter thread).
        std::vector<XrTime> targets;
        // Interpolated between the previous real frame and the newest one,
        // ending with the newest real frame's own copy.
        bool interpolated{false};
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
            // The depth swapchains' SwapchainRecord::releases when copied.
            std::vector<std::uint64_t> depth_releases;
            // Per view, its color swapchain's SwapchainRecord::releases when
            // copied (for frames shown as they are).
            std::vector<std::uint64_t> color_releases;
            XrCompositionLayerQuad quad{};
            XrCompositionLayerCylinderKHR cylinder{};
            XrCompositionLayerEquirectKHR equirect{};
            XrCompositionLayerEquirect2KHR equirect2{};
            XrCompositionLayerCubeKHR cube{};
        };
        bool valid{false};
        bool has_projection{false};
        bool projection_stale{false}; // its swapchain was destroyed: never repeat it
        // Its primary projection holds a claimed quad frame's four source
        // views: shown only through generated (composed) views.
        bool quad{false};
        std::uint32_t projection_index{0};
        XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        std::vector<Layer> layers;
    };

    // Just-in-time presentation: a real frame generated frames are written
    // from on the presenter thread, right before the refreshes showing them.
    struct JitFrame {
        std::uint64_t id{0};       // ThreadedFrame::id
        std::uint64_t analyzed{0}; // FrameGenExecuteResult::frame
        XrTime time{0};            // the display time it was rendered for
        XrTime previous_time{0};   // the real frame before's, when continuous (0: shown unchanged only)
        // The real frame analyzed right before it, when continuous (0: none):
        // its motion moves this frame while this one's is still measured
        // (FrameGenGpu::SynthesizeStale).
        std::uint64_t previous_analyzed{0};
        FrameCopy copy;            // its layers; the primary projection's views are its own
        std::vector<FrameGenOutputKind> kinds;       // per view
        std::vector<std::array<float, 3>> velocity; // per view (Analysis::velocity)
        bool depth{false};                          // Analysis::depth
        // Its lead sample (UpdateLeadLocked), taken when a refresh first
        // chooses it: on time only if its game frame had reached the GPU.
        std::uint32_t lead{0};
        std::uint32_t multiplier{2};
        XrTime needed{0};                // the refresh it is first needed for
        std::optional<double> slack_ms;  // how much later its end could have come
        std::uint64_t capture_mark{0};   // FrameGenGpu::CaptureMark covering its views' captures
        bool captured_early{false};      // captured a refresh or more before it was needed
        // Kept while the refresh it is first needed for was still to come: a
        // refresh passing over it then missed it, not the frame its refresh.
        bool ahead{false};
        bool scored{false};
        // Quad views: the claimed frame's composition and eye views (its
        // analyzed views are the four source views); null on the canvas path.
        std::shared_ptr<const FrameGenQuadFrame> quad;
        // A frame the layer composed itself while Frame Generation composes
        // quad views: shown in its own refresh as it is, nothing written.
        bool as_is{false};
    };

    // An application frame the presenter thread presents.
    struct ThreadedFrame {
        std::uint64_t id{0};
        XrTime predicted{0};           // the display time returned to the app
        XrTime target_time{0};         // the display time of target_slot
        XrDuration reported_period{0}; // the period returned to the app
        std::uint64_t target_slot{0};  // the slot the frame is predicted to reach
        // The first slot it fills: target_slot, or when interpolating the
        // first of the multiplier - 1 slots before it, which show frames
        // generated between the previous real frame and this one.
        std::uint64_t first_slot{0};
        XrTime first_time{0};
        bool interpolate{false};
        bool jit{false};               // presented just in time (jit_ when it was waited)
        std::uint32_t multiplier{2};
        std::uint32_t lead{0};         // refreshes between its release and target_slot
        bool begun{false};
        bool claimed{false}; // its end has started
        bool quad{false};    // claimed by ClaimQuadFrame: Frame Generation composes its quad views
        std::uint64_t capture_mark{0}; // last_capture_mark_ at its begin: the captures released before it
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

    // Quad views: a view's latest color capture (CaptureOnRelease), as the
    // backend's Execute matches its source against it.
    struct QuadCapture {
        bool ok{false}; // the capture was submitted
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::uint32_t image_index{0};
        std::uint32_t array_index{0};
        XrRect2Di rect{};
        std::uint64_t releases{0}; // the swapchain's SwapchainRecord::releases it captured
    };

    // An output image per output index: its extent and what it holds.
    struct OutputSpec {
        FrameGenExtent extent;
        FrameGenOutputKind kind{FrameGenOutputKind::kSrgb8};
    };

    bool WantsEngagementLocked() const;
    // Frames are generated or about to be: the game's releases are captured.
    bool CapturingLocked() const;
    // The state while no frame is engaged: off when turned off, else waiting.
    FrameGenState IdleStateLocked() const;
    bool RunnableLocked() const;
    std::uint32_t EffectiveMultiplierLocked() const;
    // Dynamic in effect: chosen, and the presenter thread extrapolates.
    bool DynamicLocked() const;
    // Dynamic: refreshes per real frame, smoothed and rounded (2-4).
    std::uint32_t DynamicSlotsLocked() const;
    // Dynamic: generated frames (sets of output images) one real frame may use.
    std::uint32_t DynamicSetCapLocked() const;
    void UpdateAutoLocked(std::chrono::steady_clock::time_point now, const std::optional<PendingFrame>& frame,
                          bool started_late = false);
    // Auto, presenting from its own thread: whether the game's own time per
    // frame (auto_busy_) is within reach of `slots` periods a frame (`period`:
    // the grid the game's frames are timed by, GameGridLocked).
    bool AutoProbeInReachLocked(std::uint32_t slots, XrDuration period) const;
    // Dynamic: standing by, engages once the game runs below the refresh
    // rate; engaged, the game keeps its own rate (generated frames fill the
    // refreshes it misses) and Dynamic stands down when Frame Generation
    // does not pay (the benefit guard, or the runtime halving the frame
    // rate) or to probe whether the game holds the rate alone. `period`: the
    // refresh period.
    void UpdateDynamicLocked(std::chrono::steady_clock::time_point now, XrDuration period);
    void DynamicEngageLocked(std::chrono::steady_clock::time_point now, double rate, double refresh);
    void DynamicStandDownLocked(std::chrono::steady_clock::time_point now, const std::string& reason, bool probe);
    // Turned back on (toggled, configured, retried after a pause): Dynamic
    // stands by again and judges the game's rate afresh.
    void DynamicReenableLocked(std::chrono::steady_clock::time_point now);
    // Seconds standing by this session (Dynamic).
    double StandbySecondsLocked(std::chrono::steady_clock::time_point now) const;
    // ", dynamic=standby (reason)" or engaged for summaries; empty unless Dynamic.
    std::string DynamicSummaryLocked() const;
    // A gap between real frames nothing is measured or extrapolated across:
    // relative to the game's own pace (hitches are not pauses), and never
    // more than a second (loading stalls always are).
    XrDuration PauseLimitLocked() const;
    // A real frame ended (wall clock): the game's smoothed frame interval,
    // pauses left out.
    void NoteRealEndLocked(std::chrono::steady_clock::time_point end);
    void NotePauseLocked(XrDuration gap);
    void MaybeRetryLocked();
    void PauseLocked(const std::string& reason);
    void UnavailableLocked(const std::string& reason);
    void SetStateLocked(FrameGenState state, const std::string& reason);
    void RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection);
    std::optional<FrameGenOutputKind> OutputKindLocked(std::size_t view, std::int64_t source_format) const;

    // Quad views: Frame Generation can compose them (just in time, from its
    // presenter thread, with a backend that composes).
    bool QuadCapableLocked() const;
    // The four source views are the capture targets (not the layer's
    // composed views): while presenting quad views, or for a frame of the
    // quad compose site Frame Generation could compose.
    bool QuadTargetsLocked(const FrameGenQuadFrame* quad) const;
    void RecordQuadCaptureTargetsLocked(const XrCompositionLayerProjection& source);
    // Every source view's latest release was captured, as Execute will look
    // for it (QuadCapture).
    bool QuadCapturedLocked(const XrCompositionLayerProjection& source) const;
    // An application frame ends: counts its quad route, and restarts the
    // presenter thread when frames now want the other composition.
    void NoteQuadEndLocked(const FrameGenQuadFrame* quad, bool projection);
    // Writing composed frames kept failing: the layer composes quad views
    // for a while (ClaimQuadFrame declines).
    void DeclineQuadLocked(std::chrono::steady_clock::time_point now, const std::string& error);
    // ...and that while is not over: nothing engages, and the layer's
    // composed frames pass straight through to the runtime at full rate.
    bool QuadDeclinedLocked() const;

    XrResult SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time, const FrameGenRuntimeCalls& calls);
    // A runtime wait returned with this period at `now`: the refresh period
    // follows a shorter (or about the same) period at once, never one about
    // two to five times it, and another longer one only once the periods
    // reported have agreed with it for a while (kRefreshChangeAfter).
    void NoteRuntimePeriodLocked(XrDuration period, std::chrono::steady_clock::time_point now);
    // A presenter-thread runtime wait (`presenter`) or, passing frames
    // through, one of the game's: whether the runtime held it a refresh or
    // more past the one asked for or reported a multiple of the refresh
    // period (it halves the frame rate, or cuts it to a third or a quarter),
    // over halving_window_ (while halving is reported, a quarter of it from
    // when the presenter thread's waits start). A wait asked for late shows
    // that only when the runtime reported a multiple and still blocked it;
    // one it reported a multiple for and released at once, for a display
    // time off the refresh grid (the period echoing its caller's own
    // interval), was not held, and any other says nothing. Only the
    // presenter thread's waits report halving; the game's own can clear it
    // (they may also wait for the game's frame before, as MSFS's do).
    void NoteRuntimeHalvingLocked(std::chrono::steady_clock::time_point called,
                                  std::chrono::steady_clock::time_point returned, XrTime display_time,
                                  XrDuration period, bool presenter);
    void NotePhaseLocked(std::chrono::steady_clock::time_point called, std::chrono::steady_clock::time_point returned,
                         XrTime display_time, XrDuration period);
    // A runtime wait returned for `display_time` (any of them: the presenter
    // thread's, a generated cycle's, or one of the game's): whether the
    // runtime's display times advance with the time that passes between its
    // waits (clock_displaced_). `rendered`: the wait said to render.
    void NoteRuntimeClockLocked(std::chrono::steady_clock::time_point returned, XrTime display_time, XrDuration period,
                                bool rendered);
    // "the runtime's display times ... " for the log and Dynamic's reason.
    std::string RuntimeClockTextLocked() const;
    // What paced submissions are spaced by, and the presenter thread's
    // half-refresh thresholds judged by: the period reported, at most the
    // refresh period.
    XrDuration SubmitSpacingLocked(XrDuration period) const;
    // The period the game's frames are timed by just in time and on the
    // presenter thread: the refresh period, unless the runtime holds frames
    // to a multiple of it (runtime_halving_), then the period it reports.
    XrDuration GameGridLocked() const;
    // When the paced submission for `display_time` goes to the runtime, from
    // `now` (recorded as the newest point of submit_grid_), and whether it is
    // already more than half a refresh past the time the runtime would have
    // released its wait.
    std::pair<std::chrono::steady_clock::time_point, bool>
    ScheduleSubmissionLocked(std::chrono::steady_clock::time_point now, XrTime display_time, XrDuration period);
    // Sleeps until the slot's submission is due (ScheduleSubmissionLocked).
    // Returns true when the slot was already more than half a refresh past
    // the time the runtime would have released its wait.
    bool PaceSubmission(XrTime display_time, XrDuration period);
    XrResult NextSlot(XrSession session, const FrameGenRuntimeCalls& calls, XrDuration period, XrFrameState& state);
    XrResult RunCycle(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                      const FrameGenRuntimeCalls& calls);
    // Just-in-time presentation: what measuring a real frame's motion gave
    // (Generate with `analyze`).
    struct Analysis {
        std::uint64_t frame{0}; // FrameGenExecuteResult::frame
        bool synthesized{false}; // motion was measured (a previous frame to measure it against)
        std::vector<FrameGenOutputKind> kinds;
        // Per view: how far the eye moves per unit forward in time (the
        // interval between the two real frames), in its view space, where
        // depth is used (generated frames are seen from where it will be).
        std::vector<std::array<float, 3>> velocity;
        bool depth{false}; // depth was used for a view
    };

    // Generates one frame per fraction into the output swapchain sets from
    // the newest real frame. On success `sets` holds the generated
    // projection views, one set per fraction, and `exact` which of them
    // reproduce the newest frame exactly (its own time: an interpolated
    // fraction of 1, or a frame written unchanged); only those may carry its
    // depth, which describes its image and eye position, not a generated
    // frame's. Without `release` the output images, written or not, stay
    // acquired until ReleaseOutputs. With `analyze` it only measures the
    // newest frame's motion for just-in-time synthesis (no fractions, no
    // output images), filtered for `mode` and, interpolating, for cover
    // frames too.
    // `composition`: quad views, how each eye is composed from the views
    // (FrameGenTuning::composition).
    bool Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame, FrameGenSynthesisMode mode,
                  const std::vector<float>& fractions, std::vector<std::vector<XrCompositionLayerProjectionView>>& sets,
                  std::vector<std::uint8_t>& exact, bool release = true, Analysis* analyze = nullptr,
                  const FrameGenComposition* composition = nullptr);
    // Depth chains of a frame copy that describe images the game has since
    // replaced are dropped.
    void RecordDepthReleasesLocked(FrameCopy& copy) const;
    void DropStaleDepthLocked(FrameCopy& copy) const;
    // Likewise for color, for frames shown as they are (the layer's composed
    // frames, or a frame passed through): the runtime shows a swapchain's
    // newest image, so once a newer one was released into a copy's
    // swapchains, the copy would show that image with its own views.
    void RecordColorReleasesLocked(FrameCopy& copy) const;
    bool ColorReleasedSinceLocked(const FrameCopy& copy) const;
    // Just in time: what shows a frame as it is (`copy` of real frame `id`,
    // made for display time `time`; 0 for a frame passed through): the copy
    // while its images are still the newest; else a newer kept frame the
    // layer composed whose images are, unless a frame Frame Generation
    // writes comes between them (shown early, with its own views, correct
    // for the runtime's reprojection); else nothing (copy null).
    struct AsIs {
        const FrameCopy* copy{nullptr};
        std::uint64_t id{0};
        XrTime time{0};
    };
    AsIs CurrentAsIsLocked(const FrameCopy& copy, std::uint64_t id, XrTime time) const;
    // How far each eye moves per unit of generated-frame time from the real
    // frame shown at `newest_time` (FrameGenViewRequest::shift), in that
    // eye's view space: as far as it moved since the previous real frame
    // (last_real_), whose display time `newest_time` follows.
    std::vector<std::array<float, 3>> EyeShiftsLocked(const XrCompositionLayerProjection& projection,
                                                      XrTime newest_time, FrameGenSynthesisMode mode) const;
    // Summary statistics: the eye shift's prediction, and when generation
    // ran on the GPU.
    void NoteEyeMovementLocked(const XrCompositionLayerProjection& projection, const PendingFrame& frame);
    void NoteGpuTimingLocked(std::uint64_t submission, const PendingFrame& frame,
                             const std::optional<FrameGenGpuWindow>& window,
                             const std::vector<FrameGenAnalysisTiming>& timings);
    // One output image per spec in each of the first `sets` output sets.
    // `keep_replaced`: an output made anew keeps its old swapchain alive
    // (retired_outputs_) until a frame is written into the new ones.
    bool EnsureOutputs(const std::vector<OutputSpec>& specs, std::size_t sets, std::string& error,
                       bool keep_replaced = false);
    // Acquires and waits the images of the first `sets` output sets.
    bool AcquireOutputs(std::size_t sets, std::string& error, bool& timed_out);
    void DestroyOutputs();
    // Destroys outputs EnsureOutputs replaced (nothing shows them any more).
    void DestroyRetiredOutputs();
    // Destroys the output sets from `keep` on (frame thread).
    void TrimOutputs(std::size_t keep);
    void ReleaseOutputs();
    void RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time, std::uint64_t sequence);
    void InvalidateContinuityLocked();
    void NoteFailureLocked(const std::string& error);
    void LogMemoryBudgetLocked();
    // `distinct`: the submission shows something the one before did not;
    // `generated`: a generated frame (not a real frame or its exact copy).
    void NoteSubmissionLocked(bool distinct, bool generated = false);
    // How long the game's xrEndFrame spent in EndFrame (summary).
    void NoteEndCall(std::chrono::steady_clock::time_point entered);

    // Threaded presentation.
    bool ThreadedEligibleLocked() const;
    // Running threaded presentation continues while this holds; unlike
    // engagement it does not need a fresh run of steady frames.
    bool StayThreadedLocked() const;
    void DropUnclaimedLocked();
    void UpdateLeadLocked(bool late, std::optional<double> slack_ms, std::uint32_t lead, std::uint32_t multiplier);
    // Just in time: a kept real frame's lead sample, once.
    void ScoreLeadLocked(JitFrame& frame, bool late);
    // A kept real frame dropped before any refresh chose it: late when its
    // views were still not captured.
    void ScoreErasedLocked(JitFrame& frame);
    // GPU-aware pacing: the capture a game wait is held for (none while
    // gating is off or not armed), and what holding it showed.
    std::optional<std::uint64_t> GpuGateMarkLocked() const;
    // A held wait ended: reached, or timed out with or without captures
    // having run since the last held wait (`progressed`). Only timeouts
    // without progress count toward turning holding off (a slow GPU is held
    // for, a stuck one not).
    void NoteGpuWaitLocked(double waited_ms, bool reached, bool progressed);
    // The newest capture mark that has run on the GPU by now, at least
    // `known` (captures run in order).
    std::uint64_t ReachedCaptureMarkLocked(std::uint64_t known) const;
    // Arms GPU-aware pacing once a window shows the game's frames reaching
    // the GPU late (real frames uncaptured when needed, or captures ending
    // after their refresh was due).
    void NoteGpuEvidenceLocked(std::uint64_t real_frames, std::uint64_t late_gpu, std::vector<double> capture_done_ms);
    // Wakes the presenter thread and app threads waiting on slots.
    void WakeSlots();
    // Every runtime frame call goes through these, so the presenter knows the
    // runtime's frame state (waited, begun, ended) whatever its own frame
    // bookkeeping says, and never makes a call out of order.
    XrResult RuntimeWait(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameWaitInfo* info,
                         XrFrameState* state);
    XrResult RuntimeBegin(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameBeginInfo* info);
    // `paced`: the submission was scheduled (ScheduleSubmissionLocked), not
    // a frame passed straight through.
    XrResult RuntimeEnd(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameEndInfo* info, bool paced);
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
                         const ThreadedFrame& frame, std::chrono::steady_clock::time_point entered,
                         const FrameGenQuadFrame* quad);
    // Just in time: the real frame is measured and kept; the presenter thread
    // shows it (an exact copy) in its own refresh and the frames generated
    // around it in theirs (between it and the one before when
    // interpolating, after it when extrapolating). Composing quad views, a
    // claimed frame is measured from its four views; any other is kept as
    // the layer composed it (JitFrame::as_is).
    XrResult ThreadedEndJit(const XrFrameEndInfo& info, const ThreadedFrame& frame,
                            std::chrono::steady_clock::time_point entered, const FrameGenQuadFrame* quad);
    // Just in time: measures a real frame (app thread) and keeps it for the
    // presenter thread. False when nothing can be generated from it. `score`
    // carries its lead sample (JitFrame::lead to capture_mark, and scored
    // when its end was already late); a frame not kept is scored on time.
    // `quad`: a claimed quad frame's composition (kept with it).
    bool AnalyzeJit(const XrCompositionLayerProjection& projection, const PendingFrame& frame, FrameGenSynthesisMode mode,
                    FrameCopy copy, const JitFrame& score, std::shared_ptr<const FrameGenQuadFrame> quad = nullptr);
    // Presenter thread, just in time: what the refresh at `time` shows: the
    // real frame `index` of jit_frames_ at `fraction` in `mode`, or nothing
    // (repeat). `due` is set when the next real frame is needed and not
    // measured yet.
    struct JitChoice {
        std::optional<std::size_t> index;
        FrameGenSynthesisMode mode{FrameGenSynthesisMode::kInterpolate};
        float fraction{1.0f};
        bool cover{false}; // past the newest real frame while the next one is late
    };
    // Of the oldest `limit` kept real frames (all by default).
    JitChoice ChooseJitLocked(XrTime time, XrDuration period, bool interpolate, bool& due,
                              std::size_t limit = SIZE_MAX) const;
    // Presenter thread, holding no lock (it makes output swapchains, which
    // takes submit_mutex_): writes the chosen frame into the next output
    // images and fills `views` with the projection views showing them.
    // False (nothing released) when it cannot.
    // What a frame is made from must have run on the GPU (written before,
    // the frame would wait for it on the runtime's queue, and everything
    // after it there): a moved frame whose motion is not measured yet
    // becomes its real frame's exact copy; a real frame not captured yet
    // gives way to `fallback` (the choice as if it had not arrived; `frame`
    // and `choice` become it), else nothing is written. A moved frame whose
    // real frame's previous images newer captures replaced in the backend's
    // history (FrameGenGpu::Writable) becomes its exact copy; one whose
    // newest images were replaced too gives way to `fallback` the same way.
    // A claimed quad frame is written composed, into one output image per
    // eye.
    struct JitWrite {
        bool exact{false};      // the real frame's exact copy
        bool uncaptured{false}; // the chosen frame was not captured yet (whether or not the fallback was written)
        bool unmeasured{false}; // a moved frame was to be written as its exact copy: its motion was not measured yet
        bool stale{false};      // a moved frame moved by the previous real frame's motion (its own not measured yet)
        // Interpolating, a frame between two real frames needed the newer
        // one's motion before it was measured: the older one moved on past
        // itself instead (the fallback, a cover frame). Late for the lead.
        bool motion_late{false};
        bool ring_exhausted{false}; // images it was to be written from were replaced by newer captures (Writable)
        bool as_is{false};      // nothing written: the frame (JitFrame::as_is) is shown as the layer composed it
        bool failed{false};     // writing was attempted and failed: outputs, an acquire or wait, or the GPU
        bool timed_out{false};  // ...an output image the runtime did not hand back in time
        std::string error;
    };
    bool SynthesizeJit(std::optional<JitFrame>& frame, JitChoice& choice, const std::optional<JitFrame>& fallback,
                       const JitChoice& fallback_choice, std::vector<XrCompositionLayerProjectionView>& views,
                       JitWrite& write);
    // Just in time: the frame `choice` shows is its real frame unchanged.
    static bool JitCopyOnly(const JitFrame& frame, const JitChoice& choice);
    void SummaryLocked(std::chrono::steady_clock::time_point now);
    // Starts the next summary window, the presenter thread's or the inline
    // one's: every window sample and count either prints starts over.
    void NewSummaryWindowLocked(std::chrono::steady_clock::time_point now, double standby);
    void StartCompositorLocked(const FrameGenRuntimeCalls& calls);
    // Lets the presenter thread present every frame in flight, then stops it.
    void ExitThreadedLocked(std::unique_lock<std::mutex>& lock);
    // Stops the presenter thread at its next slot; frames still in flight are
    // dropped. With `limit`, waits at most that long: a thread still inside
    // a runtime call is left stopping (nothing reset) and false is returned.
    // Either way a wait past the teardown wait is logged with what the
    // thread is in.
    bool StopCompositorLocked(std::unique_lock<std::mutex>& lock,
                              std::optional<std::chrono::milliseconds> limit = std::nullopt);
    void JoinCompositorLocked(std::unique_lock<std::mutex>& lock);
    // Detaches a presenter thread that never left a runtime call (logged).
    void AbandonPresenterLocked();
    // Teardown takes `lock` on a mutex the presenter thread holds over its
    // runtime calls (submit_mutex_, gpu_mutex_): within the teardown wait, or
    // briefly while the thread has been inside one runtime call longer than
    // that (PresenterWedged); false (logged) when it could not.
    bool LockForTeardown(std::unique_lock<std::timed_mutex>& lock, const std::string& what);
    // The presenter thread has been inside one runtime call longer than the
    // teardown wait (read without locks).
    bool PresenterWedged() const;
    // The presenter thread's runtime call (PresenterActivity): set around
    // each one it makes, on that thread only.
    struct PresenterCall {
        PresenterCall(FrameGenPresenter& presenter, const char* call);
        ~PresenterCall();
        PresenterCall(const PresenterCall&) = delete;
        PresenterCall& operator=(const PresenterCall&) = delete;
        FrameGenPresenter* presenter{nullptr}; // null off the presenter thread
    };
    void CompositorLoop();
    static bool CopyFrame(const XrFrameEndInfo& info, FrameCopy& copy, std::string& unsupported);
    // Points `pointers` at `copy`'s layers, replacing its primary projection's
    // views with `generated` when given; those keep the copy's depth chain
    // only when `generated_depth` (an exact copy of the real frame).
    static XrFrameEndInfo BuildFrame(FrameCopy& copy, const std::vector<XrCompositionLayerProjectionView>* generated,
                                     XrSpace generated_space, XrCompositionLayerFlags generated_flags,
                                     bool generated_depth, std::vector<const XrCompositionLayerBaseHeader*>& pointers);

    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    XrSession session_{XR_NULL_HANDLE};
    Logger* logger_{nullptr};
    FrameGenTrace trace_;
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
    // The submission grid: when the last submission to the runtime was due
    // (a paced one's scheduled time), or was made (a frame passed straight
    // through, or a paced one sent more than half a refresh late).
    std::chrono::steady_clock::time_point submit_grid_{};
    XrDuration last_period_{0};
    // The runtime's refresh period: the period it reports, except one about
    // two to five times the refresh period already seen (a runtime cutting
    // the frame rate to that fraction may report the period it holds frames
    // to, and SteamVR with Pimax's headset driver reports a late caller's
    // own frame interval in whole refreshes), and another longer one until
    // the periods reported have agreed with it for a while
    // (refresh_candidate_: it, and since when). Dynamic judges the game's
    // rate against it, and submissions are spaced by at most it. A runtime
    // cutting the rate from its first frame is indistinguishable from a
    // headset at that fraction of the refresh rate.
    XrDuration refresh_period_{0};
    std::optional<std::pair<XrDuration, std::chrono::steady_clock::time_point>> refresh_candidate_;
    // Runtime halving (NoteRuntimeHalvingLocked): the last wait measured, its
    // source's waits over the window (when, how many refreshes the runtime
    // held each to: 1 for the next one, and whether a halved one blocked for
    // half a refresh or more), since when that source has been measured, and
    // the verdict.
    struct HalvingWait {
        std::chrono::steady_clock::time_point returned{};
        XrTime display_time{0};
        bool presenter{false};
    };
    struct HalvingSample {
        std::chrono::steady_clock::time_point returned{};
        std::uint32_t held{1};
        bool blocked{false};
    };
    std::optional<HalvingWait> halving_last_;
    std::deque<HalvingSample> halving_samples_;
    std::chrono::steady_clock::time_point halving_since_{};
    std::chrono::milliseconds halving_window_{4000};
    bool runtime_halving_{false};
    // While runtime_halving_: when the presenter thread's waits last showed
    // the runtime halving the frame rate (Dynamic stands down for one shown
    // while it is engaged).
    std::optional<std::chrono::steady_clock::time_point> halving_confirmed_at_;
    bool logged_runtime_halving_{false};
    // Auto back at full rate for one shown while it is engaged (where the
    // runtime reports the refresh period, UpdateAutoLocked): it stays
    // there until this while the runtime still halves, the hold doubling
    // from FrameGenDynamicPolicy::hold up to its hold_max each time.
    std::optional<std::chrono::steady_clock::time_point> auto_halving_hold_until_;
    std::chrono::milliseconds auto_halving_hold_{10000};
    // The runtime's own cadence: when a runtime wait blocked and returned for
    // display time phase_xr_ at phase_wall_, the wait for display time t
    // would return at phase_wall_ + (t - phase_xr_).
    std::chrono::steady_clock::time_point phase_wall_{};
    XrTime phase_xr_{0};
    // Since the phase was measured, a wait came back for a display time off
    // its refresh grid: the runtime echoes its caller's own interval, and
    // submissions are not held to the phase until a wait blocks again
    // (ScheduleSubmissionLocked).
    bool phase_echoed_{false};
    // The runtime's display clock (NoteRuntimeClockLocked). Varjo's runtime,
    // after nothing waited for a while (a game loading a mission), returns a
    // display time seconds to a minute ahead, then moves each wait's only
    // 10 µs on until its refreshes catch up: refreshes presented from those
    // never reach the game's frames, and the game, told them, renders for a
    // time far ahead. Displaced by a wait whose display time ran far further
    // past this process's clock than any wait's while steady
    // (clock_offset_ceiling_: SteamVR lags after a pause and catches up, which
    // never counts; taken over the first kClockWarmupWaits waits, and anew
    // after each recovery), by one that went back, or by kClockStalledWaits
    // waits in a row a refresh or more apart whose display times barely
    // moved; steady again after kClockSteadyWaits waits, over
    // kClockSteadyTime, advanced with the time between them. Displaced,
    // nothing engages, Auto holds its multiplier, and the presenter thread
    // stops: frames pass straight through, as without Frame Generation.
    struct ClockWait {
        std::chrono::steady_clock::time_point returned{};
        XrTime display_time{0};
    };
    std::optional<ClockWait> clock_last_;
    XrDuration clock_offset_ceiling_{0}; // the highest display time minus steady-clock time while steady
    std::uint32_t clock_warmup_waits_{0};  // waits the ceiling was taken over (kClockWarmupWaits)
    // The last waits judged (time since the one before, display time step,
    // offset over the ceiling), logged when the clock is displaced.
    struct ClockSample {
        XrDuration wall{0};
        XrDuration step{0};
        XrDuration over{0};
    };
    std::array<ClockSample, 8> clock_history_{};
    std::size_t clock_history_next_{0};
    bool clock_displaced_{false};
    std::chrono::steady_clock::time_point clock_displaced_at_{};
    XrDuration clock_ahead_{0};         // how far display times ran past the ceiling (0: they stalled)
    std::uint32_t clock_stalled_waits_{0}; // in a row
    std::uint32_t clock_steady_waits_{0};  // in a row, while displaced
    std::chrono::steady_clock::time_point clock_steady_since_{};
    XrDuration clock_gap_{0};                 // the time passed before the wait that displaced it
    std::uint32_t logged_clock_displacements_{0}; // logged one by one up to a few, then only in summaries
    bool clock_displacement_logged_{false};       // this one was: so is its end

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
    // The game's real frames: the last one's end and the smoothed interval
    // between their ends, nanoseconds (0 until measured; PauseLimitLocked).
    std::optional<std::chrono::steady_clock::time_point> real_end_;
    double real_interval_ns_{0.0};
    std::chrono::steady_clock::time_point multiplier_changed_at_{};
    std::chrono::steady_clock::time_point probe_not_before_{};
    std::chrono::milliseconds probe_backoff_{10000};
    bool probing_{false};
    std::uint32_t probe_from_{0};
    // Presenting from its own thread: the game's last frames' own time per
    // frame (from one wait to the next, the cadence's hold left out), ms,
    // and when the last wait's hold ended (AutoProbeInReachLocked). Not just
    // in time, the game's xrEndFrame is held for its frame's slot too
    // (ThreadedEnd): the last such hold, left out of the time it falls in.
    std::array<double, 32> auto_busy_{};
    std::size_t auto_busy_count_{0};
    std::chrono::steady_clock::time_point auto_busy_since_{};
    std::chrono::steady_clock::time_point auto_end_held_from_{};
    std::chrono::steady_clock::time_point auto_end_held_to_{};
    std::uint64_t auto_end_held_frame_{0}; // the frame whose end was held then
    std::size_t auto_late_percent_{0}; // for the log
    // Auto never steps above this again in the session once video memory
    // ran near its budget: each step up allocates more output images.
    std::uint32_t auto_memory_ceiling_{4};
    std::optional<FrameGenMemoryBudget> memory_budget_;
    std::chrono::steady_clock::time_point memory_checked_at_{};
    std::uint64_t working_bytes_{0}; // the backend's own images after the last Execute (log)
    // When Generate next asks the backend for working_bytes_ (under gpu_mutex_).
    std::chrono::steady_clock::time_point working_bytes_due_{};
    std::uint64_t output_bytes_{0};  // output swapchain images, estimated from their size (log)
    std::uint64_t output_set_bytes_{0}; // one set of them
    // Since when the budget has had room for another set (UpdateAutoLocked),
    // and how often the ceiling rose again this session.
    std::optional<std::chrono::steady_clock::time_point> memory_room_since_;
    std::uint32_t memory_recoveries_{0};
    std::chrono::milliseconds memory_recovery_delay_{30000};

    // Dynamic (FrameGenDynamicPolicy): standing by, frames pass straight
    // through; engaged, the game runs at its own rate (released at most once
    // a refresh), generated frames fill the refreshes it misses, and the
    // benefit guard watches what generation adds.
    FrameGenDynamicPolicy dynamic_policy_;
    bool dynamic_standby_{true};
    std::optional<std::chrono::steady_clock::time_point> dynamic_below_since_;
    std::chrono::steady_clock::time_point dynamic_hold_until_{};
    std::chrono::milliseconds dynamic_hold_{10000};
    std::chrono::milliseconds dynamic_probe_after_{30000};
    std::chrono::steady_clock::time_point dynamic_engaged_at_{};
    bool dynamic_probing_{false};
    double passthrough_fps_{0.0}; // the game's own rate when Dynamic last engaged
    std::string dynamic_reason_;
    std::uint64_t dynamic_engagements_{0};
    std::uint64_t dynamic_stand_downs_{0};
    std::uint64_t dynamic_probes_{0};
    std::optional<std::chrono::steady_clock::time_point> standby_since_; // standing by since (Dynamic)
    double standby_seconds_{0.0}; // earlier stand-by stretches this session

    // Session totals, never reset by summaries: summary windows and the
    // benefit guard keep a copy from their start and use the differences.
    struct Totals {
        std::uint64_t submissions{0};
        std::uint64_t distinct{0};
        std::uint64_t generated_distinct{0};
        std::uint64_t app_frames{0};
        std::uint64_t captured{0};
        std::uint64_t unready{0};      // refreshes whose real frame was not captured yet
        std::uint64_t unmeasured{0};   // moved frames written as exact copies: motion not measured yet
        std::uint64_t stale_motion{0}; // moved frames moved by the previous real frame's motion instead
        std::uint64_t analyses_timed{0};
        std::uint64_t analyses_late{0};
        std::uint64_t copies_skipped{0};
        std::uint64_t ring_exhausted{0}; // refreshes whose real frame's images newer captures had replaced
        std::uint64_t stale_skipped{0};  // refreshes not submitted: nothing current to show
        std::uint64_t unrendered{0};     // refreshes the runtime did not render (shouldRender false)
        std::uint64_t late_cpu{0};
        std::uint64_t late_gpu{0};
        std::uint64_t gpu_waits{0};
        std::uint64_t gpu_wait_timeouts{0};
        std::uint64_t pauses{0};
        std::uint64_t unwritten_wait{0};    // an output image the runtime did not hand back in time
        std::uint64_t unwritten_failed{0};  // writing (or acquiring an output) failed
        std::uint64_t unwritten_outputs{0}; // output images could not be made
        std::uint64_t unwritten_source{0};  // no real frame to write from
        std::uint64_t non_renderable{0};    // real frames without a projection
        std::uint64_t restarts{0};          // presenter thread starts and synthesis mode switches
        std::uint64_t lead_ups{0};
        std::uint64_t lead_downs{0};
        std::uint64_t lead_probes{0};
        std::uint64_t lead_probes_failed{0};
        std::uint64_t lead_downs_undone{0};
        // Frames of the layer's quad compose site, by route (FrameGenQuadRoute).
        std::uint64_t quad_claimed{0};
        std::uint64_t quad_uncaptured{0};
        std::uint64_t quad_busy{0};
        std::uint64_t quad_declined{0};
        std::uint64_t quad_not_presenting{0};
        std::uint64_t quad_setups{0};     // views set up from a frame not captured
        std::uint64_t quad_unanalyzed{0}; // claimed frames whose analysis failed
        // Presenter-thread runtime waits measured for halving, and of those
        // halved (NoteRuntimeHalvingLocked).
        std::uint64_t halving_measured{0};
        std::uint64_t halving_halved{0};
        // Times the runtime's display clock was displaced, and runtime waits
        // that found it so (NoteRuntimeClockLocked).
        std::uint64_t clock_displacements{0};
        std::uint64_t clock_unsteady_waits{0};
    };
    // ", quad=..." for summaries over the window since `start` (empty when
    // no frame of the quad compose site ended in it).
    std::string QuadSummaryLocked(const Totals& start) const;
    // ", runtimeClock=..." for summaries (empty while the runtime's display
    // clock stayed steady over the window).
    std::string ClockSummaryLocked(const Totals& start) const;
    Totals totals_;
    Totals summary_totals_;            // at last_summary_
    double summary_standby_seconds_{0.0};
    std::chrono::milliseconds summary_interval_{30000};
    // The benefit guard's window (Dynamic engaged).
    struct GuardWindow {
        bool valid{false};
        std::chrono::steady_clock::time_point start{};
        Totals totals;
    };
    GuardWindow guard_;
    std::array<std::uint32_t, 4> guard_failing_{}; // windows in a row each trigger (a)-(d) failed
    std::vector<double> guard_capture_done_ms_;

    // GPU-aware pacing (VECTORXR_FRAMEGEN_GPU_AHEAD: off, 0, 1, 2 or
    // always): a game wait is held until the capture recorded at the begin
    // of the frame gpu_frames_ahead_ - 1 before the newest begun one has run
    // on the GPU (0: the newest capture). Armed by evidence, then for the
    // session; turned off after kGpuTimeoutsBeforeOff timeouts in a row with
    // no capture running meanwhile (a slow GPU is held for, a stuck one not).
    bool gpu_gate_enabled_{true};
    bool gpu_gate_armed_{false};
    int gpu_frames_ahead_{1};
    std::uint32_t gpu_timeouts_in_row_{0};
    std::uint64_t gpu_reached_mark_{0}; // the newest capture mark seen run (ReachedCaptureMarkLocked)
    std::uint32_t gpu_waiters_{0}; // app threads inside WaitCapture (Stop waits for them)
    std::uint64_t last_capture_mark_{0};
    std::deque<std::uint64_t> begun_capture_marks_; // ThreadedFrame::capture_mark, newest last

    std::uint64_t generated_{0};
    std::uint64_t repeated_{0};
    std::uint64_t failures_{0};
    std::uint64_t demoted_{0};
    std::uint64_t captured_{0};
    mutable std::deque<std::chrono::steady_clock::time_point> app_frames_;
    mutable std::deque<std::chrono::steady_clock::time_point> submissions_;
    mutable std::deque<std::chrono::steady_clock::time_point> distinct_submissions_;
    std::uint64_t repeated_submissions_{0}; // since the last summary
    // The generated set the presenter thread last showed: (Generated::generation, index).
    std::optional<std::pair<std::uint64_t, std::size_t>> last_shown_set_;
    std::uint64_t next_generation_{1};
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
    std::uint32_t logged_depth_decisions_{0};
    std::string logged_hardware_status_; // last optical flow hardware status logged
    bool logged_depth_info_{false};
    bool logged_capture_failure_{false};
    bool logged_uncaptured_{false};
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
    std::uint64_t drain_started_slot_{0}; // slot_index_ when the drain began (kDrainRefreshes)
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
    // just adds latency. That is why a GPU-bound game's frames rarely show
    // the slack that would take a slot off (DCS, 2026-10-09: captured ~9 ms
    // before their refresh at a lead of 5 and of 8 alike, the lead climbing
    // after each burst of hitches): after a calm while a slot is taken off
    // anyway (a probe, FrameGenLeadPolicy).
    std::uint32_t lead_slots_{0};
    std::deque<std::uint8_t> lead_history_; // last frames at the current lead: 0 on time, 1 late, 2 slack
    FrameGenLeadPolicy lead_policy_;
    std::chrono::steady_clock::time_point lead_calm_since_{}; // the lead's last step (none yet: unset)
    struct LeadProbe {
        std::chrono::steady_clock::time_point at;
        std::uint32_t from{0}; // the lead before it
        // Its first frames are not judged: the game's pacing still follows
        // the lead before it.
        std::uint32_t grace{2};
        bool slack{false}; // a step down for frames with a refresh to spare, not a calm probe
    };
    std::optional<LeadProbe> lead_probe_; // a probe being judged
    std::uint32_t lead_floor_{0};         // the lowest lead known to hold (0: none yet)
    // lead_floor_ as a probe last set it (held, or undone above it): frames
    // late at the floor lift it one refresh above this at most.
    std::uint32_t lead_floor_proven_{0};
    // The wait before a try below lead_floor_proven_: explore_after, doubled
    // by each failure there.
    std::chrono::milliseconds lead_explore_after_{10000};
    // The wait before a probe back to lead_floor_proven_ from a floor late
    // frames raised: explore_after, doubled by each that failed (a hitch can
    // come during one), explore_after again once one holds or a probe proves
    // another lead (UpdateLeadLocked).
    std::chrono::milliseconds lead_return_after_{10000};
    // Frames were late at lead_floor_ or below it (a probe or step down
    // undone): steps down for slack below it also wait, as calm probes do.
    bool lead_floor_failed_{false};
    // A new run measures its lead afresh (StartCompositorLocked, Start).
    void ResetLeadLocked();
    // Dynamic: refreshes between real frames' slots, smoothed, and the slot
    // the last real frame was shown in.
    double dynamic_interval_slots_{0.0};
    std::optional<std::uint64_t> last_offer_slot_;
    std::uint32_t dropped_unbegun_{0}; // dropped frames whose begin is still to come
    std::uint32_t dropped_unended_{0}; // dropped frames whose end is still to come
    // Runtime frame calls that succeeded this session.
    std::uint64_t runtime_waits_{0};
    std::uint64_t runtime_begins_{0};
    std::uint64_t runtime_ends_{0};
    bool logged_order_guard_{false};
    double display_offset_periods_{0.0};
    std::uint32_t display_offset_samples_{0};
    // For the watchdog logs and PresenterActivity (read without locks).
    std::atomic<const char*> compositor_phase_{"stopped"};
    bool logged_watchdog_{false};
    // The presenter thread (its Windows thread id), the runtime call it is in
    // (PresenterCall) and since when (steady clock, ns): read without locks.
    std::atomic<std::uint32_t> compositor_thread_id_{0};
    std::atomic<const char*> presenter_call_{nullptr};
    std::atomic<std::int64_t> presenter_call_since_{0};
    std::atomic<std::int64_t> teardown_wait_ms_{2000};
    std::atomic<bool> abandoned_{false};
    bool logged_abandoned_start_{false};
    bool logged_swallowed_{false};
    std::optional<SlotRequest> slot_request_;
    std::optional<SlotOffer> slot_offer_;
    bool offer_done_{false};
    FrameCopy newest_copy_;              // the newest real frame's layers
    FrameCopy passthrough_copy_;         // the last frame passed straight through
    bool generation_pending_{false};     // frames are being generated from newest_copy_
    bool generated_ready_{false};        // extrapolated_ belongs to newest_copy_
    std::size_t next_generated_set_{0};
    std::uint64_t skipped_generated_{0}; // generated frames passed over: made for a refresh already past
    // Just-in-time presentation (latched when the presenter thread starts).
    bool jit_{false};
    std::deque<JitFrame> jit_frames_; // oldest first
    // The last frame the presenter thread submitted (real frame id, mode,
    // fraction), for distinct-frame counting, and what was submitted: the
    // layers, with the views showing Frame Generation's output images when
    // it wrote the frame, or, shown as it is, without (a refresh nothing is
    // written for repeats exactly that, never an older frame).
    std::optional<std::tuple<std::uint64_t, int, float>> jit_shown_;
    std::vector<XrCompositionLayerProjectionView> jit_views_;
    FrameCopy jit_content_;
    bool jit_shown_as_is_{false}; // jit_content_ was shown as it is (jit_views_ empty)...
    std::uint64_t jit_content_id_{0}; // ...of this real frame (0: one passed through)
    XrTime jit_content_time_{0};  // ...made for this display time
    bool jit_exact_{false}; // they were a real frame's exact copy
    // The presenter thread began a runtime frame and did not end it (nothing
    // current to show): the runtime shows its last frame again, and the next
    // begin discards it.
    bool presenter_frame_open_{false};
    bool jit_interpolate_{false}; // the presenter thread interpolates (latched with jit_)
    // Just in time, the real frames a canvas's ring keeps
    // (FrameGenTuning::history_ring): chosen as the presenter thread starts,
    // five where video memory allows, else three (CanvasRingLocked).
    std::uint8_t jit_canvas_ring_{3};
    // Extrapolating just in time, a frame whose motion is not measured yet
    // is moved by the previous real frame's (VECTORXR_FRAMEGEN_STALE_MOTION=0:
    // shown unchanged instead).
    bool stale_motion_enabled_{true};
    bool logged_canvas_ring_{false};
    std::uint8_t CanvasRingLocked();
    // The real frames the backend keeps for a frame of this path.
    std::uint8_t JitRingLocked(bool quad) const {
        return quad ? FrameGenHistoryRing(true) : jit_canvas_ring_;
    }
    bool logged_jit_{false};
    bool jit_started_{false}; // a frame was written just in time since the presenter thread started
    std::uint64_t newest_copy_id_{0}; // the ThreadedFrame::id newest_copy_ came from
    bool newest_copy_fresh_{false};   // newest_copy_ came from a frame ended since the presenter thread started

    // Quad views (FrameGenQuadFrame), under mutex_. Offered: the layer's quad
    // compose site offers frames Frame Generation could compose (latched
    // into quad_mode_ when the presenter thread starts); a switch between
    // composing them and the layer's composed views restarts that thread.
    bool quad_offered_{false};
    bool quad_mode_{false};
    bool quad_switch_pending_{false};
    std::optional<std::uint64_t> quad_claim_id_; // the ThreadedFrame claimed and not yet ended
    std::array<FrameGenExtent, 4> quad_extents_{};
    std::array<QuadCapture, 4> quad_captures_{};
    // Writing composed frames kept failing: the layer composes them until
    // then (doubling from quad_decline_first_ up to quad_decline_longest_).
    std::chrono::steady_clock::time_point quad_declined_until_{};
    bool interpolate_on_shaders_{false};
    bool logged_interpolate_on_shaders_{false};
    std::chrono::milliseconds quad_decline_first_{10000};
    std::chrono::milliseconds quad_decline_longest_{80000};
    std::chrono::milliseconds quad_decline_{10000};
    std::uint64_t quad_write_declines_{0};
    bool quad_decline_resumes_{false}; // a decline started: log when composing resumes
    std::uint32_t quad_unwritten_in_row_{0}; // presenter thread: attempted writes that failed in a row
    std::uint32_t logged_quad_write_failures_{0};
    bool logged_quad_lost_claim_{false};
    bool logged_quad_unanalyzed_{false};
    // Presenter thread (and Stop): output images replaced while composing
    // quad views, kept alive while the last frame written still shows them
    // (until a frame is written into the new ones, or the thread exits).
    std::vector<Output> retired_outputs_;
    std::uint64_t jit_covers_{0};   // refreshes shown past the newest real frame while the next was late (summary)
    XrTime last_target_time_{0};    // just in time: the display time the newest waited frame was told
    // Real frames generated from, and how many of them used the game's depth,
    // since the last summary.
    std::uint64_t depth_window_frames_{0};
    std::uint64_t depth_window_used_{0};
    // What the last generated frames knew about the game's depth (Status).
    bool depth_seen_{false};     // views were generated at all
    bool depth_sent_{false};     // they carried depth
    bool depth_readable_{false}; // and the backend could read it
    FrameGenDepthStatus depth_status_;
    // How far generated frames moved with the eye (the last one per real
    // frame, millimeters) since the last summary.
    double shift_window_sum_mm_{0.0};
    double shift_window_max_mm_{0.0};
    std::uint64_t shift_window_frames_{0};
    // How far the first eye moved between real frames, and how far that was
    // from continuing its movement between the two before (the eye shift's
    // prediction), millimeters, since the last summary.
    std::optional<std::array<double, 3>> eye_velocity_; // meters per second
    std::uint64_t eye_velocity_sequence_{0};
    double eye_move_sum_mm_{0.0};
    double eye_miss_sum_mm_{0.0};
    std::uint64_t eye_frames_{0};
    // When generation ran on the GPU: its start after the real frame's slot
    // was released and its end after the first generated slot was,
    // milliseconds, since the last summary (FrameGenGpu::LastGpuWindow).
    struct GpuDue {
        std::uint64_t submission{0};
        std::chrono::steady_clock::time_point real_due{};
        std::chrono::steady_clock::time_point generated_due{};
    };
    std::deque<GpuDue> gpu_due_;
    std::vector<double> gpu_start_ms_;
    std::vector<double> gpu_done_ms_;
    // Analyses on the GPU (FrameGenGpu::TakeTimings): their newest capture's
    // end after the real frame's refresh was due, their length, and the
    // optical flow hardware's part, milliseconds, since the last summary.
    std::vector<double> capture_done_ms_;
    std::vector<double> analysis_ms_;
    std::vector<double> measure_lag_ms_; // an analysis' end after its newest capture's
    std::vector<double> ofa_ms_;
    // An analysis's end after its newest capture's, smoothed (milliseconds):
    // a real frame moved at its first refresh needs that much more lead.
    double analysis_lag_ms_{0.0};
    // Since the last summary: game waits held for its GPU, the game's
    // xrEndFrame time in EndFrame, the runtime's xrEndFrame time for frames
    // submitted (RuntimeEnd), the presenter thread's wait for the GPU lock to
    // write a frame, and the longest pause, milliseconds.
    std::vector<double> gpu_wait_ms_;
    std::vector<double> end_call_ms_;
    std::vector<double> runtime_end_ms_;
    std::vector<double> synth_lock_ms_;
    double pause_longest_ms_{0.0};
    std::uint32_t logged_pauses_{0};
    // Research capture: a file %LOCALAPPDATA%\VectorXR\framegen-capture.request
    // (checked once a second) writes the next kCaptureFrames real frames'
    // inputs under %LOCALAPPDATA%\VectorXR\captures (FrameGenGpu::DumpNextExecute).
    // Under gpu_mutex_.
    static constexpr std::uint32_t kCaptureFrames = 5;
    std::string NextCaptureDirectory();
    std::chrono::steady_clock::time_point last_capture_check_{};
    std::uint32_t capture_frames_left_{0};
    std::uint32_t capture_index_{0};
    std::string capture_root_;
    // Timed: teardown never waits on them for long (LockForTeardown).
    std::timed_mutex submit_mutex_;      // presenter-thread submissions vs output swapchain calls
    std::timed_mutex gpu_mutex_;         // the GPU backend (Track, Capture, Execute, WaitIdle, Forget, Reset)
    double gpu_ms_{-1.0};                // last generation GPU time, read without the backend
    // The GPU did not finish this session's Frame Generation work before
    // teardown: swapchains it may still use are never reported idle.
    bool gpu_stalled_{false};
};

} // namespace depthxr
