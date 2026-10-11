#include <windows.h>

#include "depthxr/framegen_presenter.h"

#include "depthxr/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace depthxr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kOverlapWait{250};
constexpr std::chrono::seconds kClaimedFrameWait{5};
constexpr XrDuration kOutputWaitTimeout = 100'000'000; // 100 ms
constexpr std::uint32_t kFailuresBeforePause = 3;
constexpr std::uint32_t kDemotionsBeforePause = 3;
constexpr std::uint32_t kMaxMultiplier = 4;
// Times a session the video memory ceiling may rise again (see UpdateAutoLocked).
constexpr std::uint32_t kMemoryRecoveries = 2;
constexpr std::chrono::seconds kPhaseLifetime{2};
constexpr int kAutoMaxBackoffDoublings = 3; // 10 s, 20 s, 40 s, 80 s
// Auto stays at full rate this long after inline cycles kept starting late.
constexpr std::chrono::seconds kInlineHold{60};
// The presenter thread waits this long for frames in flight when it stops.
constexpr std::chrono::seconds kThreadedDrain{2};
// Dynamic: a stand-down this soon after engaging doubles the next hold; this
// long engaged without one, the hold starts over at the policy's.
constexpr std::chrono::seconds kDynamicShortEngagement{60};
constexpr std::chrono::minutes kDynamicLongEngagement{5};
// GPU-aware pacing turns itself off for the session after this many game
// waits in a row timed out (a backend whose captures never report done).
constexpr std::uint32_t kGpuTimeoutsBeforeOff = 5;
// A gap between real frames is a pause past four of the game's own frame
// intervals, and never shorter than kPauseFloor (a hitch of a slow game is
// not one) nor longer than kPauseCeiling (a loading stall always is).
constexpr std::chrono::milliseconds kPauseFloor{250};
constexpr std::chrono::milliseconds kPauseCeiling{1000};
// Dynamic probes standing by only for a game whose engaged rate is within
// this share of the refresh rate of stay_above (what it runs at standing by
// is not much faster: the generated frames' own cost is about that).
constexpr double kProbeReach = 0.10;
// Pauses logged one by one per session; later ones only in summaries.
constexpr std::uint32_t kLoggedPauses = 3;
// Summary sample windows keep at most this many values.
constexpr std::size_t kMaxSamples = 4096;
// Composing quad views, this many attempted writes failing in a row (about
// 110 ms at 90 Hz) hand them back to the layer for a while.
constexpr std::uint32_t kQuadUnwrittenBeforeDecline = 10;
// Composing quad views: first few write failures logged one by one.
constexpr std::uint32_t kLoggedQuadWriteFailures = 3;

std::string MultiplierText(std::uint32_t multiplier) {
    return std::to_string(multiplier) + "x";
}

const XrCompositionLayerProjection* FindProjection(const XrFrameEndInfo& info, std::uint32_t* index = nullptr) {
    for (std::uint32_t i = 0; i < info.layerCount; ++i) {
        const auto* base = info.layers ? info.layers[i] : nullptr;
        if (base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (projection->viewCount > 0 && projection->views) {
                if (index) *index = i;
                return projection;
            }
        }
    }
    return nullptr;
}

FrameGenViewGeometry Geometry(const XrCompositionLayerProjectionView& view) {
    FrameGenViewGeometry geometry;
    geometry.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                            view.pose.orientation.w};
    geometry.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    geometry.angle_left = view.fov.angleLeft;
    geometry.angle_right = view.fov.angleRight;
    geometry.angle_up = view.fov.angleUp;
    geometry.angle_down = view.fov.angleDown;
    return geometry;
}

// A head moves well under 2 m/s (meters per second); eyes that moved faster
// between two real frames are a tracking glitch.
constexpr double kMaxEyeSpeed = 2.0;

// `v` rotated by the unit quaternion `q`, and by its inverse.
std::array<double, 3> Rotate(const XrQuaternionf& q, const std::array<double, 3>& v) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    // v + 2 w (u x v) + 2 u x (u x v), u = q.xyz.
    const std::array<double, 3> uv{y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]};
    const std::array<double, 3> uuv{y * uv[2] - z * uv[1], z * uv[0] - x * uv[2], x * uv[1] - y * uv[0]};
    return {v[0] + 2.0 * (w * uv[0] + uuv[0]), v[1] + 2.0 * (w * uv[1] + uuv[1]), v[2] + 2.0 * (w * uv[2] + uuv[2])};
}

std::array<double, 3> RotateInverse(const XrQuaternionf& q, const std::array<double, 3>& v) {
    return Rotate(XrQuaternionf{-q.x, -q.y, -q.z, q.w}, v);
}

// The depth chained to a projection view, if any.
const XrCompositionLayerDepthInfoKHR* DepthInfo(const XrCompositionLayerProjectionView& view) {
    for (auto* next = static_cast<const XrBaseInStructure*>(view.next); next; next = next->next) {
        if (next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            return reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next);
        }
    }
    return nullptr;
}

FrameGenExtent Extent(const XrCompositionLayerProjectionView& view) {
    return {static_cast<std::uint32_t>(std::max(0, view.subImage.imageRect.extent.width)),
            static_cast<std::uint32_t>(std::max(0, view.subImage.imageRect.extent.height))};
}

std::string Number(double value, int decimals) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

// "median/largest" of a summary window, "-" when empty.
std::string MedianAndMax(std::vector<double> values) {
    if (values.empty()) {
        return "-";
    }
    std::sort(values.begin(), values.end());
    return Number(values[values.size() / 2], 1) + "/" + Number(values.back(), 1);
}

std::optional<double> Median(std::vector<double> values) {
    if (values.empty()) {
        return std::nullopt;
    }
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
    return values[values.size() / 2];
}

void AddSample(std::vector<double>& samples, double value) {
    if (samples.size() < kMaxSamples) {
        samples.push_back(value);
    }
}

double Milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Per second over a window of `seconds` (0 when too short to say).
double PerSecond(std::uint64_t count, double seconds) {
    return seconds > 0.05 ? static_cast<double>(count) / seconds : 0.0;
}

double RatePerSecond(std::deque<Clock::time_point>& stamps, Clock::time_point now) {
    while (!stamps.empty() && now - stamps.front() > std::chrono::seconds(1)) {
        stamps.pop_front();
    }
    if (stamps.size() < 2) {
        return 0.0;
    }
    const double span = std::chrono::duration<double>(stamps.back() - stamps.front()).count();
    return span > 0.0 ? static_cast<double>(stamps.size() - 1) / span : 0.0;
}

// As RatePerSecond, counting only the stamps after `since` (frames up to a
// switch, the one whose end made it included, say nothing about the frames
// after it, nor does the gap the switch itself takes).
double RateSince(std::deque<Clock::time_point>& stamps, Clock::time_point now, Clock::time_point since) {
    RatePerSecond(stamps, now);
    const auto first = std::upper_bound(stamps.begin(), stamps.end(), since);
    const auto count = stamps.end() - first;
    if (count < 2) {
        return 0.0;
    }
    const double span = std::chrono::duration<double>(stamps.back() - *first).count();
    return span > 0.0 ? static_cast<double>(count - 1) / span : 0.0;
}

// Sleeps with sub-millisecond precision; condition variables and plain
// sleeps round to the 15.6 ms system tick on Windows.
void SleepUntil(void* timer, Clock::time_point deadline) {
    const auto remaining = deadline - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return;
    }
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
        if (SetWaitableTimer(static_cast<HANDLE>(timer), &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(static_cast<HANDLE>(timer), 100) == WAIT_OBJECT_0) {
            return;
        }
    }
    std::this_thread::sleep_until(deadline);
}

} // namespace

const char* ToString(FrameGenDepthState state) {
    switch (state) {
    case FrameGenDepthState::kNotSent: return "not-sent";
    case FrameGenDepthState::kNotReadable: return "not-readable";
    case FrameGenDepthState::kChecking: return "checking";
    case FrameGenDepthState::kInUse: return "in-use";
    case FrameGenDepthState::kEmpty: return "empty";
    case FrameGenDepthState::kUnusable: return "unusable";
    default: return "unknown";
    }
}

const char* ToString(FrameGenState state) {
    switch (state) {
    case FrameGenState::kWaiting:
        return "waiting";
    case FrameGenState::kActive:
        return "active";
    case FrameGenState::kPaused:
        return "paused";
    case FrameGenState::kUnavailable:
        return "unavailable";
    default:
        return "off";
    }
}

FrameGenPresenter::~FrameGenPresenter() {
    // Process teardown: the runtime and graphics device may already be gone,
    // so never call into them here. Sessions end through Stop(), which the
    // layer runs on xrDestroySession and xrDestroyInstance.
    (void)gpu_.release();
    if (compositor_.joinable()) {
        compositor_.detach();
    }
    if (timer_) {
        CloseHandle(static_cast<HANDLE>(timer_));
    }
    if (compositor_timer_) {
        CloseHandle(static_cast<HANDLE>(compositor_timer_));
    }
    if (compositor_wake_) {
        CloseHandle(static_cast<HANDLE>(compositor_wake_));
    }
}

void FrameGenPresenter::BeginSession() {
    // As Start: what an abandoned thread still uses is never reset under it
    // (and its GPU lock may be held for good).
    if (abandoned_) return;
    std::scoped_lock lock(gpu_mutex_);
    gpu_stalled_ = false;
}

void FrameGenPresenter::Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
                              Logger* logger, std::mutex* queue_mutex, GpuFactory factory) {
    Stop();
    std::scoped_lock lock(gpu_mutex_, mutex_);
    if (abandoned_) {
        // What the thread left behind still uses is never reset under it.
        logger_ = logger;
        const std::string reason = "its presenter thread of an earlier session never left the runtime";
        UnavailableLocked(reason);
        if (logger_ && !logged_abandoned_start_) {
            logged_abandoned_start_ = true;
            logger_->Error("Frame Generation stays off for this process: " + reason + ".");
        }
        return;
    }
    session_ = session;
    xr_ = dispatch;
    logger_ = logger;
    queue_mutex_ = queue_mutex;
    if (logger_ && logger_->IsDebugEnabled()) {
        trace_.Start(*logger_);
    }
    started_ = true;
    user_enabled_ = true;
    gpu_stalled_ = false; // the last session's stalled work went with its swapchains
    started_at_ = Clock::now();
    healthy_streak_ = 0;
    automatic_retries_ = 0;
    paused_at_.reset();
    state_ = configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff;
    reason_.clear();
    standby_since_ = started_at_;
    // GPU-aware pacing arms itself once frames reach the GPU late; the
    // variable sets it for measurements: off, held for the newest capture
    // (0) or one or two game frames before it, or always (from the start).
    gpu_gate_enabled_ = true;
    gpu_gate_armed_ = false;
    {
        const char* stale = std::getenv("VECTORXR_FRAMEGEN_STALE_MOTION");
        stale_motion_enabled_ = !(stale && std::string(stale) == "0");
    }
    gpu_frames_ahead_ = 1;
    gpu_timeouts_in_row_ = 0;
    std::string gpu_gate_setting;
    if (const char* ahead = std::getenv("VECTORXR_FRAMEGEN_GPU_AHEAD"); ahead && *ahead) {
        gpu_gate_setting = ahead;
        if (gpu_gate_setting == "off") {
            gpu_gate_enabled_ = false;
        } else if (gpu_gate_setting == "always") {
            gpu_gate_armed_ = true;
        } else if (gpu_gate_setting == "0" || gpu_gate_setting == "1" || gpu_gate_setting == "2") {
            gpu_frames_ahead_ = gpu_gate_setting[0] - '0';
            gpu_gate_armed_ = true;
        } else {
            gpu_gate_setting.clear(); // not a setting: the default
        }
    }
    if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    std::string error;
    gpu_ = factory ? factory(binding_chain, error) : nullptr;
    if (!gpu_) {
        UnavailableLocked(error.empty() ? "no graphics backend" : error);
        return;
    }
    if (xr_.formats) {
        std::uint32_t count = 0;
        if (XR_SUCCEEDED(xr_.formats(session, 0, &count, nullptr)) && count) {
            runtime_formats_.resize(count);
            if (XR_FAILED(xr_.formats(session, count, &count, runtime_formats_.data()))) {
                runtime_formats_.clear();
            }
        }
    }
    if (logger_) {
        logger_->Info(std::string("Frame Generation enabled for this session: backend=") + gpu_->Name() +
                      ", mode=" + ToString(settings_.synthesis_mode) + ", flowQuality=" + ToString(settings_.flow_quality) +
                      ", motionEstimation=" + ToString(settings_.motion_estimation) +
                      "; frame pacing is owned by Frame Generation (Turbo stays off).");
        if (!gpu_gate_setting.empty()) {
            logger_->Info("Frame Generation: GPU-aware pacing set by VECTORXR_FRAMEGEN_GPU_AHEAD=" + gpu_gate_setting +
                          (gpu_gate_enabled_ ? ": the game's waits are held until its GPU has run the capture of " +
                                                   (gpu_frames_ahead_ == 0 ? std::string("its newest frame")
                                                                           : std::to_string(gpu_frames_ahead_) +
                                                                                 " frame(s) before its newest begun one") +
                                                   ", from the start."
                                             : ": off."));
        }
    }
}

void FrameGenPresenter::Stop() {
    trace_.Stop();
    std::unique_lock lock(mutex_);
    if (!started_) {
        return;
    }
    if (!abandoned_ && !StopCompositorLocked(lock, TeardownWait())) {
        AbandonPresenterLocked();
    }
    // From here no release thread starts a capture.
    started_ = false;
    if (abandoned_) {
        // The thread left behind may still use the backend, the output
        // images and the runtime when its call returns: all of them are kept
        // for the process's exit, as is the rest of the session's state.
        state_ = FrameGenState::kOff;
        reason_.clear();
        return;
    }
    // A game wait held for its GPU uses the backend without the GPU lock;
    // it returns within its timeout, before the backend is reset. One that
    // does not (logged) keeps the backend, unreset, and its outputs.
    const bool waiters_returned = idle_cv_.wait_for(lock, TeardownWait(), [&] { return gpu_waiters_ == 0; });
    if (!waiters_returned && logger_) {
        logger_->Error("Frame Generation: a game wait held for its GPU did not return within " +
                       std::to_string(TeardownWait().count()) + " ms of the session's end; its backend is kept.");
    }
    lock.unlock();
    {
        // Finish outstanding GPU work before the runtime frees the output
        // images it writes. Teardown runs between frames, so no EndFrame
        // touches the outputs concurrently.
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (gpu_ && !waiters_returned) {
            gpu_stalled_ = true;
        } else if (gpu_) {
            gpu_->Reset();
            // A backend whose work did not finish keeps what it used; the
            // swapchains that work reads and writes must stay too.
            if (!gpu_->WaitIdle()) {
                gpu_stalled_ = true;
            }
        }
    }
    DestroyOutputs();
    {
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock.lock();
        if (waiters_returned) {
            gpu_.reset();
        } else {
            (void)gpu_.release(); // the wait still reads it
        }
    }
    if (logger_ && (generated_ || repeated_ || failures_ || demoted_)) {
        logger_->Info("Frame Generation session summary: generated=" + std::to_string(generated_) + ", repeated=" +
                      std::to_string(repeated_) + ", failures=" + std::to_string(failures_) + ", demoted=" +
                      std::to_string(demoted_) + ", captured=" + std::to_string(captured_) + ".");
    }
    state_ = FrameGenState::kOff;
    reason_.clear();
    swapchains_.clear();
    output_kinds_.clear();
    end_thread_ = {};
    logged_claimed_wait_ = false;
    last_runtime_submit_ = {};
    last_period_ = 0;
    phase_wall_ = {};
    phase_xr_ = 0;
    auto_multiplier_ = 1;
    auto_memory_ceiling_ = kMaxMultiplier;
    memory_budget_.reset();
    memory_checked_at_ = {};
    memory_room_since_.reset();
    memory_recoveries_ = 0;
    full_rate_ = false;
    cadence_.clear();
    last_app_end_.reset();
    real_end_.reset();
    real_interval_ns_ = 0.0;
    multiplier_changed_at_ = {};
    probe_not_before_ = {};
    probe_backoff_ = auto_initial_backoff_;
    probing_ = false;
    runtime_formats_.clear();
    frames_.clear();
    capture_targets_.clear();
    // A session's frame times say nothing about the next one's.
    last_returned_time_ = 0;
    last_real_ = {};
    extrapolated_ = {};
    consecutive_failures_ = consecutive_demotions_ = 0;
    // A claimed quad frame ending after this is swallowed (EndFrame).
    quad_offered_ = quad_mode_ = quad_switch_pending_ = false;
    quad_claim_id_.reset();
    quad_extents_ = {};
    quad_captures_ = {};
    quad_declined_until_ = {};
    quad_decline_ = quad_decline_first_;
    quad_write_declines_ = 0;
    quad_decline_resumes_ = false;
    quad_unwritten_in_row_ = 0;
    logged_quad_write_failures_ = 0;
    logged_quad_lost_claim_ = logged_quad_unanalyzed_ = false;
    newest_copy_fresh_ = false;
    generated_ = repeated_ = failures_ = demoted_ = captured_ = 0;
    threaded_frames_.clear();
    dropped_unended_ = 0;
    threaded_disabled_ = logged_threaded_ = false;
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
    presenter_frame_open_ = false;
    logged_order_guard_ = logged_watchdog_ = false;
    display_offset_periods_ = 0.0;
    display_offset_samples_ = 0;
    slot_index_ = 0;
    slot_time_ = slot_period_ = 0;
    app_latency_ms_ = 0.0;
    passthrough_copy_ = {};
    jit_covers_ = 0;
    logged_jit_ = false;
    ResetLeadLocked();
    dynamic_interval_slots_ = 0.0;
    dynamic_standby_ = true;
    dynamic_below_since_.reset();
    dynamic_hold_until_ = {};
    dynamic_hold_ = dynamic_policy_.hold;
    dynamic_probe_after_ = dynamic_policy_.probe_after;
    dynamic_engaged_at_ = {};
    dynamic_probing_ = false;
    passthrough_fps_ = 0.0;
    dynamic_reason_.clear();
    dynamic_engagements_ = dynamic_stand_downs_ = dynamic_probes_ = 0;
    standby_since_.reset();
    standby_seconds_ = 0.0;
    totals_ = {};
    summary_totals_ = {};
    summary_standby_seconds_ = 0.0;
    last_summary_ = {};
    guard_ = {};
    guard_failing_ = {};
    guard_capture_done_ms_.clear();
    gpu_gate_armed_ = false;
    gpu_timeouts_in_row_ = 0;
    gpu_reached_mark_ = 0;
    last_capture_mark_ = 0;
    begun_capture_marks_.clear();
    gpu_due_.clear();
    gpu_start_ms_.clear();
    gpu_done_ms_.clear();
    capture_done_ms_.clear();
    analysis_ms_.clear();
    measure_lag_ms_.clear();
    ofa_ms_.clear();
    analysis_lag_ms_ = 0.0;
    gpu_wait_ms_.clear();
    end_call_ms_.clear();
    synth_lock_ms_.clear();
    pause_longest_ms_ = 0.0;
    logged_pauses_ = 0;
    last_offer_slot_.reset();
    dropped_unbegun_ = 0;
    gpu_ms_ = -1.0;
    inline_hold_until_.reset();
    last_cycle_started_late_ = false;
    app_frames_.clear();
    submissions_.clear();
    distinct_submissions_.clear();
    repeated_submissions_ = 0;
    last_shown_set_.reset();
    logged_first_generation_ = logged_capture_failure_ = logged_depth_ = logged_uncaptured_ = false;
    logged_depth_decisions_ = 0;
    logged_depth_info_ = false;
    session_ = XR_NULL_HANDLE;
    idle_cv_.notify_all();
}

bool FrameGenPresenter::OwnsPacing() const {
    std::scoped_lock lock(mutex_);
    return started_ && gpu_ && state_ != FrameGenState::kUnavailable;
}

bool FrameGenPresenter::Started() const {
    std::scoped_lock lock(mutex_);
    return started_;
}

void FrameGenPresenter::SetTraceEnabled(bool enabled) {
    Logger* logger = nullptr;
    {
        std::scoped_lock lock(mutex_);
        if (!started_ || abandoned_) {
            return;
        }
        logger = logger_;
    }
    // Outside the presenter lock: stopping joins the trace's writer.
    if (!enabled) {
        trace_.Stop();
    } else if (logger) {
        trace_.Start(*logger);
    }
}

void FrameGenPresenter::Configure(const FrameGenResolvedSettings& settings) {
    std::scoped_lock lock(mutex_);
    // Just in time, the presenter thread keeps the mode it started with
    // (StayThreadedLocked): another one starts it again, after a fresh run of
    // steady frames, as turning Frame Generation off and on does. Frames
    // engaged inline in between would keep it from starting again (an app
    // that waits before ending its last frame always has one in flight).
    const bool restart = jit_ && compositor_running_ && settings.synthesis_mode != settings_.synthesis_mode;
    settings_ = settings;
    if (restart) {
        healthy_streak_ = 0;
        ++totals_.restarts; // the benefit guard's window says nothing about either mode
        if (state_ == FrameGenState::kActive) {
            SetStateLocked(FrameGenState::kWaiting, "");
        }
        if (logger_) {
            logger_->Info(std::string("Frame Generation restarts for ") + ToString(settings_.synthesis_mode) +
                          " once frames are steady again.");
        }
    }
}

void FrameGenPresenter::SetConfigured(bool enabled) {
    std::scoped_lock lock(mutex_);
    if (configured_enabled_ == enabled) {
        return;
    }
    configured_enabled_ = enabled;
    healthy_streak_ = 0;
    if (enabled) {
        DynamicReenableLocked(Clock::now());
    }
    if (started_ && state_ != FrameGenState::kUnavailable && state_ != FrameGenState::kPaused) {
        SetStateLocked(enabled && user_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
    }
}

void FrameGenPresenter::SetAutoPolicy(std::chrono::milliseconds settle, std::chrono::milliseconds steady,
                                      std::chrono::milliseconds probe_confirm, std::chrono::milliseconds probe_backoff) {
    std::scoped_lock lock(mutex_);
    auto_settle_ = settle;
    auto_steady_ = steady;
    auto_probe_confirm_ = probe_confirm;
    auto_initial_backoff_ = probe_backoff;
    probe_backoff_ = probe_backoff;
}

void FrameGenPresenter::SetLeadPolicy(const FrameGenLeadPolicy& policy) {
    std::scoped_lock lock(mutex_);
    lead_policy_ = policy;
    lead_explore_after_ = policy.explore_after;
}

void FrameGenPresenter::ResetLeadLocked() {
    lead_slots_ = 0;
    lead_history_.clear();
    lead_calm_since_ = {};
    lead_probe_.reset();
    lead_floor_ = 0;
    lead_explore_after_ = lead_policy_.explore_after;
    lead_floor_failed_ = false;
}

void FrameGenPresenter::SetDynamicPolicy(const FrameGenDynamicPolicy& policy) {
    std::scoped_lock lock(mutex_);
    dynamic_policy_ = policy;
    dynamic_hold_ = policy.hold;
    dynamic_probe_after_ = policy.probe_after;
}

void FrameGenPresenter::SetSummaryInterval(std::chrono::milliseconds interval) {
    std::scoped_lock lock(mutex_);
    summary_interval_ = interval;
}

void FrameGenPresenter::SetQuadDecline(std::chrono::milliseconds first, std::chrono::milliseconds longest) {
    std::scoped_lock lock(mutex_);
    quad_decline_first_ = first;
    quad_decline_longest_ = std::max(first, longest);
    quad_decline_ = first;
}

void FrameGenPresenter::SetInterpolateOnShaders(bool on) {
    std::scoped_lock lock(mutex_);
    interpolate_on_shaders_ = on;
    logged_interpolate_on_shaders_ = false;
}

void FrameGenPresenter::SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age) {
    std::scoped_lock lock(mutex_);
    engage_streak_frames_ = steady_frames;
    engage_min_age_ = minimum_session_age;
}

void FrameGenPresenter::SetMemoryRecoveryDelay(std::chrono::milliseconds delay) {
    std::scoped_lock lock(mutex_);
    memory_recovery_delay_ = delay;
}

void FrameGenPresenter::SetRetryPolicy(std::chrono::milliseconds delay, std::uint32_t attempts) {
    std::scoped_lock lock(mutex_);
    retry_delay_ = delay;
    retry_attempts_ = attempts;
}

bool FrameGenPresenter::Toggle() {
    std::scoped_lock lock(mutex_);
    if (state_ == FrameGenState::kPaused) {
        // A press after a pause retries rather than turning the feature off.
        user_enabled_ = true;
        consecutive_failures_ = consecutive_demotions_ = 0;
        paused_at_.reset();
        DynamicReenableLocked(Clock::now());
        SetStateLocked(configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
    } else {
        user_enabled_ = !user_enabled_;
        if (user_enabled_) {
            // Turned back on: Auto gets a fresh look, not the rest of a
            // full-rate hold from before, and Dynamic judges the game's rate
            // again before engaging.
            inline_hold_until_.reset();
            DynamicReenableLocked(Clock::now());
        }
        if (state_ != FrameGenState::kUnavailable) {
            SetStateLocked(user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
        }
    }
    healthy_streak_ = 0;
    return user_enabled_;
}

bool FrameGenPresenter::ResetFrames() {
    std::unique_lock lock(mutex_);
    // The session is starting or stopping: the presenter thread must not
    // touch the runtime frame loop past this point. One still inside a
    // runtime call is left stopping (the runtime may hold it until the
    // session ends: the layer ends it, then stops the thread again).
    if (!abandoned_ && !StopCompositorLocked(lock, TeardownWait())) {
        return false;
    }
    dropped_unended_ = 0;
    dropped_unbegun_ = 0;
    // A new session starts with no runtime frame in flight.
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
    presenter_frame_open_ = false;
    frames_.clear();
    healthy_streak_ = 0;
    InvalidateContinuityLocked();
    if (state_ == FrameGenState::kActive) {
        SetStateLocked(IdleStateLocked(), "");
    }
    idle_cv_.notify_all();
    return true;
}

void FrameGenPresenter::SetTeardownWait(std::chrono::milliseconds wait) {
    teardown_wait_ms_ = std::max<std::int64_t>(wait.count(), 1);
}

std::chrono::milliseconds FrameGenPresenter::TeardownWait() const {
    return std::chrono::milliseconds(teardown_wait_ms_.load());
}

bool FrameGenPresenter::StopPresenter() {
    std::unique_lock lock(mutex_);
    return abandoned_ || StopCompositorLocked(lock, TeardownWait());
}

void FrameGenPresenter::AbandonPresenter() {
    std::scoped_lock lock(mutex_);
    AbandonPresenterLocked();
}

void FrameGenPresenter::AbandonPresenterLocked() {
    if (!compositor_running_ || !compositor_.joinable()) {
        return;
    }
    // It ends when its call returns (compositor_stop_ stays set); until then
    // it holds what its call uses, so nothing of the session is let go.
    compositor_stop_ = true;
    compositor_.detach();
    abandoned_ = true;
    if (logger_) {
        logger_->Error("Frame Generation leaves its presenter thread behind: it never left the runtime (" +
                       PresenterActivity() +
                       "). Its GPU work, output images and the runtime's session are kept for the process's exit, "
                       "and Frame Generation stays off for the rest of the process.");
    }
}

bool FrameGenPresenter::PresenterAbandoned() const {
    return abandoned_;
}

std::string FrameGenPresenter::PresenterActivity() const {
    std::string text = std::string("presenter thread ") + compositor_phase_.load();
    if (const char* call = presenter_call_.load()) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const double seconds = static_cast<double>(now - presenter_call_since_.load()) / 1e9;
        text += std::string(", in the runtime's ") + call + " for " + Number(seconds, 1) + " s";
    }
    if (abandoned_) {
        text += ", left behind";
    }
    return text;
}

bool FrameGenPresenter::LockForTeardown(std::unique_lock<std::timed_mutex>& lock, const std::string& what) {
    // The presenter thread holds it over runtime calls: a runtime holding
    // one must not hold this caller too. While one call has held it past
    // the teardown wait already, later callers only try briefly.
    const auto limit = PresenterWedged() ? std::chrono::milliseconds(50) : TeardownWait();
    if (lock.try_lock_for(limit)) {
        return true;
    }
    if (logger_) {
        logger_->Error("Frame Generation: " + what + " waited " + std::to_string(limit.count()) +
                       " ms for its presenter thread (" + PresenterActivity() + ") and went on without it.");
    }
    return false;
}

bool FrameGenPresenter::PresenterWedged() const {
    if (!presenter_call_.load()) {
        return false;
    }
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    return now - presenter_call_since_.load() >
           std::chrono::duration_cast<std::chrono::nanoseconds>(TeardownWait()).count();
}

FrameGenPresenter::PresenterCall::PresenterCall(FrameGenPresenter& owner, const char* call) {
    if (GetCurrentThreadId() != owner.compositor_thread_id_.load()) {
        return;
    }
    presenter = &owner;
    owner.presenter_call_since_ =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    owner.presenter_call_ = call;
}

FrameGenPresenter::PresenterCall::~PresenterCall() {
    if (presenter) {
        presenter->presenter_call_ = nullptr;
    }
}

void FrameGenPresenter::AbandonFrame(XrTime display_time) {
    {
        std::scoped_lock lock(mutex_);
        // The oldest open frame is the one ending, as in EndFrame; the time
        // is only a hint (apps may end with another time than they were given).
        (void)display_time;
        if (quad_claim_id_) {
            // Claimed in this xrEndFrame (ClaimQuadFrame), which the layer
            // then returned from: that frame's end never comes.
            const std::uint64_t id = *quad_claim_id_;
            quad_claim_id_.reset();
            threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                  [&](const ThreadedFrame& frame) { return frame.id == id; }),
                                   threaded_frames_.end());
        } else if (dropped_unended_ > 0) {
            --dropped_unended_;
        } else if (!frames_.empty()) {
            frames_.pop_front();
        } else {
            const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                         [](const ThreadedFrame& frame) { return !frame.claimed; });
            if (it != threaded_frames_.end()) {
                threaded_frames_.erase(it);
            }
        }
        healthy_streak_ = 0;
        InvalidateContinuityLocked();
    }
    idle_cv_.notify_all();
    WakeSlots();
}

