#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <Windows.h>

#include "depthxr/openxr_layer.h"
#include "depthxr/turbo_metrics.h"

using namespace std::chrono_literals;

namespace depthxr {

// Test-only access to the singleton's frame-pacing state. The production DLL
// is compiled without DEPTHXR_TESTING, so this peer cannot affect its ABI or
// hot paths.
class TurboFrameTestPeer {
  public:
    enum class Source { kForced, kProbing, kFallback };

    struct HandoffStats {
        bool active{false};
        bool wait_valid{false};
        bool wait_completed{false};
        std::uint64_t armed{0};
        std::uint64_t wait_intercepts{0};
        std::uint64_t begin_intercepts{0};
        std::uint64_t second_poll_blocks{0};
        std::uint64_t cancellations{0};
    };

    static OpenXrLayer& Layer() {
        return OpenXrLayer::Instance();
    }

    static void Prepare(TurboPacingMode mode,
                        PFN_xrWaitFrame wait_frame,
                        PFN_xrBeginFrame begin_frame,
                        PFN_xrEndFrame end_frame) {
        OpenXrLayer& layer = Layer();
        layer.StopTurboAsyncWorker();
        layer.ResetTurboFrameState();

        {
            std::scoped_lock lock(layer.mutex_);
            layer.resolved_settings_ = {};
            layer.resolved_settings_.core.enabled = true;
            layer.resolved_settings_.turbo.enabled = true;
            layer.resolved_settings_.turbo.metrics_mode = TurboMetricsMode::kOff;
            layer.resolved_settings_.turbo.pacing_mode =
                mode == TurboPacingMode::kSequenced ? TurboPacingSetting::kSequenced
                                                    : TurboPacingSetting::kAsync;
            layer.current_exe_name_ = "turbo-frame-tests.exe";
            layer.runtime_name_ = "Fake OpenXR Runtime";
            layer.runtime_version_ = "test";
            layer.active_session_ = XR_NULL_HANDLE;
            layer.quadviews_session_active_ = false;
            layer.varjo_compatible_quadviews_active_ = false;
            layer.quad_views_extension_requested_ = false;
            layer.varjo_foveated_rendering_extension_requested_ = false;
            layer.turbo_toggle_enabled_ = true;
            layer.turbo_runtime_error_streak_.store(0);
            layer.turbo_effective_active_.store(false);
            layer.turbo_recovery_blocked_.store(false);
            layer.turbo_binding_last_poll_time_ = std::chrono::steady_clock::now();
            layer.turbo_binding_down_cached_ = false;
            layer.turbo_pacing_mode_ = mode;
            layer.turbo_pacing_source_ = OpenXrLayer::TurboPacingSource::kForced;
            layer.turbo_pacing_resolved_ = true;
            layer.turbo_pacing_verdict_pending_ = false;
            layer.turbo_cadence_healthy_streak_ = 90;
            layer.turbo_cadence_ready_ = true;
            layer.session_begin_wall_time_ = std::chrono::steady_clock::now() - 10s;
            layer.pacing_last_end_time_.reset();
            layer.app_submitting_layers_.reset();
            layer.next_wait_frame_ = wait_frame;
            layer.next_begin_frame_ = begin_frame;
            layer.next_end_frame_ = end_frame;
            layer.frame_pacing_debug_enabled_.store(false, std::memory_order_relaxed);
        }
        {
            std::scoped_lock lock(layer.turbo_mutex_);
            layer.turbo_last_predicted_display_time_ = 10'000'000'000;
            layer.turbo_last_predicted_display_period_ = 11'111'111;
            layer.turbo_last_should_render_ = true;
            layer.turbo_max_returned_display_time_ = 10'000'000'000;
            layer.turbo_last_wait_frame_wall_time_ = std::chrono::steady_clock::now();
            layer.turbo_frame_begun_ = true;
            layer.turbo_valve_open_ = true;
        }
        layer.turbo_frame_interception_required_.store(true, std::memory_order_release);
    }

    static void Experiment(bool wait_for_submit = false, bool entry = false, int prediction = 100, int limit = 0) {
        auto& layer = Layer();
        layer.turbo_experiment_.enabled = true;
        layer.turbo_experiment_.wait_for_submit = wait_for_submit;
        layer.turbo_experiment_.sample_at_entry = entry;
        layer.turbo_experiment_.prediction_percent = prediction;
        layer.turbo_experiment_.frame_limit = limit;
        layer.turbo_effective_active_.store(true);
        layer.turbo_effective_async_.store(true);
    }

    static XrResult XRAPI_CALL Clock(XrInstance, const LARGE_INTEGER*, XrTime* time) {
        *time = 10'000'000'000;
        return XR_SUCCESS;
    }

    static void PredictionFixture(bool clock) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        std::promise<void> ready;
        ready.set_value();
        layer.turbo_async_wait_ = ready.get_future().share();
        layer.turbo_async_wait_completed_ = true;
        layer.turbo_async_wait_polled_ = false;
        layer.turbo_last_predicted_display_time_ = 10'020'000'000;
        layer.turbo_max_returned_display_time_ = 10'000'000'000;
        layer.turbo_convert_counter_time_ = clock ? reinterpret_cast<PFN_xrVoidFunction>(&Clock) : nullptr;
    }

