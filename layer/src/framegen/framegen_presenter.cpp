#include <windows.h>

#include "depthxr/framegen_presenter.h"

#include "depthxr/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace depthxr {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kOverlapWait{250};
constexpr XrDuration kOutputWaitTimeout = 100'000'000; // 100 ms
constexpr std::uint32_t kFailuresBeforePause = 3;

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

FrameGenPresenter::~FrameGenPresenter() { Stop(); }

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
    state_ = FrameGenState::kWaiting;
    reason_.clear();
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
    // Finish outstanding GPU work before the runtime frees the output
    // images it writes. Teardown runs between frames, so no EndFrame touches
    // the outputs concurrently.
    if (gpu_) {
        gpu_->Reset();
    }
    lock.unlock();
    DestroyOutputs();
    lock.lock();
    gpu_.reset();
    if (timer_) {
        CloseHandle(static_cast<HANDLE>(timer_));
        timer_ = nullptr;
    }
    if (logger_ && (generated_ || repeated_ || failures_)) {
        logger_->Info("Frame Generation session summary: generated=" + std::to_string(generated_) +
                      ", repeated=" + std::to_string(repeated_) + ", failures=" + std::to_string(failures_) + ".");
    }
    started_ = false;
    state_ = FrameGenState::kOff;
    reason_.clear();
    swapchains_.clear();
    runtime_formats_.clear();
    frames_.clear();
    last_real_ = {};
    extrapolated_ = {};
    consecutive_failures_ = 0;
    generated_ = repeated_ = failures_ = 0;
    app_frames_.clear();
    submissions_.clear();
    logged_first_generation_ = false;
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

void FrameGenPresenter::SetEngagementPolicy(std::uint32_t steady_frames, std::chrono::milliseconds minimum_session_age) {
    std::scoped_lock lock(mutex_);
    engage_streak_frames_ = steady_frames;
    engage_min_age_ = minimum_session_age;
}

bool FrameGenPresenter::Toggle() {
    std::scoped_lock lock(mutex_);
    if (state_ == FrameGenState::kPaused) {
        // A press after a pause retries rather than turning the feature off.
        user_enabled_ = true;
        consecutive_failures_ = 0;
        SetStateLocked(FrameGenState::kWaiting, "");
    } else {
        user_enabled_ = !user_enabled_;
        if (state_ != FrameGenState::kUnavailable) {
            SetStateLocked(user_enabled_ ? FrameGenState::kWaiting : FrameGenState::kOff, "");
        }
    }
    healthy_streak_ = 0;
    return user_enabled_;
}

XrSwapchainUsageFlags FrameGenPresenter::SourceUsage(const XrSwapchainCreateInfo& info) const {
    std::scoped_lock lock(mutex_);
    if (!started_ || !gpu_ || (info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        return 0;
    }
    return gpu_->RequiredSourceUsage();
}

void FrameGenPresenter::TrackSwapchain(XrSwapchain swapchain, std::int64_t format, std::uint32_t sample_count) {
    std::scoped_lock lock(mutex_);
    if (!started_) {
        return;
    }
    swapchains_[swapchain] = {format, std::max(1u, sample_count), std::nullopt};
}

void FrameGenPresenter::ForgetSwapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(mutex_);
    swapchains_.erase(swapchain);
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

bool FrameGenPresenter::WantsEngagementLocked() const {
    return started_ && gpu_ && user_enabled_ && state_ != FrameGenState::kPaused &&
           state_ != FrameGenState::kUnavailable && healthy_streak_ >= engage_streak_frames_ &&
           Clock::now() - started_at_ >= engage_min_age_;
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
    if (state == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation engaged: ") + ToString(settings_.synthesis_mode) +
                      ", application held at half the runtime refresh rate.");
    } else if (state == FrameGenState::kPaused || state == FrameGenState::kUnavailable) {
        logger_->Error(std::string("Frame Generation ") + ToString(state) + ": " + reason + ".");
    } else if (previous == FrameGenState::kActive) {
        logger_->Info(std::string("Frame Generation disengaged (") + ToString(state) + ").");
    }
}

