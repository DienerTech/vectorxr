#include "depthxr/settings_resolver.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace depthxr {
namespace {

std::string NormalizeExe(std::string_view value) {
    std::string normalized(value);
    const size_t slash = normalized.find_last_of("/\\");
    if (slash != std::string::npos) {
        normalized = normalized.substr(slash + 1);
    }
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized;
}

void ApplyOverride(ResolvedSettings& target, const SettingsOverride& source) {
    if (source.enabled) {
        target.enabled = *source.enabled;
    }
    if (source.stereo_boost_enabled) {
        target.stereo_boost_enabled = *source.stereo_boost_enabled;
    }
    if (source.world_scale_enabled) {
        target.world_scale_enabled = *source.world_scale_enabled;
    }
    if (source.fov_scale_enabled) {
        target.fov_scale_enabled = *source.fov_scale_enabled;
    }
    if (source.stereo_boost) {
        target.stereo_boost = *source.stereo_boost;
    }
    if (source.world_scale) {
        target.world_scale = *source.world_scale;
    }
    if (source.fov_scale) {
        target.fov_scale = *source.fov_scale;
    }
    if (source.log_level) {
        target.log_level = *source.log_level;
    }
}

} // namespace

bool ExeNameMatches(std::string_view lhs, std::string_view rhs) {
    return NormalizeExe(lhs) == NormalizeExe(rhs);
}

const Profile* FindMatchingProfile(const ConfigDocument& config, std::string_view exe_name) {
    for (const Profile& profile : config.profiles) {
        if (ExeNameMatches(profile.match.exe_name, exe_name)) {
            return &profile;
        }
    }
    return nullptr;
}

ResolvedSettings ResolveSettings(const ConfigDocument& config, std::string_view exe_name) {
    ResolvedSettings resolved = config.global;

    if (const Profile* profile = FindMatchingProfile(config, exe_name)) {
        ApplyOverride(resolved, profile->settings);
    }

    return resolved;
}

} // namespace depthxr
