#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/config_parser.h"
#include "depthxr/logger.h"
#include "depthxr/settings_resolver.h"

namespace depthxr {

class OpenXrLayer {
  public:
    static OpenXrLayer& Instance();

    void SetLayerDirectory(std::filesystem::path dll_directory);
    void SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr);

    XrResult OnInstanceCreated(const XrInstanceCreateInfo* create_info, XrInstance instance);
    XrResult GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
    XrResult DestroyInstance(XrInstance instance);
    XrResult CreateSession(XrInstance instance, const XrSessionCreateInfo* create_info, XrSession* session);
    XrResult BeginSession(XrSession session, const XrSessionBeginInfo* begin_info);
    XrResult EndFrame(XrSession session, const XrFrameEndInfo* frame_end_info);
    XrResult CreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* create_info, XrSpace* space);
    XrResult LocateSpace(XrSpace space, XrSpace base_space, XrTime time, XrSpaceLocation* location);
    XrResult LocateViews(XrSession session,
                         const XrViewLocateInfo* view_locate_info,
                         XrViewState* view_state,
                         uint32_t view_capacity_input,
                         uint32_t* view_count_output,
                         XrView* views);

  private:
    OpenXrLayer() = default;

    void ReloadConfigIfNeeded();
    void RefreshResolvedSettings();
    void CaptureInstanceFunctions();
    void LogResolvedSettings(const ResolvedRuntimeConfig& settings);
    void ResetPivotActivationState();
    void ResetDepthToggleState();
    bool IsPivotXrActive(const PivotXrResolvedSettings& settings);
    bool IsDepthXrActive();
    void ResetSessionState();
    XrResult CreateInternalReferenceSpaces(XrSession session);
    bool EnsureEyeOffsets(XrSession session,
                          XrViewConfigurationType view_configuration_type,
                          XrTime display_time,
                          uint32_t view_count);
    void CachePivotPoseDelta(XrTime time, const XrPosef& pose_delta);
    XrPosef FindPivotPoseDelta(XrTime time) const;
    void PrunePivotPoseDeltas(XrTime time);
    bool IsTrackedViewSpace(XrSpace space) const;
    XrResult LocateSpaceWithPivot(XrSpace space,
                                  XrSpace base_space,
                                  XrTime time,
                                  const PivotXrResolvedSettings& settings,
                                  bool pivotxr_active,
                                  XrSpaceLocation* location,
                                  double* applied_extra_yaw_radians,
                                  double* applied_extra_pitch_radians);

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
    double pivotxr_smoothed_extra_yaw_radians_{0.0};
    double pivotxr_smoothed_extra_pitch_radians_{0.0};
    std::optional<std::chrono::steady_clock::time_point> pivotxr_last_smoothing_wall_time_;
    bool pivotxr_toggle_enabled_{false};
    bool pivotxr_activation_key_was_down_{false};
    bool depthxr_toggle_enabled_{true};
    bool depthxr_toggle_binding_was_down_{false};
    XrSession active_session_{XR_NULL_HANDLE};
    XrSpace internal_local_space_{XR_NULL_HANDLE};
    XrSpace internal_view_space_{XR_NULL_HANDLE};
    XrSpace internal_stage_space_{XR_NULL_HANDLE};
    XrViewConfigurationType active_primary_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    bool has_active_primary_view_configuration_{false};
    bool has_logged_quad_view_short_count_{false};
    bool has_logged_pivotxr_spike_mode_{false};
    std::unordered_set<XrSpace> tracked_view_spaces_;
    std::unordered_set<XrSpace> tracked_local_spaces_;
    std::unordered_set<XrSpace> tracked_stage_spaces_;
    std::vector<XrPosef> cached_eye_offset_poses_;
    std::map<XrTime, XrPosef> cached_pivot_pose_deltas_;

    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr_{nullptr};
    PFN_xrDestroyInstance next_destroy_instance_{nullptr};
    PFN_xrCreateSession next_create_session_{nullptr};
    PFN_xrBeginSession next_begin_session_{nullptr};
    PFN_xrEndFrame next_end_frame_{nullptr};
    PFN_xrCreateReferenceSpace next_create_reference_space_{nullptr};
    PFN_xrLocateSpace next_locate_space_{nullptr};
    PFN_xrLocateViews next_locate_views_{nullptr};
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