    static bool IsForced() { return Layer().turbo_pacing_source_ == OpenXrLayer::TurboPacingSource::kForced; }
    static void DisableEffectiveTurbo() { Layer().turbo_effective_active_.store(false); }
    static void Toggle(bool enabled) { Layer().turbo_toggle_enabled_ = enabled; }
    static void MetricsFrame(bool capturing) {
        Layer().RecordTurboMetricsFrame(false, 0.0, false,
            capturing ? TurboMetricsMode::kAlways : TurboMetricsMode::kOff, {}, true, 0);
    }
    static void ResetMetrics() { Layer().ResetTurboMetricsState(); }
    static void WaitMetrics() {
        if (Layer().turbo_metrics_write_future_.valid()) Layer().turbo_metrics_write_future_.wait();
    }
    static void BlockMetricsWriter(std::shared_future<void> gate) {
        auto& layer = Layer();
        auto previous = std::move(layer.turbo_metrics_write_future_);
        layer.turbo_metrics_write_future_ = std::async(std::launch::async,
            [previous = std::move(previous), gate]() mutable {
                if (previous.valid()) previous.wait();
                gate.wait();
            });
    }
    static void FlushMetrics() { Layer().FlushTurboMetrics(false); }
    static bool OsdStateTransitions() {
        auto& layer=Layer();
        layer.resolved_settings_={}; layer.resolved_settings_.core.enabled=true;
        layer.resolved_settings_.turbo.enabled=false;
        layer.turbo_toggle_enabled_=true; layer.turbo_recovery_blocked_=false;
        layer.turbo_auto_suspended_=false; layer.turbo_effective_active_=false;
        bool ok=layer.BuildOsdSnapshot().turbo=="Disabled";
        layer.resolved_settings_.turbo.enabled=true; layer.turbo_toggle_enabled_=false;
        auto snapshot=layer.BuildOsdSnapshot();
        ok=ok && snapshot.turbo=="Enabled / Off" && snapshot.modules.find("Turbo")!=std::string::npos;
        layer.turbo_toggle_enabled_=true; layer.turbo_effective_active_=true; layer.turbo_effective_async_=true;
        ok=ok && layer.BuildOsdSnapshot().turbo=="Async";
        layer.turbo_metrics_active_.store(true);
        ok=ok && layer.BuildOsdSnapshot().turbo=="Async / Analyzing";
        layer.turbo_toggle_enabled_=false;
        ok=ok && layer.BuildOsdSnapshot().turbo=="Enabled / Off / Analyzing";
        layer.turbo_metrics_active_.store(false); layer.turbo_toggle_enabled_=true;

        layer.resolved_settings_.pivotxr.enabled=true;
        PivotXrResolvedProfile profile; profile.name="DCS Stepped";
        layer.resolved_settings_.pivotxr.profiles={profile}; layer.pivotxr_active_profile_index_=0;
        layer.pivotxr_engaged_=true; layer.pivotxr_quick_view_active_=false; layer.pivotxr_quick_view_transitioning_=false;
        layer.pivotxr_manual_view_transition_={}; layer.pivotxr_profile_view_transition_={};
        layer.pivotxr_profile_view_transition_.current.yaw_radians=.1;
        ok=ok && layer.BuildOsdSnapshot().pivot=="DCS Stepped Applied, Nudges Applied";
        layer.pivotxr_engaged_=false; layer.pivotxr_profile_view_transition_={};
        ok=ok && layer.BuildOsdSnapshot().pivot=="Pivot ready";
        layer.pivotxr_engaged_=true; layer.resolved_settings_.pivotxr.enabled=false;
        ok=ok && layer.BuildOsdSnapshot().pivot=="Disabled";
        layer.pivotxr_engaged_=false;
        return ok;
    }
    inline static XrTime osd_locate_time{};
    inline static std::vector<XrSwapchain> osd_releases;
    inline static bool osd_release_failure{};
    static XrResult XRAPI_CALL LocateOsd(XrSpace,XrSpace,XrTime time,XrSpaceLocation* location) {
        osd_locate_time=time;
        location->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        location->pose.orientation.w=1;
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL ReleaseOsd(XrSwapchain swapchain,const XrSwapchainImageReleaseInfo*) {
        osd_releases.push_back(swapchain);
        return osd_release_failure && osd_releases.size()==1?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;
    }
    static bool OsdLatePoseAndRelease() {
        auto& layer=Layer();
        const auto old_locate=layer.next_locate_space_;
        const auto old_release=layer.next_release_swapchain_image_;
        layer.next_locate_space_=&LocateOsd;layer.next_release_swapchain_image_=&ReleaseOsd;
        layer.osd_should_render_=true;
        // Use a later runtime prediction than the rendered frame. No GPU source
        // is supplied: this also exercises setup failure without leaking images.
        layer.turbo_last_predicted_display_time_=30'000'000'000;
        const XrSwapchain left=reinterpret_cast<XrSwapchain>(0x3456),right=reinterpret_cast<XrSwapchain>(0x4567);
        auto queue=[&] {
            OpenXrLayer::PendingOsdComposite pending;
            pending.frame_time=20'000'000'000;pending.swapchains={left,right};
            pending.quad.size={1,1};pending.quad.pose.orientation.w=1;
            pending.quad.pose.position.z=-1;
            layer.pending_osd_composite_=std::move(pending);
            osd_locate_time=0;osd_releases.clear();
        };
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.displayTime=20'000'000'000;frame.layerCount=1;
        bool drawn=false,ok=true;
        queue();
        ok=layer.FinishOsdComposite(&frame,drawn)==XR_SUCCESS && !drawn &&
            osd_locate_time==30'000'000'000 && frame.displayTime==20'000'000'000 &&
            osd_releases==std::vector<XrSwapchain>{left,right} && !layer.pending_osd_composite_;
        layer.ReleasePendingOsdImages();
        ok=ok && osd_releases.size()==2;
        queue();frame.displayTime++;
        ok=ok && layer.FinishOsdComposite(&frame,drawn)==XR_SUCCESS && !drawn &&
            osd_locate_time==0 && osd_releases.size()==2;
        queue();osd_release_failure=true;
        ok=ok && layer.ReleasePendingOsdImages()==XR_ERROR_RUNTIME_FAILURE &&
            osd_releases==std::vector<XrSwapchain>{left,right} && !layer.pending_osd_composite_;
        osd_release_failure=false;
        layer.next_locate_space_=old_locate;layer.next_release_swapchain_image_=old_release;
        return ok;
    }
    static double PredictionSampleAgeMs() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
            *layer.turbo_last_wait_frame_wall_time_).count();
    }
    static bool LimiterReset() { return !Layer().turbo_limit_last_wait_.has_value(); }

    static void Cleanup() {
        OpenXrLayer& layer = Layer();
        layer.StopTurboAsyncWorker();
        layer.ResetTurboFrameState();
    }

    static XrResult ForwardEndFrame(XrSession session, const XrFrameEndInfo* frame_end_info) {
        OpenXrLayer& layer = Layer();
        std::unique_lock lock(layer.mutex_);
        return layer.ForwardEndFrame(session, frame_end_info, lock);
    }

    static HandoffStats Handoff() {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        return {
            layer.turbo_async_handoff_active_,
            layer.turbo_async_wait_.valid(),
            layer.turbo_async_wait_completed_,
            layer.turbo_async_handoff_armed_total_,
            layer.turbo_async_handoff_wait_intercepts_,
            layer.turbo_async_handoff_begin_intercepts_,
            layer.turbo_async_handoff_second_poll_blocks_,
            layer.turbo_async_handoff_cancellations_,
        };
    }

    static int SequencedState() {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        return static_cast<int>(layer.turbo_seq_state_);
    }

    static int EngagingState() {
        return static_cast<int>(OpenXrLayer::TurboSequencedState::kEngaging);
    }

    static void StalePrediction() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        layer.turbo_last_predicted_display_time_ = 10'000'000'000;
        layer.turbo_max_returned_display_time_ = 20'000'000'000;
        layer.turbo_last_wait_frame_wall_time_ = std::chrono::steady_clock::now() + 1h;
    }

    static int ActiveState() {
        return static_cast<int>(OpenXrLayer::TurboSequencedState::kActive);
    }

    static void SetPolicy(TurboPacingMode mode, Source source) {
        OpenXrLayer& layer = Layer();
        layer.turbo_pacing_mode_ = mode;
        switch (source) {
        case Source::kForced:
            layer.turbo_pacing_source_ = OpenXrLayer::TurboPacingSource::kForced;
            break;
        case Source::kProbing:
            layer.turbo_pacing_source_ = OpenXrLayer::TurboPacingSource::kProbing;
            break;
        case Source::kFallback:
            layer.turbo_pacing_source_ = OpenXrLayer::TurboPacingSource::kFallback;
            break;
        }
        layer.turbo_drain_timeout_count_ = 0;
        layer.turbo_timeout_window_start_.reset();
        layer.turbo_pacing_verdict_pending_ = false;
        layer.turbo_auto_suspended_.store(false, std::memory_order_relaxed);
    }

    static TurboPacingMode PacingMode() {
        return Layer().turbo_pacing_mode_;
    }

    static bool HandleTimeout(std::chrono::steady_clock::time_point now) {
        return Layer().HandleTurboDrainTimeout(now);
    }

    static bool AutoSuspended() {
        return Layer().turbo_auto_suspended_.load(std::memory_order_relaxed);
    }

    static void BeforeFirstEngagement() {
        auto& layer = Layer();
        layer.turbo_cadence_ready_ = false;
        layer.turbo_cadence_healthy_streak_ = 0;
        layer.session_begin_wall_time_ = std::chrono::steady_clock::now();
    }

    static void BeginStabilityCheck() {
        auto& layer = Layer();
        layer.turbo_pacing_verdict_pending_ = true;
        layer.turbo_stable_accumulated_ms_ = 59000.0;
        layer.runtime_name_.clear(); // Never write a real pacing sidecar from this test.
    }

    static bool StillProbingAfterAnotherSecond() {
        auto& layer = Layer();
        layer.NoteTurboPacingStableFrame(1000.0);
        return layer.turbo_pacing_verdict_pending_;
    }

    static void ResolveAuto(const std::string& runtime, const std::string& system) {
        auto& layer = Layer();
        layer.runtime_name_ = runtime;
        layer.system_name_ = system;
        layer.resolved_settings_.turbo.pacing_mode = TurboPacingSetting::kAuto;
        layer.resolved_settings_.turbo.runtime_pins.clear();
        layer.ResolveTurboPacingModeLocked();
        layer.runtime_name_.clear();
    }
};

} // namespace depthxr

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

