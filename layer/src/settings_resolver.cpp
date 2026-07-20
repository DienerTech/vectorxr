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
    if (source.compatibility_mode.has_value()) {
        target.compatibility_mode = *source.compatibility_mode;
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
    resolved.step_trigger_degrees = settings.step_trigger_degrees;
    resolved.step_amount_degrees = settings.step_amount_degrees;
    resolved.step_hysteresis_degrees = settings.step_hysteresis_degrees;

    // Direction tunings collapse to the symmetric values unless advanced axes
    // are enabled, so the runtime always reads one shape.
    if (settings.advanced_axes) {
        resolved.yaw_positive = settings.yaw_left;
        resolved.yaw_negative = settings.yaw_right;
        resolved.pitch_positive = settings.pitch_up;
        resolved.pitch_negative = settings.pitch_down;
    } else {
        const PivotAxisTuning yaw{settings.yaw_rotation_multiplier, settings.yaw_deadzone_degrees,
                                  settings.yaw_max_extra_degrees};
        const PivotAxisTuning pitch{settings.pitch_rotation_multiplier, settings.pitch_deadzone_degrees,
                                    settings.pitch_max_extra_degrees};
        resolved.yaw_positive = yaw;
        resolved.yaw_negative = yaw;
        resolved.pitch_positive = pitch;
        resolved.pitch_negative = pitch;
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

            const bool binding_shadowed = std::any_of(
                resolved.profiles.begin(), resolved.profiles.end(), [&](const PivotXrResolvedProfile& earlier) {
                    return SameActivationInput(earlier.activation_binding, profile.activation_binding);
                });
            // A shadowed toggle/hold profile cannot be selected. Always-on
            // participation does not depend on its optional binding, however,
            // so retain that profile and shadow only its suspend/resume input.
            if (binding_shadowed && profile.activation_mode != ActivationMode::AlwaysOn) {
                continue;
            }

            PivotXrResolvedProfile candidate;
            candidate.name = profile.name;
            candidate.activation_mode = profile.activation_mode;
            candidate.activation_binding = binding_shadowed ? InputBinding{} : profile.activation_binding;
            candidate.set_origin_binding = profile.set_origin_binding;
            candidate.release_origin_binding = profile.release_origin_binding;
            ApplyPivotSettings(candidate, profile.settings);
            resolved.profiles.push_back(std::move(candidate));
        }
    }

    // Custom profiles replace the default profile entirely; the default only
    // participates when no custom profile targets this application.
    if (resolved.profiles.empty() && config.pivotxr.enabled) {
        PivotXrResolvedProfile candidate;
        candidate.name = "Default";
        candidate.activation_mode = config.pivotxr.activation_mode;
        candidate.activation_binding = config.pivotxr.activation_binding;
        candidate.set_origin_binding = config.pivotxr.set_origin_binding;
        candidate.release_origin_binding = config.pivotxr.release_origin_binding;
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
