#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#include "depthxr/config_parser.h"
#include "depthxr/effects.h"
#include "depthxr/logger.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/seen_apps.h"
#include "depthxr/settings_resolver.h"
#include "depthxr/turbo_metrics.h"

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestParseConfig() {
    const std::string json = R"json(
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "debug",
    "logRetentionFiles": 9,
    "trackSeenApps": false
  },
  "applications": [
    {
      "id": "game",
      "name": "Game",
      "enabled": true,
      "match": { "exe": "Game.exe" }
    }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.08
      },
      "bindings": {
        "toggleEnabled": {
          "type": "keyboard",
          "chord": ["F7"]
        }
      },
      "profiles": [
        {
          "name": "Game",
          "applicationIds": ["game"],
          "enabled": true,
          "settings": {
            "stereoBoost": 1.2
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.2,
        "pitchSmoothing": 0.15,
        "pitchDeadzoneDegrees": 10.0,
        "maxExtraPitchDegrees": 18.0
      },
      "profiles": [
        {
          "name": "Game",
          "applicationIds": ["game"],
          "enabled": true,
          "activationMode": "toggle",
          "activationBinding": {
            "type": "keyboard",
            "chord": ["F8"]
          },
          "settings": {
            "rotationMultiplier": 1.7,
            "smoothing": 0.2,
            "deadzoneDegrees": 8.0,
            "maxExtraYawDegrees": 30.0,
            "pitchRotationMultiplier": 1.2,
            "pitchSmoothing": 0.15,
            "pitchDeadzoneDegrees": 10.0,
            "maxExtraPitchDegrees": 18.0
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid v3 config: " + result.error);
    Expect(result.document.version == 3, "Version was not normalized to 3");
    Expect(result.document.core.log_level == depthxr::LogLevel::Debug, "Core log level mismatch");
    Expect(result.document.core.log_retention_files == 9, "Core log retention mismatch");
    Expect(!result.document.core.track_seen_apps, "Core trackSeenApps mismatch");
    Expect(result.document.applications.size() == 1, "Application registry count mismatch");
    Expect(result.document.applications[0].id == "game", "Application registry id mismatch");
    Expect(std::abs(result.document.depthxr.defaults.stereo_boost - 1.1) < 0.0001, "DepthXR stereoBoost mismatch");
    Expect(std::abs(result.document.depthxr.defaults.convergence - 0.08) < 0.0001, "DepthXR convergence mismatch");
    Expect(result.document.depthxr.bindings.toggle_enabled.chord[0] == "F7", "DepthXR toggle binding mismatch");
    Expect(result.document.depthxr.profiles.size() == 1, "DepthXR profile count mismatch");
    Expect(result.document.depthxr.profiles[0].application_ids[0] == "game", "DepthXR profile application id mismatch");
    Expect(result.document.pivotxr.profiles.size() == 1, "PivotXR profile count mismatch");
    Expect(result.document.pivotxr.profiles[0].activation_binding.chord[0] == "F8", "PivotXR profile activation binding mismatch");
    Expect(std::abs(result.document.pivotxr.profiles[0].settings.yaw_max_extra_degrees - 30.0) < 0.0001,
           "PivotXR profile yaw clamp mismatch");
    Expect(std::abs(result.document.pivotxr.profiles[0].settings.pitch_rotation_multiplier - 1.2) < 0.0001,
           "PivotXR profile pitch multiplier mismatch");
}

void TestLogLevelCompatibility() {
    const auto info = depthxr::ParseLogLevel("info");
    const auto debug = depthxr::ParseLogLevel("debug");
    const auto none = depthxr::ParseLogLevel("none");
    const auto off = depthxr::ParseLogLevel("off");
    const auto error = depthxr::ParseLogLevel("error");
    Expect(info.has_value() && *info == depthxr::LogLevel::Info, "info should enable normal logging");
    Expect(debug.has_value() && *debug == depthxr::LogLevel::Debug, "debug should enable verbose logging");
    Expect(none.has_value() && *none == depthxr::LogLevel::Info, "legacy none should normalize to info");
    Expect(off.has_value() && *off == depthxr::LogLevel::Info, "legacy off should normalize to info");
    Expect(error.has_value() && *error == depthxr::LogLevel::Info, "legacy error should normalize to info");
}

void TestResolveRuntimeConfig() {
    const std::string json = R"json(
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "applications": [
    {
      "id": "dcs",
      "name": "DCS",
      "enabled": true,
      "match": { "exe": "DCS.exe" }
    }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.05,
        "convergence": 0.0
      },
      "bindings": {
        "toggleEnabled": {
          "type": "none"
        }
      },
      "profiles": [
        {
          "name": "DCS",
          "applicationIds": ["dcs"],
          "enabled": true,
          "settings": {
            "stereoBoost": 1.15,
            "convergence": 0.12
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": true,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 22.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "activationMode": "hold",
      "activationBinding": { "type": "keyboard", "chord": ["F9"] },
      "profiles": [
        {
          "name": "DCS",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "hold",
          "activationBinding": {
            "type": "keyboard",
            "chord": ["Ctrl", "Space"]
          },
          "settings": {
            "rotationMultiplier": 1.7,
            "smoothing": 0.25,
            "deadzoneDegrees": 9.0,
            "maxExtraYawDegrees": 22.0,
            "pitchRotationMultiplier": 1.35,
            "pitchSmoothing": 0.3,
            "pitchDeadzoneDegrees": 14.0,
            "maxExtraPitchDegrees": 24.0
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid resolve test config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "dcs.exe");
    Expect(resolved.core.enabled, "Core enabled should be true");
    Expect(std::abs(resolved.depthxr.stereo_boost - 1.15) < 0.0001, "Profile stereoBoost override was not applied");
    Expect(std::abs(resolved.depthxr.convergence - 0.12) < 0.0001, "Profile convergence override was not applied");
    Expect(resolved.depthxr_bindings.toggle_enabled.type == depthxr::InputBindingType::None, "DepthXR toggle binding should be none");
    Expect(resolved.pivotxr.enabled, "PivotXR module enable was not resolved");
    Expect(resolved.pivotxr.profiles.size() == 1, "PivotXR resolved candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& pivot_profile = resolved.pivotxr.profiles[0];
    Expect(pivot_profile.activation_mode == depthxr::ActivationMode::Hold, "PivotXR activation mode mismatch");
    Expect(pivot_profile.activation_binding.chord.size() == 2, "PivotXR activation chord size mismatch");
    Expect(pivot_profile.activation_binding.chord[1] == "Space", "PivotXR activation binding was not resolved");
    Expect(std::abs(pivot_profile.yaw_max_extra_degrees - 22.0) < 0.0001, "PivotXR yaw clamp mismatch");
    Expect(std::abs(pivot_profile.pitch_rotation_multiplier - 1.35) < 0.0001,
           "PivotXR pitch multiplier mismatch");
}

void TestDisabledProfileFallsBackToDefaults() {
    const std::string json = R"json(
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "applications": [
    {
      "id": "dcs",
      "name": "DCS",
      "enabled": true,
      "match": { "exe": "DCS.exe" }
    }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.05,
        "convergence": 0.01
      },
      "bindings": {
        "toggleEnabled": {
          "type": "none"
        }
      },
      "profiles": [
        {
          "name": "DCS",
          "applicationIds": ["dcs"],
          "enabled": false,
          "settings": {
            "stereoBoost": 1.2,
            "convergence": 0.12
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": []
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected disabled-profile config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(std::abs(resolved.depthxr.stereo_boost - 1.05) < 0.0001, "Disabled profile should fall back to defaults");
    Expect(std::abs(resolved.depthxr.convergence - 0.01) < 0.0001, "Disabled profile convergence should fall back to defaults");
    Expect(!resolved.pivotxr.enabled, "PivotXR should be disabled");
}

void TestPivotProfileResolution() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.0,
        "convergence": 0.0
      },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": {
      "enabled": true,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": [
        {
          "name": "DCS",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "toggle",
          "activationBinding": { "type": "keyboard", "chord": ["F8"] },
          "settings": {
            "rotationMultiplier": 2.0,
            "smoothing": 0.3,
            "deadzoneDegrees": 10.0,
            "maxExtraYawDegrees": 60.0,
            "pitchRotationMultiplier": 1.5,
            "pitchSmoothing": 0.25,
            "pitchDeadzoneDegrees": 14.0,
            "maxExtraPitchDegrees": 45.0
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid pivot profile config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(resolved.pivotxr.enabled, "PivotXR should be enabled for DCS");
    Expect(resolved.pivotxr.profiles.size() == 1, "PivotXR resolved candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& dcs_profile = resolved.pivotxr.profiles[0];
    Expect(dcs_profile.activation_mode == depthxr::ActivationMode::Toggle, "PivotXR activation mode mismatch");
    Expect(dcs_profile.activation_binding.chord[0] == "F8", "PivotXR activation binding mismatch");
    Expect(std::abs(dcs_profile.yaw_rotation_multiplier - 2.0) < 0.0001, "PivotXR yaw multiplier mismatch");
    Expect(std::abs(dcs_profile.yaw_max_extra_degrees - 60.0) < 0.0001, "PivotXR yaw clamp mismatch");
    Expect(std::abs(dcs_profile.pitch_rotation_multiplier - 1.5) < 0.0001, "PivotXR pitch multiplier mismatch");

    // No profile for a different exe falls back to the default Pivot profile.
    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "other.exe");
    Expect(resolved_other.pivotxr.enabled, "PivotXR default profile should resolve for exe with no matching profile");
    Expect(resolved_other.pivotxr.profiles.size() == 1, "PivotXR default candidate count mismatch");
    Expect(resolved_other.pivotxr.profiles[0].activation_binding.type == depthxr::InputBindingType::None,
           "PivotXR default activation binding should fall back to none");
    Expect(std::abs(resolved_other.pivotxr.profiles[0].yaw_rotation_multiplier - 1.5) < 0.0001,
           "PivotXR default yaw multiplier mismatch");
}

void TestPivotMultiProfileResolution() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {},
      "profiles": [
        {
          "id": "dcs-mild",
          "name": "DCS Mild",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "toggle",
          "activationBinding": { "type": "keyboard", "chord": ["F8"] },
          "settings": { "rotationMultiplier": 1.5 }
        },
        {
          "id": "dcs-disabled",
          "name": "DCS Disabled",
          "applicationIds": ["dcs"],
          "enabled": false,
          "activationBinding": { "type": "keyboard", "chord": ["F10"] },
          "settings": { "rotationMultiplier": 3.0 }
        },
        {
          "id": "dcs-strong",
          "name": "DCS Strong",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "hold",
          "activationBinding": { "type": "keyboard", "chord": ["F9"] },
          "settings": { "rotationMultiplier": 2.5 }
        },
        {
          "name": "DCS Shadowed",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationBinding": { "type": "keyboard", "chord": ["F8"] },
          "settings": { "rotationMultiplier": 4.0 }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected multi-profile pivot config: " + result.error);
    Expect(result.document.pivotxr.profiles[0].id == "dcs-mild", "Pivot profile id was not parsed");

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(resolved.pivotxr.enabled, "PivotXR should be enabled when custom profiles match");
    // Disabled profile skipped; duplicate-F8 profile pruned as shadowed.
    Expect(resolved.pivotxr.profiles.size() == 2, "PivotXR multi-profile candidate count mismatch");
    Expect(resolved.pivotxr.profiles[0].name == "DCS Mild", "Candidate priority order mismatch (first)");
    Expect(resolved.pivotxr.profiles[1].name == "DCS Strong", "Candidate priority order mismatch (second)");
    Expect(std::abs(resolved.pivotxr.profiles[1].yaw_rotation_multiplier - 2.5) < 0.0001,
           "Second candidate settings mismatch");
    Expect(resolved.pivotxr.profiles[1].activation_mode == depthxr::ActivationMode::Hold,
           "Second candidate activation mode mismatch");
}

void TestPivotResponseModeAndAdvancedAxes() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } },
    { "id": "msfs", "name": "MSFS", "enabled": true, "match": { "exe": "MSFS.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {},
      "profiles": [
        {
          "name": "DCS Stepped",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "alwaysOn",
          "activationBinding": { "type": "keyboard", "chord": ["F8"] },
          "settings": {
            "responseMode": "stepped",
            "stepTriggerDegrees": 12.0,
            "stepAmountDegrees": 15.0,
            "stepHysteresisDegrees": 5.0,
            "deadzoneDegrees": 6.0
          }
        },
        {
          "name": "MSFS Advanced",
          "applicationIds": ["msfs"],
          "enabled": true,
          "activationBinding": { "type": "keyboard", "chord": ["F9"] },
          "settings": {
            "advancedAxes": true,
            "yawLeft": { "rotationMultiplier": 2.0, "deadzoneDegrees": 5.0, "maxExtraDegrees": 90.0 },
            "yawRight": { "rotationMultiplier": 1.2 },
            "pitchUp": { "rotationMultiplier": 1.8 },
            "pitchDown": { "rotationMultiplier": 1.1, "maxExtraDegrees": 30.0 }
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected response-mode config: " + result.error);

    const depthxr::ResolvedRuntimeConfig stepped = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(stepped.pivotxr.profiles.size() == 1, "Stepped candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& stepped_profile = stepped.pivotxr.profiles[0];
    Expect(stepped_profile.activation_mode == depthxr::ActivationMode::AlwaysOn,
           "alwaysOn activation mode was not parsed");
    Expect(stepped_profile.response_mode == depthxr::PivotResponseMode::Stepped,
           "Stepped response mode was not resolved");
    Expect(std::abs(stepped_profile.step_trigger_degrees - 12.0) < 0.0001, "Step trigger mismatch");
    Expect(std::abs(stepped_profile.step_amount_degrees - 15.0) < 0.0001, "Step amount mismatch");
    Expect(std::abs(stepped_profile.step_hysteresis_degrees - 5.0) < 0.0001, "Step hysteresis mismatch");

    const depthxr::ResolvedRuntimeConfig advanced = depthxr::ResolveRuntimeConfig(result.document, "MSFS.exe");
    Expect(advanced.pivotxr.profiles.size() == 1, "Advanced candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& advanced_profile = advanced.pivotxr.profiles[0];
    Expect(std::abs(advanced_profile.yaw_positive.rotation_multiplier - 2.0) < 0.0001,
           "Advanced yaw-left multiplier mismatch");
    Expect(std::abs(advanced_profile.yaw_positive.max_extra_degrees - 90.0) < 0.0001,
           "Advanced yaw-left max extra mismatch");
    Expect(std::abs(advanced_profile.yaw_negative.rotation_multiplier - 1.2) < 0.0001,
           "Advanced yaw-right multiplier mismatch");
    Expect(std::abs(advanced_profile.pitch_positive.rotation_multiplier - 1.8) < 0.0001,
           "Advanced pitch-up multiplier mismatch");
    Expect(std::abs(advanced_profile.pitch_negative.max_extra_degrees - 30.0) < 0.0001,
           "Advanced pitch-down max extra mismatch");

    // Symmetric profiles collapse direction tunings to the yaw/pitch values.
    Expect(std::abs(stepped_profile.yaw_positive.deadzone_degrees - 6.0) < 0.0001,
           "Symmetric collapse should mirror yaw deadzone into direction tunings");
}

void TestTurboModuleResolution() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "turbo": {
      "enabled": false,
      "toggleBinding": { "type": "keyboard", "chord": ["Ctrl", "F7"] },
      "profiles": [
        { "id": "turbo-dcs", "name": "DCS", "enabled": true, "applicationIds": ["dcs"] }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected turbo config: " + result.error);
    Expect(result.document.turbo.profiles.size() == 1, "Turbo profile count mismatch");
    Expect(result.document.turbo.profiles[0].id == "turbo-dcs", "Turbo profile id mismatch");

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(resolved.turbo.enabled, "Turbo should resolve enabled for a matched profile");
    Expect(resolved.turbo.toggle_binding.chord.size() == 2, "Turbo toggle binding chord mismatch");

    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "other.exe");
    Expect(!resolved_other.turbo.enabled, "Turbo default-off should not apply to unmatched applications");
}

void TestTurboPacingModeParsing() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "turbo": {
      "enabled": true,
      "pacingMode": "auto",
      "runtimePins": { "Oculus": "sequenced", "SteamVR/OpenXR": "async" },
      "toggleBinding": { "type": "none" },
      "profiles": []
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected turbo pacing config: " + result.error);
    Expect(result.document.turbo.pacing_mode == depthxr::TurboPacingSetting::kAuto,
           "Turbo pacing mode should parse auto");
    Expect(result.document.turbo.runtime_pins.size() == 2, "Turbo runtime pin count mismatch");

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "any.exe");
    Expect(resolved.turbo.pacing_mode == depthxr::TurboPacingSetting::kAuto,
           "Turbo pacing mode should resolve through");
    Expect(resolved.turbo.runtime_pins.size() == 2, "Turbo runtime pins should resolve through");
    bool found_oculus_pin = false;
    for (const auto& [runtime_name, pin] : resolved.turbo.runtime_pins) {
        if (runtime_name == "Oculus") {
            found_oculus_pin = pin == depthxr::TurboPacingMode::kSequenced;
        }
    }
    Expect(found_oculus_pin, "Oculus runtime pin should resolve to sequenced");

    // Invalid values are rejected rather than silently defaulted.
    std::string invalid = json;
    const std::string needle = "\"pacingMode\": \"auto\"";
    invalid.replace(invalid.find(needle), needle.size(), "\"pacingMode\": \"pipelined\"");
    const depthxr::ParseResult invalid_result = depthxr::ParseConfig(invalid);
    Expect(!invalid_result.ok, "Config parser should reject an unknown turbo pacing mode");

    std::string unsupported_pin = json;
    const std::string pin_needle = "\"Oculus\": \"sequenced\"";
    unsupported_pin.replace(unsupported_pin.find(pin_needle), pin_needle.size(),
                            "\"Oculus\": \"unsupported\"");
    const depthxr::ParseResult unsupported_result = depthxr::ParseConfig(unsupported_pin);
    Expect(!unsupported_result.ok, "Config parser should reject an 'unsupported' runtime pin");
}

void TestTurboModuleOptional() {
    // Configs written before turbo existed must still parse, with turbo off.
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected config without turbo module: " + result.error);
    Expect(!result.document.turbo.enabled, "Turbo should default to disabled when absent");
    Expect(result.document.turbo.pacing_mode == depthxr::TurboPacingSetting::kAuto,
           "Turbo pacing mode should default to auto when absent");
    Expect(result.document.turbo.runtime_pins.empty(), "Turbo runtime pins should default empty");
    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "any.exe");
    Expect(!resolved.turbo.enabled, "Turbo resolved settings should default to disabled");
}

void TestTurboMetricsSettingsParsing() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "turbo": {
      "enabled": true,
      "pacingMode": "auto",
      "metricsMode": "binding",
      "metricsBinding": { "type": "keyboard", "chord": ["ctrl", "f10"] },
      "toggleBinding": { "type": "none" },
      "profiles": []
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected turbo metrics config: " + result.error);
    Expect(result.document.turbo.metrics_mode == depthxr::TurboMetricsMode::kBinding,
           "Turbo metrics mode should parse binding");
    Expect(result.document.turbo.metrics_binding.type == depthxr::InputBindingType::Keyboard,
           "Turbo metrics binding should parse as keyboard");

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "any.exe");
    Expect(resolved.turbo.metrics_mode == depthxr::TurboMetricsMode::kBinding,
           "Turbo metrics mode should resolve through");
    Expect(resolved.turbo.metrics_binding.chord.size() == 2,
           "Turbo metrics binding chord should resolve through");

    // Absent keys default to always-on capture with no binding.
    std::string without_metrics = json;
    const std::string mode_needle = "\"metricsMode\": \"binding\",";
    without_metrics.replace(without_metrics.find(mode_needle), mode_needle.size(), "");
    const std::string binding_needle =
        "\"metricsBinding\": { \"type\": \"keyboard\", \"chord\": [\"ctrl\", \"f10\"] },";
    without_metrics.replace(without_metrics.find(binding_needle), binding_needle.size(), "");
    const depthxr::ParseResult default_result = depthxr::ParseConfig(without_metrics);
    Expect(default_result.ok, "Config parser rejected config without metrics keys: " + default_result.error);
    Expect(default_result.document.turbo.metrics_mode == depthxr::TurboMetricsMode::kAlways,
           "Turbo metrics mode should default to always");
    Expect(default_result.document.turbo.metrics_binding.type == depthxr::InputBindingType::None,
           "Turbo metrics binding should default to none");

    // Invalid values are rejected rather than silently defaulted.
    std::string invalid = json;
    const std::string needle = "\"metricsMode\": \"binding\"";
    invalid.replace(invalid.find(needle), needle.size(), "\"metricsMode\": \"sometimes\"");
    const depthxr::ParseResult invalid_result = depthxr::ParseConfig(invalid);
    Expect(!invalid_result.ok, "Config parser should reject an unknown turbo metrics mode");
}

void TestTurboMetricsSessionRoundTrip() {
    const std::filesystem::path test_directory =
        std::filesystem::current_path() / "build" / "vectorxr-test-turbo-metrics";
    std::error_code error;
    std::filesystem::remove_all(test_directory, error);
    error.clear();
    std::filesystem::create_directories(test_directory, error);
    Expect(!error, "Failed to create turbo-metrics test directory: " + error.message());

    const std::filesystem::path path = test_directory / "turbo-metrics.json";
    std::string record_error;

    depthxr::TurboMetricsSession session;
    session.session_id = "DCS.exe-1000";
    session.app_name = "DCS.exe";
    session.runtime_name = "PiOpenXR";
    session.layer_version = "0.12.1";
    session.collection_mode = "binding";
    session.live = true;
    session.started_unix_seconds = 1000;
    session.updated_unix_seconds = 1015;

    depthxr::TurboMetricsBucket off_bucket;
    off_bucket.state = "off";
    off_bucket.frames = 500;
    off_bucket.seconds = 7.25;
    off_bucket.avg_fps = 69.12;
    off_bucket.avg_frame_ms = 14.47;
    off_bucket.p99_frame_ms = 22.5;
    off_bucket.max_frame_ms = 41.03;
    off_bucket.avg_wait_block_ms = 6.81;
    session.buckets.push_back(off_bucket);

    depthxr::TurboMetricsBucket seq_bucket;
    seq_bucket.state = "sequenced";
    seq_bucket.frames = 900;
    seq_bucket.seconds = 10.0;
    seq_bucket.avg_fps = 90.0;
    seq_bucket.avg_frame_ms = 11.11;
    seq_bucket.p99_frame_ms = 15.5;
    seq_bucket.max_frame_ms = 30.2;
    seq_bucket.avg_wait_block_ms = 1.42;
    seq_bucket.fabricated_waits = 890;
    seq_bucket.drain_timeouts = 1;
    seq_bucket.discarded_frames = 3;
    session.buckets.push_back(seq_bucket);

    Expect(depthxr::RecordTurboMetricsSession(path, session, &record_error),
           "Failed to record turbo metrics session: " + record_error);

    // Update the same session (periodic flush) — upsert, not append.
    session.updated_unix_seconds = 1030;
    session.live = false;
    session.buckets[1].frames = 1800;
    Expect(depthxr::RecordTurboMetricsSession(path, session, &record_error),
           "Failed to upsert turbo metrics session: " + record_error);

    auto sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(sessions.size() == 1, "Turbo metrics session upsert should not duplicate");
    Expect(sessions[0].session_id == "DCS.exe-1000", "Turbo metrics session id mismatch");
    Expect(sessions[0].app_name == "DCS.exe", "Turbo metrics app name mismatch");
    Expect(sessions[0].runtime_name == "PiOpenXR", "Turbo metrics runtime name mismatch");
    Expect(sessions[0].collection_mode == "binding", "Turbo metrics collection mode mismatch");
    Expect(!sessions[0].live, "Turbo metrics live flag should update on upsert");
    Expect(sessions[0].updated_unix_seconds == 1030, "Turbo metrics updated timestamp mismatch");
    Expect(sessions[0].buckets.size() == 2, "Turbo metrics bucket count mismatch");
    Expect(sessions[0].buckets[0].state == "off", "Turbo metrics bucket order mismatch");
    Expect(sessions[0].buckets[0].frames == 500, "Turbo metrics off bucket frames mismatch");
    Expect(std::abs(sessions[0].buckets[0].avg_fps - 69.12) < 0.001,
           "Turbo metrics off bucket avgFps mismatch");
    Expect(std::abs(sessions[0].buckets[0].avg_wait_block_ms - 6.81) < 0.001,
           "Turbo metrics off bucket avgWaitBlockMs mismatch");
    Expect(sessions[0].buckets[1].frames == 1800, "Turbo metrics sequenced bucket frames mismatch");
    Expect(sessions[0].buckets[1].fabricated_waits == 890,
           "Turbo metrics sequenced bucket fabricatedWaits mismatch");
    Expect(sessions[0].buckets[1].drain_timeouts == 1,
           "Turbo metrics sequenced bucket drainTimeouts mismatch");
    Expect(sessions[0].buckets[1].discarded_frames == 3,
           "Turbo metrics sequenced bucket discardedFrames mismatch");
    Expect(std::abs(sessions[0].buckets[1].p99_frame_ms - 15.5) < 0.001,
           "Turbo metrics sequenced bucket p99FrameMs mismatch");

    // Retention: the newest sessions win, capped at kTurboMetricsRetainedSessions.
    for (int i = 0; i < 12; ++i) {
        depthxr::TurboMetricsSession extra;
        extra.session_id = "extra-" + std::to_string(i);
        extra.app_name = "other.exe";
        extra.collection_mode = "always";
        extra.started_unix_seconds = 2000 + i;
        extra.updated_unix_seconds = 2000 + i;
        depthxr::TurboMetricsBucket bucket;
        bucket.state = "async";
        bucket.frames = 10 + i;
        extra.buckets.push_back(bucket);
        Expect(depthxr::RecordTurboMetricsSession(path, extra, &record_error),
               "Failed to record extra turbo metrics session: " + record_error);
    }
    sessions = depthxr::ReadTurboMetricsSessions(path);
    Expect(sessions.size() == depthxr::kTurboMetricsRetainedSessions,
           "Turbo metrics retention cap not applied");
    Expect(sessions.front().session_id == "extra-11", "Turbo metrics sessions should be newest-first");
    for (const auto& retained : sessions) {
        Expect(retained.session_id != "DCS.exe-1000",
               "Oldest turbo metrics session should have been evicted");
    }

    std::filesystem::remove_all(test_directory, error);
}

void TestQuadViewsProfileResolution() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.0,
        "convergence": 0.0
      },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": []
    },
    "quadviews": {
      "enabled": true,
      "defaults": {
        "trackingMode": "eye",
        "focusHorizontalSizePercent": 32.0,
        "focusVerticalSizePercent": 32.0,
        "focusScale": 1.1,
        "peripheralScale": 0.45,
        "foveateSharpness": 0.0,
        "transitionThicknessPercent": 25.0,
        "horizontalOffsetDegrees": 0.0,
        "verticalOffsetDegrees": 0.0,
        "gazeSmoothing": 0.15,
        "gazeDeadzoneDegrees": 1.5
      },
      "profiles": [
        {
          "name": "DCS",
          "applicationIds": ["dcs"],
          "enabled": true,
          "settings": {
            "trackingMode": "eye",
            "focusHorizontalSizePercent": 34.0,
            "focusVerticalSizePercent": 30.0,
            "focusScale": 1.2,
            "peripheralScale": 0.4,
            "foveateSharpness": 60.0,
            "transitionThicknessPercent": 25.0,
            "horizontalOffsetDegrees": 1.5,
            "verticalOffsetDegrees": -2.0,
            "gazeSmoothing": 0.1,
            "gazeDeadzoneDegrees": 0.8
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid Quadviews config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(resolved.quadviews.enabled, "Quadviews module enable was not resolved");
    Expect(resolved.quadviews.tracking_mode == depthxr::QuadViewsTrackingMode::Eye,
           "Quadviews profile tracking mode mismatch");
    Expect(std::abs(resolved.quadviews.focus_horizontal_size_percent - 34.0) < 0.0001,
           "Quadviews focus horizontal size mismatch");
    Expect(std::abs(resolved.quadviews.peripheral_scale - 0.4) < 0.0001,
           "Quadviews peripheral scale mismatch");
    Expect(std::abs(resolved.quadviews.foveate_sharpness - 60.0) < 0.0001,
           "Quadviews foveate sharpness mismatch");
    Expect(std::abs(resolved.quadviews.transition_thickness_percent - 25.0) < 0.0001,
           "Quadviews transition thickness mismatch");
    Expect(std::abs(resolved.quadviews.vertical_offset_degrees + 2.0) < 0.0001,
           "Quadviews vertical offset mismatch");

    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "other.exe");
    Expect(resolved_other.quadviews.tracking_mode == depthxr::QuadViewsTrackingMode::Eye,
           "Quadviews default tracking mode mismatch");
    Expect(std::abs(resolved_other.quadviews.focus_horizontal_size_percent - 32.0) < 0.0001,
           "Quadviews default focus size mismatch");
}

void TestEnabledProfileOverridesDisabledDefault() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "msfs", "name": "MSFS", "enabled": true, "match": { "exe": "FlightSimulator2024.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.02
      },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": [
        {
          "name": "MSFS",
          "applicationIds": ["msfs"],
          "enabled": true,
          "settings": {
            "stereoBoost": 1.25,
            "convergence": 0.08
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": [
        {
          "name": "MSFS",
          "applicationIds": ["msfs"],
          "enabled": true,
          "activationMode": "hold",
          "activationBinding": { "type": "keyboard", "chord": ["F9"] },
          "settings": {
            "rotationMultiplier": 2.1,
            "smoothing": 0.1,
            "deadzoneDegrees": 4.0,
            "maxExtraYawDegrees": 45.0,
            "pitchRotationMultiplier": 1.3,
            "pitchSmoothing": 0.12,
            "pitchDeadzoneDegrees": 6.0,
            "maxExtraPitchDegrees": 25.0
          }
        }
      ]
    },
    "quadviews": {
      "enabled": false,
      "defaults": {
        "trackingMode": "eye",
        "focusHorizontalSizePercent": 32.0,
        "focusVerticalSizePercent": 32.0,
        "focusScale": 1.1,
        "peripheralScale": 0.45,
        "foveateSharpness": 0.0,
        "transitionThicknessPercent": 25.0,
        "horizontalOffsetDegrees": 0.0,
        "verticalOffsetDegrees": 0.0,
        "gazeSmoothing": 0.15,
        "gazeDeadzoneDegrees": 1.5
      },
      "profiles": [
        {
          "name": "MSFS",
          "applicationIds": ["msfs"],
          "enabled": true,
          "settings": {
            "trackingMode": "head",
            "focusHorizontalSizePercent": 40.0,
            "focusVerticalSizePercent": 36.0,
            "focusScale": 1.0,
            "peripheralScale": 0.5,
            "foveateSharpness": 25.0,
            "transitionThicknessPercent": 20.0,
            "horizontalOffsetDegrees": 1.0,
            "verticalOffsetDegrees": -1.0,
            "gazeSmoothing": 0.2,
            "gazeDeadzoneDegrees": 2.0
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected opt-in profile config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "FlightSimulator2024.exe");
    Expect(resolved.depthxr.enabled, "Enabled custom profile should turn DepthXR on when default is off");
    Expect(std::abs(resolved.depthxr.stereo_boost - 1.25) < 0.0001,
           "DepthXR custom profile should override disabled default");
    Expect(resolved.pivotxr.enabled, "Enabled custom profile should turn PivotXR on when default is off");
    Expect(resolved.pivotxr.profiles.size() == 1, "PivotXR opt-in candidate count mismatch");
    Expect(resolved.pivotxr.profiles[0].activation_mode == depthxr::ActivationMode::Hold,
           "PivotXR custom profile activation mode mismatch");
    Expect(std::abs(resolved.pivotxr.profiles[0].yaw_rotation_multiplier - 2.1) < 0.0001,
           "PivotXR custom profile should override disabled default");
    Expect(resolved.quadviews.enabled, "Enabled custom profile should turn Quadviews on when default is off");
    Expect(resolved.quadviews.tracking_mode == depthxr::QuadViewsTrackingMode::Head,
           "Quadviews custom profile should override disabled default");

    // Unmatched applications still follow the default profile enabled flag.
    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(!resolved_other.depthxr.enabled, "DepthXR default-off profile should not apply to unmatched applications");
    Expect(!resolved_other.pivotxr.enabled, "PivotXR default-off profile should not apply to unmatched applications");
    Expect(!resolved_other.quadviews.enabled, "Quadviews default-off profile should not apply to unmatched applications");

    // A disabled custom profile is inert: defaults apply again.
    depthxr::ConfigDocument inert = result.document;
    inert.quadviews.profiles[0].enabled = false;
    const depthxr::ResolvedRuntimeConfig resolved_inert = depthxr::ResolveRuntimeConfig(inert, "FlightSimulator2024.exe");
    Expect(!resolved_inert.quadviews.enabled, "Inactive custom profile should fall back to disabled default");
}

void TestInvalidPivotActivationBindingRejected() {
    const std::string json = R"json(
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "applications": [],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.05,
        "convergence": 0.0
      },
      "bindings": {
        "toggleEnabled": { "type": "none" }
      },
      "profiles": []
    },
    "pivotxr": {
      "enabled": true,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": [
        {
          "name": "Game",
          "applicationIds": [],
          "enabled": true,
          "activationBinding": {
            "type": "keyboard",
            "chord": ["Mouse4"]
          },
          "settings": {
            "rotationMultiplier": 1.5,
            "smoothing": 0.2,
            "deadzoneDegrees": 8.0,
            "maxExtraYawDegrees": 25.0,
            "pitchRotationMultiplier": 1.0,
            "pitchSmoothing": 0.2,
            "pitchDeadzoneDegrees": 12.0,
            "maxExtraPitchDegrees": 20.0
          }
        }
      ]
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(!result.ok, "Config parser accepted an unsupported PivotXR activation binding");
}

void TestNoneBindingParsed() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.0,
        "convergence": 0.0
      },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0,
        "maxExtraYawDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "maxExtraPitchDegrees": 20.0
      },
      "profiles": []
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid config with none binding: " + result.error);
    Expect(result.document.depthxr.bindings.toggle_enabled.type == depthxr::InputBindingType::None,
           "DepthXR toggle binding should be None");
}

void TestDeviceBindingMetadataParsed() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "bindings": {
        "toggleEnabled": {
          "type": "device",
          "deviceGuid": "{11111111-2222-3333-4444-555555555555}",
          "productGuid": "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}",
          "deviceName": "VKB Gladiator",
          "inputPath": "button-12",
          "inputLabel": "Button 12"
        }
      }
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid device binding metadata: " + result.error);
    const depthxr::InputBinding& binding = result.document.depthxr.bindings.toggle_enabled;
    Expect(binding.type == depthxr::InputBindingType::Device, "Device binding type mismatch");
    Expect(binding.device_guid == "{11111111-2222-3333-4444-555555555555}", "Device GUID metadata mismatch");
    Expect(binding.product_guid == "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}", "Product GUID metadata mismatch");
    Expect(binding.device_name == "VKB Gladiator", "Device name metadata mismatch");
    Expect(binding.input_path == "button-12", "Device input path mismatch");
    Expect(binding.input_label == "Button 12", "Device input label metadata mismatch");
}

void TestBindingSoundFeedbackParsed() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {
      "bindings": {
        "toggleEnabled": {
          "type": "keyboard",
          "chord": ["F8"],
          "sound": {
            "enabled": true,
            "activateSound": "C:\\sounds\\on.wav",
            "deactivateSound": ""
          }
        }
      }
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid binding sound feedback: " + result.error);
    const depthxr::SoundFeedback& sound = result.document.depthxr.bindings.toggle_enabled.sound;
    Expect(sound.enabled, "Binding sound feedback should be enabled");
    Expect(sound.activate_sound == "C:\\sounds\\on.wav", "Activate sound path mismatch");
    Expect(sound.deactivate_sound.empty(), "Deactivate sound should default to bundled (empty)");
}

void TestBindingWithoutSoundDefaultsDisabled() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": { "bindings": { "toggleEnabled": { "type": "keyboard", "chord": ["F8"] } } },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected binding without sound: " + result.error);
    Expect(!result.document.depthxr.bindings.toggle_enabled.sound.enabled,
           "Binding without sound block should default to disabled feedback");
}

void TestCoreSoundVolumeParsedAndClamped() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7, "sound": { "volume": 150 } },
  "applications": [],
  "modules": {
    "depthxr": { "bindings": { "toggleEnabled": { "type": "none" } } },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected core sound volume: " + result.error);
    Expect(result.document.core.sound_volume == 100, "Core sound volume should clamp to 100");
}

void TestExeMatch() {
    Expect(depthxr::ExeNameMatches("DCS.exe", "dcs.exe"), "Case-insensitive exe match failed");
    Expect(depthxr::ExeNameMatches("C:\\Games\\DCS.exe", "dcs.exe"), "Basename exe match failed");
}

void TestSeenAppObservationRecording() {
    const std::filesystem::path test_directory =
        std::filesystem::current_path() / "build" / "vectorxr-test-seen-app-observations";
    std::error_code error;
    std::filesystem::remove_all(test_directory, error);
    error.clear();
    std::filesystem::create_directories(test_directory, error);
    Expect(!error, "Failed to create seen-apps test directory: " + error.message());

    const std::filesystem::path path = test_directory / "seen-apps.json";
    std::string record_error;
    Expect(depthxr::RecordSeenAppObservation(path, "DCS.exe", 100, &record_error),
           "Failed to record first seen app observation: " + record_error);
    Expect(depthxr::RecordSeenAppObservation(path, "c:\\Games\\dcs.exe", 140, &record_error),
           "Failed to merge second seen app observation: " + record_error);

    std::ifstream stream(path);
    Expect(stream.good(), "Failed to open seen-apps test output");

    std::ostringstream contents;
    contents << stream.rdbuf();
    const std::string text = contents.str();
    Expect(text.find("\"exe\": \"DCS.exe\"") != std::string::npos, "Seen app display exe was not preserved");
    Expect(text.find("\"firstSeenUnixSeconds\": 100") != std::string::npos, "Seen app first-seen timestamp mismatch");
    Expect(text.find("\"lastSeenUnixSeconds\": 140") != std::string::npos, "Seen app last-seen timestamp mismatch");
    Expect(text.find("\"launchCount\": 2") != std::string::npos, "Seen app launch count did not merge");

    std::filesystem::remove_all(test_directory, error);
}

void TestRuntimePacingObservationRoundTrip() {
    const std::filesystem::path test_directory =
        std::filesystem::current_path() / "build" / "vectorxr-test-runtime-pacing";
    std::error_code error;
    std::filesystem::remove_all(test_directory, error);
    error.clear();
    std::filesystem::create_directories(test_directory, error);
    Expect(!error, "Failed to create runtime-pacing test directory: " + error.message());

    const std::filesystem::path path = test_directory / "runtime-pacing.json";
    std::string record_error;

    depthxr::RuntimePacingObservation first;
    first.runtime_name = "Oculus";
    first.runtime_version = "1.205.0";
    first.mode = depthxr::TurboPacingMode::kSequenced;
    first.source = "discovered";
    first.layer_version = "0.13.0";
    first.last_used_unix_seconds = 100;
    first.probe_timeouts = 3;
    first.stable_seconds = 60;
    Expect(depthxr::RecordRuntimePacingObservation(path, first, &record_error),
           "Failed to record first runtime pacing observation: " + record_error);

    depthxr::RuntimePacingObservation second;
    second.runtime_name = "SteamVR/OpenXR";
    second.mode = depthxr::TurboPacingMode::kAsync;
    second.source = "preset";
    second.last_used_unix_seconds = 200;
    Expect(depthxr::RecordRuntimePacingObservation(path, second, &record_error),
           "Failed to record second runtime pacing observation: " + record_error);

    // Upsert: a later verdict for the same runtime replaces the mode but
    // preserves first-used.
    first.mode = depthxr::TurboPacingMode::kAsync;
    first.last_used_unix_seconds = 300;
    Expect(depthxr::RecordRuntimePacingObservation(path, first, &record_error),
           "Failed to upsert runtime pacing observation: " + record_error);

    const auto observations = depthxr::ReadRuntimePacingObservations(path);
    Expect(observations.size() == 2, "Runtime pacing observation count mismatch");

    const auto oculus = depthxr::FindRuntimePacingObservation(path, "Oculus");
    Expect(oculus.has_value(), "Oculus runtime pacing observation missing");
    Expect(oculus->mode == depthxr::TurboPacingMode::kAsync, "Upserted mode was not applied");
    Expect(oculus->first_used_unix_seconds == 100, "First-used timestamp was not preserved on upsert");
    Expect(oculus->last_used_unix_seconds == 300, "Last-used timestamp was not updated on upsert");
    Expect(oculus->runtime_version == "1.205.0", "Runtime version mismatch");
    Expect(oculus->source == "discovered", "Source mismatch");

    const auto steam = depthxr::FindRuntimePacingObservation(path, "SteamVR/OpenXR");
    Expect(steam.has_value(), "SteamVR runtime pacing observation missing");
    Expect(steam->mode == depthxr::TurboPacingMode::kAsync, "SteamVR mode mismatch");

    Expect(!depthxr::FindRuntimePacingObservation(path, "Varjo").has_value(),
           "Unknown runtime should have no observation");

    std::filesystem::remove_all(test_directory, error);
}

void TestQuadViewStereoBoostKeepsInsetViewsInSync() {
    depthxr::ViewAdjustmentData views[4] = {
        {{-0.03, 0.0, 0.01}, {-1.0, 0.8, 0.9, -0.9}},
        {{0.03, 0.0, -0.01}, {-0.8, 1.0, 0.9, -0.9}},
        {{-0.03, 0.0, 0.01}, {-0.4, 0.2, 0.3, -0.3}},
        {{0.03, 0.0, -0.01}, {-0.2, 0.4, 0.3, -0.3}},
    };

    depthxr::ApplyStereoBoost(views, 1.4, depthxr::ViewLayout::kStereoWithFoveatedInset);

    Expect(std::abs(views[0].position.x - views[2].position.x) < 0.0001,
           "Left outer/inset positions diverged under quad-view stereo boost");
    Expect(std::abs(views[1].position.x - views[3].position.x) < 0.0001,
           "Right outer/inset positions diverged under quad-view stereo boost");
    Expect(std::abs(views[0].position.z - views[2].position.z) < 0.0001,
           "Left outer/inset depth positions diverged under quad-view stereo boost");
    Expect(std::abs(views[1].position.z - views[3].position.z) < 0.0001,
           "Right outer/inset depth positions diverged under quad-view stereo boost");
    Expect(std::abs(views[0].position.x + 0.042) < 0.0001, "Left eye stereo boost value mismatch");
    Expect(std::abs(views[1].position.x - 0.042) < 0.0001, "Right eye stereo boost value mismatch");
    Expect(std::abs(views[0].position.z - 0.014) < 0.0001, "Left eye stereo boost depth-axis value mismatch");
    Expect(std::abs(views[1].position.z + 0.014) < 0.0001, "Right eye stereo boost depth-axis value mismatch");
}

void TestStereoBoostScalesRotatedEyeBaseline() {
    depthxr::ViewAdjustmentData views[2] = {
        {{-0.02, 0.0, 0.02}, {-1.0, 0.8, 0.9, -0.9}},
        {{0.02, 0.0, -0.02}, {-0.8, 1.0, 0.9, -0.9}},
    };

    depthxr::ApplyStereoBoost(views, 1.5, depthxr::ViewLayout::kStereo);

    Expect(std::abs(views[0].position.x + 0.03) < 0.0001, "Left eye rotated baseline x mismatch");
    Expect(std::abs(views[0].position.z - 0.03) < 0.0001, "Left eye rotated baseline z mismatch");
    Expect(std::abs(views[1].position.x - 0.03) < 0.0001, "Right eye rotated baseline x mismatch");
    Expect(std::abs(views[1].position.z + 0.03) < 0.0001, "Right eye rotated baseline z mismatch");
}

void TestQuadViewConvergenceKeepsInsetOffsetsAligned() {
    depthxr::ViewAdjustmentData views[4] = {
        {{-0.03, 0.0, 0.0}, {-1.0, 0.8, 0.9, -0.9}},
        {{0.03, 0.0, 0.0}, {-0.8, 1.0, 0.9, -0.9}},
        {{-0.03, 0.0, 0.0}, {-0.4, 0.2, 0.3, -0.3}},
        {{0.03, 0.0, 0.0}, {-0.2, 0.4, 0.3, -0.3}},
    };

    const double original_left_outer_span =
        std::tan(views[0].fov.angle_right) - std::tan(views[0].fov.angle_left);
    const double original_left_inset_span =
        std::tan(views[2].fov.angle_right) - std::tan(views[2].fov.angle_left);

    depthxr::ApplyConvergence(views, 0.07, depthxr::ViewLayout::kStereoWithFoveatedInset);

    Expect(std::abs((std::tan(views[0].fov.angle_left) - std::tan(views[2].fov.angle_left)) -
                        ((std::tan(-1.0)) - std::tan(-0.4))) < 0.0001,
           "Left outer/inset horizontal relationship changed under convergence");
    Expect(std::abs((std::tan(views[1].fov.angle_right) - std::tan(views[3].fov.angle_right)) -
                        (std::tan(1.0) - std::tan(0.4))) < 0.0001,
           "Right outer/inset horizontal relationship changed under convergence");
    Expect(std::abs((std::tan(views[0].fov.angle_right) - std::tan(views[0].fov.angle_left)) -
                    original_left_outer_span) < 0.0001,
           "Outer horizontal FoV span changed under convergence");
    Expect(std::abs((std::tan(views[2].fov.angle_right) - std::tan(views[2].fov.angle_left)) -
                    original_left_inset_span) < 0.0001,
           "Inset horizontal FoV span changed under convergence");
}

double ExtractYaw(const depthxr::ViewOrientation& orientation) {
    return std::atan2(
        2.0 * (orientation.w * orientation.y + orientation.x * orientation.z),
        1.0 - 2.0 * (orientation.y * orientation.y + orientation.x * orientation.x));
}

depthxr::ViewOrientation YawOrientation(double yaw_radians) {
    return {0.0, std::sin(yaw_radians * 0.5), 0.0, std::cos(yaw_radians * 0.5)};
}

void TestPivotYawAmplifiesBeyondDeadzone() {
    depthxr::ViewAdjustmentData views[2] = {
        {{-0.03, 0.0, 0.0}, {-1.0, 0.8, 0.9, -0.9}, YawOrientation(30.0 * 3.14159265358979323846 / 180.0)},
        {{0.03, 0.0, 0.0}, {-0.8, 1.0, 0.9, -0.9}, YawOrientation(30.0 * 3.14159265358979323846 / 180.0)},
    };

    double smoothed_extra_yaw = 0.0;
    depthxr::ApplyPivotYaw(views, 1.5, 10.0, 0.0, depthxr::ViewLayout::kStereo, smoothed_extra_yaw);

    const double left_yaw = ExtractYaw(views[0].orientation) * 180.0 / 3.14159265358979323846;
    const double right_yaw = ExtractYaw(views[1].orientation) * 180.0 / 3.14159265358979323846;

    Expect(std::abs(left_yaw - 40.0) < 0.2, "PivotXR yaw amplification did not reach the expected left-eye yaw");
    Expect(std::abs(right_yaw - 40.0) < 0.2, "PivotXR yaw amplification did not reach the expected right-eye yaw");
    Expect(std::abs(views[0].position.x + 0.03) < 0.0001, "PivotXR should keep the left-eye position unchanged");
    Expect(std::abs(views[1].position.x - 0.03) < 0.0001, "PivotXR should keep the right-eye position unchanged");
    Expect(smoothed_extra_yaw > 0.0, "PivotXR should accumulate positive extra yaw beyond the deadzone");
}

void TestPivotYawNoOpInsideDeadzone() {
    depthxr::ViewAdjustmentData views[2] = {
        {{-0.03, 0.0, 0.0}, {-1.0, 0.8, 0.9, -0.9}, YawOrientation(5.0 * 3.14159265358979323846 / 180.0)},
        {{0.03, 0.0, 0.0}, {-0.8, 1.0, 0.9, -0.9}, YawOrientation(5.0 * 3.14159265358979323846 / 180.0)},
    };

    double smoothed_extra_yaw = 0.0;
    depthxr::ApplyPivotYaw(views, 1.7, 10.0, 0.0, depthxr::ViewLayout::kStereo, smoothed_extra_yaw);

    const double left_yaw = ExtractYaw(views[0].orientation) * 180.0 / 3.14159265358979323846;
    Expect(std::abs(left_yaw - 5.0) < 0.2, "PivotXR should leave yaw unchanged inside the deadzone");
    Expect(std::abs(smoothed_extra_yaw) < 0.0001, "PivotXR should not accumulate extra yaw inside the deadzone");
}

void TestLoggerCollapsesDuplicateMessages() {
    const std::filesystem::path test_directory =
        std::filesystem::current_path() / "build" / "vectorxr-test-logger-duplicate-collapse";
    std::error_code error;
    std::filesystem::remove_all(test_directory, error);
    error.clear();
    std::filesystem::create_directories(test_directory, error);
    Expect(!error, "Failed to create logger test directory: " + error.message());

    const std::filesystem::path log_base_path = test_directory / "vectorxr-test.log";
    std::filesystem::path active_log_path;
    {
        depthxr::Logger logger;
        logger.Initialize(log_base_path);
        logger.SetLevel(depthxr::LogLevel::Debug);
        active_log_path = logger.ActiveLogPath();
        logger.Error("Repeated failure");
        logger.Error("Repeated failure");
        logger.Error("Repeated failure");
        logger.Info("Different message");
    }

    std::ifstream stream(active_log_path);
    Expect(stream.good(), "Failed to open logger test output");

    std::ostringstream contents;
    contents << stream.rdbuf();
    const std::string text = contents.str();
    Expect(text.find("Repeated failure") != std::string::npos, "Logger omitted the original repeated message");
    Expect(text.find("Previous error message repeated 2 additional times") != std::string::npos,
           "Logger did not summarize repeated duplicate errors");
    Expect(text.find("Different message") != std::string::npos,
           "Logger did not continue writing after collapsing duplicates");

    std::filesystem::remove_all(test_directory, error);
}

} // namespace

int main() {
    TestParseConfig();
    TestLogLevelCompatibility();
    TestResolveRuntimeConfig();
    TestDisabledProfileFallsBackToDefaults();
    TestPivotProfileResolution();
    TestPivotMultiProfileResolution();
    TestPivotResponseModeAndAdvancedAxes();
    TestTurboModuleResolution();
    TestTurboPacingModeParsing();
    TestTurboModuleOptional();
    TestTurboMetricsSettingsParsing();
    TestTurboMetricsSessionRoundTrip();
    TestQuadViewsProfileResolution();
    TestEnabledProfileOverridesDisabledDefault();
    TestInvalidPivotActivationBindingRejected();
    TestNoneBindingParsed();
    TestDeviceBindingMetadataParsed();
    TestBindingSoundFeedbackParsed();
    TestBindingWithoutSoundDefaultsDisabled();
    TestCoreSoundVolumeParsedAndClamped();
    TestExeMatch();
    TestSeenAppObservationRecording();
    TestRuntimePacingObservationRoundTrip();
    TestQuadViewStereoBoostKeepsInsetViewsInSync();
    TestStereoBoostScalesRotatedEyeBaseline();
    TestQuadViewConvergenceKeepsInsetOffsetsAligned();
    TestPivotYawAmplifiesBeyondDeadzone();
    TestPivotYawNoOpInsideDeadzone();
    TestLoggerCollapsesDuplicateMessages();
    std::cout << "depthxr_layer_tests passed\n";
    return 0;
}
