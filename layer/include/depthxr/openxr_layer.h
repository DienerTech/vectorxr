#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/config_parser.h"
#include "depthxr/effects.h"
#include "depthxr/logger.h"
#include "depthxr/settings_resolver.h"

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11PixelShader;
struct ID3D11Query;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11VertexShader;

namespace depthxr {

class OpenXrLayer {
  public:
    static OpenXrLayer& Instance();

    void SetLayerDirectory(std::filesystem::path dll_directory);
    void SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr);

    struct InstanceCreateDiagnostics {
        bool app_requested_quad_views{false};
        bool app_requested_varjo_foveated_rendering{false};
        bool app_requested_eye_gaze{false};
        bool cheap_eye_gaze_probe_ran{false};
        bool cheap_eye_gaze_probe_supported{false};
        bool optimistic_eye_gaze_request{false};
        XrResult first_create_result{XR_SUCCESS};
        bool retried_without_eye_gaze{false};
        XrResult retry_create_result{XR_SUCCESS};
        uint32_t first_downstream_extension_count{0};
        uint32_t final_downstream_extension_count{0};
        // True when the layer forwarded the native Varjo quad-views extensions to
        // the runtime (Varjo compatible quadviews) instead of stripping them for
        // stereo-composite emulation.
        bool varjo_compatible_quad_forwarded{false};
        // Diagnostics for WHY Varjo compatible forwarding did/did not happen. The
        // forward requires eligible && the runtime advertising XR_VARJO_quad_views
        // to a pre-instance extension probe. These capture each input plus the raw
        // extension list the probe saw, so a truncation-free (short-session) capture
        // pinpoints the cause.
        bool varjo_compatible_eligible{false};
        bool runtime_advertises_varjo_quad{false};
        bool runtime_advertises_varjo_foveated{false};
        bool pre_instance_extension_scan_ran{false};
        XrResult pre_instance_extension_scan_result{XR_SUCCESS};
        uint32_t pre_instance_extension_count{0};
        std::string pre_instance_extensions;
        // Authoritative Varjo signal: the active OpenXR runtime (from the registry).
        // This, not the extension probe, now drives the forward decision.
        bool active_runtime_is_varjo{false};
        std::string active_runtime_path;
    };

    // True when VectorXR quadviews should run in Varjo compatible mode, i.e. core
    // and the quadviews module are enabled. Safe to call at instance-creation time
    // (loads config lazily). Does not consider runtime capability — the caller
    // pairs this with a runtime extension probe.
    bool IsVarjoCompatibleQuadviewsEligible();