XrSwapchainUsageFlags FrameGenPresenter::SourceUsage(const XrSwapchainCreateInfo& info) const {
    std::scoped_lock lock(mutex_);
    if (!started_ || !gpu_) {
        return 0;
    }
    if ((info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        return gpu_->SupportsDepth() ? gpu_->RequiredDepthUsage() : 0;
    }
    return gpu_->RequiredSourceUsage();
}

void FrameGenPresenter::TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count,
                                       XrSwapchainUsageFlags usage) {
    std::scoped_lock lock(mutex_);
    if (!started_) {
        return;
    }
    const XrSwapchainUsageFlags required = gpu_ ? gpu_->RequiredSourceUsage() : 0;
    SwapchainRecord record;
    record.format = format;
    record.sample_count = std::max(1u, sample_count);
    record.depth = (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    record.capturable = !record.depth && (usage & required) == required;
    const XrSwapchainUsageFlags depth_required = gpu_ ? gpu_->RequiredDepthUsage() : 0;
    record.depth_capturable = record.depth && gpu_ && gpu_->SupportsDepth() && record.sample_count == 1 &&
                              (usage & depth_required) == depth_required;
    swapchains_[swapchain] = record;
}

bool FrameGenPresenter::ForgetSwapchain(XrSwapchain swapchain) {
    // Holding submit_mutex_ keeps the presenter thread from submitting the
    // swapchain while it goes; afterwards no copy refers to it. A presenter
    // thread inside a runtime call past the teardown wait holds it: the
    // swapchain is forgotten all the same (no later submission names it) and
    // kept (false), as the submission in flight may name it.
    std::unique_lock submit(submit_mutex_, std::defer_lock);
    const bool submit_held = LockForTeardown(submit, "a swapchain being destroyed");
    {
        std::scoped_lock lock(mutex_);
        swapchains_.erase(swapchain);
        capture_targets_.erase(std::remove_if(capture_targets_.begin(), capture_targets_.end(),
                                              [&](const CaptureTarget& target) { return target.swapchain == swapchain; }),
                               capture_targets_.end());
        for (QuadCapture& capture : quad_captures_) {
            if (capture.swapchain == swapchain) capture = {};
        }
        StripSwapchain(newest_copy_, swapchain);
        StripSwapchain(passthrough_copy_, swapchain);
        StripSwapchain(jit_content_, swapchain);
        for (JitFrame& frame : jit_frames_) {
            StripSwapchain(frame.copy, swapchain);
        }
    }
    std::unique_lock gpu_lock(gpu_mutex_, std::defer_lock);
    if (!LockForTeardown(gpu_lock, "a swapchain being destroyed")) {
        // The backend may still use its images: kept.
        return false;
    }
    bool idle = !gpu_stalled_;
    if (gpu_ && idle) {
        // A recent submission may still read or write this swapchain's
        // images; destroys are rare, so always let it finish first.
        idle = gpu_->WaitIdle() || gpu_->WaitIdle() || gpu_->WaitIdle();
    }
    if (gpu_) {
        gpu_->Forget(swapchain);
    }
    if (!idle && logger_) {
        logger_->Error("Frame Generation: the GPU did not finish its work on a swapchain being destroyed within 6 s.");
    }
    return idle && submit_held;
}

bool FrameGenPresenter::ForgetSpace(XrSpace space) {
    // As for swapchains: under submit_mutex_, no presenter-thread submission
    // is using the space, and none will after this. Without it (a presenter
    // thread inside a runtime call past the teardown wait) the space is
    // forgotten all the same and kept (false): the submission in flight may
    // name it.
    std::unique_lock submit(submit_mutex_, std::defer_lock);
    const bool submit_held = LockForTeardown(submit, "a space being destroyed");
    std::scoped_lock lock(mutex_);
    StripSpace(newest_copy_, space);
    StripSpace(passthrough_copy_, space);
    StripSpace(jit_content_, space);
    // Frames generated from a real frame share its projection's space.
    for (auto it = jit_frames_.begin(); it != jit_frames_.end();) {
        if (it->copy.has_projection && it->copy.layers[it->copy.projection_index].projection.space == space) {
            ScoreErasedLocked(*it);
            it = jit_frames_.erase(it);
        } else {
            ++it;
        }
    }
    for (JitFrame& frame : jit_frames_) {
        StripSpace(frame.copy, space); // other layers in that space
    }
    if (jit_content_.has_projection && jit_content_.layers[jit_content_.projection_index].projection.space == space) {
        jit_content_ = {};
        jit_views_.clear();
        jit_shown_as_is_ = false;
    }
    if (extrapolated_.space == space) {
        extrapolated_.valid = false;
        generated_ready_ = false;
    }
    if (last_real_.space == space) {
        InvalidateContinuityLocked();
    }
    return submit_held;
}

void FrameGenPresenter::NoteRelease(XrSwapchain swapchain, std::uint32_t image_index) {
    std::scoped_lock lock(mutex_);
    if (const auto it = swapchains_.find(swapchain); it != swapchains_.end()) {
        it->second.released = image_index;
        ++it->second.releases;
    }
}

std::optional<FrameGenOutputKind> FrameGenPresenter::OutputKindLocked(std::size_t view, std::int64_t source_format) const {
    const auto requested = gpu_ ? gpu_->Classify(source_format) : std::nullopt;
    // Composing quad views, the four views' history never depends on the two
    // eyes' output images: the backend's own images take the composed eyes'.
    if (quad_mode_) {
        return requested;
    }
    if (requested && view < output_kinds_.size() && output_kinds_[view].valid &&
        output_kinds_[view].requested == *requested) {
        return output_kinds_[view].kind;
    }
    return requested;
}

void FrameGenPresenter::CaptureOnRelease(XrSwapchain swapchain, std::uint32_t image_index) {
    auto capturing = [&] {
        if (!started_ || !gpu_ || !user_enabled_ || !configured_enabled_ || state_ == FrameGenState::kPaused ||
            state_ == FrameGenState::kUnavailable) {
            return false;
        }
        // Only while frames are generated or about to be: the wait that
        // engages comes before the game's release, so the first engaged
        // frame is captured. Passing frames straight through (Auto at full
        // rate, Dynamic standing by, waiting for steady frames) captures
        // nothing.
        const bool armed = compositor_running_ || WantsEngagementLocked() ||
                           std::any_of(frames_.begin(), frames_.end(),
                                       [](const PendingFrame& frame) { return frame.engaged && !frame.demoted; });
        if (!armed) {
            return false;
        }
        const auto record = swapchains_.find(swapchain);
        return record != swapchains_.end() && record->second.sample_count == 1 &&
               (record->second.depth ? record->second.depth_capturable : record->second.capturable) &&
               std::any_of(capture_targets_.begin(), capture_targets_.end(),
                           [&](const CaptureTarget& target) { return target.swapchain == swapchain; });
    };
    {
        // Most releases have nothing to capture; check before the queue lock.
        std::scoped_lock lock(mutex_);
        if (!capturing()) {
            return;
        }
    }
    // Lock order everywhere: submit, queue, GPU, presenter. The copy may wait
    // on a fence, so it runs without the presenter lock.
    std::unique_lock<std::mutex> queue_lock;
    if (queue_mutex_) {
        queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
    }
    std::scoped_lock gpu_lock(gpu_mutex_);
    struct Job {
        std::size_t view{0};
        FrameGenImage source;
        FrameGenViewRequest request;
        bool depth{false};
        FrameGenDepthEncoding encoding;
        bool ok{false}; // submitted
    };
    std::vector<Job> jobs;
    FrameGenFlowQuality quality;
    std::uint64_t releases = 0; // the swapchain's releases the jobs capture
    {
        std::scoped_lock lock(mutex_);
        if (!capturing()) {
            return;
        }
        quality = settings_.flow_quality;
        const auto record = swapchains_.find(swapchain);
        releases = record->second.releases;
        for (const CaptureTarget& target : capture_targets_) {
            if (target.swapchain != swapchain) {
                continue;
            }
            if (target.depth) {
                Job job;
                job.view = target.view;
                job.depth = true;
                job.encoding = target.encoding;
                job.source = {swapchain, image_index, target.array_index, target.rect, record->second.format};
                job.request.extent = target.color_extent;
                jobs.push_back(job);
                continue;
            }
            const auto kind = OutputKindLocked(target.view, record->second.format);
            if (!kind) {
                continue;
            }
            Job job;
            job.view = target.view;
            job.source = {swapchain, image_index, target.array_index, target.rect, record->second.format};
            job.request.extent = {static_cast<std::uint32_t>(target.rect.extent.width),
                                  static_cast<std::uint32_t>(target.rect.extent.height)};
            job.request.output = *kind;
            jobs.push_back(job);
        }
    }
    std::uint64_t captured = 0;
    bool color_captured = false;
    std::string failure;
    for (Job& job : jobs) {
        std::string error;
        if (!gpu_->Track(swapchain, xr_.images, error) ||
            !(job.depth ? gpu_->CaptureDepth(job.view, job.source, job.encoding, job.request, error)
                        : gpu_->Capture(job.view, job.source, job.request, quality, error))) {
            // Generation then copies the image itself. An empty error means
            // the view is not set up for this region yet; nothing to report.
            if (failure.empty()) {
                failure = error;
            }
            continue;
        }
        job.ok = true;
        ++captured;
        color_captured = color_captured || !job.depth;
    }
    // Still under the GPU lock: what the game's waits are held for.
    const std::uint64_t mark = captured ? gpu_->CaptureMark() : 0;
    trace_.Record("cap", reinterpret_cast<std::uint64_t>(swapchain), image_index, static_cast<std::int64_t>(captured),
                  static_cast<std::int64_t>(jobs.size()), static_cast<std::int64_t>(releases),
                  static_cast<std::int64_t>(mark));
    std::scoped_lock lock(mutex_);
    captured_ += captured;
    totals_.captured += captured;
    last_capture_mark_ = std::max(last_capture_mark_, mark);
    if (color_captured) {
        if (const auto record = swapchains_.find(swapchain); record != swapchains_.end()) {
            record->second.captured_releases = releases;
        }
    }
    // Each view's latest capture, as Execute matches it (ClaimQuadFrame).
    for (const Job& job : jobs) {
        if (!job.depth && job.view < quad_captures_.size()) {
            quad_captures_[job.view] = {job.ok, swapchain, job.source.image_index, job.source.array_index,
                                        job.source.rect, releases};
        }
    }
    if (logger_ && !failure.empty() && !logged_capture_failure_) {
        logged_capture_failure_ = true;
        logger_->Error("Frame Generation could not capture a released image: " + failure);
    }
}

void FrameGenPresenter::RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection) {
    capture_targets_.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const auto& sub = projection.views[i].subImage;
        CaptureTarget color;
        color.swapchain = sub.swapchain;
        color.array_index = sub.imageArrayIndex;
        color.rect = sub.imageRect;
        color.view = i;
        capture_targets_.push_back(color);
        if (const auto* depth = DepthInfo(projection.views[i])) {
            CaptureTarget target;
            target.swapchain = depth->subImage.swapchain;
            target.array_index = depth->subImage.imageArrayIndex;
            target.rect = depth->subImage.imageRect;
            target.view = i;
            target.depth = true;
            target.encoding = {depth->minDepth, depth->maxDepth, depth->nearZ, depth->farZ};
            target.color_extent = Extent(projection.views[i]);
            capture_targets_.push_back(target);
            if (logger_ && !logged_depth_info_) {
                logged_depth_info_ = true;
                const auto record = swapchains_.find(depth->subImage.swapchain);
                logger_->Info("Frame Generation: the game submits depth described as minDepth=" + Number(depth->minDepth, 3) +
                              ", maxDepth=" + Number(depth->maxDepth, 3) + ", nearZ=" + Number(depth->nearZ, 4) +
                              ", farZ=" + Number(depth->farZ, 1) + ", region " +
                              std::to_string(depth->subImage.imageRect.extent.width) + "x" +
                              std::to_string(depth->subImage.imageRect.extent.height) + ", format " +
                              std::to_string(record != swapchains_.end() ? record->second.format : 0) +
                              (record != swapchains_.end() && record->second.depth_capturable ? ", readable"
                                                                                               : ", not readable") +
                              "; it is checked against the other eye before use.");
            }
        }
    }
}

std::uint8_t FrameGenPresenter::CanvasRingLocked() {
    // Each further slot keeps a real frame per view: its image, and depth,
    // trust and flow at half resolution or less (about one and a half
    // images' worth).
    const FrameCopy& copy = newest_copy_.valid ? newest_copy_ : passthrough_copy_;
    std::uint64_t slot_bytes = 0;
    if (copy.valid && copy.has_projection && copy.projection_index < copy.layers.size()) {
        for (const XrCompositionLayerProjectionView& view : copy.layers[copy.projection_index].views) {
            const std::uint64_t pixels = static_cast<std::uint64_t>(std::max(view.subImage.imageRect.extent.width, 0)) *
                                         static_cast<std::uint64_t>(std::max(view.subImage.imageRect.extent.height, 0));
            slot_bytes += pixels * 6;
        }
    }
    const std::uint8_t roomy = kFrameGenCanvasRingRoomy;
    const std::uint8_t lean = FrameGenHistoryRing(false);
    const std::uint64_t extra = slot_bytes * (roomy - lean);
    const std::optional<FrameGenMemoryBudget> memory = gpu_ ? gpu_->MemoryBudget() : std::nullopt;
    // Unknown sizes or budget: the default. Room means the extra slots and
    // a tenth of the budget still fit.
    const bool room = slot_bytes && memory && memory->budget_bytes &&
                      memory->usage_bytes + extra + memory->budget_bytes / 10 < memory->budget_bytes;
    const std::uint8_t ring = room ? roomy : lean;
    if (logger_ && !logged_canvas_ring_ && slot_bytes && memory && memory->budget_bytes) {
        logged_canvas_ring_ = true;
        constexpr double megabytes = 1024.0 * 1024.0;
        logger_->Info("Frame Generation keeps the last " + std::to_string(ring) + " real frames to generate from" +
                      (room ? " (" + Number(static_cast<double>(extra) / megabytes, 0) +
                                  " MB more video memory than three, so slow games keep every frame they need)."
                            : " (video memory is short: usage " +
                                  Number(static_cast<double>(memory->usage_bytes) / megabytes, 0) + " of " +
                                  Number(static_cast<double>(memory->budget_bytes) / megabytes, 0) + " MB)."));
    }
    return ring;
}

bool FrameGenPresenter::QuadCapableLocked() const {
    return started_ && gpu_ && ThreadedEligibleLocked() && gpu_->SupportsJustInTime() && gpu_->SupportsComposition();
}

bool FrameGenPresenter::QuadTargetsLocked(const FrameGenQuadFrame* quad) const {
    return quad_mode_ || (quad && quad->route != FrameGenQuadRoute::kDeclined && QuadCapableLocked());
}

void FrameGenPresenter::RecordQuadCaptureTargetsLocked(const XrCompositionLayerProjection& source) {
    capture_targets_.clear();
    std::array<FrameGenExtent, 4> extents{};
    for (std::uint32_t i = 0; i < 4 && i < source.viewCount; ++i) {
        const auto& sub = source.views[i].subImage;
        CaptureTarget color;
        color.swapchain = sub.swapchain;
        color.array_index = sub.imageArrayIndex;
        color.rect = sub.imageRect;
        color.view = i;
        capture_targets_.push_back(color);
        extents[i] = Extent(source.views[i]);
    }
    bool changed = false;
    for (std::size_t i = 0; i < extents.size(); ++i) {
        changed = changed || extents[i].width != quad_extents_[i].width || extents[i].height != quad_extents_[i].height;
    }
    if (changed) {
        quad_extents_ = extents;
        if (logger_) {
            logger_->Info("Frame Generation can compose the quad views itself: periphery " +
                          std::to_string(extents[0].width) + "x" + std::to_string(extents[0].height) + ", focus " +
                          std::to_string(extents[2].width) + "x" + std::to_string(extents[2].height) +
                          "; their releases are captured while it presents.");
        }
    }
}

bool FrameGenPresenter::QuadCapturedLocked(const XrCompositionLayerProjection& source) const {
    if (source.viewCount < quad_captures_.size() || !source.views) {
        return false;
    }
    for (std::size_t i = 0; i < quad_captures_.size(); ++i) {
        const XrSwapchainSubImage& sub = source.views[i].subImage;
        const QuadCapture& capture = quad_captures_[i];
        const auto record = swapchains_.find(sub.swapchain);
        if (!capture.ok || record == swapchains_.end() || record->second.sample_count != 1 ||
            !record->second.released || capture.swapchain != sub.swapchain ||
            capture.image_index != *record->second.released || capture.array_index != sub.imageArrayIndex ||
            capture.rect.offset.x != sub.imageRect.offset.x || capture.rect.offset.y != sub.imageRect.offset.y ||
            capture.rect.extent.width != sub.imageRect.extent.width ||
            capture.rect.extent.height != sub.imageRect.extent.height || capture.releases != record->second.releases) {
            return false;
        }
    }
    return true;
}

FrameGenQuadRoute FrameGenPresenter::ClaimQuadFrame(const XrCompositionLayerProjection& source) {
    std::scoped_lock lock(mutex_);
    // Without composition, the layer's composed views are Frame
    // Generation's frames as before (its canvas), captured from them.
    if (!QuadCapableLocked() || source.viewCount < 4 || !source.views) {
        return FrameGenQuadRoute::kNotPresenting;
    }
    quad_offered_ = true;
    RecordQuadCaptureTargetsLocked(source);
    if (!compositor_running_ || !jit_ || !quad_mode_ || quad_switch_pending_ || compositor_drain_ ||
        compositor_stop_ || !StayThreadedLocked() || Clock::now() < quad_declined_until_) {
        return FrameGenQuadRoute::kNotPresenting;
    }
    // The frame ending is the oldest open one, as EndFrame matches it: a
    // just-in-time frame of the presenter thread's, with nothing older.
    if (dropped_unended_ > 0 || !frames_.empty() || quad_claim_id_) {
        return FrameGenQuadRoute::kBusy;
    }
    const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                 [](const ThreadedFrame& frame) { return !frame.claimed; });
    if (it == threaded_frames_.end() || !it->jit) {
        return FrameGenQuadRoute::kBusy;
    }
    if (!QuadCapturedLocked(source)) {
        return FrameGenQuadRoute::kUncaptured;
    }
    // Claimed under the same lock as the decision: no stop drops it (only
    // unclaimed frames are), and the presenter thread drains it.
    it->claimed = true;
    it->quad = true;
    quad_claim_id_ = it->id;
    return FrameGenQuadRoute::kClaimed;
}

void FrameGenPresenter::NoteQuadEndLocked(const FrameGenQuadFrame* quad, bool projection) {
    if (quad) {
        switch (quad->route) {
        case FrameGenQuadRoute::kClaimed: ++totals_.quad_claimed; break;
        case FrameGenQuadRoute::kUncaptured: ++totals_.quad_uncaptured; break;
        case FrameGenQuadRoute::kBusy: ++totals_.quad_busy; break;
        case FrameGenQuadRoute::kDeclined: ++totals_.quad_declined; break;
        default: ++totals_.quad_not_presenting; break;
        }
        if (quad->route == FrameGenQuadRoute::kDeclined) {
            quad_offered_ = false;
        }
    } else if (projection) {
        // A projection that did not come from the quad compose site.
        quad_offered_ = false;
    }
    if (!compositor_running_ || !jit_ || (!quad && !projection)) {
        return;
    }
    // Frames that now want the other composition restart the presenter
    // thread in it, as a synthesis mode switch does.
    const bool composed = quad && quad->route != FrameGenQuadRoute::kDeclined && QuadCapableLocked();
    if (composed != quad_mode_ && !quad_switch_pending_) {
        quad_switch_pending_ = true;
        healthy_streak_ = 0;
        ++totals_.restarts;
        if (logger_) {
            logger_->Info(composed ? "Frame Generation restarts to compose the quad views itself once frames are "
                                     "steady again."
                                   : "Frame Generation restarts on the layer's composed quad views once frames are "
                                     "steady again.");
        }
    }
}

void FrameGenPresenter::DeclineQuadLocked(Clock::time_point now, const std::string& error) {
    quad_unwritten_in_row_ = 0;
    // A decline long after the last one starts the doubling over.
    if (quad_declined_until_ != Clock::time_point{} && now - quad_declined_until_ > std::chrono::minutes(5)) {
        quad_decline_ = quad_decline_first_;
    }
    const std::chrono::milliseconds decline = quad_decline_;
    quad_declined_until_ = now + decline;
    quad_decline_ = std::min(quad_decline_ * 2, quad_decline_longest_);
    ++quad_write_declines_;
    quad_decline_resumes_ = true;
    if (logger_) {
        logger_->Error("Frame Generation could not write " + std::to_string(kQuadUnwrittenBeforeDecline) +
                       " composed frames in a row (" + error + "); it stands by for " +
                       Number(std::chrono::duration<double>(decline).count(), 1) +
                       " s: the layer composes the quad views and its frames pass straight through.");
    }
}

bool FrameGenPresenter::QuadDeclinedLocked() const {
    // Only where the layer offers quad views (composing them is what
    // failed): its composed views alone still generate frames.
    return quad_offered_ && quad_declined_until_ != Clock::time_point{} && Clock::now() < quad_declined_until_ &&
           QuadCapableLocked();
}

std::string FrameGenPresenter::QuadSummaryLocked(const Totals& start) const {
    const std::uint64_t claimed = totals_.quad_claimed - start.quad_claimed;
    const std::uint64_t uncaptured = totals_.quad_uncaptured - start.quad_uncaptured;
    const std::uint64_t busy = totals_.quad_busy - start.quad_busy;
    const std::uint64_t declined = totals_.quad_declined - start.quad_declined;
    const std::uint64_t not_presenting = totals_.quad_not_presenting - start.quad_not_presenting;
    if (!quad_mode_ && claimed + uncaptured + busy + declined + not_presenting == 0) {
        return {};
    }
    // fgComposed: frames Frame Generation composed; d3d11Composed: frames
    // the layer composed (not captured / not a frame of its own / declined
    // by the layer / not presenting); setups: views set up from a frame.
    return ", quad=" + std::to_string(quad_mode_ && compositor_running_) + ", fgComposed=+" + std::to_string(claimed) +
           ", d3d11Composed=+" + std::to_string(uncaptured + busy + declined + not_presenting) + " (" +
           std::to_string(uncaptured) + "/" + std::to_string(busy) + "/" + std::to_string(declined) + "/" +
           std::to_string(not_presenting) + "), setups=+" + std::to_string(totals_.quad_setups - start.quad_setups) +
           ", claimedUnanalyzed=+" + std::to_string(totals_.quad_unanalyzed - start.quad_unanalyzed) +
           (Clock::now() < quad_declined_until_ ? ", quadDeclined=1" : "");
}

bool FrameGenPresenter::RunnableLocked() const {
    return started_ && gpu_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && healthy_streak_ >= engage_streak_frames_ &&
           Clock::now() - started_at_ >= engage_min_age_;
}

bool FrameGenPresenter::WantsEngagementLocked() const {
    return RunnableLocked() && EffectiveMultiplierLocked() >= 2 && !QuadDeclinedLocked();
}

FrameGenState FrameGenPresenter::IdleStateLocked() const {
    return user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff;
}

bool FrameGenPresenter::StayThreadedLocked() const {
    // Just in time, the presenter thread interpolates or extrapolates as it
    // started: another mode starts it again.
    // Likewise frames that want the other quad views composition. While
    // composed quad frames could not be written for a while, nothing is
    // presented from it: the layer's frames pass straight through (shown as
    // they are by the presenter thread, the runtime could show a newer
    // composed image with an older frame's views).
    const bool same_mode = !jit_ || jit_interpolate_ == (settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate);
    return started_ && gpu_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && EffectiveMultiplierLocked() >= 2 && same_mode &&
           !quad_switch_pending_ && !QuadDeclinedLocked();
}

bool FrameGenPresenter::ThreadedEligibleLocked() const {
    return gpu_ && gpu_->SupportsThreadedPresentation() && !threaded_disabled_;
}

std::uint32_t FrameGenPresenter::EffectiveMultiplierLocked() const {
    switch (settings_.multiplier) {
    case FrameGenMultiplier::k2:
        return 2;
    case FrameGenMultiplier::k3:
        return 3;
    case FrameGenMultiplier::k4:
        return 4;
    case FrameGenMultiplier::kDynamic:
        // Standing by is full rate: frames pass straight through.
        return DynamicLocked() ? (dynamic_standby_ ? 1 : DynamicSlotsLocked()) : auto_multiplier_;
    default:
        return auto_multiplier_;
    }
}

XrDuration FrameGenPresenter::PauseLimitLocked() const {
    // A hitch of a game at 30 fps (a frame of five or six refreshes) is part
    // of its pace: frames are still measured and covered across it.
    const auto own = std::chrono::nanoseconds(static_cast<std::int64_t>(4.0 * real_interval_ns_));
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::max<Clock::duration>(kPauseFloor, std::min<Clock::duration>(own, kPauseCeiling)))
        .count();
}

void FrameGenPresenter::NoteRealEndLocked(Clock::time_point end) {
    if (real_end_ && end > *real_end_) {
        const double interval = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - *real_end_).count());
        // A pause is not the game's pace.
        if (interval <= static_cast<double>(PauseLimitLocked())) {
            real_interval_ns_ = real_interval_ns_ > 0.0 ? real_interval_ns_ * 0.9 + interval * 0.1 : interval;
        }
    }
    real_end_ = end;
}

double FrameGenPresenter::StandbySecondsLocked(Clock::time_point now) const {
    double seconds = standby_seconds_;
    if (standby_since_ && dynamic_standby_ && DynamicLocked() && now > *standby_since_) {
        seconds += std::chrono::duration<double>(now - *standby_since_).count();
    }
    return seconds;
}

std::string FrameGenPresenter::DynamicSummaryLocked() const {
    if (!DynamicLocked()) {
        return {};
    }
    return std::string(", dynamic=") + (dynamic_standby_ ? (dynamic_probing_ ? "probing" : "standby") : "engaged") +
           (dynamic_reason_.empty() ? std::string() : " (" + dynamic_reason_ + ")");
}

void FrameGenPresenter::NotePauseLocked(XrDuration gap) {
    ++totals_.pauses;
    const double gap_ms = static_cast<double>(gap) / 1e6;
    pause_longest_ms_ = std::max(pause_longest_ms_, gap_ms);
    if (logger_ && logged_pauses_ < kLoggedPauses) {
        ++logged_pauses_;
        logger_->Info("Frame Generation: the game sent no frame for " + Number(gap_ms / 1000.0, 2) +
                      " s; nothing is generated across the gap.");
    }
}

bool FrameGenPresenter::DynamicLocked() const {
    // Interpolating, the frames between two real frames are only known once
    // the second ends: written just in time, any number of them fit.
    return settings_.multiplier == FrameGenMultiplier::kDynamic && ThreadedEligibleLocked() &&
           (settings_.synthesis_mode == FrameGenSynthesisMode::kExtrapolate || gpu_->SupportsJustInTime());
}

std::uint32_t FrameGenPresenter::DynamicSlotsLocked() const {
    // Two until measured: always engaged, so frames the game misses are filled.
    if (dynamic_interval_slots_ <= 0.0) return 2;
    return static_cast<std::uint32_t>(std::clamp(std::lround(dynamic_interval_slots_), 2l, 4l));
}

std::uint32_t FrameGenPresenter::DynamicSetCapLocked() const {
    // Two generated frames and a spare cover a game at a third of the refresh
    // rate or faster; slower, the last frame is shown again until the next
    // real one (a fourth set costs as much video memory as the game's own
    // frame at headset resolution). Near the video memory budget, one fewer.
    return std::min<std::uint32_t>(3, auto_memory_ceiling_);
}

void FrameGenPresenter::UpdateAutoLocked(Clock::time_point now, const std::optional<PendingFrame>& frame,
                                         bool started_late) {
    const bool dynamic = DynamicLocked();
    // Dynamic where the presenter thread cannot extrapolate is Auto.
    const bool automatic = settings_.multiplier == FrameGenMultiplier::kAuto ||
                           (settings_.multiplier == FrameGenMultiplier::kDynamic && !dynamic);
    const XrDuration period = frame && frame->period > 0 ? frame->period : last_period_;
    if (!(automatic || dynamic) || period <= 0 || !RunnableLocked()) {
        last_app_end_ = now;
        cadence_.clear();
        dynamic_below_since_.reset(); // standing by, the game's rate is measured afresh
        return;
    }
    // Paging makes the game late too, and reading that as a reason to step
    // up allocates more output images and compounds it. Sample the process
    // budget twice a second; within 5% of it, step down (EnsureOutputs frees
    // the sets above the new ceiling). Budgets shrink while other processes
    // need memory and grow back: with room for another set and a tenth of the
    // budget to spare for memory_recovery_delay_, the ceiling rises again, at
    // most kMemoryRecoveries times a session so a budget hovering at the edge
    // does not reallocate headset-size images back and forth.
    const bool sampled_memory =
        memory_checked_at_ == Clock::time_point{} || now - memory_checked_at_ >= std::chrono::milliseconds(500);
    if (sampled_memory) {
        memory_checked_at_ = now;
        memory_budget_ = gpu_ ? gpu_->MemoryBudget() : std::nullopt;
    }
    // Just in time one set of output images serves any multiplier: there is
    // nothing to give back by generating fewer frames.
    const bool memory_pressure = !(compositor_running_ && jit_) && memory_budget_ && memory_budget_->budget_bytes &&
                                 memory_budget_->usage_bytes > memory_budget_->budget_bytes - memory_budget_->budget_bytes / 20;
    if (memory_pressure) {
        memory_room_since_.reset();
    } else if (sampled_memory && auto_memory_ceiling_ < kMaxMultiplier && memory_recoveries_ < kMemoryRecoveries &&
               memory_budget_ && memory_budget_->budget_bytes && output_set_bytes_) {
        const std::uint64_t budget = memory_budget_->budget_bytes;
        if (memory_budget_->usage_bytes + output_set_bytes_ + budget / 10 >= budget) {
            memory_room_since_.reset();
        } else if (!memory_room_since_) {
            memory_room_since_ = now;
        } else if (now - *memory_room_since_ >= memory_recovery_delay_) {
            memory_room_since_.reset();
            ++memory_recoveries_;
            auto_memory_ceiling_ = dynamic ? kMaxMultiplier : auto_memory_ceiling_ + 1;
            if (logger_) {
                logger_->Info(std::string("Frame Generation: process video memory has room again; ") +
                              (dynamic ? "dynamic uses up to three generated frames per real frame."
                                       : "automatic may step up to " + MultiplierText(auto_memory_ceiling_) + "."));
                LogMemoryBudgetLocked();
            }
        }
    }
    if (dynamic) {
        // No multiplier to choose: near the budget, real frames use at most
        // two sets of output images (EnsureOutputs frees the third).
        if (memory_pressure && sampled_memory && auto_memory_ceiling_ > 2) {
            auto_memory_ceiling_ = 2;
            if (logger_) {
                logger_->Info("Frame Generation (dynamic) uses at most two generated frames per real frame until video "
                              "memory frees up: process video memory is near its budget.");
                LogMemoryBudgetLocked();
            }
        }
        UpdateDynamicLocked(now, period);
        last_app_end_ = now;
        cadence_.clear();
        return;
    }
    if (memory_pressure) {
        if (sampled_memory) {
            auto_memory_ceiling_ = std::min(auto_memory_ceiling_, auto_multiplier_ > 2 ? auto_multiplier_ - 1 : 2u);
        }
        if (auto_multiplier_ > auto_memory_ceiling_) {
            const std::uint32_t before = auto_multiplier_;
            auto_multiplier_ = auto_memory_ceiling_;
            probing_ = false;
            multiplier_changed_at_ = now;
            last_app_end_ = now;
            cadence_.clear();
            if (logger_) {
                logger_->Info("Frame Generation automatic multiplier " + MultiplierText(before) + " -> " +
                              MultiplierText(auto_multiplier_) +
                              ": process video memory is near its budget; it stays at most this until video memory "
                              "frees up.");
                LogMemoryBudgetLocked();
            }
            return;
        }
    }
    // Frames right after a change (a new multiplier, engaging, new output
    // images) carry its one-off cost and say nothing about the app.
    if (last_app_end_ && now - multiplier_changed_at_ >= auto_settle_) {
        // An engaged frame should take N refreshes; a passed-through one, one.
        const double multiplier = frame && frame->engaged ? static_cast<double>(frame->multiplier) : 1.0;
        const auto expected = std::chrono::nanoseconds(static_cast<std::int64_t>(period * multiplier * 1.25));
        const bool late = now - *last_app_end_ > expected;
        cadence_.push_back({now, late, late && started_late});
    }
    last_app_end_ = now;
    while (!cadence_.empty() && (now - cadence_.front().time > std::chrono::seconds(6) || cadence_.size() > 4096)) {
        cadence_.pop_front();
    }
    if (now - multiplier_changed_at_ < auto_settle_) {
        return;
    }
    std::size_t recent = 0, recent_late = 0, recent_started_late = 0, steady = 0, steady_late = 0;
    for (const Cadence& entry : cadence_) {
        if (now - entry.time <= std::chrono::seconds(1)) {
            ++recent;
            recent_late += entry.late ? 1 : 0;
            recent_started_late += entry.started_late ? 1 : 0;
        }
        if (now - entry.time <= auto_steady_) {
            ++steady;
            steady_late += entry.late ? 1 : 0;
        }
    }
    const std::uint32_t before = auto_multiplier_;
    const bool held = inline_hold_until_ && now < *inline_hold_until_;
    bool gave_up_inline = false;
    // More than 30% of the last second's frames missed the cadence: step up.
    if (recent >= 8 && recent_late * 10 > recent * 3) {
        auto_late_percent_ = recent_late * 100 / recent;
        if (!compositor_running_ && auto_multiplier_ > 1 && recent_started_late * 2 >= recent_late) {
            // Inline cycles that start late hold the game's frame thread for
            // the generated slots, so a higher multiplier only slows it more.
            auto_multiplier_ = 1;
            probing_ = false;
            inline_hold_until_ = now + kInlineHold;
            gave_up_inline = true;
        } else if (held && auto_multiplier_ == 1) {
            // Stay at full rate until the hold ends.
        } else if (probing_) {
            // The lower multiplier could not keep up: go back and wait longer
            // before probing again.
            auto_multiplier_ = std::min(probe_from_, auto_memory_ceiling_);
            probing_ = false;
            probe_backoff_ = std::min(probe_backoff_ * 2, auto_initial_backoff_ * (1 << kAutoMaxBackoffDoublings));
        } else if (!memory_pressure && auto_multiplier_ < auto_memory_ceiling_) {
            ++auto_multiplier_;
        }
        probe_not_before_ = now + probe_backoff_;
    } else if (probing_ && now - multiplier_changed_at_ >= auto_probe_confirm_) {
        probing_ = false;
        probe_backoff_ = auto_initial_backoff_;
        probe_not_before_ = now + probe_backoff_;
    } else if (!probing_ && auto_multiplier_ > 1 && now >= probe_not_before_ && !cadence_.empty() &&
               now - cadence_.front().time >= auto_steady_ && steady >= 8 && steady_late * 50 <= steady) {
        // Five steady seconds with at most 2% of frames late: try one step down.
        probe_from_ = auto_multiplier_;
        --auto_multiplier_;
        probing_ = true;
    }
    if (auto_multiplier_ != before) {
        multiplier_changed_at_ = now;
        cadence_.clear();
        if (logger_ && gave_up_inline) {
            logger_->Info("Frame Generation automatic multiplier " + MultiplierText(before) +
                          " -> full rate: generated cycles kept starting late (" + std::to_string(auto_late_percent_) +
                          "% of frames late), so the game's frame takes longer than the runtime allows before the "
                          "first generated slot and Frame Generation would only slow it. Retrying in " +
                          std::to_string(kInlineHold.count()) + " s. Extrapolate on Direct3D 12 presents from its "
                          "own thread and avoids this.");
        } else if (logger_) {
            const bool up = auto_multiplier_ > before;
            logger_->Info("Frame Generation automatic multiplier " +
                          (before == 1 ? std::string("full rate") : MultiplierText(before)) + " -> " +
                          (auto_multiplier_ == 1 ? std::string("full rate") : MultiplierText(auto_multiplier_)) +
                          (up ? " (" + std::to_string(auto_late_percent_) + "% of frames late" +
                                    (probe_backoff_ > auto_initial_backoff_
                                         ? ", next probe in " + std::to_string(probe_backoff_.count() / 1000) + " s"
                                         : std::string()) +
                                    ")."
                              : " (probing a lower multiplier)."));
        }
    }
}