enum class RuntimeCall { kWait, kBegin, kEnd };

class FakeRuntime {
  public:
    FakeRuntime() {
        Expect(active_ == nullptr, "Only one fake runtime may be active at a time");
        active_ = this;
    }

    ~FakeRuntime() {
        ReleaseAll();
        active_ = nullptr;
    }

    FakeRuntime(const FakeRuntime&) = delete;
    FakeRuntime& operator=(const FakeRuntime&) = delete;

    static XrResult XRAPI_CALL WaitFrame(XrSession,
                                         const XrFrameWaitInfo* frame_wait_info,
                                         XrFrameState* frame_state) {
        FakeRuntime& runtime = Active();
        std::unique_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_wait_info && frame_state &&
                                 frame_wait_info->type == XR_TYPE_FRAME_WAIT_INFO &&
                                 frame_state->type == XR_TYPE_FRAME_STATE;
        runtime.calls_.push_back(RuntimeCall::kWait);
        ++runtime.wait_entered_;
        ++runtime.active_waits_;
        runtime.max_active_waits_ = std::max(runtime.max_active_waits_, runtime.active_waits_);
        runtime.cv_.notify_all();
        runtime.cv_.wait(lock, [&runtime] { return runtime.wait_released_; });
        const XrResult result = runtime.wait_result_;
        if (XR_SUCCEEDED(result) && frame_state) {
            ++runtime.wait_tokens_;
            runtime.next_display_time_ += runtime.display_period_;
            frame_state->predictedDisplayTime = runtime.next_display_time_;
            frame_state->predictedDisplayPeriod = runtime.display_period_;
            frame_state->shouldRender = XR_TRUE;
        }
        --runtime.active_waits_;
        ++runtime.wait_exited_;
        runtime.cv_.notify_all();
        return result;
    }

