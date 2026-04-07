#include "depthxr/openxr_layer.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "depthxr/config_path.h"
#include "depthxr/effects.h"
#include "depthxr/process_info.h"

namespace depthxr {
namespace {

bool NearlyEqual(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

bool SameSettings(const ResolvedSettings& lhs, const ResolvedSettings& rhs) {
    return lhs.enabled == rhs.enabled &&
           lhs.stereo_boost_enabled == rhs.stereo_boost_enabled &&
           lhs.world_scale_enabled == rhs.world_scale_enabled &&
           lhs.fov_scale_enabled == rhs.fov_scale_enabled &&
           NearlyEqual(lhs.stereo_boost, rhs.stereo_boost) &&
           NearlyEqual(lhs.world_scale, rhs.world_scale) &&
           NearlyEqual(lhs.fov_scale, rhs.fov_scale) &&
           lhs.log_level == rhs.log_level;
}

ConfigDocument DefaultConfig() {
    ConfigDocument document;
    document.version = 1;
    return document;
}

} // namespace

OpenXrLayer& OpenXrLayer::Instance() {
    static OpenXrLayer layer;
    return layer;
}

void OpenXrLayer::SetDllDirectory(std::filesystem::path dll_directory) {
    std::scoped_lock lock(mutex_);
    dll_directory_ = std::move(dll_directory);
}

void OpenXrLayer::SetNextProcAddr(PFN_xrGetInstanceProcAddr next_get_instance_proc_addr) {
    std::scoped_lock lock(mutex_);
    next_get_instance_proc_addr_ = next_get_instance_proc_addr;
}

XrResult OpenXrLayer::OnInstanceCreated(const XrInstanceCreateInfo* create_info, XrInstance instance) {
    std::scoped_lock lock(mutex_);

    instance_ = instance;
    config_path_ = ResolveConfigPath();
    log_path_ = ResolveLogPath();
    logger_.Initialize(log_path_);

    current_exe_name_ = GetCurrentExecutableName();
    logger_.Info("DepthXR attached to process: " + current_exe_name_);

    if (create_info) {
        std::ostringstream stream;
        stream << "Application=" << create_info->applicationInfo.applicationName << ", Engine="
               << create_info->applicationInfo.engineName;
        logger_.Info(stream.str());
    }

    CaptureInstanceFunctions();
    if (!next_destroy_instance_ || !next_locate_views_) {
        logger_.Error("Failed to resolve required OpenXR function pointers");
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    ReloadConfigIfNeeded();
    RefreshResolvedSettings();
    return XR_SUCCESS;
}

XrResult OpenXrLayer::GetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    if (!function || !name) {
        return XR_ERROR_VALIDATION_FAILURE;
    }

    if (std::string_view(name) == "xrGetInstanceProcAddr") {
        return next_get_instance_proc_addr_(instance, name, function);
    }

    return next_get_instance_proc_addr_(instance, name, function);
}

XrResult OpenXrLayer::DestroyInstance(XrInstance instance) {
    std::scoped_lock lock(mutex_);

    const XrResult result = next_destroy_instance_(instance);
    if (XR_SUCCEEDED(result)) {
        instance_ = XR_NULL_HANDLE;
        next_destroy_instance_ = nullptr;
        next_locate_views_ = nullptr;
        has_loaded_config_ = false;
    }

    return result;
}

XrResult OpenXrLayer::LocateViews(XrSession session,
                                  const XrViewLocateInfo* view_locate_info,
                                  XrViewState* view_state,
                                  uint32_t view_capacity_input,
                                  uint32_t* view_count_output,
                                  XrView* views) {
    const XrResult result =
        next_locate_views_(session, view_locate_info, view_state, view_capacity_input, view_count_output, views);

    if (XR_FAILED(result) || !views || !view_count_output) {
        return result;
    }

    std::scoped_lock lock(mutex_);
    ReloadConfigIfNeeded();
    RefreshResolvedSettings();

    if (!resolved_settings_.enabled) {
        return result;
    }

    const uint32_t count = std::min(view_capacity_input, *view_count_output);
    if (count == 0) {
        return result;
    }

    // DepthXR keeps the interception surface minimal and applies all current
    // stereo/depth experiments in xrLocateViews.
    std::vector<ViewAdjustmentData> adjusted_views(count);
    for (uint32_t i = 0; i < count; ++i) {
        adjusted_views[i].position.x = views[i].pose.position.x;
        adjusted_views[i].position.y = views[i].pose.position.y;
        adjusted_views[i].position.z = views[i].pose.position.z;
        adjusted_views[i].fov.angle_left = views[i].fov.angleLeft;
        adjusted_views[i].fov.angle_right = views[i].fov.angleRight;
        adjusted_views[i].fov.angle_up = views[i].fov.angleUp;
        adjusted_views[i].fov.angle_down = views[i].fov.angleDown;
    }

    if (resolved_settings_.stereo_boost_enabled && !NearlyEqual(resolved_settings_.stereo_boost, 1.0)) {
        ApplyStereoBoost(adjusted_views, resolved_settings_.stereo_boost);
    }
    if (resolved_settings_.world_scale_enabled && !NearlyEqual(resolved_settings_.world_scale, 1.0)) {
        ApplyWorldScale(adjusted_views, resolved_settings_.world_scale);
    }
    if (resolved_settings_.fov_scale_enabled && !NearlyEqual(resolved_settings_.fov_scale, 1.0)) {
        ApplyFovScale(adjusted_views, resolved_settings_.fov_scale);
    }

    for (uint32_t i = 0; i < count; ++i) {
        views[i].pose.position.x = static_cast<float>(adjusted_views[i].position.x);
        views[i].pose.position.y = static_cast<float>(adjusted_views[i].position.y);
        views[i].pose.position.z = static_cast<float>(adjusted_views[i].position.z);
        views[i].fov.angleLeft = static_cast<float>(adjusted_views[i].fov.angle_left);
        views[i].fov.angleRight = static_cast<float>(adjusted_views[i].fov.angle_right);
        views[i].fov.angleUp = static_cast<float>(adjusted_views[i].fov.angle_up);
        views[i].fov.angleDown = static_cast<float>(adjusted_views[i].fov.angle_down);
    }

    return result;
}

void OpenXrLayer::ReloadConfigIfNeeded() {
    if (config_path_.empty()) {
        config_path_ = ResolveConfigPath();
    }

    if (!std::filesystem::exists(config_path_)) {
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
            logger_.Info("No config file found. Using default settings.");
        }
        return;
    }

