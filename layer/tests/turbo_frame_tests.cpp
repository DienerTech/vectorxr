#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "depthxr/openxr_layer.h"

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
        return runtime.begin_result_;
    }

    static XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* frame_end_info) {
        FakeRuntime& runtime = Active();
        std::unique_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_end_info &&
                                 frame_end_info->type == XR_TYPE_FRAME_END_INFO;
        runtime.calls_.push_back(RuntimeCall::kEnd);
        ++runtime.end_entered_;
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
    std::cout << "depthxr_turbo_frame_tests passed\n";
    return 0;
}
