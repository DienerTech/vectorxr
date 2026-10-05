#include <windows.h>

#include "depthxr/framegen_presenter.h"

#include "depthxr/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace depthxr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kOverlapWait{250};
constexpr std::chrono::seconds kClaimedFrameWait{5};
constexpr XrDuration kOutputWaitTimeout = 100'000'000; // 100 ms
constexpr std::uint32_t kFailuresBeforePause = 3;
constexpr std::uint32_t kDemotionsBeforePause = 3;
constexpr std::uint32_t kMaxMultiplier = 4;
constexpr std::chrono::seconds kPhaseLifetime{2};
constexpr int kAutoMaxBackoffDoublings = 3; // 10 s, 20 s, 40 s, 80 s

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
    geometry.angle_left = view.fov.angleLeft;
    geometry.angle_right = view.fov.angleRight;
    geometry.angle_up = view.fov.angleUp;
    geometry.angle_down = view.fov.angleDown;
    return geometry;
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
    if (timer_) {
        CloseHandle(static_cast<HANDLE>(timer_));
    }
}

void FrameGenPresenter::Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
                              Logger* logger, std::mutex* queue_mutex, GpuFactory factory) {
    Stop();
    std::scoped_lock lock(mutex_);
    session_ = session;
    xr_ = dispatch;
    logger_ = logger;
    queue_mutex_ = queue_mutex;
    started_ = true;
    user_enabled_ = true;
    started_at_ = Clock::now();
    healthy_streak_ = 0;
    automatic_retries_ = 0;
    paused_at_.reset();
    state_ = configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff;
    reason_.clear();
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
                      "; frame pacing is owned by Frame Generation (Turbo stays off).");
    }
}

void FrameGenPresenter::Stop() {
    std::unique_lock lock(mutex_);
    if (!started_) {
        return;
    }
    // From here no release thread captures into the backend.
    started_ = false;
    // Finish outstanding GPU work before the runtime frees the output images
    // it writes. Teardown runs between frames, so no EndFrame touches the
    // outputs concurrently.
    if (gpu_) {
        gpu_->Reset();
    }
    lock.unlock();
    DestroyOutputs();
    lock.lock();
    gpu_.reset();
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
    auto_multiplier_ = 2;
    full_rate_ = false;
    cadence_.clear();
    last_app_end_.reset();
    multiplier_changed_at_ = {};
    probe_not_before_ = {};
    probe_backoff_ = auto_initial_backoff_;
    probing_ = false;
    runtime_formats_.clear();
    frames_.clear();
    capture_targets_.clear();
    last_real_ = {};
    extrapolated_ = {};
    consecutive_failures_ = consecutive_demotions_ = 0;
    generated_ = repeated_ = failures_ = demoted_ = captured_ = 0;
    app_frames_.clear();
    submissions_.clear();
    logged_first_generation_ = logged_capture_failure_ = false;
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

void FrameGenPresenter::Configure(const FrameGenResolvedSettings& settings) {
    std::scoped_lock lock(mutex_);
    settings_ = settings;
}

void FrameGenPresenter::SetConfigured(bool enabled) {
    std::scoped_lock lock(mutex_);
    if (configured_enabled_ == enabled) {
        return;
    }
    configured_enabled_ = enabled;
    healthy_streak_ = 0;
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

void FrameGenPresenter::SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age) {
    std::scoped_lock lock(mutex_);
    engage_streak_frames_ = steady_frames;
    engage_min_age_ = minimum_session_age;
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
        SetStateLocked(configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
    } else {
        user_enabled_ = !user_enabled_;
        if (state_ != FrameGenState::kUnavailable) {
            SetStateLocked(user_enabled_ && configured_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
        }
    }
    healthy_streak_ = 0;
    return user_enabled_;
}

void FrameGenPresenter::ResetFrames() {
    std::scoped_lock lock(mutex_);
    frames_.clear();
    healthy_streak_ = 0;
    InvalidateContinuityLocked();
    if (state_ == FrameGenState::kActive) {
        SetStateLocked(FrameGenState::kWaiting, "");
    }
    idle_cv_.notify_all();
}

void FrameGenPresenter::AbandonFrame(XrTime display_time) {
    {
        std::scoped_lock lock(mutex_);
        frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                     [&](const PendingFrame& frame) { return frame.app_time == display_time; }),
                      frames_.end());
        healthy_streak_ = 0;
        InvalidateContinuityLocked();
    }
    idle_cv_.notify_all();
}

