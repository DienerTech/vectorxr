#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <Windows.h>

#include "depthxr/openxr_layer.h"
#include "depthxr/framegen_gpu.h"
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
            layer.turbo_recovery_.Configure({}, {});
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
    static void Suspend() { Layer().turbo_auto_suspended_.store(true, std::memory_order_relaxed); }
    static void ConfigureTurbo(bool enabled) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.resolved_settings_.turbo.enabled = enabled;
    }
    static bool PacingResolved() { return Layer().turbo_pacing_resolved_; }
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
        layer.pivotxr_engaged_=true; layer.pivotxr_activation_gain_=1; layer.pivotxr_quick_view_active_=false; layer.pivotxr_quick_view_transitioning_=false;
        layer.pivotxr_manual_view_transition_={}; layer.pivotxr_profile_view_transition_={};
        layer.pivotxr_profile_view_transition_.current.yaw_radians=.1;
        // Without Snap Views the nudge joins the single Pivot line.
        ok=ok && layer.BuildOsdSnapshot().pivot=="Motion: Active / Continuous + Nudge / DCS Stepped" &&
            layer.BuildOsdSnapshot().pivot_secondary.empty();
        layer.pivotxr_engaged_=false; layer.pivotxr_activation_gain_=0; layer.pivotxr_profile_view_transition_={};
        ok=ok && layer.BuildOsdSnapshot().pivot=="Motion: Off";
        layer.pivotxr_engaged_=true; layer.resolved_settings_.pivotxr.enabled=false;
        ok=ok && layer.BuildOsdSnapshot().pivot=="Motion: Disabled";
        layer.pivotxr_engaged_=false;
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

    // Starts Frame Generation through the layer's own session hook, with a
    // fake GPU backend and fake swapchain entry points.
    static void PrepareFrameGen(XrSession session, FrameGenPresenter::GpuFactory factory,
                                const FrameGenSwapchainDispatch& xr) {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.resolved_settings_.framegen.enabled = true;
        // Auto starts at full rate; these tests need an engaged 2x session.
        layer.resolved_settings_.framegen.multiplier = FrameGenMultiplier::k2;
        layer.next_enumerate_swapchain_formats_ = xr.formats;
        layer.next_create_swapchain_ = xr.create;
        layer.next_destroy_swapchain_ = xr.destroy;
        layer.next_enumerate_swapchain_images_ = xr.images;
        layer.next_acquire_swapchain_image_ = xr.acquire;
        layer.next_wait_swapchain_image_ = xr.wait;
        layer.next_release_swapchain_image_ = xr.release;
        layer.framegen_gpu_factory_ = std::move(factory);
        layer.StartFrameGen(session, nullptr);
        layer.framegen_.SetEngagementPolicy(2, std::chrono::milliseconds(0));
    }

    static void StopFrameGen() {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.framegen_.Stop();
        layer.framegen_owns_pacing_.store(false);
        layer.framegen_session_enabled_ = false;
        layer.turbo_yielded_to_framegen_ = false;
        layer.framegen_gpu_factory_ = CreateFrameGenGpu;
        layer.resolved_settings_.framegen = {};
    }

    // A session Frame Generation latches starts with no frame of Turbo's open:
    // the harness otherwise models a Turbo pipeline mid-flight.
    static void FreshSession() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        layer.turbo_frame_begun_ = false;
        layer.turbo_begin_owed_ = false;
    }
    // An instance's first load of the settings file at `config`, into a new log
    // at `log`, begun as xrCreateInstance begins one. Returns that log's path.
    static std::filesystem::path LoadConfigFile(const std::filesystem::path& config, const std::filesystem::path& log) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.BeginInstanceLogLocked(log);
        layer.config_path_ = config;
        layer.has_loaded_config_ = false;
        layer.ReloadConfigIfNeeded();
        return layer.logger_.ActiveLogPath();
    }
    // The settings file is read again now (as the layer's watcher does).
    static void PollConfigFile() { Layer().PollConfigFile(); }
    static ConfigDocument LoadedConfig() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        return layer.config_;
    }
    // The log so far: an error reaches the file at once, with everything
    // logged before it.
    static std::string Log() {
        static int reads = 0;
        auto& layer = Layer();
        layer.logger_.Error("Turbo frame tests: log read " + std::to_string(++reads) + ".");
        std::ifstream stream(layer.logger_.ActiveLogPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }
    // Forgets the test's settings file and lets its log go.
    static void UnloadConfigFile(const std::filesystem::path& log) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.config_path_.clear();
        layer.has_loaded_config_ = false;
        layer.BeginInstanceLogLocked(log);
    }
    // A settings.json reload: a new document, re-resolved as the frame loop does.
    static void ReloadConfig(const ConfigDocument& config) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.config_ = config;
        ++layer.config_generation_;
        layer.RefreshResolvedSettings();
    }
    static bool TurboEffective() { return Layer().turbo_effective_active_.load(); }
    static bool TurboYielded() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        return layer.turbo_yielded_to_framegen_;
    }
    static FrameGenPresenter& FrameGen() { return Layer().framegen_; }
    static bool FrameGenOwnsPacing() { return Layer().framegen_owns_pacing_.load(); }
    static bool TurboInterceptionRequired() { return Layer().turbo_frame_interception_required_.load(); }
    static bool TurboConfigured() { return Layer().resolved_settings_.turbo.enabled; }
    static OsdSnapshot Snapshot() {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        return layer.BuildOsdSnapshot();
    }
    // The OSD's modules line for a Quadviews session, before it begins
    // (nullopt) or once it has begun with `begun`.
    static std::string QuadViewsSessionModules(std::optional<XrViewConfigurationType> begun) {
        OpenXrLayer& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.quadviews_session_active_ = true;
        layer.has_active_primary_view_configuration_ = begun.has_value();
        layer.active_primary_view_configuration_type_ = begun.value_or(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
        std::string modules = layer.BuildOsdSnapshot().modules;
        layer.quadviews_session_active_ = false;
        layer.has_active_primary_view_configuration_ = false;
        layer.active_primary_view_configuration_type_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        return modules;
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

    static std::string FailureReason() { return Layer().turbo_recovery_.Reason(); }
    static double StabilityMs() { return Layer().turbo_stable_accumulated_ms_; }
    static void ConfigureRecovery(const std::filesystem::path& path) {
        auto& layer = Layer();
        TurboRecoveryRecord identity;
        identity.fingerprint = "turbo-frame-failure-simulation";
        identity.application = layer.current_exe_name_;
        identity.runtime = layer.runtime_name_;
        identity.process_id = GetCurrentProcessId();
        identity.process_created = TurboProcessCreationTime();
        layer.turbo_recovery_.Configure(path, std::move(identity));
        layer.resolved_settings_.turbo.interrupted_session_recovery = true;
    }
    static XrResult XRAPI_CALL EndSessionStub(XrSession) { return XR_SUCCESS; }
    static XrResult EndFaultedSession() {
        auto& layer = Layer();
        const auto previous = layer.next_end_session_;
        layer.next_end_session_ = &EndSessionStub;
        const auto result = layer.EndSession(XR_NULL_HANDLE);
        layer.next_end_session_ = previous;
        return result;
    }
    static bool ProbeRequired() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        return layer.turbo_begin_probe_required_;
    }
    static bool WaitsBeforeBegin() { return Layer().turbo_async_waits_before_begin_; }
    static bool DrainBounded(std::chrono::milliseconds timeout) { return Layer().DrainTurboAsyncWait(timeout); }
    static void ResetFrameState() { Layer().ResetTurboFrameState(); }
    static void StopWorker() { Layer().StopTurboAsyncWorker(); }
    static bool WorkerJoinable() { return Layer().turbo_async_worker_.joinable(); }
    static bool StaleCompletionPublished() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        return layer.turbo_async_wait_completed_ || layer.turbo_async_wait_.valid();
    }
    static std::filesystem::path SnapshotPath(const std::filesystem::path& log) {
        return TurboTimingTrace::SnapshotPath(log, 0);
    }
    static std::filesystem::path StartRecorder(const std::filesystem::path& path) {
        auto& layer = Layer();
        layer.logger_.Initialize(path);
        layer.logger_.SetLevel(LogLevel::Debug);
        layer.turbo_trace_.Start(layer.logger_);
        return layer.logger_.ActiveLogPath();
    }
    static void EndRecorder() {
        Layer().turbo_trace_.Stop();
        Layer().logger_.SetLevel(LogLevel::Info);
    }
    static void NeedSequencedCompensation() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.turbo_mutex_);
        layer.turbo_seq_state_ = OpenXrLayer::TurboSequencedState::kActive;
        layer.turbo_frame_begun_ = false;
        layer.turbo_begin_owed_ = false;
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

    // A native Varjo quad views session (Varjo compatible quadviews) with
    // Pivot, DepthXR and focus sharpening off, located and ended through the
    // layer's own hooks. The settings stay as set here: no file is read.
    static void PrepareVarjoNative(PFN_xrLocateViews locate_views, PFN_xrEndFrame end_frame) {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.has_loaded_config_ = true;
        layer.resolved_settings_ = {};
        layer.resolved_settings_.core.enabled = true;
        layer.resolved_settings_.depthxr.enabled = false;
        layer.resolved_settings_.quadviews.enabled = true;
        layer.resolved_settings_.quadviews.foveate_sharpness = 0.0;
        layer.resolved_settings_generation_ = layer.config_generation_;
        layer.resolved_settings_session_ = layer.active_session_;
        layer.quadviews_session_active_ = true;
        layer.varjo_compatible_quadviews_active_ = true;
        layer.varjo_foveated_rendering_extension_requested_ = false;
        layer.varjo_native_foveation_diagnostic_ = {};
        layer.next_locate_views_ = locate_views;
        layer.next_end_frame_ = end_frame;
        layer.turbo_frame_interception_required_.store(false, std::memory_order_release);
    }
    // Varjo focus sharpening asked for: frames take the projection rewrite
    // (the sharpen itself is skipped, with no Direct3D 11 device here).
    static void SharpenVarjoFocus() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.resolved_settings_.quadviews.foveate_sharpness = 50.0;
    }
    // The foveation summary's submittedFocusFov: checked, mismatched, unlocated.
    static std::array<std::uint64_t, 3> VarjoSubmittedFocus() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        const auto& diagnostic = layer.varjo_native_foveation_diagnostic_;
        return {diagnostic.submitted_focus_checked, diagnostic.submitted_focus_mismatched,
                diagnostic.submitted_focus_unlocated};
    }
    static void EndVarjoNative() {
        auto& layer = Layer();
        std::scoped_lock lock(layer.mutex_);
        layer.resolved_settings_ = {};
        layer.quadviews_session_active_ = false;
        layer.varjo_compatible_quadviews_active_ = false;
        layer.varjo_native_foveation_diagnostic_ = {};
        layer.next_locate_views_ = nullptr;
        layer.next_end_frame_ = nullptr;
    }
};

} // namespace depthxr