    static XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo* frame_begin_info) {
        FakeRuntime& runtime = Active();
        std::scoped_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_begin_info &&
                                 frame_begin_info->type == XR_TYPE_FRAME_BEGIN_INFO;
        runtime.calls_.push_back(RuntimeCall::kBegin);
        ++runtime.begin_calls_;
        runtime.cv_.notify_all();
        if (runtime.strict_order_) {
            if (!runtime.wait_tokens_ || runtime.frame_open_) return XR_ERROR_CALL_ORDER_INVALID;
            --runtime.wait_tokens_;
            runtime.frame_open_ = true;
        }
        return runtime.begin_result_;
    }

    static XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* frame_end_info) {
        FakeRuntime& runtime = Active();
        std::unique_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_end_info &&
                                 frame_end_info->type == XR_TYPE_FRAME_END_INFO;
        runtime.calls_.push_back(RuntimeCall::kEnd);
        ++runtime.end_entered_;
        if (runtime.strict_order_) {
            if (!runtime.frame_open_) return XR_ERROR_CALL_ORDER_INVALID;
            runtime.frame_open_ = false;
        }
        if (runtime.release_wait_on_next_end_) {
            runtime.release_wait_on_next_end_ = false;
            runtime.wait_released_ = true;
        }
        runtime.cv_.notify_all();
        runtime.cv_.wait(lock, [&runtime] { return runtime.end_released_; });
        ++runtime.end_exited_;
        runtime.cv_.notify_all();
        return runtime.end_result_;
    }

    void BlockEnd() {
        std::scoped_lock lock(mutex_);
        end_released_ = false;
    }

    void StrictOrder() { strict_order_ = true; }

    void ReleaseEnd() {
        std::scoped_lock lock(mutex_);
        end_released_ = true;
        cv_.notify_all();
    }

    void BlockWait() {
        std::scoped_lock lock(mutex_);
        wait_released_ = false;
    }

    void ReleaseWait() {
        std::scoped_lock lock(mutex_);
        wait_released_ = true;
        cv_.notify_all();
    }

    void ReleaseWaitOnNextEnd() {
        std::scoped_lock lock(mutex_);
        release_wait_on_next_end_ = true;
    }

    void ReleaseAll() {
        std::scoped_lock lock(mutex_);
        end_released_ = true;
        wait_released_ = true;
        cv_.notify_all();
    }

    void SetEndResult(XrResult result) {
        std::scoped_lock lock(mutex_);
        end_result_ = result;
    }

    bool WaitForEndEntered(int count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return end_entered_ >= count; });
    }

    bool WaitForWaitEntered(int count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return wait_entered_ >= count; });
    }

    bool WaitForWaitExited(int count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return wait_exited_ >= count; });
    }

    int WaitCalls() const {
        std::scoped_lock lock(mutex_);
        return wait_entered_;
    }

    int BeginCalls() const {
        std::scoped_lock lock(mutex_);
        return begin_calls_;
    }

    int EndCalls() const {
        std::scoped_lock lock(mutex_);
        return end_entered_;
    }

    int MaxConcurrentWaits() const {
        std::scoped_lock lock(mutex_);
        return max_active_waits_;
    }

    bool ValidStructs() const {
        std::scoped_lock lock(mutex_);
        return valid_structs_;
    }

    std::vector<RuntimeCall> Calls() const {
        std::scoped_lock lock(mutex_);
        return calls_;
    }

  private:
    static FakeRuntime& Active() {
        Expect(active_ != nullptr, "Fake runtime callback invoked without an active runtime");
        return *active_;
    }

    inline static FakeRuntime* active_{nullptr};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RuntimeCall> calls_;
    bool end_released_{true};
    bool wait_released_{true};
    bool release_wait_on_next_end_{false};
    bool valid_structs_{true};
    bool strict_order_{false}, frame_open_{true};
    int wait_tokens_{0};
    int wait_entered_{0};
    int wait_exited_{0};
    int begin_calls_{0};
    int end_entered_{0};
    int end_exited_{0};
    int active_waits_{0};
    int max_active_waits_{0};
    XrResult wait_result_{XR_SUCCESS};
    XrResult begin_result_{XR_SUCCESS};
    XrResult end_result_{XR_SUCCESS};
    XrTime next_display_time_{20'000'000'000};
    XrDuration display_period_{11'111'111};
};

class TurboHarness {
  public:
    TurboHarness(FakeRuntime& runtime, depthxr::TurboPacingMode mode) : runtime_(runtime) {
        depthxr::TurboFrameTestPeer::Prepare(mode,
                                             &FakeRuntime::WaitFrame,
                                             &FakeRuntime::BeginFrame,
                                             &FakeRuntime::EndFrame);
    }

    ~TurboHarness() {
        runtime_.ReleaseAll();
        depthxr::TurboFrameTestPeer::Cleanup();
    }

  private:
    FakeRuntime& runtime_;
};

XrSession TestSession() {
    return reinterpret_cast<XrSession>(static_cast<std::uintptr_t>(0x1234));
}

XrFrameEndInfo FrameEndInfo() {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = 20'000'000'000;
    info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return info;
}

XrResult AppWait(XrFrameState* state) {
    const XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    return depthxr::TurboFrameTestPeer::Layer().WaitFrame(TestSession(), &wait_info, state);
}

XrResult AppBegin() {
    const XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    return depthxr::TurboFrameTestPeer::Layer().BeginFrame(TestSession(), &begin_info);
}

