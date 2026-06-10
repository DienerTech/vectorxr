#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/config_parser.h"
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

    XrResult OnInstanceCreated(const XrInstanceCreateInfo* create_info,
                               XrInstance instance,
                               bool eye_gaze_extension_enabled);
    XrResult GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
    XrResult DestroyInstance(XrInstance instance);
    XrResult CreateSession(XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session);
    XrResult DestroySession(XrSession session);
    XrResult BeginSession(XrSession session, const XrSessionBeginInfo* begin_info);
    XrResult AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attach_info);
    XrResult SyncActions(XrSession session, const XrActionsSyncInfo* sync_info);
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
        uint32_t failure_logs_remaining{8};
        uint32_t next_gpu_timing_query{0};
        std::array<QuadViewsInputCopy, 4> input_copies;
        std::array<QuadViewsCompositionTarget, 2> targets;
        std::array<QuadViewsGpuTimingQuery, 4> gpu_timing_queries;
    };

    void ReloadConfigIfNeeded();
    void RefreshResolvedSettings();
    void CaptureInstanceFunctions();
    void LogResolvedSettings(const ResolvedRuntimeConfig& settings);
    void ResetPivotActivationState();
    void ResetDepthToggleState();
    bool IsPivotXrActive(const PivotXrResolvedSettings& settings);
    bool IsDepthXrActive();
    bool IsQuadViewsActive() const;
    void ResetSwapchainState();
    void LogSwapchainSummary(XrSwapchain swapchain, const SwapchainInfo& info, std::string_view event_name);
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
                                  const PivotXrResolvedSettings& settings,
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
    std::string last_failed_config_error_;
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
    double pivotxr_smoothed_extra_yaw_radians_{0.0};
    double pivotxr_smoothed_extra_pitch_radians_{0.0};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_last_smoothing_wall_time_;
    bool pivotxr_toggle_enabled_{false};
    bool pivotxr_activation_key_was_down_{false};
    bool depthxr_toggle_enabled_{true};
    bool depthxr_toggle_binding_was_down_{false};
    bool quad_views_extension_requested_{false};
    bool varjo_foveated_rendering_extension_requested_{false};
    bool eye_gaze_extension_enabled_{false};
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
    double quadviews_smoothed_focus_yaw_radians_{0.0};
    double quadviews_smoothed_focus_pitch_radians_{0.0};
    std::optional<std::chrono::steady_clock::time_point> quadviews_last_focus_smoothing_wall_time_;
    XrViewConfigurationType active_primary_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrViewConfigurationType active_runtime_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    bool has_active_primary_view_configuration_{false};
    bool has_logged_quad_view_short_count_{false};
    bool has_logged_pivotxr_spike_mode_{false};
    bool has_logged_quadviews_view_configuration_capabilities_{false};
    uint32_t cached_quadviews_stereo_recommended_width_{0};
    uint32_t cached_quadviews_stereo_recommended_height_{0};
    std::unordered_set<XrSpace> tracked_view_spaces_;
    std::unordered_set<XrSpace> tracked_local_spaces_;
    std::unordered_set<XrSpace> tracked_stage_spaces_;
    std::vector<XrPosef> cached_eye_offset_poses_;
    std::map<XrTime, XrPosef> cached_pivot_pose_deltas_;
    std::map<XrTime, std::array<XrFovf, 4>> cached_quadviews_fovs_;
    std::unordered_map<XrSwapchain, SwapchainInfo> tracked_swapchains_;
    D3D11QuadViewsCompositor d3d11_quadviews_compositor_;

    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr_{nullptr};
    PFN_xrDestroyInstance next_destroy_instance_{nullptr};
    PFN_xrCreateSession next_create_session_{nullptr};
    PFN_xrDestroySession next_destroy_session_{nullptr};
    PFN_xrBeginSession next_begin_session_{nullptr};
    PFN_xrAttachSessionActionSets next_attach_session_action_sets_{nullptr};
    PFN_xrSyncActions next_sync_actions_{nullptr};
    PFN_xrEndFrame next_end_frame_{nullptr};
    PFN_xrGetSystemProperties next_get_system_properties_{nullptr};
    PFN_xrEnumerateEnvironmentBlendModes next_enumerate_environment_blend_modes_{nullptr};
    PFN_xrEnumerateViewConfigurations next_enumerate_view_configurations_{nullptr};
    PFN_xrGetViewConfigurationProperties next_get_view_configuration_properties_{nullptr};
    PFN_xrEnumerateViewConfigurationViews next_enumerate_view_configuration_views_{nullptr};
    PFN_xrCreateSwapchain next_create_swapchain_{nullptr};
    PFN_xrDestroySwapchain next_destroy_swapchain_{nullptr};
    PFN_xrEnumerateSwapchainImages next_enumerate_swapchain_images_{nullptr};
    PFN_xrAcquireSwapchainImage next_acquire_swapchain_image_{nullptr};
    PFN_xrWaitSwapchainImage next_wait_swapchain_image_{nullptr};
    PFN_xrReleaseSwapchainImage next_release_swapchain_image_{nullptr};
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
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
