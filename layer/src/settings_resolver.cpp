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
    if (source.stereo_boost.has_value()) {
        target.stereo_boost = *source.stereo_boost;
    }
    if (source.convergence.has_value()) {
        target.convergence = *source.convergence;
    }
    if (source.depth_anchor.has_value()) {
        target.depth_anchor = *source.depth_anchor;
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

    resolved.enabled = true;
    ApplyDepthOverride(resolved, profile->settings);
    return resolved;
}

void ApplyPivotSettings(PivotXrResolvedProfile& resolved, const PivotXrSettings& settings) {
    resolved.smoothing = settings.smoothing;
    resolved.activation_ramp_seconds = settings.activation_ramp_seconds;
    resolved.yaw_rotation_multiplier = settings.yaw_rotation_multiplier;
    resolved.yaw_deadzone_degrees = settings.yaw_deadzone_degrees;
    resolved.yaw_max_extra_degrees = settings.yaw_max_extra_degrees;
    resolved.pitch_rotation_multiplier = settings.pitch_rotation_multiplier;
    resolved.pitch_deadzone_degrees = settings.pitch_deadzone_degrees;
    resolved.pitch_max_extra_degrees = settings.pitch_max_extra_degrees;
    resolved.response_mode = settings.response_mode;
    resolved.step_glide_mode = settings.step_glide_mode;
    resolved.step_glide_seconds = settings.step_glide_seconds;

    if (settings.advanced_axes) {
        resolved.yaw_positive = settings.yaw_left;
        resolved.yaw_negative = settings.yaw_right;
        resolved.pitch_positive = settings.pitch_up;
        resolved.pitch_negative = settings.pitch_down;
        resolved.yaw_step_positive = settings.yaw_left_step;
        resolved.yaw_step_negative = settings.yaw_right_step;
        resolved.pitch_step_positive = settings.pitch_up_step;
        resolved.pitch_step_negative = settings.pitch_down_step;
    } else {
        const PivotAxisTuning yaw{settings.yaw_rotation_multiplier, settings.yaw_deadzone_degrees,
                                  settings.yaw_max_extra_degrees};
        const PivotAxisTuning pitch{settings.pitch_rotation_multiplier, settings.pitch_deadzone_degrees,
                                    settings.pitch_max_extra_degrees};
        resolved.yaw_positive = yaw;
        resolved.yaw_negative = yaw;
        resolved.pitch_positive = pitch;
        resolved.pitch_negative = pitch;
        resolved.yaw_step_positive = settings.yaw_step;
        resolved.yaw_step_negative = settings.yaw_step;
        resolved.pitch_step_positive = settings.pitch_step;
        resolved.pitch_step_negative = settings.pitch_step;
    }
}

namespace {

// Binding identity for activation arbitration: two profiles collide when the
// same physical input would trigger both. Sound and display metadata are
// irrelevant here. Unbound (None) profiles never collide — they simply can't
// be activated by input.
bool SameActivationInput(const InputBinding& lhs, const InputBinding& rhs) {
    if (lhs.type != rhs.type || lhs.type == InputBindingType::None) {
        return false;
    }
    if (lhs.type == InputBindingType::Keyboard) {
        return lhs.chord == rhs.chord;
    }
    return lhs.device_guid == rhs.device_guid && lhs.input_path == rhs.input_path;
}

std::vector<PivotActivationBinding> FilterUnclaimedActivationBindings(
    const std::vector<PivotActivationBinding>& source,
    const std::vector<PivotXrResolvedProfile>& earlier_profiles) {
    std::vector<PivotActivationBinding> filtered;
    for (const PivotActivationBinding& activation : source) {
        const InputBinding& binding = activation.binding;
        if (binding.type == InputBindingType::None) {
            continue;
        }
        const bool claimed_here = std::any_of(filtered.begin(), filtered.end(), [&](const PivotActivationBinding& accepted) {
            return SameActivationInput(accepted.binding, binding);
        });
        const bool claimed_earlier = std::any_of(
            earlier_profiles.begin(), earlier_profiles.end(), [&](const PivotXrResolvedProfile& earlier) {
                return std::any_of(earlier.activation_bindings.begin(), earlier.activation_bindings.end(),
                                   [&](const PivotActivationBinding& earlier_binding) {
                                       return SameActivationInput(earlier_binding.binding, binding);
                                   });
            });
        if (!claimed_here && !claimed_earlier) {
            filtered.push_back(activation);
        }
    }
    return filtered;
}
} // namespace