namespace {

void (*release_runtime_on_failure)() = nullptr;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        if (release_runtime_on_failure) release_runtime_on_failure();
        std::exit(1);
    }
}

enum class RuntimeCall { kWait, kBegin, kEnd };

// Models the OpenXR frame-call rules VectorXR depends on:
// - xrBeginFrame needs a completed xrWaitFrame (XR_ERROR_CALL_ORDER_INVALID
//   otherwise, with no state change) and returns XR_FRAME_DISCARDED when a
//   frame is already begun.
// - A second xrWaitFrame while a waited frame is still unbegun must block until
//   a begin (spec). The layer must never do that; it is counted as a violation
//   and failed immediately instead of deadlocking the test.
// - Optional interlock profile (PiOpenXR/Oculus/Varjo): a wait issued from a
//   thread other than the submitting thread completes only after a later
//   xrEndFrame, and xrBeginFrame is accepted before that wait completes.
// Injected failures can be one-shot or persistent, and can model a rejected
// begin/end that the runtime nevertheless consumed.
class FakeRuntime {
  public:
    FakeRuntime() : frame_thread_(std::this_thread::get_id()) {
        Expect(active_ == nullptr, "Only one fake runtime may be active at a time");
        active_ = this;
        release_runtime_on_failure = [] { Active().ReleaseAll(); };
    }

    ~FakeRuntime() {
        ReleaseAll();
        release_runtime_on_failure = nullptr;
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
        if (runtime.wait_tokens_ > 0) {
            ++runtime.stacked_waits_;
            runtime.cv_.notify_all();
            return XR_ERROR_RUNTIME_FAILURE;
        }
        ++runtime.active_waits_;
        runtime.max_active_waits_ = std::max(runtime.max_active_waits_, runtime.active_waits_);
        runtime.cv_.notify_all();
        const bool interlocked = runtime.interlock_ && std::this_thread::get_id() != runtime.frame_thread_;
        const int end_mark = runtime.end_entered_;
        runtime.cv_.wait(lock, [&runtime, interlocked, end_mark] {
            if (runtime.release_all_) return true;
            return runtime.wait_released_ && (!interlocked || runtime.end_entered_ > end_mark);
        });
        const XrResult result = TakeResult(runtime.wait_result_, runtime.wait_failures_remaining_);
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
        std::unique_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_begin_info &&
                                 frame_begin_info->type == XR_TYPE_FRAME_BEGIN_INFO;
        runtime.calls_.push_back(RuntimeCall::kBegin);
        ++runtime.begin_calls_;
        runtime.cv_.notify_all();
        runtime.cv_.wait(lock, [&runtime] { return runtime.begin_released_ || runtime.release_all_; });
        if (!runtime.wait_tokens_ && !runtime.interlock_) {
            ++runtime.begin_rejections_;
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        const bool discarded = runtime.frame_open_;
        const XrResult result = TakeResult(runtime.begin_result_, runtime.begin_failures_remaining_);
        if (XR_SUCCEEDED(result) || runtime.begin_failure_consumes_frame_) {
            if (runtime.wait_tokens_ > 0) --runtime.wait_tokens_;
            runtime.frame_open_ = true;
        }
        return XR_FAILED(result) ? result : discarded ? XR_FRAME_DISCARDED : result;
    }

    static XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* frame_end_info) {
        FakeRuntime& runtime = Active();
        std::unique_lock lock(runtime.mutex_);
        runtime.valid_structs_ = runtime.valid_structs_ && frame_end_info &&
                                 frame_end_info->type == XR_TYPE_FRAME_END_INFO;
        runtime.calls_.push_back(RuntimeCall::kEnd);
        runtime.frame_thread_ = std::this_thread::get_id();
        ++runtime.end_entered_;
        runtime.cv_.notify_all();
        if (!runtime.frame_open_) {
            ++runtime.end_rejections_;
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        const XrResult result = TakeResult(runtime.end_result_, runtime.end_failures_remaining_);
        if (XR_SUCCEEDED(result) || runtime.end_failure_consumes_frame_) {
            runtime.frame_open_ = false;
        }
        if (runtime.release_wait_on_next_end_) {
            runtime.release_wait_on_next_end_ = false;
            runtime.wait_released_ = true;
        }
        if (XR_SUCCEEDED(result) && frame_end_info && frame_end_info->layerCount == 0) ++runtime.empty_ends_;
        runtime.cv_.notify_all();
        runtime.cv_.wait(lock, [&runtime] { return runtime.end_released_ || runtime.release_all_; });
        ++runtime.end_exited_;
        runtime.cv_.notify_all();
        return result;
    }

    // PiOpenXR/Oculus/Varjo profile: off-thread waits complete only after a
    // later submit, and begins are accepted before that wait completes.
    void InterlockOffThreadWaits() {
        std::scoped_lock lock(mutex_);
        interlock_ = true;
    }

    void BlockEnd() {
        std::scoped_lock lock(mutex_);
        end_released_ = false;
    }

    void BlockBegin() {
        std::scoped_lock lock(mutex_);
        begin_released_ = false;
    }

    void ReleaseBegin() {
        std::scoped_lock lock(mutex_);
        begin_released_ = true;
        cv_.notify_all();
    }

    bool WaitForBeginEntered(int count, std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] { return begin_calls_ >= count; });
    }

    void NoOpenFrame() { frame_open_ = false; }

    // Models another component consuming the waited frame behind our back.
    void DropWaitTokens() {
        std::scoped_lock lock(mutex_);
        wait_tokens_ = 0;
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
        release_all_ = true;
        cv_.notify_all();
    }

    // count < 0: persistent; otherwise the next `count` calls fail.
    void SetEndResult(XrResult result, bool consumes_frame = false, int count = -1) {
        std::scoped_lock lock(mutex_);
        end_result_ = result;
        end_failure_consumes_frame_ = consumes_frame;
        end_failures_remaining_ = count;
    }

    void SetWaitResult(XrResult result, int count = -1) {
        std::scoped_lock lock(mutex_);
        wait_result_ = result;
        wait_failures_remaining_ = count;
    }

    void SetBeginResult(XrResult result, bool consumes_frame = false, int count = -1) {
        std::scoped_lock lock(mutex_);
        begin_result_ = result;
        begin_failure_consumes_frame_ = consumes_frame;
        begin_failures_remaining_ = count;
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

    int EmptyEnds() const {
        std::scoped_lock lock(mutex_);
        return empty_ends_;
    }

    int BeginRejections() const {
        std::scoped_lock lock(mutex_);
        return begin_rejections_;
    }

    int EndRejections() const {
        std::scoped_lock lock(mutex_);
        return end_rejections_;
    }

    int StackedWaits() const {
        std::scoped_lock lock(mutex_);
        return stacked_waits_;
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

    static XrResult TakeResult(XrResult configured, int& remaining) {
        if (XR_SUCCEEDED(configured) || remaining == 0) return XR_SUCCESS;
        if (remaining > 0) --remaining;
        return configured;
    }

    inline static FakeRuntime* active_{nullptr};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<RuntimeCall> calls_;
    std::thread::id frame_thread_;
    bool interlock_{false};
    bool end_released_{true};
    bool begin_released_{true};
    bool wait_released_{true};
    bool release_all_{false};
    bool release_wait_on_next_end_{false};
    bool valid_structs_{true};
    bool frame_open_{true};
    bool begin_failure_consumes_frame_{false};
    bool end_failure_consumes_frame_{false};
    int wait_tokens_{0};
    int wait_entered_{0};
    int wait_exited_{0};
    int begin_calls_{0};
    int begin_rejections_{0};
    int end_entered_{0};
    int end_exited_{0};
    int end_rejections_{0};
    int empty_ends_{0};
    int stacked_waits_{0};
    int active_waits_{0};
    int max_active_waits_{0};
    int wait_failures_remaining_{-1};
    int begin_failures_remaining_{-1};
    int end_failures_remaining_{-1};
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

// --- Frame Generation through the layer hooks ------------------------------------

constexpr std::int64_t kFrameGenFormat = 29;
constexpr XrDuration kFrameGenPeriod = 11'111'111; // FakeRuntime::display_period_
std::uint64_t g_framegen_handles = 0x9000;

XrResult XRAPI_PTR FgFormats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* formats) {
    *count = 1;
    if (capacity) formats[0] = kFrameGenFormat;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR FgCreate(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* swapchain) {
    *swapchain = reinterpret_cast<XrSwapchain>(++g_framegen_handles);
    return XR_SUCCESS;
}
XrResult XRAPI_PTR FgDestroy(XrSwapchain) { return XR_SUCCESS; }
XrResult XRAPI_PTR FgImages(XrSwapchain, std::uint32_t, std::uint32_t* count, XrSwapchainImageBaseHeader*) {
    *count = 3;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR FgAcquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    *index = 0;
    return XR_SUCCESS;
}
XrResult XRAPI_PTR FgWait(XrSwapchain, const XrSwapchainImageWaitInfo*) { return XR_SUCCESS; }
XrResult XRAPI_PTR FgRelease(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }

class FgGpu final : public depthxr::FrameGenGpu {
  public:
    explicit FgGpu(int* executes) : executes_(executes) {}
    const char* Name() const override { return "Fake"; }
    std::optional<depthxr::FrameGenOutputKind> Classify(std::int64_t format) const override {
        return format == kFrameGenFormat ? std::optional(depthxr::FrameGenOutputKind::kSrgb8) : std::nullopt;
    }
    std::vector<std::int64_t> OutputFormats(depthxr::FrameGenOutputKind) const override { return {kFrameGenFormat}; }
    XrSwapchainUsageFlags RequiredSourceUsage() const override { return 0; }
    XrSwapchainUsageFlags OutputUsage() const override { return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; }
    bool Track(XrSwapchain, PFN_xrEnumerateSwapchainImages, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Capture(std::size_t, const depthxr::FrameGenImage&, const depthxr::FrameGenViewRequest&,
                 depthxr::FrameGenFlowQuality, std::string&) override {
        return true;
    }
    bool WaitIdle() override { return true; }
    bool Execute(const std::vector<depthxr::FrameGenViewSubmission>& views, const depthxr::FrameGenTuning&,
                 depthxr::FrameGenExecuteResult& result, std::string&) override {
        ++*executes_;
        result.synthesized.assign(views.size(), true);
        return true;
    }
    void ResetHistory() override {}
    void Reset() override {}

  private:
    int* executes_;
};

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
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    if (experimental) depthxr::TurboFrameTestPeer::Experiment(true, true);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Toggle test could not establish async pacing");
    Expect(runtime.WaitForWaitExited(1), "Initial worker wait did not complete");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()),
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
        Expect(XR_SUCCEEDED(AppBegin()), "Queued begin lost its wait at the off transition");
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
        Expect(XR_SUCCEEDED(AppBegin()) && AppWait(&state) == XR_SUCCESS,
               "Re-enabled pipeline lost its queued frame");
    }
    Expect(runtime.MaxConcurrentWaits() == 1, "Toggle duplicated a runtime wait");
}