void FrameGenPresenter::PauseLocked(const std::string& reason) {
    SetStateLocked(FrameGenState::kPaused, reason);
    healthy_streak_ = 0;
}

void FrameGenPresenter::UnavailableLocked(const std::string& reason) {
    SetStateLocked(FrameGenState::kUnavailable, reason);
}

void FrameGenPresenter::NoteCadenceLocked(bool renderable) {
    healthy_streak_ = renderable ? healthy_streak_ + 1 : 0;
}

XrResult FrameGenPresenter::WaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state,
                                      const FrameGenRuntimeCalls& calls) {
    bool engage = false;
    std::uint64_t sequence = 0;
    {
        std::unique_lock lock(mutex_);
        engage = WantsEngagementLocked();
        // An engaged frame in progress owns the runtime until its extra
        // wait/begin/end has finished; two outstanding waits, or a begin
        // between our wait and end, would break frame ordering. An engaging
        // wait also lets the previous frame finish so pairs never overlap.
        const std::uint64_t previous = wait_sequence_;
        auto blocking = [&] {
            return std::any_of(frames_.begin(), frames_.end(), [&](const PendingFrame& frame) {
                return frame.engaged || (engage && frame.sequence == previous);
            });
        };
        if (blocking() && !idle_cv_.wait_for(lock, kOverlapWait, [&] { return !blocking(); })) {
            PauseLocked("the application starts a frame before the previous one ends");
            engage = false;
            // Still blocked: the stuck frame keeps its runtime frame. Waiting
            // further could deadlock an app that ends frames on this thread.
        }
        if (!engage && state_ == FrameGenState::kActive) {
            SetStateLocked(FrameGenState::kWaiting, "");
        }
        sequence = ++wait_sequence_;
    }
    const XrResult result = calls.wait(session, info, state);
    if (XR_FAILED(result) || !state) {
        return result;
    }
    std::scoped_lock lock(mutex_);
    PendingFrame frame;
    frame.slot_time = state->predictedDisplayTime;
    frame.period = state->predictedDisplayPeriod;
    frame.should_render = state->shouldRender == XR_TRUE;
    frame.engaged = engage && frame.period > 0 && state_ != FrameGenState::kPaused;
    frame.sequence = sequence;
    frame.wait_return = Clock::now();
    if (frame.engaged) {
        state->predictedDisplayTime = frame.slot_time + frame.period;
        state->predictedDisplayPeriod = frame.period * 2;
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
    {
        std::scoped_lock lock(mutex_);
        auto it = std::find_if(frames_.begin(), frames_.end(),
                               [&](const PendingFrame& pending) { return pending.app_time == info->displayTime; });
        if (it == frames_.end() && !frames_.empty()) {
            it = frames_.begin();
        }
        if (it != frames_.end()) {
            frame = *it;
        }
    }
    const bool renderable = info->layerCount > 0 && FindProjection(*info) != nullptr;
    XrResult result = XR_SUCCESS;
    if (!frame || !frame->engaged) {
        result = calls.submit(session, info);
    } else if (!frame->should_render || !renderable) {
        // Engaged but nothing to generate from: present in the slot we hold.
        result = SubmitAt(session, *info, frame->slot_time, calls);
        std::scoped_lock lock(mutex_);
        last_real_.valid = false;
        extrapolated_.valid = false;
    } else {
        result = RunPair(session, *info, *frame, calls);
    }
    {
        std::scoped_lock lock(mutex_);
        if (frame) {
            frames_.erase(std::remove_if(frames_.begin(), frames_.end(),
                                         [&](const PendingFrame& pending) { return pending.sequence == frame->sequence; }),
                          frames_.end());
        }
        NoteCadenceLocked(renderable && (!frame || frame->should_render));
        const auto now = Clock::now();
        app_frames_.push_back(now);
        if (!frame || !frame->engaged) {
            NoteSubmissionLocked();
        }
        if (logger_ && state_ == FrameGenState::kActive && now - last_summary_ >= std::chrono::seconds(60)) {
            last_summary_ = now;
            logger_->Info("Frame Generation: generated=" + std::to_string(generated_) + ", repeated=" +
                          std::to_string(repeated_) + ", failures=" + std::to_string(failures_) + ", gpuMs=" +
                          Number(gpu_ ? gpu_->LastGpuMilliseconds() : -1.0, 2) + ".");
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

XrResult FrameGenPresenter::SubmitSecond(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                                         const FrameGenRuntimeCalls& calls) {
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    XrResult result = calls.wait(session, &wait_info, &state);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrWaitFrame failed for the real frame (" + std::to_string(result) + ")");
        return result;
    }
    // Some runtimes return xrWaitFrame immediately instead of pacing it. Keep
    // the two submissions at least most of a refresh apart so the compositor
    // does not receive both for one slot and drop the generated frame.
    SleepUntil(timer_, frame.wait_return + std::chrono::nanoseconds(frame.period * 3 / 4));
    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    result = calls.begin(session, &begin_info);
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime xrBeginFrame failed for the real frame (" + std::to_string(result) + ")");
        return result;
    }
    XrFrameEndInfo real = info;
    real.displayTime = state.predictedDisplayTime;
    if (state.shouldRender != XR_TRUE) {
        real.layerCount = 0;
        real.layers = nullptr;
    }
    result = calls.submit(session, &real);
    std::scoped_lock lock(mutex_);
    NoteSubmissionLocked();
    if (XR_FAILED(result)) {
        PauseLocked("runtime rejected the real frame (" + std::to_string(result) + ")");
    }
    return result;
}

XrResult FrameGenPresenter::RunPair(XrSession session, const XrFrameEndInfo& info, const PendingFrame& frame,
                                    const FrameGenRuntimeCalls& calls) {
    std::uint32_t projection_index = 0;
    const XrCompositionLayerProjection& projection = *FindProjection(info, &projection_index);
    const XrTime newest_time = frame.slot_time + frame.period;
    FrameGenSynthesisMode mode;
    bool continuous;
    XrTime previous_time = 0;
    {
        std::scoped_lock lock(mutex_);
        mode = settings_.synthesis_mode;
        previous_time = last_real_.display_time;
        continuous = last_real_.valid && last_real_.sequence + 1 == frame.sequence &&
                     newest_time > previous_time && newest_time - previous_time <= 4 * frame.period;
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
        XrFrameEndInfo generated = info;
        generated.layers = layers.data();
        return generated;
    };

    XrResult result = XR_SUCCESS;
    if (mode == FrameGenSynthesisMode::kInterpolate) {
        std::vector<XrCompositionLayerProjectionView> views;
        const float fraction = continuous ? FrameGenFraction(mode, previous_time, newest_time, frame.slot_time) : 1.0f;
        bool synthesized = false;
        if (Generate(projection, frame, fraction, views, synthesized)) {
            result = SubmitAt(session, with_views(views, projection.space, projection.layerFlags), frame.slot_time, calls);
        } else {
            result = SubmitAt(session, info, frame.slot_time, calls);
        }
    } else {
        // Slot k shows the previous real frame pushed forward, generated during
        // the previous app frame; the newest frame's own extrapolation is
        // generated next and shown one app frame later.
        if (extrapolated_.valid && continuous && extrapolated_.views.size() == projection.viewCount) {
            result = SubmitAt(session, with_views(extrapolated_.views, extrapolated_.space, extrapolated_.flags),
                              frame.slot_time, calls);
        } else {
            result = SubmitAt(session, info, frame.slot_time, calls);
        }
        if (XR_SUCCEEDED(result)) {
            const float fraction = continuous
                ? FrameGenFraction(mode, previous_time, newest_time, newest_time + frame.period) : 0.0f;
            bool synthesized = false;
            extrapolated_.valid = Generate(projection, frame, fraction, extrapolated_.views, synthesized);
            extrapolated_.space = projection.space;
            extrapolated_.flags = projection.layerFlags;
        }
    }
    if (XR_FAILED(result)) {
        std::scoped_lock lock(mutex_);
        PauseLocked("runtime rejected the generated frame (" + std::to_string(result) + ")");
        last_real_.valid = false;
        extrapolated_.valid = false;
        return result;
    }
    RecordReal(projection, newest_time, frame.sequence);
    return SubmitSecond(session, info, frame, calls);
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
                                 float fraction, std::vector<XrCompositionLayerProjectionView>& views,
                                 bool& synthesized) {
    synthesized = false;
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
        tuning.fraction = fraction;
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
            submission.request.output = *kind;
            submission.request.newest = Geometry(view);
            submission.request.synthesize = continuous && last_real_.views[i].extent.width == submission.request.extent.width &&
                                            last_real_.views[i].extent.height == submission.request.extent.height;
            submission.request.previous = submission.request.synthesize ? last_real_.views[i].geometry : Geometry(view);
            submission.request.temporal = true;
        }
    }
    if (!error.empty() || !EnsureOutputs(projection, kinds, error)) {
        std::scoped_lock lock(mutex_);
        ++failures_;
        if (++consecutive_failures_ >= kFailuresBeforePause) {
            PauseLocked(error);
        }
        return false;
    }

    // Acquire every output image before writing any of them.
    bool acquired = true;
    for (std::size_t i = 0; i < outputs_.size() && acquired; ++i) {
        Output& output = outputs_[i];
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xr_.acquire(output.swapchain, &acquire, &output.index))) {
            error = "output image acquire failed";
            acquired = false;
            break;
        }
        output.acquired = true;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = kOutputWaitTimeout;
        const XrResult waited = xr_.wait(output.swapchain, &wait);
        if (waited != XR_SUCCESS) {
            // Never write or release an image the runtime has not handed over.
            error = "output image wait result=" + std::to_string(waited);
            acquired = false;
        }
    }
    bool ok = acquired;
    if (ok) {
        std::scoped_lock lock(mutex_);
        for (std::size_t i = 0; i < submissions.size() && ok; ++i) {
            const Output& output = outputs_[i];
            submissions[i].output = {output.swapchain, output.index, 0,
                                     {{0, 0}, {static_cast<std::int32_t>(output.extent.width),
                                               static_cast<std::int32_t>(output.extent.height)}},
                                     output.format};
            ok = gpu_->Track(submissions[i].source.swapchain, xr_.images, error) &&
                 gpu_->Track(output.swapchain, xr_.images, error);
        }
    }
    FrameGenExecuteResult executed;
    if (ok) {
        // Vulkan queues need external synchronization with the runtime's
        // own submissions on the session queue.
        std::unique_lock<std::mutex> queue_lock;
        if (queue_mutex_) queue_lock = std::unique_lock<std::mutex>(*queue_mutex_);
        std::scoped_lock lock(mutex_);
        ok = gpu_->Execute(submissions, tuning, executed, error);
    }
    if (acquired || ok) {
        ReleaseOutputs();
    } else {
        // A wait that timed out keeps its acquisition; release only images
        // the runtime handed over. The next frame retries from a clean slate.
        for (Output& output : outputs_) {
            if (output.acquired) {
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xr_.release(output.swapchain, &release);
                output.acquired = false;
            }
        }
    }

    std::scoped_lock lock(mutex_);
    if (!ok) {
        ++failures_;
        if (++consecutive_failures_ >= kFailuresBeforePause) {
            PauseLocked("generation failed: " + error);
        } else if (logger_) {
            logger_->Error("Frame Generation could not generate a frame: " + error);
        }
        return false;
    }
    consecutive_failures_ = 0;
    synthesized = std::any_of(executed.synthesized.begin(), executed.synthesized.end(), [](bool value) { return value; });
    if (synthesized) {
        ++generated_;
    } else {
        ++repeated_;
    }
    views.assign(projection.views, projection.views + projection.viewCount);
    for (std::size_t i = 0; i < views.size(); ++i) {
        views[i].next = nullptr; // a generated image has no matching depth
        views[i].subImage.swapchain = outputs_[i].swapchain;
        views[i].subImage.imageRect = submissions[i].output.rect;
        views[i].subImage.imageArrayIndex = 0;
    }
    if (logger_ && synthesized && !logged_first_generation_) {
        logged_first_generation_ = true;
        std::string extents;
        for (const auto& submission : submissions) {
            extents += (extents.empty() ? "" : ",") + std::to_string(submission.request.extent.width) + "x" +
                       std::to_string(submission.request.extent.height);
        }
        logger_->Info("Frame Generation produced its first frame: views=" + std::to_string(views.size()) + " (" + extents +
                      "), backend=" + gpu_->Name() + ", fraction=" + Number(fraction, 3) + ".");
    }
    return true;
}

