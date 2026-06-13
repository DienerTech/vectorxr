#include "depthxr/settings_resolver.h"

#include <algorithm>
#include <cctype>

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

void ApplyDepthOverride(DepthXrResolvedSettings& target, const DepthXrSettingsOverride& source) {
    if (source.stereo_boost_enabled.has_value()) {
        target.stereo_boost_enabled = *source.stereo_boost_enabled;
    }
    if (source.convergence_enabled.has_value()) {
        target.convergence_enabled = *source.convergence_enabled;
    }
    if (source.stereo_boost.has_value()) {
        target.stereo_boost = *source.stereo_boost;
    }
    if (source.convergence.has_value()) {
        target.convergence = *source.convergence;
    }
}

} // namespace

bool ExeNameMatches(std::string_view lhs, std::string_view rhs) {
    return NormalizeExe(lhs) == NormalizeExe(rhs);
}

const RegisteredApplication* FindMatchingApplication(const ConfigDocument& config, std::string_view exe_name) {
    for (const RegisteredApplication& application : config.applications) {
        if (ExeNameMatches(application.match.exe_name, exe_name)) {
            return &application;
        }
    }
    return nullptr;
}

const DepthXrProfile* FindMatchingDepthXrProfile(const ConfigDocument& config, std::string_view exe_name) {
    const RegisteredApplication* application = FindMatchingApplication(config, exe_name);
    if (!application) {
        return nullptr;
    }

    for (const DepthXrProfile& profile : config.depthxr.profiles) {
        if (!profile.enabled) {
            continue;
        }

        if (std::find(profile.application_ids.begin(), profile.application_ids.end(), application->id) != profile.application_ids.end()) {
            return &profile;
        }
    }
    return nullptr;
}

DepthXrResolvedSettings ResolveDepthXrSettings(const ConfigDocument& config, std::string_view exe_name) {
    DepthXrResolvedSettings resolved = config.depthxr.defaults;
    resolved.enabled = config.depthxr.enabled;

    const DepthXrProfile* profile = FindMatchingDepthXrProfile(config, exe_name);
    if (!profile || !profile->enabled) {
        return resolved;
    }

    if (profile->mode == ProfileMode::Disable) {
        resolved.enabled = false;
        return resolved;
    }

    ApplyDepthOverride(resolved, profile->settings);
    return resolved;
}

const PivotXrProfile* FindMatchingPivotXrProfile(const ConfigDocument& config, std::string_view exe_name) {
    const RegisteredApplication* application = FindMatchingApplication(config, exe_name);
    if (!application) {
        return nullptr;
    }

    for (const PivotXrProfile& profile : config.pivotxr.profiles) {
        if (!profile.enabled) {
            continue;
        }

        if (std::find(profile.application_ids.begin(), profile.application_ids.end(), application->id) != profile.application_ids.end()) {
            return &profile;
        }
    }
    return nullptr;
}

void ApplyPivotSettings(PivotXrResolvedSettings& resolved, const PivotXrSettings& settings) {
    resolved.yaw_rotation_multiplier = settings.yaw_rotation_multiplier;
    resolved.yaw_smoothing = settings.yaw_smoothing;
    resolved.yaw_deadzone_degrees = settings.yaw_deadzone_degrees;
    resolved.yaw_max_extra_degrees = settings.yaw_max_extra_degrees;
    resolved.pitch_rotation_multiplier = settings.pitch_rotation_multiplier;
    resolved.pitch_smoothing = settings.pitch_smoothing;
    resolved.pitch_deadzone_degrees = settings.pitch_deadzone_degrees;
    resolved.pitch_max_extra_degrees = settings.pitch_max_extra_degrees;
}

PivotXrResolvedSettings ResolvePivotXrSettings(const ConfigDocument& config, std::string_view exe_name) {
    PivotXrResolvedSettings resolved;
    resolved.enabled = config.pivotxr.enabled;

    if (!config.pivotxr.enabled) {
        return resolved;
    }

    resolved.activation_mode = config.pivotxr.activation_mode;
    resolved.activation_binding = config.pivotxr.activation_binding;
    ApplyPivotSettings(resolved, config.pivotxr.defaults);

    const PivotXrProfile* profile = FindMatchingPivotXrProfile(config, exe_name);
    if (profile) {
        if (profile->mode == ProfileMode::Disable) {
            resolved.enabled = false;
            return resolved;
        }

        resolved.activation_mode = profile->activation_mode;
        resolved.activation_binding = profile->activation_binding;
        ApplyPivotSettings(resolved, profile->settings);
    }

    return resolved;
}

const QuadViewsProfile* FindMatchingQuadViewsProfile(const ConfigDocument& config, std::string_view exe_name) {
    const RegisteredApplication* application = FindMatchingApplication(config, exe_name);
    if (!application) {
        return nullptr;
    }

    for (const QuadViewsProfile& profile : config.quadviews.profiles) {
        if (!profile.enabled) {
            continue;
        }

        if (std::find(profile.application_ids.begin(), profile.application_ids.end(), application->id) != profile.application_ids.end()) {
            return &profile;
        }
    }
    return nullptr;
}

void ApplyQuadViewsSettings(QuadViewsResolvedSettings& resolved, const QuadViewsSettings& settings) {
    resolved.tracking_mode = settings.tracking_mode;
    resolved.focus_horizontal_size_percent = settings.focus_horizontal_size_percent;
    resolved.focus_vertical_size_percent = settings.focus_vertical_size_percent;
    resolved.focus_scale = settings.focus_scale;
    resolved.peripheral_scale = settings.peripheral_scale;
    resolved.foveate_sharpness = settings.foveate_sharpness;
    resolved.transition_thickness_percent = settings.transition_thickness_percent;
    resolved.horizontal_offset_degrees = settings.horizontal_offset_degrees;
    resolved.vertical_offset_degrees = settings.vertical_offset_degrees;
    resolved.gaze_smoothing = settings.gaze_smoothing;
    resolved.gaze_deadzone_degrees = settings.gaze_deadzone_degrees;
}

QuadViewsResolvedSettings ResolveQuadViewsSettings(const ConfigDocument& config, std::string_view exe_name) {
    QuadViewsResolvedSettings resolved;
    resolved.enabled = config.quadviews.enabled;
    ApplyQuadViewsSettings(resolved, config.quadviews.defaults);

    if (!config.quadviews.enabled) {
        return resolved;
    }

    const QuadViewsProfile* profile = FindMatchingQuadViewsProfile(config, exe_name);
    if (profile) {
        if (profile->mode == ProfileMode::Disable) {
            resolved.enabled = false;
            return resolved;
        }

        ApplyQuadViewsSettings(resolved, profile->settings);
    }

    return resolved;
}

ResolvedRuntimeConfig ResolveRuntimeConfig(const ConfigDocument& config, std::string_view exe_name) {
    ResolvedRuntimeConfig resolved;
    resolved.core = config.core;
    resolved.depthxr = ResolveDepthXrSettings(config, exe_name);
    resolved.depthxr_bindings = config.depthxr.bindings;
    resolved.pivotxr = ResolvePivotXrSettings(config, exe_name);
    resolved.quadviews = ResolveQuadViewsSettings(config, exe_name);
    return resolved;
}

} // namespace depthxr