    XrResult OnInstanceCreated(const XrInstanceCreateInfo* create_info,
                               XrInstance instance,
                               bool eye_gaze_extension_enabled,
                               const InstanceCreateDiagnostics& diagnostics);
    XrResult GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
    XrResult DestroyInstance(XrInstance instance);
    XrResult CreateSession(XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session);
    XrResult DestroySession(XrSession session);
    XrResult BeginSession(XrSession session, const XrSessionBeginInfo* begin_info);
    XrResult AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attach_info);
    XrResult SyncActions(XrSession session, const XrActionsSyncInfo* sync_info);
    XrResult WaitFrame(XrSession session, const XrFrameWaitInfo* frame_wait_info, XrFrameState* frame_state);
    XrResult BeginFrame(XrSession session, const XrFrameBeginInfo* frame_begin_info);
    XrResult EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info);
    XrResult GetSystemProperties(XrInstance instance, XrSystemId system_id, XrSystemProperties* properties);
    XrResult EnumerateEnvironmentBlendModes(XrInstance instance,
                                            XrSystemId system_id,
                                            XrViewConfigurationType view_configuration_type,
                                            uint32_t environment_blend_mode_capacity_input,
                                            uint32_t* environment_blend_mode_count_output,
                                            XrEnvironmentBlendMode* environment_blend_modes);
    XrResult EnumerateViewConfigurations(XrInstance instance,
                                         XrSystemId system_id,
                                         uint32_t view_configuration_type_capacity_input,
                                         uint32_t* view_configuration_type_count_output,
                                         XrViewConfigurationType* view_configuration_types);
    XrResult GetViewConfigurationProperties(XrInstance instance,
                                            XrSystemId system_id,
                                            XrViewConfigurationType view_configuration_type,
                                            XrViewConfigurationProperties* configuration_properties);
    XrResult EnumerateViewConfigurationViews(XrInstance instance,
                                             XrSystemId system_id,
                                             XrViewConfigurationType view_configuration_type,
                                             uint32_t view_capacity_input,
                                             uint32_t* view_count_output,
                                             XrViewConfigurationView* views);
    XrResult EnumerateSwapchainFormats(XrSession session,
                                       uint32_t format_capacity_input,
                                       uint32_t* format_count_output,
                                       int64_t* formats);
    XrResult CreateSwapchain(XrSession session, const XrSwapchainCreateInfo* create_info, XrSwapchain* swapchain);
    XrResult DestroySwapchain(XrSwapchain swapchain);
    XrResult EnumerateSwapchainImages(XrSwapchain swapchain,
                                      uint32_t image_capacity_input,
                                      uint32_t* image_count_output,
                                      XrSwapchainImageBaseHeader* images);
    XrResult AcquireSwapchainImage(XrSwapchain swapchain,
                                   const XrSwapchainImageAcquireInfo* acquire_info,
                                   uint32_t* index);
    XrResult WaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* wait_info);
    XrResult ReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* release_info);
    XrResult EnumerateReferenceSpaces(XrSession session,
                                      uint32_t space_capacity_input,
                                      uint32_t* space_count_output,
                                      XrReferenceSpaceType* spaces);
    XrResult GetReferenceSpaceBoundsRect(XrSession session,
                                         XrReferenceSpaceType reference_space_type,
                                         XrExtent2Df* bounds);
    XrResult CreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* create_info, XrSpace* space);
    XrResult DestroySpace(XrSpace space);
    XrResult LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location);
    XrResult LocateViews(XrSession session,
                         const XrViewLocateInfo* view_locate_info,
                         XrViewState* view_state,
                         uint32_t view_capacity_input,
                         uint32_t* view_count_output,
                         XrView* views);

  private:
    OpenXrLayer() = default;
    ~OpenXrLayer();

    struct SwapchainInfo {
        XrSession session{XR_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t array_size{0};
        uint32_t mip_count{0};
        uint32_t sample_count{0};
        int64_t format{0};
        XrSwapchainUsageFlags usage_flags{0};
        XrSwapchainCreateFlags create_flags{0};
        uint32_t image_count{0};
        uint64_t acquire_count{0};
        uint64_t wait_count{0};
        uint64_t release_count{0};
        uint32_t last_acquired_image_index{0};
        bool images_enumerated{false};
        bool has_last_acquired_image_index{false};
        bool release_deferred{false};
        bool quadviews_session{false};
        bool d3d11_shader_resources_attempted{false};
        bool d3d11_shader_resources_available{false};
        std::vector<ID3D11Texture2D*> d3d11_images;
        std::vector<ID3D11ShaderResourceView*> d3d11_shader_resources;
    };

    struct QuadViewsCompositionTarget {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        uint32_t width{0};
        uint32_t height{0};
        int64_t format{0};
        uint32_t image_count{0};
        std::vector<ID3D11Texture2D*> d3d11_images;
        std::vector<ID3D11RenderTargetView*> image_render_target_views;
        ID3D11Texture2D* render_texture{nullptr};
        ID3D11RenderTargetView* render_target_view{nullptr};
    };

    struct QuadViewsInputCopy {
        uint32_t width{0};
        uint32_t height{0};
        int64_t format{0};
        ID3D11Texture2D* texture{nullptr};
        ID3D11ShaderResourceView* shader_resource{nullptr};
    };

    struct QuadViewsGpuTimingQuery {
        ID3D11Query* disjoint{nullptr};
        ID3D11Query* start{nullptr};
        ID3D11Query* end{nullptr};
        bool issued{false};
        XrTime frame_time{0};
    };

    struct D3D11QuadViewsCompositor {
        ID3D11Device* device{nullptr};
        ID3D11DeviceContext* context{nullptr};
        ID3D11VertexShader* vertex_shader{nullptr};
        ID3D11PixelShader* pixel_shader{nullptr};
        ID3D11SamplerState* sampler{nullptr};
        ID3D11BlendState* blend_state{nullptr};
        ID3D11Buffer* constants{nullptr};
        bool initialized{false};
        bool failed{false};
        bool gpu_timing_available{false};
        bool has_logged_capabilities{false};
        bool has_logged_prewarm{false};
        bool has_last_completed_gpu_timing{false};
        uint32_t failure_logs_remaining{8};
        uint32_t next_gpu_timing_query{0};
        double last_completed_gpu_ms{0.0};
        XrTime last_completed_gpu_frame_time{0};
        std::array<QuadViewsInputCopy, 4> input_copies;
        std::array<QuadViewsCompositionTarget, 2> targets;
        std::array<QuadViewsGpuTimingQuery, 4> gpu_timing_queries;
    };

    // Standalone focus-view sharpen used only in Varjo compatible quadviews. The
    // compositor is not running in that mode (the runtime drives the panels), so
    // when foveate_sharpness > 0 we run a 1:1 CAS pass over the app's focus views
    // (2/3) into our own swapchains and repoint the submitted layer at them.
    struct D3D11FocusSharpen {
        ID3D11VertexShader* vertex_shader{nullptr};
        ID3D11PixelShader* pixel_shader{nullptr};
        ID3D11SamplerState* sampler{nullptr};
        ID3D11Buffer* constants{nullptr};
        bool initialized{false};
        bool failed{false};
        bool has_logged_active{false};
        uint32_t failure_logs_remaining{8};
        std::array<QuadViewsCompositionTarget, 2> targets;
    };

    struct PivotDiagnosticState {
        bool has_view_pose{false};
        XrTime view_time{0};
        XrSpaceLocationFlags view_location_flags{0};
        bool pivot_active{false};
        bool space_is_view{false};
        bool base_space_is_view{false};
        double raw_yaw_radians{0.0};
        double raw_pitch_radians{0.0};
        double steady_extra_yaw_radians{0.0};
        double steady_extra_pitch_radians{0.0};
        double eased_extra_yaw_radians{0.0};
        double eased_extra_pitch_radians{0.0};
        double activation_gain{0.0};
        // Origin-relative measurement context: raw_yaw/raw_pitch above are
        // measured against this origin when active.
        bool origin_active{false};
        double origin_yaw_radians{0.0};
        double origin_pitch_radians{0.0};
        // Stepped response state (0 when continuous or centered).
        int yaw_step{0};
        int pitch_step{0};
        std::string_view recomposition_mode{"none"};

        bool has_eye_offsets{false};
        XrTime eye_offsets_time{0};
        XrViewConfigurationType eye_offsets_view_configuration{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
        uint32_t eye_offset_count{0};
        std::array<XrPosef, 4> eye_offsets{};
    };

    void ReloadConfigIfNeeded();
    void StartConfigWatcher();
    void StopConfigWatcher();
    void ConfigWatcherLoop();
    void PollConfigFile();
    void RefreshResolvedSettings();
    void CaptureInstanceFunctions();
    void LogResolvedSettings(const ResolvedRuntimeConfig& settings);
    void ResetPivotActivationState();
    void ResetDepthToggleState();
    bool IsPivotXrActive();
    const PivotXrResolvedProfile& ActivePivotProfile() const;
    bool IsDepthXrActive();
    // Turbo mode (async frame pacing). IsTurboActive polls the toggle binding
    // and applies the Varjo deferred-release guard; ForwardEndFrame wraps
    // next_end_frame_ with the deferred-begin / async-wait dance; DrainTurbo*
    // must run before teardown that could strand the wait thread.
    bool IsTurboActive();
    void ResetTurboToggleState();
    // Pacing-strategy resolution and discovery. ResolveTurboPacingModeLocked
    // runs under the config lock on the frame thread when turbo first engages
    // (and again after a config change); the verdict/stall helpers run on the
    // frame thread after the lock is released.
    void ResolveTurboPacingModeLocked();
    void RecordTurboPacingVerdict(TurboPacingMode mode, const char* source, std::int64_t stable_seconds);
    void NoteTurboPacingStableFrame(std::chrono::steady_clock::time_point now);
    // Returns true when turbo should suspend for the session (level-2 trip or
    // non-auto mode); false when it adapted (async -> sequenced fallback).
    bool HandleTurboDrainTimeout(std::chrono::steady_clock::time_point now);
    // Releases config_lock (mutex_) before any blocking runtime call so
    // xrLocateViews/xrLocateSpace from other app threads are never serialized
    // behind the compositor pacing wait. Call as the tail of EndFrame only.
    XrResult ForwardEndFrame(XrSession session,
                             const XrFrameEndInfo* frame_end_info,
                             std::unique_lock<std::mutex>& config_lock);
    void DrainTurboAsyncWait();
    void ResetTurboFrameState();
    // Frame pacing telemetry: logs a summary line (debug level) every ~5s so a
    // judder report can be localized to our drain, the runtime's end-frame, the
    // runtime's wait pacing, or upstream of the layer entirely.
    void RecordFramePacing(std::chrono::steady_clock::time_point frame_start,
                           std::chrono::steady_clock::time_point after_drain,
                           std::chrono::steady_clock::time_point after_end,
                           bool turbo_engaged);
    bool IsQuadViewsActive() const;
    void ResetSwapchainState();
    void LogSwapchainSummary(XrSwapchain swapchain, const SwapchainInfo& info, std::string_view event_name);
    bool ShouldDeferSwapchainRelease(const SwapchainInfo& info) const;
    XrResult FlushDeferredSwapchainReleaseLocked(XrSwapchain swapchain,
                                                 SwapchainInfo& info,
                                                 std::string_view reason);
    XrResult FlushDeferredSwapchainReleasesLocked(std::string_view reason);
    bool ShouldLogQuadViewsDebugHeartbeat(std::optional<std::chrono::steady_clock::time_point>& last_heartbeat);
    void ResetQuadViewsDebugHeartbeatState();
    void ResetSessionState();
    void ResetInstanceState();
    void ResetD3D11QuadViewsCompositor();
    XrResult CreateInternalReferenceSpaces(XrSession session);
    void DestroyInternalReferenceSpaces();
    XrResult CreateEyeGazeResources(XrSession session);
    void DestroyEyeGazeResources();
    bool LocateEyeGazeFocusOffsets(XrSession session,
                                   XrSpace base_space,
                                   XrTime time,
                                   const QuadViewsResolvedSettings& settings,
                                   double* yaw_radians,
                                   double* pitch_radians);
    bool EnsureEyeOffsets(XrSession session,
                          XrViewConfigurationType view_configuration_type,
                          XrTime display_time,
                          uint32_t view_count);
    void CachePivotPoseDelta(XrTime time, const XrPosef& pose_delta);
    bool FindPivotPoseDelta(XrTime time, XrPosef* pose_delta, XrTime* matched_time) const;
    void PrunePivotPoseDeltas(XrTime time);
    void CacheQuadViewsFovs(XrTime time, std::span<const XrView> views);
    bool FindQuadViewsFovs(XrTime time, std::array<XrFovf, 4>* fovs, XrTime* matched_time) const;
    void PruneQuadViewsFovs(XrTime time);
    bool IsTrackedViewSpace(XrSpace space) const;
    XrResult LocateRuntimeViews(XrSession session,
                                const XrViewLocateInfo* view_locate_info,
                                XrViewState* view_state,
                                uint32_t view_capacity_input,
                                uint32_t* view_count_output,
                                XrView* views,
                                bool* synthesized_quad_views);
    bool SynthesizeQuadViewsFromStereo(std::span<const XrView> stereo_views,
                                       const QuadViewsResolvedSettings& quadviews_settings,
                                       double focus_yaw_radians,
                                       double focus_pitch_radians,
                                       uint32_t view_capacity_input,
                                       uint32_t* view_count_output,
                                       XrView* views) const;
    XrResult LocateSpaceWithPivot(XrSpace space,
                                  XrSpace base_space,
                                  XrTime time,
                                  bool pivotxr_active,
                                  XrSpaceLocation* location,
                                  double* applied_extra_yaw_radians,
                                  double* applied_extra_pitch_radians,
                                  XrPosef* applied_pose_delta,
                                  bool update_smoothing);
    bool EnsureD3D11QuadViewsCompositor(const XrCompositionLayerProjection* projection_layer,
                                        uint32_t output_width,
                                        uint32_t output_height,
                                        int64_t output_format);
    bool EnsureD3D11SwapchainShaderResources(SwapchainInfo& swapchain);
    void TryPrewarmD3D11QuadViewsCompositor();
    bool ComposeQuadViewsD3D11(const XrCompositionLayerProjection* source_layer,
                               XrTime display_time,
                               const XrPosef& reverse_delta,
                               bool has_non_identity_delta,
                               XrCompositionLayerProjection* composed_layer,
                               std::vector<XrCompositionLayerProjectionView>* composed_views);

    // Varjo compatible quadviews focus sharpen (see D3D11FocusSharpen).
    void ResetD3D11FocusSharpen();
    bool EnsureD3D11FocusSharpen();
    bool EnsureFocusSharpenTarget(QuadViewsCompositionTarget& target,
                                  uint32_t width,
                                  uint32_t height,
                                  int64_t format);
    // Sharpens native focus views (indices 2/3) in place: renders each into one of
    // our swapchains and repoints its subImage. Best-effort — any view that cannot
    // be sharpened is left untouched (submits the app's unsharpened focus).
    void SharpenNativeFocusViews(std::vector<XrCompositionLayerProjectionView>& views, XrTime display_time);

    std::mutex mutex_;
    std::filesystem::path dll_directory_;
    std::filesystem::path config_path_;
    std::filesystem::path log_path_;
    std::filesystem::file_time_type last_config_write_time_{};
    std::filesystem::file_time_type last_failed_config_write_time_{};
    bool has_config_timestamp_{false};
    bool has_failed_config_timestamp_{false};

    Logger logger_;
    ConfigDocument config_;
    bool has_loaded_config_{false};
    uint64_t config_generation_{0};
    uint64_t resolved_settings_generation_{~0ull};
    XrSession resolved_settings_session_{XR_NULL_HANDLE};
    std::string last_failed_config_error_;

    // Config hot-reload runs on a dedicated watcher thread so that filesystem
    // latency never blocks the render hot path. The watcher does all I/O without
    // holding mutex_, taking it only briefly to publish a parsed change.
    std::thread config_watcher_thread_;
    std::mutex config_watcher_mutex_;
    std::condition_variable config_watcher_cv_;
    bool config_watcher_stop_{false};
    std::string runtime_name_;
    std::string current_exe_name_;
    ResolvedRuntimeConfig resolved_settings_;
    std::optional<ResolvedRuntimeConfig> last_logged_settings_;
    uint64_t locate_views_call_count_{0};
    uint32_t pending_locate_views_diagnostics_{0};
    uint32_t pending_end_frame_diagnostics_{0};
    uint32_t pending_eye_gaze_diagnostics_{0};
    uint32_t pending_eye_gaze_sync_diagnostics_{0};
    uint32_t pending_quadviews_compositor_diagnostics_{0};
    uint32_t eye_gaze_diagnostic_stride_counter_{0};
    uint32_t pending_pivot_diagnostics_{0};
    uint64_t pivot_diagnostic_stride_counter_{0};
    std::optional<std::chrono::steady_clock::time_point> last_quadviews_locate_debug_heartbeat_;
    std::optional<std::chrono::steady_clock::time_point> last_quadviews_end_frame_debug_heartbeat_;
    std::optional<std::chrono::steady_clock::time_point> last_quadviews_compositor_debug_heartbeat_;
    std::optional<std::chrono::steady_clock::time_point> last_quadviews_eye_gaze_debug_heartbeat_;
    PivotDiagnosticState pivot_diagnostic_;
    double pivotxr_smoothed_extra_yaw_radians_{0.0};
    double pivotxr_smoothed_extra_pitch_radians_{0.0};
    // Stepped response mode: signed persistent step per axis (with hysteresis).
    int pivotxr_yaw_step_{0};
    int pivotxr_pitch_step_{0};
    // Activation envelope in [0,1]: eases the pivot effect in/out on the
    // enable/disable transition so toggling never snaps the view, independent
    // of the per-frame tracking smoothing.
    double pivotxr_activation_gain_{0.0};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_last_smoothing_wall_time_;
    // Per resolved-profile input edge state, index-aligned with
    // resolved_settings_.pivotxr.profiles. Arbitration is last-pressed-wins:
    // pressing another candidate's binding retargets the engaged profile and
    // the activation envelope carries the view across the switch.
    struct PivotProfileInputState {
        bool was_down{false};
        bool down_cached{false};
        bool set_origin_was_down{false};
        bool set_origin_down_cached{false};
        bool release_origin_was_down{false};
        bool release_origin_down_cached{false};
        // Always-on profiles only: true after the user pressed the binding to
        // suspend the automatic engagement.
        bool always_on_suspended{false};
    };
    std::vector<PivotProfileInputState> pivotxr_profile_input_states_;
    // Engaged profile index; retains the last-engaged profile during the
    // release ramp so the easing keeps using that profile's settings.
    size_t pivotxr_active_profile_index_{0};
    bool pivotxr_engaged_{false};
    // Optional captured pivot origin: head yaw/pitch (radians) in the app's
    // reference space at the moment the set-origin binding fired. Empty means
    // the default HMD/reference-space origin. Capture happens on the
    // xrLocateViews drive path so the pose shares the frame's displayTime
    // pipeline with the pivot drive.
    struct PivotOrigin {
        double yaw_radians{0.0};
        double pitch_radians{0.0};
    };
    std::optional<PivotOrigin> pivotxr_origin_;
    bool pivotxr_origin_capture_pending_{false};
    bool depthxr_toggle_enabled_{true};
    bool depthxr_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_binding_last_poll_time_;
    std::optional<std::chrono::steady_clock::time_point> depthxr_binding_last_poll_time_;
    bool depthxr_binding_down_cached_{false};

    // Turbo mode. The runtime always sees a conformant wait->begin->end
    // sequence; the application is decoupled from runtime frame pacing: its
    // xrWaitFrame returns immediately with a fabricated predictedDisplayTime
    // (exactly one frame of pipelining), its xrBeginFrame becomes a no-op, and
    // the real wait+begin happen inside xrEndFrame. turbo_mutex_ guards this
    // state; it is never held across a blocking runtime wait except when
    // spec-ordering requires it (second poll, teardown drains). mutex_ must
    // NOT be required by WaitFrame/BeginFrame, which stay off the config lock.
    std::mutex turbo_mutex_;
    std::future<void> turbo_async_wait_;
    bool turbo_async_wait_polled_{false};
    bool turbo_async_wait_completed_{false};
    XrTime turbo_last_predicted_display_time_{0};
    XrDuration turbo_last_predicted_display_period_{0};
    bool turbo_last_should_render_{true};
    XrTime turbo_max_returned_display_time_{0};
    std::optional<std::chrono::steady_clock::time_point> turbo_last_wait_frame_wall_time_;
    // Toggle binding state (mutex_-guarded, polled from EndFrame like Depth's).
    bool turbo_toggle_enabled_{true};
    bool turbo_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> turbo_binding_last_poll_time_;
    bool turbo_binding_down_cached_{false};
    bool has_logged_turbo_varjo_note_{false};
    // Info once per pipelining engage/release cycle; small Debug budget for the
    // first fabricated xrWaitFrame returns of each cycle.
    bool turbo_pipelining_logged_{false};
    int turbo_fabricated_wait_log_budget_{0};
    // Self-healing: some runtimes interlock xrWaitFrame with the next submit
    // (observed on Pimax's PiOpenXR, Oculus, and Varjo) so the pipelined wait
    // stalls until our drain timeout. Timeouts are counted in a rolling window
    // (they interleave with clean frames, so a consecutive streak never
    // trips). Two-level circuit: async pacing trips into sequenced pacing
    // (under Auto), sequenced pacing trips into a session suspend that the
    // toggle binding re-arms.
    int turbo_drain_timeout_count_{0};
    std::optional<std::chrono::steady_clock::time_point> turbo_timeout_window_start_;
    std::atomic<bool> turbo_auto_suspended_{false};

    // Pacing strategy state. Source records how the active mode was chosen —
    // forced by settings, pinned per runtime, seeded from the known-runtime
    // table, read back from a recorded sidecar verdict, probing an unknown
    // runtime, or the in-session fallback after async tripped.
    enum class TurboPacingSource { kForced, kPinned, kPreset, kDiscovered, kProbing, kFallback };
    // Frame-submission-thread only (resolved under the config lock at
    // EndFrame, consumed after it is released) — except turbo_frame_prebegun_,
    // which WaitFrame/BeginFrame consult under turbo_mutex_.
    TurboPacingMode turbo_pacing_mode_{TurboPacingMode::kAsync};
    TurboPacingSource turbo_pacing_source_{TurboPacingSource::kProbing};
    bool turbo_pacing_resolved_{false};
    // Auto only: a stable window is still owed before the verdict is written.
    bool turbo_pacing_verdict_pending_{false};
    std::int64_t turbo_probe_timeout_total_{0};
    std::optional<std::chrono::steady_clock::time_point> turbo_stable_since_;
    // Sequenced pacing (turbo_mutex_): the runtime frame the app is about to
    // render was already waited+begun inside the previous EndFrame.
    bool turbo_frame_prebegun_{false};
    std::string runtime_version_;

    // Frame pacing telemetry (debug log level): quantifies judder sources by
    // timing the frame loop. EndFrame-side fields are only touched from the
    // app's frame-submission thread (frame calls are app-serialized);
    // WaitFrame-side counters live under turbo_mutex_.
    std::optional<std::chrono::steady_clock::time_point> pacing_last_end_time_;
    std::optional<std::chrono::steady_clock::time_point> pacing_window_start_;
    uint32_t pacing_frames_{0};
    double pacing_delta_sum_ms_{0.0};
    double pacing_delta_max_ms_{0.0};
    double pacing_drain_sum_ms_{0.0};
    double pacing_drain_max_ms_{0.0};
    double pacing_end_sum_ms_{0.0};
    double pacing_end_max_ms_{0.0};
    // Guarded by turbo_mutex_.
    double pacing_wait_sum_ms_{0.0};
    double pacing_wait_max_ms_{0.0};
    uint32_t pacing_wait_samples_{0};
    uint32_t pacing_fabricated_waits_{0};
    bool quad_views_extension_requested_{false};
    bool varjo_foveated_rendering_extension_requested_{false};
    bool eye_gaze_extension_enabled_{false};
    // True for the life of the instance when native Varjo quad-views were
    // forwarded to the runtime (Varjo compatible quadviews). Single source of truth
    // that suppresses quad->stereo synthesis, stereo composite, and combined-eye
    // emulation on this instance.
    bool varjo_compatible_quadviews_active_{false};
    bool defer_quadviews_swapchain_releases_{false};
    XrSession active_session_{XR_NULL_HANDLE};
    XrSpace internal_local_space_{XR_NULL_HANDLE};
    XrSpace internal_view_space_{XR_NULL_HANDLE};
    XrSpace internal_stage_space_{XR_NULL_HANDLE};
    XrActionSet quadviews_action_set_{XR_NULL_HANDLE};
    XrAction quadviews_eye_gaze_action_{XR_NULL_HANDLE};
    XrSpace quadviews_eye_gaze_space_{XR_NULL_HANDLE};
    XrPath eye_gaze_interaction_profile_path_{XR_NULL_PATH};
    XrPath eye_gaze_pose_path_{XR_NULL_PATH};
    bool eye_gaze_resources_ready_{false};
    bool eye_gaze_action_set_attached_{false};
    bool has_logged_eye_gaze_focus_active_{false};
    bool has_logged_eye_gaze_focus_unavailable_{false};
    uint32_t eye_gaze_unavailable_streak_{0};
    double quadviews_smoothed_focus_yaw_radians_{0.0};
    double quadviews_smoothed_focus_pitch_radians_{0.0};
    std::optional<std::chrono::steady_clock::time_point> quadviews_last_focus_smoothing_wall_time_;
    XrViewConfigurationType active_primary_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrViewConfigurationType active_runtime_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    bool has_active_primary_view_configuration_{false};
    bool has_logged_quad_view_short_count_{false};
    bool has_logged_pivotxr_spike_mode_{false};
    bool has_logged_quadviews_view_configuration_capabilities_{false};
    bool has_logged_varjo_compatible_view_sizes_{false};
    bool has_logged_system_properties_{false};
    uint32_t cached_quadviews_stereo_recommended_width_{0};
    uint32_t cached_quadviews_stereo_recommended_height_{0};
    uint32_t cached_quadviews_stereo_max_width_{0};
    uint32_t cached_quadviews_stereo_max_height_{0};
    std::unordered_set<XrSpace> tracked_view_spaces_;
    std::unordered_set<XrSpace> tracked_local_spaces_;
    std::unordered_set<XrSpace> tracked_stage_spaces_;
    std::vector<XrPosef> cached_eye_offset_poses_;
    XrTime cached_eye_offsets_display_time_{0};
    XrViewConfigurationType cached_eye_offsets_view_configuration_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    std::optional<std::chrono::steady_clock::time_point> last_app_action_sync_time_;
    std::vector<ViewAdjustmentData> locate_views_original_scratch_;
    std::vector<ViewAdjustmentData> locate_views_adjusted_scratch_;
    std::vector<std::vector<XrCompositionLayerProjectionView>> end_frame_projection_views_scratch_;
    std::vector<XrCompositionLayerProjection> end_frame_projection_layers_scratch_;
    std::vector<const XrCompositionLayerBaseHeader*> end_frame_layers_scratch_;
    std::map<XrTime, XrPosef> cached_pivot_pose_deltas_;
    std::map<XrTime, std::array<XrFovf, 4>> cached_quadviews_fovs_;
    std::unordered_map<XrSwapchain, SwapchainInfo> tracked_swapchains_;
    D3D11QuadViewsCompositor d3d11_quadviews_compositor_;
    D3D11FocusSharpen d3d11_focus_sharpen_;

    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr_{nullptr};
    PFN_xrGetInstanceProperties next_get_instance_properties_{nullptr};
    PFN_xrDestroyInstance next_destroy_instance_{nullptr};
    PFN_xrCreateSession next_create_session_{nullptr};
    PFN_xrDestroySession next_destroy_session_{nullptr};
    PFN_xrBeginSession next_begin_session_{nullptr};
    PFN_xrAttachSessionActionSets next_attach_session_action_sets_{nullptr};
    PFN_xrSyncActions next_sync_actions_{nullptr};
    PFN_xrWaitFrame next_wait_frame_{nullptr};
    PFN_xrBeginFrame next_begin_frame_{nullptr};
    PFN_xrEndFrame next_end_frame_{nullptr};
    PFN_xrGetSystemProperties next_get_system_properties_{nullptr};
    PFN_xrEnumerateEnvironmentBlendModes next_enumerate_environment_blend_modes_{nullptr};
    PFN_xrEnumerateViewConfigurations next_enumerate_view_configurations_{nullptr};
    PFN_xrGetViewConfigurationProperties next_get_view_configuration_properties_{nullptr};
    PFN_xrEnumerateViewConfigurationViews next_enumerate_view_configuration_views_{nullptr};
    PFN_xrEnumerateSwapchainFormats next_enumerate_swapchain_formats_{nullptr};
    PFN_xrCreateSwapchain next_create_swapchain_{nullptr};
    PFN_xrDestroySwapchain next_destroy_swapchain_{nullptr};
    PFN_xrEnumerateSwapchainImages next_enumerate_swapchain_images_{nullptr};
    PFN_xrAcquireSwapchainImage next_acquire_swapchain_image_{nullptr};
    PFN_xrWaitSwapchainImage next_wait_swapchain_image_{nullptr};
    PFN_xrReleaseSwapchainImage next_release_swapchain_image_{nullptr};
    PFN_xrEnumerateReferenceSpaces next_enumerate_reference_spaces_{nullptr};
    PFN_xrGetReferenceSpaceBoundsRect next_get_reference_space_bounds_rect_{nullptr};
    PFN_xrCreateReferenceSpace next_create_reference_space_{nullptr};
    PFN_xrCreateActionSpace next_create_action_space_{nullptr};
    PFN_xrDestroySpace next_destroy_space_{nullptr};
    PFN_xrLocateSpace next_locate_space_{nullptr};
    PFN_xrLocateViews next_locate_views_{nullptr};
    PFN_xrStringToPath next_string_to_path_{nullptr};
    PFN_xrCreateActionSet next_create_action_set_{nullptr};
    PFN_xrDestroyActionSet next_destroy_action_set_{nullptr};
    PFN_xrCreateAction next_create_action_{nullptr};
    PFN_xrDestroyAction next_destroy_action_{nullptr};
    PFN_xrSuggestInteractionProfileBindings next_suggest_interaction_profile_bindings_{nullptr};
    PFN_xrGetActionStatePose next_get_action_state_pose_{nullptr};
    PFN_xrGetCurrentInteractionProfile next_get_current_interaction_profile_{nullptr};
    PFN_xrPathToString next_path_to_string_{nullptr};
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