bool WaitForSecondPollBlock() {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (depthxr::TurboFrameTestPeer::Handoff().second_poll_blocks > 0) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

void TestAsyncTogglePreservesQueuedFrame(bool experimental) {
    FakeRuntime runtime;
    runtime.StrictOrder();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    if (experimental) depthxr::TurboFrameTestPeer::Experiment(true, true);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Toggle test could not establish async pacing");
    Expect(runtime.WaitForWaitExited(1), "Initial worker wait did not complete");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "First virtual frame failed");
    // A legal pipelined application can wait for N+1 before it submits N.
    // Its Begin(N+1) arrives AFTER Turbo is switched off and N is submitted.
    Expect(AppWait(&state) == XR_SUCCESS, "Queued app wait failed");
    for (int cycle = 0; cycle < 4; ++cycle) {
        Expect(runtime.WaitForWaitExited(1 + cycle * 2), "Previous runtime wait did not complete");
        depthxr::TurboFrameTestPeer::Toggle(false);
        runtime.BlockWait();
        const int before = runtime.WaitCalls();
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
               "Off transition violated runtime frame order");
        // Previously the pipeline was removed here: this forwarded a begin
        // without a completed runtime wait and returned -37 in MSFS and DCS.
        Expect(AppBegin() == XR_SUCCESS, "Queued begin lost its wait at the off transition");
        Expect(runtime.WaitForWaitEntered(before + 1), "Off transition lost runtime pacing ownership");
        auto next_wait = std::async(std::launch::async, [] {
            XrFrameState next{XR_TYPE_FRAME_STATE}; return AppWait(&next);
        });
        Expect(next_wait.wait_for(30ms) == std::future_status::timeout,
               "Turbo off still allowed the app to run ahead of the blocked runtime");
        runtime.ReleaseWait();
        Expect(next_wait.wait_for(2s) == std::future_status::ready && next_wait.get() == XR_SUCCESS,
               "Re-coupled wait did not resume");
        depthxr::TurboFrameTestPeer::Toggle(true);
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
               "On transition violated runtime frame order");
        Expect(AppBegin() == XR_SUCCESS && AppWait(&state) == XR_SUCCESS,
               "Re-enabled pipeline lost its queued frame");
    }
    Expect(runtime.MaxConcurrentWaits() == 1, "Toggle duplicated a runtime wait");
}

void TestMetricsPauseFlushesWithoutAnotherFrame() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto path = std::filesystem::temp_directory_path() /
        ("vectorxr-pause-test-" + std::to_string(GetCurrentProcessId()) + ".json");
    const char* old = std::getenv("VECTORXR_TURBO_METRICS_PATH");
    const std::string previous_path = old ? old : "";
    _putenv_s("VECTORXR_TURBO_METRICS_PATH", path.string().c_str());
    depthxr::TurboFrameTestPeer::ResetMetrics();
    depthxr::TurboFrameTestPeer::MetricsFrame(true);
    depthxr::TurboFrameTestPeer::MetricsFrame(true);
    depthxr::TurboFrameTestPeer::FlushMetrics();
    depthxr::TurboFrameTestPeer::WaitMetrics();
    auto sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(sessions.size() == 1 && sessions[0].live && sessions[0].buckets[0].frames == 1,
           "Initial capture snapshot was not live");
    const auto id = sessions[0].session_id;
    std::promise<void> release;
    depthxr::TurboFrameTestPeer::BlockMetricsWriter(release.get_future().share());
    depthxr::TurboFrameTestPeer::MetricsFrame(true);
    auto paused = std::async(std::launch::async, [] { depthxr::TurboFrameTestPeer::MetricsFrame(false); });
    Expect(paused.wait_for(100ms) == std::future_status::ready,
           "Pausing blocked on a busy metrics writer");
    paused.get();
    release.set_value();
    depthxr::TurboFrameTestPeer::WaitMetrics(); // No subsequent application frame.
    sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(sessions.size() == 1 && !sessions[0].live && sessions[0].buckets[0].frames == 2,
           "Pause lost the tail or left the session live");
    std::this_thread::sleep_for(10ms);
    depthxr::TurboFrameTestPeer::MetricsFrame(true);
    depthxr::TurboFrameTestPeer::WaitMetrics();
    sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(sessions[0].live && sessions[0].session_id == id && sessions[0].buckets[0].frames == 2,
           "Resume changed sessions, counted paused time, or failed to restore live status");
    depthxr::TurboFrameTestPeer::MetricsFrame(true);
    depthxr::TurboFrameTestPeer::MetricsFrame(false);
    depthxr::TurboFrameTestPeer::ResetMetrics();
    sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(!sessions[0].live && sessions[0].buckets[0].frames == 3,
           "Final shutdown was overwritten by a stale capture snapshot");
    std::filesystem::remove(path);
    _putenv_s("VECTORXR_TURBO_METRICS_PATH", previous_path.c_str());
}

void TestAsyncHandoffCoversEndFrameWindow() {
    FakeRuntime runtime;
    runtime.BlockEnd();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const XrFrameEndInfo end_info = FrameEndInfo();

    auto end_result = std::async(std::launch::async, [&] {
        return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    });
    Expect(runtime.WaitForEndEntered(1), "Async establishment never entered runtime xrEndFrame");
    Expect(depthxr::TurboFrameTestPeer::Handoff().active,
           "Async handoff was not armed before runtime xrEndFrame");

    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS, "Handoff-shielded app xrWaitFrame failed");
    Expect(AppBegin() == XR_SUCCESS, "Handoff-shielded app xrBeginFrame failed");
    Expect(runtime.WaitCalls() == 0 && runtime.BeginCalls() == 0,
           "An app frame call slipped through the pre-publication handoff shield");
    Expect(state.predictedDisplayTime > 10'000'000'000 &&
               state.predictedDisplayPeriod == 11'111'111,
           "Fabricated async frame timing was not monotonic and period-correct");

    runtime.ReleaseEnd();
    Expect(end_result.wait_for(2s) == std::future_status::ready && end_result.get() == XR_SUCCESS,
           "Async establishment xrEndFrame did not finish successfully");
    Expect(runtime.WaitForWaitExited(1), "Async worker did not complete its runtime xrWaitFrame");

    const auto handoff = depthxr::TurboFrameTestPeer::Handoff();
    Expect(runtime.EndCalls() == 1 && runtime.WaitCalls() == 1 && runtime.BeginCalls() == 0,
           "Async establishment did not submit exactly one End and one worker Wait");
    Expect(runtime.MaxConcurrentWaits() == 1,
           "Async establishment allowed concurrent downstream xrWaitFrame calls");
    Expect(handoff.armed == 1 && handoff.wait_intercepts == 1 &&
               handoff.begin_intercepts == 1 && handoff.cancellations == 0,
           "Async handoff diagnostics did not describe the protected window");
    Expect(runtime.ValidStructs(), "Layer sent malformed frame structures to the fake runtime");
}

