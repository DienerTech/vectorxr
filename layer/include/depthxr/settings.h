#pragma once

#include <optional>
#include <string>
#include <vector>

namespace depthxr {

enum class LogLevel {
    Error,
    Info,
    Debug,
};

enum class ActivationMode {
    Toggle,
    Hold,
};

enum class InputBindingType {
    None,
    Keyboard,
    Device,
};

enum class QuadViewsTrackingMode {
    Head,
    Eye,
};

enum class PerformanceCollectionMode {
    Summary,
    Diagnostic,
};

// Custom profiles override the defaults for their applications. The legacy
// Disable value is still parsed for older configs but no longer has runtime
// behavior; a profile's enabled flag controls whether it participates.
enum class ProfileMode {
    Custom,
    Disable,
};

struct InputBinding {
    InputBindingType type{InputBindingType::None};
    std::vector<std::string> chord;
    std::string device_guid;
    std::string input_path{"button-1"};
    std::string product_guid;
    std::string device_name;
    std::string input_label;
};

struct CoreSettings {
    bool enabled{true};
    LogLevel log_level{LogLevel::Info};
    int log_retention_files{7};
    bool track_seen_apps{true};
};

struct DepthXrSettingsOverride {
    std::optional<bool> stereo_boost_enabled;
    std::optional<bool> convergence_enabled;
    std::optional<double> stereo_boost;
    std::optional<double> convergence;
};

struct DepthXrResolvedSettings {
    bool enabled{true};
    bool stereo_boost_enabled{true};
    bool convergence_enabled{true};
    double stereo_boost{1.0};
    double convergence{0.0};
};

struct DepthXrBindings {
    InputBinding toggle_enabled{InputBindingType::None, {}, "", "button-1", "", "", ""};
};

struct ProfileMatch {
    std::string exe_name;
};

struct RegisteredApplication {
    std::string id;
    std::string name;
    bool enabled{true};
    ProfileMatch match;
};

struct DepthXrProfile {
    std::string name;
    std::vector<std::string> application_ids;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    DepthXrSettingsOverride settings;
};

struct DepthXrModuleConfig {
    bool enabled{true};
    DepthXrResolvedSettings defaults;
    DepthXrBindings bindings;
    std::vector<DepthXrProfile> profiles;
};

struct PivotXrSettings {
    double yaw_rotation_multiplier{1.5};
    double yaw_smoothing{0.2};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{25.0};
    double pitch_rotation_multiplier{1.0};
    double pitch_smoothing{0.2};
    double pitch_deadzone_degrees{12.0};
    double pitch_max_extra_degrees{20.0};
};

struct PivotXrProfile {
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    PivotXrSettings settings;
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrSettings defaults;
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    std::vector<PivotXrProfile> profiles;
};

// Resolved at runtime for a specific executable — flattened from module + matched profile.
struct PivotXrResolvedSettings {
    bool enabled{false};
    ActivationMode activation_mode{ActivationMode::Toggle};
    InputBinding activation_binding;
    double yaw_rotation_multiplier{1.5};
    double yaw_smoothing{0.2};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{25.0};
    double pitch_rotation_multiplier{1.0};
    double pitch_smoothing{0.2};
    double pitch_deadzone_degrees{12.0};
    double pitch_max_extra_degrees{20.0};
};

struct QuadViewsSettings {
    QuadViewsTrackingMode tracking_mode{QuadViewsTrackingMode::Eye};
    double focus_horizontal_size_percent{32.0};
    double focus_vertical_size_percent{32.0};
    double focus_scale{1.1};
    double peripheral_scale{0.45};
    double foveate_sharpness{0.0};
    double transition_thickness_percent{25.0};
    double horizontal_offset_degrees{0.0};
    double vertical_offset_degrees{0.0};
    double gaze_smoothing{0.15};
    double gaze_deadzone_degrees{1.5};
};

struct QuadViewsProfile {
    std::string name;
    bool enabled{true};
    ProfileMode mode{ProfileMode::Custom};
    std::vector<std::string> application_ids;
    QuadViewsSettings settings;
};

struct QuadViewsModuleConfig {
    bool enabled{false};
    QuadViewsSettings defaults;
    std::vector<QuadViewsProfile> profiles;
};

struct QuadViewsResolvedSettings : QuadViewsSettings {
    bool enabled{false};
};

struct PerformanceMonitorProfile {
    std::string name;
    bool enabled{true};
    std::vector<std::string> application_ids;
    PerformanceCollectionMode collection_mode{PerformanceCollectionMode::Summary};
    int retention_sessions{20};
    bool allow_dynamic_consumers{false};
};

struct PerformanceMonitorModuleConfig {
    std::vector<PerformanceMonitorProfile> profiles;
};

struct PerformanceMonitorResolvedSettings {
    bool enabled{false};
    std::string profile_name;
    std::string application_id;
    PerformanceCollectionMode collection_mode{PerformanceCollectionMode::Summary};
    int retention_sessions{20};
    bool allow_dynamic_consumers{false};
};

struct ConfigDocument {
    int version{3};
    CoreSettings core;
    std::vector<RegisteredApplication> applications;
    DepthXrModuleConfig depthxr;
    PivotXrModuleConfig pivotxr;
    QuadViewsModuleConfig quadviews;
    PerformanceMonitorModuleConfig performance;
};

struct ResolvedRuntimeConfig {
    CoreSettings core;
    DepthXrResolvedSettings depthxr;
    DepthXrBindings depthxr_bindings;
    PivotXrResolvedSettings pivotxr;
    QuadViewsResolvedSettings quadviews;
    PerformanceMonitorResolvedSettings performance;
};

const char* ToString(LogLevel level);
std::optional<LogLevel> ParseLogLevel(const std::string& value);

const char* ToString(ActivationMode mode);
std::optional<ActivationMode> ParseActivationMode(const std::string& value);
std::optional<std::string> ParseActivationKey(const std::string& value);
const char* ToString(InputBindingType type);
std::optional<InputBindingType> ParseInputBindingType(const std::string& value);
const char* ToString(QuadViewsTrackingMode mode);
std::optional<QuadViewsTrackingMode> ParseQuadViewsTrackingMode(const std::string& value);
const char* ToString(PerformanceCollectionMode mode);
std::optional<PerformanceCollectionMode> ParsePerformanceCollectionMode(const std::string& value);
const char* ToString(ProfileMode mode);
std::optional<ProfileMode> ParseProfileMode(const std::string& value);

} // namespace depthxr
