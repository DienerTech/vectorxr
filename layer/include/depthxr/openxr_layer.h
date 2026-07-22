#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
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
#include "depthxr/quadviews_recovery.h"
#include "depthxr/runtime_compatibility.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/settings_resolver.h"
#include "depthxr/swapchain_state.h"

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3DDeviceContextState;
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
    bool CanCreateInstance();

    struct InstanceCreateDiagnostics {
        bool app_requested_quad_views{false};
        bool app_requested_varjo_foveated_rendering{false};
        bool app_requested_eye_gaze{false};
        EyeGazeProbeState eye_gaze_probe_state{EyeGazeProbeState::kIndeterminate};
        bool eye_gaze_probe_known_unreliable{false};
        EyeGazeRequestReason eye_gaze_request_reason{EyeGazeRequestReason::kQuadviewsNotRequested};
        bool layer_injected_eye_gaze_request{false};
        XrResult first_create_result{XR_SUCCESS};
        bool retried_without_eye_gaze{false};
        XrResult retry_create_result{XR_SUCCESS};
        uint32_t first_downstream_extension_count{0};
        uint32_t final_downstream_extension_count{0};
        // True when the layer forwarded the native Varjo quad-views extensions to
        // the runtime (Varjo compatible quadviews) instead of stripping them for
        // stereo-composite emulation.
        bool varjo_compatible_quad_forwarded{false};
        // Diagnostics for native forwarding and for whether VectorXR's own
        // profile is eligible to modify that native path. Forwarding itself is
        // transparent whenever the active runtime is Varjo or advertises every
        // requested native extension.
        bool varjo_compatible_eligible{false};
        bool runtime_advertises_varjo_quad{false};
        bool runtime_advertises_varjo_foveated{false};
        bool pre_instance_extension_scan_ran{false};
        bool pre_instance_extension_scan_complete{false};
        std::string pre_instance_extension_scan_detail{"not-run"};
        XrResult pre_instance_extension_scan_result{XR_SUCCESS};
        uint32_t pre_instance_extension_count{0};
        std::string pre_instance_extensions;
        // Authoritative fallback Varjo signal: the active runtime manifest.
        // This covers Varjo's unreliable pre-instance extension enumeration.
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
    XrResult EndSession(XrSession session);
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
    XrResult GetVisibilityMaskKHR(XrSession session,
                                  XrViewConfigurationType view_configuration_type,
                                  uint32_t view_index,
                                  XrVisibilityMaskTypeKHR visibility_mask_type,
                                  XrVisibilityMaskKHR* visibility_mask);
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
        // Index at the moment the app last RELEASED an image — the content the
        // current frame's composite must sample. Under turbo pipelining the
        // app re-acquires the next image before EndFrame runs the compositor,
        // so last_acquired has already advanced past this frame's image
        // (observed as mismatched per-eye content on Pimax).
        uint32_t last_released_image_index{0};
        bool images_enumerated{false};
        bool has_last_acquired_image_index{false};
        bool has_last_released_image_index{false};
        uint32_t deferred_release_count{0};
        bool quadviews_session{false};
        SwapchainImageQueue image_states;
        std::unordered_set<uint32_t> d3d11_shader_resource_slices_attempted;
        std::unordered_set<uint32_t> d3d11_shader_resource_slices_available;
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

    static constexpr uint32_t kQuadViewsPixelProbeSize = 32;
    static constexpr uint32_t kQuadViewsPixelProbeBandsPerEye = 3;
    static constexpr uint32_t kQuadViewsPixelProbeCount = 2 * kQuadViewsPixelProbeBandsPerEye;

    // Debug-only, recovery-triggered pixel probes. Six tiny output regions are
    // copied to staging resources and read only after an event query completes,
    // so diagnostics never stall the frame waiting for the GPU.
    struct QuadViewsPixelProbe {
        std::array<ID3D11Texture2D*, kQuadViewsPixelProbeCount> staging_textures{};
        ID3D11Query* completion{nullptr};
        int64_t format{0};
        bool issued{false};
        XrTime frame_time{0};
        uint64_t target_generation{0};
        std::array<uint32_t, 2> output_image_indices{};
        std::array<uint64_t, kQuadViewsPixelProbeCount> previous_hashes{};
        bool has_previous_hashes{false};
    };

    struct D3D11QuadViewsCompositor {
        ID3D11Device* device{nullptr};
        ID3D11DeviceContext* context{nullptr};
        ID3D11DeviceContext1* context1{nullptr};
        ID3DDeviceContextState* layer_context_state{nullptr};
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
        uint64_t output_target_generation{0};
        double last_completed_gpu_ms{0.0};
        XrTime last_completed_gpu_frame_time{0};
        std::array<QuadViewsInputCopy, 4> input_copies;
        std::array<QuadViewsCompositionTarget, 2> targets;
        std::array<QuadViewsGpuTimingQuery, 4> gpu_timing_queries;
        QuadViewsPixelProbe pixel_probe;
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
        bool has_logged_skipped{false};
        uint32_t consecutive_skipped_frames{0};
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

    // Session-scoped evidence for the native Varjo foveation contract. The
    // first occurrence of each request state is logged at Info, while exact
    // focus-FOV movement ranges are summarized periodically at Debug and once
    // at teardown. This keeps the hot xrLocateViews path useful without
    // producing a line per frame.
    struct VarjoNativeFoveationDiagnosticState {
        uint64_t locate_calls{0};
        std::array<uint64_t, 3> request_state_counts{}; // absent, present/inactive, present/active
        uint64_t vector_injected_locate_requests{0};
        uint64_t rendering_gaze_queries{0};
        uint64_t rendering_gaze_tracked_queries{0};
        uint64_t rendering_gaze_untracked_queries{0};
        uint64_t rendering_gaze_failed_queries{0};
        uint8_t logged_request_state_mask{0};
        uint64_t successful_quad_locates{0};
        bool focus_ranges_initialized{false};
        bool focus_motion_logged{false};
        std::array<XrFovf, 2> initial_focus_fovs{};
        std::array<double, 2> min_focus_yaw{};
        std::array<double, 2> max_focus_yaw{};
        std::array<double, 2> min_focus_pitch{};
        std::array<double, 2> max_focus_pitch{};
        std::array<double, 2> min_focus_width{};
        std::array<double, 2> max_focus_width{};
        std::array<double, 2> min_focus_height{};
        std::array<double, 2> max_focus_height{};
        std::optional<std::chrono::steady_clock::time_point> last_summary_wall_time;
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
    void ResetDepthAnchorToggleState();
    void PollDepthAnchorToggle();
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
    void QueueRuntimePacingWrite(RuntimePacingObservation observation);
    void DrainRuntimePacingWrites();
    void NoteTurboPacingStableFrame(double app_frame_delta_ms);
    // Returns true when turbo should suspend for the session (level-2 trip or
    // non-auto mode); false when it adapted (async -> sequenced fallback).
    bool HandleTurboDrainTimeout(std::chrono::steady_clock::time_point now);
    // Releases config_lock (mutex_) before any blocking runtime call so
    // xrLocateViews/xrLocateSpace from other app threads are never serialized
    // behind the compositor pacing wait. Call as the tail of EndFrame only.
    XrResult ForwardEndFrame(XrSession session,
                             const XrFrameEndInfo* frame_end_info,
                             std::unique_lock<std::mutex>& config_lock);
    void ObserveCompositionLayerTopology(const XrFrameEndInfo* frame_end_info);
    void EnsureTurboAsyncWorkerLocked();
    void StopTurboAsyncWorker();
    void TurboAsyncWorkerLoop();
    void DrainTurboAsyncWait();
    void ResetTurboFrameState();
    // Frame pacing telemetry: logs a summary line (debug level) every ~5s so a
    // judder report can be localized to our drain, the runtime's end-frame, the
    // runtime's wait pacing, or upstream of the layer entirely.
    void RecordFramePacing(std::chrono::steady_clock::time_point frame_start,
                           std::chrono::steady_clock::time_point after_drain,
                           std::chrono::steady_clock::time_point after_end,
                           bool turbo_engaged);
    // Turbo metrics capture: per-pacing-state (off/async/sequenced) frame
    // stats, flushed to the turbo-metrics sidecar so the app can show the
    // measured effect of each strategy. Runs at the tail of ForwardEndFrame
    // on the frame thread; the periodic flush hands the snapshot to a
    // detached-async write so the frame thread never touches the filesystem.
    // Config values are passed in (copied under the config lock by
    // ForwardEndFrame) because the recorder runs after that lock is released.
    void RecordTurboMetricsFrame(bool turbo_engaged,
                                 double frame_blocked_ms,
                                 bool timed_out,
                                 TurboMetricsMode metrics_mode,
                                 const InputBinding& metrics_binding,
                                 bool metrics_available,
                                 int sound_volume);
    bool IsTurboMetricsCaptureArmed(const InputBinding& binding, int sound_volume);
    void FlushTurboMetrics(bool final_flush);
    void ResetTurboMetricsState();
    // Logs shouldRender changes (turbo_mutex_ held by caller). A silent
    // shouldRender=false is one of the ways an app goes black while its
    // frame loop keeps running — worth an info line the first few times.
    void NoteTurboShouldRenderLocked(bool should_render);
    bool IsQuadViewsActive() const;
    bool IsQuadViewsEmulationActive() const;
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
    void RecycleD3D11QuadViewsCompositionTargets();
    XrResult CreateInternalReferenceSpaces(XrSession session);
    void DestroyInternalReferenceSpaces();
    XrResult CreateVarjoNativeFoveationResources(XrSession session);
    void DestroyVarjoNativeFoveationResources();
    bool LocateVarjoRenderingGaze(XrTime display_time,
                                  XrResult* locate_result,
                                  XrSpaceLocationFlags* location_flags);
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
    bool CacheEyeOffsetsFromLocatedViews(XrViewConfigurationType view_configuration_type,
                                         XrTime display_time,
                                         const XrPosef& runtime_view_pose,
                                         std::span<const XrView> located_views,
                                         uint32_t view_count);
    struct DepthSubmissionGeometry {
        XrSpace space{XR_NULL_HANDLE};
        XrViewConfigurationType view_configuration_type{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
        std::vector<XrPosef> native_poses;
        std::vector<XrPosef> render_poses;
        std::vector<XrFovf> native_fovs;
        std::vector<XrFovf> render_fovs;
    };
    void CachePivotPoseDelta(XrTime time, const XrPosef& pose_delta);
    bool FindPivotPoseDelta(XrTime time, XrPosef* pose_delta, XrTime* matched_time) const;
    void PrunePivotPoseDeltas(XrTime time);
    void CacheDepthSubmissionGeometry(XrTime time,
                                      XrSpace space,
                                      XrViewConfigurationType view_configuration_type,
                                      std::span<const ViewAdjustmentData> native_views,
                                      std::span<const ViewAdjustmentData> render_views);
    bool FindDepthSubmissionGeometry(XrTime time,
                                     XrSpace space,
                                     XrViewConfigurationType view_configuration_type,
                                     uint32_t view_count,
                                     const DepthSubmissionGeometry** geometry,
                                     XrTime* matched_time) const;
    void PruneDepthSubmissionGeometry(XrTime time);
    uint32_t RestoreDepthSubmissionGeometry(std::span<XrCompositionLayerProjectionView> views,
                                            uint32_t first_view,
                                            const DepthSubmissionGeometry& geometry,
                                            const XrPosef& reverse_pose_delta,
                                            bool has_reverse_pose_delta) const;
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
    void RecordVarjoNativeLocateDiagnostics(const XrViewLocateInfo* view_locate_info,
                                            bool vector_request_injected,
                                            bool rendering_gaze_queried,
                                            XrResult rendering_gaze_result,
                                            XrSpaceLocationFlags rendering_gaze_flags,
                                            const XrViewState* view_state,
                                            XrResult result,
                                            uint32_t view_capacity_input,
                                            const uint32_t* view_count_output,
                                            const XrView* views);
    void LogVarjoNativeFoveationSummaryLocked(std::string_view reason, bool info_level);
    bool EnsureD3D11QuadViewsPixelProbeResources(int64_t format);
    void ReleaseD3D11QuadViewsPixelProbeResources(bool preserve_history);
    void PollD3D11QuadViewsPixelProbe();
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
    bool EnsureD3D11SwapchainShaderResources(SwapchainInfo& swapchain, uint32_t array_slice = 0);
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
    std::string system_name_;
    uint32_t system_vendor_id_{0};
    std::string graphics_api_;
    std::string current_exe_name_;
    ResolvedRuntimeConfig resolved_settings_;
    std::optional<ResolvedRuntimeConfig> last_logged_settings_;
    uint64_t locate_views_call_count_{0};
    uint32_t pending_locate_views_diagnostics_{0};
    uint32_t pending_end_frame_diagnostics_{0};
    bool depth_view_info_pending_{false};
    bool depth_submission_info_pending_{false};
    std::optional<XrTime> depth_submission_info_not_before_time_;
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
    bool depth_anchor_toggle_inverted_{false};
    bool depth_anchor_active_{false};
    bool depth_anchor_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> depth_anchor_binding_last_poll_time_;
    bool depth_anchor_binding_down_cached_{false};

    // Turbo mode. The runtime always sees a conformant wait->begin->end
    // sequence; the application is decoupled from runtime frame pacing: its
    // xrWaitFrame returns immediately with a fabricated predictedDisplayTime
    // (exactly one frame of pipelining), its xrBeginFrame becomes a no-op, and
    // the real wait+begin happen inside xrEndFrame. turbo_mutex_ guards this
    // state; it is never held across a blocking runtime wait except when
    // spec-ordering requires it (second poll, teardown drains). mutex_ must
    // NOT be required by WaitFrame/BeginFrame, which stay off the config lock.
    // False when Turbo has never been enabled for the current session. This
    // gives non-Turbo sessions a literal wait/begin/end pass-through instead
    // of making SteamVR observe VectorXR's pacing bookkeeping. Once armed it
    // stays armed until the frame state is reset because an established Turbo
    // pipeline is structural for the remainder of the session.
    std::atomic<bool> turbo_frame_interception_required_{false};
    std::mutex turbo_mutex_;
    std::condition_variable turbo_async_worker_cv_;
    std::thread turbo_async_worker_;
    bool turbo_async_worker_stop_{false};
    bool turbo_async_job_pending_{false};
    XrSession turbo_async_job_session_{XR_NULL_HANDLE};
    std::shared_ptr<std::promise<void>> turbo_async_job_completion_;
    // Shared snapshots let WaitFrame, EndFrame, and teardown observe the same
    // in-flight wait without concurrently mutating a std::future object.
    std::shared_future<void> turbo_async_wait_;
    std::uint64_t turbo_async_wait_generation_{0};
    bool turbo_async_wait_polled_{false};
    bool turbo_async_wait_completed_{false};
    XrResult turbo_async_wait_result_{XR_SUCCESS};
    XrTime turbo_last_predicted_display_time_{0};
    XrDuration turbo_last_predicted_display_period_{0};
    bool turbo_last_should_render_{true};
    XrEnvironmentBlendMode turbo_last_environment_blend_mode_{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrTime turbo_max_returned_display_time_{0};
    std::optional<std::chrono::steady_clock::time_point> turbo_last_wait_frame_wall_time_;
    // Toggle binding state (mutex_-guarded, polled from EndFrame like Depth's).
    bool turbo_toggle_enabled_{true};
    bool turbo_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> turbo_binding_last_poll_time_;
    bool turbo_binding_down_cached_{false};
    bool has_logged_turbo_varjo_note_{false};
    bool has_logged_turbo_session_compatibility_block_{false};
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
    // EndFrame, consumed after it is released); the sequenced state machine
    // below is turbo_mutex_-guarded because WaitFrame/BeginFrame consult it.
    TurboPacingMode turbo_pacing_mode_{TurboPacingMode::kAsync};
    TurboPacingSource turbo_pacing_source_{TurboPacingSource::kProbing};
    bool turbo_pacing_resolved_{false};
    // Auto only: a stable window is still owed before the verdict is written.
    bool turbo_pacing_verdict_pending_{false};
    std::int64_t turbo_probe_timeout_total_{0};
    std::future<void> runtime_pacing_write_future_;
    std::optional<RuntimePacingObservation> pending_runtime_pacing_write_;
    // Accumulated healthy engaged frame time; the verdict lands at 60s. Not
    // wall-clock, so loading screens neither earn nor destroy stability.
    double turbo_stable_accumulated_ms_{0.0};
    // Cadence gate: a stalled runtime wait is only evidence against a pacing
    // mode when the app itself is pacing normally. Turbo stays passive until
    // the app delivers a streak of healthy frames (app-time only — our own
    // drain/join blocking is subtracted via turbo_last_frame_blocked_ms_),
    // and pauses across loading hitches. Discovery counts nothing while
    // paused, so loading screens cannot poison verdicts.
    uint32_t turbo_cadence_healthy_streak_{0};
    bool turbo_cadence_ready_{false};
    // Set when xrBeginSession succeeds. The first turbo engage additionally
    // requires the session to be a few seconds old: MSFS2024 pumps full-rate
    // frames immediately during its VR-mode transition, so the healthy-streak
    // gate alone passed within a second and turbo engaged mid-transition.
    std::optional<std::chrono::steady_clock::time_point> session_begin_wall_time_;
    bool turbo_cadence_pause_logged_{false};
    double turbo_last_frame_blocked_ms_{0.0};
    // Sequenced pacing state machine (turbo_mutex_). DCS overlaps xrWaitFrame
    // (sim thread) with xrEndFrame (render thread) — spec-legal — so the
    // fabrication shield must never have a per-frame gap, and establishment
    // must hand off through the app's own WaitFrame call so we never issue a
    // runtime wait that duplicates one the app already has in flight:
    //   kInactive -> kEngaging (EndFrame decides to establish; no runtime calls)
    //   kEngaging -> kActive  (the app's next WaitFrame runs REAL on its own
    //                          thread — the handshake — then Begin passes real
    //                          via turbo_begin_owed_)
    //   kActive               (steady state: app wait/begin fabricated with no
    //                          gaps; EndFrame does End -> Wait -> Begin
    //                          synchronously on the frame thread)
    // The pipeline is STRUCTURAL: once kActive it persists until session end.
    // PiOpenXR wedges when the wait-issuing thread migrates a second time
    // (engage moved waits sim->render; unwind moved them back; re-engage
    // hardlocked DCS inside the runtime), so the turbo toggle must never
    // change thread topology — it only operates the pacing valve below.
    enum class TurboSequencedState { kInactive, kEngaging, kActive };
    TurboSequencedState turbo_seq_state_{TurboSequencedState::kInactive};
    // The app's next xrBeginFrame must pass through to the runtime (its wait
    // ran real during the establishment handshake).
    bool turbo_begin_owed_{false};
    // (turbo_mutex_) True while ForwardEndFrame is between its state snapshot
    // and the runtime xrEndFrame returning. An owed establishment begin that
    // arrives in that window is swallowed and flagged (turbo_begin_deferred_)
    // so it can be issued on the frame thread right after the submit —
    // otherwise Begin(N+1) reaches the runtime before End(N), which is how
    // MSFS2024 orders its frame calls and how it wedged PiOpenXR on the first
    // pipelined frame (2026-07-07).
    bool turbo_end_frame_in_flight_{false};
    bool turbo_begin_deferred_{false};
    // The frame the app will submit next has an open (begun) runtime frame.
    // Set by our pre-begin, a compensation begin, or the app's own begin
    // passing through; consumed at EndFrame. A fabricated wait whose frame
    // was never begun (engage/disengage race) is compensated at EndFrame.
    bool turbo_frame_begun_{false};
    // Serializes every real xrWaitFrame we issue or forward — the spec makes
    // concurrent waits app-UB, and a handshake/compensation wait must never
    // overlap a steady-state one.
    std::mutex turbo_runtime_wait_mutex_;
    // Hang forensics: a budget of debug markers around every blocking call in
    // the pipelined path (End, steady wait, swapchain acquire/wait, owed
    // begin) so a hardlock's log ends at the exact culprit. Refilled at each
    // sequenced engage.
    int turbo_seq_debug_log_budget_{0};
    bool TurboSequencedDebugTick();
    // Internal helper locates (pivot drive, origin capture, eye offsets, eye
    // gaze) run under mutex_ at the app's displayTime. Under sequenced turbo
    // that time can sit one period past the runtime's last real xrWaitFrame,
    // and a runtime may block such a locate until the next wait — which runs
    // inside EndFrame behind the same mutex_ (deadlock, observed on
    // PiOpenXR via xrLocateSpace before that path was unlocked). Clamp these
    // locates to the known horizon; the sources are smoothed/near-static so
    // <=1 period of staleness is invisible. Cache keys keep the app's time.
    XrTime ClampInternalLocateTime(XrTime app_time);
    XrResult ApplyPivotToLocatedSpace(XrSpace space,
                                      XrSpace base_space,
                                      XrTime time,
                                      bool pivotxr_active,
                                      XrSpaceLocation* location,
                                      double* applied_extra_yaw_radians,
                                      double* applied_extra_pitch_radians,
                                      XrPosef* applied_pose_delta,
                                      bool update_smoothing);
    // The pacing valve (turbo_mutex_): with the pipeline structural, the
    // turbo toggle only flips this. Open: app waits fabricate instantly
    // (decoupled). Closed: app waits block consuming a pacing token — one is
    // posted per completed pre-wait — re-coupling the app to genuine runtime
    // pacing with zero thread-topology change. A bounded cv timeout lets a
    // blocked wait fall back to fabrication so an app that stops submitting
    // can never deadlock against the valve.
    bool turbo_valve_open_{false};
    int turbo_pacing_tokens_{0};
    std::condition_variable turbo_valve_cv_;
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
    double pacing_submit_delta_sum_periods_{0.0};
    double pacing_submit_delta_min_periods_{0.0};
    double pacing_submit_delta_max_periods_{0.0};
    uint32_t pacing_submit_delta_samples_{0};
    // Cached from resolved settings so pass-through Wait/EndFrame can opt into
    // timing without taking Logger's mutex on every frame.
    std::atomic<bool> frame_pacing_debug_enabled_{false};

    // Turbo metrics capture: frame stats segmented by pacing state so the
    // app can compare turbo-off/async/sequenced within a session. Frame-thread
    // only except the *_pending_ counters (turbo_mutex_), which WaitFrame-side
    // paths feed and RecordTurboMetricsFrame drains once per EndFrame.
    // Histogram bins are 0.5 ms wide (256 ms cap) — enough resolution for a
    // p99 frame time without meaningful memory cost.
    static constexpr std::size_t kTurboMetricsHistogramBins = 512;
    static constexpr double kTurboMetricsHistogramBinMs = 0.5;
    struct TurboMetricsAccum {
        std::int64_t frames{0};
        double delta_sum_ms{0.0};
        double delta_max_ms{0.0};
        double wait_block_sum_ms{0.0};
        std::int64_t fabricated_waits{0};
        std::int64_t drain_timeouts{0};
        std::int64_t discarded_frames{0};
        std::array<std::uint32_t, kTurboMetricsHistogramBins> histogram{};
    };
    // Indexed by pacing state: 0 = off, 1 = async, 2 = sequenced.
    std::array<TurboMetricsAccum, 3> turbo_metrics_accum_{};
    bool turbo_metrics_capture_armed_{false};
    bool turbo_metrics_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> turbo_metrics_binding_last_poll_time_;
    bool turbo_metrics_binding_down_cached_{false};
    // Cleared whenever capture pauses so the first frame interval after a
    // resume (which spans the pause) never lands in the stats.
    bool turbo_metrics_was_capturing_{false};
    std::optional<std::chrono::steady_clock::time_point> turbo_metrics_last_end_time_;
    std::string turbo_metrics_session_id_;
    std::int64_t turbo_metrics_started_unix_seconds_{0};
    // Mode observed while recording; kept as a member so the teardown flush
    // never has to touch resolved_settings_ outside the config lock.
    TurboMetricsMode turbo_metrics_collection_mode_{TurboMetricsMode::kAlways};
    std::optional<std::chrono::steady_clock::time_point> turbo_metrics_last_flush_time_;
    bool turbo_metrics_dirty_{false};
    std::future<void> turbo_metrics_write_future_;
    // Guarded by turbo_mutex_: app-visible runtime-pacing block time observed
    // on the WaitFrame side (pass-through waits, valve re-coupling) and
    // fabricated-wait count since the last EndFrame.
    double turbo_metrics_wait_pending_ms_{0.0};
    std::int64_t turbo_metrics_fabricated_pending_{0};

    // Session-scoped black-screen forensics (added after the DCS-on-SteamVR
    // one-frame-then-black session, where none of the three signals below
    // were logged and the cause could not be discriminated). Budgeted so a
    // flapping state cannot flood the log.
    int end_frame_error_log_budget_{5};
    int should_render_log_budget_{8};      // turbo_mutex_
    std::optional<bool> last_noted_should_render_; // turbo_mutex_
    int submission_transition_log_budget_{8};
    std::optional<bool> app_submitting_layers_;
    int composition_topology_log_budget_{12};
    std::optional<std::uint64_t> last_composition_topology_signature_;
    bool quad_views_extension_requested_{false};
    bool varjo_foveated_rendering_extension_requested_{false};
    bool d3d11_graphics_extension_requested_{false};
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
    XrSpace varjo_native_view_space_{XR_NULL_HANDLE};
    XrSpace varjo_native_combined_eye_space_{XR_NULL_HANDLE};
    bool varjo_native_rendering_gaze_tracked_{false};
    bool has_logged_varjo_native_rendering_gaze_active_{false};
    bool has_logged_varjo_native_rendering_gaze_unavailable_{false};
    bool varjo_native_foveation_resources_attempted_{false};
    uint32_t varjo_native_rendering_gaze_transition_logs_remaining_{8};
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
    std::optional<std::chrono::steady_clock::time_point> quadviews_last_valid_gaze_wall_time_;
    std::optional<std::chrono::steady_clock::time_point> quadviews_eye_gaze_loss_started_wall_time_;
    bool quadviews_eye_gaze_loss_was_locate_failure_{false};
    bool quadviews_has_seen_valid_gaze_{false};
    QuadViewsRecoveryStabilizer quadviews_compositor_recovery_;
    XrViewConfigurationType active_primary_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrViewConfigurationType active_runtime_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    uint32_t pending_quadviews_pixel_diagnostics_{0};
    bool has_active_primary_view_configuration_{false};
    bool has_logged_quad_view_short_count_{false};
    bool has_logged_pivotxr_spike_mode_{false};
    bool has_logged_quadviews_view_configuration_capabilities_{false};
    uint64_t varjo_native_view_configuration_calls_{0};
    bool has_logged_varjo_native_view_configuration_count_{false};
    bool has_logged_varjo_native_view_configuration_signature_limit_{false};
    std::unordered_set<std::string> logged_varjo_native_view_configuration_signatures_;
    VarjoNativeFoveationDiagnosticState varjo_native_foveation_diagnostic_;
    bool has_logged_visibility_mask_mapping_{false};
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
    std::optional<std::chrono::steady_clock::time_point> last_eye_gaze_self_sync_time_;
    std::vector<ViewAdjustmentData> locate_views_original_scratch_;
    std::vector<ViewAdjustmentData> locate_views_adjusted_scratch_;
    std::vector<ViewAdjustmentData> locate_views_depth_native_scratch_;
    std::vector<std::vector<XrCompositionLayerProjectionView>> end_frame_projection_views_scratch_;
    std::vector<XrCompositionLayerProjection> end_frame_projection_layers_scratch_;
    std::vector<const XrCompositionLayerBaseHeader*> end_frame_layers_scratch_;
    std::map<XrTime, XrPosef> cached_pivot_pose_deltas_;
    std::map<XrTime, std::vector<DepthSubmissionGeometry>> cached_depth_submission_geometry_;
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
    PFN_xrEndSession next_end_session_{nullptr};
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
    PFN_xrGetVisibilityMaskKHR next_get_visibility_mask_khr_{nullptr};
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