void TestAsyncSecondPollWaitsForPublishedRuntimeWait() {
    FakeRuntime runtime;
    runtime.BlockEnd();
    runtime.BlockWait();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const XrFrameEndInfo end_info = FrameEndInfo();

    auto end_result = std::async(std::launch::async, [&] {
        return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    });
    Expect(runtime.WaitForEndEntered(1), "Second-poll test never entered runtime xrEndFrame");

    XrFrameState first_state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&first_state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "First app poll was not fabricated during async handoff");

    auto second_wait = std::async(std::launch::async, [] {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        return AppWait(&state);
    });
    Expect(WaitForSecondPollBlock(), "Second app poll did not block on handoff publication");
    Expect(second_wait.wait_for(30ms) == std::future_status::timeout,
           "Second app poll returned before the runtime wait was published");

    runtime.ReleaseEnd();
    Expect(runtime.WaitForWaitEntered(1), "Published async worker wait never reached the runtime");
    Expect(second_wait.wait_for(30ms) == std::future_status::timeout,
           "Second app poll returned before the published runtime wait completed");
    runtime.ReleaseWait();

    Expect(end_result.wait_for(2s) == std::future_status::ready && end_result.get() == XR_SUCCESS,
           "Second-poll test xrEndFrame did not complete");
    Expect(second_wait.wait_for(2s) == std::future_status::ready && second_wait.get() == XR_SUCCESS,
           "Second app poll did not resume after the runtime wait completed");
    const auto handoff = depthxr::TurboFrameTestPeer::Handoff();
    Expect(handoff.second_poll_blocks == 1 && runtime.WaitCalls() == 1 &&
               runtime.MaxConcurrentWaits() == 1,
           "Second-poll guard did not preserve the one-runtime-wait invariant");
}

void TestAsyncSubmitFailureCancelsHandoff() {
    FakeRuntime runtime;
    runtime.BlockEnd();
    runtime.SetEndResult(XR_ERROR_RUNTIME_FAILURE);
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const XrFrameEndInfo end_info = FrameEndInfo();

    auto end_result = std::async(std::launch::async, [&] {
        return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    });
    Expect(runtime.WaitForEndEntered(1), "Failure test never entered runtime xrEndFrame");
    XrFrameState first_state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&first_state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "Failure test did not intercept the first app frame calls");

    auto second_wait = std::async(std::launch::async, [] {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        return AppWait(&state);
    });
    Expect(WaitForSecondPollBlock(), "Failure test's second poll never waited for publication");
    runtime.ReleaseEnd();

    Expect(end_result.wait_for(2s) == std::future_status::ready &&
               end_result.get() == XR_ERROR_RUNTIME_FAILURE,
           "Runtime xrEndFrame failure was not returned to the application");
    Expect(second_wait.wait_for(2s) == std::future_status::ready && second_wait.get() == XR_SUCCESS,
           "Cancelled handoff did not return the second app poll to runtime pacing");
    const auto handoff = depthxr::TurboFrameTestPeer::Handoff();
    Expect(!handoff.active && !handoff.wait_valid && handoff.cancellations == 1,
           "Failed submit did not cancel the unpublished async handoff exactly once");
    Expect(runtime.WaitCalls() == 1 && runtime.MaxConcurrentWaits() == 1,
           "Cancelled handoff launched a worker wait or duplicated the app's runtime wait");
}

void TestSequencedHandshakeAndSteadyStateOrdering() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    const XrFrameEndInfo end_info = FrameEndInfo();

    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Sequenced establishment submit failed");
    Expect(depthxr::TurboFrameTestPeer::SequencedState() ==
               depthxr::TurboFrameTestPeer::EngagingState(),
           "Sequenced mode did not enter its app-wait handshake");

    XrFrameState handshake_state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&handshake_state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "Sequenced establishment did not pass the app's wait/begin through");
    Expect(depthxr::TurboFrameTestPeer::SequencedState() ==
               depthxr::TurboFrameTestPeer::ActiveState(),
           "Sequenced handshake did not activate the persistent interception shield");

    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Sequenced steady-state submit failed");
    Expect(runtime.EndCalls() == 2 && runtime.WaitCalls() == 2 && runtime.BeginCalls() == 2,
           "Sequenced runtime did not receive one wait/begin pair after each applicable submit");

    const int waits_before_app_poll = runtime.WaitCalls();
    const int begins_before_app_poll = runtime.BeginCalls();
    XrFrameState fabricated_state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&fabricated_state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "Sequenced steady-state app frame calls were not fabricated");
    Expect(runtime.WaitCalls() == waits_before_app_poll &&
               runtime.BeginCalls() == begins_before_app_poll,
           "Sequenced steady-state app calls escaped the interception shield");

    const std::vector<RuntimeCall> expected{
        RuntimeCall::kEnd,
        RuntimeCall::kWait,
        RuntimeCall::kBegin,
        RuntimeCall::kEnd,
        RuntimeCall::kWait,
        RuntimeCall::kBegin,
    };
    Expect(runtime.Calls() == expected,
           "Sequenced runtime ordering was not End -> Wait -> Begin in steady state");
    Expect(runtime.MaxConcurrentWaits() == 1 && runtime.ValidStructs(),
           "Sequenced mode duplicated a runtime wait or sent malformed frame structures");
    depthxr::TurboFrameTestPeer::StalePrediction();
    XrFrameState stale_state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&stale_state) == XR_SUCCESS && stale_state.predictedDisplayTime == 20'000'000'001,
           "Cached prediction should use the minimum monotonic correction, not add a refresh period");
    Expect(stale_state.predictedDisplayPeriod == 11'111'111 && stale_state.shouldRender == XR_TRUE,
           "Prediction correction must retain runtime period and shouldRender");
}