void FrameGenPresenter::UpdateDynamicLocked(Clock::time_point now, XrDuration period) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    const double refresh = 1e9 / static_cast<double>(period);
    if (dynamic_standby_) {
        // A gap (a pause, a loading stall) is not the game's rate; a hitch is.
        if (last_app_end_ && now - *last_app_end_ > std::chrono::nanoseconds(PauseLimitLocked())) {
            dynamic_below_since_.reset();
        }
        // Right after a switch the game's frames carry its cost; after a
        // stand-down, nothing engages until the hold ends.
        if (now - multiplier_changed_at_ < policy.settle || now < dynamic_hold_until_) {
            dynamic_below_since_.reset();
            return;
        }
        const double rate = RateSince(app_frames_, now, multiplier_changed_at_);
        const double share = dynamic_probing_ ? policy.stay_above : policy.engage_below;
        if (rate > 0.0 && rate < share * refresh) {
            if (!dynamic_below_since_) {
                dynamic_below_since_ = now;
            } else if (now - *dynamic_below_since_ >= policy.engage_after) {
                DynamicEngageLocked(now, rate, refresh);
            }
            return;
        }
        dynamic_below_since_.reset();
        if (dynamic_probing_ && now - multiplier_changed_at_ >= policy.settle + policy.engage_after) {
            // The probe held: the game keeps the rate alone.
            dynamic_probing_ = false;
            dynamic_probe_after_ = policy.probe_after;
            dynamic_hold_ = policy.hold;
            dynamic_reason_ = "the game holds " + Number(rate, 1) + " fps alone";
            if (logger_) {
                logger_->Info("Frame Generation (dynamic): the game holds " + Number(rate, 1) +
                              " fps: stays standing by.");
            }
        }
        return;
    }
    if (!compositor_running_) {
        return; // engaging, or presenting inline for a moment: nothing to judge
    }
    // Engaged: the benefit guard. Windows with a pause, a frame without a
    // projection (loading), a refresh the runtime did not render (the
    // headset off), a presenter restart or a mode switch say nothing about
    // what generation adds and are left out, unless pauses are most of the
    // game's frames (a game too slow to generate for at all).
    if (now - multiplier_changed_at_ < policy.settle) {
        guard_.valid = false;
        return;
    }
    if (!guard_.valid) {
        guard_ = {true, now, totals_};
        guard_capture_done_ms_.clear();
        return;
    }
    if (now - guard_.start < policy.guard_window) {
        return;
    }
    const Totals start = guard_.totals;
    const double seconds = std::chrono::duration<double>(now - guard_.start).count();
    guard_ = {true, now, totals_};
    const std::uint64_t app_frames = totals_.app_frames - start.app_frames;
    const std::uint64_t pauses = totals_.pauses - start.pauses;
    std::vector<double> capture_done = std::move(guard_capture_done_ms_);
    guard_capture_done_ms_.clear();
    NoteGpuEvidenceLocked(app_frames, totals_.late_gpu - start.late_gpu, std::move(capture_done));
    const bool mostly_pauses = app_frames >= 4 && pauses * 2 >= app_frames;
    if ((pauses > 0 && !mostly_pauses) || totals_.non_renderable != start.non_renderable ||
        totals_.unrendered != start.unrendered || totals_.restarts != start.restarts) {
        return;
    }
    const double distinct = PerSecond(totals_.distinct - start.distinct, seconds);
    const double app_rate = PerSecond(app_frames, seconds);
    const double generated = PerSecond(totals_.generated_distinct - start.generated_distinct, seconds);
    const double gap = std::max(1.0, refresh - app_rate);
    const std::uint64_t refreshes = totals_.submissions - start.submissions;
    const std::uint64_t unready = totals_.unready - start.unready;
    const std::uint64_t timed = totals_.analyses_timed - start.analyses_timed;
    const std::uint64_t late = totals_.analyses_late - start.analyses_late;
    // Showing clearly more distinct frames than the game alone did, engaged
    // pays even with few frames generated, real frames on the GPU late or
    // motion measured late: the game, released once a refresh and late at
    // once, fills refreshes it missed standing by (the lead absorbs its frame
    // time's jitter), and a hitch's late frames cost a few refreshes of a
    // window that still shows more. (b), (c) and (d) then judge nothing;
    // probes check whether standing by would hold.
    const bool winning = passthrough_fps_ > 0.0 && distinct >= passthrough_fps_ + policy.distinct_margin_fps;
    const std::array<bool, 4> failing{
        passthrough_fps_ > 0.0 && !winning,
        !winning && generated < policy.generated_share * gap,
        !winning && refreshes > 0 &&
            static_cast<double>(unready) > policy.unready_share * static_cast<double>(refreshes),
        !winning && timed > 0 && static_cast<double>(late) > policy.late_analyses_share * static_cast<double>(timed),
    };
    for (std::size_t i = 0; i < failing.size(); ++i) {
        guard_failing_[i] = failing[i] ? guard_failing_[i] + 1 : 0;
    }
    auto percent = [](std::uint64_t part, std::uint64_t whole) {
        return Number(whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0, 0) + "%";
    };
    const std::array<std::string, 4> reasons{
        "it showed " + Number(distinct, 0) + " distinct frames/s against " + Number(passthrough_fps_, 0) +
            " passing through",
        "it generated too few frames (" + Number(generated, 1) + "/s of a " + Number(gap, 1) + "/s gap)",
        "real frames reached the GPU too late (" + percent(unready, refreshes) + " of refreshes)",
        "motion was measured too late (" + percent(late, timed) + " of analyses)",
    };
    // Two failing windows in a row: one can be a hitch. A cause failing now
    // (frames on the GPU too late, motion measured too late, too few
    // generated) is named rather than its effect (no more distinct frames
    // than the game alone).
    const std::array<std::size_t, 3> causes{2, 3, 1};
    for (const std::size_t i : causes) {
        if (guard_failing_[i] >= 2) {
            DynamicStandDownLocked(now, reasons[i], false);
            return;
        }
    }
    if (guard_failing_[0] >= 2) {
        const auto cause = std::find_if(causes.begin(), causes.end(), [&](std::size_t i) { return failing[i]; });
        DynamicStandDownLocked(now, reasons[cause != causes.end() ? *cause : 0], false);
        return;
    }
    if (std::none_of(failing.begin(), failing.end(), [](bool value) { return value; })) {
        if (now - dynamic_engaged_at_ >= kDynamicLongEngagement) {
            dynamic_hold_ = policy.hold;
        }
        // Engaged, the game runs at its own rate: one well below what a
        // probe needs standing by would only lose its generated frames for
        // a while and engage again.
        const bool within_reach = app_rate >= (policy.stay_above - kProbeReach) * refresh;
        if (now - dynamic_engaged_at_ >= dynamic_probe_after_ && within_reach) {
            DynamicStandDownLocked(now,
                                   "probes standing by after " +
                                       Number(std::chrono::duration<double>(now - dynamic_engaged_at_).count(), 0) +
                                       " s engaged",
                                   true);
        }
    }
}

void FrameGenPresenter::DynamicEngageLocked(Clock::time_point now, double rate, double refresh) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    const bool probe_failed = dynamic_probing_;
    const double below = (probe_failed ? policy.stay_above : policy.engage_below) * refresh;
    dynamic_standby_ = false;
    dynamic_below_since_.reset();
    passthrough_fps_ = rate;
    dynamic_engaged_at_ = now;
    multiplier_changed_at_ = now;
    cadence_.clear();
    if (standby_since_) {
        standby_seconds_ += std::chrono::duration<double>(now - *standby_since_).count();
        standby_since_.reset();
    }
    guard_.valid = false;
    guard_failing_ = {};
    ++dynamic_engagements_;
    if (probe_failed) {
        dynamic_probing_ = false;
        dynamic_probe_after_ = std::min(dynamic_probe_after_ * 2, policy.probe_max);
    }
    dynamic_reason_ = "the game ran at " + Number(rate, 1) + " fps (below " + Number(below, 1) + " of " +
                      Number(refresh, 1) + ")";
    if (logger_) {
        logger_->Info(
            std::string("Frame Generation (dynamic) engages") + (probe_failed ? " again" : "") + ": the game ran at " +
            Number(rate, 1) + " fps (below " + Number(below, 1) + " of " + Number(refresh, 1) + ") for " +
            Number(std::chrono::duration<double>(policy.engage_after).count(), 1) +
            " s; generated frames fill the refreshes it misses" +
            (probe_failed ? " (next probe in " + std::to_string(dynamic_probe_after_.count() / 1000) + " s)." : "."));
    }
}

void FrameGenPresenter::DynamicStandDownLocked(Clock::time_point now, const std::string& reason, bool probe) {
    const FrameGenDynamicPolicy& policy = dynamic_policy_;
    dynamic_standby_ = true;
    dynamic_below_since_.reset();
    multiplier_changed_at_ = now;
    cadence_.clear();
    standby_since_ = now;
    guard_.valid = false;
    guard_failing_ = {};
    dynamic_reason_ = reason;
    if (probe) {
        // No hold: the probe engages again as soon as the game falls short.
        dynamic_probing_ = true;
        ++dynamic_probes_;
        if (logger_) {
            logger_->Info("Frame Generation (dynamic) " + reason + ": frames pass straight through while it checks "
                          "whether the game holds the refresh rate alone.");
        }
        return;
    }
    ++dynamic_stand_downs_;
    dynamic_probing_ = false;
    const std::chrono::milliseconds hold = dynamic_hold_;
    dynamic_hold_until_ = now + hold;
    if (now - dynamic_engaged_at_ < kDynamicShortEngagement) {
        dynamic_hold_ = std::min(dynamic_hold_ * 2, policy.hold_max);
    }
    if (logger_) {
        logger_->Info("Frame Generation (dynamic) stands down: " + reason +
                      "; frames pass straight through, retrying in " +
                      Number(std::chrono::duration<double>(hold).count(), 0) + " s.");
    }
}

void FrameGenPresenter::DynamicReenableLocked(Clock::time_point now) {
    // What the game ran at before says nothing about it now: it engages
    // again only after running below the refresh rate a while, and the
    // guard and probes start over. A stand-down's hold stays.
    if (!dynamic_standby_) {
        dynamic_standby_ = true;
        standby_since_ = now;
    }
    dynamic_below_since_.reset();
    dynamic_probing_ = false;
    multiplier_changed_at_ = now;
    cadence_.clear();
    guard_.valid = false;
    guard_failing_ = {};
}

void FrameGenPresenter::MaybeRetryLocked() {
    if (state_ != FrameGenState::kPaused || !paused_at_ || automatic_retries_ >= retry_attempts_ ||
        Clock::now() - *paused_at_ < retry_delay_) {
        return;
    }
    ++automatic_retries_;
    paused_at_.reset();
    consecutive_failures_ = consecutive_demotions_ = 0;
    healthy_streak_ = 0;
    DynamicReenableLocked(Clock::now());
    if (logger_) {
        logger_->Info("Frame Generation retrying after a pause (" + std::to_string(automatic_retries_) + " of " +
                      std::to_string(retry_attempts_) + ").");
    }
    SetStateLocked(user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
}

void FrameGenPresenter::SetStateLocked(FrameGenState state, const std::string& reason) {
    if (state == state_ && reason == reason_) {
        return;
    }
    const FrameGenState previous = state_;
    state_ = state;
    reason_ = reason;
    if (!logger_) {
        return;
    }
    if (state == FrameGenState::kActive && full_rate_ && !reason.empty()) {
        logger_->Info("Frame Generation standing by: " + reason + ".");
    } else if (state == FrameGenState::kActive && full_rate_ && DynamicLocked() && dynamic_standby_) {
        // The game's rate right after a switch is the engaged one's, not its own.
        const auto now = Clock::now();
        const double refresh = last_period_ > 0 ? 1e9 / static_cast<double>(last_period_) : 0.0;
        const double rate = now - multiplier_changed_at_ >= std::chrono::seconds(1) ? RatePerSecond(app_frames_, now) : 0.0;
        logger_->Info("Frame Generation (dynamic) standing by: " +
                      (rate > 0.0 ? "the game holds " + Number(rate, 1) + " fps" : std::string("frames pass straight through")) +
                      " (engages below " + Number(dynamic_policy_.engage_below * refresh, 1) + ").");
    } else if (state == FrameGenState::kActive && full_rate_) {
        logger_->Info("Frame Generation standing by: the application holds full refresh.");
    } else if (state == FrameGenState::kActive && DynamicLocked()) {
        logger_->Info(std::string("Frame Generation engaged: ") + ToString(settings_.synthesis_mode) +
                      " dynamic, the application runs at its own rate and generated frames fill the refreshes "
                      "between its frames.");
    } else if (state == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation engaged: ") + ToString(settings_.synthesis_mode) + " " +
                      MultiplierText(EffectiveMultiplierLocked()) + ", application held at 1/" +
                      std::to_string(EffectiveMultiplierLocked()) + " of the runtime refresh rate.");
    } else if (state == FrameGenState::kPaused || state == FrameGenState::kUnavailable) {
        logger_->Error(std::string("Frame Generation ") + ToString(state) + ": " + reason + ".");
    } else if (previous == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation disengaged (") + ToString(state) + ").");
    }
}

void FrameGenPresenter::PauseLocked(const std::string& reason) {
    SetStateLocked(FrameGenState::kPaused, reason);
    paused_at_ = Clock::now();
    healthy_streak_ = 0;
}

void FrameGenPresenter::UnavailableLocked(const std::string& reason) {
    SetStateLocked(FrameGenState::kUnavailable, reason);
}

void FrameGenPresenter::InvalidateContinuityLocked() {
    last_real_.valid = false;
    extrapolated_.valid = false;
}

void FrameGenPresenter::LogMemoryBudgetLocked() {
    if (!logger_ || !gpu_) return;
    if (const auto memory = gpu_->MemoryBudget()) {
        constexpr double megabytes = 1024.0 * 1024.0;
        // Frame Generation's own share: its working images as allocated (once
        // it has generated) and the output swapchain images it created.
        const std::string own = working_bytes_ || output_bytes_
                                    ? "; Frame Generation working " + Number(working_bytes_ / megabytes, 0) +
                                          " MB, output images " + Number(output_bytes_ / megabytes, 0) + " MB"
                                    : std::string();
        logger_->Info("Frame Generation process video memory: usage=" + Number(memory->usage_bytes / megabytes, 0) +
                      " MB, budget=" + Number(memory->budget_bytes / megabytes, 0) + " MB" + own +
                      (memory->usage_bytes > memory->budget_bytes ? " (over budget)." : "."));
    }
}

void FrameGenPresenter::NoteFailureLocked(const std::string& error) {
    ++failures_;
    if (++consecutive_failures_ >= kFailuresBeforePause) {
        PauseLocked("generation failed: " + error);
    } else if (logger_) {
        logger_->Error("Frame Generation could not generate a frame: " + error);
    }
}

XrResult FrameGenPresenter::WaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state,
                                      const FrameGenRuntimeCalls& calls) {
    if (state) {
        std::unique_lock lock(mutex_);
        MaybeRetryLocked();
        if (!compositor_running_ && compositor_.joinable()) {
            // The presenter thread stopped itself after a runtime error.
            StopCompositorLocked(lock);
        }
        const bool inline_engaged = std::any_of(frames_.begin(), frames_.end(), [](const PendingFrame& frame) {
            return frame.engaged && !frame.demoted;
        });
        // A thread that waits again before ending its own frame would wait
        // for itself while the presenter thread waits for that frame to end:
        // such apps stay inline.
        const bool own_frame_open = std::this_thread::get_id() == end_thread_ && !frames_.empty();
        const bool threaded =
            ThreadedEligibleLocked() &&
            (compositor_running_ ? StayThreadedLocked() : WantsEngagementLocked() && !inline_engaged && !own_frame_open);
        if (threaded) {
            if (const auto result = ThreadedWaitLocked(lock, state, calls)) {
                return *result;
            }
            if (compositor_.joinable()) {
                // The presenter thread stopped (or is stopping) under this
                // wait: its frames in flight go before the runtime is waited
                // inline.
                StopCompositorLocked(lock);
            }
        } else if (compositor_running_) {
            ExitThreadedLocked(lock);
        }
    }
    bool engage = false;
    std::uint32_t multiplier = 2;
    std::uint64_t sequence = 0;
    {
        std::unique_lock lock(mutex_);
        MaybeRetryLocked();
        engage = WantsEngagementLocked();
        bool demoted = false;
        auto demote = [&] {
            for (PendingFrame& frame : frames_) {
                if (frame.engaged && !frame.demoted && !frame.claimed) {
                    frame.demoted = true;
                    ++demoted_;
                    demoted = true;
                }
            }
        };
        // The thread that ends frames is waiting again before ending its
        // engaged frame: holding it would only stall until the timeout.
        // Apps that wait on one thread and end on another take the hold.
        if (std::this_thread::get_id() == end_thread_) {
            demote();
        }
        // An engaged frame in progress owns the runtime until its extra
        // wait/begin/end has finished. One whose end has not started yet is
        // demoted after a short hold; one already in its end is finishing a
        // runtime call and is waited for.
        auto blocking = [&] {
            return std::any_of(frames_.begin(), frames_.end(),
                               [](const PendingFrame& frame) { return frame.engaged && !frame.demoted; });
        };
        if (blocking() && !idle_cv_.wait_for(lock, kOverlapWait, [&] { return !blocking(); })) {
            demote();
            // What still blocks is inside its own runtime calls; a second
            // runtime wait now would break frame ordering, so let it finish.
            while (!idle_cv_.wait_for(lock, kClaimedFrameWait, [&] { return !blocking(); })) {
                if (logger_ && !logged_claimed_wait_) {
                    logged_claimed_wait_ = true;
                    logger_->Error("Frame Generation: a generated frame has been inside the runtime for over 5 s.");
                }
            }
        }
        if (demoted) {
            // A demoted frame shows in slot k alone; this frame passes through
            // so the runtime sees a clean single frame next.
            engage = false;
            InvalidateContinuityLocked();
            if (++consecutive_demotions_ >= kDemotionsBeforePause) {
                PauseLocked("the application starts frames before the previous one ends");
            }
        }
        multiplier = EffectiveMultiplierLocked();
        // Automatic and at full rate: nothing to generate, but the feature is
        // doing its job, so it reads as active. So does standing by while the
        // layer composes the quad views again for a while.
        const bool declined = RunnableLocked() && QuadDeclinedLocked();
        const bool full_rate = !demoted && RunnableLocked() && (multiplier < 2 || declined);
        if (full_rate != full_rate_) {
            full_rate_ = full_rate;
            if (state_ == FrameGenState::kActive) state_ = FrameGenState::kWaiting; // re-log the transition
        }
        if (full_rate) {
            SetStateLocked(FrameGenState::kActive,
                           declined && multiplier >= 2 ? "the layer composes the quad views for a while: composed frames "
                                                         "could not be written, so frames pass straight through"
                                                       : "");
        } else if (!engage && state_ == FrameGenState::kActive) {
            SetStateLocked(IdleStateLocked(), "");
        }
        sequence = ++wait_sequence_;
    }
    const auto called = Clock::now();
    const XrResult result = RuntimeWait(calls, session, info, state);
    if (XR_FAILED(result) || !state) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    NotePhaseLocked(called, Clock::now(), state->predictedDisplayTime, state->predictedDisplayPeriod);
    PendingFrame frame;
    frame.slot_time = state->predictedDisplayTime;
    frame.period = state->predictedDisplayPeriod;
    frame.should_render = state->shouldRender == XR_TRUE;
    // Still wanted after the runtime's wait: turned off meanwhile, the frame
    // passes through (and the state stays off).
    frame.engaged = engage && frame.period > 0 && state_ != FrameGenState::kPaused && user_enabled_ && configured_enabled_;
    frame.multiplier = multiplier;
    frame.sequence = sequence;
    frame.wait_return = Clock::now();
    if (frame.period > 0) last_period_ = frame.period;
    if (frame.engaged) {
        state->predictedDisplayPeriod = frame.period * static_cast<XrDuration>(multiplier);
        state->predictedDisplayTime = frame.slot_time + frame.period * static_cast<XrDuration>(multiplier - 1) -
                                      DisplayOffsetLocked(state->predictedDisplayPeriod);
        SetStateLocked(FrameGenState::kActive, "");
    }
    frame.reported_period = state->predictedDisplayPeriod;
    state->predictedDisplayTime = std::max(state->predictedDisplayTime, last_returned_time_ + 1);
    last_returned_time_ = state->predictedDisplayTime;
    frame.app_time = state->predictedDisplayTime;
    // Frames the application abandoned without an end never block a wait.
    while (!frames_.empty() && frames_.front().sequence + 4 < sequence) {
        frames_.pop_front();
    }
    frames_.push_back(frame);
    return result;
}

XrResult FrameGenPresenter::BeginFrame(XrSession session, const XrFrameBeginInfo* info, const FrameGenRuntimeCalls& calls) {
    {
        std::scoped_lock lock(mutex_);
        // A frame dropped when the presenter thread stopped has no runtime
        // frame; it was waited before anything still open, so begins first.
        if (dropped_unbegun_ > 0) {
            --dropped_unbegun_;
            return XR_SUCCESS;
        }
        // Frames begin in the order they were waited; one the runtime waited
        // for (inline) begins there, one the presenter thread presents only
        // here.
        const auto inline_frame =
            std::find_if(frames_.begin(), frames_.end(), [](const PendingFrame& frame) { return !frame.begun; });
        if (inline_frame != frames_.end()) {
            inline_frame->begun = true;
        } else {
            const auto threaded = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                               [](const ThreadedFrame& frame) { return !frame.begun; });
            if (threaded != threaded_frames_.end()) {
                threaded->begun = true;
                // The captures released before it: the next wait may be held
                // until they have run on the GPU (GpuGateMarkLocked).
                threaded->capture_mark = last_capture_mark_;
                begun_capture_marks_.push_back(last_capture_mark_);
                while (begun_capture_marks_.size() > 4) {
                    begun_capture_marks_.pop_front();
                }
                slot_cv_.notify_all();
                return XR_SUCCESS;
            }
            if (compositor_running_) {
                // The presenter thread owns the runtime frame loop.
                return XR_SUCCESS;
            }
        }
        if (!RuntimeWaitPendingLocked()) {
            // Nothing waited is left to begin: forwarding would break the
            // runtime's frame order.
            if (logger_ && !logged_order_guard_) {
                logged_order_guard_ = true;
                logger_->Error("Frame Generation: held back an xrBeginFrame with no waited frame to begin.");
            }
            return XR_SUCCESS;
        }
    }
    const XrResult result = RuntimeBegin(calls, session, info);
    WakeSlots();
    return result;
}

XrResult FrameGenPresenter::EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls,
                                     const FrameGenQuadFrame* quad) {
    const auto entered = Clock::now();
    // Every return: how long the game's xrEndFrame was held here (after any
    // lock below is released).
    struct EndCall {
        FrameGenPresenter* presenter;
        Clock::time_point entered;
        ~EndCall() { presenter->NoteEndCall(entered); }
    } end_call{this, entered};
    {
        std::optional<ThreadedFrame> threaded;
        {
            std::scoped_lock lock(mutex_);
            NoteQuadEndLocked(quad, FindProjection(*info) != nullptr);
            if (quad && quad->route == FrameGenQuadRoute::kClaimed) {
                // Claimed in the layer's xrEndFrame: this end is that frame's,
                // ahead of any frame dropped since (the claimed frame was the
                // oldest open one). Its primary projection holds four views,
                // which a stereo runtime must never get: it is shown composed
                // by the presenter thread, or not at all.
                const auto claimed =
                    quad_claim_id_ ? std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                  [&](const ThreadedFrame& pending) { return pending.id == *quad_claim_id_; })
                                   : threaded_frames_.end();
                quad_claim_id_.reset();
                if (claimed == threaded_frames_.end()) {
                    healthy_streak_ = 0;
                    if (logger_ && !logged_quad_lost_claim_) {
                        logged_quad_lost_claim_ = true;
                        logger_->Error("Frame Generation: a claimed quad views frame was dropped before its end "
                                       "(the session stopped); it was not shown.");
                    }
                    return XR_SUCCESS;
                }
                threaded = *claimed;
            }
            // Frames end in the order they were waited, whatever display time
            // an app reports (MSFS reports one period later): the oldest open
            // frame is the one ending. Dropped frames are the oldest, then
            // frames the runtime waited for, then the presenter thread's.
            if (!threaded) {
                if (dropped_unended_ > 0) {
                    // Its runtime frame went to the frames around it when the
                    // presenter thread stopped; there is nothing to end.
                    --dropped_unended_;
                    healthy_streak_ = 0;
                    return XR_SUCCESS;
                }
                if (frames_.empty() && !threaded_frames_.empty()) {
                    const auto it = std::find_if(threaded_frames_.begin(), threaded_frames_.end(),
                                                 [](const ThreadedFrame& pending) { return !pending.claimed; });
                    if (it != threaded_frames_.end()) {
                        it->claimed = true;
                        threaded = *it;
                    }
                }
                if (!threaded && frames_.empty() && compositor_running_) {
                    // The presenter thread owns the runtime frame loop and
                    // holds no runtime frame for this one.
                    if (logger_ && !logged_swallowed_) {
                        logged_swallowed_ = true;
                        logger_->Error("Frame Generation: an application frame matched no waited frame while "
                                       "presenting from its own thread; it was not shown.");
                    }
                    return XR_SUCCESS;
                }
            }
        }
        if (threaded) {
            return ThreadedEnd(session, *info, calls, *threaded, entered, quad);
        }
    }
    std::optional<PendingFrame> frame;
    const XrCompositionLayerProjection* projection = FindProjection(*info);
    const bool renderable = info->layerCount > 0 && projection != nullptr;
    {
        std::scoped_lock lock(mutex_);
        end_thread_ = std::this_thread::get_id();
        // The oldest waited frame is the one ending, as on the threaded path.
        // MSFS ends frames one period after the time it was given, which can
        // equal the prediction of its next pipelined frame: matching by time
        // would take that frame's record instead of this one's.
        const auto it = frames_.begin();
        if (it != frames_.end()) {
            // Claiming under the lock: from here a held wait lets this frame
            // finish its extra runtime frame instead of demoting it.
            if (it->engaged && !it->demoted) {
                it->claimed = true;
            }
            frame = *it;
            NoteDisplayOffsetLocked(info->displayTime, it->app_time, it->reported_period);
            app_work_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - it->wait_return).count();
            ++app_work_frames_;
        }
        // Quad views Frame Generation could compose keep their four views as
        // targets (ClaimQuadFrame): the composed views would take them over,
        // and no later view release would be captured.
        if (renderable && !QuadTargetsLocked(quad)) {
            RecordCaptureTargetsLocked(*projection);
        }
    }
    XrResult result = XR_SUCCESS;
    bool open;
    {
        std::scoped_lock lock(mutex_);
        open = RuntimeFrameOpenLocked();
    }
    if (!open) {
        // No runtime frame is begun: forwarding would break the runtime's
        // frame order.
        std::scoped_lock lock(mutex_);
        if (frame) {
            frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                         [&](const PendingFrame& pending) { return pending.sequence == frame->sequence; }),
                          frames_.end());
        }
        healthy_streak_ = 0;
        if (logger_ && !logged_order_guard_) {
            logged_order_guard_ = true;
            logger_->Error("Frame Generation: held back an xrEndFrame with no begun frame to end.");
        }
        WakeSlots();
        return XR_SUCCESS;
    }
    if (!frame || !frame->engaged) {
        result = RuntimeEnd(calls, session, info);
        bool eligible;
        {
            std::scoped_lock lock(mutex_);
            eligible = ThreadedEligibleLocked();
        }
        if (eligible && renderable) {
            // What the presenter thread shows until its first real frame, and
            // a check that it can repeat this application's layers at all.
            FrameCopy copy;
            std::string unsupported;
            const bool copied = CopyFrame(*info, copy, unsupported);
            std::scoped_lock lock(mutex_);
            if (copied) {
                passthrough_copy_ = std::move(copy);
                // Shown as it is when the presenter thread starts: only while
                // nothing newer was released into its swapchains.
                RecordColorReleasesLocked(passthrough_copy_);
                if (compositor_running_) {
                    // The presenter thread is about to start: its first
                    // slots show this frame, not the one before.
                    newest_copy_ = passthrough_copy_;
                }
            } else if (!threaded_disabled_) {
                threaded_disabled_ = true;
                if (logger_) {
                    logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                                  ", which the presenter thread cannot repeat between real frames.");
                }
            }
        }
    } else if (frame->demoted || !frame->should_render || !renderable) {
        // Present in the one slot this frame still holds.
        result = SubmitAt(session, *info, frame->slot_time, calls);
        std::scoped_lock lock(mutex_);
        InvalidateContinuityLocked();
    } else {
        result = RunCycle(session, *info, *frame, calls);
    }
    {
        std::scoped_lock lock(mutex_);
        if (frame) {
            frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                         [&](const PendingFrame& pending) { return pending.sequence == frame->sequence; }),
                          frames_.end());
        }
        healthy_streak_ = renderable && (!frame || frame->should_render) ? healthy_streak_ + 1 : 0;
        const auto now = Clock::now();
        app_frames_.push_back(now);
        ++totals_.app_frames;
        NoteRealEndLocked(now);
        totals_.non_renderable += renderable ? 0 : 1;
        if (!frame || !frame->engaged) {
            NoteSubmissionLocked(true);
        }
        UpdateAutoLocked(now, frame, frame && frame->engaged && last_cycle_started_late_);
        if (state_ == FrameGenState::kActive && now - last_summary_ >= summary_interval_) {
            const double seconds =
                last_summary_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last_summary_).count();
            const Totals& start = summary_totals_;
            const double standby = StandbySecondsLocked(now);
            if (logger_) {
                LogMemoryBudgetLocked();
                // Rates are the window's averages. appWorkMs: the game's own
                // time per frame between its wait and its end. lateCycles:
                // cycles whose first generated slot was already past due, so
                // the game's frame did not fit the time the runtime gives
                // before that slot.
                logger_->Info(
                    "Frame Generation: multiplier=" +
                    (full_rate_ ? std::string(DynamicLocked() && dynamic_standby_ ? "standing by" : "full rate")
                                : MultiplierText(EffectiveMultiplierLocked())) +
                    (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "") +
                    ", appFps=" + Number(PerSecond(totals_.app_frames - start.app_frames, seconds), 1) +
                    ", outputFps=" + Number(PerSecond(totals_.submissions - start.submissions, seconds), 1) +
                    ", distinctFps=" + Number(PerSecond(totals_.distinct - start.distinct, seconds), 1) +
                    ", standbyS=" + Number(standby - summary_standby_seconds_, 1) +
                    ", repeatedFrames=" + std::to_string(repeated_submissions_) +
                    ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
                    ", lateCycles=" + std::to_string(late_cycles_) + "/" + std::to_string(cycles_) +
                    ", threaded=" + std::to_string(compositor_running_) + ", generated=" + std::to_string(generated_) +
                    ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) +
                    ", demoted=" + std::to_string(demoted_) + ", captured=+" +
                    std::to_string(totals_.captured - start.captured) + " (" + std::to_string(captured_) + ")" +
                    ", pauses=" + std::to_string(totals_.pauses - start.pauses) + "/" + Number(pause_longest_ms_, 0) +
                    ", gpuMs=" + Number(gpu_ms_, 2) + DynamicSummaryLocked() + QuadSummaryLocked(start) + ".");
            }
            last_summary_ = now;
            summary_totals_ = totals_;
            summary_standby_seconds_ = standby;
            app_work_ms_ = 0.0;
            app_work_frames_ = 0;
            cycles_ = 0;
            late_cycles_ = 0;
            repeated_submissions_ = 0;
            pause_longest_ms_ = 0.0;
            end_call_ms_.clear();
        }
    }
    idle_cv_.notify_all();
    WakeSlots(); // the presenter thread waits for inline frames to end
    return result;
}

