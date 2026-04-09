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

struct DepthXrProfile {
    std::string name;
    ProfileMatch match;
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
    double rotation_multiplier{1.5};
    double smoothing{0.2};
    double deadzone_degrees{8.0};
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrResolvedSettings defaults;
};

struct ConfigDocument {
    int version{2};
    CoreSettings core;
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

} // namespace depthxr