XrSwapchainUsageFlags FrameGenPresenter::SourceUsage(const XrSwapchainCreateInfo& info) const {
    std::scoped_lock lock(mutex_);
    if (!started_ || !gpu_ || (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        return 0;
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
    swapchains_[swapchain] = {format, std::max(1u, sample_count), (usage & required) == required, std::nullopt};
}

void FrameGenPresenter::ForgetSwapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(mutex_);
    swapchains_.erase(swapchain);
    if (gpu_) {
        // A recent submission may still read or write this swapchain's
        // images; destroys are rare, so always let it finish first.
        gpu_->WaitIdle();
    }
    capture_targets_.erase(std::remove_if(capture_targets_.begin(), capture_targets_.end(),
                                          [&](const CaptureTarget& target) { return target.swapchain == swapchain; }),
                           capture_targets_.end());
    if (gpu_) {
        gpu_->Forget(swapchain);
    }
}

void FrameGenPresenter::NoteRelease(XrSwapchain swapchain, std::uint32_t image_index) {
    std::scoped_lock lock(mutex_);
    if (const auto it = swapchains_.find(swapchain); it != swapchains_.end()) {
        it->second.released = image_index;
    }
}

std::optional<FrameGenOutputKind> FrameGenPresenter::OutputKindLocked(std::size_t view, std::int64_t source_format) const {
    const auto requested = gpu_ ? gpu_->Classify(source_format) : std::nullopt;
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
        const auto record = swapchains_.find(swapchain);
        return record != swapchains_.end() && record->second.sample_count == 1 && record->second.capturable &&
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
    // Lock order everywhere: queue, then presenter.
    std::unique_lock<std::mutex> queue_lock;
    if (queue_mutex_) {
        queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
    }
    std::scoped_lock lock(mutex_);
    if (!capturing()) {
        return;
    }
    const auto record = swapchains_.find(swapchain);
    for (const CaptureTarget& target : capture_targets_) {
        if (target.swapchain != swapchain) {
            continue;
        }
        const auto kind = OutputKindLocked(target.view, record->second.format);
        if (!kind) {
            continue;
        }
        FrameGenImage source{swapchain, image_index, target.array_index, target.rect, record->second.format};
        FrameGenViewRequest request;
        request.extent = {static_cast<std::uint32_t>(target.rect.extent.width),
                          static_cast<std::uint32_t>(target.rect.extent.height)};
        request.output = *kind;
        std::string error;
        if (!gpu_->Track(swapchain, xr_.images, error) ||
            !gpu_->Capture(target.view, source, request, settings_.flow_quality, error)) {
            // Generation then copies the image itself. An empty error means
            // the view is not set up for this region yet; nothing to report.
            if (logger_ && !error.empty() && !logged_capture_failure_) {
                logged_capture_failure_ = true;
                logger_->Error("Frame Generation could not capture a released image: " + error);
            }
            continue;
        }
        ++captured_;
    }
}

void FrameGenPresenter::RecordCaptureTargetsLocked(const XrCompositionLayerProjection& projection) {
    capture_targets_.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        const auto& sub = projection.views[i].subImage;
        capture_targets_.push_back({sub.swapchain, sub.imageArrayIndex, sub.imageRect, i});
    }
}

bool FrameGenPresenter::RunnableLocked() const {
    return started_ && gpu_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && healthy_streak_ >= engage_streak_frames_ &&
           Clock::now() - started_at_ >= engage_min_age_;
}

bool FrameGenPresenter::WantsEngagementLocked() const {
    return RunnableLocked() && EffectiveMultiplierLocked() >= 2;
}

std::uint32_t FrameGenPresenter::EffectiveMultiplierLocked() const {
    switch (settings_.multiplier) {
    case FrameGenMultiplier::k2:
        return 2;
    case FrameGenMultiplier::k3:
        return 3;
    case FrameGenMultiplier::k4:
        return 4;
    default:
        return auto_multiplier_;
    }
}

