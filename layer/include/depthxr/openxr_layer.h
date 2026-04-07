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

    void SetDllDirectory(std::filesystem::path dll_directory);
    void SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr);

    XrResult OnInstanceCreated(const XrInstanceCreateInfo* create_info, XrInstance instance);
    XrResult GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
    XrResult DestroyInstance(XrInstance instance);
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
    void LogResolvedSettings(const ResolvedSettings& settings);

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
    ResolvedSettings resolved_settings_;
    std::optional<ResolvedSettings> last_logged_settings_;

    PFN_xrGetInstanceProcAddr next_get_instance_proc_addr_{nullptr};
    PFN_xrDestroyInstance next_destroy_instance_{nullptr};
    PFN_xrLocateViews next_locate_views_{nullptr};
    XrInstance instance_{XR_NULL_HANDLE};
};

} // namespace depthxr
