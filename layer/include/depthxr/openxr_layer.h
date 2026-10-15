#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
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
#include "depthxr/delayed_action_set_attachment.h"
#include "depthxr/pivot_step.h"
#include "depthxr/pivot_view.h"
#include "depthxr/effects.h"
#include "depthxr/logger.h"
#include "depthxr/quadviews_frame_cache.h"
#include "depthxr/quadviews_recovery.h"
#include "depthxr/runtime_compatibility.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/runtime_relay.h"
#include "depthxr/osd_renderer.h"
#include "depthxr/process_exit.h"
#include "depthxr/framegen_d3d11_bridge.h"
#include "depthxr/framegen_vulkan_bridge.h"
#include "depthxr/framegen_presenter.h"
#include "depthxr/framegen_runtime_queue.h"
#include "depthxr/turbo_recovery.h"
#include "depthxr/turbo_trace.h"
#include "depthxr/settings_resolver.h"
#include "depthxr/swapchain_state.h"
#include "depthxr/thread_census.h"

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

#if defined(DEPTHXR_TESTING)
class TurboFrameTestPeer;
#endif

class OpenXrLayer {
  public:
    static OpenXrLayer& Instance();

    void SetLayerDirectory(std::filesystem::path dll_directory);
    void SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr);
    bool CanCreateInstance();

    struct InstanceCreateDiagnostics {
        bool turbo_clock_enabled{false};
        bool app_requested_quad_views{false};
        bool app_requested_varjo_foveated_rendering{false};
        bool app_requested_eye_gaze{false};
        EyeGazeProbeState eye_gaze_probe_state{EyeGazeProbeState::kIndeterminate};
        bool eye_gaze_probe_structurally_unreliable{false};
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
        uint32_t pre_instance_missing_forwarded_extension_count{0};
        std::string pre_instance_missing_forwarded_extensions;
        // Authoritative fallback Varjo signal: the active runtime manifest.
        // This covers Varjo's unreliable pre-instance extension enumeration.
        bool active_runtime_is_varjo{false};
        std::string active_runtime_path;
        // XR_KHR_D3D12_enable, for the Direct3D 11 bridge (Frame Generation in
        // Direct3D 11 games): whether the layer added it to a Direct3D 11
        // application's extensions, whether the runtime refused the instance
        // with it, and whether the instance has it.
        bool app_requested_d3d11{false};
        bool d3d12_wanted{false};       // Frame Generation is enabled for the executable
        bool d3d12_scan_trusted{false}; // the runtime's pre-instance extension list was believable
        bool layer_injected_d3d12{false};
        bool retried_without_d3d12{false};
        XrResult d3d12_retry_result{XR_SUCCESS};
        bool d3d12_refused_alone{false}; // the instance was created with every other addition
        bool d3d12_enabled{false};
        // The application's Vulkan extension: 2 for XR_KHR_vulkan_enable2, 1
        // for XR_KHR_vulkan_enable, 0 for neither (the Vulkan bridge's
        // XR_KHR_D3D12_enable is then a Vulkan application's).
        int vulkan_enable_version{0};
        // XR_KHR_vulkan_enable was requested (also alongside _enable2).
        bool vulkan_enable1_requested{false};
        // The names of the layers below VectorXR, nearest first.
        std::string layers_below;
    };

    // True when VectorXR quadviews should run in Varjo compatible mode, i.e. core
    // and the quadviews module are enabled. Safe to call at instance-creation time
    // (loads config lazily). Does not consider runtime capability — the caller
    // pairs this with a runtime extension probe.
    bool IsVarjoCompatibleQuadviewsEligible();
    bool WantsTurboClockConversion();
    // Whether Frame Generation is enabled for this executable, so a Direct3D 11
    // application's instance needs XR_KHR_D3D12_enable for the bridge. Safe
    // before the instance exists (reads the config file).
    bool WantsD3D11Bridge();
    // Likewise for a Vulkan application (XR_KHR_vulkan_enable or _enable2)
    // and the Vulkan bridge: Frame Generation enabled for the executable,
    // VECTORXR_VULKAN_BRIDGE not "off".
    bool WantsVulkanBridge();


    XrResult OnInstanceCreated(const XrInstanceCreateInfo* create_info,
                               XrInstance instance,
                               bool eye_gaze_extension_enabled,
                               const InstanceCreateDiagnostics& diagnostics);
    // OnInstanceCreated failed (or threw): the layer forgets the instance, as
    // at xrDestroyInstance, so the application can create another.
    void AbandonInstance();
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
    XrResult CreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action);
    XrResult DestroyAction(XrAction action);
    XrResult DestroyActionSet(XrActionSet set);
    XrResult SuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* bindings);
    XrResult CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space);
    XrResult LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location);
    XrResult PollEvent(XrInstance instance, XrEventDataBuffer* event_data);
    XrResult LocateViews(XrSession session,
                         const XrViewLocateInfo* view_locate_info,
                         XrViewState* view_state,
                         uint32_t view_capacity_input,
                         uint32_t* view_count_output,
                         XrView* views);

  private:
#if defined(DEPTHXR_TESTING)
    friend class TurboFrameTestPeer;
    friend class PivotPoseTestPeer;
    friend class PivotSnapTestPeer;
    friend class PivotGazeTestPeer;
    friend class EyeGazeTestPeer;
    friend class D3D11BridgeLayerTestPeer;
    friend class VulkanBridgeLayerTestPeer;