void TestRepeatedSubmissionFailuresSuspend() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    runtime.SetEndResult(XR_ERROR_TIME_INVALID);
    for (int i = 0; i < 3; ++i) {
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_ERROR_TIME_INVALID,
               "Submission errors must be returned to the app");
        Expect(depthxr::TurboFrameTestPeer::AutoSuspended() == (i == 2),
               "Submission breaker must trip at three consecutive errors");
    }
    Expect(!depthxr::TurboFrameTestPeer::Handoff().wait_valid && runtime.WaitCalls() == 0,
           "Failed submissions must not start an async worker wait");
}

void TestStartupFailuresDoNotQuarantineTurbo() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::BeforeFirstEngagement();
    runtime.SetEndResult(XR_ERROR_TIME_INVALID);
    const auto end_info = FrameEndInfo();
    for (int i = 0; i < 3; ++i) {
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_ERROR_TIME_INVALID,
               "Startup submission errors must still reach the app");
    }
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended(),
           "Startup errors before Turbo engages must not create a Turbo safety failure");
    Expect(runtime.WaitCalls() == 0, "Startup test must not have started a Turbo pipeline");
}

void TestSubmissionFailureRestartsStabilityWindow() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::BeginStabilityCheck();
    runtime.SetEndResult(XR_ERROR_TIME_INVALID);
    const auto end_info = FrameEndInfo();
    depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    Expect(depthxr::TurboFrameTestPeer::StillProbingAfterAnotherSecond(),
           "A failed submission must restart the 60-second stability window");
}

void TestAutoSubmissionErrorsTryBothModes() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::SetPolicy(depthxr::TurboPacingMode::kAsync, depthxr::TurboFrameTestPeer::Source::kProbing);
    const auto end_info = FrameEndInfo();
    runtime.SetEndResult(XR_ERROR_TIME_INVALID);
    for (int i = 0; i < 3; ++i)
        depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended(), "Auto must try Sequenced after Async submission errors");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced, "Auto failed to switch strategy");
    Expect(depthxr::TurboFrameTestPeer::SequencedState() == depthxr::TurboFrameTestPeer::EngagingState(),
           "Rejected Async submissions must still establish the Sequenced handshake");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && AppBegin() == XR_SUCCESS, "Sequenced retry handshake failed");
    for (int i = 0; i < 3; ++i) {
        depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
        Expect(depthxr::TurboFrameTestPeer::AutoSuspended() == (i == 2), "Sequenced must get its own three-error budget");
    }
}

void TestAutoHasNoRuntimeMappings() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    for (const auto* name : {"Pimax OpenXR", "SteamVR/OpenXR", "Oculus", "Varjo", "Unlisted runtime"}) {
        depthxr::TurboFrameTestPeer::ResolveAuto(name, "test-only aapvr Crystal headset");
        Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kAsync,
               "Every untested setup must start Async regardless of runtime/headset name");
    }
}

void TestExperimentalSubmitGate(bool release_before_timeout, bool failed_submit = false, bool sample_at_entry = false) {
    FakeRuntime runtime;
    runtime.BlockEnd();
    if (failed_submit) runtime.SetEndResult(XR_ERROR_RUNTIME_FAILURE);
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::Experiment(true, sample_at_entry);
    const auto end_info = FrameEndInfo();
    auto end = std::async(std::launch::async, [&] {
        return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    });
    Expect(runtime.WaitForEndEntered(1), "Experimental submit did not reach runtime");
    auto wait = std::async(std::launch::async, [] {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        return AppWait(&state);
    });
    Expect(wait.wait_for(10ms) == std::future_status::timeout,
           "Experimental app wait did not wait behind submission");
    if (release_before_timeout) runtime.ReleaseEnd();
    Expect(wait.wait_for(2s) == std::future_status::ready && wait.get() == XR_SUCCESS,
           "Experimental app wait deadlocked or failed");
    if (!release_before_timeout) {
        if (sample_at_entry) Expect(depthxr::TurboFrameTestPeer::PredictionSampleAgeMs() >= 40,
                                   "Entry sampling accidentally included the submission gate wait");
        Expect(runtime.WaitCalls() == 0, "Gate timeout bypassed the handoff and duplicated a real wait");
        runtime.ReleaseEnd();
    }
    Expect(end.wait_for(2s) == std::future_status::ready &&
           end.get() == (failed_submit ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS), "Experimental submit failed");
    Expect(runtime.WaitForWaitExited(1) && runtime.MaxConcurrentWaits() == 1,
           "Experimental gate allowed duplicate runtime waits");
}