PivotXrResolvedSettings ResolvePivotXrSettings(const ConfigDocument& config, std::string_view exe_name) {
    PivotXrResolvedSettings resolved;

    const RegisteredApplication* application = FindMatchingApplication(config, exe_name);
    if (application) {
        for (const PivotXrProfile& profile : config.pivotxr.profiles) {
            if (!profile.enabled) {
                continue;
            }
            if (std::find(profile.application_ids.begin(), profile.application_ids.end(), application->id) ==
                profile.application_ids.end()) {
                continue;
            }

            std::vector<PivotActivationBinding> activation_bindings =
                FilterUnclaimedActivationBindings(profile.activation_bindings, resolved.profiles);
            const bool all_bindings_shadowed = !profile.activation_bindings.empty() && activation_bindings.empty();
            if (all_bindings_shadowed && !profile.always_active) {
                continue;
            }

            PivotXrResolvedProfile candidate;
            candidate.name = profile.name;
            candidate.always_active = profile.always_active;
            candidate.activation_bindings = std::move(activation_bindings);
            candidate.set_origin_bindings = profile.set_origin_bindings;
            candidate.release_origin_bindings = profile.release_origin_bindings;
            ApplyPivotSettings(candidate, profile.settings);
            resolved.profiles.push_back(std::move(candidate));
        }
    }

    // Custom profiles replace the default profile entirely; the default only
    // participates when no custom profile targets this application.
    if (resolved.profiles.empty() && config.pivotxr.enabled) {
        PivotXrResolvedProfile candidate;
        candidate.name = "Default";
        candidate.always_active = config.pivotxr.always_active;
        candidate.activation_bindings =
            FilterUnclaimedActivationBindings(config.pivotxr.activation_bindings, {});
        candidate.set_origin_bindings = config.pivotxr.set_origin_bindings;
        candidate.release_origin_bindings = config.pivotxr.release_origin_bindings;
        ApplyPivotSettings(candidate, config.pivotxr.defaults);
        resolved.profiles.push_back(std::move(candidate));
    }

    resolved.enabled = !resolved.profiles.empty();
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

    const QuadViewsProfile* profile = FindMatchingQuadViewsProfile(config, exe_name);
    if (profile) {
        resolved.enabled = true;
        ApplyQuadViewsSettings(resolved, profile->settings);
    }

    return resolved;
}

TurboResolvedSettings ResolveTurboSettings(const ConfigDocument& config, std::string_view exe_name) {
    TurboResolvedSettings resolved;
    resolved.enabled = config.turbo.enabled;
    resolved.toggle_binding = config.turbo.toggle_binding;
    resolved.pacing_mode = config.turbo.pacing_mode;
    resolved.runtime_pins = config.turbo.runtime_pins;
    resolved.metrics_mode = config.turbo.metrics_mode;
    resolved.metrics_binding = config.turbo.metrics_binding;

    const RegisteredApplication* application = FindMatchingApplication(config, exe_name);
    if (application) {
        for (const TurboProfile& profile : config.turbo.profiles) {
            if (!profile.enabled) {
                continue;
            }
            if (std::find(profile.application_ids.begin(), profile.application_ids.end(), application->id) !=
                profile.application_ids.end()) {
                resolved.enabled = true;
                break;
            }
        }
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
    resolved.turbo = ResolveTurboSettings(config, exe_name);
    return resolved;
}

} // namespace depthxr