// Suspension and disabling Turbo must remove the off-thread wait (it is what
// interlocks with submission on PiOpenXR/Oculus/Varjo) without forwarding a
// queued app Begin that has no runtime wait.
void TestAsyncReleaseHandsOverToFrameThread(bool disable_in_settings) {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Release test could not establish async pacing");
    Expect(runtime.WaitForWaitExited(1), "Release test worker wait did not complete");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Release test first virtual frame failed");
    Expect(AppWait(&state) == XR_SUCCESS, "Release test queued app wait failed");

    if (disable_in_settings) depthxr::TurboFrameTestPeer::ConfigureTurbo(false);
    else depthxr::TurboFrameTestPeer::Suspend();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Releasing async pacing violated runtime frame order");
    Expect(XR_SUCCEEDED(AppBegin()), "Queued begin lost its runtime wait when async pacing was released");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced &&
               depthxr::TurboFrameTestPeer::SequencedState() == depthxr::TurboFrameTestPeer::ActiveState(),
           "Released async pacing did not hand its frame to frame-thread pacing");
    Expect(!depthxr::TurboFrameTestPeer::Handoff().wait_valid && !depthxr::TurboFrameTestPeer::Handoff().active,
           "Released async pacing left a worker wait or shield armed");

    Expect(AppWait(&state) == XR_SUCCESS, "Frame-thread pacing did not return the next app wait");
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Frame-thread pacing submit failed after release");
    const std::vector<RuntimeCall> expected{
        RuntimeCall::kEnd, RuntimeCall::kWait,                      // async establishment + worker wait
        RuntimeCall::kBegin, RuntimeCall::kEnd, RuntimeCall::kWait, RuntimeCall::kBegin, // release frame
        RuntimeCall::kEnd, RuntimeCall::kWait, RuntimeCall::kBegin, // frame-thread steady state
    };
    Expect(runtime.Calls() == expected, "Released async pacing did not continue as End -> Wait -> Begin");
    Expect(runtime.MaxConcurrentWaits() == 1, "Release duplicated a runtime wait");
    depthxr::TurboFrameTestPeer::Cleanup();
    Expect(!depthxr::TurboFrameTestPeer::PacingResolved(),
           "A released session must re-resolve the configured strategy next session");
}