#endif

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
        // An image acquired whose wait timed out: still the runtime's, waited
        // for again before anything is drawn into it.
        bool awaiting_wait{false};
        uint32_t awaiting_index{0};
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
        // The size and format each target could not be built for
        // (EnsureFocusSharpenTarget): not tried again until they change.
        struct TargetShape {
            uint32_t width{0};
            uint32_t height{0};
            int64_t format{0};
        };
        std::array<std::optional<TargetShape>, 2> failed_shapes;
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
        // The focus FOVs the runtime returned for the last few display times,
        // and how the application's submitted focus views compare for the same
        // display time: checked, other FOVs submitted (mbucchia's Varjo layers
        // patched DCS for submitting focus FOVs other than located), and no
        // locate kept for that display time.
        static constexpr size_t kLocatedFocusKept = 8;
        std::array<XrTime, kLocatedFocusKept> located_focus_times{};
        std::array<std::array<XrFovf, 2>, kLocatedFocusKept> located_focus_fovs{};
        size_t located_focus_next{0};
        uint64_t submitted_focus_checked{0};
        uint64_t submitted_focus_mismatched{0};
        uint64_t submitted_focus_unlocated{0};
        bool submitted_focus_mismatch_logged{false};
    };

    void ReloadConfigIfNeeded();
    void StartConfigWatcher();
    void StopConfigWatcher();
    void ConfigWatcherLoop();
    void PollConfigFile();
    // Log lines for the active config's warnings that this instance's log does
    // not show yet, which are then counted as shown. Caller holds mutex_.
    std::vector<std::string> UnloggedConfigWarningsLocked();
    // Starts this instance's log at `log_path` (at xrCreateInstance): a new
    // log, which shows none of the config's warnings yet. Caller holds mutex_.
    void BeginInstanceLogLocked(const std::filesystem::path& log_path);
    void RefreshResolvedSettings();
    void ResetPivotInputStateForConfigChange();
    void PollRuntimeRelay();
    // Watcher thread: has the flight recorder freeze a Frame Generation
    // snapshot when Dynamic stands down or runtime halving is confirmed.
    void PollFrameGenRecorder();
    // The app's status line for Frame Generation (RuntimeStatusDocument::framegen_reason).
    static std::string FrameGenStatusReason(const FrameGenStatus& framegen);
    void CaptureInstanceFunctions();
    void LogResolvedSettings(const ResolvedRuntimeConfig& settings);
    void ResetPivotActivationState();
    void ResetPivotPoseDeltaContinuityState();
    void ResetDepthToggleState();
    void ResetDepthAnchorToggleState();
    void PollDepthAnchorToggle();
    bool IsPivotXrActive();
    const PivotXrResolvedProfile& ActivePivotProfile() const;
    // Snap View and Enhanced Motion transitions. Input may arrive on any
    // locate call, so each keeps the composed transform unchanged until the
    // next xrLocateViews drive; see ApplyPivotToLocatedSpace.
    void EngagePivotMotionProfile(size_t index);
    void ActivatePivotQuickView(size_t profile_index, size_t view_index);
    void ReleasePivotQuickView(double duration_seconds);
    void ClearPivotSnapState();
    void ResetPivotMotionState();
    void FoldPivotMotionIntoSnap(double settle_seconds);
    bool PivotSnapEffectActive() const;
    bool PivotMotionVisible() const;
    bool PivotProfilesShareNudgeSet(size_t lhs, size_t rhs) const;
    double SelectedQuickViewTransitionSeconds() const;
    double PivotPositionAssistYawRadians() const;
    PivotViewOffset PivotMotionOffset() const;
    PivotViewOffset ComposedPivotMotionOffset() const;
    PivotViewOffset CapturePivotMotionForSnapEntry() const;
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
    void ArmTurboAsyncHandoffLocked(const char* reason);
    void PublishTurboAsyncHandoffLocked();
    void CancelTurboAsyncHandoffLocked(const char* reason);
    void EnsureTurboAsyncWorkerLocked();
    void StopTurboAsyncWorker();
    void TurboAsyncWorkerLoop(std::uint64_t epoch);
    // Waits for the pipelined runtime xrWaitFrame without consuming it. Returns
    // false if it is still outstanding after `timeout`; teardown never waits
    // unbounded on a runtime call that may only release on a later submit.
    bool DrainTurboAsyncWait(std::chrono::milliseconds timeout);
    // EndSession only: releases an interlocked off-thread wait with a balancing
    // begin/empty submit, then waits a bounded time for it to complete.
    void ReleaseTurboPendingWaitForSessionEnd(XrSession session);
    // stop_recorder=false leaves the flight recorder running (xrDestroySession
    // stops it once Frame Generation's presenter is down).
    void ResetTurboFrameState(bool stop_recorder = true);
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
                                 const std::vector<InputBinding>& metrics_bindings,
                                 bool metrics_available,
                                 int sound_volume);
    bool IsTurboMetricsCaptureArmed(const std::vector<InputBinding>& bindings, int sound_volume);
    void FlushTurboMetrics(bool final_flush, bool queue_after_pending = false);
    void ResetTurboMetricsState();
    // Logs shouldRender changes (turbo_mutex_ held by caller). A silent
    // shouldRender=false is one of the ways an app goes black while its
    // frame loop keeps running — worth an info line the first few times.
    void NoteTurboShouldRenderLocked(bool should_render);
    bool IsQuadViewsActive() const;
    bool IsQuadViewsEmulationActive() const;
    // Frame Generation captures the Direct3D 11 quad views compositor's
    // images (it generates frames from them where it cannot compose the quad
    // views itself): the diagnostic overlay stays out of them, or its motion
    // would be measured. Caller holds mutex_.
    bool QuadViewsOverlayWithheld() const;
    // The diagnostic visualization can be shown in this session (the app's
    // live control): an emulated quad views session with the Direct3D 11
    // compositor, the overlay not withheld. Caller holds mutex_.
    bool QuadViewsDiagnosticVisualizationAvailable() const;
    void ResetSwapchainState();
    void LogSwapchainSummary(XrSwapchain swapchain, const SwapchainInfo& info, std::string_view event_name);
    // Logs xrEnumerateSwapchainFormats' list when it differs from the last one this session.
    void LogSwapchainFormatsOnChange(const std::string& line);
    // Counts an application call on the calling thread; logs the first call of each kind on each thread.
    void NoteThread(ThreadCensus::Call call);
    bool ShouldDeferSwapchainRelease(const SwapchainInfo& info) const;
    // An application's release of `swapchain`'s image (forwarded as it came,
    // or deferred until its xrEndFrame), on to the runtime.
    XrResult ForwardAppRelease(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* release_info);
    XrResult FlushDeferredSwapchainReleaseLocked(XrSwapchain swapchain,
                                                 SwapchainInfo& info,
                                                 std::string_view reason);
    XrResult FlushDeferredSwapchainReleasesLocked(std::string_view reason);
    void PollQuadViewsDiagnosticVisualizationToggle();
    void ResetQuadViewsDiagnosticVisualizationState();
    bool PollInputBindingDown(const InputBinding& binding);
    // A toggle's list: the first of its bindings held down (its label and
    // sound say what acted), or null. Every binding is polled each time, so
    // each device's reconnection and diagnostics keep running.
    const InputBinding* PollInputBindingsDown(const std::vector<InputBinding>& bindings);
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
    bool FinalizeEyeGazeBindings(); // Called under mutex_, immediately before attachment.
    void RecordEyeGazeSyncResult(bool application, XrResult result);
    void ResetEyeGazeInputState();
    void DestroyEyeGazeResources();
    void UpdateEyeGazeResources(QuadViewsTrackingMode previous_mode); // Called under mutex_.
    void TryAttachEyeGazeActionSetFallback(XrSession session);
    bool LocateEyeGazeFocusOffsets(XrSession session,
                                   XrSpace base_space,
                                   XrTime time,
                                   const QuadViewsSettings& settings,
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
    struct QuadViewsGazeDiagnostic {
        bool valid{false};
        double raw_yaw_radians{0.0};
        double raw_pitch_radians{0.0};
        double smoothed_yaw_radians{0.0};
        double smoothed_pitch_radians{0.0};
    };
    struct QuadViewsFrameState {
        std::array<XrFovf, 4> fovs{};
        QuadViewsGazeDiagnostic gaze;
    };
    struct PivotSpacePoseDelta {
        XrSpace space{XR_NULL_HANDLE};
        XrPosef pose_delta{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    };
    struct PivotPoseDeltaFrame {
        // The VIEW-space representation is the frame's canonical logical
        // Pivot update. Per-space values are derived from it and never drive
        // smoothing a second time.
        XrPosef canonical_view_pose_delta{{0.0f, 0.0f, 0.0f, 1.0f},
                                          {0.0f, 0.0f, 0.0f}};
        XrSpace source_space{XR_NULL_HANDLE};
        // OpenXR applications normally submit one projection space. Keep a
        // small fixed cache for multi-query/multi-layer games so frame records
        // and one-frame continuity never allocate on the hot path.
        static constexpr std::size_t kMaxSpacePoseDeltas = 8;
        std::array<PivotSpacePoseDelta, kMaxSpacePoseDeltas> space_pose_deltas{};
        std::size_t space_pose_delta_count{0};
    };
    struct ResolvedPivotSpaceDelta {
        XrSpace space{XR_NULL_HANDLE};
        XrPosef forward{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        XrPosef reverse{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        XrResult locate_result{XR_SUCCESS};
        XrSpaceLocationFlags location_flags{0};
        double locate_duration_us{0.0};
        bool available{false};
        bool non_identity{false};
        std::string_view mode{"none"};
    };
    bool CachePivotPoseDelta(XrTime time,
                             XrSpace source_space,
                             const XrPosef& source_pose_delta,
                             const XrPosef& view_pose_in_source);
    bool FindPivotPoseDeltaForSpace(const PivotPoseDeltaFrame& frame,
                                    XrSpace space,
                                    XrPosef* pose_delta) const;
    void CachePivotPoseDeltaForSpace(PivotPoseDeltaFrame& frame,
                                     XrSpace space,
                                     const XrPosef& pose_delta) const;
    void PrunePivotPoseDeltas(XrTime time);
    std::string DescribeSpace(XrSpace space) const;
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
    void CacheQuadViewsFrame(XrTime time,
                             std::span<const XrView> views,
                             const QuadViewsGazeDiagnostic& gaze);
    bool FindQuadViewsFrame(XrTime time, QuadViewsFrameState* frame, XrTime* matched_time) const;
    void PruneQuadViewsFrames(XrTime time);
    bool IsTrackedViewSpace(XrSpace space) const;
    XrResult LocateRuntimeViews(XrSession session,
                                const XrViewLocateInfo* view_locate_info,
                                XrViewState* view_state,
                                uint32_t view_capacity_input,
                                uint32_t* view_count_output,
                                XrView* views,
                                bool* synthesized_quad_views,
                                QuadViewsGazeDiagnostic* gaze_diagnostic);
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
    // A native quad projection layer the application submitted for
    // `display_time`: its focus views' FOVs against the ones located for that
    // display time (VarjoNativeFoveationDiagnosticState). Caller holds mutex_.
    void NoteVarjoNativeLocatedFocusLocked(XrTime display_time, const XrView* views);
    void NoteVarjoNativeSubmittedFocusLocked(XrTime display_time, const XrCompositionLayerProjectionView* views,
                                            uint32_t view_count);
    bool EnsureD3D11QuadViewsPixelProbeResources(int64_t format);
    void ReleaseD3D11QuadViewsPixelProbeResources(bool preserve_history);
    void PollD3D11QuadViewsPixelProbe();
    bool SynthesizeQuadViewsFromStereo(std::span<const XrView> stereo_views,
                                       const QuadViewsSettings& quadviews_settings,
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
    // The first four swapchains of the quad views session that can be the
    // compositor's inputs; false while fewer exist.
    bool SelectQuadViewsPrewarmInputs(std::array<SwapchainInfo*, 4>* inputs);
    void TryPrewarmD3D11QuadViewsCompositor();
    // An emulated quad views frame (views 0/1 peripheral, 2/3 focus) as it is
    // composed into stereo, worked out on the CPU alone: what the Direct3D 11
    // compositor draws from, and what Frame Generation composes from.
    struct QuadViewsComposePlan {
        uint32_t width{0}; // each eye's composed image
        uint32_t height{0};
        double density{1.0};
        int64_t format{0}; // the focus swapchain's (the composed images')
        bool has_cached_fovs{false};
        XrTime matched_time{0};
        QuadViewsGazeDiagnostic gaze{};
        std::array<XrFovf, 4> fovs{}; // located at the display time, else as submitted
        // As submitted, posed times the Pivot reverse delta, fov = fovs[i].
        std::array<XrCompositionLayerProjectionView, 4> source_views{};
        // The composed eyes: source views 0/1 with their depth geometry
        // restored; subImage unset.
        std::array<XrCompositionLayerProjectionView, 2> eye_views{};
        uint32_t restored{0}; // eye views whose depth geometry was restored
        // Per eye: views eye and eye + 2, with the diagnostic overlay while
        // the visualization is shown.
        FrameGenComposition composition;
        bool log_diagnostic{false};      // the frame logs the compositor's Debug diagnostics
        // Frame Generation's thread shows the composed frame later, between
        // frames it writes itself (left to the layer while that thread
        // presents just in time): the outputs' releases are lazy, and the
        // runtime's queue (Frame Generation's) waits for the game's frame
        // only when the composed frame is submitted.
        bool shown_later{false};
    };
    // The plan for `source_layer` at `display_time`; false when its views
    // cannot be composed. Logs the focus rectangles on diagnostic frames.
    bool BuildQuadViewsComposePlan(const XrCompositionLayerProjection* source_layer,
                                   XrTime display_time,
                                   const XrPosef& reverse_delta,
                                   bool has_non_identity_delta,
                                   const DepthSubmissionGeometry* depth_geometry,
                                   QuadViewsComposePlan* plan);
    // Logs, at Info, how the focus inset moved since the last summary (in
    // `tracking_mode`), and starts the next one.
    void LogQuadViewsInsetMotion(std::string_view reason, QuadViewsTrackingMode tracking_mode);
    // The four views' swapchains, when each has a released (or acquired)
    // image of its own to compose from.
    bool FindQuadViewsSourceSwapchains(const XrCompositionLayerProjection* source_layer,
                                       std::array<SwapchainInfo*, 4>* swapchains);
    static uint32_t QuadViewsSourceImageIndex(const SwapchainInfo& swapchain);
    bool ComposeQuadViewsD3D11(const XrCompositionLayerProjection* source_layer,
                               XrTime display_time,
                               const QuadViewsComposePlan& plan,
                               uint32_t* restored_view_count,
                               XrCompositionLayerProjection* composed_layer,
                               std::vector<XrCompositionLayerProjectionView>* composed_views);

    // Varjo compatible quadviews focus sharpen (see D3D11FocusSharpen).
    void ResetD3D11FocusSharpen();
    bool EnsureD3D11FocusSharpen();
    bool EnsureFocusSharpenTarget(QuadViewsCompositionTarget& target,
                                  uint32_t width,
                                  uint32_t height,
                                  int64_t format);
    // Acquires and waits for the image of `target` to draw into; false (with
    // `reason`) when there is none this frame.
    bool AcquireFocusSharpenImage(QuadViewsCompositionTarget& target, uint32_t* index, const char** reason);
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
    TurboTimingTrace turbo_trace_; // destroyed before logger_
    // Which threads the application calls the frame loop and swapchain functions on, per session.
    ThreadCensus thread_census_;
    // Immutable between BeginSession and teardown; live edits apply next session.
    OsdRenderer osd_;
    std::atomic<bool> osd_vulkan_{false}, turbo_metrics_active_{false};
    std::mutex osd_vulkan_queue_mutex_;
    std::atomic<bool> osd_monitoring_{false};
    std::atomic<bool> osd_should_render_{true};
    std::optional<std::chrono::steady_clock::time_point> osd_last_input_poll_;
    bool osd_toggle_down_{false}, osd_cycle_down_{false}, osd_was_enabled_{false};
    // The binding of each list that last went down: its sound plays for the
    // change it made (none yet: the list's first binding's).
    std::optional<SoundFeedback> osd_toggle_sound_;
    std::optional<SoundFeedback> osd_cycle_sound_;
    std::uint64_t osd_settings_revision_{1}; // bumped whenever settings re-resolve
    OsdSnapshot BuildOsdSnapshot() const;
    void PrepareOsd();
    TurboExperimentalSettings turbo_experiment_;
    bool turbo_clock_enabled_{false};
    PFN_xrVoidFunction turbo_convert_counter_time_{nullptr};
    std::optional<std::chrono::steady_clock::time_point> turbo_limit_last_wait_;
    bool turbo_trace_last_engaged_{false};
    XrResult TraceRuntimeWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState*);
    XrResult TraceRuntimeBeginFrame(XrSession, const XrFrameBeginInfo*);
    XrResult TraceRuntimeEndFrame(XrSession, const XrFrameEndInfo*);
    // The layer's final submission: OSD overlay, then the runtime xrEndFrame.
    // Frame Generation submits each of its runtime frames through it.
    XrResult SubmitRuntimeFrame(XrSession, const XrFrameEndInfo*);
    // After xrDestroySession or xrDestroyInstance: the bridge's summary, then its teardown.
    void EndD3D11Bridge();
    void EndVulkanBridge();
    // The layer's own swapchains (Quadviews' outputs): through the queue
    // substitution and the Direct3D 11 bridge when the session is bridged,
    // else straight to the runtime. `shown_later`: Frame Generation's thread
    // shows the image later (QuadViewsComposePlan::shown_later), so its calls
    // may be held back. Releasing `index`, `drawn`: the image holds a new
    // frame (Frame Generation captures it; the bridge copies it into the
    // runtime's image), else its content is dropped. Frame Generation is told
    // of every release made (NoteFrameGenRelease).
    XrResult AcquireLayerSwapchainImage(XrSwapchain swapchain, uint32_t image_count, uint32_t* index);
    XrResult WaitLayerSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info, bool shown_later);
    XrResult ReleaseLayerSwapchainImage(XrSwapchain swapchain, uint32_t index, bool drawn, bool shown_later);
    XrResult DestroyLayerSwapchain(XrSwapchain swapchain);
    // Frame Generation is told of each release of a swapchain it tracks
    // (FrameGenPresenter::NoteRelease), drawn or not: the runtime shows the
    // swapchain's image released last wherever a frame shows the swapchain,
    // even a frame composed before it.
    void NoteFrameGenRelease(XrSwapchain swapchain, uint32_t index);
#if defined(DEPTHXR_TESTING)
    std::mutex framegen_noted_releases_mutex_;
    std::map<XrSwapchain, uint64_t> framegen_noted_releases_; // each swapchain's NoteFrameGenRelease calls
#endif
    void PollFrameGenToggle();
    // `unavailable`: Frame Generation cannot run this session (a Vulkan
    // session without the Vulkan bridge), for that reason.
    void StartFrameGen(XrSession session, const void* binding_chain, const std::string& unavailable = {});
    // Whether the OSD panel is placed in the world (VECTORXR_OSD_ANCHOR).
    void ConfigureOsdAnchor();
    // A session (or its instance) is going: Frame Generation's presenter
    // thread is stopped within its teardown wait. One a runtime holds inside
    // a call is let go by ending the runtime's session (a runtime may hold
    // its waits until then); one that still never leaves is left behind
    // (FrameGenPresenter::AbandonPresenter), and the session and instance
    // are then kept downstream for the process's exit. `step` names what
    // the teardown waits on (TeardownWatch::Step).
    void StopFrameGenPresenterForTeardown(XrSession session, const std::function<void(const char*)>& step);
    // A presenter thread was left behind: neither this module nor the one
    // below it (the runtime) may be unloaded under it.
    void PinModulesForAbandonedPresenter();
    // Frame Generation is latched per session. While it owns pacing, the app's
    // Wait/Begin/End go to the presenter and Turbo stays off for the session.
    FrameGenPresenter framegen_;
    // Direct3D 12: the runtime is given Frame Generation's queue (see
    // FrameGenQueueSubstitution); decided at xrCreateSession.
    FrameGenQueueSubstitution framegen_queue_;
    // Direct3D 11 games with Frame Generation: the session is Direct3D 12 and
    // the game renders into images shared with it (see D3D11Bridge); decided
    // at xrCreateSession.
    D3D11Bridge d3d11_bridge_;
    bool d3d11_bridge_lost_logged_{false};    // mutex_
    bool d3d11_bridge_depth_logged_{false};   // mutex_
    bool d3d11_bridge_sharpen_logged_{false}; // mutex_
    std::chrono::steady_clock::time_point d3d11_bridge_last_summary_; // mutex_
    // Quad views frames of a bridged session (mutex_): composed by Frame
    // Generation, by the Direct3D 11 compositor, or split into projection
    // layers when composing failed. The session's, and as of the last summary.
    struct D3D11BridgeQuadFrames {
        std::uint64_t fg{0};
        std::uint64_t d3d11{0};
        std::uint64_t split{0};
    };
    // The frame being ended came from the emulated quad views compose site
    // of a session whose pacing Frame Generation owns: how it was routed
    // (FrameGenPresenter::ClaimQuadFrame). Set in EndFrame under mutex_,
    // consumed by TraceRuntimeEndFrame; only the application's end-frame
    // thread touches it.
    std::optional<FrameGenQuadFrame> pending_framegen_quad_;
    bool framegen_quad_unforwarded_logged_{false};
    // The frame's quad route is dropped: an EndFrame that returns before
    // forwarding a claimed frame abandons it (`abandon`, when the caller
    // does not abandon the frame itself).
    void DropPendingFrameGenQuad(bool abandon);
    D3D11BridgeQuadFrames d3d11_bridge_quad_frames_;
    D3D11BridgeQuadFrames d3d11_bridge_quad_frames_summarized_;
    // Vulkan games with Frame Generation: the session is Direct3D 12 and the
    // game renders into images shared with it (see VulkanBridge); decided at
    // xrCreateSession. Frame Generation runs a Vulkan session only through it.
    VulkanBridge vulkan_bridge_;
    bool vulkan_bridge_lost_logged_{false};  // mutex_
    bool vulkan_bridge_depth_logged_{false}; // mutex_
    std::chrono::steady_clock::time_point vulkan_bridge_last_summary_; // mutex_
    // Either bridge's swapchain, and one whose runtime images never get
    // anything (its calls need no ordering with the game's queue).
    bool BridgedSwapchain(XrSwapchain swapchain) const {
        return d3d11_bridge_.Bridged(swapchain) || vulkan_bridge_.Bridged(swapchain);
    }
    bool UnmirroredSwapchain(XrSwapchain swapchain) const {
        return d3d11_bridge_.Unmirrored(swapchain) || vulkan_bridge_.Unmirrored(swapchain);
    }
    FrameGenQueueSubstitution::Calls FrameGenQueueCalls() const {
        return {next_acquire_swapchain_image_, next_wait_swapchain_image_, next_release_swapchain_image_};
    }
    // Whether the application's calls on `swapchain` may be held back
    // (FrameGenQueueSubstitution: Frame Generation captures its images and
    // shows images of its own only).
    // A bridged swapchain the layer composes (Quadviews' views) reaches the
    // runtime only when composing fails, likewise.
    bool FrameGenLazy(XrSwapchain swapchain) {
        return framegen_queue_.Active() &&
               ((framegen_.ShowsOwnImagesOnly() && framegen_.CapturesFrom(swapchain)) ||
                d3d11_bridge_.Composed(swapchain));
    }
    // A layer's swapchain (not a projection's) a frame shows while Frame
    // Generation shows images of its own only and does not capture it: kept
    // lazy unless VECTORXR_FRAMEGEN_SHOWN_LAYERS=off (FrameGenQueueSubstitution::Flush).
    bool FrameGenShownLazily(XrSwapchain swapchain) {
        return framegen_queue_.ShownLayersLazy() && framegen_.ShowsOwnImagesOnly() &&
               !framegen_.CapturesFrom(swapchain);
    }
    FrameGenPresenter::GpuFactory framegen_gpu_factory_{CreateFrameGenGpu};
    FrameGenRuntimeCalls framegen_calls_;
    std::atomic<bool> framegen_owns_pacing_{false};
    bool framegen_session_enabled_{false};  // mutex_
    // A session of this instance started with Frame Generation on: its
    // teardown calls are watched (TeardownWatch). Sessions without it log
    // their teardown as 0.19 did.
    std::atomic<bool> framegen_teardowns_watched_{false};
    bool turbo_yielded_to_framegen_{false}; // mutex_
    bool framegen_toggle_binding_was_down_{false};
    bool framegen_binding_down_cached_{false};
    std::optional<InputBinding> framegen_toggle_pressed_; // its list's binding that last went down (ActingBinding)
    std::optional<std::chrono::steady_clock::time_point> framegen_binding_last_poll_time_;
    XrTime TurboClockNow();
    void ConfigureTurboExperiment();
    std::string TurboTimingConfiguration() const;

    struct InputBindingDiagnosticLogState {
        bool failure_active{false};
        std::string signature;
        std::uint64_t failed_attempts{0};
        std::uint64_t suppressed_attempts{0};
        std::optional<std::chrono::steady_clock::time_point> last_log_time;
    };
    // Device bindings can be polled several times per frame across Pivot,
    // Depth, and Turbo. Keep one bounded diagnostic stream per DirectInput
    // device instead of flooding the session log with the same HRESULT.
    std::mutex input_binding_diagnostic_mutex_;
    std::unordered_map<std::string, InputBindingDiagnosticLogState> input_binding_diagnostic_states_;
    ConfigDocument config_;
    bool has_loaded_config_{false};
    uint64_t config_generation_{0};
    uint64_t resolved_settings_generation_{~0ull};
    XrSession resolved_settings_session_{XR_NULL_HANDLE};
    std::string last_failed_config_error_;
    // The warnings of the config in config_: unknown keys its parse ignored.
    std::vector<std::string> config_warnings_;
    // The config warnings this instance's log already shows. Each is logged
    // once per log, however often the file reloads.
    std::unordered_set<std::string> logged_config_warnings_;

    // Config hot-reload runs on a dedicated watcher thread so that filesystem
    // latency never blocks the render hot path. The watcher does all I/O without
    // holding mutex_, taking it only briefly to publish a parsed change.
    std::thread config_watcher_thread_;
    std::mutex config_watcher_mutex_;
    std::condition_variable config_watcher_cv_;
    bool config_watcher_stop_{false};

    // Versioned, session-targeted app<->layer relay. The watcher owns all
    // filesystem I/O; frame paths only update in-memory state and a dirty bit.
    std::vector<QuadViewDimensions> quadviews_dimensions_; // mutex_
    std::uint64_t quadviews_dimensions_at_{0};
    std::filesystem::path runtime_relay_root_;
    std::string runtime_relay_session_id_;
    std::vector<std::string> runtime_relay_sessions_to_remove_;
    std::uint64_t runtime_relay_session_sequence_{0};
    std::uint64_t runtime_relay_last_applied_revision_{0};
    std::uint64_t runtime_relay_acknowledged_revision_{0};
    std::atomic<bool> runtime_relay_status_dirty_{false};
    std::optional<std::chrono::steady_clock::time_point> runtime_relay_last_status_write_;
    // The session's status and control files, removed by a normal process
    // exit (games that skip OpenXR teardown). Guarded by mutex_.
    ProcessExitFile runtime_relay_exit_status_;
    ProcessExitFile runtime_relay_exit_control_;
    bool runtime_relay_pruned_{false}; // Watcher thread only.
    // The Frame Generation events PollFrameGenRecorder last saw (watcher
    // thread only): Dynamic stand-downs and probes, and runtime halving.
    FrameGenSnapshotTrigger framegen_recorder_;

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
    PivotStepGlideState pivotxr_yaw_step_glide_;
    PivotStepGlideState pivotxr_pitch_step_glide_;
    // Activation envelope in [0,1]: eases the pivot effect in/out on the
    // enable/disable transition so toggling never snaps the view, independent
    // of the per-frame tracking smoothing.
    double pivotxr_activation_gain_{0.0};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_last_smoothing_wall_time_;
    // EndFrame can occasionally arrive without a matching LocateViews cache
    // entry. Bridge one such miss so amplified poses do not flash to identity.
    std::optional<PivotPoseDeltaFrame> pivotxr_last_matched_pose_delta_;
    std::size_t pivotxr_consecutive_pose_delta_misses_{0};
    // A continuity miss was logged and its recovery is not yet.
    bool pivotxr_pose_delta_miss_logged_{false};
    // Per resolved-profile input edge state, index-aligned with
    // resolved_settings_.pivotxr.profiles. Arbitration is last-pressed-wins:
    // pressing another candidate's binding retargets the engaged profile and
    // the activation envelope carries the view across the switch.
    struct PivotProfileInputState {
        struct BindingState {
            bool was_down{false};
            bool down_cached{false};
        };
        std::vector<BindingState> activation;
        std::vector<BindingState> set_origin;
        std::vector<BindingState> release_origin;
        std::vector<BindingState> nudge_yaw_left;
        std::vector<BindingState> nudge_yaw_right;
        std::vector<BindingState> nudge_pitch_up;
        std::vector<BindingState> nudge_pitch_down;
        std::vector<BindingState> nudge_center;
        struct QuickViewState {
            std::vector<BindingState> activation;
            bool toggle_latched{false};
        };
        std::vector<QuickViewState> quick_views;
        // Toggle controls share one latch per profile. Hold state remains
        // per binding and is derived from the cached input state.
        bool toggle_latched{false};
        bool toggle_suspended{false};
    };
    // The swapchain format list last logged this session.
    std::string last_logged_swapchain_formats_;
    // The application called xrCreateSession on this instance, and the
    // number of extensions it enabled for it.
    bool instance_requested_session_{false};
    uint32_t instance_enabled_extension_count_{0};
    std::vector<PivotProfileInputState> pivotxr_profile_input_states_;
    // IsPivotXrActive's per-call claim lists (under mutex_), kept so the
    // locate paths do not allocate.
    std::vector<const InputBinding*> pivotxr_claimed_view_inputs_scratch_;
    std::vector<std::string_view> pivotxr_polled_nudge_sets_scratch_;
    // Engaged profile index; retains the last-engaged profile during the
    // release ramp so the easing keeps using that profile's settings. While a
    // stacked Snap View is selected it names the motion profile; the snap's
    // owner is pivotxr_quick_view_profile_index_.
    size_t pivotxr_active_profile_index_{0};
    bool pivotxr_engaged_{false};
    // A stacked Snap View keeps applying Enhanced Motion on top of the snap.
    // Stacked motion is measured from a head reference captured on the first
    // drive after stacking starts (or after a recenter re-measures it).
    bool pivotxr_snap_stacked_{false};
    bool pivotxr_snap_reference_pending_{false};
    bool pivotxr_snap_reference_valid_{false};
    double pivotxr_snap_reference_yaw_radians_{0.0};
    double pivotxr_snap_reference_pitch_radians_{0.0};
    // A released stacked snap hands its motion back to the seated origin on
    // the next drive, folding the difference into the return transition.
    bool pivotxr_snap_return_pending_{false};
    // Position Assist in meters (right/up/forward; angles unused), expressed
    // in the gravity-aligned heading of PivotPositionAssistYawRadians().
    PivotViewOffset pivotxr_position_offset_;
    // Independent Nudge Sets contribute to the global transition; active-only
    // Nudge Sets use the profile transition and are cleared on profile release.
    PivotViewTransitionState pivotxr_manual_view_transition_;
    PivotViewTransitionState pivotxr_profile_view_transition_;
    PivotViewTransitionState pivotxr_quick_view_transition_;
    bool pivotxr_quick_view_retarget_pending_{false};
    PivotViewOffset pivotxr_quick_view_pending_pose_;
    double pivotxr_quick_view_pending_duration_seconds_{0.0};
    bool pivotxr_quick_view_active_{false};
    bool pivotxr_quick_view_transitioning_{false};
    size_t pivotxr_quick_view_profile_index_{0};
    size_t pivotxr_quick_view_index_{0};
    size_t pivotxr_quick_view_return_profile_index_{0};
    bool pivotxr_quick_view_return_engaged_{false};
    // Optional full seated origin in the app's reference space. Its position
    // anchors translation for every Pivot mode. Capture happens on the
    // xrLocateViews drive path and shares the pivot drive's displayTime.
    struct PivotOrigin {
        XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        double yaw_radians{0.0};
        double pitch_radians{0.0};
        XrTime capture_time{0};
        XrSession session{XR_NULL_HANDLE};
        XrSpaceLocationFlags location_flags{0};
    };
    std::optional<PivotOrigin> pivotxr_origin_;
    bool pivotxr_origin_capture_pending_{false};
    struct PivotTranslationAnchor {
        XrVector3f position;
        XrSpace space{XR_NULL_HANDLE};
    };
    // Without Set Origin, capture once per engagement, never once per frame:
    // repeatedly anchoring at the moving head would cancel the rotated lean.
    std::optional<PivotTranslationAnchor> pivotxr_translation_anchor_;
    // A runtime recenter (REFERENCE_SPACE_CHANGE_PENDING) moves the anchor's
    // reference origin under the stored coordinates. The drive path re-anchors
    // at the first located frame at or after the change time.
    struct PivotReferenceChange {
        XrReferenceSpaceType type;
        XrTime change_time;
    };
    std::vector<PivotReferenceChange> pivotxr_reference_changes_;
    bool PivotAnchorAffectedBy(XrReferenceSpaceType type) const;
    bool depthxr_toggle_enabled_{true};
    bool depthxr_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_binding_last_poll_time_;
    std::optional<std::chrono::steady_clock::time_point> depthxr_binding_last_poll_time_;
    bool depthxr_binding_down_cached_{false};
    std::optional<InputBinding> depthxr_toggle_pressed_; // its list's binding that last went down (ActingBinding)
    bool depth_anchor_toggle_inverted_{false};
    bool depth_anchor_active_{false};
    bool depth_anchor_toggle_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point> depth_anchor_binding_last_poll_time_;
    bool depth_anchor_binding_down_cached_{false};
    std::optional<InputBinding> depth_anchor_toggle_pressed_; // its list's binding that last went down (ActingBinding)

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
    // Async pacing must never expose a pass-through gap between retiring one
    // runtime wait and publishing the next. DCS can call xrWaitFrame from its
    // sim thread while xrEndFrame is still running on the render thread; if
    // that app wait reaches the runtime during the gap, the replacement worker
    // issues a second real wait and runtimes such as Virtual Desktop's Oculus
    // compatibility path interlock until the slipped wait receives a Begin.
    // The handoff shield keeps app Wait/Begin calls fabricated across that
    // drain -> Begin -> End -> replacement-publication window.
    std::condition_variable turbo_async_handoff_cv_;
    bool turbo_async_handoff_active_{false};
    std::uint64_t turbo_async_handoff_armed_total_{0};
    std::uint64_t turbo_async_handoff_wait_intercepts_{0};
    std::uint64_t turbo_async_handoff_begin_intercepts_{0};
    std::uint64_t turbo_async_handoff_second_poll_blocks_{0};
    std::uint64_t turbo_async_handoff_cancellations_{0};
    int turbo_async_handoff_debug_log_budget_{0};
    std::thread turbo_async_worker_;
    bool turbo_async_worker_stop_{false};
    // A worker abandoned at teardown (blocked inside the runtime) exits on its
    // own once its epoch is stale; it never takes a later session's job.
    std::uint64_t turbo_async_worker_epoch_{0};
    bool turbo_async_job_pending_{false};
    XrSession turbo_async_job_session_{XR_NULL_HANDLE};
    std::shared_ptr<std::promise<void>> turbo_async_job_completion_;
    // Shared snapshots let WaitFrame, EndFrame, and teardown observe the same
    // in-flight wait without concurrently mutating a std::future object.
    std::shared_future<void> turbo_async_wait_;
    // The worker's latest job. Session teardown clears turbo_async_wait_ even
    // when that wait is still inside the runtime; this one stays until the
    // worker stops, so StopTurboAsyncWorker can tell a busy worker from an
    // idle one.
    std::shared_future<void> turbo_async_worker_job_;
    std::uint64_t turbo_async_wait_generation_{0};
    // (turbo_mutex_) The pipelined wait generation the runtime held past a
    // late-wait bound (0: none). Later app polls and submits for that same
    // wait give up after the short drain bound, so the application's frame
    // loop keeps running while the runtime holds it (issue #88). Generations
    // never repeat, so a newer wait is never mistaken for this one.
    std::uint64_t turbo_async_held_generation_{0};
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
    std::optional<InputBinding> turbo_toggle_pressed_; // its list's binding that last went down (ActingBinding)
    bool has_logged_turbo_varjo_note_{false};
    TurboRecoveryGuard turbo_recovery_; // mutex_ (config lock)
    bool turbo_recovery_retry_requested_{false};
    std::atomic<bool> turbo_recovery_blocked_{false};
    std::atomic<int> turbo_runtime_error_streak_{0};
    std::atomic<bool> turbo_effective_active_{false};
    std::atomic<bool> turbo_effective_async_{false};
    enum class TurboFrameOperation { kWait, kBegin, kEnd };
    // Records a failed runtime frame call. Failures never stop frame
    // forwarding: ForwardEndFrame re-establishes a known runtime frame state
    // (see OpenTurboRuntimeFrame), and three consecutive failures fall back or
    // suspend Turbo for the session while frames continue at runtime pacing.
    void NoteTurboRuntimeFailure(XrResult result, XrTime submitted_display_time,
                                 TurboFrameOperation operation = TurboFrameOperation::kEnd);
    // Frame thread only. Opens the runtime frame a submit needs when none is
    // open. A second xrWaitFrame must never be stacked on a waited-but-unbegun
    // frame: the spec makes it block until a begin that would never arrive.
    // When a failed begin left that uncertain, xrBeginFrame probes it first;
    // XR_ERROR_CALL_ORDER_INVALID proves no waited frame exists.
    XrResult OpenTurboRuntimeFrame(XrSession session, XrTime display_time, double& frame_blocked_ms);
    // Frame thread only, after the async wait has been retired. Moves frame
    // ownership to frame-thread (sequenced) pacing without an interception
    // gap: an app frame already fabricated keeps its begin swallowed.
    void HandOverTurboToFrameThread(const char* reason);
    // (turbo_mutex_) A runtime wait succeeded but its begin failed with an
    // error that does not prove the frame unconsumed.
    bool turbo_begin_probe_required_{false};
    // (frame thread) The runtime rejected a begin issued before the pipelined
    // wait completed (XR_ERROR_CALL_ORDER_INVALID): it does not interlock the
    // wait with submission, so late waits are waited for instead.
    bool turbo_async_waits_before_begin_{false};
    int turbo_dropped_frame_log_budget_{0};
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
    enum class TurboPacingSource { kForced, kPinned, kDiscovered, kProbing, kFallback };
    std::uint64_t turbo_test_started_at_{0};
    bool TryTurboAutoFallbackLocked(const std::string& reason);
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
                                      bool update_smoothing,
                                      const XrSpaceLocation* anchor_space_in_reference = nullptr);
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
    // Frame-thread state (ForwardEndFrame only). Only a manual toggle-off keeps
    // an async pipeline re-coupled. A stall while re-coupled, a safety
    // suspension, a recovery block, or disabling Turbo in settings hands the
    // owned frame to frame-thread (sequenced) pacing, which cannot interlock
    // with submission. A handover re-resolves the configured strategy at the
    // next session.
    bool turbo_async_recouple_stalled_{false};
    bool turbo_async_handed_over_{false};
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
    std::optional<InputBinding> turbo_metrics_pressed_; // its list's binding that last went down (ActingBinding)
    // The fields above belong to the frame thread. A settings refresh (any
    // thread) asks for their reset here; the frame thread performs it.
    std::atomic<bool> turbo_metrics_binding_reset_pending_{false};
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
    // Of the frames whose quad views Frame Generation claimed (composes).
    std::optional<std::uint64_t> last_claimed_composition_topology_signature_;
    bool quad_views_extension_requested_{false};
    bool varjo_foveated_rendering_extension_requested_{false};
    // Either extension above, or OpenXR 1.1 (where the foveated inset is core).
    bool application_can_select_quad_views_{false};
    // The last xrEnumerateViewConfigurations listed the foveated inset.
    bool quad_view_configuration_offered_{false};
    bool d3d11_graphics_extension_requested_{false};
    // XR_KHR_D3D12_enable is enabled on the instance (the Direct3D 11 bridge needs it).
    bool d3d12_extension_enabled_{false};
    // The application's Vulkan extension (InstanceCreateDiagnostics::vulkan_enable_version).
    int vulkan_enable_version_{0};
    // InstanceCreateDiagnostics::vulkan_enable1_requested.
    bool vulkan_enable1_requested_{false};
    // Latched when the instance was made: the Vulkan bridge was wanted then
    // and the instance has XR_KHR_D3D12_enable, so the XR_KHR_vulkan_enable(2)
    // hooks add what the bridge needs (vulkan_enable_hooks.h).
    bool vulkan_bridge_extensions_{false};
    bool eye_gaze_extension_enabled_{false};
    // True for the life of the instance when native Varjo quad-views were
    // forwarded to the runtime (Varjo compatible quadviews). Single source of truth
    // that suppresses quad->stereo synthesis, stereo composite, and combined-eye
    // emulation on this instance.
    bool varjo_compatible_quadviews_active_{false};
    bool defer_quadviews_swapchain_releases_{false};
    // Quadviews changes the application's view/swapchain topology. Once a session
    // is created, keep its initial activation state until teardown so a config
    // reload cannot remove the compositor from underneath a running title.
    std::optional<bool> quadviews_session_active_;
    std::optional<bool> deferred_quadviews_config_active_;
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
    bool eye_gaze_bindings_ready_{false};
    bool eye_gaze_binding_augmentation_disabled_{false};
    // Instance-owned application actions may survive session recreation. Cache
    // only value data; a caller's extension chain must never be replayed.
    std::vector<XrActionSuggestedBinding> application_eye_gaze_bindings_;
    bool application_eye_gaze_binding_chain_unsupported_{false};
    DelayedActionSetAttachment<XrActionSet> eye_gaze_action_set_attachment_;
    bool has_logged_eye_gaze_focus_active_{false};
    bool has_logged_eye_gaze_focus_unavailable_{false};
    uint32_t eye_gaze_unavailable_streak_{0};
    double quadviews_smoothed_focus_yaw_radians_{0.0};
    double quadviews_raw_focus_yaw_radians_{0.0};
    double quadviews_raw_focus_pitch_radians_{0.0};
    XrTime quadviews_raw_focus_time_{0};
    bool quadviews_raw_focus_valid_{false};
    // Synthesized-compositor diagnostic visualization. It deliberately starts
    // hidden every session and can only be changed through its optional binding.
    bool quadviews_diagnostic_visualization_enabled_{false};
    bool quadviews_diagnostic_visualization_binding_was_down_{false};
    std::optional<std::chrono::steady_clock::time_point>
        quadviews_diagnostic_visualization_binding_last_poll_time_;
    bool quadviews_diagnostic_visualization_binding_down_cached_{false};
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
    uint32_t logged_visibility_mask_shapes_{0}; // one bit per synthesized view and mask type
    bool has_logged_system_properties_{false};
    uint32_t cached_quadviews_stereo_recommended_width_{0};
    uint32_t cached_quadviews_stereo_recommended_height_{0};
    uint32_t cached_quadviews_stereo_max_width_{0};
    uint32_t cached_quadviews_stereo_max_height_{0};
    std::unordered_set<XrSpace> tracked_view_spaces_;
    // Action resources outlive xrDestroyAction/xrDestroyActionSet when spaces
    // still reference them. Shared identity prevents reused handles from
    // changing the classification of an existing eye-gaze space.
    struct ApplicationPoseAction {
        XrActionSet owner{XR_NULL_HANDLE};
        bool eye_gaze{false};
    };
    std::unordered_map<XrAction, std::shared_ptr<ApplicationPoseAction>> application_pose_actions_;
    std::unordered_map<XrAction, XrActionSet> application_action_owners_;
    // Instance lifetime: application action sets ever passed to a successful
    // xrAttachSessionActionSets. Their actions can never be suggested again
    // (XR_ERROR_ACTIONSETS_ALREADY_ATTACHED), even for a later session.
    std::unordered_set<XrActionSet> application_attached_action_sets_;
    struct ApplicationActionSpace {
        std::shared_ptr<ApplicationPoseAction> action;
        bool includes_eyes{false};
    };
    std::unordered_map<XrSpace, ApplicationActionSpace> application_action_spaces_;
    bool IsHeadRelativeEyeGaze(XrSpace space, XrSpace base_space) const;
    std::unordered_set<XrSpace> tracked_local_spaces_;
    std::unordered_set<XrSpace> tracked_stage_spaces_;
    std::vector<XrPosef> cached_eye_offset_poses_;
    std::vector<XrView> eye_offset_views_scratch_;
    XrTime cached_eye_offsets_display_time_{0};
    XrViewConfigurationType cached_eye_offsets_view_configuration_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    std::optional<std::chrono::steady_clock::time_point> last_app_action_sync_time_;
    std::optional<std::chrono::steady_clock::time_point> last_eye_gaze_self_sync_time_;
    std::optional<std::chrono::steady_clock::time_point> last_app_action_sync_attempt_time_;
    std::optional<std::chrono::steady_clock::time_point> last_eye_gaze_self_sync_attempt_time_;
    std::optional<XrResult> last_app_action_sync_result_;
    std::optional<XrResult> last_eye_gaze_self_sync_result_;
    XrSessionState observed_session_state_{XR_SESSION_STATE_UNKNOWN};
    bool eye_gaze_sync_not_focused_{false};
    uint32_t eye_gaze_sync_transition_logs_remaining_{32};
    std::optional<std::chrono::steady_clock::time_point> last_eye_gaze_bound_sources_probe_;
    std::string eye_gaze_bound_sources_diagnostic_{"not-probed"};
    std::vector<ViewAdjustmentData> locate_views_original_scratch_;
    std::vector<ViewAdjustmentData> locate_views_adjusted_scratch_;
    std::vector<ViewAdjustmentData> locate_views_depth_native_scratch_;
    std::vector<std::vector<XrCompositionLayerProjectionView>> end_frame_projection_views_scratch_;
    std::vector<XrCompositionLayerProjection> end_frame_projection_layers_scratch_;
    std::vector<const XrCompositionLayerBaseHeader*> end_frame_layers_scratch_;
    std::vector<ResolvedPivotSpaceDelta> end_frame_pivot_space_deltas_scratch_;
    std::map<XrTime, PivotPoseDeltaFrame> cached_pivot_pose_deltas_;
    // A pruned frame kept for the next one CachePivotPoseDelta stores.
    std::map<XrTime, PivotPoseDeltaFrame>::node_type pivot_pose_delta_spare_;
    // Info-level breadcrumbs are emitted once per submitted space, while
    // debug diagnostics retain per-frame details without spamming normal logs.
    std::unordered_set<XrSpace> logged_pivot_space_conversions_;
    std::unordered_set<XrSpace> failed_pivot_space_conversions_;
    std::map<XrTime, std::vector<DepthSubmissionGeometry>> cached_depth_submission_geometry_;
    // A pruned frame kept for the next one CacheDepthSubmissionGeometry stores.
    std::map<XrTime, std::vector<DepthSubmissionGeometry>>::node_type depth_submission_geometry_spare_;
    QuadViewsFrameCache<QuadViewsFrameState> cached_quadviews_frames_;
    // Quad views placement diagnostics (BuildQuadViewsComposePlan), per session.
    uint64_t quadviews_placement_mismatch_frames_{0};
    uint32_t quadviews_placement_mismatch_logs_remaining_{3};
    std::array<std::array<float, 4>, 2> quadviews_previous_focus_rect_{};
    std::array<bool, 2> quadviews_has_previous_focus_rect_{};
    std::array<double, 2> quadviews_focus_max_step_px_{};
    // The inset's motion since the last Info-level summary
    // (LogQuadViewsInsetMotion): whether, how often and how far it moves,
    // without debug logging.
    struct QuadViewsInsetMotion {
        uint64_t frames{0};       // planned frames with a previous inset to compare
        uint64_t moved_frames{0}; // of them, those whose inset moved 1 output pixel or more
        uint64_t gaze_frames{0};  // of them, those placed by the eye gaze
        double step_px_sum{0.0};  // the frames' moves (the larger eye's), in output pixels
        std::array<double, 2> max_step_px{};
        uint32_t output_width{0};
        uint32_t output_height{0};
    };
    QuadViewsInsetMotion quadviews_inset_motion_{};
    std::optional<std::chrono::steady_clock::time_point> quadviews_inset_motion_since_;
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
    PFN_xrPollEvent next_poll_event_{nullptr};
    PFN_xrLocateViews next_locate_views_{nullptr};
    PFN_xrStringToPath next_string_to_path_{nullptr};
    PFN_xrCreateActionSet next_create_action_set_{nullptr};
    PFN_xrDestroyActionSet next_destroy_action_set_{nullptr};
    PFN_xrCreateAction next_create_action_{nullptr};
    PFN_xrDestroyAction next_destroy_action_{nullptr};
    PFN_xrSuggestInteractionProfileBindings next_suggest_interaction_profile_bindings_{nullptr};
    PFN_xrGetActionStatePose next_get_action_state_pose_{nullptr};
    PFN_xrEnumerateBoundSourcesForAction next_enumerate_bound_sources_for_action_{nullptr};
    PFN_xrGetCurrentInteractionProfile next_get_current_interaction_profile_{nullptr};
    PFN_xrPathToString next_path_to_string_{nullptr};
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
