#pragma once

#include <filesystem>
#include <mutex>
#include <optional>

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
    XrResult BeginSession(XrSession session, const XrSessionBeginInfo* begin_info);
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

    std::mutex mutex_;
    std::filesystem::path dll_directory_;
    std::filesystem::path config_path_;
    std::filesystem::path log_path_;
    std::filesystem::file_time_type last_config_write_time_{};
    bool has_config_timestamp_{false};

    Logger logger_;
    ConfigDocument config_;
    bool has_loaded_config_{false};
    std::string current_exe_name_;
    ResolvedRuntimeConfig resolved_settings_;
    std::optional<ResolvedRuntimeConfig> last_logged_settings_;
    uint64_t locate_views_call_count_{0};
    uint32_t pending_locate_views_diagnostics_{0};
    XrSession active_session_{XR_NULL_HANDLE};
    XrViewConfigurationType active_primary_view_configuration_type_{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    bool has_active_primary_view_configuration_{false};
    bool has_logged_quad_view_short_count_{false};

    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr_{nullptr};
    PFN_xrDestroyInstance next_destroy_instance_{nullptr};
    PFN_xrBeginSession next_begin_session_{nullptr};
    PFN_xrLocateViews next_locate_views_{nullptr};
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