bool FrameGenPresenter::EnsureOutputs(const XrCompositionLayerProjection& projection,
                                      std::vector<FrameGenOutputKind>& kinds, std::string& error) {
    if (outputs_.size() != projection.viewCount) {
        DestroyOutputs();
        outputs_.resize(projection.viewCount);
    }
    for (std::uint32_t i = 0; i < projection.viewCount; ++i) {
        Output& output = outputs_[i];
        const FrameGenExtent extent = Extent(projection.views[i]);
        if (output.swapchain && output.extent.width == extent.width && output.extent.height == extent.height &&
            output.kind == kinds[i]) {
            continue;
        }
        if (output.swapchain) {
            ForgetSwapchain(output.swapchain);
            xr_.destroy(output.swapchain);
            output = {};
        }
        std::vector<std::int64_t> candidates;
        {
            std::scoped_lock lock(mutex_);
            candidates = gpu_->OutputFormats(kinds[i]);
            if (kinds[i] != FrameGenOutputKind::kSrgb8) {
                for (auto format : gpu_->OutputFormats(FrameGenOutputKind::kSrgb8)) candidates.push_back(format);
            }
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
        // An sRGB fallback for a UNORM or float source keeps the encode honest.
        {
            std::scoped_lock lock(mutex_);
            const auto chosen_kind = gpu_->Classify(format);
            if (chosen_kind) kinds[i] = *chosen_kind;
        }
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        {
            std::scoped_lock lock(mutex_);
            create.usageFlags = gpu_->OutputUsage();
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
        output.extent = extent;
        output.format = format;
        output.kind = kinds[i];
        {
            std::scoped_lock lock(mutex_);
            extrapolated_.valid = false;
        }
    }
    return true;
}

void FrameGenPresenter::ReleaseOutputs() {
    for (Output& output : outputs_) {
        if (!output.acquired) {
            continue;
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr_.release(output.swapchain, &release);
        output.acquired = false;
    }
}

void FrameGenPresenter::DestroyOutputs() {
    ReleaseOutputs();
    for (Output& output : outputs_) {
        if (output.swapchain) {
            ForgetSwapchain(output.swapchain);
            if (xr_.destroy) xr_.destroy(output.swapchain);
        }
    }
    outputs_.clear();
    std::scoped_lock lock(mutex_);
    extrapolated_ = {};
}

void FrameGenPresenter::NoteSubmissionLocked() {
    submissions_.push_back(Clock::now());
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
    return status;
}

std::string FrameGenPresenter::OsdText() const {
    const FrameGenStatus status = Status();
    switch (status.state) {
    case FrameGenState::kActive: {
        std::string text = std::string(status.mode == FrameGenSynthesisMode::kExtrapolate ? "Extrapolate" : "Interpolate") +
                           "  /  " + Number(status.app_fps, 0) + " -> " + Number(status.output_fps, 0) + " fps";
        if (status.gpu_ms > 0.0) text += "  /  " + Number(status.gpu_ms, 1) + " ms";
        return text;
    }
    case FrameGenState::kWaiting:
        return "Waiting for steady frames";
    case FrameGenState::kPaused:
        return "Paused  /  " + status.reason;
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
        return Number(status.app_fps, 0) + " -> " + Number(status.output_fps, 0) + " fps";
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