void TestAsyncRecoupleStallReleasesPipeline() {
    FakeRuntime runtime;
    runtime.InterlockOffThreadWaits();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitEntered(1), "Stall test could not establish async pacing");

    // Manual toggle-off keeps the async pipeline re-coupled. Its interlocked
    // worker wait stalls until this submit, which the runtime accepts.
    depthxr::TurboFrameTestPeer::Toggle(false);
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Stall test toggle-off submit failed");
    Expect(runtime.WaitForWaitExited(1), "Interlocked worker wait did not release on submit");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kAsync,
           "A manual toggle-off must keep the async pipeline until its wait retires");

    // Turbo being off must not keep the stall: the retired wait hands the
    // frame to frame-thread pacing.
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Release after a re-coupled stall failed");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced &&
               depthxr::TurboFrameTestPeer::SequencedState() == depthxr::TurboFrameTestPeer::ActiveState(),
           "A stalled re-coupled pipeline was not released to frame-thread pacing");
    const int waits = runtime.WaitCalls();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitCalls() == waits + 1 && !depthxr::TurboFrameTestPeer::Handoff().wait_valid,
           "Released pipeline launched another off-thread wait");
    Expect(runtime.MaxConcurrentWaits() == 1 && runtime.StackedWaits() == 0 && runtime.BeginRejections() == 0,
           "Stall release duplicated, stacked, or misordered a runtime wait");
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
    Expect(XR_SUCCEEDED(AppBegin()), "Handoff-shielded app xrBeginFrame failed");
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
    Expect(AppWait(&first_state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()),
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
    Expect(AppWait(&first_state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()),
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
    Expect(AppWait(&handshake_state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()),
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
    Expect(AppWait(&fabricated_state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()),
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

XrResult AppFrame(const XrFrameEndInfo& end_info) {
    XrFrameState state{XR_TYPE_FRAME_STATE};
    const XrResult wait_result = AppWait(&state);
    if (XR_FAILED(wait_result)) return wait_result;
    const XrResult begin_result = AppBegin();
    if (XR_FAILED(begin_result)) return begin_result;
    return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
}

void ExpectHealthyFrames(FakeRuntime& runtime, int count, const std::string& context) {
    const auto end_info = FrameEndInfo();
    for (int frame = 0; frame < count; ++frame) {
        Expect(AppFrame(end_info) == XR_SUCCESS, context + ": an application frame failed after recovery");
    }
    Expect(runtime.StackedWaits() == 0 && runtime.MaxConcurrentWaits() == 1,
           context + ": a runtime wait was stacked on an unbegun frame or duplicated");
}

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string ReadDirectory(const std::filesystem::path& directory) {
    std::string text;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        std::ifstream stream(entry.path());
        text.append(std::istreambuf_iterator<char>(stream), {});
    }
    return text;
}

// A conformant runtime rejects a begin before its wait completes and finishes
// the wait itself: Turbo waits for it and submits the frame (previously this
// suspended Turbo and failed the application's xrEndFrame).
void TestConformantLateWaitIsWaitedFor() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Late-wait test could not establish async pacing");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    for (int late = 0; late < 2; ++late) {
        // Block only the next worker wait, never one still being issued.
        Expect(runtime.WaitForWaitExited(1 + 2 * late), "Previous worker wait did not complete");
        Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Late-wait virtual frame failed");
        runtime.BlockWait();
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
               "Frame before the late wait failed");
        Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Late-wait app frame was not fabricated");
        const int ends_before = runtime.EndCalls();
        auto release = std::async(std::launch::async, [&runtime] {
            std::this_thread::sleep_for(600ms);
            runtime.ReleaseWait();
        });
        const auto start = std::chrono::steady_clock::now();
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
               "A late conformant wait failed the application's xrEndFrame");
        const double elapsed = ElapsedMs(start);
        release.get();
        Expect(elapsed >= 550.0 && elapsed < 2200.0, "Late wait was not waited for within its bound");
        Expect(runtime.EndCalls() == ends_before + 1, "Late-wait frame was not submitted exactly once");
    }
    Expect(runtime.BeginRejections() == 1 && depthxr::TurboFrameTestPeer::WaitsBeforeBegin(),
           "A conformant runtime was probed with more than one early xrBeginFrame");
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended() &&
               depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kAsync,
           "Two isolated late waits suspended or abandoned async pacing");
    ExpectHealthyFrames(runtime, 32, "Late conformant wait");
}

// A runtime wait that never completes must not freeze the application inside
// VectorXR: the frame is dropped after a bounded wait, and forwarding resumes
// as soon as the runtime completes the wait.
void TestStuckWaitDropsFrameThenResumes() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Stuck-wait test could not establish async pacing");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Stuck-wait first frame failed");
    runtime.BlockWait();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Stuck-wait frame before the stall failed");
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Stuck-wait app frame was not fabricated");
    const int ends_before = runtime.EndCalls();
    const auto start = std::chrono::steady_clock::now();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "A stuck runtime wait returned an error to the application");
    const double elapsed = ElapsedMs(start);
    Expect(elapsed >= 2000.0 && elapsed < 3000.0, "Stuck-wait xrEndFrame was not bounded");
    Expect(runtime.EndCalls() == ends_before && depthxr::TurboFrameTestPeer::Handoff().wait_valid,
           "A frame was submitted without its wait, or the retained wait was discarded");
    auto poll = std::async(std::launch::async, [] {
        XrFrameState next{XR_TYPE_FRAME_STATE};
        return AppWait(&next);
    });
    Expect(poll.wait_for(50ms) == std::future_status::timeout,
           "The next app poll ran ahead of the runtime's outstanding wait");
    runtime.ReleaseWait();
    Expect(poll.wait_for(2s) == std::future_status::ready && poll.get() == XR_SUCCESS,
           "The app poll did not resume when the runtime completed its wait");
    Expect(XR_SUCCEEDED(AppBegin()) &&
               depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.EndCalls() == ends_before + 1,
           "Forwarding did not resume after the stuck wait completed");
    ExpectHealthyFrames(runtime, 16, "Stuck wait");
}

// PiOpenXR/Oculus/Varjo: the off-thread wait completes only after a submit.
// Auto discovery must keep accepting those early begins and fall back to
// frame-thread pacing (restores the coverage the strict-only fixture lost).
void TestInterlockedRuntimeAutoFallsBackToSequenced() {
    FakeRuntime runtime;
    runtime.InterlockOffThreadWaits();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    depthxr::TurboFrameTestPeer::SetPolicy(depthxr::TurboPacingMode::kAsync,
                                          depthxr::TurboFrameTestPeer::Source::kProbing);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitEntered(1), "Interlock test could not establish async pacing");
    int frames = 0;
    while (depthxr::TurboFrameTestPeer::SequencedState() != depthxr::TurboFrameTestPeer::ActiveState() &&
           frames < 8) {
        Expect(AppFrame(end_info) == XR_SUCCESS, "An interlocked async frame failed");
        ++frames;
    }
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced &&
               depthxr::TurboFrameTestPeer::SequencedState() == depthxr::TurboFrameTestPeer::ActiveState() &&
               frames <= 5,
           "Auto did not hand interlocked async pacing to the frame thread after two stalls");
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended() && runtime.BeginRejections() == 0,
           "The interlock fallback suspended Turbo or was rejected");
    const auto start = std::chrono::steady_clock::now();
    ExpectHealthyFrames(runtime, 64, "Interlocked fallback");
    Expect(ElapsedMs(start) < 64 * 50.0, "Frame-thread pacing still stalled on the interlocked runtime");
}

// A forced async strategy is never adapted, but stalls must still end in a
// suspension that keeps frames flowing rather than freezing the game.
void TestInterlockedForcedAsyncSuspendsAndKeepsRunning() {
    FakeRuntime runtime;
    runtime.InterlockOffThreadWaits();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Forced interlock test could not establish async pacing");
    int frames = 0;
    while (!depthxr::TurboFrameTestPeer::AutoSuspended() && frames < 16) {
        Expect(AppFrame(end_info) == XR_SUCCESS, "A forced interlocked async frame failed");
        ++frames;
    }
    Expect(depthxr::TurboFrameTestPeer::AutoSuspended(), "Repeated forced-async stalls never suspended Turbo");
    ExpectHealthyFrames(runtime, 32, "Forced interlocked suspension");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced,
           "A suspended interlocked pipeline was not handed to the frame thread");
}

// Ressay's -37: the runtime rejected the deferred begin after a completed
// wait. The runtime proved it holds no waited frame, so the frame is opened on
// the frame thread and the game keeps running; the event is recorded as a
// recovered (non-blocking) fault and freezes recorder history.
void TestOwnershipDesyncRecoversInSession() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto recovery_path = std::filesystem::temp_directory_path() /
        ("vxr-desync-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(recovery_path);
    depthxr::TurboFrameTestPeer::ConfigureRecovery(recovery_path);
    const auto recorder_log = depthxr::TurboFrameTestPeer::StartRecorder(recovery_path / "frame.log");
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Desync test could not establish a completed wait");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Desync virtual frame failed");
    runtime.DropWaitTokens();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "A rejected deferred begin was returned to the application instead of recovered");
    Expect(runtime.BeginRejections() == 1 &&
               depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced &&
               depthxr::TurboFrameTestPeer::SequencedState() == depthxr::TurboFrameTestPeer::ActiveState(),
           "Desync recovery did not continue on frame-thread pacing");
    ExpectHealthyFrames(runtime, 32, "Ownership desync");
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended() && depthxr::TurboFrameTestPeer::FailureReason().empty(),
           "A single recovered desync suspended or blocked Turbo");
    const std::string faults = ReadDirectory(recovery_path / "faults");
    Expect(faults.find("recovered") != std::string::npos && faults.find("xrBeginFrame") != std::string::npos,
           "The recovered fault was not archived with its operation");
    depthxr::TurboFrameTestPeer::EndRecorder();
    std::string recording;
    for (unsigned part = 0; part < 2; ++part) {
        std::ifstream stream(depthxr::TurboTimingTrace::RecorderPath(recorder_log, part));
        recording.append(std::istreambuf_iterator<char>(stream), {});
    }
    Expect(recording.find("fault.begin") != std::string::npos && recording.find("a=-37") != std::string::npos &&
               recording.find("recover.handover") != std::string::npos,
           "Flight recorder omitted the fault or its recovery");
    Expect(std::filesystem::exists(depthxr::TurboFrameTestPeer::SnapshotPath(recorder_log)),
           "A fault did not freeze a recorder snapshot");
    std::error_code error;
    std::filesystem::remove_all(recovery_path, error);
}

