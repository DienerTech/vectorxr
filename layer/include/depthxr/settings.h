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

struct SettingsOverride {
    std::optional<bool> enabled;
    std::optional<bool> stereo_boost_enabled;
    std::optional<bool> convergence_enabled;
    std::optional<bool> world_scale_enabled;
    std::optional<bool> fov_scale_enabled;
    std::optional<double> stereo_boost;
    std::optional<double> convergence;
    std::optional<double> world_scale;
    std::optional<double> fov_scale;
    std::optional<LogLevel> log_level;
};

struct ResolvedSettings {
    bool enabled{true};
    bool stereo_boost_enabled{true};
    bool convergence_enabled{true};
    bool world_scale_enabled{true};
    bool fov_scale_enabled{true};
    double stereo_boost{1.0};
    double convergence{0.0};
    double world_scale{1.0};
    double fov_scale{1.0};
    LogLevel log_level{LogLevel::Info};
};

struct ProfileMatch {
    std::string exe_name;
};

struct Profile {
    ProfileMatch match;
    SettingsOverride settings;
};

struct ConfigDocument {
    int version{1};
    ResolvedSettings global;
    std::vector<Profile> profiles;
};

const char* ToString(LogLevel level);
std::optional<LogLevel> ParseLogLevel(const std::string& value);

} // namespace depthxr