XrResult FrameGenPresenter::SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time,
                                     const FrameGenRuntimeCalls& calls) {
    XrFrameEndInfo copy = info;
    copy.displayTime = time;
    const XrResult result = RuntimeEnd(calls, session, &copy);
    std::scoped_lock lock(mutex_);
    NoteSubmissionLocked(true);
    return result;
}

void FrameGenPresenter::NotePhaseLocked(Clock::time_point called, Clock::time_point returned, XrTime display_time,
                                        XrDuration period) {
    // A wait that blocked returned on the runtime's own schedule; one that
    // returned at once says nothing about it.
    const auto blocked = std::max<std::chrono::nanoseconds>(std::chrono::milliseconds(1),
                                                            std::chrono::nanoseconds(period / 10));
    if (period > 0 && returned - called >= blocked) {
        phase_wall_ = returned;
        phase_xr_ = display_time;
    }
}

bool FrameGenPresenter::PaceSubmission(XrTime display_time, XrDuration period) {
    // Pimax and SteamVR can return the waits inside a generated cycle at once
    // instead of pacing them, so the cycle's frames would reach the
    // compositor inside one refresh and only the newest would be shown. Each
    // slot is held until the runtime would have released its wait, measured
    // from the last wait that did block, and at least most of a refresh after
    // the previous submission. Runtimes that pace their waits rarely sleep.
    if (period <= 0) {
        return false;
    }
    const auto now = Clock::now();
    Clock::time_point target{};
    bool late = false;
    {
        std::scoped_lock lock(mutex_);
        if (last_runtime_submit_ != Clock::time_point{}) {
            target = last_runtime_submit_ + std::chrono::nanoseconds(period * 17 / 20);
        }
        if (phase_wall_ != Clock::time_point{} && now - phase_wall_ <= kPhaseLifetime && display_time > phase_xr_) {
            const auto ideal = phase_wall_ + std::chrono::nanoseconds(display_time - phase_xr_);
            late = now > ideal + std::chrono::nanoseconds(period / 2);
            target = std::max(target, ideal);
        }
    }
    // A wrong phase never costs more than about one refresh.
    SleepUntil(timer_, std::min(target, now + std::chrono::nanoseconds(period * 6 / 5)));
    return late;
}

XrResult FrameGenPresenter::NextSlot(XrSession session, const FrameGenRuntimeCalls& calls, XrDuration period,
                                     XrFrameState& state) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    state = {XR_TYPE_FRAME_STATE};
    const auto called = Clock::now();
    XrResult result = RuntimeWait(calls, session, &wait_info, &state);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrWaitFrame failed inside a generated cycle (" + std::to_string(result) + ")");
        return result;
    }
    {
        std::scoped_lock lock(mutex_);
        NotePhaseLocked(called, Clock::now(), state.predictedDisplayTime, state.predictedDisplayPeriod);
    }
    PaceSubmission(state.predictedDisplayTime, period);
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    result = RuntimeBegin(calls, session, &begin_info);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrBeginFrame failed inside a generated cycle (" + std::to_string(result) + ")");
    }
    return result;
}

XrResult FrameGenPresenter::RunCycle(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                                     const FrameGenRuntimeCalls& calls) {
    std::uint32_t projection_index = 0;
    const XrCompositionLayerProjection& projection = *FindProjection(info, &projection_index);
    const std::uint32_t generated = std::max(2u, frame.multiplier) - 1;
    const XrDuration period = frame.period;
    const XrTime newest_time = frame.slot_time + period * static_cast<XrDuration>(generated);
    FrameGenSynthesisMode mode;
    bool continuous;
    XrTime previous_time = 0;
    std::size_t extrapolated_sets = 0;
    {
        std::scoped_lock lock(mutex_);
        mode = settings_.synthesis_mode;
        previous_time = last_real_.display_time;
        continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence && newest_time > previous_time &&
                     newest_time - previous_time <= period * static_cast<XrDuration>(kMaxMultiplier + 2);
        if (extrapolated_.valid && continuous && extrapolated_.space == projection.space && !extrapolated_.sets.empty() &&
            extrapolated_.sets.front().size() == projection.viewCount) {
            extrapolated_sets = extrapolated_.sets.size();
        }
    }

    // Replaces the primary projection layer with generated views.
    std::vector<const XrCompositionLayerBaseHeader*> layers(info.layers, info.layers + info.layerCount);
    XrCompositionLayerProjection generated_layer{};
    std::vector<XrCompositionLayerProjectionView> with_depth;
    auto with_views = [&](const std::vector<XrCompositionLayerProjectionView>& views, XrSpace space,
                          XrCompositionLayerFlags flags, bool depth) {
        generated_layer = projection;
        generated_layer.next = nullptr;
        generated_layer.space = space;
        generated_layer.layerFlags = flags;
        generated_layer.viewCount = static_cast<std::uint32_t>(views.size());
        generated_layer.views = views.data();
        if (depth && views.size() == projection.viewCount) {
            // The newest real frame's depth chain, valid during this end: its
            // depth image is the one the game just released.
            with_depth = views;
            for (std::size_t i = 0; i < with_depth.size(); ++i) {
                with_depth[i].next = projection.views[i].next;
            }
            generated_layer.views = with_depth.data();
        }
        layers[projection_index] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated_layer);
        XrFrameEndInfo generated_info = info;
        generated_info.layers = layers.data();
        return generated_info;
    };

    // Interpolated frames need the newest real frame, so they are generated
    // before the first slot; extrapolated ones were generated last cycle.
    std::vector<std::vector<XrCompositionLayerProjectionView>> interpolated;
    std::vector<std::uint8_t> interpolated_exact;
    bool interpolated_ready = false;
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            const XrTime target = frame.slot_time + period * static_cast<XrDuration>(slot);
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, target) : 1.0f);
        }
        interpolated_ready = Generate(projection, frame, mode, fractions, interpolated, interpolated_exact);
    }
    bool generated_ok = interpolated_ready;

    XrTime time = frame.slot_time;
    bool should_render = frame.should_render;
    bool real_shown = false; // the real frame went out in an earlier slot
    for (std::uint32_t slot = 0; slot <= generated; ++slot) {
        if (slot > 0) {
            XrFrameState state;
            const XrResult next = NextSlot(session, calls, period, state);
            if (XR_FAILED(next)) {
                std::scoped_lock lock(mutex_);
                InvalidateContinuityLocked();
                return next;
            }
            time = state.predictedDisplayTime;
            should_render = state.shouldRender == XR_TRUE;
        } else {
            const bool late = PaceSubmission(time, period);
            std::scoped_lock lock(mutex_);
            ++cycles_;
            late_cycles_ += late ? 1 : 0;
            last_cycle_started_late_ = late;
        }
        XrFrameEndInfo content = info; // the real frame, in the cycle's last slot
        bool distinct = !real_shown;
        bool generated_content = false;
        bool moved = false; // a generated frame, not an exact copy of a real one
        if (slot < generated) {
            if (interpolated_ready) {
                // The newest frame's depth describes its own image and eye
                // position: only an exact copy of it may carry it.
                const bool exact = slot < interpolated_exact.size() && interpolated_exact[slot];
                content = with_views(interpolated[slot], projection.space, projection.layerFlags, exact);
                generated_content = true;
                moved = !exact;
            } else if (mode == FrameGenSynthesisMode::kExtrapolate && slot < extrapolated_sets) {
                // From the previous real frame, whose depth image the game has
                // since replaced: no depth.
                content = with_views(extrapolated_.sets[slot], extrapolated_.space, extrapolated_.flags, false);
                generated_content = true;
                moved = !(slot < extrapolated_.exact.size() && extrapolated_.exact[slot]);
            }
        }
        if (generated_content) {
            distinct = true;
        } else {
            real_shown = true;
        }
        content.displayTime = time;
        if (!should_render) {
            content.layerCount = 0;
            content.layers = nullptr;
            distinct = false;
        }
        const XrResult result = RuntimeEnd(calls, session, &content);
        std::scoped_lock lock(mutex_);
        NoteSubmissionLocked(distinct, moved);
        if (XR_FAILED(result)) {
            PauseLocked(std::string(slot < generated ? "runtime rejected a generated frame (" : "runtime rejected the real frame (") +
                        std::to_string(result) + ")");
            InvalidateContinuityLocked();
            return result;
        }
    }

    if (mode == FrameGenSynthesisMode::kExtrapolate) {
        // The newest frame pushed forward, for the next cycle's generated slots.
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            const XrTime target = newest_time + period * static_cast<XrDuration>(slot + 1);
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, target) : 0.0f);
        }
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        const bool ok = Generate(projection, frame, mode, fractions, sets, exact);
        generated_ok = ok;
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = ok;
        extrapolated_.sets = std::move(sets);
        extrapolated_.exact = std::move(exact);
        extrapolated_.generation = next_generation_++;
        extrapolated_.space = projection.space;
        extrapolated_.flags = projection.layerFlags;
    }
    // A frame that did not reach the GPU history (not captured, or failed) is
    // not the previous frame of the next one: its interval and eye motion
    // would be measured against a different pair than the flow.
    if (generated_ok) RecordReal(projection, newest_time, frame.sequence);
    std::scoped_lock lock(mutex_);
    if (!generated_ok) InvalidateContinuityLocked();
    consecutive_demotions_ = 0;
    return XR_SUCCESS;
}

void FrameGenPresenter::RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time,
                                   std::uint64_t sequence) {
    std::scoped_lock lock(mutex_);
    last_real_.interval = last_real_.valid && last_real_.sequence + 1 == sequence && display_time > last_real_.display_time
                              ? display_time - last_real_.display_time
                              : 0;
    last_real_.valid = true;
    last_real_.sequence = sequence;
    last_real_.display_time = display_time;
    last_real_.space = projection.space;
    last_real_.views.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        last_real_.views.push_back({Extent(projection.views[i]), Geometry(projection.views[i])});
    }
}

std::vector<std::array<float, 3>> FrameGenPresenter::EyeShiftsLocked(const XrCompositionLayerProjection& projection,
                                                                     XrTime newest_time,
                                                                     FrameGenSynthesisMode mode) const {
    std::vector<std::array<float, 3>> shifts(projection.viewCount, {0.0f, 0.0f, 0.0f});
    const double seconds = static_cast<double>(newest_time - last_real_.display_time) / 1e9;
    if (!last_real_.valid || seconds <= 0.0 || last_real_.views.size() != projection.viewCount) {
        return shifts;
    }
    // One unit of generated-frame time is the interval between the two real
    // frames, so the eyes move as far per unit as they did between them:
    // onward when extrapolating, back toward the previous frame when
    // interpolating. Runtimes are not asked where the eyes will be: Pimax
    // returns about the current pose for any later time, which kept
    // generated frames at the real frame's eye position.
    const double sign = mode == FrameGenSynthesisMode::kExtrapolate ? 1.0 : -1.0;
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const XrPosef& pose = projection.views[i].pose;
        const std::array<float, 3>& previous = last_real_.views[i].geometry.position;
        const std::array<double, 3> delta{pose.position.x - previous[0], pose.position.y - previous[1],
                                          pose.position.z - previous[2]};
        // Into the eye's own view space at the newest frame: the same
        // movement in the view the game rendered, whatever it did to the
        // world around it.
        const std::array<double, 3> local = RotateInverse(pose.orientation, delta);
        const double length = std::sqrt(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
        if (std::isfinite(length) && length < kMaxEyeSpeed * seconds) {
            shifts[i] = {static_cast<float>(sign * local[0]), static_cast<float>(sign * local[1]),
                         static_cast<float>(sign * local[2])};
        }
    }
    return shifts;
}

void FrameGenPresenter::NoteEyeMovementLocked(const XrCompositionLayerProjection& projection,
                                              const PendingFrame& frame) {
    const double seconds = static_cast<double>(frame.app_time - last_real_.display_time) / 1e9;
    if (projection.viewCount == 0 || last_real_.views.empty() || seconds <= 0.0) {
        eye_velocity_.reset();
        return;
    }
    const XrVector3f& now = projection.views[0].pose.position;
    const std::array<float, 3>& before = last_real_.views[0].geometry.position;
    const std::array<double, 3> moved{now.x - before[0], now.y - before[1], now.z - before[2]};
    const double length = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    if (eye_velocity_ && eye_velocity_sequence_ + 1 == frame.sequence && length < kMaxEyeSpeed * seconds) {
        const std::array<double, 3>& velocity = *eye_velocity_;
        const std::array<double, 3> miss{moved[0] - velocity[0] * seconds, moved[1] - velocity[1] * seconds,
                                         moved[2] - velocity[2] * seconds};
        eye_move_sum_mm_ += 1000.0 * length;
        eye_miss_sum_mm_ += 1000.0 * std::sqrt(miss[0] * miss[0] + miss[1] * miss[1] + miss[2] * miss[2]);
        ++eye_frames_;
    }
    eye_velocity_ = std::array<double, 3>{moved[0] / seconds, moved[1] / seconds, moved[2] / seconds};
    eye_velocity_sequence_ = frame.sequence;
}

void FrameGenPresenter::NoteGpuTimingLocked(std::uint64_t submission, const PendingFrame& frame,
                                            const std::optional<FrameGenGpuWindow>& window,
                                            const std::vector<FrameGenAnalysisTiming>& timings) {
    if (submission && frame.real_due != Clock::time_point{} && frame.generated_due != Clock::time_point{}) {
        gpu_due_.push_back({submission, frame.real_due, frame.generated_due});
        while (gpu_due_.size() > 16) {
            gpu_due_.pop_front();
        }
    }
    // Analyses the GPU finished (newer than the window below, which is read
    // a few submissions late): when their newest capture ended against the
    // real frame's refresh, how long they ran, and whether motion was
    // measured before the frames moved by it were needed.
    for (const FrameGenAnalysisTiming& timing : timings) {
        const auto due = std::find_if(gpu_due_.begin(), gpu_due_.end(),
                                      [&](const GpuDue& entry) { return entry.submission == timing.submission; });
        if (due == gpu_due_.end()) {
            continue;
        }
        if (timing.capture_end != Clock::time_point{}) {
            const double done = Milliseconds(timing.capture_end - due->real_due);
            AddSample(capture_done_ms_, done);
            AddSample(guard_capture_done_ms_, done);
        }
        if (timing.start != Clock::time_point{} && timing.end != Clock::time_point{}) {
            AddSample(analysis_ms_, Milliseconds(timing.end - timing.start));
        }
        if (timing.capture_end != Clock::time_point{} && timing.end > timing.capture_end) {
            const double lag = Milliseconds(timing.end - timing.capture_end);
            analysis_lag_ms_ = analysis_lag_ms_ > 0.0 ? analysis_lag_ms_ * 0.9 + lag * 0.1 : lag;
            AddSample(measure_lag_ms_, lag);
        }
        if (timing.hardware_start != Clock::time_point{} && timing.hardware_end != Clock::time_point{}) {
            AddSample(ofa_ms_, Milliseconds(timing.hardware_end - timing.hardware_start));
        }
        if (timing.end != Clock::time_point{}) {
            ++totals_.analyses_timed;
            totals_.analyses_late += timing.end > due->generated_due ? 1 : 0;
        }
    }
    if (!window) {
        return;
    }
    while (!gpu_due_.empty() && gpu_due_.front().submission <= window->submission) {
        const GpuDue due = gpu_due_.front();
        gpu_due_.pop_front();
        if (due.submission == window->submission) {
            AddSample(gpu_start_ms_, Milliseconds(window->start - due.real_due));
            AddSample(gpu_done_ms_, Milliseconds(window->end - due.generated_due));
        }
    }
}

bool FrameGenPresenter::Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                 FrameGenSynthesisMode mode, const std::vector<float>& fractions,
                                 std::vector<std::vector<XrCompositionLayerProjectionView>>& sets,
                                 std::vector<std::uint8_t>& exact, bool release, Analysis* analyze,
                                 const FrameGenComposition* composition) {
    const std::size_t count = analyze ? 0 : std::min(fractions.size(), kFrameGenMaxGenerated);
    if (count == 0 && !analyze) {
        return false;
    }
    // Each generated frame's time past the newest real frame (before it when
    // interpolating), in the fractions' units, and where the eyes move.
    std::vector<float> times(count, 0.0f);
    for (std::size_t index = 0; index < count; ++index) {
        times[index] = mode == FrameGenSynthesisMode::kExtrapolate ? fractions[index] : 1.0f - fractions[index];
    }
    std::vector<std::array<float, 3>> shifts(projection.viewCount, {0.0f, 0.0f, 0.0f});
    std::vector<FrameGenViewSubmission> submissions(projection.viewCount);
    std::vector<FrameGenOutputKind> kinds(projection.viewCount);
    FrameGenTuning tuning;
    std::string error;
    {
        std::scoped_lock lock(mutex_);
        if (!gpu_ || state_ == FrameGenState::kUnavailable) {
            return false;
        }
        tuning.mode = mode;
        tuning.spare = frame.spare;
        tuning.quality = settings_.flow_quality;
        tuning.hardware_flow = settings_.motion_estimation == FrameGenMotionEstimation::kHardware;
        if (tuning.hardware_flow && interpolate_on_shaders_ && mode == FrameGenSynthesisMode::kInterpolate) {
            tuning.hardware_flow = false;
            if (logger_ && !logged_interpolate_on_shaders_) {
                logged_interpolate_on_shaders_ = true;
                logger_->Info("Frame Generation measures motion on the shader cores while interpolating, though "
                              "NVIDIA's optical flow hardware is chosen: beside the game's rendering the hardware's "
                              "vectors come too late for the frames between real frames. Extrapolate uses the "
                              "hardware; VECTORXR_FRAMEGEN_INTERPOLATE_HARDWARE=1 keeps it for Interpolate too.");
            }
        }
        tuning.diagnostic = settings_.diagnostic_view;
        tuning.artifact_suppression = settings_.artifact_suppression;
        tuning.motion_limit_percent = settings_.motion_limit_percent;
        tuning.generated = static_cast<std::uint8_t>(count);
        for (std::size_t index = 0; index < count; ++index) {
            tuning.fractions[index] = fractions[index];
        }
        // Just in time: motion only, filtered for cover frames as well when
        // interpolating.
        tuning.analyze_only = analyze != nullptr;
        tuning.cover = analyze && mode == FrameGenSynthesisMode::kInterpolate;
        // Quad views: the periphery moves as its focus view does where an
        // eye blends them, and a focus view limits its periphery's motion.
        tuning.composition = analyze ? composition : nullptr;
        tuning.history_ring = analyze && !composition ? jit_canvas_ring_ : 0;
        // Nothing is measured across a pause: the motion of a game that
        // stopped for a while says nothing about its next frames.
        if (last_real_.valid && frame.app_time - last_real_.display_time > PauseLimitLocked()) {
            NotePauseLocked(frame.app_time - last_real_.display_time);
            InvalidateContinuityLocked();
        }
        const bool continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence &&
                                last_real_.space == projection.space && last_real_.views.size() == projection.viewCount;
        if (continuous && last_real_.interval > 0 && frame.app_time > last_real_.display_time) {
            tuning.interval_ratio = std::clamp(static_cast<float>(frame.app_time - last_real_.display_time) /
                                                   static_cast<float>(last_real_.interval),
                                               0.25f, 4.0f);
        }
        if (continuous) {
            // Just in time the movement is kept forward in time: frames are
            // written past the newest frame or before it as each refresh needs.
            shifts = EyeShiftsLocked(projection, frame.app_time, analyze ? FrameGenSynthesisMode::kExtrapolate : mode);
            NoteEyeMovementLocked(projection, frame);
        }
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
            const auto& view = projection.views[i];
            const auto record = swapchains_.find(view.subImage.swapchain);
            if (record == swapchains_.end() || !record->second.released) {
                error = "a projection swapchain image was never released through the layer";
                break;
            }
            if (record->second.sample_count > 1) {
                UnavailableLocked("multisampled projection swapchains are not supported");
                return false;
            }
            if (!record->second.capturable) {
                UnavailableLocked("the runtime refused the copy usage on the game's swapchains");
                return false;
            }
            const auto kind = gpu_->Classify(record->second.format);
            if (!kind) {
                UnavailableLocked("projection swapchain format " + std::to_string(record->second.format) +
                                  " is not supported");
                return false;
            }
            kinds[i] = *kind;
            auto& submission = submissions[i];
            submission.source = {view.subImage.swapchain, *record->second.released, view.subImage.imageArrayIndex,
                                 view.subImage.imageRect, record->second.format};
            submission.request.extent = Extent(view);
            if (analyze) {
                // As captured at release (OutputKindLocked): the history's
                // images match it, and frames written later encode for it.
                submission.request.output = OutputKindLocked(i, record->second.format).value_or(*kind);
            }
            submission.request.newest = Geometry(view);
            submission.request.synthesize = continuous && last_real_.views[i].extent.width == submission.request.extent.width &&
                                            last_real_.views[i].extent.height == submission.request.extent.height;
            submission.request.previous = submission.request.synthesize ? last_real_.views[i].geometry : Geometry(view);
            submission.request.temporal = true;
            submission.request.shift = shifts[i];
            if (i == 0) {
                depth_seen_ = true;
                depth_sent_ = depth_readable_ = false;
            }
            if (const auto* depth = DepthInfo(view)) {
                const auto depth_record = swapchains_.find(depth->subImage.swapchain);
                submission.request.depth =
                    depth_record != swapchains_.end() && depth_record->second.depth_capturable;
                depth_sent_ = true;
                depth_readable_ = depth_readable_ || (submission.request.depth && gpu_->SupportsDepth());
            }
        }
        if (!error.empty()) {
            NoteFailureLocked(error);
            return false;
        }
    }
    std::vector<OutputSpec> specs(projection.viewCount);
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        specs[i] = {Extent(projection.views[i]), kinds[i]};
    }
    if (count && !EnsureOutputs(specs, count, error)) {
        std::scoped_lock lock(mutex_);
        NoteFailureLocked(error);
        return false;
    }

    bool timed_out = false;
    if (count && !AcquireOutputs(count, error, timed_out)) {
        // A timed-out wait keeps its acquisition and is retried next frame;
        // an image the runtime has not handed over is never written or
        // released.
        std::scoped_lock lock(mutex_);
        if (!timed_out) {
            NoteFailureLocked(error);
        }
        ++repeated_;
        return false;
    }

    FrameGenExecuteResult executed;
    bool ok = true;
    double gpu_ms = -1.0;
    std::uint64_t working_bytes = 0;
    std::optional<FrameGenGpuWindow> window;
    std::vector<FrameGenAnalysisTiming> timings;
    FrameGenDepthStatus depth;
    std::string hardware_status;
    {
        // Vulkan queues need external synchronization with the runtime's own
        // submissions on the session queue. Lock order: queue, GPU,
        // presenter; the backend may wait on fences, so not under mutex_.
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (!gpu_) {
            ok = false;
            error = "the session ended";
        }
        for (std::size_t i = 0; i < submissions.size() && ok; ++i) {
            ok = gpu_->Track(submissions[i].source.swapchain, xr_.images, error);
            for (std::size_t set = 0; set < count && ok; ++set) {
                const Output& output = output_sets_[set][i];
                submissions[i].request.output = output.kind;
                submissions[i].outputs.push_back({output.swapchain, output.index, 0,
                                                  {{0, 0}, {static_cast<std::int32_t>(output.extent.width),
                                                            static_cast<std::int32_t>(output.extent.height)}},
                                                  output.format});
                ok = gpu_->Track(output.swapchain, xr_.images, error);
            }
        }
        if (ok) {
            if (const std::string directory = NextCaptureDirectory(); !directory.empty()) {
                gpu_->DumpNextExecute(directory);
            }
        }
        ok = ok && gpu_->Execute(submissions, tuning, executed, error);
        if (gpu_) {
            gpu_ms = gpu_->LastGpuMilliseconds();
            window = gpu_->LastGpuWindow();
            timings = gpu_->TakeTimings();
            depth = gpu_->DepthStatus();
            working_bytes = gpu_->WorkingMemoryBytes();
            if (tuning.hardware_flow) {
                hardware_status = gpu_->HardwareFlowStatus();
            }
        }
    }
    // Every output here was acquired and waited; release them all so the
    // next frame starts clean (or the caller does, once it may replace what
    // the runtime shows). Unwritten images are not submitted this frame.
    if (release && count) {
        ReleaseOutputs();
    }

    std::scoped_lock lock(mutex_);
    if (!ok) {
        NoteGpuTimingLocked(0, frame, std::nullopt, timings); // earlier analyses still finished
    }
    if (!ok && executed.uncaptured) {
        // The first frame of a view, or one whose region changed: its image
        // was released before it could be captured. The next one will be.
        ++repeated_;
        if (logger_ && !logged_uncaptured_) {
            logged_uncaptured_ = true;
            logger_->Info("Frame Generation skipped a frame whose image was not captured when the game released it "
                          "(a view's first frame, or its region changed); the next frames are captured.");
        }
        return false;
    }
    if (!ok) {
        NoteFailureLocked(error);
        return false;
    }
    gpu_ms_ = gpu_ms;
    if (working_bytes) working_bytes_ = working_bytes;
    depth_status_ = depth;
    consecutive_failures_ = 0;
    NoteGpuTimingLocked(executed.submission, frame, window, timings);
    if (logger_ && !hardware_status.empty() && hardware_status != logged_hardware_status_) {
        logged_hardware_status_ = hardware_status;
        logger_->Info(hardware_status == "active"
                          ? "Frame Generation measures motion with NVIDIA's optical flow hardware (experimental): it "
                            "starts matching each eye's image as soon as the game releases it."
                          : "Frame Generation cannot use NVIDIA's optical flow hardware (" + hardware_status +
                                "); block matching measures motion instead.");
    }
    if (logger_ && depth.decisions != logged_depth_decisions_) {
        logged_depth_decisions_ = depth.decisions;
        const FrameGenDepthEvidence& evidence = depth.evidence;
        const double moved = evidence.decoded.samples;
        logger_->Info(
            std::string("Frame Generation checked the game's depth against the other eye: reading it ") +
            ToString(depth.reading) +
            (depth.reading == FrameGenDepthReading::kUnusable
                 ? std::string(", so head movement is compensated for rotation only")
                 : (std::abs(depth.scale - 1.0f) > 0.005f
                        ? " and " + Number(depth.scale, 3) +
                              " times as far as described (its near plane is described wrongly)"
                        : std::string()) +
                       ", head movement is compensated in full") +
            ". Where depth moves content (" + Number(evidence.binocular > 0.0 ? 100.0 * moved / evidence.binocular : 0.0, 0) +
            "% of what both eyes see) it leaves " + Number(evidence.decoded.Ratio(), 2) +
            " of the error without depth (swapped " + Number(evidence.swapped.Ratio(), 2) +
            "); the other eye disproves " + Number(moved > 0.0 ? 100.0 * evidence.decoded.fallback / moved : 0.0, 0) +
            "% of it, which is treated as far (cockpit glass written to depth, or depth wrong there)." +
            (depth.reading == FrameGenDepthReading::kUnusable
                 ? " Some games only fill the depth they send while their own depth reprojection setting is on "
                   "(MSFS 2024: Motion reprojection set to Depth); it is checked again now and then."
                 : ""));
    }
    const bool synthesized =
        std::any_of(executed.synthesized.begin(), executed.synthesized.end(), [](bool value) { return value; });
    if (synthesized) {
        ++depth_window_frames_;
        if (std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
            ++depth_window_used_;
        }
    }
    if (logger_ && !logged_depth_ &&
        std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
        logged_depth_ = true;
        logger_->Info("Frame Generation uses the game's depth: head movement is compensated exactly, so content "
                      "fixed in the cockpit stays still in generated frames.");
    }
    if (synthesized) {
        generated_ += count;
    } else {
        repeated_ += count;
    }
    if (analyze) {
        analyze->frame = executed.frame;
        analyze->synthesized = synthesized;
        analyze->kinds = kinds;
        analyze->velocity.assign(projection.viewCount, {0.0f, 0.0f, 0.0f});
        for (std::size_t i = 0; i < analyze->velocity.size() && i < executed.depth.size(); ++i) {
            if (executed.depth[i]) analyze->velocity[i] = shifts[i];
        }
        sets.clear();
        exact.clear();
        return executed.frame != 0;
    }
    sets.assign(count, {});
    exact.assign(count, 0);
    double moved_mm = 0.0;
    for (std::size_t set = 0; set < count; ++set) {
        // At the newest frame's own time (or written unchanged) a set is
        // that frame exactly: same image, same eye position.
        exact[set] = !synthesized || times[set] <= 0.0f ? 1 : 0;
        auto& views = sets[set];
        views.assign(projection.views, projection.views + projection.viewCount);
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i].next = nullptr; // a generated image has no matching depth
            views[i].subImage.swapchain = output_sets_[set][i].swapchain;
            views[i].subImage.imageRect = submissions[i].outputs[set].rect;
            views[i].subImage.imageArrayIndex = 0;
            if (i < executed.depth.size() && executed.depth[i]) {
                // Seen from where the eye will be (FrameGenViewRequest::shift);
                // the runtime still corrects the rotation.
                const std::array<double, 3> local{times[set] * shifts[i][0], times[set] * shifts[i][1],
                                                  times[set] * shifts[i][2]};
                const std::array<double, 3> world = Rotate(views[i].pose.orientation, local);
                views[i].pose.position.x += static_cast<float>(world[0]);
                views[i].pose.position.y += static_cast<float>(world[1]);
                views[i].pose.position.z += static_cast<float>(world[2]);
                moved_mm = std::max(moved_mm, 1000.0 * std::sqrt(local[0] * local[0] + local[1] * local[1] +
                                                                 local[2] * local[2]));
            }
        }
    }
    if (std::any_of(executed.depth.begin(), executed.depth.end(), [](bool value) { return value; })) {
        shift_window_sum_mm_ += moved_mm;
        shift_window_max_mm_ = std::max(shift_window_max_mm_, moved_mm);
        ++shift_window_frames_;
    }
    if (logger_ && synthesized && !logged_first_generation_) {
        logged_first_generation_ = true;
        std::string extents;
        for (const auto& submission : submissions) {
            extents += (extents.empty() ? "" : ",") + std::to_string(submission.request.extent.width) + "x" +
                       std::to_string(submission.request.extent.height);
        }
        logger_->Info("Frame Generation produced its first frames: views=" + std::to_string(projection.viewCount) + " (" +
                      extents + "), generatedPerFrame=" + std::to_string(count) + ", backend=" + gpu_->Name() +
                      ", firstFraction=" + Number(fractions.front(), 3) + ", capturedAtRelease=" +
                      std::to_string(captured_ > 0) + ".");
    }
    return true;
}