// An app poll already blocked on the async handoff when a failure hands frame
// ownership to the frame thread must stay fabricated. Falling through to a
// real runtime wait would race the frame thread's own wait.
void TestHandoverKeepsBlockedPollShielded() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Blocked-poll test could not establish a completed wait");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Blocked-poll virtual frame failed");
    runtime.DropWaitTokens();
    runtime.BlockBegin();
    auto end = std::async(std::launch::async, [&] {
        return depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info);
    });
    Expect(runtime.WaitForBeginEntered(1), "Deferred begin never reached the runtime");
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "First poll during the handoff failed");
    auto blocked_poll = std::async(std::launch::async, [] {
        XrFrameState next{XR_TYPE_FRAME_STATE};
        return AppWait(&next);
    });
    Expect(WaitForSecondPollBlock(), "Second poll did not wait on the handoff");
    runtime.ReleaseBegin();
    Expect(end.wait_for(3s) == std::future_status::ready && end.get() == XR_SUCCESS,
           "The handed-over frame was not recovered");
    Expect(blocked_poll.wait_for(2s) == std::future_status::ready && blocked_poll.get() == XR_SUCCESS,
           "The blocked poll did not resume after the handover");
    const std::vector<RuntimeCall> expected{
        RuntimeCall::kEnd, RuntimeCall::kWait,                       // async establishment + worker wait
        RuntimeCall::kBegin,                                         // deferred begin, rejected (-37)
        RuntimeCall::kWait, RuntimeCall::kBegin, RuntimeCall::kEnd,  // frame-thread recovery + submit
        RuntimeCall::kWait, RuntimeCall::kBegin,                     // sequenced post-submit pacing
    };
    Expect(runtime.Calls() == expected && runtime.StackedWaits() == 0,
           "A blocked poll reached the runtime alongside the frame thread's own wait");
    ExpectHealthyFrames(runtime, 16, "Handover with a blocked poll");
}

// A begin rejected for another reason may or may not have consumed the waited
// frame. Recovery probes with xrBeginFrame before any further wait, so it never
// stacks a wait on an unbegun frame (which the spec makes block forever).
void TestFailedBeginProbesBeforeWaiting(bool consumes_frame) {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Probe test could not establish a completed wait");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Probe test virtual frame failed");
    runtime.SetBeginResult(XR_ERROR_RUNTIME_FAILURE, consumes_frame, 1);
    const auto calls_before = runtime.Calls().size();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "A failed deferred begin was not recovered within the frame");
    const auto calls = runtime.Calls();
    Expect(calls.size() > calls_before + 1 && calls[calls_before] == RuntimeCall::kBegin &&
               calls[calls_before + 1] == RuntimeCall::kBegin,
           "Recovery did not probe with xrBeginFrame before waiting again");
    Expect(!depthxr::TurboFrameTestPeer::ProbeRequired(), "A settled probe stayed armed");
    ExpectHealthyFrames(runtime, 16, consumes_frame ? "Consumed failed begin" : "Unconsumed failed begin");
}

void TestAsyncWaitFailureRecoversOnFrameThread() {
    FakeRuntime runtime;
    runtime.SetWaitResult(XR_ERROR_RUNTIME_FAILURE, 1);
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitExited(1), "Wait failure test could not establish async pacing");
    Expect(AppFrame(end_info) == XR_SUCCESS,
           "A failed async wait was returned to the application instead of recovered");
    Expect(runtime.EndCalls() == 2 && runtime.EndRejections() == 0,
           "The recovered frame was not submitted with a begun runtime frame");
    Expect(depthxr::TurboFrameTestPeer::PacingMode() == depthxr::TurboPacingMode::kSequenced,
           "A failed async wait did not hand pacing to the frame thread");
    ExpectHealthyFrames(runtime, 16, "Async wait failure");
}

void TestCompensationWaitFailureRetriesNextFrame() {
    FakeRuntime runtime;
    runtime.NoOpenFrame();
    runtime.SetWaitResult(XR_ERROR_RUNTIME_FAILURE, 1);
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    depthxr::TurboFrameTestPeer::NeedSequencedCompensation();
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_ERROR_RUNTIME_FAILURE,
           "Failed compensation wait was not returned");
    Expect(runtime.BeginCalls() == 0 && runtime.EndCalls() == 0 && !depthxr::TurboFrameTestPeer::AutoSuspended(),
           "Failed compensation wait fell through to submit or suspended on one failure");
    Expect(AppFrame(end_info) == XR_SUCCESS && runtime.EndCalls() == 1,
           "The next frame did not re-open and submit after a failed compensation wait");
    ExpectHealthyFrames(runtime, 16, "Compensation wait failure");
}

void TestSequencedPostSubmitFailureRecovers(bool fail_begin) {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Post-submit failure test could not start the sequenced handshake");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Sequenced handshake failed");
    if (fail_begin) runtime.SetBeginResult(XR_ERROR_RUNTIME_FAILURE, false, 1);
    else runtime.SetWaitResult(XR_ERROR_RUNTIME_FAILURE, 1);
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Failure preparing the next frame changed the already-accepted submit result");
    Expect(depthxr::TurboFrameTestPeer::ProbeRequired() == fail_begin,
           "A failed post-submit begin did not require a begin probe");
    const int waits_before = runtime.WaitCalls();
    Expect(AppFrame(end_info) == XR_SUCCESS, "The frame after a post-submit failure was not recovered");
    Expect(!fail_begin || runtime.WaitCalls() == waits_before + 1,
           "Recovery waited again instead of beginning the already-waited frame");
    ExpectHealthyFrames(runtime, 16, fail_begin ? "Post-submit begin failure" : "Post-submit wait failure");
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended(), "One post-submit failure suspended Turbo");
}

void TestSessionEndingWaitDoesNotCreateSafetyFault(XrResult error) {
    FakeRuntime runtime;
    runtime.NoOpenFrame();
    runtime.SetWaitResult(error);
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    depthxr::TurboFrameTestPeer::NeedSequencedCompensation();
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == error &&
               !depthxr::TurboFrameTestPeer::AutoSuspended() &&
               depthxr::TurboFrameTestPeer::FailureReason().empty() && runtime.EndCalls() == 0,
           "Ordinary session-ending wait created a Turbo fault or fell through to submit");
}

void TestRejectedEndRecovers(bool consumes_frame) {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    runtime.SetEndResult(XR_ERROR_CALL_ORDER_INVALID, consumes_frame, 1);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_ERROR_CALL_ORDER_INVALID,
           "A rejected submit result was not returned to the application");
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended(), "One rejected submit suspended Turbo");
    ExpectHealthyFrames(runtime, 16, consumes_frame ? "Consumed rejected end" : "Unconsumed rejected end");
}

// Persistent failures end Turbo's pacing, but every frame call still returns
// promptly, and forwarding resumes when the runtime recovers.
void TestPersistentFailuresSuspendWithoutStoppingFrames() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Persistent-failure test could not start the sequenced handshake");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Sequenced handshake failed");
    runtime.SetBeginResult(XR_ERROR_RUNTIME_FAILURE);
    for (int frame = 0; frame < 6; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        const XrResult result = AppFrame(end_info);
        Expect(frame == 0 ? result == XR_SUCCESS : result == XR_ERROR_RUNTIME_FAILURE,
               "A persistent begin failure was hidden or changed");
        Expect(ElapsedMs(start) < 500.0, "A failing frame call blocked the application");
    }
    Expect(depthxr::TurboFrameTestPeer::AutoSuspended(), "Persistent failures did not suspend Turbo");
    runtime.SetBeginResult(XR_SUCCESS);
    ExpectHealthyFrames(runtime, 16, "Persistent failure recovery");
}

// Session end must not hang on a pending interlocked wait: it is released
// with a balancing begin and empty submit.
void TestSessionEndReleasesInterlockedWait() {
    FakeRuntime runtime;
    runtime.InterlockOffThreadWaits();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitEntered(1), "Session-end interlock test could not establish async pacing");
    const auto start = std::chrono::steady_clock::now();
    Expect(depthxr::TurboFrameTestPeer::EndFaultedSession() == XR_SUCCESS, "xrEndSession failed");
    const double elapsed = ElapsedMs(start);
    Expect(elapsed < 1500.0,
           "Session end waited " + std::to_string(elapsed) + "ms on an interlocked wait instead of releasing it");
    Expect(runtime.WaitForWaitExited(1, 0ms), "Session end left the interlocked worker wait outstanding");
    Expect(runtime.StackedWaits() == 0 && runtime.EndRejections() == 0,
           "Session-end balancing violated frame ordering");
}