    const auto timestamp = std::filesystem::last_write_time(config_path_);
    if (has_loaded_config_ && has_config_timestamp_ && timestamp == last_config_write_time_) {
        return;
    }

    const ParseResult loaded = LoadConfigFromFile(config_path_);
    if (!loaded.ok) {
        logger_.Error("Failed to parse config: " + loaded.error);
        if (!has_loaded_config_) {
            config_ = DefaultConfig();
            has_loaded_config_ = true;
        }
        return;
    }

    config_ = loaded.document;
    has_loaded_config_ = true;
    has_config_timestamp_ = true;
    last_config_write_time_ = timestamp;
    logger_.Info("Loaded config from " + config_path_.string());
}

void OpenXrLayer::RefreshResolvedSettings() {
    resolved_settings_ = ResolveSettings(config_, current_exe_name_);
    logger_.SetLevel(resolved_settings_.log_level);
    if (!last_logged_settings_ || !SameSettings(*last_logged_settings_, resolved_settings_)) {
        LogResolvedSettings(resolved_settings_);
        last_logged_settings_ = resolved_settings_;
    }
}

void OpenXrLayer::CaptureInstanceFunctions() {
    PFN_xrVoidFunction function = nullptr;

    if (XR_SUCCEEDED(next_get_instance_proc_addr_(
            instance_, "xrDestroyInstance", &function))) {
        next_destroy_instance_ = reinterpret_cast<PFN_xrDestroyInstance>(function);
    }

    function = nullptr;
    if (XR_SUCCEEDED(next_get_instance_proc_addr_(instance_, "xrLocateViews", &function))) {
        next_locate_views_ = reinterpret_cast<PFN_xrLocateViews>(function);
    }
}

void OpenXrLayer::LogResolvedSettings(const ResolvedSettings& settings) {
    std::ostringstream stream;
    stream << "Resolved settings for " << current_exe_name_ << ": "
           << "enabled=" << settings.enabled
           << ", stereoBoost=" << settings.stereo_boost
           << ", worldScale=" << settings.world_scale
           << ", fovScale=" << settings.fov_scale
           << ", logLevel=" << ToString(settings.log_level);
    logger_.Debug(stream.str());
}

} // namespace depthxr