std::string FrameGenPresenter::NextCaptureDirectory() {
    std::error_code ignored;
    if (capture_frames_left_ == 0) {
        const auto now = Clock::now();
        if (now - last_capture_check_ < std::chrono::seconds(1)) {
            return {};
        }
        last_capture_check_ = now;
        const char* local = std::getenv("LOCALAPPDATA");
        if (!local || !*local) {
            return {};
        }
        const std::filesystem::path root = std::filesystem::path(local) / "VectorXR";
        const std::filesystem::path request = root / "framegen-capture.request";
        if (!std::filesystem::exists(request, ignored)) {
            return {};
        }
        std::filesystem::remove(request, ignored);
        const std::time_t time = std::time(nullptr);
        std::tm local_time{};
        localtime_s(&local_time, &time);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local_time);
        capture_root_ = (root / "captures" / (std::string("framegen-") + stamp)).string();
        capture_frames_left_ = kCaptureFrames;
        capture_index_ = 0;
    }
    const std::filesystem::path directory =
        std::filesystem::path(capture_root_) / ("frame-" + std::to_string(capture_index_++));
    if (!std::filesystem::create_directories(directory, ignored) && !std::filesystem::exists(directory, ignored)) {
        capture_frames_left_ = 0;
        return {};
    }
    if (--capture_frames_left_ == 0 && logger_) {
        logger_->Info("Frame Generation captured " + std::to_string(kCaptureFrames) +
                      " real frames for offline replay in " + capture_root_ + ".");
    }
    return directory.string();
}

bool FrameGenPresenter::EnsureOutputs(const std::vector<OutputSpec>& specs, std::size_t sets, std::string& error,
                                      bool keep_replaced) {
    const std::uint32_t count = static_cast<std::uint32_t>(specs.size());
    if (!output_sets_.empty() && output_sets_.front().size() != count) {
        if (keep_replaced) {
            // Shown until a frame is written into the new ones.
            for (auto& set : output_sets_) {
                for (Output& output : set) {
                    if (output.swapchain) retired_outputs_.push_back(output);
                }
            }
            output_sets_.clear();
            std::scoped_lock lock(mutex_);
            output_kinds_.clear();
            extrapolated_ = {};
        } else {
            DestroyOutputs();
        }
    }
    if (output_kinds_.size() != count) {
        std::scoped_lock lock(mutex_);
        output_kinds_.assign(count, {});
    }
    // Near the video memory budget, sets above the session's ceiling go
    // (the ceiling never rises again, so they are not made again).
    std::size_t ceiling = output_sets_.size();
    {
        std::scoped_lock lock(mutex_);
        if (DynamicLocked()) {
            ceiling = DynamicSetCapLocked();
        } else if (settings_.multiplier == FrameGenMultiplier::kAuto && auto_memory_ceiling_ < kMaxMultiplier) {
            ceiling = auto_memory_ceiling_;
        }
    }
    if (output_sets_.size() > ceiling && ceiling >= sets) {
        TrimOutputs(ceiling);
    }
    // A higher multiplier adds sets; a lower one keeps them, unused, so
    // moving between multipliers never reallocates headset-resolution images
    // (hundreds of milliseconds on the frame thread at high resolutions).
    while (output_sets_.size() < sets) {
        output_sets_.emplace_back(count);
    }
    bool created = false;
    for (std::size_t set = 0; set < sets; ++set) {
        for (std::uint32_t i = 0; i < count; ++i) {
            Output& output = output_sets_[set][i];
            const FrameGenExtent extent = specs[i].extent;
            const FrameGenOutputKind kind = specs[i].kind;
            if (output.swapchain && output.extent.width == extent.width && output.extent.height == extent.height &&
                output.requested == kind) {
                continue;
            }
            if (output.swapchain) {
                {
                    std::scoped_lock lock(mutex_);
                    output_kinds_[i] = {};
                }
                if (keep_replaced) {
                    retired_outputs_.push_back(output);
                } else if (ForgetSwapchain(output.swapchain)) {
                    // Unfinished GPU work may still write it: then it is left
                    // to the runtime, which frees it with the session.
                    std::scoped_lock submit(submit_mutex_);
                    const PresenterCall call(*this, "xrDestroySwapchain");
                    xr_.destroy(output.swapchain);
                }
                output = {};
            }
            std::vector<std::int64_t> candidates;
            XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            {
                std::scoped_lock lock(mutex_);
                candidates = gpu_->OutputFormats(kind);
                if (kind != FrameGenOutputKind::kSrgb8) {
                    for (auto format : gpu_->OutputFormats(FrameGenOutputKind::kSrgb8)) candidates.push_back(format);
                }
                create.usageFlags = gpu_->OutputUsage();
            }
            std::int64_t format = 0;
            for (const auto candidate : candidates) {
                if (runtime_formats_.empty() ||
                    std::find(runtime_formats_.begin(), runtime_formats_.end(), candidate) != runtime_formats_.end()) {
                    format = candidate;
                    break;
                }
            }
            if (!format) {
                error = "the runtime offers no supported output swapchain format";
                return false;
            }
            create.format = format;
            create.sampleCount = 1;
            create.width = extent.width;
            create.height = extent.height;
            create.faceCount = 1;
            create.arraySize = 1;
            create.mipCount = 1;
            const XrResult result = xr_.create(session_, &create, &output.swapchain);
            if (XR_FAILED(result)) {
                output = {};
                error = "output swapchain creation failed (" + std::to_string(result) + ")";
                return false;
            }
            created = true;
            if (xr_.images) {
                xr_.images(output.swapchain, 0, &output.image_count, nullptr);
            }
            std::scoped_lock lock(mutex_);
            output.extent = extent;
            output.format = format;
            output.requested = kind;
            // An sRGB fallback for a UNORM or float source must also encode.
            output.kind = gpu_->Classify(format).value_or(FrameGenOutputKind::kSrgb8);
            output_kinds_[i] = {true, output.requested, output.kind};
            extrapolated_.valid = false;
        }
    }
    if (created) {
        // The allocation stalled this frame: let Auto settle again.
        std::scoped_lock lock(mutex_);
        multiplier_changed_at_ = Clock::now();
        cadence_.clear();
    }
    if (created) {
        // Rough video memory estimate: per view, two full-resolution history
        // copies, a full-resolution intermediate (unless generated frames are
        // drawn straight into the outputs) and the flow pyramid in the
        // backend, plus every output swapchain image.
        double working = 0.0, swapchains = 0.0;
        double copies = 3.0;
        {
            std::scoped_lock lock(mutex_);
            copies = gpu_ && gpu_->DrawsOutputs() ? 2.0 : 3.0;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const Output& first = output_sets_.front()[i];
            const double pixels = static_cast<double>(first.extent.width) * first.extent.height;
            const double bytes = first.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
            working += pixels * (copies * bytes + 3.4);
        }
        for (const auto& set : output_sets_) {
            for (const Output& output : set) {
                const double bytes = output.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
                swapchains += static_cast<double>(output.extent.width) * output.extent.height * bytes *
                              std::max<std::uint32_t>(output.image_count, 1);
            }
        }
        const double megabytes = 1024.0 * 1024.0;
        std::scoped_lock lock(mutex_);
        // Once generating, the backend reports what its images really take.
        if (working_bytes_) working = static_cast<double>(working_bytes_);
        output_bytes_ = static_cast<std::uint64_t>(swapchains);
        output_set_bytes_ = static_cast<std::uint64_t>(swapchains / static_cast<double>(output_sets_.size()));
        if (logger_) {
            logger_->Info("Frame Generation output swapchains ready: generatedPerFrame=" + std::to_string(sets) +
                          ", setsKept=" + std::to_string(output_sets_.size()) +
                          ", imagesPerSwapchain=" + std::to_string(output_sets_.front().front().image_count) +
                          ", estimated video memory " + Number((working + swapchains) / megabytes, 0) +
                          " MB (working " + Number(working / megabytes, 0) + " MB" + (working_bytes_ ? "" : " estimated") +
                          ", output swapchains " + Number(swapchains / megabytes, 0) + " MB).");
            LogMemoryBudgetLocked();
        }
    }
    return true;
}

bool FrameGenPresenter::AcquireOutputs(std::size_t sets, std::string& error, bool& timed_out) {
    timed_out = false;
    for (std::size_t index = 0; index < sets && index < output_sets_.size(); ++index) {
        for (Output& output : output_sets_[index]) {
            if (!output.acquired) {
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrResult acquired;
                {
                    std::scoped_lock submit(submit_mutex_);
                    const PresenterCall call(*this, "xrAcquireSwapchainImage");
                    acquired = xr_.acquire(output.swapchain, &acquire, &output.index);
                }
                if (XR_FAILED(acquired)) {
                    error = "output image acquire failed (" + std::to_string(acquired) + ")";
                    return false;
                }
                output.acquired = true;
                output.waited = false;
            }
            if (!output.waited) {
                XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait.timeout = kOutputWaitTimeout;
                const XrResult waited = [&] {
                    const PresenterCall call(*this, "xrWaitSwapchainImage");
                    return xr_.wait(output.swapchain, &wait);
                }();
                if (waited == XR_TIMEOUT_EXPIRED) {
                    timed_out = true;
                    error = "output image wait timed out";
                    return false;
                }
                if (XR_FAILED(waited)) {
                    error = "output image wait failed (" + std::to_string(waited) + ")";
                    return false;
                }
                output.waited = true;
            }
        }
    }
    return true;
}

void FrameGenPresenter::ReleaseOutputs() {
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (!output.acquired || !output.waited) {
                continue;
            }
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrReleaseSwapchainImage");
                xr_.release(output.swapchain, &release);
            }
            output.acquired = false;
            output.waited = false;
        }
    }
}

void FrameGenPresenter::TrimOutputs(std::size_t keep) {
    {
        // Frames generated into the sets going are dropped: the presenter
        // thread shows the newest real frame until the next batch is ready.
        std::scoped_lock submit(submit_mutex_);
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = false;
        generated_ready_ = false;
        last_shown_set_.reset();
    }
    double freed = 0.0;
    for (std::size_t set = keep; set < output_sets_.size(); ++set) {
        for (Output& output : output_sets_[set]) {
            if (!output.swapchain) continue;
            const double bytes = output.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
            freed += static_cast<double>(output.extent.width) * output.extent.height * bytes *
                     std::max<std::uint32_t>(output.image_count, 1);
            // Unfinished GPU work may still write it: then it is left to
            // the runtime, which frees it with the session.
            if (ForgetSwapchain(output.swapchain)) {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrDestroySwapchain");
                if (xr_.destroy) xr_.destroy(output.swapchain);
            }
        }
    }
    output_sets_.resize(keep);
    std::scoped_lock lock(mutex_);
    output_bytes_ = output_bytes_ > freed ? output_bytes_ - static_cast<std::uint64_t>(freed) : 0;
    if (logger_) {
        logger_->Info("Frame Generation freed " + Number(freed / (1024.0 * 1024.0), 0) +
                      " MB of output images near the video memory budget (" + std::to_string(keep) +
                      " sets kept).");
        LogMemoryBudgetLocked();
    }
}

void FrameGenPresenter::DestroyRetiredOutputs() {
    std::vector<Output> retired = std::move(retired_outputs_);
    retired_outputs_.clear();
    for (Output& output : retired) {
        if (output.acquired && output.waited) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrReleaseSwapchainImage");
            xr_.release(output.swapchain, &release);
        }
        // Unfinished GPU work may still write it: then it is left to the
        // runtime, which frees it with the session.
        if (output.swapchain && ForgetSwapchain(output.swapchain)) {
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrDestroySwapchain");
            if (xr_.destroy) xr_.destroy(output.swapchain);
        }
    }
}

void FrameGenPresenter::DestroyOutputs() {
    DestroyRetiredOutputs();
    ReleaseOutputs();
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (output.swapchain && ForgetSwapchain(output.swapchain)) {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrDestroySwapchain");
                if (xr_.destroy) xr_.destroy(output.swapchain);
            }
        }
    }
    output_sets_.clear();
    std::scoped_lock lock(mutex_);
    output_kinds_.clear();
    extrapolated_ = {};
}

void FrameGenPresenter::NoteSubmissionLocked(bool distinct, bool generated) {
    last_runtime_submit_ = Clock::now();
    submissions_.push_back(last_runtime_submit_);
    ++totals_.submissions;
    if (distinct) {
        distinct_submissions_.push_back(last_runtime_submit_);
        ++totals_.distinct;
        totals_.generated_distinct += generated ? 1 : 0;
    } else {
        ++repeated_submissions_;
    }
}

void FrameGenPresenter::NoteEndCall(Clock::time_point entered) {
    const double ms = Milliseconds(Clock::now() - entered);
    std::scoped_lock lock(mutex_);
    AddSample(end_call_ms_, ms);
}

// --- Threaded presentation -----------------------------------------------------

void FrameGenPresenter::RecordDepthReleasesLocked(FrameCopy& copy) const {
    for (FrameCopy::Layer& layer : copy.layers) {
        layer.depth_releases.assign(layer.depth.size(), 0);
        for (std::size_t view = 0; view < layer.depth.size(); ++view) {
            if (const auto it = swapchains_.find(layer.depth[view].subImage.swapchain); it != swapchains_.end()) {
                layer.depth_releases[view] = it->second.releases;
            }
        }
    }
}

void FrameGenPresenter::DropStaleDepthLocked(FrameCopy& copy) const {
    // The runtime reads a swapchain's newest image: once the game has
    // released its next depth image, the copied chain describes that one.
    for (FrameCopy::Layer& layer : copy.layers) {
        for (std::size_t view = 0; view < layer.has_depth.size() && view < layer.depth_releases.size(); ++view) {
            const auto it = swapchains_.find(layer.depth[view].subImage.swapchain);
            if (layer.has_depth[view] && it != swapchains_.end() && it->second.releases != layer.depth_releases[view]) {
                layer.has_depth[view] = 0;
            }
        }
    }
}

void FrameGenPresenter::RecordColorReleasesLocked(FrameCopy& copy) const {
    for (FrameCopy::Layer& layer : copy.layers) {
        layer.color_releases.assign(layer.views.size(), 0);
        for (std::size_t view = 0; view < layer.views.size(); ++view) {
            if (const auto it = swapchains_.find(layer.views[view].subImage.swapchain); it != swapchains_.end()) {
                layer.color_releases[view] = it->second.releases;
            }
        }
    }
}

bool FrameGenPresenter::ColorReleasedSinceLocked(const FrameCopy& copy) const {
    for (const FrameCopy::Layer& layer : copy.layers) {
        for (std::size_t view = 0; view < layer.views.size() && view < layer.color_releases.size(); ++view) {
            const auto it = swapchains_.find(layer.views[view].subImage.swapchain);
            if (it != swapchains_.end() && it->second.releases != layer.color_releases[view]) {
                return true;
            }
        }
    }
    return false;
}

FrameGenPresenter::AsIs FrameGenPresenter::CurrentAsIsLocked(const FrameCopy& copy, std::uint64_t id, XrTime time) const {
    // A copy without its projection (a swapchain or space it used was
    // destroyed) shows nothing of the scene either.
    const auto current = [&](const FrameCopy& candidate) {
        return candidate.valid && !(candidate.has_projection && candidate.projection_stale) &&
               !ColorReleasedSinceLocked(candidate);
    };
    if (current(copy)) {
        return {&copy, id, time};
    }
    // The layer composed newer frames into the same swapchains: the runtime
    // would show the newest one's images with this frame's views. That
    // frame, kept already, is shown instead, unless a frame Frame
    // Generation writes comes first (the refresh showing it would step back).
    for (const JitFrame& frame : jit_frames_) {
        if (frame.time <= time) {
            continue;
        }
        if (!frame.as_is) {
            return {};
        }
        if (current(frame.copy)) {
            return {&frame.copy, frame.id, frame.time};
        }
    }
    return {};
}

bool FrameGenPresenter::CopyFrame(const XrFrameEndInfo& info, FrameCopy& copy, std::string& unsupported) {
    copy = {};
    copy.blend_mode = info.environmentBlendMode;
    for (std::uint32_t i = 0; i < info.layerCount; ++i) {
        const XrCompositionLayerBaseHeader* header = info.layers ? info.layers[i] : nullptr;
        if (!header) {
            unsupported = "a missing composition layer";
            return false;
        }
        FrameCopy::Layer layer;
        layer.type = header->type;
        switch (header->type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
            const auto& source = *reinterpret_cast<const XrCompositionLayerProjection*>(header);
            layer.projection = source;
            layer.projection.next = nullptr;
            if (source.viewCount && source.views) {
                layer.views.assign(source.views, source.views + source.viewCount);
            }
            layer.depth.assign(layer.views.size(), XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
            layer.has_depth.assign(layer.views.size(), 0);
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                const auto* next = static_cast<const XrBaseInStructure*>(layer.views[view].next);
                layer.views[view].next = nullptr;
                if (next && next->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                    layer.depth[view] = *reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(next);
                    layer.depth[view].next = nullptr;
                    layer.has_depth[view] = 1;
                }
            }
            if (!copy.has_projection) {
                copy.has_projection = true;
                copy.projection_index = static_cast<std::uint32_t>(copy.layers.size());
            }
            break;
        }
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            layer.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(header);
            layer.quad.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            layer.cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(header);
            layer.cylinder.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            layer.equirect = *reinterpret_cast<const XrCompositionLayerEquirectKHR*>(header);
            layer.equirect.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            layer.equirect2 = *reinterpret_cast<const XrCompositionLayerEquirect2KHR*>(header);
            layer.equirect2.next = nullptr;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            layer.cube = *reinterpret_cast<const XrCompositionLayerCubeKHR*>(header);
            layer.cube.next = nullptr;
            break;
        default:
            unsupported = "composition layer type " + std::to_string(header->type);
            return false;
        }
        copy.layers.push_back(std::move(layer));
    }
    copy.valid = true;
    return true;
}

XrFrameEndInfo FrameGenPresenter::BuildFrame(FrameCopy& copy, const std::vector<XrCompositionLayerProjectionView>* generated,
                                            XrSpace generated_space, XrCompositionLayerFlags generated_flags,
                                            bool generated_depth,
                                            std::vector<const XrCompositionLayerBaseHeader*>& pointers) {
    pointers.clear();
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const void* header = nullptr;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            // A claimed quad frame's four views never reach the runtime: it
            // is shown only through the eye views composed from it.
            if (copy.has_projection && i == copy.projection_index && !generated &&
                (copy.projection_stale || copy.quad)) {
                continue;
            }
            if (generated && copy.has_projection && i == copy.projection_index) {
                // The real frame's depth describes its own image and eye
                // position. A generated frame moves content and is seen from
                // where the eye will be, so the depth would misplace near
                // edges for a runtime that corrects head movement with it;
                // generated frames are already seen from the predicted eye
                // position, so such a runtime only rotates them. An exact
                // copy of the real frame keeps it (while the game has not
                // released a newer depth image: DropStaleDepthLocked).
                layer.views = *generated;
                layer.depth.resize(layer.views.size(), XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
                layer.has_depth.resize(layer.views.size(), 0);
                if (!generated_depth) {
                    std::fill(layer.has_depth.begin(), layer.has_depth.end(), std::uint8_t{0});
                }
                layer.projection.space = generated_space;
                layer.projection.layerFlags = generated_flags;
            }
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                layer.views[view].next = layer.has_depth[view] ? &layer.depth[view] : nullptr;
            }
            layer.projection.viewCount = static_cast<std::uint32_t>(layer.views.size());
            layer.projection.views = layer.views.data();
            header = &layer.projection;
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            header = &layer.quad;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            header = &layer.cylinder;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            header = &layer.equirect;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            header = &layer.equirect2;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            header = &layer.cube;
            break;
        default:
            continue;
        }
        pointers.push_back(static_cast<const XrCompositionLayerBaseHeader*>(header));
    }
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.environmentBlendMode = copy.blend_mode;
    info.layerCount = static_cast<std::uint32_t>(pointers.size());
    info.layers = pointers.empty() ? nullptr : pointers.data();
    return info;
}

XrResult FrameGenPresenter::RuntimeWait(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameWaitInfo* info,
                                        XrFrameState* state) {
    XrResult result = XR_SUCCESS;
    {
        const PresenterCall call(*this, "xrWaitFrame");
        result = calls.wait(session, info, state);
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_waits_;
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeBegin(const FrameGenRuntimeCalls& calls, XrSession session,
                                         const XrFrameBeginInfo* info) {
    XrResult result = XR_SUCCESS;
    {
        const PresenterCall call(*this, "xrBeginFrame");
        result = calls.begin(session, info);
    }
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_begins_;
        // A begin over a frame still open discards that frame.
        runtime_ends_ = std::max(runtime_ends_, runtime_begins_ - 1);
        runtime_waits_ = std::max(runtime_waits_, runtime_begins_);
        presenter_frame_open_ = false;
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeEnd(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameEndInfo* info) {
    XrResult result = XR_SUCCESS;
    if (trace_.Enabled() && info) {
        // What the runtime is given: its first projection layer's views.
        const XrCompositionLayerProjection* projection = nullptr;
        for (std::uint32_t i = 0; info->layers && i < info->layerCount && !projection; ++i) {
            if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                projection = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
            }
        }
        const bool views = projection && projection->views && projection->viewCount > 0;
        trace_.Record("sub", 0, info->displayTime, info->layerCount,
                      views ? reinterpret_cast<std::int64_t>(projection->views[0].subImage.swapchain) : 0,
                      views ? projection->views[0].subImage.imageRect.extent.width : 0);
        if (views) {
            trace_.RecordPose("sub.v0", 0, projection->views[0].pose);
            if (projection->viewCount > 1) {
                trace_.RecordPose("sub.v1", 0, projection->views[1].pose);
            }
        }
    }
    {
        const PresenterCall call(*this, "xrEndFrame");
        result = calls.submit(session, info);
    }
    {
        std::scoped_lock lock(mutex_);
        // Counted even when the runtime rejects the frame: it is over either way.
        if (runtime_ends_ < runtime_begins_) {
            ++runtime_ends_;
        }
        presenter_frame_open_ = false;
    }
    WakeSlots();
    return result;
}

void FrameGenPresenter::NoteDisplayOffsetLocked(XrTime submitted, XrTime returned, XrDuration period) {
    if (period <= 0 || returned <= 0 || submitted <= 0) {
        return;
    }
    const double sample = static_cast<double>(submitted - returned) / static_cast<double>(period);
    if (sample < -2.0 || sample > 3.0) {
        return; // not a convention, just a stray time
    }
    display_offset_periods_ =
        display_offset_samples_ == 0 ? sample : display_offset_periods_ * 0.9 + sample * 0.1;
    if (display_offset_samples_ < 1000000) {
        ++display_offset_samples_;
    }
    if (logger_ && display_offset_samples_ == 30 && std::abs(display_offset_periods_) >= 0.1) {
        logger_->Info("Frame Generation: the game ends frames " + Number(display_offset_periods_, 2) +
                      " periods after the display time it was given; engaged frames are told that much earlier so "
                      "they are rendered for when they are shown.");
    }
}

XrDuration FrameGenPresenter::DisplayOffsetLocked(XrDuration period) const {
    if (display_offset_samples_ < 8 || std::abs(display_offset_periods_) < 0.1) {
        return 0;
    }
    return static_cast<XrDuration>(std::clamp(display_offset_periods_, -1.0, 2.0) * static_cast<double>(period));
}

void FrameGenPresenter::StripSpace(FrameCopy& copy, XrSpace space) {
    if (!copy.valid || !space) {
        return;
    }
    std::vector<FrameCopy::Layer> kept;
    kept.reserve(copy.layers.size());
    std::optional<std::uint32_t> projection_index;
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const bool primary = copy.has_projection && i == copy.projection_index;
        XrSpace layer_space = XR_NULL_HANDLE;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            layer_space = layer.projection.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            layer_space = layer.quad.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            layer_space = layer.cylinder.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            layer_space = layer.equirect.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            layer_space = layer.equirect2.space;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            layer_space = layer.cube.space;
            break;
        default:
            break;
        }
        if (layer_space == space) {
            if (!primary) {
                continue;
            }
            // Neither repeated nor reused for generated views, which share
            // its space: the presenter thread waits for the next real frame.
            copy.projection_stale = true;
        }
        if (primary) {
            projection_index = static_cast<std::uint32_t>(kept.size());
        }
        kept.push_back(std::move(layer));
    }
    copy.layers = std::move(kept);
    if (projection_index) {
        copy.projection_index = *projection_index;
    }
}

void FrameGenPresenter::StripSwapchain(FrameCopy& copy, XrSwapchain swapchain) {
    if (!copy.valid || !swapchain) {
        return;
    }
    std::vector<FrameCopy::Layer> kept;
    kept.reserve(copy.layers.size());
    std::optional<std::uint32_t> projection_index;
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const bool primary = copy.has_projection && i == copy.projection_index;
        bool uses = false;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            for (std::size_t view = 0; view < layer.views.size(); ++view) {
                if (layer.has_depth[view] && layer.depth[view].subImage.swapchain == swapchain) {
                    layer.has_depth[view] = 0;
                }
                if (layer.views[view].subImage.swapchain == swapchain) {
                    // The primary projection stays for generated views,
                    // which replace its images; it is never repeated.
                    if (primary) {
                        copy.projection_stale = true;
                    } else {
                        uses = true;
                    }
                }
            }
            break;
        case XR_TYPE_COMPOSITION_LAYER_QUAD:
            uses = layer.quad.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
            uses = layer.cylinder.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
            uses = layer.equirect.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
            uses = layer.equirect2.subImage.swapchain == swapchain;
            break;
        case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
            uses = layer.cube.swapchain == swapchain;
            break;
        default:
            break;
        }
        if (uses) {
            continue;
        }
        if (primary) {
            projection_index = static_cast<std::uint32_t>(kept.size());
        }
        kept.push_back(std::move(layer));
    }
    copy.layers = std::move(kept);
    if (projection_index) {
        copy.projection_index = *projection_index;
    }
}

void FrameGenPresenter::WakeSlots() {
    slot_cv_.notify_all();
    if (compositor_wake_) {
        SetEvent(static_cast<HANDLE>(compositor_wake_));
    }
}

bool FrameGenPresenter::CompositorWait(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
                                       const std::function<bool()>& ready) {
    for (;;) {
        if (ready()) {
            return true;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto until = std::min(deadline, now + std::chrono::milliseconds(100));
        if (compositor_wake_ && compositor_timer_) {
            LARGE_INTEGER due{};
            due.QuadPart =
                -std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count() / 100);
            HANDLE handles[] = {static_cast<HANDLE>(compositor_wake_), static_cast<HANDLE>(compositor_timer_)};
            const bool armed = SetWaitableTimer(handles[1], &due, 0, nullptr, nullptr, FALSE) != 0;
            lock.unlock();
            // The wake event is set after every change the presenter thread
            // waits for, so nothing is missed between the check and here.
            WaitForMultipleObjects(armed ? 2 : 1, handles, FALSE, armed ? 200 : 2);
            lock.lock();
        } else {
            slot_cv_.wait_until(lock, until);
        }
    }
}

