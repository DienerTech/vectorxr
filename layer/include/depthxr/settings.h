#pragma once

#include <optional>
#include <string>
#include <vector>

namespace depthxr {

enum class LogLevel {
    Off,
    Error,
    Info,
    Debug,
};

enum class ActivationMode {
    Toggle,
    Hold,
};

struct CoreSettings {
    bool enabled{true};
    LogLevel log_level{LogLevel::Info};
    int log_retention_files{7};
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
    DepthXrSettingsOverride settings;
};

struct DepthXrModuleConfig {
    bool enabled{true};
    DepthXrResolvedSettings defaults;
    std::vector<DepthXrProfile> profiles;
};

struct PivotXrResolvedSettings {
    bool enabled{false};
    ActivationMode activation_mode{ActivationMode::Toggle};
    std::string activation_key{"F8"};
    double yaw_rotation_multiplier{1.5};
    double yaw_smoothing{0.2};
    double yaw_deadzone_degrees{8.0};
    double yaw_max_extra_degrees{25.0};
    double pitch_rotation_multiplier{1.0};
    double pitch_smoothing{0.2};
    double pitch_deadzone_degrees{12.0};
    double pitch_max_extra_degrees{20.0};
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrResolvedSettings defaults;
};

struct ConfigDocument {
    int version{3};
    CoreSettings core;
    std::vector<RegisteredApplication> applications;
    DepthXrModuleConfig depthxr;
    PivotXrModuleConfig pivotxr;
};

struct ResolvedRuntimeConfig {
    CoreSettings core;
    DepthXrResolvedSettings depthxr;
    PivotXrResolvedSettings pivotxr;
};

const char* ToString(LogLevel level);
std::optional<LogLevel> ParseLogLevel(const std::string& value);

const char* ToString(ActivationMode mode);
std::optional<ActivationMode> ParseActivationMode(const std::string& value);
std::optional<std::string> ParseActivationKey(const std::string& value);

} // namespace depthxr