void FrameGenPresenter::UpdateAutoLocked(Clock::time_point now, const std::optional<PendingFrame>& frame) {
    const bool automatic = settings_.multiplier == FrameGenMultiplier::kAuto;
    const XrDuration period = frame && frame->period > 0 ? frame->period : last_period_;
    if (!automatic || period <= 0 || !RunnableLocked()) {
        last_app_end_ = now;
        cadence_.clear();
        return;
    }
    if (last_app_end_) {
        // An engaged frame should take N refreshes; a passed-through one, one.
        const double multiplier = frame && frame->engaged ? static_cast<double>(frame->multiplier) : 1.0;
        const auto expected = std::chrono::nanoseconds(static_cast<std::int64_t>(period * multiplier * 1.25));
        cadence_.emplace_back(now, now - *last_app_end_ > expected);
    }
    last_app_end_ = now;
    while (!cadence_.empty() && (now - cadence_.front().first > std::chrono::seconds(6) || cadence_.size() > 4096)) {
        cadence_.pop_front();
    }
    if (now - multiplier_changed_at_ < auto_settle_) {
        return;
    }
    std::size_t recent = 0, recent_late = 0, steady = 0, steady_late = 0;
    for (const auto& [time, late] : cadence_) {
        if (now - time <= std::chrono::seconds(1)) {
            ++recent;
            recent_late += late ? 1 : 0;
        }
        if (now - time <= auto_steady_) {
            ++steady;
            steady_late += late ? 1 : 0;
        }
    }
    const std::uint32_t before = auto_multiplier_;
    // More than 30% of the last second's frames missed the cadence: step up.
    if (recent >= 8 && recent_late * 10 > recent * 3) {
        auto_late_percent_ = recent_late * 100 / recent;
        if (probing_) {
            // The lower multiplier could not keep up: go back and wait longer
            // before probing again.
            auto_multiplier_ = probe_from_;
            probing_ = false;
            probe_backoff_ = std::min(probe_backoff_ * 2, auto_initial_backoff_ * (1 << kAutoMaxBackoffDoublings));
        } else if (auto_multiplier_ < kMaxMultiplier) {
            ++auto_multiplier_;
        }
        probe_not_before_ = now + probe_backoff_;
    } else if (probing_ && now - multiplier_changed_at_ >= auto_probe_confirm_) {
        probing_ = false;
        probe_backoff_ = auto_initial_backoff_;
        probe_not_before_ = now + probe_backoff_;
    } else if (!probing_ && auto_multiplier_ > 1 && now >= probe_not_before_ && !cadence_.empty() &&
               now - cadence_.front().first >= auto_steady_ && steady >= 8 && steady_late * 50 <= steady) {
        // Five steady seconds with at most 2% of frames late: try one step down.
        probe_from_ = auto_multiplier_;
        --auto_multiplier_;
        probing_ = true;
    }
    if (auto_multiplier_ != before) {
        multiplier_changed_at_ = now;
        cadence_.clear();
        if (logger_) {
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

void FrameGenPresenter::MaybeRetryLocked() {
    if (state_ != FrameGenState::kPaused || !paused_at_ || automatic_retries_ >= retry_attempts_ ||
        Clock::now() - *paused_at_ < retry_delay_) {
        return;
    }
    ++automatic_retries_;
    paused_at_.reset();
    consecutive_failures_ = consecutive_demotions_ = 0;
    healthy_streak_ = 0;
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
    if (state == FrameGenState::kActive && full_rate_) {
        logger_->Info("Frame Generation standing by: the application holds full refresh.");
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
        // doing its job, so it reads as active.
        const bool full_rate = !demoted && RunnableLocked() && multiplier < 2;
        if (full_rate != full_rate_) {
            full_rate_ = full_rate;
            if (state_ == FrameGenState::kActive) state_ = FrameGenState::kWaiting; // re-log the transition
        }
        if (full_rate) {
            SetStateLocked(FrameGenState::kActive, "");
        } else if (!engage && state_ == FrameGenState::kActive) {
            SetStateLocked(FrameGenState::kWaiting, "");
        }
        sequence = ++wait_sequence_;
    }
    const auto called = Clock::now();
    const XrResult result = calls.wait(session, info, state);
    if (XR_FAILED(result) || !state) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    NotePhaseLocked(called, Clock::now(), state->predictedDisplayTime, state->predictedDisplayPeriod);
    PendingFrame frame;
    frame.slot_time = state->predictedDisplayTime;
    frame.period = state->predictedDisplayPeriod;
    frame.should_render = state->shouldRender == XR_TRUE;
    frame.engaged = engage && frame.period > 0 && state_ != FrameGenState::kPaused;
    frame.multiplier = multiplier;
    frame.sequence = sequence;
    frame.wait_return = Clock::now();
    if (frame.period > 0) last_period_ = frame.period;
    if (frame.engaged) {
        state->predictedDisplayTime = frame.slot_time + frame.period * static_cast<XrDuration>(multiplier - 1);
        state->predictedDisplayPeriod = frame.period * static_cast<XrDuration>(multiplier);
        SetStateLocked(FrameGenState::kActive, "");
    }
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
    return calls.begin(session, info);
}

XrResult FrameGenPresenter::EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls) {
    std::optional<PendingFrame> frame;
    const XrCompositionLayerProjection* projection = FindProjection(*info);
    const bool renderable = info->layerCount > 0 && projection != nullptr;
    {
        std::scoped_lock lock(mutex_);
        end_thread_ = std::this_thread::get_id();
        auto it = std::find_if(frames_.begin(), frames_.end(),
                               [&](const PendingFrame& pending) { return pending.app_time == info->displayTime; });
        if (it == frames_.end() && !frames_.empty()) {
            it = frames_.begin();
        }
        if (it != frames_.end()) {
            // Claiming under the lock: from here a held wait lets this frame
            // finish its extra runtime frame instead of demoting it.
            if (it->engaged && !it->demoted) {
                it->claimed = true;
            }
            frame = *it;
            app_work_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - it->wait_return).count();
            ++app_work_frames_;
        }
        if (renderable) {
            RecordCaptureTargetsLocked(*projection);
        }
    }
    XrResult result = XR_SUCCESS;
    if (!frame || !frame->engaged) {
        result = calls.submit(session, info);
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
        if (!frame || !frame->engaged) {
            NoteSubmissionLocked();
        }
        UpdateAutoLocked(now, frame);
        if (logger_ && state_ == FrameGenState::kActive && now - last_summary_ >= std::chrono::seconds(30)) {
            last_summary_ = now;
            // appWorkMs: the game's own time per frame between its wait and
            // its end. lateCycles: cycles whose first generated slot was
            // already past due, so the game's frame did not fit the time the
            // runtime gives before that slot.
            logger_->Info("Frame Generation: multiplier=" +
                          (full_rate_ ? std::string("full rate") : MultiplierText(EffectiveMultiplierLocked())) +
                          (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "") + ", appFps=" +
                          Number(RatePerSecond(app_frames_, now), 1) + ", outputFps=" + Number(RatePerSecond(submissions_, now), 1) +
                          ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
                          ", lateCycles=" + std::to_string(late_cycles_) + "/" + std::to_string(cycles_) +
                          ", generated=" + std::to_string(generated_) + ", repeated=" + std::to_string(repeated_) +
                          ", failures=" + std::to_string(failures_) + ", demoted=" + std::to_string(demoted_) +
                          ", captured=" + std::to_string(captured_) + ", gpuMs=" +
                          Number(gpu_ ? gpu_->LastGpuMilliseconds() : -1.0, 2) + ".");
            app_work_ms_ = 0.0;
            app_work_frames_ = 0;
            cycles_ = 0;
            late_cycles_ = 0;
        }
    }
    idle_cv_.notify_all();
    return result;
}

XrResult FrameGenPresenter::SubmitAt(XrSession session, const XrFrameEndInfo& info, XrTime time,
                                     const FrameGenRuntimeCalls& calls) {
    XrFrameEndInfo copy = info;
    copy.displayTime = time;
    const XrResult result = calls.submit(session, &copy);
    std::scoped_lock lock(mutex_);
    NoteSubmissionLocked();
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
    XrResult result = calls.wait(session, &wait_info, &state);
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
    result = calls.begin(session, &begin_info);
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
    auto with_views = [&](const std::vector<XrCompositionLayerProjectionView>& views, XrSpace space,
                          XrCompositionLayerFlags flags) {
        generated_layer = projection;
        generated_layer.next = nullptr;
        generated_layer.space = space;
        generated_layer.layerFlags = flags;
        generated_layer.viewCount = static_cast<std::uint32_t>(views.size());
        generated_layer.views = views.data();
        layers[projection_index] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&generated_layer);
        XrFrameEndInfo generated_info = info;
        generated_info.layers = layers.data();
        return generated_info;
    };

    // Interpolated frames need the newest real frame, so they are generated
    // before the first slot; extrapolated ones were generated last cycle.
    std::vector<std::vector<XrCompositionLayerProjectionView>> interpolated;
    bool interpolated_ready = false;
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        std::vector<float> fractions;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time,
                                                              frame.slot_time + period * static_cast<XrDuration>(slot))
                                           : 1.0f);
        }
        interpolated_ready = Generate(projection, frame, fractions, interpolated);
    }

    XrTime time = frame.slot_time;
    bool should_render = frame.should_render;
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
        }
        XrFrameEndInfo content = info; // the real frame, in the cycle's last slot
        if (slot < generated) {
            if (interpolated_ready) {
                content = with_views(interpolated[slot], projection.space, projection.layerFlags);
            } else if (mode == FrameGenSynthesisMode::kExtrapolate && slot < extrapolated_sets) {
                content = with_views(extrapolated_.sets[slot], extrapolated_.space, extrapolated_.flags);
            }
        }
        content.displayTime = time;
        if (!should_render) {
            content.layerCount = 0;
            content.layers = nullptr;
        }
        const XrResult result = calls.submit(session, &content);
        std::scoped_lock lock(mutex_);
        NoteSubmissionLocked();
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
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time,
                                                              newest_time + period * static_cast<XrDuration>(slot + 1))
                                           : 0.0f);
        }
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        const bool ok = Generate(projection, frame, fractions, sets);
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = ok;
        extrapolated_.sets = std::move(sets);
        extrapolated_.space = projection.space;
        extrapolated_.flags = projection.layerFlags;
    }
    RecordReal(projection, newest_time, frame.sequence);
    std::scoped_lock lock(mutex_);
    consecutive_demotions_ = 0;
    return XR_SUCCESS;
}