void FrameGenPresenter::StartCompositorLocked(const FrameGenRuntimeCalls& calls) {
    compositor_calls_ = calls;
    compositor_stop_ = compositor_drain_ = false;
    slot_request_.reset();
    slot_offer_.reset();
    offer_done_ = false;
    last_release_slot_.reset();
    last_offer_slot_.reset();
    newest_copy_ = passthrough_copy_;
    newest_copy_id_ = 0;
    newest_copy_fresh_ = false;
    generation_pending_ = generated_ready_ = false;
    next_generated_set_ = 0;
    jit_ = gpu_->SupportsJustInTime();
    // Quad views the layer's compose site offers are composed by Frame
    // Generation for this run (switching restarts it: NoteQuadEndLocked).
    quad_mode_ = jit_ && QuadCapableLocked() && quad_offered_;
    quad_switch_pending_ = false;
    quad_unwritten_in_row_ = 0;
    jit_interpolate_ = settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate;
    jit_canvas_ring_ = jit_ && !quad_mode_ ? CanvasRingLocked() : FrameGenHistoryRing(false);
    jit_frames_.clear();
    jit_shown_.reset();
    jit_views_.clear();
    jit_content_ = {};
    jit_shown_as_is_ = false;
    jit_content_id_ = 0;
    jit_content_time_ = 0;
    jit_started_ = false;
    last_target_time_ = 0;
    if (quad_mode_ && quad_decline_resumes_) {
        quad_decline_resumes_ = false;
        if (logger_) {
            logger_->Info("Frame Generation composes the quad views again.");
        }
    }
    // Pacing measured by an earlier run (another mode, rate or engagement)
    // does not describe this one.
    ResetLeadLocked();
    dynamic_interval_slots_ = 0.0;
    begun_capture_marks_.clear();
    ++totals_.restarts;
    InvalidateContinuityLocked();
    if (!compositor_timer_) {
        compositor_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (!compositor_wake_) {
        compositor_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    compositor_running_ = true;
    try {
        compositor_ = std::thread([this] { CompositorLoop(); });
    } catch (...) {
        compositor_running_ = false;
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Error("Frame Generation could not start its presenter thread; presenting inline.");
        }
        return;
    }
    if (logger_ && !logged_threaded_) {
        logged_threaded_ = true;
        logger_->Info(std::string("Frame Generation presents from its own thread (") + gpu_->Name() + ", " +
                      ToString(settings_.synthesis_mode) +
                      (jit_ ? ", each frame written right before its refresh" : "") +
                      (quad_mode_ ? ", composing the quad views itself" : "") +
                      "): the game keeps rendering while generated frames are shown, and its frame waits are paced "
                      "without reaching the runtime.");
    }
}

void FrameGenPresenter::JoinCompositorLocked(std::unique_lock<std::mutex>& lock) {
    std::thread thread = std::move(compositor_);
    if (!thread.joinable()) {
        return;
    }
    lock.unlock();
    thread.join();
    lock.lock();
}

void FrameGenPresenter::DropUnclaimedLocked() {
    // Frames whose end has not started lose their slot; one already in its
    // end is handed its slot or released without one.
    for (auto it = threaded_frames_.begin(); it != threaded_frames_.end();) {
        if (!it->claimed) {
            ++dropped_unended_;
            dropped_unbegun_ += it->begun ? 0 : 1;
            it = threaded_frames_.erase(it);
        } else {
            ++it;
        }
    }
}

bool FrameGenPresenter::StopCompositorLocked(std::unique_lock<std::mutex>& lock,
                                             std::optional<std::chrono::milliseconds> limit) {
    // Also after the thread stopped itself: its frames would otherwise stay
    // in flight forever and hold every later wait.
    DropUnclaimedLocked();
    if (compositor_running_) {
        compositor_stop_ = true;
        WakeSlots();
        // It stops at its next slot, unless a runtime call holds it: never
        // waited on silently, and with a limit not past it.
        const auto started = Clock::now();
        const auto report = limit ? std::min(*limit, TeardownWait()) : TeardownWait();
        bool reported = false;
        while (!slot_cv_.wait_for(lock, std::chrono::milliseconds(20), [&] { return !compositor_running_; })) {
            const auto waited = Clock::now() - started;
            if (!reported && waited >= report) {
                reported = true;
                if (logger_) {
                    logger_->Error("Frame Generation: its presenter thread has not stopped after " +
                                   Number(std::chrono::duration<double>(waited).count(), 1) + " s (" +
                                   PresenterActivity() + ")" + (limit ? "; it is left stopping." : "."));
                }
            }
            if (limit && waited >= *limit) {
                return false;
            }
        }
    }
    JoinCompositorLocked(lock);
    compositor_stop_ = compositor_drain_ = false;
    slot_request_.reset();
    last_release_slot_.reset();
    generation_pending_ = generated_ready_ = false;
    newest_copy_ = {};
    jit_frames_.clear();
    jit_content_ = {};
    jit_views_.clear();
    jit_shown_.reset();
    jit_shown_as_is_ = false;
    // A claimed frame still ending is not shown (ThreadedEndJit): nothing
    // presents it any more.
    quad_mode_ = false;
    compositor_phase_ = "stopped";
    InvalidateContinuityLocked();
    WakeSlots();
    return true;
}

void FrameGenPresenter::ExitThreadedLocked(std::unique_lock<std::mutex>& lock) {
    compositor_drain_ = true;
    // This thread also ends frames: one it has not started ending would only
    // end after this wait returns, so it gives up its slot.
    if (std::this_thread::get_id() == end_thread_) {
        DropUnclaimedLocked();
    }
    WakeSlots();
    if (!slot_cv_.wait_for(lock, kThreadedDrain, [&] { return !compositor_running_; }) && logger_) {
        logger_->Error("Frame Generation: frames in flight did not end within 2 s; stopping the presenter thread.");
    }
    StopCompositorLocked(lock);
    if (logger_) {
        logger_->Info("Frame Generation stopped presenting from its own thread; frames go straight to the runtime.");
    }
}

void FrameGenPresenter::UpdateLeadLocked(bool late, std::optional<double> slack_ms, std::uint32_t lead,
                                         std::uint32_t multiplier) {
    const auto now = Clock::now();
    if (lead_calm_since_ == Clock::time_point{}) {
        lead_calm_since_ = now;
    }
    // A probe nothing late undid within its judging time: its lead holds.
    if (lead_probe_ && now - lead_probe_->at >= lead_policy_.judge) {
        if (lead_floor_ == 0 || lead_slots_ < lead_floor_) {
            lead_floor_ = lead_slots_;
            lead_explore_after_ = lead_policy_.explore_after;
            lead_floor_failed_ = false;
        }
        lead_probe_.reset();
    }
    // Frames predicted with another lead say nothing about this one (those
    // in flight across a step would move it twice, or judge a probe by
    // frames it never set).
    if (lead == lead_slots_ && lead_probe_ && lead_probe_->grace > 0) {
        --lead_probe_->grace;
    } else if (lead == lead_slots_) {
        const double period_ms = static_cast<double>(std::max<XrDuration>(last_period_, 1)) / 1e6;
        // Slack: it could have arrived a refresh later and still made its slot.
        lead_history_.push_back(late ? 1 : (slack_ms && *slack_ms > period_ms * 1.1) ? 2 : 0);
        while (lead_history_.size() > 10) {
            lead_history_.pop_front();
        }
        const auto count = [&](std::uint8_t kind) {
            return static_cast<std::size_t>(std::count(lead_history_.begin(), lead_history_.end(), kind));
        };
        const bool hysteresis = lead_policy_.slack_hysteresis;
        if (count(1) >= 2) {
            if (lead_probe_) {
                // The probe (or step down) failed: the lead before it is the
                // lowest known to hold, and below it is tried only rarely
                // (more rarely after each failure there).
                const bool exploring = lead_floor_ != 0 && lead_probe_->from <= lead_floor_;
                lead_floor_ = lead_probe_->from;
                lead_floor_failed_ = true;
                lead_explore_after_ = exploring ? std::min(lead_explore_after_ * 2, lead_policy_.explore_max)
                                                : lead_policy_.explore_after;
                ++(lead_probe_->slack ? totals_.lead_downs_undone : totals_.lead_probes_failed);
                lead_probe_.reset();
            } else if (hysteresis && lead_floor_ != 0 && lead_slots_ <= lead_floor_) {
                // Late at the lowest lead known to hold: it holds no longer.
                lead_floor_ = std::min(lead_slots_ + 1, 4 * multiplier);
                lead_floor_failed_ = true;
            }
            lead_slots_ = std::min(lead_slots_ + 1, 4 * multiplier);
            ++totals_.lead_ups;
            lead_history_.clear();
            lead_calm_since_ = now;
            return;
        }
        // Frames with a refresh to spare: one slot less, judged as a probe.
        // Below the lowest lead known to hold, once frames were late there,
        // only every lead_explore_after_ (as calm probes): a game whose
        // frames mostly fit a lead but a few in ten need one more otherwise
        // steps down and back up about once a second, each time with two
        // late frames (DCS and MSFS 2024, 2026-10-09).
        const bool exploring = lead_floor_ != 0 && lead_slots_ <= lead_floor_ && lead_floor_failed_;
        if (lead_history_.size() >= 8 && count(2) >= 7 && lead_slots_ > 1 &&
            (!hysteresis || !exploring || now - lead_calm_since_ >= lead_explore_after_)) {
            if (hysteresis) {
                lead_probe_ = LeadProbe{now, lead_slots_};
                lead_probe_->slack = true;
            }
            --lead_slots_;
            ++totals_.lead_downs;
            lead_history_.clear();
            lead_calm_since_ = now;
            return;
        }
    }
    // Calm: one slot less (a probe), every probe_after down to the lowest
    // lead known to hold, below it every lead_explore_after_. A burst of
    // hitches leaves latency for seconds, not for good.
    if (lead_probe_ || lead_slots_ <= 1) {
        return;
    }
    const auto wait =
        lead_floor_ != 0 && lead_slots_ <= lead_floor_ ? lead_explore_after_ : lead_policy_.probe_after;
    if (now - lead_calm_since_ < wait) {
        return;
    }
    lead_probe_ = LeadProbe{now, lead_slots_};
    --lead_slots_;
    ++totals_.lead_probes;
    lead_history_.clear();
    lead_calm_since_ = now;
}

void FrameGenPresenter::ScoreLeadLocked(JitFrame& frame, bool late) {
    if (frame.scored) {
        return;
    }
    frame.scored = true;
    // Slack counts only when its game frame was also on the GPU a refresh
    // early: one ending early but captured just in time needs its lead.
    UpdateLeadLocked(late, late || !frame.captured_early ? std::nullopt : frame.slack_ms, frame.lead,
                     frame.multiplier);
}

void FrameGenPresenter::ScoreErasedLocked(JitFrame& frame) {
    if (frame.scored) {
        return;
    }
    // Thread-safe without the GPU lock (FrameGenGpu::CaptureReached).
    const bool captured = !gpu_ || !frame.capture_mark || gpu_->CaptureReached(frame.capture_mark);
    ScoreLeadLocked(frame, !captured);
}

std::optional<std::uint64_t> FrameGenPresenter::GpuGateMarkLocked() const {
    if (!gpu_gate_enabled_ || !gpu_gate_armed_ || !gpu_) {
        return std::nullopt;
    }
    // The newest begun frame's mark covers the captures of the frame before
    // it: waiting for it leaves one game frame on the GPU, like a runtime.
    std::uint64_t mark = 0;
    if (gpu_frames_ahead_ <= 0) {
        mark = last_capture_mark_;
    } else if (begun_capture_marks_.size() >= static_cast<std::size_t>(gpu_frames_ahead_)) {
        mark = begun_capture_marks_[begun_capture_marks_.size() - static_cast<std::size_t>(gpu_frames_ahead_)];
    }
    return mark ? std::optional<std::uint64_t>(mark) : std::nullopt;
}

std::uint64_t FrameGenPresenter::ReachedCaptureMarkLocked(std::uint64_t known) const {
    // Thread-safe without the GPU lock (FrameGenGpu::CaptureReached).
    std::uint64_t low = known;
    std::uint64_t high = last_capture_mark_;
    if (!gpu_ || high <= low) {
        return low;
    }
    if (gpu_->CaptureReached(high)) {
        return high;
    }
    while (high - low > 1) {
        const std::uint64_t middle = low + (high - low) / 2;
        (gpu_->CaptureReached(middle) ? low : high) = middle;
    }
    return low;
}

void FrameGenPresenter::NoteGpuWaitLocked(double waited_ms, bool reached, bool progressed) {
    if (waited_ms > 0.5) {
        ++totals_.gpu_waits;
        AddSample(gpu_wait_ms_, waited_ms);
    }
    if (!reached) {
        ++totals_.gpu_wait_timeouts;
    }
    if (reached || progressed) {
        gpu_timeouts_in_row_ = 0;
        return;
    }
    if (++gpu_timeouts_in_row_ >= kGpuTimeoutsBeforeOff && gpu_gate_enabled_) {
        gpu_gate_enabled_ = false;
        if (logger_) {
            logger_->Error("Frame Generation: the GPU never reported the game's frames captured while its waits were "
                           "held (" + std::to_string(kGpuTimeoutsBeforeOff) +
                           " timeouts in a row, no capture running meanwhile); GPU-aware pacing is off for this "
                           "session.");
        }
    }
}

void FrameGenPresenter::NoteGpuEvidenceLocked(std::uint64_t real_frames, std::uint64_t late_gpu,
                                              std::vector<double> capture_done_ms) {
    if (gpu_gate_armed_ || !gpu_gate_enabled_) {
        return;
    }
    const std::optional<double> capture_done = Median(std::move(capture_done_ms));
    const bool late_frames = real_frames > 0 && late_gpu * 100 > real_frames;
    if (!late_frames && !(capture_done && *capture_done > 0.0)) {
        return;
    }
    gpu_gate_armed_ = true;
    if (logger_) {
        logger_->Info("Frame Generation holds the game's waits until its GPU catches up (" +
                      (late_frames ? std::to_string(late_gpu) + " of " + std::to_string(real_frames) +
                                         " real frames were not on the GPU when first needed"
                                   : "its frames were captured " + Number(*capture_done, 1) +
                                         " ms after their refresh, typically") +
                      "): one game frame may stay queued on its GPU, as a runtime's wait allows.");
    }
}

std::optional<XrResult> FrameGenPresenter::ThreadedWaitLocked(std::unique_lock<std::mutex>& lock, XrFrameState* state,
                                                            const FrameGenRuntimeCalls& calls) {
    if (!compositor_running_) {
        JoinCompositorLocked(lock);
        if (!compositor_running_) {
            if (!WantsEngagementLocked() || !ThreadedEligibleLocked()) {
                return std::nullopt;
            }
            StartCompositorLocked(calls);
            if (!compositor_running_) {
                return std::nullopt;
            }
        }
    }
    // Like a runtime: a wait returns only once the previous frame has begun,
    // and at most two frames are in flight.
    slot_cv_.wait_for(lock, std::chrono::seconds(1), [&] {
        return !compositor_running_ ||
               (std::all_of(frames_.begin(), frames_.end(), [](const PendingFrame& frame) { return frame.begun; }) &&
                std::all_of(threaded_frames_.begin(), threaded_frames_.end(),
                            [](const ThreadedFrame& frame) { return frame.begun; }) &&
                threaded_frames_.size() < 2);
    });
    if (!compositor_running_) {
        return std::nullopt;
    }
    // Released every Nth refresh; a late application is released at once.
    // Dynamic releases at most once a refresh, like a runtime: the game runs
    // at its own rate and generated frames fill the refreshes it misses.
    const bool dynamic = DynamicLocked();
    const std::uint32_t multiplier = std::max(2u, EffectiveMultiplierLocked());
    const std::uint32_t cadence = dynamic ? 1 : multiplier;
    std::uint64_t release = last_release_slot_ ? *last_release_slot_ + cadence : slot_index_ + 1;
    release = std::max(release, slot_index_);
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2),
                              [&] { return !compositor_running_ || slot_index_ >= release; })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error("Frame Generation: the presenter thread has published no refresh for 2 s (" +
                           PresenterActivity() + ").");
        }
    }
    if (!compositor_running_) {
        return std::nullopt;
    }
    // The cadence counts from the refresh it released this wait in, not from
    // the end of a GPU wait below: a game held for its GPU is not also held
    // back a cadence.
    last_release_slot_ = slot_index_;
    // A runtime's wait holds the game until its GPU has caught up; this one
    // must too, or the game runs frames ahead on its GPU and they are
    // captured after their refresh (one game frame may stay queued).
    if (const std::optional<std::uint64_t> mark = GpuGateMarkLocked()) {
        if (gpu_->CaptureReached(*mark)) {
            // The GPU keeps up: earlier timeouts were a hitch.
            gpu_timeouts_in_row_ = 0;
            gpu_reached_mark_ = std::max(gpu_reached_mark_, *mark);
        } else {
            // Timeouts in a row turn holding off only while nothing runs: a
            // capture finishing since the last wait means a slow GPU (a
            // hitch), not a stuck one.
            const std::uint64_t before = gpu_reached_mark_;
            ++gpu_waiters_;
            FrameGenGpu* const gpu = gpu_.get();
            const XrDuration wait_period = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
            // At least a few refreshes, and two of the game's own frames: a
            // game whose GPU runs slower than that for a while is held, not
            // let go after each frame.
            const auto timeout = std::max({std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::nanoseconds(4 * wait_period)),
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::nanoseconds(static_cast<std::int64_t>(2.0 * real_interval_ns_))),
                                           std::chrono::milliseconds(50)});
            const std::uint64_t waited_for = *mark;
            lock.unlock();
            const auto begun = Clock::now();
            const bool reached = gpu->WaitCapture(waited_for, timeout);
            const double waited_ms = Milliseconds(Clock::now() - begun);
            lock.lock();
            --gpu_waiters_;
            idle_cv_.notify_all();
            gpu_reached_mark_ = reached ? std::max(gpu_reached_mark_, waited_for) : ReachedCaptureMarkLocked(before);
            NoteGpuWaitLocked(waited_ms, reached, gpu_reached_mark_ > before);
            if (!compositor_running_) {
                return std::nullopt;
            }
        }
    }
    const XrDuration period = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
    const double period_ms = static_cast<double>(period) / 1e6;
    const bool interpolate = settings_.synthesis_mode == FrameGenSynthesisMode::kInterpolate;
    // Interpolating just in time, a frame is shown after the frames between
    // the previous real frame and it, which are needed from the refresh
    // after the previous one's: it must end about a real frame interval
    // earlier than it is shown.
    const bool interpolate_jit = interpolate && jit_;
    if (lead_slots_ == 0) {
        // Until measured, assume the frame takes one application frame.
        const double latency = app_latency_ms_ > 0.0 ? app_latency_ms_ : period_ms * multiplier;
        const double interval = interpolate_jit ? (dynamic ? latency : period_ms * multiplier) : 0.0;
        lead_slots_ = static_cast<std::uint32_t>(std::clamp(
            std::ceil((latency + interval + 1.0) / period_ms) - (interpolate_jit ? 1.0 : 0.0), 1.0, 4.0 * multiplier));
    }
    // Predict the first slot the frame will really reach. Interpolated
    // frames before it are shown first, so the frame itself is shown (and
    // rendered for) multiplier - 1 slots later; just in time they are made
    // for the slots it is shown after instead.
    const std::uint64_t slots = std::clamp<std::uint64_t>(lead_slots_, 1, 4 * multiplier);
    const std::uint64_t between = interpolate && !interpolate_jit ? multiplier - 1 : 0;
    ThreadedFrame frame;
    frame.id = ++threaded_id_;
    frame.multiplier = multiplier;
    frame.interpolate = interpolate;
    frame.jit = jit_;
    frame.lead = static_cast<std::uint32_t>(slots);
    frame.first_slot = slot_index_ + slots;
    frame.first_time = slot_time_ + period * static_cast<XrDuration>(slots);
    frame.target_slot = frame.first_slot + between;
    frame.target_time = frame.first_time + period * static_cast<XrDuration>(between);
    if (jit_) {
        // Each real frame has a refresh of its own after the one before (a
        // shorter lead would give two the same refresh, and only the newer
        // would ever be shown).
        if (last_target_time_ && frame.target_time < last_target_time_ + period - period / 2) {
            const XrTime behind = last_target_time_ + period - frame.target_time;
            const std::uint64_t extra = static_cast<std::uint64_t>((behind + period / 2) / period);
            frame.first_slot += extra;
            frame.target_slot += extra;
            frame.first_time += period * static_cast<XrDuration>(extra);
            frame.target_time += period * static_cast<XrDuration>(extra);
        }
        last_target_time_ = frame.target_time;
    }
    // Dynamic paces the game like a runtime (once a refresh at most), so it
    // is told the refresh period; a fixed or automatic multiplier, its N.
    frame.reported_period = period * static_cast<XrDuration>(dynamic ? 1 : multiplier);
    frame.predicted =
        std::max(frame.target_time - DisplayOffsetLocked(frame.reported_period), last_returned_time_ + 1);
    frame.wait_return = Clock::now();
    last_returned_time_ = frame.predicted;
    threaded_frames_.push_back(frame);
    state->predictedDisplayTime = frame.predicted;
    state->predictedDisplayPeriod = frame.reported_period;
    state->shouldRender = slot_should_render_ ? XR_TRUE : XR_FALSE;
    full_rate_ = false;
    SetStateLocked(FrameGenState::kActive, "");
    return XR_SUCCESS;
}

XrResult FrameGenPresenter::ThreadedEnd(XrSession session, const XrFrameEndInfo& info, const FrameGenRuntimeCalls& calls,
                                        const ThreadedFrame& frame, Clock::time_point entered,
                                        const FrameGenQuadFrame* quad) {
    if (frame.jit) {
        return ThreadedEndJit(info, frame, entered, quad);
    }
    const XrCompositionLayerProjection* projection = FindProjection(info);
    const bool renderable = info.layerCount > 0 && projection != nullptr;
    FrameCopy copy;
    std::string unsupported;
    const bool copied = CopyFrame(info, copy, unsupported);
    std::unique_lock lock(mutex_);
    end_thread_ = std::this_thread::get_id();
    NoteDisplayOffsetLocked(info.displayTime, frame.predicted, frame.reported_period);
    // The time the app rendered for: what it ends the frame with, unless that
    // is far from anything it was told.
    const XrTime content_time =
        std::llabs(info.displayTime - frame.target_time) <= 2 * frame.reported_period ? info.displayTime : frame.target_time;
    const double latency = std::chrono::duration<double, std::milli>(entered - frame.wait_return).count();
    app_latency_ms_ = app_latency_ms_ <= 0.0 ? latency : app_latency_ms_ * 0.9 + latency * 0.1;
    app_work_ms_ += latency;
    ++app_work_frames_;
    if (!renderable) {
        ++totals_.non_renderable;
    } else if (!QuadTargetsLocked(quad)) {
        // Quad views Frame Generation could compose keep their four views
        // as targets (ClaimQuadFrame).
        RecordCaptureTargetsLocked(*projection);
    }
    if (!copied && !threaded_disabled_) {
        // The next wait returns to inline presentation.
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                          ", which the presenter thread cannot repeat between real frames.");
        }
    }
    auto erase_frame = [&] {
        threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                              [&](const ThreadedFrame& pending) { return pending.id == frame.id; }),
                               threaded_frames_.end());
    };
    // How much later it could have arrived and still made its slot: the
    // presenter thread waits for a due real frame until half a refresh after
    // the slot's wait would have returned.
    std::optional<double> slack_ms;
    if (phase_wall_ != Clock::time_point{} && frame.first_time > phase_xr_) {
        const XrDuration period = std::max<XrDuration>(last_period_, 1);
        const auto deadline = phase_wall_ + std::chrono::nanoseconds(frame.first_time - phase_xr_ + period / 2);
        slack_ms = std::chrono::duration<double, std::milli>(deadline - entered).count();
    }
    const XrTime previous_time = last_real_.display_time;
    const XrDuration period = slot_period_ > 0 ? slot_period_ : last_period_;
    // When the runtime releases the wait of the slot shown at `time`.
    auto due = [&](XrTime time) {
        return phase_wall_ != Clock::time_point{} && time > phase_xr_
                   ? phase_wall_ + std::chrono::nanoseconds(time - phase_xr_)
                   : Clock::time_point{};
    };
    const Clock::time_point first_due = due(frame.first_time);
    // Interpolating, the frames between the previous real frame and this one
    // fill the slots before it: generated now, the first shown from here.
    const bool interpolate = frame.interpolate && renderable && copied && frame.multiplier >= 2;
    std::vector<std::vector<XrCompositionLayerProjectionView>> between;
    std::vector<std::uint8_t> between_exact;
    std::vector<XrTime> between_targets;
    bool between_ok = false;
    if (interpolate) {
        lock.unlock();
        // The frames between the previous real frame and this one, then this
        // frame itself (fraction 1 reproduces it) for its own slot, N - 1
        // slots after it ended: by then the game may have released its next
        // image into the same swapchain, and the runtime shows a swapchain's
        // newest image. Released to the runtime only once this frame has its
        // slot: until then the output swapchains keep showing the previous
        // real frame's frames, still being presented.
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < frame.multiplier && fractions.size() < kFrameGenMaxGenerated; ++slot) {
            const XrTime target = frame.first_time + period * static_cast<XrDuration>(slot);
            between_targets.push_back(target);
            fractions.push_back(slot + 1 == frame.multiplier
                                    ? 1.0f
                                    : FrameGenFraction(FrameGenSynthesisMode::kInterpolate, previous_time, content_time,
                                                       target));
        }
        PendingFrame pending;
        pending.app_time = content_time;
        pending.sequence = frame.id;
        pending.real_due = pending.generated_due = first_due;
        between_ok =
            Generate(*projection, pending, FrameGenSynthesisMode::kInterpolate, fractions, between, between_exact, false);
        if (between_ok) RecordReal(*projection, content_time, frame.id);
        lock.lock();
    }
    // Ask for the first slot at or after the first one this frame fills.
    slot_request_ = SlotRequest{frame.id, frame.first_slot};
    WakeSlots();
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2), [&] {
        return !compositor_running_ || (slot_offer_ && slot_offer_->frame_id == frame.id);
    })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error("Frame Generation: a real frame has waited 2 s for a slot (" + PresenterActivity() + ").");
        }
    }
    if (!slot_offer_ || slot_offer_->frame_id != frame.id) {
        // The presenter thread stopped first; no runtime frame is left for
        // this one.
        if (slot_request_ && slot_request_->frame_id == frame.id) {
            slot_request_.reset();
        }
        erase_frame();
        WakeSlots();
        lock.unlock();
        if (interpolate) {
            ReleaseOutputs();
        }
        return XR_SUCCESS;
    }
    const SlotOffer offer = *slot_offer_;
    lock.unlock();
    if (interpolate) {
        ReleaseOutputs();
    }

    // A late frame skips the frames interpolated for the slots it missed;
    // one late for its own slot is shown itself (the game has not released
    // a newer image yet).
    const std::size_t skip = offer.slot > frame.first_slot ? static_cast<std::size_t>(offer.slot - frame.first_slot) : 0;
    const bool show_between = interpolate && between_ok && skip + 1 < between.size();
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = RuntimeBegin(calls, session, &begin_info);
    if (XR_SUCCEEDED(result)) {
        XrFrameEndInfo real = info;
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection generated_layer{};
        std::vector<XrCompositionLayerProjectionView> generated_views;
        if (show_between) {
            // The projection layer replaced by the first interpolated frame;
            // the newest frame's depth chain (valid during this end) only on
            // an exact copy of it.
            std::uint32_t projection_index = 0;
            FindProjection(info, &projection_index);
            layers.assign(info.layers, info.layers + info.layerCount);
            generated_views = between[skip];
            const bool depth = skip < between_exact.size() && between_exact[skip];
            for (std::size_t i = 0; i < generated_views.size() && i < projection->viewCount; ++i) {
                generated_views[i].next = depth ? projection->views[i].next : nullptr;
            }
            generated_layer = *projection;
            generated_layer.viewCount = static_cast<std::uint32_t>(generated_views.size());
            generated_layer.views = generated_views.data();
            layers[projection_index] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated_layer);
            real.layers = layers.data();
        }
        real.displayTime = offer.time;
        if (!offer.should_render) {
            real.layerCount = 0;
            real.layers = nullptr;
        }
        result = RuntimeEnd(calls, session, &real);
    }
    bool generate = !frame.interpolate && XR_SUCCEEDED(result) && renderable && copied && offer.should_render &&
                    frame.multiplier >= 2;

    lock.lock();
    // Frames to extrapolate: one per refresh until the next real frame's,
    // and a spare for that refresh in case the frame is late. Dynamic counts
    // the refreshes to the next frame's predicted slot (it is usually in
    // flight already). One due in the very next refresh still gets its
    // spare: a frame not generated from leaves the next one without a
    // previous frame to measure motion against. Only a game holding full
    // refresh skips generation.
    std::uint32_t count = frame.multiplier;
    bool spare = true;
    if (DynamicLocked()) {
        // A pause is not the game's pace.
        if (last_offer_slot_ && offer.slot > *last_offer_slot_ &&
            static_cast<XrDuration>(offer.slot - *last_offer_slot_) * std::max<XrDuration>(period, 1) <=
                PauseLimitLocked()) {
            const double interval = static_cast<double>(offer.slot - *last_offer_slot_);
            dynamic_interval_slots_ =
                dynamic_interval_slots_ > 0.0 ? dynamic_interval_slots_ * 0.9 + interval * 0.1 : interval;
        }
        std::uint64_t gap = DynamicSlotsLocked();
        for (const ThreadedFrame& next : threaded_frames_) {
            if (next.id > frame.id) {
                gap = next.first_slot > offer.slot ? next.first_slot - offer.slot : 1;
                break;
            }
        }
        const bool full_rate = dynamic_interval_slots_ > 0.0 && dynamic_interval_slots_ < 1.25;
        count = gap <= 1 && full_rate
                    ? 0
                    : static_cast<std::uint32_t>(std::min<std::uint64_t>(std::max<std::uint64_t>(gap, 1),
                                                                         DynamicSetCapLocked()));
        spare = count == gap;
        generate = generate && count > 0;
    }
    last_offer_slot_ = offer.slot;
    NoteSubmissionLocked(offer.should_render,
                         show_between && !(skip < between_exact.size() && between_exact[skip]));
    last_shown_set_.reset(); // a real frame, or a set not among the sets below
    // From here the presenter thread fills its slots from this frame:
    // extrapolating, again until the frames generated from it are ready,
    // then those; interpolating, the rest of the frames before it, then it.
    newest_copy_ = copied ? std::move(copy) : FrameCopy{};
    generated_ready_ = false;
    generation_pending_ = generate;
    next_generated_set_ = 0;
    if (frame.interpolate) {
        extrapolated_ = {};
        extrapolated_.interpolated = true;
        if (interpolate && between_ok && compositor_running_) {
            // The rest of the frames before it, then its copy; only the copy
            // when it was shown itself, repeated if the next frame is late.
            const std::size_t first = show_between ? skip + 1 : between.size() - 1;
            extrapolated_.valid = true;
            extrapolated_.sets.assign(between.begin() + static_cast<std::ptrdiff_t>(first), between.end());
            extrapolated_.exact.assign(between_exact.begin() + static_cast<std::ptrdiff_t>(first), between_exact.end());
            extrapolated_.generation = next_generation_++;
            extrapolated_.targets.assign(between_targets.begin() + static_cast<std::ptrdiff_t>(first),
                                         between_targets.end());
            extrapolated_.space = projection->space;
            extrapolated_.flags = projection->layerFlags;
            generated_ready_ = true;
        }
    }
    RecordDepthReleasesLocked(newest_copy_);
    const Clock::time_point real_due = due(offer.time);
    const Clock::time_point generated_due = due(offer.time + period);
    const bool late = offer.slot > frame.first_slot;
    totals_.late_cpu += late ? 1 : 0;
    UpdateLeadLocked(late, slack_ms, frame.lead, frame.multiplier);
    erase_frame();
    offer_done_ = true;
    WakeSlots();
    if (renderable && offer.should_render) {
        ++healthy_streak_;
    }
    if (XR_FAILED(result)) {
        PauseLocked("runtime rejected the real frame (" + std::to_string(result) + ")");
        InvalidateContinuityLocked();
        return result;
    }
    lock.unlock();

    if (generate) {
        std::vector<float> fractions;
        std::vector<XrTime> targets;
        // One frame per generated slot, plus a spare for the slot the next
        // real frame is due in: if that frame is late, the slot still shows
        // motion instead of repeating the last generated frame.
        for (std::uint32_t slot = 1; slot <= count && fractions.size() < kFrameGenMaxGenerated; ++slot) {
            const XrTime target = offer.time + period * static_cast<XrDuration>(slot);
            targets.push_back(target);
            fractions.push_back(FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, previous_time, content_time, target));
        }
        PendingFrame pending;
        pending.app_time = content_time;
        pending.sequence = frame.id;
        pending.real_due = real_due;
        pending.generated_due = generated_due;
        pending.spare = spare && fractions.size() == count;
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        const bool ok = Generate(*projection, pending, FrameGenSynthesisMode::kExtrapolate, fractions, sets, exact);
        if (ok) RecordReal(*projection, content_time, frame.id);
        lock.lock();
        if (!ok) InvalidateContinuityLocked();
        if (compositor_running_) {
            extrapolated_.valid = ok;
            extrapolated_.interpolated = false;
            extrapolated_.sets = std::move(sets);
            extrapolated_.exact = std::move(exact);
            extrapolated_.generation = next_generation_++;
            extrapolated_.targets = std::move(targets);
            extrapolated_.space = projection->space;
            extrapolated_.flags = projection->layerFlags;
            generated_ready_ = ok;
        } else {
            // The thread stopped meanwhile: nothing it generated belongs to
            // what comes next.
            InvalidateContinuityLocked();
        }
        generation_pending_ = false;
        WakeSlots();
    } else {
        lock.lock();
        if (!renderable || (frame.interpolate && !between_ok)) {
            InvalidateContinuityLocked();
        }
    }
    app_frames_.push_back(entered);
    ++totals_.app_frames;
    NoteRealEndLocked(entered);
    PendingFrame cadence;
    cadence.engaged = true;
    cadence.multiplier = frame.multiplier;
    cadence.period = period;
    UpdateAutoLocked(entered, cadence);
    SummaryLocked(entered);
    return result;
}

void FrameGenPresenter::SummaryLocked(Clock::time_point now) {
    if (state_ != FrameGenState::kActive || now - last_summary_ < summary_interval_) {
        return;
    }
    // Rates are the window's averages; counts are the window's, with the
    // session's in parentheses where the OSD shows them.
    const double seconds =
        last_summary_ == Clock::time_point{} ? 0.0 : std::chrono::duration<double>(now - last_summary_).count();
    const Totals& start = summary_totals_;
    const auto delta = [&](std::uint64_t Totals::*field) { return totals_.*field - start.*field; };
    const double standby = StandbySecondsLocked(now);
    NoteGpuEvidenceLocked(delta(&Totals::app_frames), delta(&Totals::late_gpu), capture_done_ms_);
    if (logger_) {
        LogMemoryBudgetLocked();
        const std::string multiplier =
            DynamicLocked() ? "dynamic (" + Number(dynamic_interval_slots_, 2) + " refreshes per real frame)"
                            : MultiplierText(EffectiveMultiplierLocked()) +
                                  (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "");
        // lateReal: real frames ended after their refresh was due / not on
        // the GPU when first chosen. gpuWaits: game waits held for its GPU
        // (count/median ms/timeouts). captureDoneMs: an analysis's newest
        // capture ended this long after the real frame's refresh was due.
        // unready: refreshes whose real frame was not captured / moved
        // frames written unmoved (motion not measured yet). unwritten: an
        // output not handed back in time / writing failed / no outputs /
        // nothing to write. endCallMs: the game's xrEndFrame in Frame
        // Generation. synthLockMs: the presenter thread's wait for the GPU
        // lock before writing. ringExhausted: refreshes whose real frame's
        // images newer captures had replaced (written unmoved, or the frame
        // before). staleSkipped: refreshes not submitted, nothing current to
        // show (a frame shown as it is had newer images in its swapchains).
        logger_->Info(
            "Frame Generation: multiplier=" + multiplier + ", threaded=1, appFps=" +
            Number(PerSecond(delta(&Totals::app_frames), seconds), 1) +
            ", outputFps=" + Number(PerSecond(delta(&Totals::submissions), seconds), 1) +
            ", distinctFps=" + Number(PerSecond(delta(&Totals::distinct), seconds), 1) +
            ", standbyS=" + Number(standby - summary_standby_seconds_, 1) +
            ", repeatedFrames=" + std::to_string(repeated_submissions_) + ", appLatencyMs=" + Number(app_latency_ms_, 1) +
            ", leadSlots=" + std::to_string(lead_slots_) + ", leadFloor=" + std::to_string(lead_floor_) +
            ", leadMoves=+" + std::to_string(delta(&Totals::lead_ups)) + "/-" +
            std::to_string(delta(&Totals::lead_downs)) + ", leadProbes=" + std::to_string(delta(&Totals::lead_probes)) +
            "/" + std::to_string(delta(&Totals::lead_probes_failed)) +
            ", leadUndone=" + std::to_string(delta(&Totals::lead_downs_undone)) +
            ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
            ", lateReal=" + std::to_string(delta(&Totals::late_cpu)) + "/" + std::to_string(delta(&Totals::late_gpu)) +
            ", skippedGenerated=" + std::to_string(skipped_generated_) + ", generated=+" +
            std::to_string(delta(&Totals::generated_distinct)) + " (" + std::to_string(totals_.generated_distinct) +
            ")" + ", copiesSkipped=" + std::to_string(delta(&Totals::copies_skipped)) +
            ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) +
            ", captured=+" + std::to_string(delta(&Totals::captured)) + " (" + std::to_string(captured_) + ")" +
            ", gpuMs=" + Number(gpu_ms_, 2) + ", depthUsed=" +
            Number(depth_window_frames_ ? 100.0 * depth_window_used_ / depth_window_frames_ : 0.0, 0) +
            "%, eyeShiftMm=" + Number(shift_window_frames_ ? shift_window_sum_mm_ / shift_window_frames_ : 0.0, 1) +
            "/" + Number(shift_window_max_mm_, 1) +
            ", eyeMoveMm=" + Number(eye_frames_ ? eye_move_sum_mm_ / eye_frames_ : 0.0, 1) +
            ", eyeMissMm=" + Number(eye_frames_ ? eye_miss_sum_mm_ / eye_frames_ : 0.0, 1) +
            ", gpuStartMs=" + MedianAndMax(gpu_start_ms_) + ", gpuDoneMs=" + MedianAndMax(gpu_done_ms_) +
            ", captureDoneMs=" + MedianAndMax(capture_done_ms_) + ", analysisMs=" + MedianAndMax(analysis_ms_) +
            ", measureLagMs=" + MedianAndMax(measure_lag_ms_) +
            ", ofaMs=" + MedianAndMax(ofa_ms_) + ", gpuWaits=" + std::to_string(delta(&Totals::gpu_waits)) + "/" +
            (gpu_wait_ms_.empty() ? std::string("-") : Number(*Median(gpu_wait_ms_), 1)) + "/" +
            std::to_string(delta(&Totals::gpu_wait_timeouts)) +
            (gpu_gate_enabled_ && gpu_gate_armed_ ? "" : gpu_gate_enabled_ ? " (not armed)" : " (off)") +
            ", endCallMs=" + MedianAndMax(end_call_ms_) + ", synthLockMs=" + MedianAndMax(synth_lock_ms_) +
            (jit_ ? ", justInTime=1, covers=" + std::to_string(jit_covers_) + ", unwritten=" +
                        std::to_string(delta(&Totals::unwritten_wait)) + "/" +
                        std::to_string(delta(&Totals::unwritten_failed)) + "/" +
                        std::to_string(delta(&Totals::unwritten_outputs)) + "/" +
                        std::to_string(delta(&Totals::unwritten_source)) +
                        ", unready=" + std::to_string(delta(&Totals::unready)) + "/" +
                        std::to_string(delta(&Totals::unmeasured)) +
                        ", staleMotion=" + std::to_string(delta(&Totals::stale_motion)) +
                        ", ringExhausted=" + std::to_string(delta(&Totals::ring_exhausted)) +
                        ", staleSkipped=" + std::to_string(delta(&Totals::stale_skipped))
                  : std::string()) +
            ", pauses=" + std::to_string(delta(&Totals::pauses)) + "/" + Number(pause_longest_ms_, 0) +
            DynamicSummaryLocked() + QuadSummaryLocked(start) + ".");
    }
    last_summary_ = now;
    summary_totals_ = totals_;
    summary_standby_seconds_ = standby;
    eye_move_sum_mm_ = eye_miss_sum_mm_ = 0.0;
    eye_frames_ = 0;
    gpu_start_ms_.clear();
    gpu_done_ms_.clear();
    capture_done_ms_.clear();
    analysis_ms_.clear();
    measure_lag_ms_.clear();
    ofa_ms_.clear();
    gpu_wait_ms_.clear();
    end_call_ms_.clear();
    synth_lock_ms_.clear();
    pause_longest_ms_ = 0.0;
    app_work_ms_ = 0.0;
    app_work_frames_ = 0;
    repeated_submissions_ = 0;
    skipped_generated_ = 0;
    depth_window_frames_ = depth_window_used_ = 0;
    shift_window_sum_mm_ = shift_window_max_mm_ = 0.0;
    shift_window_frames_ = 0;
    jit_covers_ = 0;
}