// A wait that never returns cannot hang session teardown, and its late
// completion cannot publish into the next session's frame state.
void TestSessionEndWithStuckWaitIsBounded() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    runtime.BlockWait();
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitEntered(1), "Stuck session-end test could not establish async pacing");
    Expect(!depthxr::TurboFrameTestPeer::DrainBounded(250ms), "A blocked wait reported completion");
    const auto start = std::chrono::steady_clock::now();
    Expect(depthxr::TurboFrameTestPeer::EndFaultedSession() == XR_SUCCESS, "xrEndSession failed");
    const double elapsed = ElapsedMs(start);
    Expect(elapsed < 6000.0, "Session teardown was not bounded by a stuck runtime wait");
    runtime.ReleaseWait();
    Expect(runtime.WaitForWaitExited(1), "Stuck wait never returned after release");
    std::this_thread::sleep_for(50ms);
    Expect(!depthxr::TurboFrameTestPeer::StaleCompletionPublished(),
           "A wait from an ended session published into the next session's state");
}

// After a session ended around a wait the runtime never returned (its future
// cleared by the bounded session teardown), xrDestroyInstance's worker stop
// must still be bounded: the worker is released, not joined.
void TestWorkerStopAfterStuckSessionEndIsBounded() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    runtime.BlockWait();
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS &&
               runtime.WaitForWaitEntered(1), "Stuck worker-stop test could not establish async pacing");
    Expect(depthxr::TurboFrameTestPeer::EndFaultedSession() == XR_SUCCESS, "xrEndSession failed");
    depthxr::TurboFrameTestPeer::ResetFrameState();
    Expect(depthxr::TurboFrameTestPeer::DrainBounded(0ms),
           "Session teardown should have dropped the stuck wait's frame future");
    const auto start = std::chrono::steady_clock::now();
    depthxr::TurboFrameTestPeer::StopWorker();
    const double elapsed = ElapsedMs(start);
    Expect(elapsed < 3000.0, "Stopping the Turbo worker joined a wait still inside the runtime");
    Expect(!depthxr::TurboFrameTestPeer::WorkerJoinable(), "The stuck Turbo worker was neither joined nor released");
    runtime.ReleaseWait();
    Expect(runtime.WaitForWaitExited(1), "Stuck wait never returned after release");
    std::this_thread::sleep_for(50ms);
    Expect(!depthxr::TurboFrameTestPeer::StaleCompletionPublished(),
           "A released worker published into a later session's state");
}

void TestHealthyPipelineRetainsStrictOrder(depthxr::TurboPacingMode mode) {
    FakeRuntime runtime;
    TurboHarness harness(runtime, mode);
    const auto end_info = FrameEndInfo();
    Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
           "Healthy simulation could not establish pacing");
    XrFrameState state{XR_TYPE_FRAME_STATE};
    for (int frame = 0; frame < 2048; ++frame) {
        Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()) &&
                   depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &end_info) == XR_SUCCESS,
               "Healthy frame violated strict wait/begin/end ordering");
    }
    Expect(!depthxr::TurboFrameTestPeer::AutoSuspended() && runtime.MaxConcurrentWaits() == 1 &&
               runtime.StackedWaits() == 0 && runtime.BeginRejections() == 0 && runtime.EndRejections() == 0,
           "Healthy simulation tripped Safety or violated runtime frame ordering");
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
    Expect(AppWait(&state) == XR_SUCCESS && XR_SUCCEEDED(AppBegin()), "Sequenced retry handshake failed");
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

void TestSequencedStallPolicySuspends() {
    FakeRuntime runtime;
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kAsync);
    // Policy coverage is separate from strict frame-order simulation: the old
    // interlock fixture accepted Begin with no completed Wait and hid -37.
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
    Expect(runtime.Calls().empty(), "A policy-only test unexpectedly issued runtime calls");
}

} // namespace

// Frame Generation owns pacing through the layer's real hooks: Turbo yields,
// the runtime sees an ordered wait/begin/end pair per engaged app frame, and
// the OSD reports both features.
void TestFrameGenOwnsFramePacing() {
    FakeRuntime runtime;
    // A new session: the runtime has no frame open, and Turbo owns none.
    runtime.NoOpenFrame();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    depthxr::TurboFrameTestPeer::FreshSession();
    int executes = 0;
    depthxr::TurboFrameTestPeer::PrepareFrameGen(
        TestSession(), [&](const void*, std::string&) { return std::make_unique<FgGpu>(&executes); },
        {FgFormats, FgCreate, FgDestroy, FgImages, FgAcquire, FgWait, FgRelease});
    Expect(depthxr::TurboFrameTestPeer::FrameGenOwnsPacing(), "Frame Generation should own pacing with a backend");
    Expect(!depthxr::TurboFrameTestPeer::TurboInterceptionRequired() && !depthxr::TurboFrameTestPeer::TurboConfigured(),
           "Turbo must yield to Frame Generation for the session");

    const XrSwapchain left = reinterpret_cast<XrSwapchain>(0x71), right = reinterpret_cast<XrSwapchain>(0x72);
    auto& framegen = depthxr::TurboFrameTestPeer::FrameGen();
    framegen.TrackSwapchain(left, kFrameGenFormat, 1, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    framegen.TrackSwapchain(right, kFrameGenFormat, 1, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose.orientation.w = 1.0f;
        views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        views[eye].subImage.swapchain = eye ? right : left;
        views[eye].subImage.imageRect = {{0, 0}, {1024, 1024}};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 2;
    projection.views = views.data();
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);

    auto frame = [&] {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        Expect(AppWait(&state) == XR_SUCCESS, "Frame Generation app wait failed");
        Expect(AppBegin() == XR_SUCCESS, "Frame Generation app begin failed");
        framegen.NoteRelease(left, 1);
        framegen.NoteRelease(right, 1);
        XrFrameEndInfo info = FrameEndInfo();
        info.displayTime = state.predictedDisplayTime;
        info.layerCount = 1;
        info.layers = &layer;
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &info) == XR_SUCCESS,
               "Frame Generation app end failed");
        return state;
    };

    for (int i = 0; i < 2; ++i) {
        Expect(frame().predictedDisplayPeriod == kFrameGenPeriod, "Frame Generation passes through until steady");
    }
    Expect(runtime.WaitCalls() == 2 && runtime.EndCalls() == 2, "Pass-through frames make one runtime frame each");
    const XrFrameState engaged = frame();
    Expect(engaged.predictedDisplayPeriod == 2 * kFrameGenPeriod, "Engaged app sees a doubled period");
    Expect(runtime.WaitCalls() == 4 && runtime.BeginCalls() == 4 && runtime.EndCalls() == 4 && executes == 1,
           "An engaged app frame makes two runtime frames and one generation");
    frame();
    const std::vector<RuntimeCall> calls = runtime.Calls();
    const std::vector<RuntimeCall> tail(calls.end() - 6, calls.end());
    const std::vector<RuntimeCall> pair{RuntimeCall::kWait, RuntimeCall::kBegin, RuntimeCall::kEnd,
                                        RuntimeCall::kWait, RuntimeCall::kBegin, RuntimeCall::kEnd};
    Expect(tail == pair, "Engaged frames must reach the runtime as wait/begin/end twice in order");
    Expect(runtime.MaxConcurrentWaits() == 1 && runtime.ValidStructs(), "Runtime waits never overlapped");
    // The strict runtime's rules hold for Frame Generation's frames too: no
    // wait stacked on a waited, unbegun frame, and no begin or end out of order.
    Expect(runtime.StackedWaits() == 0 && runtime.BeginRejections() == 0 && runtime.EndRejections() == 0,
           "Frame Generation broke the runtime's frame-call order");

    const auto snapshot = depthxr::TurboFrameTestPeer::Snapshot();
    Expect(snapshot.turbo == "Paused for Frame Generation", "OSD should show Turbo yielding");
    Expect(snapshot.modules.find("Frame Gen") != std::string::npos && snapshot.framegen.rfind("On  /  Interpolate", 0) == 0,
           "OSD should list and describe active Frame Generation");
    Expect(snapshot.framegen_session && snapshot.framegen_badge == "ON", "OSD should show Frame Generation on beside FPS");
    depthxr::TurboFrameTestPeer::StopFrameGen();
    Expect(!depthxr::TurboFrameTestPeer::FrameGenOwnsPacing(), "Stopping Frame Generation returns pacing");
}