void TestExperimentalPredictionAndLimiter() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::Experiment(false, false, 50);
    depthxr::TurboFrameTestPeer::PredictionFixture(true);
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && state.predictedDisplayTime == 10'010'000'000,
           "Prediction dampening did not use runtime clock horizon");
    Expect(state.predictedDisplayPeriod == 11'111'111 && state.shouldRender == XR_TRUE,
           "Prediction dampening altered runtime period or visibility");
    const auto previous = state.predictedDisplayTime;
    Expect(AppWait(&state) == XR_SUCCESS && state.predictedDisplayTime == previous + 1,
           "Dampening broke monotonic predictions");
    depthxr::TurboFrameTestPeer::PredictionFixture(false);
    Expect(AppWait(&state) == XR_SUCCESS && state.predictedDisplayTime == 10'020'000'000,
           "Missing clock conversion must preserve runtime prediction");
    depthxr::TurboFrameTestPeer::Experiment(false, false, 100, 20);
    Expect(AppWait(&state) == XR_SUCCESS, "Limiter first wait failed");
    const auto start = std::chrono::steady_clock::now();
    Expect(AppWait(&state) == XR_SUCCESS && std::chrono::steady_clock::now() - start >= 40ms,
           "Experimental cap did not limit app frame cadence");
    depthxr::TurboFrameTestPeer::DisableEffectiveTurbo();
    Expect(AppWait(&state) == XR_SUCCESS, "Turbo-off wait with configured limiter failed");
    Expect(depthxr::TurboFrameTestPeer::LimiterReset(), "Experimental limiter remained armed after Turbo was disabled");
    depthxr::TurboFrameTestPeer::ResolveAuto("Experiment test", "Test headset");
    Expect(depthxr::TurboFrameTestPeer::IsForced() &&
           depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kAsync,
           "Experimental session must bypass Auto discovery");
}

void TestSubmissionInterlockFallsBackThenSuspends() {
    FakeRuntime runtime;
    runtime.BlockWait();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::SetPolicy(depthxr::TurboPacingMode::kAsync,
                                          depthxr::TurboFrameTestPeer::Source::kProbing);
    const XrFrameEndInfo end_info = FrameEndInfo();

    // Establish an async worker wait that behaves like a runtime whose wait is
    // released only by the following submit (PiOpenXR/Oculus/Varjo profile).
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Interlock test could not establish async pacing");
    Expect(runtime.WaitForWaitEntered(1), "Interlock profile never received its first worker wait");
    runtime.ReleaseWaitOnNextEnd();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "First interlocked async submit failed");
    Expect(runtime.WaitForWaitExited(1), "First interlocked wait did not release on submit");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kAsync,
           "Auto pacing abandoned async before its probing threshold");

    // Retire the completed future and launch the second blocked worker wait.
    runtime.BlockWait();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Interlock test could not launch its second worker wait");
    Expect(runtime.WaitForWaitEntered(2), "Interlock profile never received its second worker wait");
    runtime.ReleaseWaitOnNextEnd();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Second interlocked async submit failed");
    Expect(runtime.WaitForWaitExited(2), "Second interlocked wait did not release on submit");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced,
           "Auto pacing did not fall back to sequenced after two probing stalls");

    // The next submit retires the final async future and establishes the
    // sequenced handshake; this verifies the two strategies compose cleanly.
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Async-to-sequenced transition submit failed");
    Expect(depthxr::TurboFrameTestPeer::SequencedState() ==
               depthxr::TurboFrameTestPeer::EngagingState(),
           "Async-to-sequenced transition did not enter the handshake state");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && AppBegin() == XR_SUCCESS,
           "Fallback sequenced handshake failed");

    // Level two of the policy suspends Turbo when even post-submit sequenced
    // waits repeatedly stall. Drive the policy clock directly so the test is
    // fast and independent of host scheduling.
    depthxr::TurboFrameTestPeer::SetPolicy(depthxr::TurboPacingMode::kSequenced,
                                          depthxr::TurboFrameTestPeer::Source::kFallback);
    const auto now = std::chrono::steady_clock::now();
    Expect(!depthxr::TurboFrameTestPeer::HandleTimeout(now),
           "Sequenced pacing suspended on its first stall");
    Expect(!depthxr::TurboFrameTestPeer::HandleTimeout(now + 1ms),
           "Sequenced pacing suspended before its three-stall threshold");
    Expect(depthxr::TurboFrameTestPeer::HandleTimeout(now + 2ms) &&
               depthxr::TurboFrameTestPeer::AutoSuspended(),
           "Sequenced pacing did not auto-suspend at its safety threshold");
    Expect(runtime.MaxConcurrentWaits() == 1,
           "Interlocked runtime profile observed concurrent downstream waits");
}

} // namespace

int main() {
    TestAsyncTogglePreservesQueuedFrame(false);
    TestAsyncTogglePreservesQueuedFrame(true);
    TestMetricsPauseFlushesWithoutAnotherFrame();
    TestExperimentalSubmitGate(true);
    TestExperimentalSubmitGate(false);
    TestExperimentalSubmitGate(false, false, true);
    TestExperimentalSubmitGate(true, true);
    TestExperimentalPredictionAndLimiter();
    TestStartupFailuresDoNotQuarantineTurbo();
    TestSubmissionFailureRestartsStabilityWindow();
    TestAsyncHandoffCoversEndFrameWindow();
    TestAsyncSecondPollWaitsForPublishedRuntimeWait();
    TestAsyncSubmitFailureCancelsHandoff();
    TestSequencedHandshakeAndSteadyStateOrdering();
    TestRepeatedSubmissionFailuresSuspend();
    TestAutoSubmissionErrorsTryBothModes();
    TestAutoHasNoRuntimeMappings();
    TestSubmissionInterlockFallsBackThenSuspends();
    Expect(depthxr::TurboFrameTestPeer::OsdStateTransitions(), "OSD lost enabled/off Turbo or applied/ready/disabled Pivot state");
    Expect(depthxr::TurboFrameTestPeer::OsdLatePoseAndRelease(), "OSD late prediction or deferred-image cleanup failed");
    std::cout << "depthxr_turbo_frame_tests passed\n";
    return 0;
}