bool FrameGenPresenter::AnalyzeJit(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                   FrameGenSynthesisMode mode, FrameCopy copy, const JitFrame& score,
                                   std::shared_ptr<const FrameGenQuadFrame> quad) {
    XrTime previous_time = 0;
    {
        std::scoped_lock lock(mutex_);
        // Nothing is measured or moved across a pause: the frame after it is
        // shown as it is, and motion is measured again from the next.
        if (last_real_.valid && frame.app_time - last_real_.display_time > PauseLimitLocked()) {
            NotePauseLocked(frame.app_time - last_real_.display_time);
            InvalidateContinuityLocked();
        }
        if (last_real_.valid && last_real_.sequence + 1 == frame.sequence && frame.app_time > last_real_.display_time) {
            previous_time = last_real_.display_time;
        }
    }
    Analysis analysis;
    std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
    std::vector<std::uint8_t> exact;
    const bool ok =
        Generate(projection, frame, mode, {}, sets, exact, true, &analysis, quad ? &quad->composition : nullptr);
    // A frame that did not reach the GPU history is not the previous frame
    // of the next one (see RunCycle).
    if (ok) RecordReal(projection, frame.app_time, frame.sequence);
    std::scoped_lock lock(mutex_);
    if (!ok || !compositor_running_ || !copy.valid || !copy.has_projection) {
        // Not kept: the frame is shown unchanged in its own refresh and
        // nothing is generated from it (frames are found by id).
        InvalidateContinuityLocked();
        if (!score.scored) {
            UpdateLeadLocked(false, score.slack_ms, score.lead, score.multiplier);
        }
        return false;
    }
    // Frames of another view size (or output kind) are never written again:
    // their output images would be made anew each time one was chosen. So
    // are frames composed otherwise, or into images of another size. Frames
    // shown as the layer composed them write nothing.
    const auto& views = copy.layers[copy.projection_index].views;
    auto differs = [&](const JitFrame& kept) {
        if (kept.as_is) {
            return false;
        }
        if ((kept.quad != nullptr) != (quad != nullptr) ||
            (quad && (kept.quad->composition.output.width != quad->composition.output.width ||
                      kept.quad->composition.output.height != quad->composition.output.height))) {
            return true;
        }
        const auto& kept_views = kept.copy.layers[kept.copy.projection_index].views;
        if (kept_views.size() != views.size() || kept.kinds != analysis.kinds) {
            return true;
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (kept_views[i].subImage.imageRect.extent.width != views[i].subImage.imageRect.extent.width ||
                kept_views[i].subImage.imageRect.extent.height != views[i].subImage.imageRect.extent.height) {
                return true;
            }
        }
        return false;
    };
    for (auto it = jit_frames_.begin(); it != jit_frames_.end();) {
        if (differs(*it)) {
            ScoreErasedLocked(*it);
            it = jit_frames_.erase(it);
        } else {
            ++it;
        }
    }
    JitFrame jit = score;
    jit.id = frame.sequence;
    jit.analyzed = analysis.frame;
    jit.time = frame.app_time;
    jit.previous_time = analysis.synthesized ? previous_time : 0;
    if (jit.previous_time && !jit_frames_.empty() && !jit_frames_.back().as_is &&
        jit_frames_.back().time == jit.previous_time && jit_frames_.back().analyzed) {
        jit.previous_analyzed = jit_frames_.back().analyzed;
    }
    RecordDepthReleasesLocked(copy);
    jit.copy = std::move(copy);
    jit.kinds = std::move(analysis.kinds);
    jit.velocity = std::move(analysis.velocity);
    jit.quad = std::move(quad);
    if (trace_.Enabled()) {
        trace_.Record("keep", jit.id, jit.time, jit.previous_time, static_cast<std::int64_t>(jit.analyzed),
                      static_cast<std::int64_t>(jit.previous_analyzed), 0, static_cast<std::int64_t>(jit.capture_mark));
        if (jit.copy.has_projection && jit.copy.projection_index < jit.copy.layers.size() &&
            !jit.copy.layers[jit.copy.projection_index].views.empty()) {
            trace_.RecordPose("keep.v0", jit.id, jit.copy.layers[jit.copy.projection_index].views[0].pose);
        }
    }
    jit_frames_.push_back(std::move(jit));
    // The backend keeps the images of the last few real frames: as many as
    // its ring for this analysis has (quad views keep more).
    while (jit_frames_.size() > JitRingLocked(jit_frames_.back().quad != nullptr)) {
        ScoreErasedLocked(jit_frames_.front());
        jit_frames_.pop_front();
    }
    return true;
}

XrResult FrameGenPresenter::ThreadedEndJit(const XrFrameEndInfo& info, const ThreadedFrame& frame,
                                           Clock::time_point entered, const FrameGenQuadFrame* quad) {
    // Claimed in the layer's xrEndFrame (ClaimQuadFrame): its primary
    // projection holds the four source views.
    const bool claimed = frame.quad && quad && quad->route == FrameGenQuadRoute::kClaimed;
    const XrCompositionLayerProjection* projection = FindProjection(info);
    const bool renderable = info.layerCount > 0 && projection != nullptr;
    FrameCopy copy;
    std::string unsupported;
    const bool copied = CopyFrame(info, copy, unsupported);
    copy.quad = claimed; // shown only through the views composed from it
    std::unique_lock lock(mutex_);
    end_thread_ = std::this_thread::get_id();
    NoteDisplayOffsetLocked(info.displayTime, frame.predicted, frame.reported_period);
    const double latency = std::chrono::duration<double, std::milli>(entered - frame.wait_return).count();
    app_latency_ms_ = app_latency_ms_ <= 0.0 ? latency : app_latency_ms_ * 0.9 + latency * 0.1;
    app_work_ms_ += latency;
    ++app_work_frames_;
    if (!renderable) {
        ++totals_.non_renderable;
    } else if (!QuadTargetsLocked(quad)) {
        // Quad views Frame Generation could compose keep their four views
        // as targets (ClaimQuadFrame).
        RecordCaptureTargetsLocked(*projection);
    }
    if (!copied && !threaded_disabled_) {
        // The next wait returns to inline presentation.
        threaded_disabled_ = true;
        if (logger_) {
            logger_->Info("Frame Generation presents inline: the game submits " + unsupported +
                          ", which the presenter thread cannot repeat between real frames.");
        }
    }
    const XrDuration period = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
    // When it is needed: extrapolating, in its own refresh; interpolating,
    // from the refresh after the newest real frame's (its own, the first),
    // for the frames between. Measured, then written on the GPU, by when
    // the runtime releases that refresh's wait.
    const XrTime needed = !frame.interpolate || jit_frames_.empty()
                              ? frame.target_time
                              : std::min(frame.target_time, jit_frames_.back().time + period);
    // Interpolating with a refresh between it and the newest real frame,
    // that refresh shows it moved: its motion is needed then, not only its
    // capture.
    const bool moved_first = needed < frame.target_time - period / 2;
    // A refresh already past (before the runtime's last paced wait) maps
    // back on the same clock: the frame is that much late.
    std::optional<double> slack_ms;
    Clock::time_point needed_due{};
    if (phase_wall_ != Clock::time_point{}) {
        needed_due = phase_wall_ + std::chrono::nanoseconds(needed - phase_xr_);
        slack_ms = std::chrono::duration<double, std::milli>(needed_due - std::chrono::milliseconds(2) - entered).count();
    }
    // Ending after its refresh was due, it is late at once. Otherwise it is
    // judged when a refresh first chooses it: its game frame must have
    // reached the GPU by then too (CompositorLoop).
    const bool late = slack_ms && *slack_ms < 0.0;
    if (late) {
        ++totals_.late_cpu;
        UpdateLeadLocked(true, slack_ms, frame.lead, frame.multiplier);
    }
    JitFrame score;
    score.lead = frame.lead;
    score.multiplier = frame.multiplier;
    score.needed = needed;
    // Moved at its first refresh, it could have come only as much later as
    // its analysis leaves (its capture is judged when that refresh nears).
    score.slack_ms = slack_ms && moved_first ? std::optional<double>(*slack_ms - analysis_lag_ms_) : slack_ms;
    score.capture_mark = last_capture_mark_; // its views were released before its end
    score.scored = late;
    // A pause is not the game's pace.
    if (!jit_frames_.empty() && frame.target_time > jit_frames_.back().time &&
        frame.target_time - jit_frames_.back().time <= PauseLimitLocked()) {
        const double interval =
            static_cast<double>(frame.target_time - jit_frames_.back().time) / static_cast<double>(period);
        dynamic_interval_slots_ =
            dynamic_interval_slots_ > 0.0 ? dynamic_interval_slots_ * 0.9 + interval * 0.1 : interval;
    }
    // The next wait may go ahead; the presenter thread waits for this
    // measurement when a refresh needs it first.
    threaded_frames_.erase(std::remove_if(threaded_frames_.begin(), threaded_frames_.end(),
                                          [&](const ThreadedFrame& pending) { return pending.id == frame.id; }),
                           threaded_frames_.end());
    // Composing quad views, only a claimed frame is measured. Any other
    // (the layer composed it) is shown as it is in its own refresh, after
    // the frames before it, and nothing is generated across it.
    const bool usable = renderable && copied && compositor_running_;
    const bool measure = usable && (claimed || !quad_mode_);
    const bool as_is = usable && !measure;
    // Shown as it is (the fallback when nothing is written), only while its
    // images are still the newest in their swapchains.
    RecordColorReleasesLocked(copy);
    if (measure) {
        // Its four views never reach the runtime as they are.
        if (!claimed) {
            newest_copy_ = copy;
            newest_copy_fresh_ = true;
            newest_copy_id_ = frame.id;
        }
        ++healthy_streak_;
    } else if (as_is) {
        newest_copy_ = copy;
        newest_copy_fresh_ = true;
        newest_copy_id_ = frame.id;
        ++healthy_streak_;
        JitFrame kept = score;
        kept.id = frame.id;
        kept.time = frame.target_time;
        kept.as_is = true;
        kept.scored = true; // scored below: nothing of it waits for the GPU
        RecordDepthReleasesLocked(copy);
        kept.copy = copy;
        trace_.Record("keep", kept.id, kept.time, 0, 0, 0, 1);
        jit_frames_.push_back(std::move(kept));
        while (jit_frames_.size() > JitRingLocked(quad_mode_)) {
            ScoreErasedLocked(jit_frames_.front());
            jit_frames_.pop_front();
        }
        InvalidateContinuityLocked();
    } else if (!renderable) {
        // No projection (a loading screen's quads, or nothing at all): the
        // presenter thread shows its layers as they are until the next frame,
        // and nothing is generated across the gap.
        newest_copy_ = copied ? copy : FrameCopy{};
        newest_copy_fresh_ = true;
        newest_copy_id_ = frame.id;
        for (JitFrame& kept : jit_frames_) {
            ScoreErasedLocked(kept);
        }
        jit_frames_.clear();
        jit_content_ = {};
        jit_views_.clear();
        jit_shown_.reset();
        jit_shown_as_is_ = false;
        InvalidateContinuityLocked();
    }
    if (!measure && !late) {
        // Not kept: on time.
        UpdateLeadLocked(false, slack_ms, frame.lead, frame.multiplier);
    }
    generation_pending_ = measure;
    WakeSlots();
    lock.unlock();
    if (measure) {
        PendingFrame pending;
        pending.app_time = frame.target_time;
        pending.sequence = frame.id;
        pending.period = period;
        // Its exact copy needs its capture by its refresh; its motion is
        // needed only a refresh later, by the first frame moved on from it,
        // unless interpolating with a refresh between it and the newest real
        // frame, which shows it moved.
        pending.real_due = needed_due;
        pending.generated_due = needed_due != Clock::time_point{} && !moved_first
                                    ? needed_due + std::chrono::nanoseconds(period)
                                    : needed_due;
        // A claimed frame keeps its composition and eye views, made for its
        // own display time, for every frame written from it.
        std::shared_ptr<FrameGenQuadFrame> kept_quad;
        if (claimed) {
            kept_quad = std::make_shared<FrameGenQuadFrame>(*quad);
            for (XrCompositionLayerProjectionView& view : kept_quad->eyes) view.next = nullptr;
            for (XrCompositionLayerProjectionView& view : kept_quad->source_views) view.next = nullptr;
        }
        const bool kept =
            AnalyzeJit(*projection, pending,
                       frame.interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate,
                       std::move(copy), score, std::move(kept_quad));
        if (claimed && !kept) {
            lock.lock();
            ++totals_.quad_unanalyzed;
            if (logger_ && !logged_quad_unanalyzed_) {
                logged_quad_unanalyzed_ = true;
                logger_->Error("Frame Generation could not measure the motion of a quad views frame it composes; "
                               "the frame was not shown.");
            }
            lock.unlock();
        }
    } else if (as_is && quad && quad->route == FrameGenQuadRoute::kUncaptured) {
        // Its views' releases were not captured (their first frame, or a new
        // region): they are set up from it, so the next ones are (the
        // backend's Execute stops there, uncaptured). Set up as composed, in
        // the ring of the claimed frames that follow (FrameGenHistoryRing).
        XrCompositionLayerProjection source{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        source.space = quad->source_space;
        source.viewCount = static_cast<std::uint32_t>(quad->source_views.size());
        source.views = quad->source_views.data();
        PendingFrame pending;
        pending.app_time = frame.target_time;
        pending.sequence = frame.id;
        pending.period = period;
        Analysis analysis;
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        std::vector<std::uint8_t> exact;
        Generate(source, pending,
                 frame.interpolate ? FrameGenSynthesisMode::kInterpolate : FrameGenSynthesisMode::kExtrapolate, {},
                 sets, exact, true, &analysis, &quad->composition);
        lock.lock();
        ++totals_.quad_setups;
        InvalidateContinuityLocked();
        lock.unlock();
    } else if (as_is) {
        // Nothing analyses it, but its views were captured: analyses' GPU
        // timings are read every few captures (the backend keeps a few).
        std::vector<FrameGenAnalysisTiming> timings;
        {
            std::unique_lock<std::mutex> queue_lock;
            if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
            std::scoped_lock gpu_lock(gpu_mutex_);
            if (gpu_) timings = gpu_->TakeTimings();
        }
        if (!timings.empty()) {
            lock.lock();
            NoteGpuTimingLocked(0, PendingFrame{}, std::nullopt, timings);
            lock.unlock();
        }
    }
    lock.lock();
    generation_pending_ = false;
    WakeSlots();
    app_frames_.push_back(entered);
    ++totals_.app_frames;
    NoteRealEndLocked(entered);
    PendingFrame cadence;
    cadence.engaged = true;
    cadence.multiplier = frame.multiplier;
    cadence.period = period;
    UpdateAutoLocked(entered, cadence);
    SummaryLocked(entered);
    return XR_SUCCESS;
}

FrameGenPresenter::JitChoice FrameGenPresenter::ChooseJitLocked(XrTime time, XrDuration period, bool interpolate,
                                                                bool& due, std::size_t limit) const {
    JitChoice choice;
    due = false;
    const std::size_t count = std::min(limit, jit_frames_.size());
    const XrDuration half = period / 2;
    // A real frame on its way that this refresh needs: interpolating, any
    // (the frames past the newest are made from the next); extrapolating,
    // one rendered for this refresh or an earlier one.
    const bool needed_in_flight =
        generation_pending_ || std::any_of(threaded_frames_.begin(), threaded_frames_.end(), [&](const ThreadedFrame& frame) {
            return interpolate || frame.target_time <= time + half;
        });
    if (count == 0) {
        due = needed_in_flight;
        return choice;
    }
    // The newest real frame whose own refresh is this one or earlier.
    std::size_t index = count;
    for (std::size_t i = count; i-- > 0;) {
        if (jit_frames_[i].time <= time + half) {
            index = i;
            break;
        }
    }
    choice.mode = FrameGenSynthesisMode::kInterpolate;
    choice.fraction = 1.0f;
    if (index == count) {
        // Before every kept real frame (the presenter thread just started):
        // the oldest, unchanged.
        choice.index = 0;
        return choice;
    }
    const JitFrame& frame = jit_frames_[index];
    if (time <= frame.time + half) {
        choice.index = index; // its own refresh: its exact copy
        return choice;
    }
    const bool newest = index + 1 == count;
    if (interpolate && !newest) {
        const JitFrame& next = jit_frames_[index + 1];
        if (next.previous_time == frame.time) {
            // Between it and the next: from the next one's measurement.
            choice.index = index + 1;
            choice.fraction = FrameGenFraction(FrameGenSynthesisMode::kInterpolate, frame.time, next.time, time);
        } else {
            // Not continuous: the nearer of the two, unchanged.
            choice.index = time - frame.time < next.time - time ? index : index + 1;
        }
        return choice;
    }
    // Past it, moved on by its motion: extrapolating, until the next real
    // frame's refresh; interpolating, the next real frame is late (a cover
    // frame). For at most a few intervals, and never past a pause: a game
    // that stopped sending frames shows its last one.
    choice.index = index;
    choice.mode = FrameGenSynthesisMode::kExtrapolate;
    choice.fraction = 0.0f;
    if (frame.previous_time && time > frame.time &&
        time - frame.time <= std::min<XrDuration>(4 * (frame.time - frame.previous_time), PauseLimitLocked())) {
        choice.fraction = FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, frame.previous_time, frame.time, time);
    }
    if (newest) {
        due = needed_in_flight;
        choice.cover = due && choice.fraction > 0.0f;
    }
    return choice;
}

bool FrameGenPresenter::JitCopyOnly(const JitFrame& frame, const JitChoice& choice) {
    return frame.previous_time == 0 ||
           (choice.mode == FrameGenSynthesisMode::kExtrapolate ? choice.fraction <= 0.0f : choice.fraction >= 1.0f);
}

bool FrameGenPresenter::SynthesizeJit(std::optional<JitFrame>& source, JitChoice& choice,
                                      const std::optional<JitFrame>& fallback, const JitChoice& fallback_choice,
                                      std::vector<XrCompositionLayerProjectionView>& views, JitWrite& write) {
    write = {};
    bool unmeasured = false; // a moved frame written as its real frame's exact copy
    double lock_wait_ms = 0.0;
    const auto exact_copy = [](JitChoice& unmoved) {
        unmoved.mode = FrameGenSynthesisMode::kInterpolate;
        unmoved.fraction = 1.0f;
        unmoved.cover = false;
    };
    // A frame the layer composed is shown as it is: nothing to write.
    if (source->as_is) {
        write.as_is = true;
        return false;
    }
    {
        // Lock order: GPU, presenter.
        const auto asked = Clock::now();
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock_wait_ms += Milliseconds(Clock::now() - asked);
        if (!gpu_) {
            return false;
        }
        // Interpolating, a frame between two real frames whose newer one's
        // motion is not measured yet gives way to the fallback (the older one
        // moved on past itself, measured already) rather than showing the
        // newer one unchanged before its time, which jumps ahead and then
        // holds (DCS and MSFS 2024, 2026-10-09: 70-140 such refreshes in 30 s).
        const bool cover_when_unmeasured = stale_motion_enabled_ && fallback && fallback->id != source->id &&
                                           !fallback->as_is &&
                                           choice.mode == FrameGenSynthesisMode::kInterpolate &&
                                           fallback_choice.mode == FrameGenSynthesisMode::kExtrapolate;
        bool motion_late = false;
        auto ready = [&](const JitFrame& frame, JitChoice& choice, bool& not_captured, bool& stale,
                         bool cover_instead = false) {
            stale = false;
            if (frame.as_is) {
                return true; // shown as the layer composed it
            }
            bool moved = !JitCopyOnly(frame, choice);
            // Newer captures took the ring slot of its previous images: it
            // can only be shown unchanged; of its own images too: not at all
            // (writing would fail and repeat the last frame anyway).
            if (moved && !gpu_->Writable(frame.analyzed, false)) {
                write.ring_exhausted = true;
                moved = false;
                exact_copy(choice);
            }
            if (!moved && !gpu_->Writable(frame.analyzed, true)) {
                write.ring_exhausted = true;
                return false;
            }
            if (moved && gpu_->Measured(frame.analyzed)) {
                return true;
            }
            if (!gpu_->Captured(frame.analyzed)) {
                not_captured = true;
                return false;
            }
            // Its motion is still being measured. Extrapolating, the previous
            // real frame's motion moves it instead, when that was measured
            // and is still kept: the refresh does not stand still and jump
            // the next (MSFS 2024 with NVIDIA's flow, 2026-10-09: motion came
            // 16 ms after the capture, and 8% of refreshes stood still).
            if (moved && stale_motion_enabled_ && choice.mode == FrameGenSynthesisMode::kExtrapolate &&
                frame.previous_analyzed && gpu_->Measured(frame.previous_analyzed) &&
                gpu_->Writable(frame.previous_analyzed, true)) {
                stale = true;
                return true;
            }
            if (moved && cover_instead) {
                motion_late = true;
                return false;
            }
            // Else its real frame unchanged.
            unmeasured = moved;
            exact_copy(choice);
            return true;
        };
        bool stale_source = false;
        if (!ready(*source, choice, write.uncaptured, stale_source, cover_when_unmeasured)) {
            JitChoice alternative = fallback_choice;
            bool fallback_uncaptured = false;
            bool stale_fallback = false;
            if (!fallback || !ready(*fallback, alternative, fallback_uncaptured, stale_fallback)) {
                std::scoped_lock lock(mutex_);
                ++repeated_;
                write.unmeasured = unmeasured;
                write.motion_late = motion_late;
                return false;
            }
            source = fallback;
            choice = alternative;
            stale_source = stale_fallback;
            if (source->as_is) {
                write.as_is = true;
                write.unmeasured = unmeasured;
                return false;
            }
        }
        write.unmeasured = unmeasured;
        write.stale = stale_source;
        write.motion_late = motion_late;
    }
    const JitFrame& frame = *source;
    write.exact = JitCopyOnly(frame, choice);
    if (!frame.copy.has_projection || frame.copy.projection_index >= frame.copy.layers.size()) {
        return false;
    }
    const FrameCopy::Layer& layer = frame.copy.layers[frame.copy.projection_index];
    if (layer.views.empty() || frame.kinds.size() != layer.views.size()) {
        return false;
    }
    // One output image per view; composing quad views, one per eye, in the
    // focus view's kind (the layer's compositor writes its format).
    const FrameGenQuadFrame* quad = frame.quad.get();
    std::vector<OutputSpec> specs;
    if (quad) {
        const FrameGenComposition& composition = quad->composition;
        if (composition.eyes.size() != quad->eyes.size()) {
            return false;
        }
        for (const FrameGenComposeEye& eye : composition.eyes) {
            if (eye.peripheral >= frame.kinds.size() || eye.focus >= frame.kinds.size()) {
                return false;
            }
            specs.push_back({composition.output, frame.kinds[eye.focus]});
        }
    } else {
        for (std::size_t i = 0; i < layer.views.size(); ++i) {
            specs.push_back({Extent(layer.views[i]), frame.kinds[i]});
        }
    }
    std::string error;
    // One set of output images, made on this thread (no other uses them).
    // Composing quad views, images made anew keep the old ones until a frame
    // is written into them: the last frame written still shows them.
    const bool ensured = EnsureOutputs(specs, 1, error, quad != nullptr);
    {
        std::scoped_lock lock(mutex_);
        const bool current = ensured && !output_sets_.empty() && jit_views_.size() == output_sets_.front().size() &&
                             std::equal(jit_views_.begin(), jit_views_.end(), output_sets_.front().begin(),
                                        [](const XrCompositionLayerProjectionView& view, const Output& output) {
                                            return view.subImage.swapchain == output.swapchain;
                                        });
        if (!current && !quad) {
            jit_views_.clear();
            jit_content_ = {};
        }
        if (!ensured) {
            ++totals_.unwritten_outputs;
            write.failed = true;
            write.error = error;
            // Composing quad views, failures hand the views back to the
            // layer for a while instead (CompositorLoop).
            if (!quad) NoteFailureLocked(error);
            return false;
        }
    }
    std::vector<FrameGenImage> images;
    for (Output& output : output_sets_.front()) {
        if (!output.acquired) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrResult acquired;
            {
                std::scoped_lock submit(submit_mutex_);
                const PresenterCall call(*this, "xrAcquireSwapchainImage");
                acquired = xr_.acquire(output.swapchain, &acquire, &output.index);
            }
            if (XR_FAILED(acquired)) {
                std::scoped_lock lock(mutex_);
                ++totals_.unwritten_failed;
                write.failed = true;
                write.error = "output image acquire failed (" + std::to_string(acquired) + ")";
                if (!quad) NoteFailureLocked(write.error);
                return false;
            }
            output.acquired = true;
            output.waited = false;
        }
        if (!output.waited) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = kOutputWaitTimeout;
            const XrResult waited = [&] {
                const PresenterCall call(*this, "xrWaitSwapchainImage");
                return xr_.wait(output.swapchain, &wait);
            }();
            if (XR_FAILED(waited) || waited == XR_TIMEOUT_EXPIRED) {
                // Kept acquired: waited for again next refresh.
                std::scoped_lock lock(mutex_);
                ++totals_.unwritten_wait;
                ++repeated_;
                write.failed = true;
                write.timed_out = waited == XR_TIMEOUT_EXPIRED;
                write.error = write.timed_out ? std::string("an output image was not handed back in time")
                                              : "output image wait failed (" + std::to_string(waited) + ")";
                return false;
            }
            output.waited = true;
        }
        images.push_back({output.swapchain, output.index, 0,
                          {{0, 0}, {static_cast<std::int32_t>(output.extent.width), static_cast<std::int32_t>(output.extent.height)}},
                          output.format});
    }
    bool ok = false;
    {
        const auto asked = Clock::now();
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock_wait_ms += Milliseconds(Clock::now() - asked);
        ok = gpu_ != nullptr;
        // A capture since its readiness above may have taken a ring slot it
        // needs (the game releases its next frame meanwhile): unmoved, or
        // not at all (the images stay acquired for the next refresh).
        if (ok && !write.exact && !gpu_->Writable(frame.analyzed, false)) {
            write.ring_exhausted = true;
            exact_copy(choice);
            write.exact = true;
        }
        if (ok && !gpu_->Writable(frame.analyzed, true)) {
            write.ring_exhausted = true;
            std::scoped_lock lock(mutex_);
            ++repeated_;
            return false;
        }
        for (const FrameGenImage& image : images) {
            ok = ok && gpu_->Track(image.swapchain, xr_.images, error);
        }
        if (ok && write.stale && !write.exact) {
            std::string stale_error;
            const bool stale_written =
                quad ? gpu_->SynthesizeComposedStale(frame.analyzed, frame.previous_analyzed, choice.fraction,
                                                     quad->composition, images, stale_error)
                     : gpu_->SynthesizeStale(frame.analyzed, frame.previous_analyzed, choice.fraction, images, stale_error);
            if (!stale_written) {
                // Refused (nothing written): its real frame unchanged after all.
                write.stale = false;
                write.unmeasured = unmeasured = true;
                exact_copy(choice);
                write.exact = true;
                ok = quad ? gpu_->SynthesizeComposed(frame.analyzed, choice.mode, choice.fraction, quad->composition,
                                                     images, error)
                          : gpu_->Synthesize(frame.analyzed, choice.mode, choice.fraction, images, error);
            }
        } else {
            write.stale = false;
            ok = ok && (quad ? gpu_->SynthesizeComposed(frame.analyzed, choice.mode, choice.fraction,
                                                        quad->composition, images, error)
                             : gpu_->Synthesize(frame.analyzed, choice.mode, choice.fraction, images, error));
        }
    }
    const bool extrapolate = choice.mode == FrameGenSynthesisMode::kExtrapolate;
    if (!ok) {
        // The images stay acquired and waited for the next refresh; the
        // runtime keeps the last ones released.
        std::scoped_lock lock(mutex_);
        AddSample(synth_lock_ms_, lock_wait_ms);
        ++totals_.unwritten_failed;
        ++repeated_;
        write.failed = true;
        write.error = error.empty() ? std::string("the GPU could not write the frame") : error;
        return false;
    }
    for (Output& output : output_sets_.front()) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        {
            std::scoped_lock submit(submit_mutex_);
            const PresenterCall call(*this, "xrReleaseSwapchainImage");
            xr_.release(output.swapchain, &release);
        }
        output.acquired = false;
        output.waited = false;
    }
    // The real frame's views, showing the images just written, seen from
    // where the eye is at the frame's time (where depth is used). Composed,
    // each eye's view as the layer composes it, moved as its periphery.
    // Frames moved by the previous real frame's motion too (their eye shift
    // reads its depth: FrameGenGpu::SynthesizeStale).
    const float eye_time = write.exact ? 0.0f : extrapolate ? choice.fraction : choice.fraction - 1.0f;
    if (quad) {
        views.assign(quad->eyes.begin(), quad->eyes.end());
    } else {
        views = layer.views;
    }
    for (std::size_t i = 0; i < views.size(); ++i) {
        const Output& output = output_sets_.front()[i];
        views[i].next = nullptr;
        views[i].subImage.swapchain = output.swapchain;
        views[i].subImage.imageArrayIndex = 0;
        views[i].subImage.imageRect = {{0, 0}, {static_cast<std::int32_t>(output.extent.width),
                                                static_cast<std::int32_t>(output.extent.height)}};
        const std::size_t moved_as = quad ? quad->composition.eyes[i].peripheral : i;
        if (eye_time != 0.0f && moved_as < frame.velocity.size()) {
            const std::array<float, 3>& velocity = frame.velocity[moved_as];
            const std::array<double, 3> local{eye_time * velocity[0], eye_time * velocity[1], eye_time * velocity[2]};
            const std::array<double, 3> world = Rotate(views[i].pose.orientation, local);
            views[i].pose.position.x += static_cast<float>(world[0]);
            views[i].pose.position.y += static_cast<float>(world[1]);
            views[i].pose.position.z += static_cast<float>(world[2]);
        }
    }
    // Output images replaced since the last frame written: nothing shows
    // them once this frame is submitted.
    if (quad && !retired_outputs_.empty()) {
        DestroyRetiredOutputs();
    }
    std::scoped_lock lock(mutex_);
    consecutive_failures_ = 0;
    AddSample(synth_lock_ms_, lock_wait_ms);
    totals_.unmeasured += unmeasured ? 1 : 0;
    totals_.stale_motion += write.stale || write.motion_late ? 1 : 0;
    if (logger_ && !logged_jit_ && !write.exact) {
        logged_jit_ = true;
        logger_->Info("Frame Generation writes each generated frame right before its refresh (" +
                      std::string(jit_interpolate_ ? "interpolating, with cover frames past the newest real frame when "
                                                     "the next one is late"
                                                   : "extrapolating, real frames shown as exact copies") +
                      (quad ? "): one output image per eye, composed from the four quad views, any number of frames "
                              "between real frames."
                            : "): one output image per eye, any number of frames between real frames."));
    }
    return true;
}