// Every settings reload while Frame Generation owns pacing resolves Turbo as
// configured (on here) and forces it off again. That must stay inert: no
// Turbo engage or disengage transition, no recovery record or fault, no
// frame interception, and the runtime keeps getting Frame Generation's
// frames in order.
void TestConfigReloadWhileFrameGenOwnsPacing() {
    const auto recovery = std::filesystem::temp_directory_path() /
                          ("vectorxr-fg-reload-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(recovery);
    FakeRuntime runtime;
    runtime.NoOpenFrame();
    TurboHarness harness(runtime, depthxr::TurboPacingMode::kSequenced);
    depthxr::TurboFrameTestPeer::FreshSession();
    depthxr::TurboFrameTestPeer::ConfigureRecovery(recovery);
    int executes = 0;
    depthxr::TurboFrameTestPeer::PrepareFrameGen(
        TestSession(), [&](const void*, std::string&) { return std::make_unique<FgGpu>(&executes); },
        {FgFormats, FgCreate, FgDestroy, FgImages, FgAcquire, FgWait, FgRelease});
    Expect(depthxr::TurboFrameTestPeer::FrameGenOwnsPacing() && depthxr::TurboFrameTestPeer::TurboYielded(),
           "Reload test: Frame Generation should own pacing with Turbo yielded");

    const XrSwapchain left = reinterpret_cast<XrSwapchain>(0x81), right = reinterpret_cast<XrSwapchain>(0x82);
    auto& framegen = depthxr::TurboFrameTestPeer::FrameGen();
    framegen.TrackSwapchain(left, kFrameGenFormat, 1, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    framegen.TrackSwapchain(right, kFrameGenFormat, 1, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    std::array<XrCompositionLayerProjectionView, 2> views{};
    for (int eye = 0; eye < 2; ++eye) {
        views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[eye].pose.orientation.w = 1.0f;
        views[eye].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        views[eye].subImage.swapchain = eye ? right : left;
        views[eye].subImage.imageRect = {{0, 0}, {1024, 1024}};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 2;
    projection.views = views.data();
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    auto frame = [&] {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        Expect(AppWait(&state) == XR_SUCCESS, "Reload test: app wait failed");
        Expect(AppBegin() == XR_SUCCESS, "Reload test: app begin failed");
        framegen.NoteRelease(left, 1);
        framegen.NoteRelease(right, 1);
        XrFrameEndInfo info = FrameEndInfo();
        info.displayTime = state.predictedDisplayTime;
        info.layerCount = 1;
        info.layers = &layer;
        Expect(depthxr::TurboFrameTestPeer::ForwardEndFrame(TestSession(), &info) == XR_SUCCESS,
               "Reload test: app end failed");
        return state;
    };
    for (int i = 0; i < 3; ++i) frame();
    Expect(executes >= 1, "Reload test: Frame Generation did not engage");

    // Turbo on for every application, as a user who had it on would have;
    // Frame Generation stays on at 2x.
    depthxr::ConfigDocument config;
    config.core.enabled = true;
    config.turbo.enabled = true;
    config.turbo.pacing_mode = depthxr::TurboPacingSetting::kSequenced;
    config.turbo.metrics_mode = depthxr::TurboMetricsMode::kOff;
    config.framegen.enabled = true;
    config.framegen.defaults.multiplier = depthxr::FrameGenMultiplier::k2;
    for (int reload = 0; reload < 3; ++reload) {
        config.core.osd.opacity = 80 + reload; // a different document each time
        depthxr::TurboFrameTestPeer::ReloadConfig(config);
        Expect(!depthxr::TurboFrameTestPeer::TurboConfigured() && depthxr::TurboFrameTestPeer::TurboYielded(),
               "A reload while Frame Generation owns pacing turned Turbo back on");
        Expect(!depthxr::TurboFrameTestPeer::TurboInterceptionRequired(),
               "A reload while Frame Generation owns pacing armed Turbo's frame interception");
        for (int i = 0; i < 2; ++i) {
            const XrFrameState state = frame();
            Expect(state.predictedDisplayPeriod == 2 * kFrameGenPeriod,
                   "Frame Generation stopped pacing the app after a reload");
        }
        Expect(!depthxr::TurboFrameTestPeer::TurboEffective(), "A reload engaged Turbo under Frame Generation");
    }
    Expect(depthxr::TurboFrameTestPeer::FailureReason().empty(),
           "A reload while Frame Generation owns pacing recorded a Turbo fault");
    Expect(!depthxr::TurboFrameTestPeer::Handoff().active && depthxr::TurboFrameTestPeer::SequencedState() == 0,
           "A reload while Frame Generation owns pacing left a Turbo pipeline behind");
    bool recovery_written = false;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(recovery, error)) {
        recovery_written |= entry.is_regular_file();
    }
    Expect(!recovery_written, "A reload while Frame Generation owns pacing wrote a Turbo recovery record");
    Expect(runtime.MaxConcurrentWaits() == 1 && runtime.StackedWaits() == 0 && runtime.BeginRejections() == 0 &&
               runtime.EndRejections() == 0,
           "Frame Generation's frames broke the runtime's call order across reloads");
    Expect(depthxr::TurboFrameTestPeer::Snapshot().turbo == "Paused for Frame Generation",
           "The OSD lost Turbo's paused-for-Frame-Generation state across reloads");
    depthxr::TurboFrameTestPeer::StopFrameGen();
    std::filesystem::remove_all(recovery, error);
}

std::size_t Occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// --- Quadviews sessions that began in stereo -------------------------------------

// Why a session with Quadviews on began in stereo. A game that did not enable
// XR_VARJO_quad_views (DCS without Use Quad View) cannot select quad views, so
// restarting it alone changes nothing; the restart advice is for a game that
// could select them but was not offered them.
void TestQuadViewsInactiveReasons() {
    const std::string not_enabled(depthxr::QuadViewsInactiveReason(false, true));
    Expect(not_enabled.find("did not enable XR_VARJO_quad_views") != std::string::npos &&
               not_enabled.find("restarting alone will not change this") != std::string::npos &&
               depthxr::QuadViewsInactiveReason(false, false) == not_enabled,
           "A game that cannot select quad views was not told so");
    const std::string not_offered(depthxr::QuadViewsInactiveReason(true, false));
    Expect(not_offered.find("not offered") != std::string::npos && not_offered.find("restart the app") != std::string::npos,
           "A game that was not offered quad views lost the restart advice");
    const std::string chose_stereo(depthxr::QuadViewsInactiveReason(true, true));
    Expect(chose_stereo.find("chose stereo") != std::string::npos && chose_stereo.find("restart") == std::string::npos,
           "A game that was offered quad views and chose stereo was told to restart");
}

// The OSD tags Quadviews only while quad views can be rendered: before the
// session begins, or once it began with the foveated inset. A Quadviews session
// that began in stereo is not tagged.
void TestQuadViewsOsdTagFollowsBegunConfiguration() {
    using Peer = depthxr::TurboFrameTestPeer;
    const auto tagged = [](std::optional<XrViewConfigurationType> begun) {
        return Peer::QuadViewsSessionModules(begun).find("Quadviews") != std::string::npos;
    };
    Expect(tagged(std::nullopt), "A Quadviews session was not tagged before it began");
    Expect(tagged(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET),
           "A session that began with the foveated inset was not tagged Quadviews");
    Expect(!tagged(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO),
           "A Quadviews session that began in stereo was tagged Quadviews");
}

// --- Native Varjo foveation diagnostics ------------------------------------------

// The focus views' FOV a fake Varjo runtime locates for every display time.
constexpr XrFovf kVarjoFocusFov{-0.3f, 0.3f, 0.25f, -0.25f};
int g_varjo_ends = 0;

XrResult XRAPI_PTR VarjoLocateViews(XrSession, const XrViewLocateInfo*, XrViewState*, std::uint32_t capacity,
                                    std::uint32_t* count, XrView* views) {
    *count = 4;
    if (capacity < 4) return capacity ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
    for (std::uint32_t view = 0; view < 4; ++view) {
        views[view].pose = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        views[view].fov = view < 2 ? XrFovf{-0.9f, 0.9f, 0.9f, -0.9f} : kVarjoFocusFov;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_PTR VarjoEndFrame(XrSession, const XrFrameEndInfo*) {
    ++g_varjo_ends;
    return XR_SUCCESS;
}

// Native Varjo quad views pass through xrEndFrame untouched while Pivot is
// idle and focus sharpening is off. The submitted focus FOV check must still
// count each frame, once, whichever path the frame takes (it sat in the
// projection rewrite only, and read 0/0/0 for such sessions).
void TestVarjoNativeSubmittedFocusCountsEveryFrame() {
    using Peer = depthxr::TurboFrameTestPeer;
    Peer::PrepareVarjoNative(&VarjoLocateViews, &VarjoEndFrame);
    g_varjo_ends = 0;
    std::array<XrCompositionLayerProjectionView, 4> views{};
    for (std::uint32_t view = 0; view < 4; ++view) {
        views[view] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        views[view].pose.orientation.w = 1.0f;
        views[view].fov = view < 2 ? XrFovf{-0.9f, 0.9f, 0.9f, -0.9f} : kVarjoFocusFov;
        views[view].subImage.swapchain = reinterpret_cast<XrSwapchain>(static_cast<std::uintptr_t>(0x81 + view));
        views[view].subImage.imageRect = {{0, 0}, {1024, 1024}};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 4;
    projection.views = views.data();
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);

    // A frame located (unless `located` is false) and submitted for `time`,
    // with its left focus view's left edge at `focus_left`.
    auto frame = [&](XrTime time, bool located, float focus_left) {
        if (located) {
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
            locate.displayTime = time;
            XrViewState state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 4> located_views{};
            for (auto& view : located_views) view = {XR_TYPE_VIEW};
            std::uint32_t count = 0;
            const XrResult result =
                Peer::Layer().LocateViews(TestSession(), &locate, &state, 4, &count, located_views.data());
            Expect(result == XR_SUCCESS && count == 4 && located_views[2].fov.angleLeft == kVarjoFocusFov.angleLeft,
                   "A native Varjo locate did not return the runtime's four views");
        }
        views[2].fov.angleLeft = focus_left;
        XrFrameEndInfo info = FrameEndInfo();
        info.displayTime = time;
        info.layerCount = 1;
        info.layers = &layer;
        Expect(Peer::Layer().EndFrame(TestSession(), &info) == XR_SUCCESS, "A native Varjo frame did not end");
    };
    using Counts = std::array<std::uint64_t, 3>;

    frame(30'000'000'000, true, kVarjoFocusFov.angleLeft);
    Expect(Peer::VarjoSubmittedFocus() == Counts{1, 0, 0},
           "A passed-through native Varjo frame's focus views were not checked");
    frame(30'011'111'111, true, kVarjoFocusFov.angleLeft - 0.01f);
    Expect(Peer::VarjoSubmittedFocus() == Counts{2, 1, 0},
           "A passed-through native Varjo frame's other focus FOV was not counted");
    frame(30'022'222'222, false, kVarjoFocusFov.angleLeft);
    Expect(Peer::VarjoSubmittedFocus() == Counts{2, 1, 1},
           "A native Varjo frame for a display time never located was not counted unlocated");
    // Sharpening sends the frame through the projection rewrite: still one check.
    Peer::SharpenVarjoFocus();
    frame(30'033'333'333, true, kVarjoFocusFov.angleLeft);
    Expect(Peer::VarjoSubmittedFocus() == Counts{3, 1, 1},
           "A rewritten native Varjo frame's focus views were not checked exactly once");
    Expect(g_varjo_ends == 4, "Every native Varjo frame must reach the runtime");
    Peer::EndVarjoNative();
}

// A settings file with a key this layer does not know (a newer app's) still
// loads. Each unknown key is logged once per log: at the first load, not again
// on reloads, and a reload that adds one logs only that one; the next
// instance's new log shows them all again. A reload that fails to parse keeps
// the last good config, as before.
void TestConfigWarningsLoggedOncePerKey() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("vectorxr-config-warnings-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto config = directory / "settings.json";
    int revision = 0;
    const auto write = [&](const std::string& osd) {
        {
            std::ofstream stream(config, std::ios::binary | std::ios::trunc);
            stream << R"({"version":3,"core":{"enabled":true,"osd":)" << osd
                   << R"(},"applications":[],"modules":{"depthxr":{"enabled":false},"pivotxr":{"enabled":false},)"
                   << R"("framegen":{"enabled":false,"presenter":true}}})";
        }
        // A distinct write time for every revision, as the watcher compares them.
        std::filesystem::last_write_time(config, std::filesystem::file_time_type::clock::now() +
                                                     std::chrono::seconds(++revision));
    };
    const std::string presenter = "Config warning: Unknown field ignored: modules.framegen.presenter";
    const std::string later = "Config warning: Unknown field ignored: core.osd.later";

    write(R"({"scale":40})");
    const auto log = depthxr::TurboFrameTestPeer::LoadConfigFile(config, directory / "config.log");
    Expect(depthxr::TurboFrameTestPeer::LoadedConfig().core.osd.scale == 40,
           "A config with an unknown key did not load");
    Expect(Occurrences(depthxr::TurboFrameTestPeer::Log(), presenter) == 1,
           "The first load did not log its unknown key once");

    write(R"({"scale":50})");
    depthxr::TurboFrameTestPeer::PollConfigFile();
    std::string text = depthxr::TurboFrameTestPeer::Log();
    Expect(depthxr::TurboFrameTestPeer::LoadedConfig().core.osd.scale == 50 &&
               text.find("Reloaded config from") != std::string::npos,
           "A reload with the same unknown key did not apply");
    Expect(Occurrences(text, presenter) == 1, "A reload logged an already-logged unknown key again");

    write(R"({"scale":60,"later":true})");
    depthxr::TurboFrameTestPeer::PollConfigFile();
    text = depthxr::TurboFrameTestPeer::Log();
    Expect(depthxr::TurboFrameTestPeer::LoadedConfig().core.osd.scale == 60 && Occurrences(text, later) == 1 &&
               Occurrences(text, presenter) == 1,
           "A reload that adds an unknown key did not log only that key, once");

    write(R"({"scale":"big","later":true})");
    depthxr::TurboFrameTestPeer::PollConfigFile();
    text = depthxr::TurboFrameTestPeer::Log();
    Expect(text.find("Failed to parse config: scale must be a number") != std::string::npos &&
               depthxr::TurboFrameTestPeer::LoadedConfig().core.osd.scale == 60,
           "A wrong type beside unknown keys did not fail the reload and keep the last good config");
    Expect(Occurrences(text, later) == 1 && Occurrences(text, presenter) == 1,
           "A failed reload logged unknown keys");

    // The next instance begins a log of its own, which shows each unknown key
    // again, once.
    write(R"({"scale":70,"later":true})");
    const auto next_log = depthxr::TurboFrameTestPeer::LoadConfigFile(config, directory / "next-instance.log");
    text = depthxr::TurboFrameTestPeer::Log();
    Expect(next_log != log && depthxr::TurboFrameTestPeer::LoadedConfig().core.osd.scale == 70,
           "The next instance did not load the settings into a new log");
    Expect(Occurrences(text, presenter) == 1 && Occurrences(text, later) == 1,
           "The next instance's log did not show each unknown key once");

    depthxr::TurboFrameTestPeer::UnloadConfigFile(next_log);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

int main() {
    TestHandoverKeepsBlockedPollShielded();
    // Runtime-profile and fault-recovery simulations: none may stop frame
    // forwarding, block the application unboundedly, or stack a runtime wait.
    TestConformantLateWaitIsWaitedFor();
    TestStuckWaitDropsFrameThenResumes();
    TestInterlockedRuntimeAutoFallsBackToSequenced();
    TestInterlockedForcedAsyncSuspendsAndKeepsRunning();
    TestOwnershipDesyncRecoversInSession();
    TestFailedBeginProbesBeforeWaiting(false);
    TestFailedBeginProbesBeforeWaiting(true);
    TestAsyncWaitFailureRecoversOnFrameThread();
    TestCompensationWaitFailureRetriesNextFrame();
    TestSequencedPostSubmitFailureRecovers(false);
    TestSequencedPostSubmitFailureRecovers(true);
    for (const auto error : {XR_ERROR_SESSION_LOST, XR_ERROR_INSTANCE_LOST, XR_ERROR_SESSION_NOT_RUNNING})
        TestSessionEndingWaitDoesNotCreateSafetyFault(error);
    TestRejectedEndRecovers(false);
    TestRejectedEndRecovers(true);
    TestPersistentFailuresSuspendWithoutStoppingFrames();
    TestSessionEndReleasesInterlockedWait();
    TestSessionEndWithStuckWaitIsBounded();
    TestWorkerStopAfterStuckSessionEndIsBounded();
    TestHealthyPipelineRetainsStrictOrder(depthxr::TurboPacingMode::kAsync);
    TestHealthyPipelineRetainsStrictOrder(depthxr::TurboPacingMode::kSequenced);
    TestFrameGenOwnsFramePacing();
    TestConfigReloadWhileFrameGenOwnsPacing();
    TestAsyncTogglePreservesQueuedFrame(false);
    TestAsyncTogglePreservesQueuedFrame(true);
    TestAsyncReleaseHandsOverToFrameThread(false);
    TestAsyncReleaseHandsOverToFrameThread(true);
    TestAsyncRecoupleStallReleasesPipeline();
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
    TestSequencedStallPolicySuspends();
    Expect(depthxr::TurboFrameTestPeer::OsdStateTransitions(), "OSD lost enabled/off Turbo or applied/ready/disabled Pivot state");
    TestVarjoNativeSubmittedFocusCountsEveryFrame();
    TestQuadViewsInactiveReasons();
    TestQuadViewsOsdTagFollowsBegunConfiguration();
    // Last: it points the layer's log at a file of its own.
    TestConfigWarningsLoggedOncePerKey();
    std::cout << "depthxr_turbo_frame_tests passed\n";
    return 0;
}