void FrameGenPresenter::RecordReal(const XrCompositionLayerProjection& projection, XrTime display_time,
                                   std::uint64_t sequence) {
    std::scoped_lock lock(mutex_);
    last_real_.valid = true;
    last_real_.sequence = sequence;
    last_real_.display_time = display_time;
    last_real_.space = projection.space;
    last_real_.views.clear();
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        last_real_.views.push_back({Extent(projection.views[i]), Geometry(projection.views[i])});
    }
}

bool FrameGenPresenter::Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                 const std::vector<float>& fractions,
                                 std::vector<std::vector<XrCompositionLayerProjectionView>>& sets) {
    const std::size_t count = std::min(fractions.size(), kFrameGenMaxGenerated);
    if (count == 0) {
        return false;
    }
    std::vector<FrameGenViewSubmission> submissions(projection.viewCount);
    std::vector<FrameGenOutputKind> kinds(projection.viewCount);
    FrameGenTuning tuning;
    std::string error;
    {
        std::scoped_lock lock(mutex_);
        if (!gpu_ || state_ == FrameGenState::kUnavailable) {
            return false;
        }
        tuning.mode = settings_.synthesis_mode;
        tuning.quality = settings_.flow_quality;
        tuning.diagnostic = settings_.diagnostic_view;
        tuning.artifact_suppression = settings_.artifact_suppression;
        tuning.motion_limit_percent = settings_.motion_limit_percent;
        tuning.generated = static_cast<std::uint8_t>(count);
        for (std::size_t index = 0; index < count; ++index) {
            tuning.fractions[index] = fractions[index];
        }
        const bool continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence &&
                                last_real_.space == projection.space && last_real_.views.size() == projection.viewCount;
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
            submission.request.newest = Geometry(view);
            submission.request.synthesize = continuous && last_real_.views[i].extent.width == submission.request.extent.width &&
                                            last_real_.views[i].extent.height == submission.request.extent.height;
            submission.request.previous = submission.request.synthesize ? last_real_.views[i].geometry : Geometry(view);
            submission.request.temporal = true;
        }
        if (!error.empty()) {
            NoteFailureLocked(error);
            return false;
        }
    }
    if (!EnsureOutputs(projection, kinds, count, error)) {
        std::scoped_lock lock(mutex_);
        NoteFailureLocked(error);
        return false;
    }

    bool timed_out = false;
    if (!AcquireOutputs(error, timed_out)) {
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
    {
        // Vulkan queues need external synchronization with the runtime's own
        // submissions on the session queue. Lock order: queue, then presenter.
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        std::scoped_lock lock(mutex_);
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
        ok = ok && gpu_->Execute(submissions, tuning, executed, error);
    }
    // Every output here was acquired and waited; release them all so the
    // next frame starts clean. Unwritten images are not submitted this frame.
    ReleaseOutputs();

    std::scoped_lock lock(mutex_);
    if (!ok) {
        NoteFailureLocked(error);
        return false;
    }
    consecutive_failures_ = 0;
    const bool synthesized =
        std::any_of(executed.synthesized.begin(), executed.synthesized.end(), [](bool value) { return value; });
    if (synthesized) {
        generated_ += count;
    } else {
        repeated_ += count;
    }
    sets.assign(count, {});
    for (std::size_t set = 0; set < count; ++set) {
        auto& views = sets[set];
        views.assign(projection.views, projection.views + projection.viewCount);
        for (std::size_t i = 0; i < views.size(); ++i) {
            views[i].next = nullptr; // a generated image has no matching depth
            views[i].subImage.swapchain = output_sets_[set][i].swapchain;
            views[i].subImage.imageRect = submissions[i].outputs[set].rect;
            views[i].subImage.imageArrayIndex = 0;
        }
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

bool FrameGenPresenter::EnsureOutputs(const XrCompositionLayerProjection& projection,
                                      const std::vector<FrameGenOutputKind>& kinds, std::size_t sets,
                                      std::string& error) {
    if (!output_sets_.empty() && output_sets_.front().size() != projection.viewCount) {
        DestroyOutputs();
    }
    if (output_kinds_.size() != projection.viewCount) {
        std::scoped_lock lock(mutex_);
        output_kinds_.assign(projection.viewCount, {});
    }
    // A lower multiplier frees the sets it no longer needs; a higher one adds.
    while (output_sets_.size() > sets) {
        for (Output& output : output_sets_.back()) {
            if (output.swapchain) {
                ForgetSwapchain(output.swapchain); // waits for submissions that may still write it
                xr_.destroy(output.swapchain);
            }
        }
        output_sets_.pop_back();
        std::scoped_lock lock(mutex_);
        extrapolated_.valid = false;
    }
    while (output_sets_.size() < sets) {
        output_sets_.emplace_back(projection.viewCount);
    }
    bool created = false;
    for (std::size_t set = 0; set < output_sets_.size(); ++set) {
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
            Output& output = output_sets_[set][i];
            const FrameGenExtent extent = Extent(projection.views[i]);
            if (output.swapchain && output.extent.width == extent.width && output.extent.height == extent.height &&
                output.requested == kinds[i]) {
                continue;
            }
            if (output.swapchain) {
                {
                    std::scoped_lock lock(mutex_);
                    output_kinds_[i] = {};
                }
                ForgetSwapchain(output.swapchain);
                xr_.destroy(output.swapchain);
                output = {};
            }
            std::vector<std::int64_t> candidates;
            XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            {
                std::scoped_lock lock(mutex_);
                candidates = gpu_->OutputFormats(kinds[i]);
                if (kinds[i] != FrameGenOutputKind::kSrgb8) {
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
            output.requested = kinds[i];
            // An sRGB fallback for a UNORM or float source must also encode.
            output.kind = gpu_->Classify(format).value_or(FrameGenOutputKind::kSrgb8);
            output_kinds_[i] = {true, output.requested, output.kind};
            extrapolated_.valid = false;
        }
    }
    if (created && logger_) {
        // Rough video memory estimate: per view, two full-resolution history
        // copies, a full-resolution intermediate and the flow pyramid in the
        // backend, plus every output swapchain image.
        double working = 0.0, swapchains = 0.0;
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
            const Output& first = output_sets_.front()[i];
            const double pixels = static_cast<double>(first.extent.width) * first.extent.height;
            const double bytes = first.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
            working += pixels * (3.0 * bytes + 3.4);
        }
        for (const auto& set : output_sets_) {
            for (const Output& output : set) {
                const double bytes = output.kind == FrameGenOutputKind::kFloat16 ? 8.0 : 4.0;
                swapchains += static_cast<double>(output.extent.width) * output.extent.height * bytes *
                              std::max<std::uint32_t>(output.image_count, 1);
            }
        }
        const double megabytes = 1024.0 * 1024.0;
        logger_->Info("Frame Generation output swapchains ready: generatedPerFrame=" + std::to_string(output_sets_.size()) +
                      ", imagesPerSwapchain=" + std::to_string(output_sets_.front().front().image_count) +
                      ", estimated video memory " + Number((working + swapchains) / megabytes, 0) + " MB (working " +
                      Number(working / megabytes, 0) + " MB, output swapchains " + Number(swapchains / megabytes, 0) +
                      " MB).");
    }
    return true;
}

bool FrameGenPresenter::AcquireOutputs(std::string& error, bool& timed_out) {
    timed_out = false;
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (!output.acquired) {
                XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                const XrResult acquired = xr_.acquire(output.swapchain, &acquire, &output.index);
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
                const XrResult waited = xr_.wait(output.swapchain, &wait);
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
            xr_.release(output.swapchain, &release);
            output.acquired = false;
            output.waited = false;
        }
    }
}

void FrameGenPresenter::DestroyOutputs() {
    ReleaseOutputs();
    for (auto& set : output_sets_) {
        for (Output& output : set) {
            if (output.swapchain) {
                ForgetSwapchain(output.swapchain);
                if (xr_.destroy) xr_.destroy(output.swapchain);
            }
        }
    }
    output_sets_.clear();
    std::scoped_lock lock(mutex_);
    output_kinds_.clear();
    extrapolated_ = {};
}

void FrameGenPresenter::NoteSubmissionLocked() {
    last_runtime_submit_ = Clock::now();
    submissions_.push_back(last_runtime_submit_);
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
    status.gpu_ms = gpu_ ? gpu_->LastGpuMilliseconds() : -1.0;
    status.generated = generated_;
    status.repeated = repeated_;
    status.failures = failures_;
    status.demoted = demoted_;
    status.captured = captured_;
    status.retry_pending = state_ == FrameGenState::kPaused && automatic_retries_ < retry_attempts_;
    status.multiplier = EffectiveMultiplierLocked();
    status.automatic = settings_.multiplier == FrameGenMultiplier::kAuto;
    status.full_rate = full_rate_ && state_ == FrameGenState::kActive;
    return status;
}

std::string FrameGenPresenter::OsdText() const {
    const FrameGenStatus status = Status();
    switch (status.state) {
    case FrameGenState::kActive: {
        if (status.full_rate) {
            return "Auto  /  full rate, no generation needed";
        }
        std::string text = std::string(status.mode == FrameGenSynthesisMode::kExtrapolate ? "Extrapolate" : "Interpolate") +
                           "  /  " + (status.automatic ? "Auto " : "") + MultiplierText(status.multiplier) + "  /  " +
                           Number(status.app_fps, 0) + " -> " + Number(status.output_fps, 0) + " fps";
        if (status.gpu_ms > 0.0) text += "  /  " + Number(status.gpu_ms, 1) + " ms";
        return text;
    }
    case FrameGenState::kWaiting:
        return "Waiting for steady frames";
    case FrameGenState::kPaused:
        return "Paused  /  " + status.reason + (status.retry_pending ? "  /  retrying soon" : "");
    case FrameGenState::kUnavailable:
        return "Unavailable  /  " + status.reason;
    default:
        return Started() ? "Off" : "Disabled";
    }
}

std::string FrameGenPresenter::CompactOsdText() const {
    const FrameGenStatus status = Status();
    switch (status.state) {
    case FrameGenState::kActive:
        if (status.full_rate) return "Full rate";
        return std::string(status.automatic ? "Auto " : "") + MultiplierText(status.multiplier) + "  /  " +
               Number(status.app_fps, 0) + " -> " + Number(status.output_fps, 0) + " fps";
    case FrameGenState::kWaiting:
        return "Waiting";
    case FrameGenState::kPaused:
        return "Paused";
    case FrameGenState::kUnavailable:
        return "Unavailable";
    default:
        return Started() ? "Off" : "Disabled";
    }
}

} // namespace depthxr
