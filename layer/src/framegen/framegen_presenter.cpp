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
constexpr std::chrono::seconds kPhaseLifetime{2};
constexpr int kAutoMaxBackoffDoublings = 3; // 10 s, 20 s, 40 s, 80 s
// Auto stays at full rate this long after inline cycles kept starting late.
constexpr std::chrono::seconds kInlineHold{60};
// The presenter thread waits this long for frames in flight when it stops.
constexpr std::chrono::seconds kThreadedDrain{2};

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

// A head moves well under 2 m/s: eye shifts per unit of generated-frame
// time (at most a real frame interval, ~50 ms at low frame rates) above
// this are tracking glitches.
constexpr double kMaxEyeShift = 0.1;

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

void FrameGenPresenter::Start(XrSession session, const void* binding_chain, const FrameGenSwapchainDispatch& dispatch,
                              Logger* logger, std::mutex* queue_mutex, GpuFactory factory) {
    Stop();
    std::scoped_lock lock(gpu_mutex_, mutex_);
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
                      ", motionEstimation=" + ToString(settings_.motion_estimation) +
                      "; frame pacing is owned by Frame Generation (Turbo stays off).");
    }
}

void FrameGenPresenter::Stop() {
    std::unique_lock lock(mutex_);
    if (!started_) {
        return;
    }
    StopCompositorLocked(lock);
    // From here no release thread starts a capture.
    started_ = false;
    lock.unlock();
    {
        // Finish outstanding GPU work before the runtime frees the output
        // images it writes. Teardown runs between frames, so no EndFrame
        // touches the outputs concurrently.
        std::scoped_lock gpu_lock(gpu_mutex_);
        if (gpu_) {
            gpu_->Reset();
        }
    }
    DestroyOutputs();
    {
        std::scoped_lock gpu_lock(gpu_mutex_);
        lock.lock();
        gpu_.reset();
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
    threaded_frames_.clear();
    dropped_unended_ = 0;
    threaded_disabled_ = logged_threaded_ = false;
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
    logged_order_guard_ = logged_watchdog_ = false;
    display_offset_periods_ = 0.0;
    display_offset_samples_ = 0;
    slot_index_ = 0;
    slot_time_ = slot_period_ = 0;
    app_latency_ms_ = 0.0;
    passthrough_copy_ = {};
    threaded_late_ = 0;
    lead_slots_ = 0;
    lead_history_.clear();
    dropped_unbegun_ = 0;
    gpu_ms_ = -1.0;
    inline_hold_until_.reset();
    last_cycle_started_late_ = false;
    app_frames_.clear();
    submissions_.clear();
    logged_first_generation_ = logged_capture_failure_ = logged_depth_ = false;
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
    std::unique_lock lock(mutex_);
    // The session is starting or stopping: the presenter thread must not
    // touch the runtime frame loop past this point.
    StopCompositorLocked(lock);
    dropped_unended_ = 0;
    dropped_unbegun_ = 0;
    // A new session starts with no runtime frame in flight.
    runtime_waits_ = runtime_begins_ = runtime_ends_ = 0;
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
        // The oldest open frame is the one ending, as in EndFrame; the time
        // is only a hint (apps may end with another time than they were given).
        (void)display_time;
        if (dropped_unended_ > 0) {
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

void FrameGenPresenter::ForgetSwapchain(XrSwapchain swapchain) {
    // Holding submit_mutex_ keeps the presenter thread from submitting the
    // swapchain while it goes; afterwards no copy refers to it.
    std::scoped_lock submit(submit_mutex_);
    {
        std::scoped_lock lock(mutex_);
        swapchains_.erase(swapchain);
        capture_targets_.erase(std::remove_if(capture_targets_.begin(), capture_targets_.end(),
                                              [&](const CaptureTarget& target) { return target.swapchain == swapchain; }),
                               capture_targets_.end());
        StripSwapchain(newest_copy_, swapchain);
        StripSwapchain(passthrough_copy_, swapchain);
    }
    std::scoped_lock gpu_lock(gpu_mutex_);
    if (gpu_) {
        // A recent submission may still read or write this swapchain's
        // images; destroys are rare, so always let it finish first.
        gpu_->WaitIdle();
        gpu_->Forget(swapchain);
    }
}

void FrameGenPresenter::ForgetSpace(XrSpace space) {
    // As for swapchains: under submit_mutex_, no presenter-thread submission
    // is using the space, and none will after this.
    std::scoped_lock submit(submit_mutex_);
    std::scoped_lock lock(mutex_);
    StripSpace(newest_copy_, space);
    StripSpace(passthrough_copy_, space);
    if (extrapolated_.space == space) {
        extrapolated_.valid = false;
        generated_ready_ = false;
    }
    if (last_real_.space == space) {
        InvalidateContinuityLocked();
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
    };
    std::vector<Job> jobs;
    FrameGenFlowQuality quality;
    {
        std::scoped_lock lock(mutex_);
        if (!capturing()) {
            return;
        }
        quality = settings_.flow_quality;
        const auto record = swapchains_.find(swapchain);
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
    std::string failure;
    for (const Job& job : jobs) {
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
        ++captured;
    }
    std::scoped_lock lock(mutex_);
    captured_ += captured;
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

bool FrameGenPresenter::RunnableLocked() const {
    return started_ && gpu_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && healthy_streak_ >= engage_streak_frames_ &&
           Clock::now() - started_at_ >= engage_min_age_;
}

bool FrameGenPresenter::WantsEngagementLocked() const {
    return RunnableLocked() && EffectiveMultiplierLocked() >= 2;
}

bool FrameGenPresenter::StayThreadedLocked() const {
    return started_ && gpu_ && user_enabled_ && configured_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && EffectiveMultiplierLocked() >= 2;
}

bool FrameGenPresenter::ThreadedEligibleLocked() const {
    return gpu_ && gpu_->SupportsThreadedPresentation() && !threaded_disabled_ &&
           settings_.synthesis_mode == FrameGenSynthesisMode::kExtrapolate;
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

void FrameGenPresenter::UpdateAutoLocked(Clock::time_point now, const std::optional<PendingFrame>& frame,
                                         bool started_late) {
    const bool automatic = settings_.multiplier == FrameGenMultiplier::kAuto;
    const XrDuration period = frame && frame->period > 0 ? frame->period : last_period_;
    if (!automatic || period <= 0 || !RunnableLocked()) {
        last_app_end_ = now;
        cadence_.clear();
        return;
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
    frame.engaged = engage && frame.period > 0 && state_ != FrameGenState::kPaused;
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

XrResult FrameGenPresenter::EndFrame(XrSession session, const XrFrameEndInfo* info, const FrameGenRuntimeCalls& calls) {
    const auto entered = Clock::now();
    {
        std::optional<ThreadedFrame> threaded;
        {
            std::scoped_lock lock(mutex_);
            // Frames end in the order they were waited, whatever display time
            // an app reports (MSFS reports one period later): the oldest open
            // frame is the one ending. Dropped frames are the oldest, then
            // frames the runtime waited for, then the presenter thread's.
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
                // The presenter thread owns the runtime frame loop and holds
                // no runtime frame for this one.
                if (logger_ && !logged_swallowed_) {
                    logged_swallowed_ = true;
                    logger_->Error("Frame Generation: an application frame matched no waited frame while presenting "
                                   "from its own thread; it was not shown.");
                }
                return XR_SUCCESS;
            }
        }
        if (threaded) {
            return ThreadedEnd(session, *info, calls, *threaded, entered);
        }
    }
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
            NoteDisplayOffsetLocked(info->displayTime, it->app_time, it->reported_period);
            app_work_ms_ += std::chrono::duration<double, std::milli>(Clock::now() - it->wait_return).count();
            ++app_work_frames_;
        }
        if (renderable) {
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
        if (!frame || !frame->engaged) {
            NoteSubmissionLocked();
        }
        UpdateAutoLocked(now, frame, frame && frame->engaged && last_cycle_started_late_);
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
                          ", threaded=" + std::to_string(compositor_running_) +
                          ", generated=" + std::to_string(generated_) + ", repeated=" + std::to_string(repeated_) +
                          ", failures=" + std::to_string(failures_) + ", demoted=" + std::to_string(demoted_) +
                          ", captured=" + std::to_string(captured_) + ", gpuMs=" + Number(gpu_ms_, 2) + ".");
            app_work_ms_ = 0.0;
            app_work_frames_ = 0;
            cycles_ = 0;
            late_cycles_ = 0;
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
    bool interpolated_ready = false;
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        std::vector<float> fractions;
        std::vector<XrTime> targets;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            targets.push_back(frame.slot_time + period * static_cast<XrDuration>(slot));
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, targets.back()) : 1.0f);
        }
        interpolated_ready = Generate(projection, frame, fractions, targets, calls, interpolated);
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
            last_cycle_started_late_ = late;
        }
        XrFrameEndInfo content = info; // the real frame, in the cycle's last slot
        if (slot < generated) {
            if (interpolated_ready) {
                content = with_views(interpolated[slot], projection.space, projection.layerFlags, true);
            } else if (mode == FrameGenSynthesisMode::kExtrapolate && slot < extrapolated_sets) {
                // From the previous real frame, whose depth image the game has
                // since replaced: no depth.
                content = with_views(extrapolated_.sets[slot], extrapolated_.space, extrapolated_.flags, false);
            }
        }
        content.displayTime = time;
        if (!should_render) {
            content.layerCount = 0;
            content.layers = nullptr;
        }
        const XrResult result = RuntimeEnd(calls, session, &content);
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
        std::vector<XrTime> targets;
        for (std::uint32_t slot = 0; slot < generated; ++slot) {
            targets.push_back(newest_time + period * static_cast<XrDuration>(slot + 1));
            fractions.push_back(continuous ? FrameGenFraction(mode, previous_time, newest_time, targets.back()) : 0.0f);
        }
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        const bool ok = Generate(projection, frame, fractions, targets, calls, sets);
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

std::vector<std::array<float, 3>> FrameGenPresenter::EyeShifts(const XrCompositionLayerProjection& projection,
                                                               XrTime newest_time, const std::vector<float>& times,
                                                               const std::vector<XrTime>& targets,
                                                               const FrameGenRuntimeCalls& calls) {
    std::vector<std::array<float, 3>> shifts(projection.viewCount, {0.0f, 0.0f, 0.0f});
    if (!calls.locate || newest_time <= 0 || times.size() != targets.size()) {
        return shifts;
    }
    std::vector<XrView> newest;
    if (XR_FAILED(calls.locate(session_, projection.space, newest_time, newest)) || newest.size() != projection.viewCount) {
        return shifts;
    }
    // Least squares through the origin: the eye moves `shift` per unit time.
    std::vector<std::array<double, 3>> sums(projection.viewCount, {0.0, 0.0, 0.0});
    double norm = 0.0;
    std::vector<XrView> later;
    for (std::size_t j = 0; j < targets.size(); ++j) {
        if (times[j] <= 0.0f) {
            continue;
        }
        if (XR_FAILED(calls.locate(session_, projection.space, targets[j], later)) || later.size() != newest.size()) {
            return shifts;
        }
        for (std::size_t i = 0; i < newest.size(); ++i) {
            // Into the eye's own view space at the newest frame: the same
            // movement in the view the game rendered, whatever it did to
            // the world around it.
            const XrQuaternionf& q = newest[i].pose.orientation;
            const std::array<double, 3> delta{later[i].pose.position.x - newest[i].pose.position.x,
                                              later[i].pose.position.y - newest[i].pose.position.y,
                                              later[i].pose.position.z - newest[i].pose.position.z};
            const std::array<double, 3> local = RotateInverse(q, delta);
            for (int axis = 0; axis < 3; ++axis) {
                sums[i][axis] += times[j] * local[axis];
            }
        }
        norm += static_cast<double>(times[j]) * times[j];
    }
    if (norm <= 0.0) {
        return shifts;
    }
    for (std::size_t i = 0; i < shifts.size(); ++i) {
        std::array<double, 3> shift{sums[i][0] / norm, sums[i][1] / norm, sums[i][2] / norm};
        // A head moves well under 2 m/s; anything more is a tracking glitch.
        const double length = std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]);
        if (std::isfinite(length) && length < kMaxEyeShift) {
            shifts[i] = {static_cast<float>(shift[0]), static_cast<float>(shift[1]), static_cast<float>(shift[2])};
        }
    }
    return shifts;
}

bool FrameGenPresenter::Generate(const XrCompositionLayerProjection& projection, const PendingFrame& frame,
                                 const std::vector<float>& fractions, const std::vector<XrTime>& targets,
                                 const FrameGenRuntimeCalls& calls,
                                 std::vector<std::vector<XrCompositionLayerProjectionView>>& sets) {
    const std::size_t count = std::min(fractions.size(), kFrameGenMaxGenerated);
    if (count == 0) {
        return false;
    }
    // Each generated frame's time past the newest real frame (before it when
    // interpolating), in the fractions' units, and where the eyes move.
    FrameGenSynthesisMode mode;
    {
        std::scoped_lock lock(mutex_);
        mode = settings_.synthesis_mode;
    }
    std::vector<float> times(count, 0.0f);
    for (std::size_t index = 0; index < count; ++index) {
        times[index] = mode == FrameGenSynthesisMode::kExtrapolate ? fractions[index] : 1.0f - fractions[index];
    }
    const std::vector<std::array<float, 3>> shifts =
        targets.size() >= count
            ? EyeShifts(projection, frame.app_time, times, std::vector<XrTime>(targets.begin(), targets.begin() + count),
                        calls)
            : std::vector<std::array<float, 3>>(projection.viewCount, {0.0f, 0.0f, 0.0f});
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
        tuning.hardware_flow = settings_.motion_estimation == FrameGenMotionEstimation::kHardware;
        tuning.diagnostic = settings_.diagnostic_view;
        tuning.artifact_suppression = settings_.artifact_suppression;
        tuning.motion_limit_percent = settings_.motion_limit_percent;
        tuning.generated = static_cast<std::uint8_t>(count);
        for (std::size_t index = 0; index < count; ++index) {
            tuning.fractions[index] = fractions[index];
        }
        const bool continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence &&
                                last_real_.space == projection.space && last_real_.views.size() == projection.viewCount;
        if (continuous && last_real_.interval > 0 && frame.app_time > last_real_.display_time) {
            tuning.interval_ratio = std::clamp(static_cast<float>(frame.app_time - last_real_.display_time) /
                                                   static_cast<float>(last_real_.interval),
                                               0.25f, 4.0f);
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
            submission.request.newest = Geometry(view);
            submission.request.synthesize = continuous && last_real_.views[i].extent.width == submission.request.extent.width &&
                                            last_real_.views[i].extent.height == submission.request.extent.height;
            submission.request.previous = submission.request.synthesize ? last_real_.views[i].geometry : Geometry(view);
            submission.request.temporal = true;
            submission.request.shift = shifts[i];
            if (const auto* depth = DepthInfo(view)) {
                const auto depth_record = swapchains_.find(depth->subImage.swapchain);
                submission.request.depth =
                    depth_record != swapchains_.end() && depth_record->second.depth_capturable;
            }
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
    if (!AcquireOutputs(count, error, timed_out)) {
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
            depth = gpu_->DepthStatus();
            if (tuning.hardware_flow) {
                hardware_status = gpu_->HardwareFlowStatus();
            }
        }
    }
    // Every output here was acquired and waited; release them all so the
    // next frame starts clean. Unwritten images are not submitted this frame.
    ReleaseOutputs();

    std::scoped_lock lock(mutex_);
    if (!ok) {
        NoteFailureLocked(error);
        return false;
    }
    gpu_ms_ = gpu_ms;
    consecutive_failures_ = 0;
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
            (depth.reading == FrameGenDepthReading::kUnusable ? ", so head movement is compensated for rotation only"
                                                              : ", head movement is compensated in full") +
            ". Where depth moves content (" + Number(evidence.binocular > 0.0 ? 100.0 * moved / evidence.binocular : 0.0, 0) +
            "% of what both eyes see) it leaves " + Number(evidence.decoded.Ratio(), 2) +
            " of the error without depth (swapped " + Number(evidence.swapped.Ratio(), 2) +
            "); the other eye disproves " + Number(moved > 0.0 ? 100.0 * evidence.decoded.fallback / moved : 0.0, 0) +
            "% of it, which is treated as far (cockpit glass written to depth, or depth wrong there).");
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
    sets.assign(count, {});
    double moved_mm = 0.0;
    for (std::size_t set = 0; set < count; ++set) {
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
    // A higher multiplier adds sets; a lower one keeps them, unused, so
    // moving between multipliers never reallocates headset-resolution images
    // (hundreds of milliseconds on the frame thread at high resolutions).
    while (output_sets_.size() < sets) {
        output_sets_.emplace_back(projection.viewCount);
    }
    bool created = false;
    for (std::size_t set = 0; set < sets; ++set) {
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
                {
                    std::scoped_lock submit(submit_mutex_);
                    xr_.destroy(output.swapchain);
                }
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
    if (created) {
        // The allocation stalled this frame: let Auto settle again.
        std::scoped_lock lock(mutex_);
        multiplier_changed_at_ = Clock::now();
        cadence_.clear();
    }
    if (created && logger_) {
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
        for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
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
        logger_->Info("Frame Generation output swapchains ready: generatedPerFrame=" + std::to_string(sets) +
                      ", setsKept=" + std::to_string(output_sets_.size()) +
                      ", imagesPerSwapchain=" + std::to_string(output_sets_.front().front().image_count) +
                      ", estimated video memory " + Number((working + swapchains) / megabytes, 0) + " MB (working " +
                      Number(working / megabytes, 0) + " MB, output swapchains " + Number(swapchains / megabytes, 0) +
                      " MB).");
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
            {
                std::scoped_lock submit(submit_mutex_);
                xr_.release(output.swapchain, &release);
            }
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
                std::scoped_lock submit(submit_mutex_);
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

// --- Threaded presentation -----------------------------------------------------

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
                                            std::vector<const XrCompositionLayerBaseHeader*>& pointers) {
    pointers.clear();
    for (std::size_t i = 0; i < copy.layers.size(); ++i) {
        FrameCopy::Layer& layer = copy.layers[i];
        const void* header = nullptr;
        switch (layer.type) {
        case XR_TYPE_COMPOSITION_LAYER_PROJECTION:
            if (copy.has_projection && i == copy.projection_index && !generated && copy.projection_stale) {
                continue;
            }
            if (generated && copy.has_projection && i == copy.projection_index) {
                // The real frame's depth stays: the game has not released a
                // newer depth image before its next real frame, and without it
                // the runtime would correct head movement on real frames but
                // not generated ones, making near geometry jump each cycle.
                layer.views = *generated;
                layer.depth.resize(layer.views.size(), XrCompositionLayerDepthInfoKHR{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR});
                layer.has_depth.resize(layer.views.size(), 0);
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
    const XrResult result = calls.wait(session, info, state);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_waits_;
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeBegin(const FrameGenRuntimeCalls& calls, XrSession session,
                                         const XrFrameBeginInfo* info) {
    const XrResult result = calls.begin(session, info);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(mutex_);
        ++runtime_begins_;
        // A begin over a frame still open discards that frame.
        runtime_ends_ = std::max(runtime_ends_, runtime_begins_ - 1);
        runtime_waits_ = std::max(runtime_waits_, runtime_begins_);
    }
    return result;
}

XrResult FrameGenPresenter::RuntimeEnd(const FrameGenRuntimeCalls& calls, XrSession session, const XrFrameEndInfo* info) {
    const XrResult result = calls.submit(session, info);
    {
        std::scoped_lock lock(mutex_);
        // Counted even when the runtime rejects the frame: it is over either way.
        if (runtime_ends_ < runtime_begins_) {
            ++runtime_ends_;
        }
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
    newest_copy_ = passthrough_copy_;
    generation_pending_ = generated_ready_ = false;
    next_generated_set_ = 0;
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
        logger_->Info(std::string("Frame Generation presents from its own thread (") + gpu_->Name() +
                      ", Extrapolate): the game keeps rendering while generated frames are shown, and its frame waits "
                      "are paced without reaching the runtime.");
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

void FrameGenPresenter::StopCompositorLocked(std::unique_lock<std::mutex>& lock) {
    // Also after the thread stopped itself: its frames would otherwise stay
    // in flight forever and hold every later wait.
    DropUnclaimedLocked();
    if (compositor_running_) {
        compositor_stop_ = true;
        WakeSlots();
        slot_cv_.wait(lock, [&] { return !compositor_running_; });
    }
    JoinCompositorLocked(lock);
    compositor_stop_ = compositor_drain_ = false;
    slot_request_.reset();
    last_release_slot_.reset();
    generation_pending_ = generated_ready_ = false;
    newest_copy_ = {};
    compositor_phase_ = "stopped";
    InvalidateContinuityLocked();
    WakeSlots();
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

void FrameGenPresenter::UpdateLeadLocked(bool late, std::optional<double> slack_ms, const ThreadedFrame& frame) {
    // Frames predicted with an older lead say nothing about this one.
    if (frame.lead < lead_slots_) {
        return;
    }
    const double period_ms = static_cast<double>(std::max<XrDuration>(last_period_, 1)) / 1e6;
    // Slack: it could have arrived a refresh later and still made its slot.
    lead_history_.push_back(late ? 1 : (slack_ms && *slack_ms > period_ms * 1.1) ? 2 : 0);
    while (lead_history_.size() > 10) {
        lead_history_.pop_front();
    }
    const auto count = [&](std::uint8_t kind) {
        return static_cast<std::size_t>(std::count(lead_history_.begin(), lead_history_.end(), kind));
    };
    if (count(1) >= 2) {
        lead_slots_ = std::min(lead_slots_ + 1, 4 * frame.multiplier);
        lead_history_.clear();
    } else if (lead_history_.size() >= 8 && count(2) >= 7 && lead_slots_ > 1) {
        --lead_slots_;
        lead_history_.clear();
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
    const std::uint32_t multiplier = std::max(2u, EffectiveMultiplierLocked());
    std::uint64_t release = last_release_slot_ ? *last_release_slot_ + multiplier : slot_index_ + 1;
    release = std::max(release, slot_index_);
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2),
                              [&] { return !compositor_running_ || slot_index_ >= release; })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error(std::string("Frame Generation: the presenter thread has published no refresh for 2 s (it is ") +
                           compositor_phase_ + ").");
        }
    }
    if (!compositor_running_) {
        return std::nullopt;
    }
    last_release_slot_ = slot_index_;
    const XrDuration period = slot_period_ > 0 ? slot_period_ : std::max<XrDuration>(last_period_, 1);
    const double period_ms = static_cast<double>(period) / 1e6;
    if (lead_slots_ == 0) {
        // Until measured, assume the frame takes one application frame.
        const double latency = app_latency_ms_ > 0.0 ? app_latency_ms_ : period_ms * multiplier;
        lead_slots_ = static_cast<std::uint32_t>(std::clamp(std::ceil((latency + 1.0) / period_ms), 1.0, 4.0 * multiplier));
    }
    // Predict the slot the frame will really reach.
    const std::uint64_t slots = std::clamp<std::uint64_t>(lead_slots_, 1, 4 * multiplier);
    ThreadedFrame frame;
    frame.id = ++threaded_id_;
    frame.multiplier = multiplier;
    frame.lead = static_cast<std::uint32_t>(slots);
    frame.target_slot = slot_index_ + slots;
    frame.target_time = slot_time_ + period * static_cast<XrDuration>(slots);
    frame.reported_period = period * static_cast<XrDuration>(multiplier);
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
                                        const ThreadedFrame& frame, Clock::time_point entered) {
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
    if (renderable) {
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
    if (phase_wall_ != Clock::time_point{} && frame.target_time > phase_xr_) {
        const XrDuration period = std::max<XrDuration>(last_period_, 1);
        const auto deadline = phase_wall_ + std::chrono::nanoseconds(frame.target_time - phase_xr_ + period / 2);
        slack_ms = std::chrono::duration<double, std::milli>(deadline - entered).count();
    }
    // Ask for the first slot at or after the one this frame was rendered for.
    slot_request_ = SlotRequest{frame.id, frame.target_slot};
    WakeSlots();
    while (!slot_cv_.wait_for(lock, std::chrono::seconds(2), [&] {
        return !compositor_running_ || (slot_offer_ && slot_offer_->frame_id == frame.id);
    })) {
        if (logger_ && !logged_watchdog_) {
            logged_watchdog_ = true;
            logger_->Error(std::string("Frame Generation: a real frame has waited 2 s for a slot (the presenter thread "
                                       "is ") +
                           compositor_phase_ + ").");
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
        return XR_SUCCESS;
    }
    const SlotOffer offer = *slot_offer_;
    lock.unlock();

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    XrResult result = RuntimeBegin(calls, session, &begin_info);
    if (XR_SUCCEEDED(result)) {
        XrFrameEndInfo real = info;
        real.displayTime = offer.time;
        if (!offer.should_render) {
            real.layerCount = 0;
            real.layers = nullptr;
        }
        result = RuntimeEnd(calls, session, &real);
    }
    const bool generate = XR_SUCCEEDED(result) && renderable && copied && offer.should_render && frame.multiplier >= 2;

    lock.lock();
    NoteSubmissionLocked();
    // From here the presenter thread fills its slots from this frame: again
    // until the frames generated from it are ready, then those.
    newest_copy_ = copied ? std::move(copy) : FrameCopy{};
    generated_ready_ = false;
    generation_pending_ = generate;
    next_generated_set_ = 0;
    const bool late = offer.slot > frame.target_slot;
    threaded_late_ += late ? 1 : 0;
    UpdateLeadLocked(late, slack_ms, frame);
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
    const XrTime previous_time = last_real_.display_time;
    const XrDuration period = slot_period_ > 0 ? slot_period_ : last_period_;
    lock.unlock();

    if (generate) {
        std::vector<float> fractions;
        std::vector<XrTime> targets;
        // One frame per generated slot, plus a spare for the slot the next
        // real frame is due in: if that frame is late, the slot still shows
        // motion instead of repeating the last generated frame.
        for (std::uint32_t slot = 1; slot <= frame.multiplier && fractions.size() < kFrameGenMaxGenerated; ++slot) {
            targets.push_back(offer.time + period * static_cast<XrDuration>(slot));
            fractions.push_back(
                FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, previous_time, content_time, targets.back()));
        }
        PendingFrame pending;
        pending.app_time = content_time;
        pending.sequence = frame.id;
        std::vector<std::vector<XrCompositionLayerProjectionView>> sets;
        const bool ok = Generate(*projection, pending, fractions, targets, calls, sets);
        RecordReal(*projection, content_time, frame.id);
        lock.lock();
        if (compositor_running_) {
            extrapolated_.valid = ok;
            extrapolated_.sets = std::move(sets);
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
        if (!renderable) {
            InvalidateContinuityLocked();
        }
    }
    app_frames_.push_back(entered);
    PendingFrame cadence;
    cadence.engaged = true;
    cadence.multiplier = frame.multiplier;
    cadence.period = period;
    UpdateAutoLocked(entered, cadence);
    if (logger_ && state_ == FrameGenState::kActive && entered - last_summary_ >= std::chrono::seconds(30)) {
        last_summary_ = entered;
        logger_->Info("Frame Generation: multiplier=" + MultiplierText(EffectiveMultiplierLocked()) +
                      (settings_.multiplier == FrameGenMultiplier::kAuto ? " (auto)" : "") + ", threaded=1, appFps=" +
                      Number(RatePerSecond(app_frames_, entered), 1) + ", outputFps=" +
                      Number(RatePerSecond(submissions_, entered), 1) + ", appLatencyMs=" + Number(app_latency_ms_, 1) +
                      ", leadSlots=" + std::to_string(lead_slots_) +
                      ", appWorkMs=" + Number(app_work_frames_ ? app_work_ms_ / app_work_frames_ : 0.0, 1) +
                      ", lateReal=" + std::to_string(threaded_late_) + ", generated=" + std::to_string(generated_) +
                      ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) +
                      ", captured=" + std::to_string(captured_) + ", gpuMs=" + Number(gpu_ms_, 2) + ", depthUsed=" +
                      Number(depth_window_frames_ ? 100.0 * depth_window_used_ / depth_window_frames_ : 0.0, 0) +
                      "%, eyeShiftMm=" +
                      Number(shift_window_frames_ ? shift_window_sum_mm_ / shift_window_frames_ : 0.0, 1) + "/" +
                      Number(shift_window_max_mm_, 1) + ".");
        app_work_ms_ = 0.0;
        app_work_frames_ = 0;
        threaded_late_ = 0;
        depth_window_frames_ = depth_window_used_ = 0;
        shift_window_sum_mm_ = shift_window_max_mm_ = 0.0;
        shift_window_frames_ = 0;
    }
    return result;
}

void FrameGenPresenter::CompositorLoop() {
    // A compositor-like thread: it must make every refresh even while the
    // game saturates the CPU.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"VectorXR Frame Generation presenter");
    std::unique_lock lock(mutex_);
    const FrameGenRuntimeCalls calls = compositor_calls_;
    const XrSession session = session_;
    void* const timer = compositor_timer_;
    std::vector<const XrCompositionLayerBaseHeader*> pointers;
    const auto ns = [](XrDuration duration) { return std::chrono::nanoseconds(duration); };
    const auto forever = Clock::time_point::max();
    while (!compositor_stop_ && !(compositor_drain_ && threaded_frames_.empty())) {
        // A runtime frame the application began before the switch owns the
        // runtime until it ends: no runtime wait while any frame is open.
        compositor_phase_ = "waiting for the game's open runtime frame";
        if (!CompositorWait(lock, Clock::now() + std::chrono::seconds(1), [&] {
                return compositor_stop_ || (!RuntimeFrameOpenLocked() && !RuntimeWaitPendingLocked());
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
            return !generated_ready_ &&
                   (!newest_copy_.valid || (newest_copy_.has_projection && newest_copy_.projection_stale));
        };
        auto eligible = [&] { return slot_request_ && (slot_request_->earliest_slot <= slot || unusable()); };
        auto real_due = [&] {
            return std::any_of(threaded_frames_.begin(), threaded_frames_.end(),
                               [&](const ThreadedFrame& frame) { return frame.target_slot <= slot; });
        };
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

        // Fill the slot: the next frame generated from the newest real frame,
        // or that real frame again.
        lock.unlock();
        SleepUntil(timer, submit_at);
        std::unique_lock submit_lock(submit_mutex_);
        lock.lock();
        FrameCopy content = newest_copy_;
        std::vector<XrCompositionLayerProjectionView> views;
        const bool use_generated =
            content.valid && content.has_projection && generated_ready_ && extrapolated_.valid &&
            !extrapolated_.sets.empty() &&
            extrapolated_.sets.front().size() == content.layers[content.projection_index].views.size();
        if (use_generated) {
            views = extrapolated_.sets[std::min(next_generated_set_, extrapolated_.sets.size() - 1)];
            ++next_generated_set_;
        }
        const XrSpace space = extrapolated_.space;
        const XrCompositionLayerFlags flags = extrapolated_.flags;
        const XrTime time = slot_time_;
        const bool should_render = slot_should_render_;
        lock.unlock();
        XrFrameEndInfo end_info = BuildFrame(content, use_generated ? &views : nullptr, space, flags, pointers);
        end_info.displayTime = time;
        if (!should_render || !content.valid) {
            end_info.layerCount = 0;
            end_info.layers = nullptr;
        }
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        compositor_phase_ = "submitting a generated frame";
        XrResult result = RuntimeBegin(calls, session, &begin_info);
        if (XR_SUCCEEDED(result)) {
            result = RuntimeEnd(calls, session, &end_info);
        }
        submit_lock.unlock();
        lock.lock();
        NoteSubmissionLocked();
        if (XR_FAILED(result)) {
            PauseLocked("runtime rejected a frame from the presenter thread (" + std::to_string(result) + ")");
            break;
        }
    }
    compositor_running_ = false;
    compositor_phase_ = "stopped";
    slot_request_.reset();
    WakeSlots();
    idle_cv_.notify_all();
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
    status.gpu_ms = gpu_ms_;
    status.generated = generated_;
    status.repeated = repeated_;
    status.failures = failures_;
    status.demoted = demoted_;
    status.captured = captured_;
    status.retry_pending = state_ == FrameGenState::kPaused && automatic_retries_ < retry_attempts_;
    status.multiplier = EffectiveMultiplierLocked();
    status.automatic = settings_.multiplier == FrameGenMultiplier::kAuto;
    status.full_rate = full_rate_ && state_ == FrameGenState::kActive;
    status.threaded = compositor_running_;
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