void FrameGenPresenter::CompositorLoop() {
    // A compositor-like thread: it must make every refresh even while the
    // game saturates the CPU.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"VectorXR Frame Generation presenter");
    compositor_thread_id_ = GetCurrentThreadId();
    std::unique_lock lock(mutex_);
    const FrameGenRuntimeCalls calls = compositor_calls_;
    const XrSession session = session_;
    void* const timer = compositor_timer_;
    std::vector<const XrCompositionLayerBaseHeader*> pointers;
    const auto ns = [](XrDuration duration) { return std::chrono::nanoseconds(duration); };
    const auto forever = Clock::time_point::max();
    // Draining, every frame in flight is presented, and just in time every
    // real frame kept until its own refresh (the frame thread waits for it):
    // the newest is shown before frames go straight to the runtime again.
    auto drained = [&] {
        return threaded_frames_.empty() &&
               (!jit_ || (!generation_pending_ &&
                          (jit_frames_.empty() ||
                           jit_frames_.back().time <= slot_time_ + std::max<XrDuration>(slot_period_, 1) / 2)));
    };
    while (!compositor_stop_ && !(compositor_drain_ && drained())) {
        // A runtime frame the application began before the switch owns the
        // runtime until it ends: no runtime wait while any frame is open (a
        // frame of this thread's left open, nothing current to show, is
        // discarded by its next begin).
        compositor_phase_ = "waiting for the game's open runtime frame";
        if (!CompositorWait(lock, Clock::now() + std::chrono::seconds(1), [&] {
                return compositor_stop_ ||
                       ((!RuntimeFrameOpenLocked() || presenter_frame_open_) && !RuntimeWaitPendingLocked());
            })) {
            if (RuntimeWaitPendingLocked()) {
                continue; // a waited frame was never begun: another wait would block in the runtime
            }
            if (logger_ && !logged_order_guard_) {
                logged_order_guard_ = true;
                logger_->Error("Frame Generation: a runtime frame begun before the presenter thread started never "
                               "ended; continuing past it.");
            }
        }
        if (compositor_stop_) {
            break;
        }
        frames_.clear(); // anything left never reached the runtime
        compositor_phase_ = "in the runtime's xrWaitFrame";
        lock.unlock();
        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const auto called = Clock::now();
        const XrResult waited = RuntimeWait(calls, session, &wait_info, &frame_state);
        const auto returned = Clock::now();
        lock.lock();
        if (XR_FAILED(waited)) {
            PauseLocked("runtime xrWaitFrame failed on the presenter thread (" + std::to_string(waited) + ")");
            break;
        }
        NotePhaseLocked(called, returned, frame_state.predictedDisplayTime, frame_state.predictedDisplayPeriod);
        const std::uint64_t slot = ++slot_index_;
        slot_time_ = frame_state.predictedDisplayTime;
        slot_period_ = frame_state.predictedDisplayPeriod;
        if (slot_period_ > 0) {
            last_period_ = slot_period_;
        }
        slot_should_render_ = frame_state.shouldRender == XR_TRUE;
        slot_cv_.notify_all();
        const XrDuration period = std::max<XrDuration>(slot_period_, 1);

        // Submit when the runtime would have released this wait, and most of
        // a refresh after the previous submission.
        Clock::time_point submit_at = returned;
        if (phase_wall_ != Clock::time_point{} && slot_time_ > phase_xr_) {
            submit_at = std::max(submit_at, phase_wall_ + ns(slot_time_ - phase_xr_));
        }
        if (last_runtime_submit_ != Clock::time_point{}) {
            submit_at = std::max(submit_at, last_runtime_submit_ + ns(period * 17 / 20));
        }
        submit_at = std::min(submit_at, returned + ns(period * 6 / 5));
        compositor_phase_ = "choosing a frame for the refresh";
        // The newest real frame cannot be repeated (a space or swapchain it
        // used was destroyed) and nothing generated from it is ready: take
        // the next real frame as soon as it ends instead.
        auto unusable = [&] {
            const bool jit_ready = jit_ && std::any_of(jit_frames_.begin(), jit_frames_.end(), [&](const JitFrame& frame) {
                                       return frame.id == newest_copy_id_;
                                   });
            return !generated_ready_ && !jit_ready &&
                   (!newest_copy_.valid || (newest_copy_.has_projection && newest_copy_.projection_stale));
        };
        auto eligible = [&] { return slot_request_ && (slot_request_->earliest_slot <= slot || unusable()); };
        auto real_due = [&] {
            return std::any_of(threaded_frames_.begin(), threaded_frames_.end(),
                               [&](const ThreadedFrame& frame) { return frame.first_slot <= slot; });
        };
        if (jit_) {
            // Just in time every refresh is this thread's: the next real
            // frame, when this refresh needs it and it is on its way, gets
            // until the refresh is due. Later, a cover frame shows this
            // refresh's time as well (waiting longer would push the next
            // refreshes late too).
            auto due = [&] {
                bool needed = false;
                ChooseJitLocked(slot_time_, period, jit_interpolate_, needed);
                return needed;
            };
            if (due()) {
                CompositorWait(lock, submit_at, [&] { return compositor_stop_ || !due(); });
            }
        } else {
            CompositorWait(lock, submit_at, [&] { return compositor_stop_ || eligible(); });
            if (!compositor_stop_ && !eligible() && (real_due() || generation_pending_)) {
                // A real frame due in this slot, or the frames being generated
                // from the newest one, get half a refresh more.
                CompositorWait(lock, submit_at + ns(period / 2), [&] {
                    return compositor_stop_ || eligible() || (!real_due() && !generation_pending_);
                });
            }
            if (!compositor_stop_ && !eligible() && unusable()) {
                // The runtime keeps showing the last frame meanwhile.
                CompositorWait(lock, submit_at + std::chrono::milliseconds(100),
                               [&] { return compositor_stop_ || eligible(); });
            }
        }
        if (eligible()) {
            // Real frames keep the spacing generated ones get. The request
            // stays: only this thread or a stop after it clears it.
            if (Clock::now() < submit_at) {
                lock.unlock();
                SleepUntil(timer, submit_at);
                lock.lock();
            }
            slot_offer_ = SlotOffer{slot_request_->frame_id, slot, slot_time_, slot_should_render_};
            slot_request_.reset();
            offer_done_ = false;
            compositor_phase_ = "handing a refresh to the game's xrEndFrame";
            WakeSlots();
            CompositorWait(lock, forever, [&] { return offer_done_; });
            slot_offer_.reset();
            continue;
        }

        if (jit_) {
            // Just in time: the frame this refresh shows, written now.
            compositor_phase_ = "writing a generated frame";
            bool due = false;
            JitChoice choice = ChooseJitLocked(slot_time_, period, jit_interpolate_, due);
            if (choice.cover && due) {
                ++jit_covers_;
            }
            // A refresh the runtime does not render (the headset is off):
            // nothing is written or shown, and it says nothing about writing
            // or the lead (real frames it passes over are not scored).
            const bool rendered = slot_should_render_;
            // Real frames a later refresh needs whose game frames are on the
            // GPU already: their lead had a refresh to spare (ScoreLeadLocked).
            for (JitFrame& kept : jit_frames_) {
                if (!rendered) {
                    kept.scored = kept.scored || kept.time <= slot_time_ + period / 2;
                } else if (!kept.scored && !kept.captured_early && kept.needed > slot_time_ + period / 2) {
                    kept.captured_early = !gpu_ || !kept.capture_mark || gpu_->CaptureReached(kept.capture_mark);
                }
            }
            std::optional<JitFrame> source;
            std::optional<JitFrame> fallback;
            JitChoice fallback_choice;
            std::optional<std::uint64_t> chosen_id;
            bool skip = false;
            if (choice.index && rendered) {
                const JitFrame& chosen = jit_frames_[*choice.index];
                chosen_id = chosen.id;
                // The outputs already hold this real frame's exact copy:
                // nothing is written, the runtime shows their newest images
                // again (a game showing its last frame costs no GPU work).
                skip = !chosen.as_is && jit_exact_ && JitCopyOnly(chosen, choice) && jit_shown_ &&
                       std::get<0>(*jit_shown_) == chosen.id && !jit_views_.empty() && !output_sets_.empty() &&
                       jit_views_.size() == output_sets_.front().size() &&
                       std::equal(jit_views_.begin(), jit_views_.end(), output_sets_.front().begin(),
                                  [](const XrCompositionLayerProjectionView& view, const Output& output) {
                                      return view.subImage.swapchain == output.swapchain;
                                  });
                if (!skip) {
                    source = chosen;
                    // As if the chosen real frame had not arrived yet: for
                    // when its game frame is still on the GPU.
                    bool unused = false;
                    fallback_choice = ChooseJitLocked(slot_time_, period, jit_interpolate_, unused, *choice.index);
                    if (fallback_choice.index && !chosen.as_is) {
                        fallback = jit_frames_[*fallback_choice.index];
                    }
                }
            }
            if (!rendered) {
                ++totals_.unrendered;
            } else if (skip) {
                ++totals_.copies_skipped;
            } else if (!choice.index) {
                ++totals_.unwritten_source;
            }
            lock.unlock();
            std::vector<XrCompositionLayerProjectionView> written_views;
            JitWrite write;
            const bool written = source && SynthesizeJit(source, choice, fallback, fallback_choice, written_views, write);
            SleepUntil(timer, submit_at);
            std::unique_lock submit_lock(submit_mutex_);
            lock.lock();
            totals_.unready += write.uncaptured ? 1 : 0;
            totals_.ring_exhausted += write.ring_exhausted ? 1 : 0;
            if (chosen_id) {
                // Each real frame's lead sample, when a refresh first chooses
                // it: late if its game frame was not on the GPU yet, its
                // motion not measured though the refresh shows it moved
                // (interpolating, the refresh before its own), or the refresh
                // it was needed for has passed. Older frames never chosen
                // were passed over: late.
                for (JitFrame& kept : jit_frames_) {
                    if (kept.scored || kept.id > *chosen_id) {
                        continue;
                    }
                    if (kept.id < *chosen_id) {
                        ScoreLeadLocked(kept, true);
                        continue;
                    }
                    const bool late = write.uncaptured || write.unmeasured || write.motion_late ||
                                      slot_time_ > kept.needed + period / 2;
                    totals_.late_gpu += late && write.uncaptured ? 1 : 0;
                    ScoreLeadLocked(kept, late);
                }
            }
            // Composing quad views, attempted writes that keep failing hand
            // the views back to the layer for a while (Frame Generation stands
            // by meanwhile); a refresh the runtime does not render (nothing is
            // written then) and a frame not on the GPU yet say nothing about
            // writing.
            const auto now = Clock::now();
            if (quad_mode_ && source && !write.as_is && now >= quad_declined_until_) {
                if (written) {
                    quad_unwritten_in_row_ = 0;
                } else if (write.failed) {
                    ++quad_unwritten_in_row_;
                    if (logger_ && quad_unwritten_in_row_ == 1 &&
                        logged_quad_write_failures_ < kLoggedQuadWriteFailures) {
                        ++logged_quad_write_failures_;
                        logger_->Error("Frame Generation could not write a composed frame (" + write.error +
                                       "); the last frame is shown again.");
                    }
                    if (quad_unwritten_in_row_ >= kQuadUnwrittenBeforeDecline) {
                        DeclineQuadLocked(now, write.error);
                    }
                }
            }
            FrameCopy content;
            bool distinct = false;
            bool generated = false;
            bool submit = true; // false: nothing current to show, the runtime shows its last frame again
            // The real frame's layers as they are now: a swapchain or space
            // the game destroyed while the frame was written is gone from
            // them (ForgetSwapchain, ForgetSpace; both hold submit_mutex_).
            const auto find_kept = [&](std::uint64_t id) {
                return std::find_if(jit_frames_.begin(), jit_frames_.end(),
                                    [&](const JitFrame& frame) { return frame.id == id; });
            };
            // A frame shown as it is (nothing of Frame Generation's; the
            // output images keep the last frame written): its copy while its
            // images are still the newest in their swapchains, else a newer
            // frame the layer composed, early, else nothing. What is shown is
            // what a refresh nothing is written for shows again.
            const auto show_as_is = [&](FrameCopy copy, std::uint64_t id, XrTime made_for) {
                const AsIs current = CurrentAsIsLocked(copy, id, made_for);
                if (!current.copy) {
                    submit = false;
                    ++totals_.stale_skipped;
                    return;
                }
                content = *current.copy;
                jit_content_ = content;
                jit_views_.clear();
                jit_exact_ = false;
                jit_shown_as_is_ = true;
                jit_content_id_ = current.id;
                jit_content_time_ = current.time;
                const std::tuple<std::uint64_t, int, float> shown{current.id, -2, 0.0f};
                distinct = jit_shown_ != shown;
                jit_shown_ = shown;
            };
            const auto as_is = write.as_is ? find_kept(source->id) : jit_frames_.end();
            const auto kept = written ? find_kept(source->id) : jit_frames_.end();
            int path = 0; // the trace's "pick": which of the branches below showed the refresh
            if (!rendered) {
                // Not shown: no layers (below), and the last frame submitted
                // stays what a refresh nothing is written for repeats.
                path = 1;
            } else if (as_is != jit_frames_.end()) {
                // A frame the layer composed.
                path = 2;
                show_as_is(as_is->copy, as_is->id, as_is->time);
            } else if (kept != jit_frames_.end()) {
                path = 3;
                content = kept->copy;
                jit_started_ = true;
                jit_content_ = content;
                jit_views_ = std::move(written_views);
                jit_exact_ = write.exact;
                jit_shown_as_is_ = false;
                const std::tuple<std::uint64_t, int, float> shown{source->id, static_cast<int>(choice.mode),
                                                                  write.exact ? 1.0f : choice.fraction};
                distinct = jit_shown_ != shown;
                jit_shown_ = shown;
                generated = true;
            } else if (written && jit_content_.valid && jit_content_.has_projection &&
                       jit_content_.layers[jit_content_.projection_index].projection.space ==
                           source->copy.layers[source->copy.projection_index].projection.space) {
                // Written, but its real frame was dropped meanwhile: the
                // image just released goes with its own views and the last
                // layers shown around it.
                path = 4;
                content = jit_content_;
                jit_views_ = std::move(written_views);
                jit_exact_ = false;
                jit_shown_as_is_ = false;
                generated = true;
            } else if (jit_shown_as_is_ && jit_content_.valid && !written) {
                // Nothing written: the frame last shown as it is, again (a
                // frame written before it would step back).
                path = 5;
                show_as_is(jit_content_, jit_content_id_, jit_content_time_);
            } else if (jit_content_.valid && !jit_views_.empty() && !written) {
                // The last frame written again: the output images' newest.
                path = 6;
                content = jit_content_;
                generated = true;
            } else if (!quad_mode_ || newest_copy_fresh_ || !jit_started_) {
                // Before any frame was written: the frame passed through last.
                // After that (its outputs remade, or its space destroyed) the
                // newest real frame as it is, rather than nothing: showing the
                // game's images, the refresh waits on the runtime's queue for
                // the game's work on them (SubmitRuntimeFrame), and the game
                // waits for them again (FrameGenQueueSubstitution::Flush).
                // Composing quad views, claimed frames never become the newest
                // copy: the frame passed through before the start is shown only
                // until anything else was, then the newest the layer composed.
                path = 7;
                const bool passed_through = quad_mode_ ? !newest_copy_fresh_ : !jit_started_;
                const FrameCopy& fallback_copy = quad_mode_ || jit_started_ ? newest_copy_ : passthrough_copy_;
                const std::uint64_t id = passed_through ? 0 : newest_copy_id_;
                const auto newest = passed_through ? jit_frames_.end() : find_kept(id);
                show_as_is(fallback_copy, id, newest != jit_frames_.end() ? newest->time : 0);
            } else {
                // Composing quad views, with nothing the layer composed since
                // the last frame written: that frame again, as the runtime
                // shows it.
                path = 8;
                submit = false;
                ++totals_.stale_skipped;
            }
            DropStaleDepthLocked(content);
            const bool usable = content.valid && content.has_projection && content.projection_index < content.layers.size();
            const XrSpace space = usable ? content.layers[content.projection_index].projection.space : XR_NULL_HANDLE;
            const XrCompositionLayerFlags flags =
                usable ? content.layers[content.projection_index].projection.layerFlags : 0;
            std::vector<XrCompositionLayerProjectionView> views = jit_views_;
            // Never the game's depth: its images' releases may wait for its
            // GPU work (FrameGenQueueSubstitution), and the frame shows an
            // image of Frame Generation's own.
            const bool depth = false;
            const XrTime time = slot_time_;
            if (trace_.Enabled()) {
                const std::uint64_t shown_id = jit_shown_ ? std::get<0>(*jit_shown_) : 0;
                const int shown_mode = jit_shown_ ? std::get<1>(*jit_shown_) : -3;
                const float shown_fraction = jit_shown_ ? std::get<2>(*jit_shown_) : 0.0f;
                const auto shown_frame = find_kept(shown_id);
                const auto bit = [](bool value, int position) { return value ? (std::int64_t{1} << position) : 0; };
                const std::int64_t flags =
                    bit(rendered, 0) | bit(skip, 1) | bit(written, 2) | bit(write.exact, 3) | bit(write.uncaptured, 4) |
                    bit(write.unmeasured, 5) | bit(write.stale, 6) | bit(write.motion_late, 7) |
                    bit(write.ring_exhausted, 8) | bit(write.as_is, 9) | bit(write.failed, 10) | bit(choice.cover, 11) |
                    bit(submit, 12) | bit(distinct, 13) | bit(generated, 14) | bit(choice.index.has_value(), 15) |
                    (static_cast<std::int64_t>(path) << 16) | (static_cast<std::int64_t>(shown_mode + 3) << 20);
                trace_.Record("pick", shown_id, time, shown_frame != jit_frames_.end() ? shown_frame->time : 0, flags,
                              static_cast<std::int64_t>(std::llround(shown_fraction * 1e6)),
                              shown_frame != jit_frames_.end() ? shown_frame->previous_time : 0,
                              static_cast<std::int64_t>(chosen_id.value_or(0)));
            }
            lock.unlock();
            XrFrameEndInfo end_info =
                BuildFrame(content, generated && usable ? &views : nullptr, space, flags, depth, pointers);
            end_info.displayTime = time;
            if (!rendered || !content.valid) {
                end_info.layerCount = 0;
                end_info.layers = nullptr;
                distinct = false;
            }
            XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
            compositor_phase_ = "submitting a generated frame";
            // Nothing current to show: the frame is begun, not ended, so the
            // runtime shows its last frame again (the next begin discards it).
            XrResult result = RuntimeBegin(calls, session, &begin_info);
            if (XR_SUCCEEDED(result) && submit) {
                result = RuntimeEnd(calls, session, &end_info);
            }
            submit_lock.unlock();
            lock.lock();
            if (!submit) {
                presenter_frame_open_ = XR_SUCCEEDED(result);
            } else {
                // Generated frames count once, when first shown.
                const bool moved = distinct && written && !write.exact;
                generated_ += moved ? 1 : 0;
                NoteSubmissionLocked(distinct, moved);
            }
            if (XR_FAILED(result)) {
                PauseLocked("runtime rejected a frame from the presenter thread (" + std::to_string(result) + ")");
                break;
            }
            continue;
        }

        // Fill the slot: the next frame generated from the newest real frame,
        // or that real frame again.
        lock.unlock();
        SleepUntil(timer, submit_at);
        std::unique_lock submit_lock(submit_mutex_);
        lock.lock();
        FrameCopy content = newest_copy_;
        DropStaleDepthLocked(content);
        std::vector<XrCompositionLayerProjectionView> views;
        bool views_exact = false;
        bool distinct = false;
        const bool use_generated =
            content.valid && content.has_projection && generated_ready_ && extrapolated_.valid &&
            !extrapolated_.sets.empty() &&
            extrapolated_.sets.front().size() == content.layers[content.projection_index].views.size();
        if (use_generated) {
            // The frame made for this refresh: frames made for refreshes
            // already past (a skipped refresh, or frames ready late) are
            // passed over, and the last one stays after its own (the next
            // real frame is late; interpolating, it is the real frame's copy).
            std::size_t index = std::min(next_generated_set_, extrapolated_.sets.size() - 1);
            while (index + 1 < extrapolated_.sets.size() && index < extrapolated_.targets.size() &&
                   extrapolated_.targets[index] + period / 2 < slot_time_) {
                ++index;
                ++skipped_generated_;
            }
            views = extrapolated_.sets[index];
            views_exact = index < extrapolated_.exact.size() && extrapolated_.exact[index];
            next_generated_set_ = index + 1;
            // The last set shown again (the next real frame is late) is a
            // repeat, as is the real frame shown again.
            const std::pair<std::uint64_t, std::size_t> shown{extrapolated_.generation, index};
            distinct = last_shown_set_ != shown;
            last_shown_set_ = shown;
        }
        const XrSpace space = extrapolated_.space;
        const XrCompositionLayerFlags flags = extrapolated_.flags;
        const XrTime time = slot_time_;
        const bool should_render = slot_should_render_;
        lock.unlock();
        XrFrameEndInfo end_info = BuildFrame(content, use_generated ? &views : nullptr, space, flags, views_exact, pointers);
        end_info.displayTime = time;
        if (!should_render || !content.valid) {
            end_info.layerCount = 0;
            end_info.layers = nullptr;
            distinct = false;
        }
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        compositor_phase_ = "submitting a generated frame";
        XrResult result = RuntimeBegin(calls, session, &begin_info);
        if (XR_SUCCEEDED(result)) {
            result = RuntimeEnd(calls, session, &end_info);
        }
        submit_lock.unlock();
        lock.lock();
        NoteSubmissionLocked(distinct, use_generated && !views_exact);
        if (XR_FAILED(result)) {
            PauseLocked("runtime rejected a frame from the presenter thread (" + std::to_string(result) + ")");
            break;
        }
    }
    if (jit_) {
        // Output images acquired for a frame that was never written go back,
        // and ones replaced while composing quad views go: nothing shows them
        // once the run that wrote them has ended (they would stay through
        // standing by otherwise, at headset resolution).
        lock.unlock();
        ReleaseOutputs();
        DestroyRetiredOutputs();
        lock.lock();
    }
    compositor_thread_id_ = 0;
    compositor_running_ = false;
    compositor_phase_ = "stopped";
    slot_request_.reset();
    WakeSlots();
    idle_cv_.notify_all();
}

bool FrameGenPresenter::ShowsOwnImagesOnly() const {
    std::scoped_lock lock(mutex_);
    return compositor_running_ && jit_ && jit_started_;
}

bool FrameGenPresenter::CapturesFrom(XrSwapchain swapchain) const {
    std::scoped_lock lock(mutex_);
    return std::any_of(capture_targets_.begin(), capture_targets_.end(),
                       [&](const CaptureTarget& target) { return target.swapchain == swapchain; });
}

FrameGenStatus FrameGenPresenter::Status() const {
    std::scoped_lock lock(mutex_);
    FrameGenStatus status;
    status.state = state_;
    status.reason = reason_;
    status.mode = settings_.synthesis_mode;
    const auto now = Clock::now();
    status.app_fps = RatePerSecond(app_frames_, now);
    status.output_fps = RatePerSecond(submissions_, now);
    status.distinct_fps = RatePerSecond(distinct_submissions_, now);
    status.gpu_ms = gpu_ms_;
    status.generated = generated_;
    status.repeated = repeated_;
    status.failures = failures_;
    status.demoted = demoted_;
    status.captured = captured_;
    status.retry_pending = state_ == FrameGenState::kPaused && automatic_retries_ < retry_attempts_;
    status.multiplier = EffectiveMultiplierLocked();
    status.dynamic = DynamicLocked();
    status.automatic = settings_.multiplier == FrameGenMultiplier::kAuto ||
                       (settings_.multiplier == FrameGenMultiplier::kDynamic && !status.dynamic);
    status.full_rate = full_rate_ && state_ == FrameGenState::kActive;
    status.standby = status.dynamic && dynamic_standby_;
    status.dynamic_reason = dynamic_reason_;
    status.dynamic_engagements = dynamic_engagements_;
    status.dynamic_stand_downs = dynamic_stand_downs_;
    status.dynamic_probes = dynamic_probes_;
    status.threaded = compositor_running_;
    status.lead_slots = lead_slots_;
    status.lead_floor = lead_floor_;
    status.lead_ups = totals_.lead_ups;
    status.lead_downs = totals_.lead_downs;
    status.lead_probes = totals_.lead_probes;
    status.lead_probes_failed = totals_.lead_probes_failed;
    status.lead_downs_undone = totals_.lead_downs_undone;
    status.late_cpu = totals_.late_cpu;
    status.late_gpu = totals_.late_gpu;
    status.gpu_gating = gpu_gate_enabled_ && gpu_gate_armed_;
    status.gpu_waits = totals_.gpu_waits;
    status.gpu_wait_timeouts = totals_.gpu_wait_timeouts;
    status.pauses = totals_.pauses;
    status.unready_capture = totals_.unready;
    status.unmeasured_copies = totals_.unmeasured;
    status.stale_motion = totals_.stale_motion;
    status.copies_skipped = totals_.copies_skipped;
    status.unwritten = totals_.unwritten_wait + totals_.unwritten_failed + totals_.unwritten_outputs +
                       totals_.unwritten_source;
    status.ring_exhausted = totals_.ring_exhausted;
    status.stale_skipped = totals_.stale_skipped;
    status.analyses_timed = totals_.analyses_timed;
    status.analyses_late = totals_.analyses_late;
    status.quad = quad_mode_ && compositor_running_;
    status.quad_claimed = totals_.quad_claimed;
    status.quad_declined =
        totals_.quad_uncaptured + totals_.quad_busy + totals_.quad_declined + totals_.quad_not_presenting;
    status.quad_setups = totals_.quad_setups;
    status.quad_unanalyzed = totals_.quad_unanalyzed;
    status.quad_write_declines = quad_write_declines_;
    status.quad_declining = now < quad_declined_until_;
    if (depth_seen_) {
        const FrameGenDepthEvidence& evidence = depth_status_.evidence;
        status.depth = !depth_sent_ ? FrameGenDepthState::kNotSent
                       : !depth_readable_ ? FrameGenDepthState::kNotReadable
                       : depth_status_.reading == FrameGenDepthReading::kUnknown ? FrameGenDepthState::kChecking
                       : depth_status_.reading == FrameGenDepthReading::kUnusable
                           ? (evidence.binocular > 0.0 && evidence.cockpit < 0.005 * evidence.binocular
                                  ? FrameGenDepthState::kEmpty
                                  : FrameGenDepthState::kUnusable)
                           : FrameGenDepthState::kInUse;
    }
    return status;
}

FrameGenOsdText DescribeFrameGen(const FrameGenStatus& status, bool started) {
    // The expanded OSD row says the state and how it runs; the line under it
    // what it does right now, or why not. The OSD separates parts as its other
    // rows do, the app with a middle dot.
    constexpr const char* kOsd = "  /  ";
    constexpr const char* kApp = " \xC2\xB7 ";
    constexpr const char* kArrow = " \xE2\x86\x92 ";
    // The OSD's second line starts as a sentence; reasons start lowercase.
    const auto sentence = [](std::string line) {
        if (!line.empty() && line[0] >= 'a' && line[0] <= 'z') line[0] = static_cast<char>(line[0] - 'a' + 'A');
        return line;
    };
    FrameGenOsdText text;
    text.state = ToString(status.state);
    switch (status.state) {
    case FrameGenState::kActive: {
        text.badge = "STBY";
        text.state = "standby";
        // Composed quad views frames kept failing to be written: frames pass
        // straight through for a while, whatever else the state says.
        if (status.quad_declining || (status.full_rate && !status.reason.empty())) {
            text.row = "Standby";
            text.detail = "Generated frames could not be shown, retrying soon";
            text.compact = std::string("Standby") + kOsd + "retrying soon";
            text.reason = text.detail;
            return text;
        }
        // Standing by: the game holds the refresh rate on its own, so its
        // frames pass straight through.
        if (status.standby || status.full_rate) {
            const std::string rate = status.dynamic ? "Dynamic" : "Auto";
            const std::string game = "game at " + Number(status.app_fps, 0) + " fps";
            text.row = std::string("Standby") + kOsd + rate;
            text.detail = "The game holds " + Number(status.app_fps, 0) + " fps on its own";
            text.compact = std::string("Standby") + kOsd + game;
            text.reason = rate + kApp + game;
            return text;
        }
        // Distinct frames: a frame shown twice (generation or the game ran
        // late) is not counted again.
        const std::string mode = status.mode == FrameGenSynthesisMode::kExtrapolate ? "Extrapolate" : "Interpolate";
        const std::string rate =
            status.dynamic ? std::string("Dynamic") : (status.automatic ? "Auto " : "") + MultiplierText(status.multiplier);
        const std::string rates = Number(status.app_fps, 0) + kArrow + Number(status.distinct_fps, 0) + " fps";
        const std::string gpu = status.gpu_ms > 0.0 ? "GPU " + Number(status.gpu_ms, 1) + " ms" : std::string();
        text.badge = "ON";
        text.state = "active";
        text.shown_fps = status.distinct_fps;
        text.row = std::string("On") + kOsd + mode + kOsd + rate;
        text.detail = rates + (gpu.empty() ? "" : kOsd + gpu);
        text.compact = rate + kOsd + rates;
        text.reason = mode + kApp + rate + kApp + rates + (gpu.empty() ? "" : kApp + gpu);
        return text;
    }
    case FrameGenState::kWaiting:
        text.badge = "WAIT";
        text.row = "Starting";
        text.detail = "Waiting for steady frames";
        text.compact = "Starting";
        text.reason = status.reason;
        return text;
    case FrameGenState::kPaused:
        text.badge = "PAUSED";
        text.row = status.retry_pending ? std::string("Paused") + kOsd + "retrying soon" : std::string("Paused");
        text.detail = sentence(status.reason);
        text.compact = "Paused";
        text.reason = status.reason;
        if (status.retry_pending) text.reason += (text.reason.empty() ? "" : kApp) + std::string("retrying soon");
        return text;
    case FrameGenState::kUnavailable:
        text.badge = "N/A";
        text.row = text.compact = "Unavailable";
        text.detail = sentence(status.reason);
        text.reason = status.reason;
        return text;
    default:
        text.row = text.compact = started ? "Off" : "Disabled";
        if (started) text.badge = "OFF";
        return text;
    }
}

FrameGenOsdText FrameGenPresenter::OsdDescription() const {
    return DescribeFrameGen(Status(), Started());
}

std::string FrameGenPresenter::OsdText() const {
    return OsdDescription().row;
}

std::string FrameGenPresenter::OsdDetailText() const {
    return OsdDescription().detail;
}

std::string FrameGenPresenter::CompactOsdText() const {
    return OsdDescription().compact;
}

} // namespace depthxr
