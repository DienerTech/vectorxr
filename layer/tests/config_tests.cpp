#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>

#include "depthxr/config_parser.h"
#include "depthxr/effects.h"
#include "depthxr/framegen_plan.h"
#include "depthxr/input_devices.h"
#include "depthxr/logger.h"
#include "depthxr/pivot_routing.h"
#include "depthxr/pivot_step.h"
#include "depthxr/pivot_view.h"
#include "depthxr/quadviews_compose.h"
#include "depthxr/quadviews_frame_cache.h"
#include "depthxr/quadviews_recovery.h"
#include "depthxr/quadviews_sizing.h"
#include "depthxr/runtime_pacing.h"
#include "depthxr/runtime_relay.h"
#include "depthxr/runtime_compatibility.h"
#include "depthxr/seen_apps.h"
#include "depthxr/session_graphics.h"
#include "depthxr/settings_resolver.h"
#include "depthxr/swapchain_state.h"
#include "depthxr/turbo_metrics.h"
#include "depthxr/turbo_trace.h"

#ifdef _WIN32
#include <d3dcompiler.h>
#endif

namespace {

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

// A toggle's first binding (an unbound list's: a None binding).
const depthxr::InputBinding& FirstBinding(const std::vector<depthxr::InputBinding>& bindings) {
    static const depthxr::InputBinding none;
    return bindings.empty() ? none : bindings.front();
}

bool FirstChordKey(const std::vector<depthxr::InputBinding>& bindings, const std::string& key) {
    const depthxr::InputBinding& binding = FirstBinding(bindings);
    return !binding.chord.empty() && binding.chord.front() == key;
}

void TestOsdConfig() {
    const auto parse=[](const std::string& osd) {
        return depthxr::ParseConfig(R"({"version":3,"core":{"enabled":true,"logLevel":"info","osd":)"+osd+R"(},"applications":[],"modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},"bindings":{"toggleEnabled":{"type":"none"}},"profiles":[]},"pivotxr":{"enabled":false,"defaults":{},"profiles":[]}}})");
    };
    const auto defaults=parse("{}");
    Expect(defaults.ok && !defaults.document.core.osd.enabled && defaults.document.core.osd.update_hz==5,
           "OSD default compatibility failed: "+defaults.error);
    const auto configured=parse(R"({"enabled":true,"visibleOnStart":false,"compact":true,"horizontalDegrees":-40,
      "verticalDegrees":35,"distanceMeters":0.5,"scale":150,"opacity":30,"updateHz":20,"showGraph":false,
      "showRuntime":false,"showTurbo":false,"showModules":false,"showClock":false,"accent":"blue",
      "toggleBinding":{"type":"keyboard","chord":["Ctrl","F10"]},"cycleBinding":{"type":"none"}})");
    Expect(configured.ok,"Valid OSD rejected: "+configured.error);
    const auto custom=parse(R"({"scale":25,"showPivot":true,"clockFormat":"12","accent":"custom","customColor":"#aB12Ef","bodyOrder":["pivot","turbo","graph","modules"],"customPresets":[{"name":"Small cockpit","settings":{"scale":25,"accent":"rose","showPivot":false}}]})");
    Expect(custom.ok && custom.document.core.osd.scale==25 && custom.document.core.osd.clock_format=="12" &&
           custom.document.core.osd.custom_color=="#aB12Ef" && custom.document.core.osd.body_order.front()=="pivot","Custom OSD configuration rejected: "+custom.error);
    for (const auto& bad:{R"({"customColor":"#zzzzzz"})",R"({"bodyOrder":["graph","graph"]})",R"({"clockFormat":"13"})",R"({"customPresets":[{"name":"Bad","settings":{"customPresets":[]}}]})"})
        Expect(!parse(bad).ok,"Malformed OSD layout accepted");
    const auto& osd=configured.document.core.osd;
    Expect(osd.compact_show_runtime && osd.compact_show_clock && !osd.compact_show_turbo && !osd.compact_show_pivot,
           "Independent compact defaults were not preserved");
    const auto compact=parse(R"({"showRuntime":false,"showClock":false,"showTurbo":false,"showPivot":false,"compactShowRuntime":true,"compactShowClock":true,"compactShowTurbo":true,"compactShowPivot":true})");
    Expect(compact.ok && compact.document.core.osd.compact_show_runtime && compact.document.core.osd.compact_show_clock &&
           compact.document.core.osd.compact_show_turbo && compact.document.core.osd.compact_show_pivot &&
           !compact.document.core.osd.show_turbo && !compact.document.core.osd.show_pivot,"Independent compact settings were dropped");
    for (const auto* key:{"compactShowRuntime","compactShowClock","compactShowTurbo","compactShowPivot"})
        Expect(!parse(std::string("{\"")+key+"\":1}").ok,"Invalid compact flag accepted");
    const auto micro=parse(R"({"compactMetrics":"fps","compactShowBrand":false,"compactShowApp":false,"compactShowClock":false,"compactShowRuntime":false})");
    Expect(micro.ok && micro.document.core.osd.compact_metrics=="fps" && !micro.document.core.osd.compact_show_brand && !micro.document.core.osd.compact_show_app,"Micro settings were dropped");
    Expect(!parse(R"({"compactMetrics":"bad"})").ok,"Unknown compact metrics accepted");
    const auto no_header=parse(R"({"showBrand":false,"showApp":false,"showRuntime":false,"showClock":false})");
    Expect(no_header.ok && !no_header.document.core.osd.show_brand && !no_header.document.core.osd.show_app &&
           !no_header.document.core.osd.show_runtime && !no_header.document.core.osd.show_clock,"Expanded header choices were dropped");
    Expect(!parse(R"({"showBrand":1})").ok && !parse(R"({"showApp":"false"})").ok,"Invalid expanded header flags accepted");
    Expect(osd.enabled && !osd.visible_on_start && osd.compact && osd.horizontal_degrees==-40 &&
           osd.vertical_degrees==35 && osd.distance_meters==.5 && osd.scale==150 && osd.opacity==30 &&
           osd.update_hz==20 && !osd.show_graph && !osd.show_runtime && !osd.show_turbo && !osd.show_modules &&
           !osd.show_clock && osd.accent=="blue" && osd.toggle_bindings.size()==1 &&
           osd.toggle_bindings.front().chord.size()==2 && osd.cycle_bindings.empty(),"OSD fields were dropped");
    for(const std::string& fields:{R"("horizontalDegrees":41)",R"("verticalDegrees":-36)",R"("distanceMeters":0)",
      R"("scale":24)",R"("opacity":101)",R"("updateHz":0)",R"("updateHz":5.5)",R"("enabled":"true")",
      R"("showClock":1)",R"("accent":"red")"})
        Expect(!parse("{"+fields+"}").ok,"Invalid OSD accepted: "+fields);
    // An unknown key is ignored with a warning; the known values still apply.
    const auto unknown=parse(R"({"scale":40,"unknown":true})");
    Expect(unknown.ok && unknown.document.core.osd.scale==40 &&
           unknown.warnings==std::vector<std::string>{"Unknown field ignored: core.osd.unknown"},
           "Unknown OSD key was not ignored with a warning: "+unknown.error);
    Expect(!parse(R"({"scale":24,"unknown":true})").ok,"Out-of-range OSD value accepted beside an unknown key");
    const auto preset=parse(R"({"customPresets":[{"name":"Small cockpit","note":"x","settings":{"scale":25,"later":1}}]})");
    Expect(preset.ok && preset.warnings==std::vector<std::string>{
               "Unknown field ignored: core.osd.customPresets[0].note",
               "Unknown field ignored: core.osd.customPresets[0].settings.later"},
           "Unknown OSD preset keys were not reported by their path: "+preset.error);
}

void TestTurboExperimentalConfig() {
    const auto parse = [](const std::string& turbo) {
        return depthxr::ParseConfig(R"({"version":3,"core":{"enabled":true,"logLevel":"info","logRetentionFiles":7},
          "applications":[{"id":"dcs","name":"DCS","enabled":true,"match":{"exe":"DCS.exe"}}],
          "modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},"bindings":{"toggleEnabled":{"type":"none"}},"profiles":[]},
          "pivotxr":{"enabled":false,"defaults":{},"profiles":[]},"turbo":)" + turbo + "}}");
    };
    const auto legacy = parse(R"({"enabled":true})");
    Expect(legacy.ok && !legacy.document.turbo.experimental.enabled &&
        legacy.document.turbo.experimental.prediction_percent == 100, "Legacy Turbo timing must remain neutral: " + legacy.error);
    const auto configured = parse(R"({"enabled":true,"experimental":{"enabled":true,"applicationIds":["dcs"],
      "waitForSubmit":true,"sampleAtEntry":true,"predictionPercent":75,"frameLimit":45,"timingTrace":true}})");
    Expect(configured.ok, "Valid experimental config rejected: " + configured.error);
    const auto dcs = depthxr::ResolveTurboSettings(configured.document, "DCS.exe");
    const auto other = depthxr::ResolveTurboSettings(configured.document, "Other.exe");
    Expect(dcs.experimental.enabled && dcs.experimental.wait_for_submit &&
           dcs.experimental.sample_at_entry && dcs.experimental.frame_limit == 45 &&
           dcs.experimental.prediction_percent == 75, "Selected application's experimental settings were lost");
    Expect(!other.experimental.enabled, "Experiments escaped the application allowlist");
    const auto turbo_off = parse(R"({"enabled":false,"experimental":{"enabled":true,"applicationIds":["dcs"]}})");
    Expect(turbo_off.ok && !depthxr::ResolveTurboSettings(turbo_off.document, "DCS.exe").experimental.enabled,
           "Legacy module timing must not apply where Turbo is off for the application");
    auto disabled = configured.document;
    disabled.applications[0].enabled = false;
    Expect(!depthxr::ResolveTurboSettings(disabled, "DCS.exe").experimental.enabled,
           "Disabled registered application enabled experiments");
    auto profiles=parse(R"({"enabled":false,"profiles":[
      {"id":"first","name":"Toolkit","enabled":true,"applicationIds":["dcs"],"experimental":{"enabled":true,"waitForSubmit":true,"sampleAtEntry":true}},
      {"id":"second","name":"Custom","enabled":true,"applicationIds":["dcs"],"experimental":{"enabled":true,"predictionPercent":80,"frameLimit":45}}]})");
    Expect(profiles.ok,"Profile timing parse failed: "+profiles.error);
    auto selected=depthxr::ResolveTurboSettings(profiles.document,"DCS.exe");
    Expect(selected.enabled && selected.experimental.enabled && selected.experimental.wait_for_submit && selected.profile_name=="Toolkit", "First profile timing not selected");
    profiles.document.turbo.profiles[0].enabled=false;
    selected=depthxr::ResolveTurboSettings(profiles.document,"DCS.exe");
    Expect(selected.experimental.prediction_percent==80 && selected.experimental.frame_limit==45 && selected.profile_name=="Custom", "Independent profile settings lost");
    profiles.document.turbo.profiles[1].experimental=depthxr::TurboExperimentalSettings{};
    Expect(!depthxr::ResolveTurboSettings(profiles.document,"DCS.exe").experimental.enabled,"Normal profile did not disable experiments");
    Expect(!parse(R"({"profiles":[{"name":"Bad","applicationIds":["dcs"],"experimental":{"predictionPercent":200}}]})").ok,"Bad profile timing accepted");
    for (const std::string& fields : {R"("predictionPercent":49)", R"("predictionPercent":101)",
         R"("predictionPercent":75.5)", R"("frameLimit":1)", R"("frameLimit":241)", R"("frameLimit":45.5)",
         R"("waitForSubmit":"yes")"}) {
        const auto invalid = parse("{\"experimental\":{" + fields + "}}");
        Expect(!invalid.ok, "Invalid experimental settings accepted: " + fields);
    }
    // An unknown key is ignored with a warning; the known values still apply.
    const auto unknown = parse(R"({"experimental":{"predictionPercent":80,"unknown":true}})");
    Expect(unknown.ok && unknown.document.turbo.experimental.prediction_percent == 80 &&
               unknown.warnings == std::vector<std::string>{"Unknown field ignored: modules.turbo.experimental.unknown"},
           "Unknown experimental Turbo key was not ignored with a warning: " + unknown.error);
    Expect(!parse(R"({"experimental":{"predictionPercent":49,"unknown":true}})").ok,
           "Out-of-range experimental Turbo value accepted beside an unknown key");
}

// A key this layer does not know (a newer app's setting) is ignored and
// reported by its JSON path; every other error stays fatal.
void TestUnknownKeysAreIgnoredWithWarnings() {
    const depthxr::ParseResult parsed = depthxr::ParseConfig(R"json(
{
  "version": 3,
  "telemetry": true,
  "core": { "enabled": true, "logLevel": "info", "sound": { "volume": 40, "chime": "soft" } },
  "applications": [
    { "id": "app-dcs", "name": "DCS", "enabled": true, "icon": "dcs.png", "match": { "exe": "DCS.exe", "path": "C:/DCS" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": { "stereoBoost": 1.2, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "keyboard", "chord": ["Ctrl", "F9"], "repeat": true } },
      "profiles": [ { "name": "DCS", "applicationIds": ["app-dcs"], "settings": { "stereoBoost": 1.5, "sharpen": 3 } } ]
    },
    "pivotxr": { "enabled": false, "defaults": { "yawLeft": { "maxExtraDegrees": 30, "curve": "s" } }, "profiles": [] },
    "turbo": {
      "enabled": true,
      "toggleBindings": [
        { "type": "keyboard", "chord": ["Ctrl", "F10"] },
        { "type": "device", "deviceGuid": "{11111111-0000-0000-0000-000000000001}", "inputPath": "button-3", "hint": "x" }
      ]
    },
    "framegen": { "enabled": true, "presenter": true },
    "eyetrack": { "enabled": true }
  }
}
)json");
    Expect(parsed.ok, "A config with unknown keys must still load: " + parsed.error);
    const std::vector<std::string> expected{
        "Unknown field ignored: telemetry",
        "Unknown field ignored: core.sound.chime",
        "Unknown field ignored: applications[0].icon",
        "Unknown field ignored: applications[0].match.path",
        "Unknown field ignored: modules.eyetrack",
        "Unknown field ignored: modules.depthxr.bindings.toggleEnabled.repeat",
        "Unknown field ignored: modules.depthxr.profiles[0].settings.sharpen",
        "Unknown field ignored: modules.pivotxr.defaults.yawLeft.curve",
        "Unknown field ignored: modules.turbo.toggleBindings[1].hint",
        "Unknown field ignored: modules.framegen.presenter",
    };
    std::string reported;
    for (const std::string& warning : parsed.warnings) reported += "\n  " + warning;
    Expect(parsed.warnings == expected, "Unknown keys were not reported once each by their JSON path:" + reported);
    const depthxr::ConfigDocument& document = parsed.document;
    Expect(document.core.sound_volume == 40 && document.applications.size() == 1 &&
               document.applications[0].match.exe_name == "DCS.exe" && document.depthxr.enabled &&
               document.depthxr.defaults.stereo_boost == 1.2 && FirstChordKey(document.depthxr.bindings.toggle_enabled_bindings, "Ctrl") &&
               document.depthxr.profiles.size() == 1 && document.depthxr.profiles[0].settings.stereo_boost == 1.5 &&
               document.pivotxr.defaults.yaw_left.max_extra_degrees == 30 && document.turbo.enabled &&
               document.turbo.toggle_bindings.size() == 2 && document.framegen.enabled,
           "Known settings beside unknown keys were not applied");

    const auto framegen = [](const std::string& module) {
        return depthxr::ParseConfig(R"({"version":3,"core":{"enabled":true},"applications":[],"modules":{"depthxr":{"enabled":false},"pivotxr":{"enabled":false},"framegen":)" +
                                    module + "}}");
    };
    const auto clean = framegen(R"({"enabled":true})");
    Expect(clean.ok && clean.warnings.empty(), "A config without unknown keys reported warnings: " + clean.error);

    // Wrong types, out-of-range values, invalid names and structural errors
    // beside an unknown key still reject the document, and name the error.
    const std::pair<const char*, const char*> fatal[] = {
        {R"({"presenter":true,"enabled":"yes"})", "enabled must be a boolean"},
        {R"({"presenter":true,"defaults":{"motionLimitPercent":30}})",
         "framegen.defaults.motionLimitPercent must be an integer from 2 to 25"},
        {R"({"presenter":true,"defaults":{"synthesisMode":"blend"}})",
         "framegen.defaults.synthesisMode must be interpolate or extrapolate"},
        {R"({"presenter":true,"defaults":[]})", "framegen.defaults must be an object"},
        {R"({"presenter":true,"profiles":[{"name":"DCS"}]})", "Missing required field: framegenProfile.applicationIds"},
        {R"({"presenter":true,"toggleBinding":{"type":"none"},"toggleBindings":[]})",
         "Fields toggleBindings and toggleBinding cannot both be present"},
    };
    for (const auto& [module, error] : fatal) {
        const auto result = framegen(module);
        Expect(!result.ok && result.error == error && result.warnings.empty(),
               std::string("An invalid setting beside an unknown key must stay fatal: ") + module + " gave '" +
                   result.error + "'");
    }
}

// The single binding keys of configs from before binding lists are read, so
// they are known keys: applied, and never reported as unknown.
void TestLegacySingularViewControlBindings() {
    const depthxr::ParseResult parsed = depthxr::ParseConfig(R"json(
{
  "version": 3,
  "core": { "enabled": true },
  "applications": [ { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } } ],
  "modules": {
    "depthxr": { "enabled": false },
    "pivotxr": {
      "enabled": false,
      "nudgeSets": [
        {
          "id": "hat",
          "name": "HAT",
          "settings": {
            "yawLeftBinding": { "type": "keyboard", "chord": ["Q"] },
            "yawRightBinding": { "type": "keyboard", "chord": ["E"] },
            "pitchUpBinding": { "type": "keyboard", "chord": ["R"] },
            "pitchDownBinding": { "type": "keyboard", "chord": ["F"] },
            "centerBinding": { "type": "keyboard", "chord": ["X"] }
          }
        }
      ],
      "profiles": [
        {
          "id": "views",
          "name": "Views",
          "applicationIds": ["dcs"],
          "enabled": true,
          "behavior": "snapViews",
          "viewControls": {
            "nudges": { "centerBinding": { "type": "keyboard", "chord": ["C"] } },
            "quickViews": [
              {
                "id": "check-six",
                "name": "Check Six",
                "yawDegrees": 180.0,
                "activationBinding": { "type": "keyboard", "chord": ["F9"] }
              }
            ]
          }
        }
      ]
    }
  }
}
)json");
    std::string reported;
    for (const std::string& warning : parsed.warnings) reported += "\n  " + warning;
    Expect(parsed.ok && parsed.warnings.empty(),
           "Legacy single binding keys must parse without warnings: " + parsed.error + reported);
    const depthxr::PivotViewControls& controls = parsed.document.pivotxr.profiles[0].view_controls;
    Expect(FirstChordKey(controls.nudges.center_bindings, "C"),
           "A nudge's legacy centerBinding was not applied");
    Expect(controls.quick_views.size() == 1 && controls.quick_views[0].activation_bindings.size() == 1 &&
               controls.quick_views[0].activation_bindings[0].binding.chord == std::vector<std::string>{"F9"} &&
               controls.quick_views[0].activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Toggle,
           "A quick view's legacy activationBinding was not applied as a toggle");
    const depthxr::PivotNudgeSettings& hat = parsed.document.pivotxr.nudge_sets[0].settings;
    Expect(FirstChordKey(hat.yaw_left_bindings, "Q") && FirstChordKey(hat.yaw_right_bindings, "E") &&
               FirstChordKey(hat.pitch_up_bindings, "R") && FirstChordKey(hat.pitch_down_bindings, "F") &&
               FirstChordKey(hat.center_bindings, "X"),
           "A nudge set's legacy single bindings were not applied");
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
        "compatibilityMode": true,
        "stereoBoost": 1.1,
        "convergence": 0.08,
        "depthAnchor": false
      },
      "bindings": {
        "toggleEnabled": {
          "type": "keyboard",
          "chord": ["F7"]
        },
        "toggleAnchor": {
          "type": "keyboard",
          "chord": ["F9"]
        }
      },
      "profiles": [
        {
          "name": "Game",
          "applicationIds": ["game"],
          "enabled": true,
          "settings": {
            "compatibilityMode": false,
            "stereoBoost": 1.2,
            "depthAnchor": true
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
    Expect(!result.document.depthxr.defaults.depth_anchor, "DepthXR default Depth Anchor mismatch");
    Expect(result.document.depthxr.profiles[0].settings.depth_anchor.value_or(false), "DepthXR profile Depth Anchor mismatch");
    Expect(FirstChordKey(result.document.depthxr.bindings.toggle_enabled_bindings, "F7"), "DepthXR toggle binding mismatch");
    Expect(FirstChordKey(result.document.depthxr.bindings.toggle_anchor_bindings, "F9"),
           "DepthXR Depth Anchor toggle binding mismatch");
    Expect(result.document.depthxr.profiles.size() == 1, "DepthXR profile count mismatch");
    Expect(result.document.depthxr.profiles[0].application_ids[0] == "game", "DepthXR profile application id mismatch");
    Expect(result.document.pivotxr.profiles.size() == 1, "PivotXR profile count mismatch");
    Expect(result.document.pivotxr.profiles[0].activation_bindings[0].binding.chord[0] == "F8", "PivotXR profile activation binding mismatch");
    Expect(std::abs(result.document.pivotxr.profiles[0].settings.yaw_max_extra_degrees - 30.0) < 0.0001,
           "PivotXR profile yaw clamp mismatch");
    Expect(std::abs(result.document.pivotxr.profiles[0].settings.pitch_rotation_multiplier - 1.2) < 0.0001,
           "PivotXR profile pitch multiplier mismatch");
}

void TestPivotActivationBindingModel() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [],
  "modules": {
    "depthxr": {},
    "pivotxr": {
      "enabled": true,
      "alwaysActive": true,
      "activationBindings": [
        {
          "behavior": "toggle",
          "binding": { "type": "keyboard", "chord": ["F8"] }
        },
        {
          "behavior": "hold",
          "binding": { "type": "keyboard", "chord": ["F9"] }
        }
      ],
      "profiles": []
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected per-binding Pivot activation model: " + result.error);
    Expect(result.document.pivotxr.always_active, "Pivot always-active baseline was not parsed");
    Expect(result.document.pivotxr.activation_bindings.size() == 2,
           "Pivot mixed activation binding count mismatch");
    Expect(result.document.pivotxr.activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Toggle &&
               result.document.pivotxr.activation_bindings[0].binding.chord[0] == "F8",
           "Pivot Toggle binding was not parsed");
    Expect(result.document.pivotxr.activation_bindings[1].behavior == depthxr::PivotActivationBehavior::Hold &&
               result.document.pivotxr.activation_bindings[1].binding.chord[0] == "F9",
           "Pivot Hold binding was not parsed");
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
        "convergence": 0.0,
        "depthAnchor": false
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
            "convergence": 0.12,
            "depthAnchor": true
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
    Expect(resolved.depthxr.depth_anchor, "Profile Depth Anchor override was not applied");
    Expect(resolved.depthxr_bindings.toggle_enabled_bindings.empty(), "DepthXR toggle binding should be none");
    Expect(resolved.pivotxr.enabled, "PivotXR module enable was not resolved");
    Expect(resolved.pivotxr.profiles.size() == 1, "PivotXR resolved candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& pivot_profile = resolved.pivotxr.profiles[0];
    Expect(!pivot_profile.always_active && pivot_profile.activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Hold, "PivotXR activation mode mismatch");
    Expect(pivot_profile.activation_bindings[0].binding.chord.size() == 2, "PivotXR activation chord size mismatch");
    Expect(pivot_profile.activation_bindings[0].binding.chord[1] == "Space", "PivotXR activation binding was not resolved");
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
    Expect(!dcs_profile.always_active && dcs_profile.activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Toggle, "PivotXR activation mode mismatch");
    Expect(dcs_profile.activation_bindings[0].binding.chord[0] == "F8", "PivotXR activation binding mismatch");
    Expect(std::abs(dcs_profile.yaw_rotation_multiplier - 2.0) < 0.0001, "PivotXR yaw multiplier mismatch");
    Expect(std::abs(dcs_profile.yaw_max_extra_degrees - 60.0) < 0.0001, "PivotXR yaw clamp mismatch");
    Expect(std::abs(dcs_profile.pitch_rotation_multiplier - 1.5) < 0.0001, "PivotXR pitch multiplier mismatch");

    // No profile for a different exe falls back to the default Pivot profile.
    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "other.exe");
    Expect(resolved_other.pivotxr.enabled, "PivotXR default profile should resolve for exe with no matching profile");
    Expect(resolved_other.pivotxr.profiles.size() == 1, "PivotXR default candidate count mismatch");
    Expect(resolved_other.pivotxr.profiles[0].activation_bindings.empty(),
           "PivotXR default activation bindings should fall back to an empty list");
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
          "activationBindings": [
            { "type": "keyboard", "chord": ["F8"] },
            { "type": "keyboard", "chord": ["F10"] },
            { "type": "keyboard", "chord": ["F8"] }
          ],
          "settings": { "rotationMultiplier": 1.5 }
        },
        {
          "id": "dcs-disabled",
          "name": "DCS Disabled",
          "applicationIds": ["dcs"],
          "enabled": false,
          "activationBinding": { "type": "keyboard", "chord": ["F11"] },
          "settings": { "rotationMultiplier": 3.0 }
        },
        {
          "id": "dcs-strong",
          "name": "DCS Strong",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "hold",
          "activationBindings": [
            { "type": "keyboard", "chord": ["F8"] },
            { "type": "keyboard", "chord": ["F9"] }
          ],
          "settings": { "rotationMultiplier": 2.5 }
        },
        {
          "name": "DCS Shadowed",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationBindings": [
            { "type": "keyboard", "chord": ["F8"] },
            { "type": "keyboard", "chord": ["F10"] }
          ],
          "settings": { "rotationMultiplier": 4.0 }
        },
        {
          "name": "DCS Always",
          "applicationIds": ["dcs"],
          "enabled": true,
          "activationMode": "alwaysOn",
          "activationBindings": [
            { "type": "keyboard", "chord": ["F8"] },
            { "type": "keyboard", "chord": ["F12"] }
          ],
          "settings": { "rotationMultiplier": 3.0 }
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
    // Disabled profiles and profiles whose entire binding list is shadowed are skipped.
    // Individual colliding bindings are stripped while each profile's remaining
    // independent bindings stay available.
    Expect(resolved.pivotxr.profiles.size() == 3, "PivotXR multi-profile candidate count mismatch");
    Expect(resolved.pivotxr.profiles[0].activation_bindings.size() == 2,
           "First candidate should retain both independent bindings and prune its duplicate");
    Expect(resolved.pivotxr.profiles[0].name == "DCS Mild", "Candidate priority order mismatch (first)");
    Expect(resolved.pivotxr.profiles[1].name == "DCS Strong", "Candidate priority order mismatch (second)");
    Expect(std::abs(resolved.pivotxr.profiles[1].yaw_rotation_multiplier - 2.5) < 0.0001,
           "Second candidate settings mismatch");
    Expect(!resolved.pivotxr.profiles[1].always_active && resolved.pivotxr.profiles[1].activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Hold,
           "Second candidate activation mode mismatch");
    Expect(resolved.pivotxr.profiles[1].activation_bindings.size() == 1 &&
               resolved.pivotxr.profiles[1].activation_bindings[0].binding.chord[0] == "F9",
           "Second candidate should retain only its unshadowed activation binding");
    Expect(resolved.pivotxr.profiles[2].name == "DCS Always", "Candidate priority order mismatch (third)");
    Expect(resolved.pivotxr.profiles[2].always_active,
           "Shadowed always-on candidate activation mode mismatch");
    Expect(resolved.pivotxr.profiles[2].activation_bindings.size() == 1 &&
               resolved.pivotxr.profiles[2].activation_bindings[0].binding.chord[0] == "F12",
           "Always-on candidate should retain its unshadowed suspend/resume binding");
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
            "deadzoneDegrees": 180.0,
            "pitchDeadzoneDegrees": 90.0
          }
        },
        {
          "name": "MSFS Advanced",
          "applicationIds": ["msfs"],
          "enabled": true,
          "activationBinding": { "type": "keyboard", "chord": ["F9"] },
          "settings": {
            "advancedAxes": true,
            "responseMode": "stepped",
            "stepGlideMode": "instant",
            "stepGlideSeconds": 0.0,
            "yawStep": { "deadzoneDegrees": 7.0, "triggerDegrees": 11.0, "amountDegrees": 17.0, "hysteresisDegrees": 3.0, "maxExtraDegrees": 80.0 },
            "pitchStep": { "deadzoneDegrees": 9.0, "triggerDegrees": 13.0, "amountDegrees": 19.0, "hysteresisDegrees": 4.0, "maxExtraDegrees": 60.0 },
            "yawLeftStep": { "amountDegrees": 21.0 },
            "yawRightStep": { "triggerDegrees": 14.0 },
            "pitchUpStep": { "maxExtraDegrees": 45.0 },
            "pitchDownStep": { "deadzoneDegrees": 12.0 },
            "yawLeft": { "rotationMultiplier": 2.0, "deadzoneDegrees": 180.0, "maxExtraDegrees": 90.0 },
            "yawRight": { "rotationMultiplier": 1.2 },
            "pitchUp": { "rotationMultiplier": 1.8, "deadzoneDegrees": 90.0 },
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
    Expect(stepped_profile.always_active,
           "alwaysOn activation mode was not parsed");
    Expect(stepped_profile.response_mode == depthxr::PivotResponseMode::Stepped,
           "Stepped response mode was not resolved");
    Expect(std::abs(stepped_profile.yaw_step_positive.trigger_degrees - 12.0) < 0.0001,
           "Legacy trigger did not migrate to yaw");
    Expect(std::abs(stepped_profile.pitch_step_positive.trigger_degrees - 12.0) < 0.0001,
           "Legacy trigger did not migrate to pitch");
    Expect(std::abs(stepped_profile.yaw_step_negative.amount_degrees - 15.0) < 0.0001,
           "Legacy amount did not migrate to both yaw directions");
    Expect(std::abs(stepped_profile.pitch_step_negative.hysteresis_degrees - 5.0) < 0.0001,
           "Legacy hysteresis did not migrate to both pitch directions");
    Expect(stepped_profile.step_glide_mode == depthxr::PivotStepGlideMode::Glide &&
               std::abs(stepped_profile.step_glide_seconds - 0.05) < 0.0001,
           "Legacy stepped smoothing did not migrate to glide");

    const depthxr::ResolvedRuntimeConfig advanced = depthxr::ResolveRuntimeConfig(result.document, "MSFS.exe");
    Expect(advanced.pivotxr.profiles.size() == 1, "Advanced candidate count mismatch");
    const depthxr::PivotXrResolvedProfile& advanced_profile = advanced.pivotxr.profiles[0];
    Expect(std::abs(advanced_profile.yaw_positive.rotation_multiplier - 2.0) < 0.0001,
           "Advanced yaw-left multiplier mismatch");
    Expect(std::abs(advanced_profile.yaw_positive.deadzone_degrees - 180.0) < 0.0001,
           "Advanced yaw-left deadzone boundary mismatch");
    Expect(std::abs(advanced_profile.yaw_positive.max_extra_degrees - 90.0) < 0.0001,
           "Advanced yaw-left max extra mismatch");
    Expect(std::abs(advanced_profile.yaw_negative.rotation_multiplier - 1.2) < 0.0001,
           "Advanced yaw-right multiplier mismatch");
    Expect(std::abs(advanced_profile.pitch_positive.rotation_multiplier - 1.8) < 0.0001,
           "Advanced pitch-up multiplier mismatch");
    Expect(std::abs(advanced_profile.pitch_positive.deadzone_degrees - 90.0) < 0.0001,
           "Advanced pitch-up deadzone boundary mismatch");
    Expect(std::abs(advanced_profile.pitch_negative.max_extra_degrees - 30.0) < 0.0001,
           "Advanced pitch-down max extra mismatch");

    Expect(advanced_profile.response_mode == depthxr::PivotResponseMode::Stepped,
           "Advanced stepped response mode mismatch");
    Expect(advanced_profile.step_glide_mode == depthxr::PivotStepGlideMode::Instant &&
               std::abs(advanced_profile.step_glide_seconds) < 0.0001,
           "Canonical instant glide settings mismatch");
    Expect(std::abs(advanced_profile.yaw_step_positive.amount_degrees - 21.0) < 0.0001 &&
               std::abs(advanced_profile.yaw_step_positive.trigger_degrees - 11.0) < 0.0001,
           "Yaw-left step override did not inherit the remaining basic yaw fields");
    Expect(std::abs(advanced_profile.yaw_step_negative.trigger_degrees - 14.0) < 0.0001 &&
               std::abs(advanced_profile.yaw_step_negative.amount_degrees - 17.0) < 0.0001,
           "Yaw-right step override did not inherit the remaining basic yaw fields");
    Expect(std::abs(advanced_profile.pitch_step_positive.max_extra_degrees - 45.0) < 0.0001 &&
               std::abs(advanced_profile.pitch_step_negative.deadzone_degrees - 12.0) < 0.0001,
           "Advanced pitch step overrides mismatch");

    // Symmetric profiles collapse direction tunings to the yaw/pitch values.
    Expect(std::abs(stepped_profile.yaw_positive.deadzone_degrees - 180.0) < 0.0001,
           "Symmetric collapse should mirror yaw deadzone into direction tunings");
    Expect(std::abs(stepped_profile.pitch_positive.deadzone_degrees - 90.0) < 0.0001,
           "Symmetric collapse should mirror pitch deadzone into direction tunings");
}

void TestPivotViewControlsParsingAndResolution() {
    const std::string json = R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7 },
  "applications": [
    { "id": "dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } }
  ],
  "modules": {
    "depthxr": {},
    "pivotxr": {
      "enabled": false,
      "nudgeSets": [
        {
          "id": "shared-hat",
          "name": "Shared HAT",
          "allowWhileInactive": true,
          "settings": {
            "yawStepDegrees": 12.0,
            "yawLeftBindings": [{ "type": "keyboard", "chord": ["Q"] }]
          }
        }
      ],
      "profiles": [
        {
          "id": "accessible-views",
          "name": "Accessible Views",
          "applicationIds": ["dcs"],
          "enabled": true,
          "behavior": "snapViews",
          "snapTurnPreference": "left",
          "nudgeSetId": "shared-hat",
          "alwaysActive": false,
          "activationBindings": [],
          "viewControls": {
            "nudges": {
              "yawStepDegrees": 35.0,
              "pitchStepDegrees": 15.0,
              "transitionSeconds": 0.2,
              "yawLeftBindings": [{ "type": "keyboard", "chord": ["Q"] }],
              "yawRightBindings": [],
              "pitchUpBindings": [],
              "pitchDownBindings": [],
              "centerBindings": [{ "type": "keyboard", "chord": ["C"] }]
            },
            "quickViews": [
              {
                "id": "check-six",
                "name": "Check Six",
                "yawDegrees": 180.0,
                "pitchDegrees": 10.0,
                "positionRightCm": 2.0,
                "positionUpCm": 3.0,
                "positionForwardCm": 4.0,
                "transitionSeconds": 0.25,
                "activationBindings": [
                  {
                    "behavior": "hold",
                    "binding": { "type": "keyboard", "chord": ["F9"] }
                  }
                ]
              }
            ]
          },
          "settings": {}
        }
      ]
    }
  }
}
)json";

    depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected Pivot View Controls: " + result.error);
    const depthxr::PivotViewControls& parsed =
        result.document.pivotxr.profiles[0].view_controls;
    Expect(std::abs(parsed.nudges.yaw_step_degrees - 35.0) < 0.0001 &&
               result.document.pivotxr.nudge_sets[0].allow_while_inactive,
           "Pivot nudge settings or inactive policy mismatch");
    Expect(parsed.nudges.yaw_left_bindings[0].chord[0] == "Q" &&
               parsed.nudges.center_bindings[0].chord[0] == "C",
           "Pivot nudge bindings mismatch");
    Expect(parsed.quick_views.size() == 1 && parsed.quick_views[0].id == "check-six",
           "Pivot Quick View identity mismatch");
    Expect(parsed.quick_views[0].activation_bindings[0].behavior ==
               depthxr::PivotActivationBehavior::Hold,
           "Pivot Quick View activation mismatch");

    const depthxr::ResolvedRuntimeConfig resolved =
        depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(resolved.pivotxr.enabled && resolved.pivotxr.profiles.size() == 1,
           "A profile with View Control bindings must resolve without Motion Assist activation");
    Expect(resolved.pivotxr.profiles[0].view_controls.quick_views[0].position_forward_cm == 4.0,
           "Resolved Pivot Quick View position mismatch");
    Expect(resolved.pivotxr.profiles[0].behavior == depthxr::PivotProfileBehavior::SnapViews &&
               resolved.pivotxr.profiles[0].allow_inactive_nudges &&
               resolved.pivotxr.profiles[0].nudge_set_id == "shared-hat",
           "Resolved Pivot profile behavior or view-control policy mismatch");
    Expect(std::abs(resolved.pivotxr.profiles[0].view_controls.nudges.yaw_step_degrees - 12.0) < 0.0001,
           "Linked Nudge Set did not override legacy inline nudge settings");

    result.document.pivotxr.profiles[0].nudge_set_id = "none";
    const depthxr::ResolvedRuntimeConfig nudges_disabled =
        depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    const depthxr::PivotNudgeSettings& disabled_nudges =
        nudges_disabled.pivotxr.profiles[0].view_controls.nudges;
    Expect(disabled_nudges.yaw_left_bindings.empty() &&
               disabled_nudges.center_bindings.empty(),
           "The reserved none Nudge Set id must disable all profile nudges");
}

void TestPivotViewTransitionMath() {
    double activation_gain = depthxr::AdvancePivotActivationGain(0.65, false, true, 0.2, 1.0);
    Expect(std::abs(activation_gain - 0.65) < 0.0001,
           "A Quick View override must freeze the suspended motion activation gain");
    activation_gain = depthxr::AdvancePivotActivationGain(activation_gain, true, true, 0.2, 1.0);
    Expect(std::abs(activation_gain - 0.65) < 0.0001,
           "A Quick View return must preserve its motion handoff gain");
    activation_gain = depthxr::AdvancePivotActivationGain(activation_gain, true, false, 0.2, 0.05);
    Expect(std::abs(activation_gain - 0.9) < 0.0001,
           "Motion activation must resume after the Quick View handoff");

    depthxr::PivotViewTransitionState state;
    depthxr::PivotViewOffset target{1.0, -0.5, 0.1, 0.2, -0.3};
    depthxr::RetargetPivotViewTransition(target, 0.2, state);
    Expect(state.active && depthxr::PivotViewOffsetNearlyZero(state.current),
           "Pivot View transition did not begin at the current offset");

    depthxr::UpdatePivotViewTransition(0.1, state);
    Expect(std::abs(state.current.yaw_radians - 0.5) < 0.0001 &&
               std::abs(state.current.right_meters - 0.05) < 0.0001,
           "Pivot View smoothstep midpoint mismatch");

    const depthxr::PivotViewOffset midpoint = state.current;
    depthxr::RetargetPivotViewTransition({}, 0.1, state);
    Expect(depthxr::PivotViewOffsetNearlyEqual(state.start, midpoint),
           "Pivot View retarget did not preserve the in-flight position");
    depthxr::UpdatePivotViewTransition(0.1, state);
    Expect(!state.active && depthxr::PivotViewOffsetNearlyZero(state.current),
           "Pivot View transition did not settle exactly on its target");

    depthxr::RetargetPivotViewTransition(target, 0.0, state);
    Expect(state.active && depthxr::PivotViewOffsetNearlyZero(state.current),
           "A zero-duration Pivot View retarget must not move the offset before the next drive update");
    depthxr::UpdatePivotViewTransition(0.0, state);
    Expect(!state.active && depthxr::PivotViewOffsetNearlyEqual(state.current, target),
           "A zero-duration Pivot View transition must land on the next drive update");
}

void TestPivotSteppedGlideMath() {
    const auto radians = [](double degrees) {
        return degrees * 3.14159265358979323846 / 180.0;
    };
    const depthxr::PivotStepTuning positive{10.0, 10.0, 15.0, 4.0, 60.0};
    const depthxr::PivotStepTuning negative{8.0, 12.0, 25.0, 3.0, 75.0};

    int step = 0;
    double displayed = 0.0;
    depthxr::PivotStepGlideState state;
    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(20.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Instant, 0.0, 0.0,
                                                 step, displayed, state);
    Expect(step == 1 && std::abs(displayed - radians(15.0)) < 0.000001,
           "Instant stepped response did not land exactly on its target");

    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(17.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Instant, 0.0, 0.0,
                                                 step, displayed, state);
    Expect(step == 1, "Step hysteresis released above the bounded release threshold");
    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(15.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Instant, 0.0, 0.0,
                                                 step, displayed, state);
    Expect(step == 0, "Step hysteresis did not release below its threshold");

    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(-20.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Instant, 0.0, 0.0,
                                                 step, displayed, state);
    Expect(step == -1 && std::abs(displayed - radians(-25.0)) < 0.000001,
           "Negative direction did not use its independent trigger and amount");

    const auto glide_at = [&](double frame_seconds, int frames) {
        int glide_step = 0;
        double glide_displayed = 0.0;
        depthxr::PivotStepGlideState glide_state;
        for (int frame = 0; frame < frames; ++frame) {
            depthxr::UpdatePivotSteppedExtraAngleRadians(radians(20.0), positive, negative,
                                                         depthxr::PivotStepGlideMode::Glide, 0.12,
                                                         frame_seconds, glide_step,
                                                         glide_displayed, glide_state);
        }
        return glide_displayed;
    };
    const double sixty_hz_midpoint = glide_at(0.01, 6);
    const double thirty_hz_midpoint = glide_at(0.02, 3);
    Expect(std::abs(sixty_hz_midpoint - thirty_hz_midpoint) < 0.000001,
           "Step glide changed with frame rate at the same elapsed time");
    Expect(std::abs(glide_at(0.02, 6) - radians(15.0)) < 0.000001,
           "Step glide did not settle exactly on target");

    step = 0;
    displayed = 0.0;
    state = {};
    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(20.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Glide, 0.12, 0.04,
                                                 step, displayed, state);
    const double before_retarget = displayed;
    depthxr::UpdatePivotSteppedExtraAngleRadians(radians(30.0), positive, negative,
                                                 depthxr::PivotStepGlideMode::Glide, 0.12, 0.0,
                                                 step, displayed, state);
    Expect(step == 2 && std::abs(displayed - before_retarget) < 0.000001 &&
               std::abs(state.start_radians - before_retarget) < 0.000001,
           "Mid-glide retarget did not start from the currently displayed angle");
    double previous = displayed;
    for (int frame = 0; frame < 6; ++frame) {
        depthxr::UpdatePivotSteppedExtraAngleRadians(radians(30.0), positive, negative,
                                                     depthxr::PivotStepGlideMode::Glide, 0.12, 0.02,
                                                     step, displayed, state);
        Expect(displayed >= previous && displayed <= radians(30.0) + 0.000001,
               "Retargeted step glide was non-monotonic or overshot");
        previous = displayed;
    }
    Expect(std::abs(displayed - radians(30.0)) < 0.000001,
           "Retargeted step glide did not settle exactly on target");
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
    Expect(FirstBinding(resolved.turbo.toggle_bindings).chord.size() == 2, "Turbo toggle binding chord mismatch");

    const depthxr::ResolvedRuntimeConfig resolved_other = depthxr::ResolveRuntimeConfig(result.document, "other.exe");
    Expect(!resolved_other.turbo.enabled, "Turbo default-off should not apply to unmatched applications");
    std::string opted_out = json;
    opted_out.insert(opted_out.find("\"id\": \"turbo-dcs\""), "\"disableSafety\": true, ");
    auto safety = depthxr::ParseConfig(opted_out);
    Expect(safety.ok, "Profile safety override should parse");
    Expect(!depthxr::ResolveRuntimeConfig(safety.document, "DCS.exe").turbo.interrupted_session_recovery,
           "Matched profile can disable safety");
    Expect(depthxr::ResolveRuntimeConfig(safety.document, "other.exe").turbo.interrupted_session_recovery,
           "Profile opt-out must not disable protection for unrelated applications");
    safety.document.turbo.profiles[0].enabled = false;
    Expect(depthxr::ResolveRuntimeConfig(safety.document, "DCS.exe").turbo.interrupted_session_recovery,
           "Disabled profiles cannot opt out of safety");
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
    Expect(result.document.turbo.interrupted_session_recovery, "Older configs should default recovery on");
    std::string recovery_off = json;
    recovery_off.insert(recovery_off.find("\"pacingMode\""), "\"interruptedSessionRecovery\": false, ");
    const auto recovery_result = depthxr::ParseConfig(recovery_off);
    Expect(recovery_result.ok && !recovery_result.document.turbo.interrupted_session_recovery,
           "Recovery opt-out should parse");
    Expect(!depthxr::ResolveRuntimeConfig(recovery_result.document, "any.exe").turbo.interrupted_session_recovery,
           "Recovery opt-out must reach resolved settings");
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

std::string FrameGenConfigJson(const std::string& framegen_module, const std::string& osd = "{}") {
    return std::string(R"json(
{
  "version": 3,
  "core": { "enabled": true, "logLevel": "info", "logRetentionFiles": 7, "osd": )json") + osd + R"json( },
  "applications": [
    { "id": "app-dcs", "name": "DCS", "enabled": true, "match": { "exe": "DCS.exe" } },
    { "id": "app-msfs", "name": "MSFS", "enabled": true, "match": { "exe": "FlightSimulator2024.exe" } }
  ],
  "modules": {
    "depthxr": {
      "enabled": false,
      "defaults": { "stereoBoost": 1.0, "convergence": 0.0 },
      "bindings": { "toggleEnabled": { "type": "none" } },
      "profiles": []
    },
    "pivotxr": { "enabled": false, "defaults": {}, "profiles": [] },
    "framegen": )json" + framegen_module + R"json(
  }
}
)json";
}

void TestFrameGenModuleOptional() {
    // Configs written before Frame Generation existed must parse with it off.
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
    Expect(result.ok, "Config without framegen module was rejected: " + result.error);
    Expect(!result.document.framegen.enabled, "Frame Generation should default off when absent");
    const auto resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(!resolved.framegen.enabled, "Frame Generation should resolve off when absent");
    Expect(resolved.framegen.synthesis_mode == depthxr::FrameGenSynthesisMode::kInterpolate,
           "Frame Generation should default to interpolation");
    Expect(resolved.framegen.flow_quality == depthxr::FrameGenFlowQuality::kBalanced,
           "Frame Generation should default to balanced flow");
    Expect(resolved.framegen.motion_estimation == depthxr::FrameGenMotionEstimation::kShader,
           "Frame Generation should default to shader motion estimation");
    Expect(resolved.framegen.artifact_suppression == 50 && resolved.framegen.motion_limit_percent == 10,
           "Frame Generation numeric defaults changed");
    Expect(resolved.framegen.diagnostic_view == depthxr::FrameGenDiagnosticView::kOff,
           "Frame Generation diagnostics should default off");
    Expect(resolved.framegen.multiplier == depthxr::FrameGenMultiplier::kDynamic,
           "Frame Generation should default to Dynamic");
    Expect(resolved.core.osd.show_framegen && !resolved.core.osd.compact_show_framegen,
           "OSD Frame Gen rows should default to full-panel only");
    Expect(resolved.core.osd.show_framegen_fps, "The OSD should show Frame Generation beside FPS by default");
    Expect(resolved.core.osd.body_order.size() == 6 && resolved.core.osd.body_order[3] == "framegen",
           "Default OSD body order should include the Frame Gen row after Turbo");
}

void TestFrameGenProfileResolution() {
    const std::string module = R"json({
      "enabled": false,
      "diagnosticView": "tint",
      "toggleBinding": { "type": "keyboard", "chord": ["Ctrl", "Alt", "F9"] },
      "defaults": { "synthesisMode": "interpolate", "flowQuality": "performance", "frameMultiplier": "2x", "artifactSuppression": 40, "motionLimitPercent": 8 },
      "profiles": [
        { "name": "Disabled", "enabled": false, "applicationIds": ["app-dcs"],
          "settings": { "synthesisMode": "interpolate", "flowQuality": "quality" } },
        { "name": "DCS", "enabled": true, "applicationIds": ["app-dcs"],
          "settings": { "synthesisMode": "extrapolate", "flowQuality": "quality", "motionEstimation": "hardware", "frameMultiplier": "4x", "artifactSuppression": 75, "motionLimitPercent": 25 } }
      ]
    })json";
    const depthxr::ParseResult result = depthxr::ParseConfig(FrameGenConfigJson(module));
    Expect(result.ok, "Frame Generation module was rejected: " + result.error);
    Expect(result.document.framegen.profiles.size() == 2, "Frame Generation profiles were not parsed");
    Expect(FirstBinding(result.document.framegen.toggle_bindings).type == depthxr::InputBindingType::Keyboard,
           "Frame Generation toggle binding was not parsed");

    const auto dcs = depthxr::ResolveFrameGenSettings(result.document, "DCS.exe");
    Expect(dcs.enabled, "Matching enabled profile should enable Frame Generation over an off default");
    Expect(dcs.profile_name == "DCS", "Disabled profiles must not win resolution");
    Expect(dcs.synthesis_mode == depthxr::FrameGenSynthesisMode::kExtrapolate &&
               dcs.flow_quality == depthxr::FrameGenFlowQuality::kQuality &&
               dcs.artifact_suppression == 75 && dcs.motion_limit_percent == 25,
           "Profile settings did not replace defaults");
    Expect(dcs.multiplier == depthxr::FrameGenMultiplier::k4, "Profile frame multiplier did not apply");
    Expect(dcs.motion_estimation == depthxr::FrameGenMotionEstimation::kHardware, "Profile motion estimation did not apply");
    Expect(dcs.diagnostic_view == depthxr::FrameGenDiagnosticView::kTint,
           "Module diagnostic view should apply to profiles");

    const auto other = depthxr::ResolveFrameGenSettings(result.document, "FlightSimulator2024.exe");
    Expect(!other.enabled && other.profile_name.empty(), "Unmatched application should use the off default");
    Expect(other.motion_estimation == depthxr::FrameGenMotionEstimation::kShader,
           "Unmatched application should keep shader motion estimation");
    Expect(other.flow_quality == depthxr::FrameGenFlowQuality::kPerformance && other.artifact_suppression == 40 &&
               other.motion_limit_percent == 8,
           "Unmatched application should use module defaults");

    const depthxr::ParseResult dynamic =
        depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": true, "defaults": { "frameMultiplier": "dynamic" } })json"));
    Expect(dynamic.ok && dynamic.document.framegen.defaults.multiplier == depthxr::FrameGenMultiplier::kDynamic,
           "Dynamic frame multiplier was not parsed");
    Expect(std::string(depthxr::ToString(depthxr::FrameGenMultiplier::kDynamic)) == "dynamic",
           "Dynamic frame multiplier did not round-trip");
    // The fixed rates, Auto among them, are still chosen explicitly.
    const depthxr::ParseResult automatic =
        depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": true, "defaults": { "frameMultiplier": "auto" } })json"));
    Expect(automatic.ok && automatic.document.framegen.defaults.multiplier == depthxr::FrameGenMultiplier::kAuto,
           "Auto frame multiplier was not parsed");
    // A profile saved without a multiplier runs Dynamic.
    const depthxr::ParseResult inherited = depthxr::ParseConfig(FrameGenConfigJson(
        R"json({ "enabled": false, "profiles": [ { "name": "DCS", "applicationIds": ["app-dcs"], "settings": { "synthesisMode": "extrapolate" } } ] })json"));
    Expect(inherited.ok && depthxr::ResolveFrameGenSettings(inherited.document, "DCS.exe").multiplier ==
                               depthxr::FrameGenMultiplier::kDynamic,
           "A profile without a frame multiplier should run Dynamic");
}

// Frame Generation's A/B toggle takes a list too, and its legacy binding.
void TestFrameGenToggleBindingList() {
    const auto config = [](const std::string& framegen) {
        return depthxr::ParseConfig(
            R"({"version":3,"core":{"enabled":true,"logLevel":"info"},"applications":[],"modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},"bindings":{"toggleEnabled":{"type":"none"}},"profiles":[]},"pivotxr":{"enabled":false,"defaults":{},"profiles":[]},"framegen":)" +
            framegen + "}}");
    };
    const auto lists = config(
        R"({"enabled":false,"toggleBindings":[{"type":"device","deviceGuid":"{11111111-0000-0000-0000-000000000001}","inputPath":"button-23"},{"type":"keyboard","chord":["Ctrl","F8"]}]})");
    Expect(lists.ok && lists.document.framegen.toggle_bindings.size() == 2 &&
               lists.document.framegen.toggle_bindings[1].type == depthxr::InputBindingType::Keyboard,
           "Frame Generation's toggle binding list was not kept: " + lists.error);
    Expect(depthxr::ResolveFrameGenSettings(lists.document, "any.exe").toggle_bindings.size() == 2,
           "Frame Generation's toggle binding list did not resolve through");
    const auto legacy = config(R"({"enabled":false,"toggleBinding":{"type":"keyboard","chord":["Ctrl","F8"]}})");
    Expect(legacy.ok && legacy.document.framegen.toggle_bindings.size() == 1,
           "Frame Generation's legacy toggle binding was not read: " + legacy.error);
    Expect(!config(R"({"enabled":false,"toggleBinding":{"type":"none"},"toggleBindings":[]})").ok,
           "Frame Generation's toggle with both a list and a legacy binding was accepted");
}

void TestFrameGenInvalidSettingsRejected() {
    const std::array<std::string, 8> invalid_modules{
        R"json({ "enabled": true, "defaults": { "frameMultiplier": "6x" } })json",
        R"json({ "enabled": true, "defaults": { "synthesisMode": "blend" } })json",
        R"json({ "enabled": true, "defaults": { "flowQuality": "ultra" } })json",
        R"json({ "enabled": true, "defaults": { "motionEstimation": "cuda" } })json",
        R"json({ "enabled": true, "defaults": { "artifactSuppression": 101 } })json",
        R"json({ "enabled": true, "defaults": { "motionLimitPercent": 1 } })json",
        R"json({ "enabled": true, "defaults": { "motionLimitPercent": 7.5 } })json",
        R"json({ "enabled": true, "diagnosticView": "wireframe" })json",
    };
    for (const auto& module : invalid_modules) {
        const auto result = depthxr::ParseConfig(FrameGenConfigJson(module));
        Expect(!result.ok, "Invalid Frame Generation settings were accepted: " + module);
    }
    // Unknown keys are ignored with a warning, like other modules' (a newer
    // app may save settings this layer does not know).
    const auto unknown = depthxr::ParseConfig(FrameGenConfigJson(
        R"json({ "enabled": true, "presenter": true, "defaults": { "frameMultiplier": "3x", "lookahead": 2 } })json"));
    Expect(unknown.ok && unknown.document.framegen.enabled &&
               unknown.document.framegen.defaults.multiplier == depthxr::FrameGenMultiplier::k3 &&
               unknown.warnings == std::vector<std::string>{"Unknown field ignored: modules.framegen.presenter",
                                                            "Unknown field ignored: modules.framegen.defaults.lookahead"},
           "Unknown Frame Generation keys were not ignored with a warning: " + unknown.error);
}

// The PERFORMANCE row: on in the expanded layout by default, after the graph;
// off in the compact layout; appended to saved orders without it.
void TestOsdPerformanceRow() {
    const auto parse=[](const std::string& osd) {
        return depthxr::ParseConfig(R"({"version":3,"core":{"enabled":true,"logLevel":"info","osd":)"+osd+R"(},"applications":[],"modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},"bindings":{"toggleEnabled":{"type":"none"}},"profiles":[]},"pivotxr":{"enabled":false,"defaults":{},"profiles":[]}}})");
    };
    const auto defaults=parse("{}");
    const auto& osd=defaults.document.core.osd;
    Expect(defaults.ok && osd.show_performance && !osd.compact_show_performance,
           "The performance row should default on in the expanded layout and off in the compact one");
    Expect(osd.body_order==std::vector<std::string>{"graph","performance","turbo","framegen","pivot","modules"},
           "The default OSD body order should put the performance row after the graph");
    const auto configured=parse(R"({"showPerformance":false,"compactShowPerformance":true,"bodyOrder":["performance","graph","turbo","framegen","pivot","modules"]})");
    Expect(configured.ok && !configured.document.core.osd.show_performance &&
           configured.document.core.osd.compact_show_performance &&
           configured.document.core.osd.body_order.front()=="performance",
           "The performance row's settings were not parsed: "+configured.error);
    Expect(configured.warnings.empty(),"The performance row's keys were reported unknown");
    const auto saved=parse(R"({"bodyOrder":["pivot","modules","graph","turbo","framegen"]})");
    Expect(saved.ok && saved.document.core.osd.body_order.size()==6 && saved.document.core.osd.body_order.back()=="performance",
           "A saved order without the performance row should get it at the end");
    Expect(!parse(R"({"showPerformance":"yes"})").ok && !parse(R"({"compactShowPerformance":1})").ok,
           "A non-boolean performance row setting was accepted");
    Expect(!parse(R"({"bodyOrder":["performance","performance"]})").ok,"A repeated performance row was accepted");
    const auto preset=parse(R"({"customPresets":[{"name":"Numbers","settings":{"showPerformance":true,"compactShowPerformance":true}}]})");
    Expect(preset.ok,"OSD presets should accept the performance row: "+preset.error);
    // As the app saves it: bodyOrder holds the rows 0.19.0-0.19.2 layers know,
    // rowOrder the full order, which wins.
    const auto split=parse(R"({"bodyOrder":["pivot","graph","turbo","framegen","modules"],"rowOrder":["pivot","performance","graph","turbo","framegen","modules"],)"
                           R"("customPresets":[{"name":"Cockpit","settings":{"bodyOrder":["modules","graph"],"rowOrder":["performance","modules","graph"]}}]})");
    Expect(split.ok && split.warnings.empty() &&
               split.document.core.osd.body_order==std::vector<std::string>{"pivot","performance","graph","turbo","framegen","modules"},
           "rowOrder should give the full row order: "+split.error);
    // A row a newer app adds is skipped with a warning, not a failed config.
    const auto newer=parse(R"({"rowOrder":["graph","thermals","performance","turbo","framegen","pivot","modules"]})");
    Expect(newer.ok && newer.document.core.osd.body_order==std::vector<std::string>{"graph","performance","turbo","framegen","pivot","modules"} &&
               newer.warnings==std::vector<std::string>{"Unknown value ignored: core.osd.rowOrder \"thermals\""},
           "An unknown OSD row should be skipped with a warning: "+newer.error);
    Expect(!parse(R"({"rowOrder":["graph","graph"]})").ok && !parse(R"({"rowOrder":["graph",3]})").ok,
           "A repeated or non-string rowOrder entry was accepted");
}

void TestFrameGenOsdRows() {
    const auto result = depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": false })json",
        R"json({ "showFramegen": false, "compactShowFramegen": true, "showFramegenFps": false, "bodyOrder": ["modules", "graph", "turbo", "pivot"] })json"));
    Expect(result.ok, "OSD Frame Gen settings were rejected: " + result.error);
    const auto& osd = result.document.core.osd;
    Expect(!osd.show_framegen && osd.compact_show_framegen, "OSD Frame Gen row toggles were not parsed");
    Expect(!osd.show_framegen_fps, "OSD Frame Gen beside FPS was not parsed");
    const auto invalid = depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": false })json",
        R"json({ "showFramegenFps": "yes" })json"));
    Expect(!invalid.ok, "A non-boolean showFramegenFps was accepted");
    const auto preset = depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": false })json",
        R"json({ "customPresets": [ { "name": "Quiet", "settings": { "showFramegenFps": false } } ] })json"));
    Expect(preset.ok, "OSD presets should accept showFramegenFps: " + preset.error);
    Expect(osd.body_order.size() == 6 && osd.body_order.back() == "framegen" && osd.body_order[4] == "performance",
           "Saved OSD orders without the Frame Gen row should append it");
    const auto ordered = depthxr::ParseConfig(FrameGenConfigJson(R"json({ "enabled": false })json",
        R"json({ "bodyOrder": ["framegen", "graph", "turbo", "pivot", "modules"] })json"));
    Expect(ordered.ok && ordered.document.core.osd.body_order.front() == "framegen",
           "OSD body order should accept the Frame Gen row");
}

void TestFrameGenPlan() {
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    depthxr::FrameGenViewRequest request;
    request.extent = {2000, 1800};
    request.synthesize = true;
    request.temporal = true;
    request.newest.angle_left = request.previous.angle_left = -0.8f;
    request.newest.angle_right = request.previous.angle_right = 0.8f;
    request.newest.angle_up = request.previous.angle_up = 0.8f;
    request.newest.angle_down = request.previous.angle_down = -0.8f;
    depthxr::FrameGenTuning tuning;

    const auto pyramid = depthxr::FrameGenPyramid({2001, 7});
    Expect(pyramid[1].width == 1001 && pyramid[1].height == 4 && pyramid[5].height == 1,
           "Frame Generation pyramid should round up and never reach zero");

    const std::array<std::pair<depthxr::FrameGenFlowQuality, std::size_t>, 3> qualities{{
        {depthxr::FrameGenFlowQuality::kPerformance, 1},
        {depthxr::FrameGenFlowQuality::kBalanced, 2},
        {depthxr::FrameGenFlowQuality::kQuality, 3},
    }};
    for (const auto& [quality, refines] : qualities) {
        tuning.quality = quality;
        const auto plan = depthxr::BuildFrameGenViewPlan(request, tuning);
        Expect(plan.synthesize, "Frame Generation plan should synthesize with history");
        // luma + 4 downsamples + search + refines + filter + synthesize
        Expect(plan.dispatches.size() == 1 + 4 + 1 + refines + 1 + 1, "Unexpected Frame Generation dispatch count");
        Expect(plan.dispatches.front().pass == FrameGenPass::kLuma &&
                   plan.dispatches.back().pass == FrameGenPass::kSynthesizeUnorm8,
               "Frame Generation plan should start with luma and end with synthesis");
        const auto& final_refine = plan.dispatches[plan.dispatches.size() - 3];
        Expect(final_refine.pass == FrameGenPass::kRefine &&
                   final_refine.outputs[0].level == depthxr::FrameGenFinalFlowLevel(quality),
               "Last refine should write the quality's flow level");
        Expect(final_refine.inputs[3].kind == FrameGenTextureKind::kFilteredPrevious,
               "Temporal flow should seed refinement when requested");
        Expect(final_refine.inputs[0].level == depthxr::FrameGenMatchLevel(final_refine.outputs[0].level) &&
                   final_refine.constants.options[3] == (1u << (final_refine.outputs[0].level - final_refine.inputs[0].level)),
               "Refine should match on a finer level with the matching cell scale");
        for (const auto& dispatch : plan.dispatches) {
            Expect(dispatch.width > 0 && dispatch.height > 0, "Frame Generation dispatch has an empty extent");
        }
    }

    request.synthesize = false;
    const auto still = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(!still.synthesize && still.dispatches.size() == 1 &&
               (still.dispatches[0].constants.options[1] & depthxr::kFrameGenFlagStill) != 0,
           "Priming should write the newest frame through one still dispatch");
    request.synthesize = true;
    request.extent = {48, 48};
    Expect(!depthxr::BuildFrameGenViewPlan(request, tuning).synthesize, "Tiny views should not run optical flow");

    request.extent = {1024, 1024};
    request.output = depthxr::FrameGenOutputKind::kFloat16;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.diagnostic = depthxr::FrameGenDiagnosticView::kFlow;
    const auto extrapolate = depthxr::BuildFrameGenViewPlan(request, tuning);
    const auto& synth = extrapolate.dispatches.back();
    Expect(synth.pass == FrameGenPass::kSynthesizeFloat16, "Float swapchains should use the float synthesis pass");
    Expect((synth.constants.options[1] & depthxr::kFrameGenFlagExtrapolate) != 0 &&
               (synth.constants.options[1] & depthxr::kFrameGenFlagSrgbEncode) == 0,
           "Extrapolation flag set and no sRGB encode for float output");
    Expect(synth.constants.options[2] == static_cast<std::uint32_t>(depthxr::FrameGenDiagnosticView::kFlow),
           "Diagnostic view should reach the synthesis pass");
    // The Flow view's fine tint (research): only with that view on; off, or
    // with another view, every constant stays as it was.
    Expect((synth.constants.options[1] & depthxr::kFrameGenFlagFlowFine) == 0, "The Flow view's fine tint is off by default");
    {
        depthxr::FrameGenTuning fine = tuning;
        fine.flow_view_fine = true;
        const auto fine_plan = depthxr::BuildFrameGenViewPlan(request, fine);
        Expect((fine_plan.dispatches.back().constants.options[1] & depthxr::kFrameGenFlagFlowFine) != 0,
               "The Flow view tints sub-pixel motion with flow_view_fine");
        fine.diagnostic = depthxr::FrameGenDiagnosticView::kConfidence;
        tuning.diagnostic = depthxr::FrameGenDiagnosticView::kConfidence;
        const auto other = depthxr::BuildFrameGenViewPlan(request, fine);
        const auto plain = depthxr::BuildFrameGenViewPlan(request, tuning);
        bool same = other.dispatches.size() == plain.dispatches.size();
        for (std::size_t i = 0; same && i < other.dispatches.size(); ++i) {
            same = std::memcmp(&other.dispatches[i].constants, &plain.dispatches[i].constants,
                               sizeof(depthxr::FrameGenConstants)) == 0;
        }
        Expect(same, "flow_view_fine leaves every constant as it was without the Flow view");
        tuning.diagnostic = depthxr::FrameGenDiagnosticView::kFlow;
    }
    // The head-motion shimmer research: Catmull-Rom resampling and the smooth
    // eye-shift lookup set their flags (the smooth lookup with depth only),
    // and off they leave every constant as it was.
    {
        Expect((synth.constants.options[1] & (depthxr::kFrameGenFlagResampleCubic | depthxr::kFrameGenFlagResampleWide |
                                              depthxr::kFrameGenFlagResamplePrevious |
                                              depthxr::kFrameGenFlagShiftSmooth)) == 0,
               "Synthesis resamples bilinearly and reads the eye-shift map per texel by default");
        const auto flags_of = [&](const depthxr::FrameGenTuning& with) {
            return depthxr::BuildFrameGenViewPlan(request, with).dispatches.back().constants.options[1];
        };
        const std::uint32_t base_flags = flags_of(tuning);
        depthxr::FrameGenTuning cubic = tuning;
        const depthxr::FrameGenResampleChoice newest = depthxr::ParseFrameGenResample("cubic-newest");
        cubic.resample = newest.resample;
        cubic.resample_previous = newest.previous;
        Expect(flags_of(cubic) == (base_flags | depthxr::kFrameGenFlagResampleCubic),
               "cubic-newest reads the newest frame by Catmull-Rom alone");
        const depthxr::FrameGenResampleChoice wide = depthxr::ParseFrameGenResample("CUBIC9");
        cubic.resample = wide.resample;
        cubic.resample_previous = wide.previous;
        Expect(flags_of(cubic) == (base_flags | depthxr::kFrameGenFlagResampleCubic | depthxr::kFrameGenFlagResampleWide |
                                   depthxr::kFrameGenFlagResamplePrevious),
               "cubic9 reads both real frames by the whole Catmull-Rom filter");
        Expect(depthxr::ParseFrameGenResample("cubic").previous &&
                   depthxr::ParseFrameGenResample("lanczos").resample == depthxr::FrameGenResample::kBilinear &&
                   !depthxr::ParseFrameGenResample("bilinear").previous,
               "VECTORXR_FRAMEGEN_SYNTH_RESAMPLE's values parse as named");
        depthxr::FrameGenTuning smooth = tuning;
        smooth.shift_smooth = true;
        Expect(flags_of(smooth) == base_flags, "The smooth eye-shift lookup needs depth");
        request.depth = true;
        const std::uint32_t depth_flags = flags_of(tuning);
        Expect(flags_of(smooth) == (depth_flags | depthxr::kFrameGenFlagShiftSmooth),
               "The smooth eye-shift lookup is flagged with depth");
        depthxr::FrameGenTuning off = tuning;
        off.resample = depthxr::FrameGenResample::kBilinear;
        off.resample_previous = true; // ignored while bilinear
        const auto with = depthxr::BuildFrameGenViewPlan(request, off);
        const auto plain = depthxr::BuildFrameGenViewPlan(request, tuning);
        bool same = with.dispatches.size() == plain.dispatches.size();
        for (std::size_t i = 0; same && i < with.dispatches.size(); ++i) {
            same = std::memcmp(&with.dispatches[i].constants, &plain.dispatches[i].constants,
                               sizeof(depthxr::FrameGenConstants)) == 0;
        }
        Expect(same, "Bilinear resampling leaves every constant as it was");
        request.depth = false;
    }

    // Depth: the trust scores, their surface vote, the stereo depth-scale
    // vote (with this frame's stereo evidence) in two stages, then the resolve.
    request.depth = true;
    for (const bool stereo : {true, false}) {
        request.trust_stereo = stereo;
        const auto depth_plan = depthxr::BuildFrameGenViewPlan(request, tuning);
        std::vector<FrameGenPass> passes;
        for (const auto& dispatch : depth_plan.dispatches) {
            passes.push_back(dispatch.pass);
            if (dispatch.pass == FrameGenPass::kTrustResolve) break;
        }
        const std::vector<FrameGenPass> expected =
            stereo ? std::vector<FrameGenPass>{FrameGenPass::kTrustUpdate, FrameGenPass::kTrustVote,
                                               FrameGenPass::kDepthScaleVote, FrameGenPass::kDepthScaleVote,
                                               FrameGenPass::kTrustResolve}
                   : std::vector<FrameGenPass>{FrameGenPass::kTrustUpdate, FrameGenPass::kTrustVote,
                                               FrameGenPass::kTrustResolve};
        Expect(passes == expected, "Depth trust passes should vote per surface before the resolve");
        const auto& trust = depth_plan.dispatches[0];
        const auto half = depthxr::FrameGenPyramid(request.extent)[1];
        Expect(trust.width == half.width && trust.outputs[1].kind == FrameGenTextureKind::kTrustTiles &&
                   depth_plan.dispatches[1].inputs[0].kind == FrameGenTextureKind::kTrustTiles &&
                   depth_plan.dispatches[1].width == (half.width + depthxr::kFrameGenTrustTile - 1) / depthxr::kFrameGenTrustTile,
               "The trust update should count its tiles for the vote, which runs per tile");
        const auto& resolve = depth_plan.dispatches[passes.size() - 1];
        Expect(resolve.inputs[1].kind == FrameGenTextureKind::kTrustVotes &&
                   resolve.inputs[2].kind == (stereo ? FrameGenTextureKind::kDepthScaleVotes : FrameGenTextureKind::kNone) &&
                   resolve.constants.options[0] == (stereo ? depthxr::kFrameGenResolveScale : 0u),
               "The resolve should read the trust vote, and the stereo scale vote only with this frame's stereo evidence");
        // The texels the resolve leaves open, judged right after it by the
        // surface around them as it decided that (kDepthDecided), never by
        // the depth being written; a thread per span of texels each way.
        const auto& surface = depth_plan.dispatches[passes.size()];
        const std::uint32_t span = depthxr::kFrameGenTrustSurfaceSpan;
        Expect(resolve.outputs[0].kind == FrameGenTextureKind::kDepth &&
                   resolve.outputs[1].kind == FrameGenTextureKind::kDepthDecided &&
                   surface.pass == FrameGenPass::kTrustSurface && surface.width == (half.width + span - 1) / span &&
                   surface.height == (half.height + span - 1) / span && surface.constants.size[0] == half.width &&
                   surface.constants.size[1] == half.height &&
                   surface.inputs[0].kind == FrameGenTextureKind::kTrustNewest &&
                   surface.inputs[1].kind == FrameGenTextureKind::kNone &&
                   surface.inputs[2].kind == resolve.inputs[2].kind &&
                   surface.inputs[3].kind == FrameGenTextureKind::kDepthDecided &&
                   surface.outputs[0].kind == FrameGenTextureKind::kDepth &&
                   surface.outputs[1].kind == FrameGenTextureKind::kNone &&
                   surface.constants.options[0] == resolve.constants.options[0],
               "The surface test should follow the resolve, reading its decisions and writing the depth alone");
        if (stereo) {
            Expect(depth_plan.dispatches[2].outputs[0].kind == FrameGenTextureKind::kDepthScaleRaw &&
                       depth_plan.dispatches[3].inputs[0].kind == FrameGenTextureKind::kDepthScaleRaw &&
                       depth_plan.dispatches[3].constants.options[0] == depthxr::kFrameGenScaleAccept,
                   "The scale vote's second stage should accept the first stage's votes");
        }
    }
    const auto scale = depthxr::BuildFrameGenDepthScaleDispatch(request.extent, request.newest, request.previous);
    Expect(scale.pass == FrameGenPass::kDepthScale && scale.outputs[0].kind == FrameGenTextureKind::kDepthScales &&
               scale.constants.size[2] == depthxr::FrameGenPyramid(request.extent)[1].width,
           "The stereo scale probe should run per trust tile over the half-resolution depth");
    // Depth matching (research): the flag reaches synthesis only with depth,
    // and never the stale-flow frame, which has no depth bound.
    tuning.depth_match = true;
    Expect((depthxr::BuildFrameGenViewPlan(request, tuning).dispatches.back().constants.options[1] &
            depthxr::kFrameGenFlagDepthMatch) != 0,
           "Depth matching should reach synthesis with depth");
    {
        auto stale = request;
        stale.depth_previous = true;
        stale.shift = {0.1f, 0.0f, 0.0f};
        const auto plan = depthxr::BuildFrameGenSynthesisPlan(stale, tuning, depthxr::FrameGenSynthesisMode::kExtrapolate,
                                                              0.5f, false, true);
        Expect((plan.dispatches.back().constants.options[1] & depthxr::kFrameGenFlagDepthMatch) == 0,
               "Depth matching should not reach a stale-flow frame");
    }
    tuning.depth_match = false;
    // The motion map (research): built with the eye-shift map's passes into
    // kMotionMap right before each generated frame, which reads it.
    tuning.depth_splat = true;
    tuning.generated = 2;
    tuning.fractions = {0.5f, 1.0f, 0.5f, 0.5f};
    {
        const auto plan = depthxr::BuildFrameGenViewPlan(request, tuning);
        int maps = 0;
        bool read = true;
        for (std::size_t i = 0; i < plan.dispatches.size(); ++i) {
            const auto& dispatch = plan.dispatches[i];
            if (dispatch.pass == FrameGenPass::kShiftClear && dispatch.outputs[1].kind == FrameGenTextureKind::kMotionMap) {
                ++maps;
                Expect((dispatch.constants.options[1] & depthxr::kFrameGenFlagDepthSplat) != 0 &&
                           i + 4 < plan.dispatches.size() &&
                           plan.dispatches[i + 3].pass == FrameGenPass::kShiftFill &&
                           plan.dispatches[i + 2].inputs[1].kind == FrameGenTextureKind::kFilteredNewest,
                       "The motion map should be splatted, resolved and filled from the filtered flow");
                const auto& synth = plan.dispatches[i + 4];
                read = read && synth.inputs[5].kind == FrameGenTextureKind::kMotionMap &&
                       (synth.constants.options[1] & depthxr::kFrameGenFlagDepthSplat) != 0;
            }
        }
        Expect(maps == 2 && read && plan.frames_in_order, "Each generated frame should read its own motion map");
        // Interpolating, none (MSFS replays: worse there).
        tuning.mode = depthxr::FrameGenSynthesisMode::kInterpolate;
        bool none = true;
        for (const auto& dispatch : depthxr::BuildFrameGenViewPlan(request, tuning).dispatches) {
            none = none && dispatch.outputs[1].kind != FrameGenTextureKind::kMotionMap &&
                   (dispatch.constants.options[1] & depthxr::kFrameGenFlagDepthSplat) == 0;
        }
        tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
        Expect(none, "Interpolation should build no motion map");
    }
    tuning.generated = 1;
    tuning.fractions = {0.5f, 0.5f, 0.5f, 0.5f};
    request.depth = false;
    tuning.depth_match = true;
    Expect((depthxr::BuildFrameGenViewPlan(request, tuning).dispatches.back().constants.options[1] &
            (depthxr::kFrameGenFlagDepthMatch | depthxr::kFrameGenFlagDepthSplat)) == 0,
           "Depth matching and the motion map should need depth");
    tuning.depth_match = false;
    tuning.depth_splat = false;
    request.trust_stereo = false;

    // Fractions follow display times and clamp.
    using depthxr::FrameGenFraction;
    using depthxr::FrameGenSynthesisMode;
    Expect(std::abs(FrameGenFraction(FrameGenSynthesisMode::kInterpolate, 0, 22'222'222, 11'111'111) - 0.5f) < 1e-4f,
           "Interpolation halfway between two real frames should be 0.5");
    Expect(std::abs(FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 22'222'222, 33'333'333) - 0.5f) < 1e-4f,
           "Extrapolation one refresh past a half-rate frame should be 0.5");
    Expect(FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 10, 100) == 1.0f &&
               FrameGenFraction(FrameGenSynthesisMode::kInterpolate, 10, 10, 10) == 1.0f,
           "Frame Generation fractions should clamp hitches and degenerate intervals");

    // Relative rotation: identity for equal poses; a yaw maps newest forward
    // into the previous view by the yaw difference.
    const float half = 0.05f;
    const auto same = depthxr::FrameGenRelativeRotation({0, std::sin(half), 0, std::cos(half)}, {0, std::sin(half), 0, std::cos(half)});
    Expect(std::abs(same[0][0] - 1) < 1e-5f && std::abs(same[0][2]) < 1e-5f && std::abs(same[2][2] - 1) < 1e-5f,
           "Equal orientations should give an identity rotation");
    const auto yawed = depthxr::FrameGenRelativeRotation({0, std::sin(half), 0, std::cos(half)}, {0, 0, 0, 1});
    // Newest forward (0,0,-1) expressed in the previous view: rotated by +0.1 rad about +Y.
    const float x = -yawed[0][2], z = -yawed[2][2];
    Expect(std::abs(x + std::sin(2 * half)) < 1e-4f && std::abs(z + std::cos(2 * half)) < 1e-4f,
           "Relative rotation should carry the newest view's yaw into previous-view space");
}

void TestFrameGenHistoryState() {
    depthxr::FrameGenHistoryState state;
    depthxr::FrameGenViewRequest request;
    request.synthesize = true;
    request.temporal = true;
    Expect(state.Configure({1000, 1000}, 29, depthxr::FrameGenOutputKind::kSrgb8, 3) == depthxr::FrameGenHistoryState::kAll,
           "First configuration should create every texture");
    Expect(!state.Effective(request).synthesize, "No history yet: the first frame must be a still");
    state.Commit(false);
    auto effective = state.Effective(request);
    Expect(effective.synthesize && !effective.temporal, "Second frame synthesizes without temporal flow");
    state.Commit(true);
    effective = state.Effective(request);
    Expect(effective.synthesize && effective.temporal, "Third frame may reuse the previous filtered flow");
    Expect(state.Configure({1000, 1000}, 29, depthxr::FrameGenOutputKind::kSrgb8, 2) == depthxr::FrameGenHistoryState::kFiltered,
           "A flow quality change should only recreate the filtered flow");
    effective = state.Effective(request);
    Expect(effective.synthesize && !effective.temporal, "A quality change keeps history but drops temporal flow");
    Expect(state.Configure({1200, 1000}, 29, depthxr::FrameGenOutputKind::kSrgb8, 2) == depthxr::FrameGenHistoryState::kAll &&
               !state.Effective(request).synthesize,
           "A view resize should drop history");
    state.Commit(false);
    state.Invalidate();
    Expect(!state.Effective(request).synthesize, "Invalidate should drop history");
}

// The image tracker fed `frames` frames, each written by `written` (the
// image whose vectors change, kNone for none, kAll for every image alike)
// with the color and depth indices `indices` gives for the frame.
constexpr int kNoImage = -1;
constexpr int kAllImages = -2;
struct TrackerFeed {
    std::uint64_t frame{0};
    std::uint64_t handle{0x30};
    std::array<std::int32_t, 4> rect{0, 0, 500, 600};
    std::array<std::uint64_t, depthxr::kFrameGenEngineImages> hashes{1, 2, 3};
    // Production (stage 2): the census reads every image while the tracker
    // is not locked and every 30th frame, else the image it chooses.
    bool production{false};
    void Run(depthxr::FrameGenEngineImageTracker& tracker, int frames, bool informative,
             const std::function<int(std::uint64_t, std::uint32_t, std::uint32_t)>& written,
             const std::function<std::pair<std::uint32_t, std::uint32_t>(std::uint64_t)>& indices) {
        for (int i = 0; i < frames; ++i) {
            ++frame;
            const auto [color, depth] = indices(frame);
            const int image = written(frame, color, depth);
            if (image == kAllImages) {
                hashes.fill(1000 + frame);
            } else if (image >= 0) {
                hashes[static_cast<std::size_t>(image)] = 1000 * (static_cast<std::uint64_t>(image) + 1) + frame;
            }
            depthxr::FrameGenEngineTrackerFrame census;
            census.frame = frame;
            census.informative = informative;
            census.handle = handle;
            census.rect = rect;
            census.images = depthxr::kFrameGenEngineImages;
            census.hashes = hashes;
            census.color_index = color;
            census.depth_index = depth;
            if (production && tracker.Locked() && frame % 30 != 0) {
                census.censused = 1u << tracker.Image(frame, color, depth, std::nullopt).value_or(0);
            }
            tracker.Add(census);
        }
    }
};

// A color (or depth) swapchain index no image policy follows by itself.
std::uint32_t Scrambled(std::uint64_t frame, std::uint64_t salt) {
    std::uint64_t h = (frame + salt) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    return static_cast<std::uint32_t>(h % 3);
}

void TestFrameGenEngineImageTracker() {
    using depthxr::FrameGenEngineImageTracker;
    using Policy = FrameGenEngineImageTracker::Policy;
    const auto cycling = [](std::uint64_t frame) {
        const auto index = static_cast<std::uint32_t>(frame % 3);
        return std::pair<std::uint32_t, std::uint32_t>(index, index);
    };
    const auto scrambled = [](std::uint64_t frame) {
        return std::pair<std::uint32_t, std::uint32_t>(Scrambled(frame, 1), Scrambled(frame, 7));
    };
    // A constant image, whatever the color swapchain does.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kConstant && tracker.Best().parameter == 1 &&
                   tracker.Image(500, 2, 2, std::nullopt) == 1u,
               "Engine image tracker: a constant image should lock");
        Expect(tracker.RunnerUp().share <= 0.6, "Engine image tracker: the runner-up of a constant image is far behind");
    }
    // The frame's number modulo the images, against a color swapchain of two.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](std::uint64_t frame, auto, auto) { return static_cast<int>((frame + 2) % 3); },
                 [](std::uint64_t frame) {
                     const auto index = static_cast<std::uint32_t>(frame % 2);
                     return std::pair<std::uint32_t, std::uint32_t>(index, index);
                 });
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kCycle && tracker.Best().parameter == 2 &&
                   tracker.Image(601, 0, 0, std::nullopt) == static_cast<std::uint32_t>((601 + 2) % 3),
               "Engine image tracker: a cycle with the frames should lock with its phase");
    }
    // Following the color swapchain, and following the depth swapchain.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, std::uint32_t color, auto) { return static_cast<int>(color); }, scrambled);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kColor && tracker.Image(9, 2, 0, std::nullopt) == 2u,
               "Engine image tracker: an image following the color swapchain should lock to it");
    }
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, auto, std::uint32_t depth) { return static_cast<int>(depth); }, scrambled);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kDepth && tracker.Image(9, 2, 1, std::nullopt) == 1u,
               "Engine image tracker: an image following the depth swapchain should lock to it");
    }
    // A color swapchain cycling with the frames: the color and the cycle
    // predict alike, which is one policy (the color, preferred).
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, std::uint32_t color, auto) { return static_cast<int>(color); }, cycling);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kColor,
               "Engine image tracker: policies predicting alike should not keep each other from locking");
    }
    // Every image written alike: any of them.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return kAllImages; }, cycling);
        Expect(!tracker.Locked() && tracker.Best().policy == Policy::kAll && tracker.AllChanged() >= 79,
               "Engine image tracker: images written alike should read as all written");
    }
    // Nothing written: no image after 300 informative frames.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 299, true, [](auto, auto, auto) { return kNoImage; }, cycling);
        Expect(!tracker.NotWritten() && tracker.Image(1, 1, 1, std::nullopt).has_value(),
               "Engine image tracker: unwritten needs 300 informative frames");
        feed.Run(tracker, 2, true, [](auto, auto, auto) { return kNoImage; }, cycling);
        Expect(tracker.NotWritten() && tracker.Best().policy == Policy::kUnwritten &&
                   !tracker.Image(1, 1, 1, std::nullopt).has_value(),
               "Engine image tracker: images never written should read as not written");
    }
    // A still scene: frames that are not informative show nothing.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 200, false, [](auto, auto, auto) { return 0; }, cycling);
        Expect(!tracker.Locked() && tracker.WindowFrames() == 0 && tracker.Best().policy == Policy::kNone,
               "Engine image tracker: frames that are not informative should be ignored");
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kConstant && tracker.Best().parameter == 2,
               "Engine image tracker: informative frames after a still scene should lock");
    }
    // Another region, another swapchain, and mispredictions unlock.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(tracker.Locked(), "Engine image tracker: locked before the region changes");
        feed.rect = {0, 0, 400, 600};
        feed.Run(tracker, 1, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(!tracker.Locked() && tracker.WindowFrames() == 0 && tracker.Unlocks() == 1,
               "Engine image tracker: another region should unlock and start over");
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(tracker.Locked(), "Engine image tracker: locked again on the new region");
        feed.handle = 0x31;
        feed.Run(tracker, 1, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(!tracker.Locked() && tracker.WindowFrames() == 0, "Engine image tracker: another swapchain should unlock");
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 1; }, cycling);
        feed.Run(tracker, 4, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(tracker.Locked() && tracker.Mispredictions() == 4, "Engine image tracker: four mispredictions keep the lock");
        feed.Run(tracker, 1, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(!tracker.Locked() && tracker.Mispredictions() == 5, "Engine image tracker: the fifth misprediction unlocks");
        feed.Run(tracker, 200, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(tracker.Locked() && tracker.Best().parameter == 2, "Engine image tracker: the new image locks in time");
    }
    // A game following the specification: the released image.
    {
        FrameGenEngineImageTracker tracker;
        Expect(tracker.Image(1, 0, 0, 2u) == 2u, "Engine image tracker: a released image is the frame's");
    }
    // Production (stage 2): once locked, the census reads the chosen image
    // alone except every 30th frame. Those frames keep the hashes; frames
    // reading every image are scored, each image against its own last census.
    {
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.production = true;
        feed.Run(tracker, 80, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(tracker.Locked(), "Engine image tracker (production): a constant image locks while every image is read");
        const std::uint32_t window = tracker.WindowFrames();
        feed.Run(tracker, 600, true, [](auto, auto, auto) { return 1; }, cycling);
        Expect(tracker.Locked() && tracker.Mispredictions() == 0 && tracker.Best().parameter == 1,
               "Engine image tracker (production): the chosen image alone keeps the lock");
        Expect(tracker.WindowFrames() == std::min<std::uint32_t>(window + 20, 120),
               "Engine image tracker (production): only censuses of every image are scored (" +
                   std::to_string(tracker.WindowFrames()) + " from " + std::to_string(window) + ")");
        // The game moves to another image: the 30th frames find it changed
        // (since its census 30 frames before) and the chosen one not, and
        // their fifth misprediction unlocks; every image is read again, and
        // the new one locks.
        feed.Run(tracker, 129, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(tracker.Locked() && tracker.Mispredictions() == 4,
               "Engine image tracker (production): four 30th frames mispredicted keep the lock");
        feed.Run(tracker, 1, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(!tracker.Locked() && tracker.Mispredictions() == 5,
               "Engine image tracker (production): the fifth unlocks");
        feed.Run(tracker, 200, true, [](auto, auto, auto) { return 2; }, cycling);
        Expect(tracker.Locked() && tracker.Best().parameter == 2,
               "Engine image tracker (production): the new image locks once every image is read");
    }
    {
        // An image following a color swapchain that changes images at random:
        // the chosen image is a different one most frames, each read right
        // after it was written, and the 30th frames find the one written then.
        FrameGenEngineImageTracker tracker;
        TrackerFeed feed;
        feed.production = true;
        feed.Run(tracker, 80, true, [](auto, std::uint32_t color, auto) { return static_cast<int>(color); }, scrambled);
        Expect(tracker.Locked() && tracker.Best().policy == Policy::kColor,
               "Engine image tracker (production): following the color locks");
        feed.Run(tracker, 900, true, [](auto, std::uint32_t color, auto) { return static_cast<int>(color); }, scrambled);
        Expect(tracker.Locked() && tracker.Mispredictions() == 0 && tracker.Best().policy == Policy::kColor,
               "Engine image tracker (production): following the color stays locked on the chosen images' censuses");
    }
}

// Stage 2: the switch's modes, and the capture dispatch's constants for two
// eyes of one grid (the shader reads them as FG_PASS_ENGINE_CAPTURE says).
void TestFrameGenMotionCapturePlan() {
    Expect(depthxr::ParseFrameGenEngineMode("Capture") == depthxr::FrameGenEngineMode::kCapture &&
               depthxr::ParseFrameGenEngineMode("census") == depthxr::FrameGenEngineMode::kCensus &&
               depthxr::ParseFrameGenEngineMode("Observe") == depthxr::FrameGenEngineMode::kObserve &&
               depthxr::ParseFrameGenEngineMode("Assist") == depthxr::FrameGenEngineMode::kAssist &&
               depthxr::ParseFrameGenEngineMode("lead") == depthxr::FrameGenEngineMode::kLead &&
               depthxr::ParseFrameGenEngineMode("leading") == depthxr::FrameGenEngineMode::kOff,
           "Engine motion: the switch takes off, census, capture, observe, assist and lead");
    const depthxr::FrameGenExtent l1 = depthxr::FrameGenPyramid({5424, 5356})[1];
    const depthxr::FrameGenDispatch dispatch =
        depthxr::BuildFrameGenMotionCaptureDispatch(l1, {0, 0, 4339, 4285}, {16, 8, 4300, 4200}, 0, 1, 2);
    const depthxr::FrameGenConstants& c = dispatch.constants;
    Expect(dispatch.pass == depthxr::FrameGenPass::kEngineCapture && dispatch.width == 2712 && dispatch.height == 2678 &&
               dispatch.outputs[0].kind == depthxr::FrameGenTextureKind::kEngineRaw,
           "Engine motion: the capture writes the raw field on the half-resolution grid");
    Expect(c.size == std::array<std::uint32_t, 4>{2712, 2678, 4339, 4285} &&
               c.fov_current == std::array<float, 4>{0.0f, 0.0f, 16.0f, 8.0f} &&
               c.fov_previous == std::array<float, 4>{4300.0f, 4200.0f, 0.0f, 1.0f} && c.options[0] == 2,
           "Engine motion: the capture's constants carry both eyes' regions and slices");
}

// The shaders' view mapping on the CPU: the previous view and back.
void TestFrameGenViewMapping() {
    depthxr::FrameGenViewGeometry newest;
    depthxr::FrameGenViewGeometry previous;
    for (auto* view : {&newest, &previous}) {
        view->angle_left = -0.9f;
        view->angle_right = 0.8f;
        view->angle_up = 0.85f;
        view->angle_down = -0.95f;
    }
    const float yaw = 0.03f, pitch = -0.02f;
    newest.orientation = {std::sin(pitch / 2) * std::cos(yaw / 2), std::cos(pitch / 2) * std::sin(yaw / 2),
                          -std::sin(pitch / 2) * std::sin(yaw / 2), std::cos(pitch / 2) * std::cos(yaw / 2)};
    newest.position = {0.03f, -0.01f, -0.02f};
    for (const bool depth : {true, false}) {
        const auto mapping = depthxr::BuildFrameGenViewMapping(newest, previous, depth);
        double worst = 0.0;
        for (int y = 1; y < 10; ++y) {
            for (int x = 1; x < 10; ++x) {
                const std::array<float, 2> uv{x / 10.0f, y / 10.0f};
                const float z = 0.4f + 0.3f * static_cast<float>(x * y);
                std::array<float, 2> back{};
                std::array<float, 2> again{};
                Expect(depthxr::FrameGenMapToPrevious(mapping, uv, z, back),
                       "View mapping: a central point should be seen in the previous view");
                Expect(depthxr::FrameGenMapToNewest(mapping, back, z, again), "View mapping: and map back");
                worst = (std::max)({worst, std::abs(static_cast<double>(again[0]) - uv[0]),
                                    std::abs(static_cast<double>(again[1]) - uv[1])});
            }
        }
        Expect(worst < 1e-4, "View mapping: MapToPrevious and MapToNewest should be inverses (" + std::to_string(worst) + ")");
    }
    // With depth, near content moves with the head's translation; far
    // content (and rotation only) does not.
    const auto with_depth = depthxr::BuildFrameGenViewMapping(newest, previous, true);
    const auto rotation_only = depthxr::BuildFrameGenViewMapping(newest, previous, false);
    std::array<float, 2> close_by{}, distant{}, rotated{};
    depthxr::FrameGenMapToPrevious(with_depth, {0.5f, 0.5f}, 0.5f, close_by);
    depthxr::FrameGenMapToPrevious(with_depth, {0.5f, 0.5f}, 200000.0f, distant);
    depthxr::FrameGenMapToPrevious(rotation_only, {0.5f, 0.5f}, 0.5f, rotated);
    Expect(std::abs(close_by[0] - rotated[0]) > 1e-3f && distant == rotated,
           "View mapping: translation should move near content only");
}

// Engine motion calibration on synthetic vectors: each convention's gains
// and camera model are found from the samples alone.
void TestFrameGenEngineCalibration() {
    using depthxr::FrameGenEngineCamera;
    const depthxr::FrameGenExtent vectors{4339, 4285};
    const depthxr::FrameGenExtent color{5424, 5356};
    auto pose = [](float yaw, float pitch, std::array<float, 3> position) {
        depthxr::FrameGenViewGeometry view;
        view.angle_left = -0.95f;
        view.angle_right = 0.8f;
        view.angle_up = 0.85f;
        view.angle_down = -0.9f;
        view.orientation = {std::sin(pitch / 2) * std::cos(yaw / 2), std::cos(pitch / 2) * std::sin(yaw / 2),
                            -std::sin(pitch / 2) * std::sin(yaw / 2), std::cos(pitch / 2) * std::cos(yaw / 2)};
        view.position = position;
        return view;
    };
    // Three frames of a head turning and leaning, the aircraft flying on
    // and turning gently (appSpaceDeltaPose).
    const std::array<depthxr::FrameGenViewGeometry, 4> heads{
        pose(0.30f, 0.02f, {0.00f, 1.20f, 0.00f}), pose(0.32f, 0.01f, {0.02f, 1.21f, -0.01f}),
        pose(0.35f, 0.03f, {0.03f, 1.19f, -0.03f}), pose(0.37f, 0.00f, {0.05f, 1.20f, -0.02f})};
    const float turn = 0.01f; // radians per frame about +Y
    const std::array<float, 4> delta{0.0f, std::sin(turn / 2), 0.0f, std::cos(turn / 2)};
    const std::array<float, 3> delta_position{0.1f, -0.2f, 2.5f};
    struct Truth {
        FrameGenEngineCamera camera;
        std::array<double, 2> gain;
    };
    std::vector<Truth> truths;
    for (const FrameGenEngineCamera camera : {FrameGenEngineCamera::kIncluded, FrameGenEngineCamera::kExcluded}) {
        for (const std::array<double, 2> gain : {std::array<double, 2>{1.0, 1.0}, std::array<double, 2>{1.0, -1.0},
                                                 std::array<double, 2>{2.0, -2.0},
                                                 std::array<double, 2>{2.0 / 4339.0, -2.0 / 4285.0}}) {
            truths.push_back({camera, gain});
            truths.push_back({camera, {-gain[0], -gain[1]}});
        }
    }
    for (const Truth& truth : truths) {
        std::vector<depthxr::FrameGenEngineCalibrationFrame> frames;
        for (std::size_t k = 2; k < heads.size(); ++k) {
            depthxr::FrameGenEngineCalibrationFrame frame;
            frame.newest = heads[k];
            frame.previous = heads[k - 1];
            frame.before_previous = heads[k - 2];
            frame.delta_orientation = delta;
            frame.delta_position = delta_position;
            frame.vectors = vectors;
            frame.color = color;
            for (int y = 0; y < 24; ++y) {
                for (int x = 0; x < 24; ++x) {
                    depthxr::FrameGenEngineCalibrationSample sample;
                    sample.uv = {(x + 0.5f) / 24.0f, (y + 0.5f) / 24.0f};
                    // The lower half a cockpit (0.5-3 m), the upper half
                    // world (40 m to 4 km) and sky.
                    const int cell = (x * 7 + y * 13) % 17;
                    sample.depth = y >= 12 ? 0.5f + 0.15f * static_cast<float>(cell)
                                           : (cell == 0 ? 0.0f : 40.0f * std::pow(1.33f, static_cast<float>(cell)));
                    std::array<double, 2> q{};
                    if (!depthxr::FrameGenEnginePrediction(frame.newest, frame.previous, delta, delta_position, 1,
                                                           truth.camera, sample.uv, sample.depth, q)) {
                        continue;
                    }
                    sample.vector = {static_cast<float>(q[0] / truth.gain[0]), static_cast<float>(q[1] / truth.gain[1])};
                    frame.samples.push_back(sample);
                }
            }
            frames.push_back(frame);
        }
        const auto calibration = depthxr::CalibrateFrameGenEngine(frames);
        const auto& best = calibration.leaderboard.front();
        const std::string what = std::string(depthxr::ToString(truth.camera)) + " gain " + std::to_string(truth.gain[0]) +
                                 "," + std::to_string(truth.gain[1]);
        Expect(best.camera == truth.camera && best.delta_sign == 1,
               "Engine calibration: the camera model should be found (" + what + ": found " +
                   depthxr::ToString(best.camera) + ", sign " + std::to_string(best.delta_sign) + ")");
        Expect(std::abs(best.gain[0] / truth.gain[0] - 1.0) < 0.01 && std::abs(best.gain[1] / truth.gain[1] - 1.0) < 0.01,
               "Engine calibration: the gains should be found within 1% (" + what + ": found " +
                   std::to_string(best.gain[0]) + "," + std::to_string(best.gain[1]) + ")");
        Expect(best.gain_error < 0.01 && std::abs(best.nearest.gain[0] - truth.gain[0]) < 1e-9,
               "Engine calibration: the nearest canonical gain should be the truth's (" + what + ")");
        if (truth.camera == FrameGenEngineCamera::kIncluded) {
            // Cockpit content: a = b = 1 / g, nothing left at the intercept.
            for (int axis = 0; axis < 2; ++axis) {
                const auto& fit = calibration.cockpit[axis];
                Expect(fit.valid && std::abs(fit.a * truth.gain[axis] - 1.0) < 0.01 && std::abs(fit.b / fit.a - 1.0) < 0.01 &&
                           fit.corr2 > 0.99,
                       "Engine calibration: the cockpit fit should find a = b = 1/g (" + what + ")");
            }
            Expect(calibration.intercept_spread_px && (*calibration.intercept_spread_px)[0] < 0.01 &&
                       (*calibration.intercept_spread_px)[1] < 0.01,
                   "Engine calibration: no jitter in exact vectors (" + what + ")");
        }
    }

    // Jitter: vectors in the vectors' pixels, four frames of one head
    // movement offset by +-0.5 pixels on x and +-0.25 on y in turn, found in
    // vector pixels; with the head still, no gain, and the offsets in the
    // vectors' own units.
    for (const bool still : {false, true}) {
        const std::array<double, 2> gain{2.0 / 4339.0, -2.0 / 4285.0};
        std::vector<depthxr::FrameGenEngineCalibrationFrame> frames;
        for (std::size_t k = 0; k < 4; ++k) {
            depthxr::FrameGenEngineCalibrationFrame frame;
            frame.newest = heads[still ? 0 : 2];
            frame.previous = heads[still ? 0 : 1];
            frame.vectors = vectors;
            frame.color = color;
            const double sign = k % 2 ? -1.0 : 1.0;
            for (int y = 0; y < 24; ++y) {
                for (int x = 0; x < 24; ++x) {
                    depthxr::FrameGenEngineCalibrationSample sample;
                    sample.uv = {(x + 0.5f) / 24.0f, (y + 0.5f) / 24.0f};
                    sample.depth = 0.5f + 0.15f * static_cast<float>((x * 7 + y * 13) % 17);
                    std::array<double, 2> q{};
                    if (!depthxr::FrameGenEnginePrediction(frame.newest, frame.previous, delta, delta_position, 1,
                                                           FrameGenEngineCamera::kIncluded, sample.uv, sample.depth, q)) {
                        continue;
                    }
                    sample.vector = {static_cast<float>(q[0] / gain[0] + sign * 0.5),
                                     static_cast<float>(q[1] / gain[1] + sign * 0.25)};
                    frame.samples.push_back(sample);
                }
            }
            frames.push_back(frame);
        }
        // +0.5, -0.5, +0.5, -0.5: a spread of sqrt(1/3).
        const double x_spread = std::sqrt(1.0 / 3.0);
        const double y_spread = 0.5 * std::sqrt(1.0 / 3.0);
        const auto calibration = depthxr::CalibrateFrameGenEngine(frames);
        if (still) {
            Expect(!calibration.intercept_spread_px && std::abs(calibration.intercept_spread[0] - x_spread) < 0.01 &&
                       std::abs(calibration.intercept_spread[1] - y_spread) < 0.01,
                   "Engine calibration: with the head still, the jitter is in the vectors' units");
        } else {
            Expect(calibration.intercept_spread_px &&
                       std::abs((*calibration.intercept_spread_px)[0] - x_spread) < 0.01 &&
                       std::abs((*calibration.intercept_spread_px)[1] - y_spread) < 0.01,
                   "Engine calibration: the jitter is found in vector pixels (" +
                       (calibration.intercept_spread_px ? std::to_string((*calibration.intercept_spread_px)[0]) + ", " +
                                                              std::to_string((*calibration.intercept_spread_px)[1])
                                                        : std::string("none")) +
                       ")");
        }
    }
}

// The frame gates (stage 1): a frame and the one generated from before it,
// paired, then each gate on its own.
void TestFrameGenEngineGates() {
    using namespace depthxr;
    Expect(std::string(FrameGenEngineGateName(0)) == "noStruct" && std::string(FrameGenEngineGateName(2)) == "pairing" &&
               std::string(FrameGenEngineGateName(7)) == "uncaptured" &&
               std::string(FrameGenEngineGateName(kFrameGenEngineGateCount - 1)) == "oneEye" &&
               (1u << (kFrameGenEngineGateCount - 1)) == kFrameGenEngineOneEye,
           "Engine gates: the summary names each gate");
    const FrameGenDepthEncoding flight{0.0f, 1.0f, 356700.0f, 0.05f};
    auto record = [&](std::int64_t time, std::uint64_t projection_frame, std::uint64_t sequence) {
        FrameGenEngineGateRecord frame;
        frame.display_time = time;
        frame.projection_frame = projection_frame;
        frame.complete = true;
        frame.sequence = sequence;
        for (std::uint32_t v = 0; v < 2; ++v) {
            FrameGenEngineGateView view;
            view.view = v;
            view.vectors = 0x31;
            view.rect = {0, 0, 4339, 4285};
            view.readable = true;
            view.depth = flight;
            frame.views.push_back(view);
        }
        return frame;
    };
    const FrameGenEngineGateRecord before = record(1000, 41, 7);
    const FrameGenEngineGateRecord now = record(1011, 42, 0);
    FrameGenEngineGateFacts good;
    good.views = 2;
    good.end_time = 1011;
    good.previous_sequence = 7;
    good.continuous = true;
    good.decoded = {flight, flight};
    good.frames_since_swapchain = 500;
    good.seconds_since_swapchain = 60.0;
    good.captured = true;
    good.just_in_time = true;
    Expect(FrameGenEngineGates(&now, &before, good) == 0, "Engine gates: a frame paired with the one before passes");

    // G1, G2: no structure the vectors can be read by.
    Expect(FrameGenEngineGates(nullptr, &before, good) == kFrameGenEngineNoStruct,
           "Engine gates: a frame without a record has no structure");
    FrameGenEngineGateRecord other = now;
    other.display_time = 1022;
    Expect(FrameGenEngineGates(&other, &before, good) == kFrameGenEngineNoStruct,
           "Engine gates: another frame's record is not this frame's");
    FrameGenEngineGateRecord partial = now;
    partial.complete = false;
    Expect(FrameGenEngineGates(&partial, &before, good) == kFrameGenEngineNoStruct,
           "Engine gates: a structure on one view only");
    FrameGenEngineGateRecord unreadable = now;
    unreadable.views[1].readable = false;
    Expect(FrameGenEngineGates(&unreadable, &before, good) == kFrameGenEngineNoStruct,
           "Engine gates: an unreadable structure");

    // G4: FRAME_SKIP.
    FrameGenEngineGateRecord skipped = now;
    skipped.views[0].frame_skip = true;
    Expect(FrameGenEngineGates(&skipped, &before, good) == kFrameGenEngineSkip, "Engine gates: FRAME_SKIP holds it back");

    // G3, G5: the pair.
    Expect(FrameGenEngineGates(&now, nullptr, good) == kFrameGenEnginePairing, "Engine gates: no record before");
    // An abandoned frame between: its record (never generated from) is the
    // one before, or the record before it is no longer the frame generated
    // from last (the presenter forgot it with the abandoned frame).
    const FrameGenEngineGateRecord abandoned = record(1005, 41, 0);
    Expect(FrameGenEngineGates(&now, &abandoned, good) == kFrameGenEnginePairing,
           "Engine gates: an abandoned frame's record pairs with nothing");
    FrameGenEngineGateFacts forgotten = good;
    forgotten.previous_sequence = 0;
    forgotten.continuous = false;
    Expect(FrameGenEngineGates(&now, &before, forgotten) == kFrameGenEnginePairing,
           "Engine gates: an abandoned frame between breaks the pair");
    // A frame without a projection between: its record (no number, no
    // views) is the one before.
    FrameGenEngineGateRecord empty;
    empty.display_time = 1005;
    Expect(FrameGenEngineGates(&now, &empty, good) == kFrameGenEnginePairing,
           "Engine gates: a frame without a projection between breaks the pair");
    FrameGenEngineGateRecord gap = before;
    gap.projection_frame = 40;
    Expect(FrameGenEngineGates(&now, &gap, good) == kFrameGenEnginePairing,
           "Engine gates: projection frames must follow each other");
    FrameGenEngineGateRecord swapped = before;
    swapped.views[1].vectors = 0x32;
    Expect(FrameGenEngineGates(&now, &swapped, good) == kFrameGenEnginePairing,
           "Engine gates: other motion vector swapchains break the pair");
    FrameGenEngineGateRecord moved = before;
    moved.views[0].rect = {0, 0, 4000, 4285};
    Expect(FrameGenEngineGates(&now, &moved, good) == kFrameGenEnginePairing,
           "Engine gates: another region breaks the pair (G3)");
    FrameGenEngineGateFacts paused = good;
    paused.paused = true;
    Expect(FrameGenEngineGates(&now, &before, paused) == kFrameGenEnginePairing, "Engine gates: a pause breaks the pair");
    FrameGenEngineGateFacts discontinuous = good;
    discontinuous.continuous = false;
    Expect(FrameGenEngineGates(&now, &before, discontinuous) == kFrameGenEnginePairing,
           "Engine gates: a frame not continuing the one before breaks the pair");

    // G6.
    FrameGenEngineGateRecord cut = now;
    cut.cut = true;
    Expect(FrameGenEngineGates(&cut, &before, good) == kFrameGenEngineCut, "Engine gates: a camera cut");

    // G7: nearZ changes (MSFS's menus to flight). The depth captured at a
    // frame's release is decoded as the frame before described its own: the
    // first frame after the change, and only it, is held back.
    {
        const FrameGenDepthEncoding menus{0.0f, 1.0f, 1000.0f, 0.05f};
        const std::array<FrameGenDepthEncoding, 6> described{menus, menus, flight, flight, flight, menus};
        for (std::size_t f = 1; f < described.size(); ++f) {
            FrameGenEngineGateRecord previous = record(1000 + 11 * static_cast<std::int64_t>(f - 1), 40 + f, f);
            FrameGenEngineGateRecord frame = record(1000 + 11 * static_cast<std::int64_t>(f), 41 + f, 0);
            for (FrameGenEngineGateView& view : frame.views) view.depth = described[f];
            FrameGenEngineGateFacts facts = good;
            facts.end_time = frame.display_time;
            facts.previous_sequence = f;
            facts.decoded = {described[f - 1], described[f - 1]};
            const bool flipped = !SameFrameGenDepthDescription(described[f], described[f - 1]);
            Expect(FrameGenEngineGates(&frame, &previous, facts) == (flipped ? kFrameGenEngineDecode : 0u),
                   "Engine gates: depth decoded as the frame before described it only on the first frame after a change "
                   "(frame " + std::to_string(f) + ")");
        }
        FrameGenEngineGateFacts undecoded = good;
        undecoded.decoded = {std::nullopt, std::nullopt};
        Expect(FrameGenEngineGates(&now, &before, undecoded) == 0, "Engine gates: no depth captured, nothing decoded wrongly");
    }

    // G8: a new swapchain, then settling.
    for (std::uint64_t frames = 0; frames < 3; ++frames) {
        FrameGenEngineGateFacts facts = good;
        facts.frames_since_swapchain = frames;
        Expect(FrameGenEngineGates(&now, &before, facts) == (frames < 2 ? kFrameGenEngineRecreate : 0u),
               "Engine gates: a swapchain made in the last 2 frames (" + std::to_string(frames) + ")");
    }
    FrameGenEngineGateFacts settling = good;
    settling.seconds_since_swapchain = 2.9;
    Expect(FrameGenEngineGates(&now, &before, settling) == kFrameGenEngineSettle, "Engine gates: settling for 3 s");
    settling.seconds_since_swapchain = 3.0;
    Expect(FrameGenEngineGates(&now, &before, settling) == 0, "Engine gates: settled after 3 s");

    // G9: frames the engine path serves.
    for (int i = 0; i < 3; ++i) {
        FrameGenEngineGateFacts facts = good;
        if (i == 0) facts.captured = false;
        if (i == 1) facts.just_in_time = false;
        if (i == 2) facts.composed = true;
        Expect(FrameGenEngineGates(&now, &before, facts) == kFrameGenEngineUncaptured,
               "Engine gates: uncaptured, not just in time or quad views (" + std::to_string(i) + ")");
    }
    // G10: one eye's vectors alone.
    FrameGenEngineGateFacts one_eye = good;
    one_eye.one_eye = true;
    Expect(FrameGenEngineGates(&now, &before, one_eye) == kFrameGenEngineOneEye, "Engine gates: one eye's vectors alone");
    // Gates that need no record still count without one.
    FrameGenEngineGateFacts fresh = good;
    fresh.frames_since_swapchain = 0;
    fresh.seconds_since_swapchain = 0.0;
    fresh.captured = false;
    Expect(FrameGenEngineGates(nullptr, nullptr, fresh) ==
               (kFrameGenEngineNoStruct | kFrameGenEngineRecreate | kFrameGenEngineSettle | kFrameGenEngineUncaptured),
           "Engine gates: without a record, the gates that need none still count");
}

// G6's cut detection: a floor of 50 m while the median of the frames
// before is small or unknown, 8 times it beyond, and 10 degrees.
void TestFrameGenEngineCuts() {
    using depthxr::FrameGenEngineCuts;
    const std::array<float, 4> still{0.0f, 0.0f, 0.0f, 1.0f};
    auto ahead = [](float meters) { return std::array<float, 3>{0.0f, 0.0f, meters}; };
    auto yaw = [](float degrees) {
        const float half = degrees * 3.14159265f / 360.0f;
        return std::array<float, 4>{0.0f, std::sin(half), 0.0f, std::cos(half)};
    };
    {
        FrameGenEngineCuts cuts;
        Expect(cuts.Median() == 0.0, "Engine cuts: no median before any frame");
        Expect(cuts.Add(still, ahead(51.0f)), "Engine cuts: warming up, past 50 m is a cut");
        cuts.Reset();
        Expect(!cuts.Add(still, ahead(49.0f)) && cuts.Median() == 49.0, "Engine cuts: warming up, 49 m is not");
    }
    {
        // Flight: 3 m a frame keeps the 50 m floor.
        FrameGenEngineCuts cuts;
        for (int i = 0; i < 30; ++i) Expect(!cuts.Add(still, ahead(3.0f)), "Engine cuts: steady movement is not a cut");
        Expect(!cuts.Add(still, ahead(49.0f)) && cuts.Add(still, ahead(51.0f)),
               "Engine cuts: 50 m holds while the movement is small");
    }
    {
        // Faster: 10 m a frame, 81 m.
        FrameGenEngineCuts cuts;
        for (int i = 0; i < 30; ++i) cuts.Add(still, ahead(-10.0f));
        Expect(std::abs(cuts.Median() - 10.0) < 1e-6, "Engine cuts: the median movement");
        Expect(!cuts.Add(still, ahead(80.0f)) && cuts.Add(still, ahead(82.0f)) && cuts.Add(still, ahead(1000.0f)),
               "Engine cuts: 8 times the median movement and a meter");
        Expect(std::abs(cuts.Median() - 10.0) < 1e-6, "Engine cuts: a few cuts move the median nowhere");
        // The median is the last 30 frames'.
        for (int i = 0; i < 30; ++i) cuts.Add(still, ahead(1.0f));
        Expect(std::abs(cuts.Median() - 1.0) < 1e-6 && cuts.Add(still, ahead(60.0f)),
               "Engine cuts: the median of the last 30 frames");
    }
    {
        // A game that speeds up for good (MSFS's sim rate at 16x, slew): its
        // first frames at the new speed are cuts, then the speed is its own.
        FrameGenEngineCuts cuts;
        for (int i = 0; i < 30; ++i) cuts.Add(still, ahead(3.0f));
        int cut = 0;
        int last = -1;
        for (int i = 0; i < 40; ++i) {
            if (cuts.Add(still, ahead(60.0f))) {
                ++cut;
                last = i;
            }
        }
        Expect(cut == 15 && last == 14 && std::abs(cuts.Median() - 60.0) < 1e-6,
               "Engine cuts: a lasting new speed is cut for 15 frames, not for good (" + std::to_string(cut) + " cuts)");
    }
    {
        FrameGenEngineCuts cuts;
        Expect(!cuts.Add(yaw(9.0f), ahead(1.0f)) && cuts.Add(yaw(11.0f), ahead(1.0f)) && cuts.Add(yaw(-11.0f), ahead(0.0f)),
               "Engine cuts: turning past 10 degrees in a frame is a cut");
        Expect(cuts.Add(still, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}),
               "Engine cuts: a delta not finite is a cut");
        Expect(cuts.Add({std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f}, ahead(1.0f)) &&
                   cuts.Add({0.0f, std::numeric_limits<float>::infinity(), 0.0f, 1.0f}, ahead(1.0f)),
               "Engine cuts: a rotation not finite is a cut");
        Expect(!cuts.Add({0.0f, 0.0f, 0.0f, 0.0f}, ahead(0.0f)), "Engine cuts: an unset rotation turns nothing");
        cuts.Reset();
        Expect(cuts.Median() == 0.0, "Engine cuts: reset");
    }
}

// Pivot's change to the views, from the pose the game rendered with and the
// pose declared, and whether it moved between frames.
void TestFrameGenDeclaredChange() {
    using namespace depthxr;
    using Quaternion = std::array<double, 4>;
    auto multiply = [](const Quaternion& a, const Quaternion& b) {
        return Quaternion{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    auto turn = [](double degrees, std::array<double, 3> axis) {
        const double half = degrees * 3.14159265358979 / 360.0;
        const double length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        return Quaternion{axis[0] / length * std::sin(half), axis[1] / length * std::sin(half),
                          axis[2] / length * std::sin(half), std::cos(half)};
    };
    // declared = change * game, as Pivot applies its reverse delta.
    auto declare = [&](const FrameGenViewGeometry& game, const Quaternion& q, std::array<double, 3> p) {
        const Quaternion g{game.orientation[0], game.orientation[1], game.orientation[2], game.orientation[3]};
        const Quaternion d = multiply(q, g);
        const Quaternion rotated = multiply(multiply(q, {game.position[0], game.position[1], game.position[2], 0.0}),
                                            {-q[0], -q[1], -q[2], q[3]});
        FrameGenViewGeometry declared = game;
        declared.orientation = {static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]),
                                static_cast<float>(d[3])};
        for (int i = 0; i < 3; ++i) declared.position[i] = static_cast<float>(rotated[i] + p[i]);
        return declared;
    };
    auto game = [&](double degrees, std::array<float, 3> position) {
        FrameGenViewGeometry view;
        const Quaternion q = turn(degrees, {0.2, 1.0, 0.1});
        view.orientation = {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]),
                            static_cast<float>(q[3])};
        view.position = position;
        return view;
    };
    const Quaternion pivot = turn(25.0, {0.0, 1.0, 0.0});
    const std::array<double, 3> offset{0.3, -0.1, 0.7};
    const FrameGenViewGeometry first = game(10.0, {0.1f, 1.2f, -0.3f});
    const FrameGenViewGeometry second = game(14.0, {0.12f, 1.21f, -0.31f});
    const FrameGenPoseChange a = FrameGenDeclaredChange(first, declare(first, pivot, offset));
    double error = 0.0;
    for (int i = 0; i < 4; ++i) error = (std::max)(error, std::abs(a.orientation[i] - pivot[i]));
    for (int i = 0; i < 3; ++i) error = (std::max)(error, std::abs(a.position[i] - offset[i]));
    Expect(error < 1e-5, "Declared change: Pivot's reverse delta is found (" + std::to_string(error) + ")");
    Expect(!FrameGenDeclaredChangeMoved(a, FrameGenDeclaredChange(second, declare(second, pivot, offset))),
           "Declared change: the same change with the head moved has not moved");
    Expect(!FrameGenDeclaredChangeMoved(FrameGenDeclaredChange(first, first), FrameGenDeclaredChange(second, second)),
           "Declared change: without Pivot nothing moves");
    Expect(FrameGenDeclaredChangeMoved(a, FrameGenDeclaredChange(second, declare(second, multiply(turn(0.1, {1.0, 0.0, 0.0}), pivot), offset))) &&
               !FrameGenDeclaredChangeMoved(
                   a, FrameGenDeclaredChange(second, declare(second, multiply(turn(0.02, {1.0, 0.0, 0.0}), pivot), offset))),
           "Declared change: turning by 0.1 degrees moves it, 0.02 degrees does not");
    Expect(FrameGenDeclaredChangeMoved(a, FrameGenDeclaredChange(second, declare(second, pivot, {0.3, -0.1, 0.701}))) &&
               !FrameGenDeclaredChangeMoved(
                   a, FrameGenDeclaredChange(second, declare(second, pivot, {0.3, -0.1, 0.7002}))),
           "Declared change: 1 mm moves it, 0.2 mm does not");
}

// Engine motion vectors, stage 3: the convention switch and decode, and the
// evidence passes in a plan (after the filter, writing nothing else; none
// without engine_motion, in capture, or in Lead); the hardware level.
void TestFrameGenEngineEvidencePlan() {
    using depthxr::FrameGenEngineCamera;
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    const auto fixed = depthxr::ParseFrameGenEngineConvention("1, -1, excluded,jitter");
    Expect(fixed && fixed->gain == std::array<float, 2>{1.0f, -1.0f} && fixed->camera == FrameGenEngineCamera::kExcluded &&
               fixed->jitter,
           "Engine evidence: a fixed convention parses");
    Expect(!depthxr::ParseFrameGenEngineConvention("1,1") && !depthxr::ParseFrameGenEngineConvention("0,1,included") &&
               !depthxr::ParseFrameGenEngineConvention("1,1,sideways") &&
               depthxr::ParseFrameGenEngineConvention("2,-2,rotation") &&
               depthxr::ParseFrameGenEngineConvention("2,-2,rotation")->camera == FrameGenEngineCamera::kRotationOnly,
           "Engine evidence: conventions that are none are refused");
    // FB: A = g / 2 (y negated), no offset; EXT: its scale and offset as well.
    depthxr::FrameGenEngineConvention convention;
    convention.gain = {2.0f, -2.0f};
    depthxr::FrameGenMotionEncoding fb;
    Expect(depthxr::FrameGenEngineDecode(convention, fb) == std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f},
           "Engine evidence: uv vectors decode as they are (y flipped twice)");
    depthxr::FrameGenMotionEncoding ext;
    ext.ext = true;
    ext.scale = {0.5f, 0.25f, 1.0f};
    ext.offset = {0.1f, -0.2f, 0.0f};
    Expect(depthxr::FrameGenEngineDecode(convention, ext) == std::array<float, 4>{0.5f, 0.25f, 0.1f, -0.2f},
           "Engine evidence: EXT's scale and offset go into the decode");
    // The convention's own offset comes out of e.
    convention.offset = {0.25f, -0.5f};
    Expect(depthxr::FrameGenEngineDecode(convention, fb) == std::array<float, 4>{1.0f, 1.0f, -0.25f, 0.5f},
           "Engine evidence: the convention's offset goes into the decode");
    convention.offset = {0.0f, 0.0f};

    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = true;
    request.depth = true;
    request.newest.angle_left = request.previous.angle_left = -0.9f;
    request.newest.angle_right = request.previous.angle_right = 0.9f;
    request.newest.angle_up = request.previous.angle_up = 0.9f;
    request.newest.angle_down = request.previous.angle_down = -0.9f;
    request.newest.orientation = {0.0f, 0.01f, 0.0f, 0.99995f};
    depthxr::FrameGenTuning tuning;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.analyze_only = true;
    tuning.engine_mode = depthxr::FrameGenEngineMode::kObserve;
    tuning.engine = convention;
    const depthxr::FrameGenViewPlan plain = depthxr::BuildFrameGenViewPlan(request, tuning);
    request.engine_motion = true;
    request.engine_images = {2, 1};
    request.engine_vectors = {800, 790};
    const depthxr::FrameGenViewPlan observed = depthxr::BuildFrameGenViewPlan(request, tuning);
    // The plan without the evidence passes is the plan without engine motion.
    std::vector<depthxr::FrameGenDispatch> rest;
    std::vector<depthxr::FrameGenDispatch> engine;
    for (const auto& dispatch : observed.dispatches) {
        (dispatch.pass == FrameGenPass::kEngineReduce || dispatch.pass == FrameGenPass::kEngineSolve ? engine : rest)
            .push_back(dispatch);
    }
    bool same = rest.size() == plain.dispatches.size();
    for (std::size_t i = 0; same && i < rest.size(); ++i) {
        // FrameGenTexture is two bytes, the constants 32 words: compared as bytes.
        same = rest[i].pass == plain.dispatches[i].pass &&
               std::memcmp(rest[i].inputs.data(), plain.dispatches[i].inputs.data(), sizeof(rest[i].inputs)) == 0 &&
               std::memcmp(rest[i].outputs.data(), plain.dispatches[i].outputs.data(), sizeof(rest[i].outputs)) == 0 &&
               std::memcmp(&rest[i].constants, &plain.dispatches[i].constants, sizeof(depthxr::FrameGenConstants)) == 0;
    }
    Expect(same, "Engine evidence: everything else in the plan is as without engine motion");
    // Each reduce by the kinds it sums (1 cockpit, 2 photometric, 4 lag, 8
    // the cockpit against the pair before), each solve by its stage.
    auto kinds = [](const std::vector<depthxr::FrameGenDispatch>& dispatches) {
        std::string order;
        for (const auto& dispatch : dispatches) {
            order += dispatch.pass == FrameGenPass::kEngineSolve ? "S" + std::to_string(dispatch.constants.options[0] & 15u)
                                                                 : "R" + std::to_string(dispatch.constants.options[0] & 15u);
            order += " ";
        }
        return order;
    };
    Expect(kinds(engine) == "R3 S1 ", "Engine evidence: cockpit and photometric together, solve (" + kinds(engine) + ")");
    bool writes_engine_only = true;
    for (const auto& dispatch : engine) {
        for (const auto& output : dispatch.outputs) {
            writes_engine_only = writes_engine_only && (output.kind == FrameGenTextureKind::kNone ||
                                                        output.kind == FrameGenTextureKind::kEnginePartials ||
                                                        output.kind == FrameGenTextureKind::kEngineParams ||
                                                        output.kind == FrameGenTextureKind::kEngineEvidence);
        }
    }
    Expect(writes_engine_only, "Engine evidence: the passes write their own textures alone");
    // After the filter, which the measured flow they read is final for.
    const auto filter = std::find_if(observed.dispatches.begin(), observed.dispatches.end(),
                                     [](const auto& dispatch) { return dispatch.pass == FrameGenPass::kFilter; });
    const auto first = std::find_if(observed.dispatches.begin(), observed.dispatches.end(),
                                    [](const auto& dispatch) { return dispatch.pass == FrameGenPass::kEngineReduce; });
    Expect(filter < first && engine[0].inputs[4].kind == FrameGenTextureKind::kFlow &&
               engine[0].inputs[4].level == observed.final_flow_level,
           "Engine evidence: taken after the filter, from the final level's measured flow");
    // Every kind reads the vectors in the convention under test (the
    // cockpit's sums come back to them as decoded on the CPU); the solve
    // carries the images.
    const depthxr::FrameGenConstants& reduce = engine[0].constants;
    const depthxr::FrameGenConstants& solve = engine[1].constants;
    Expect(reduce.tuning == std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f} &&
               reduce.size == std::array<std::uint32_t, 4>{128, 128, 512, 512} && engine[0].inputs[5].kind == FrameGenTextureKind::kNone &&
               solve.options[2] == (2u | (1u << 8)) && solve.size == std::array<std::uint32_t, 4>{512, 512, 800, 790} &&
               engine[1].outputs[1].kind == FrameGenTextureKind::kEngineEvidence,
           "Engine evidence: decode, grid and images");
    // The field before (the lag kind, with the others) and the pair before
    // (the cockpit lag kind, by the game's poses, alone) when the request
    // has them.
    request.engine_previous = true;
    request.engine_lag = true;
    request.game_previous.orientation = {0.0f, 0.02f, 0.0f, 0.9998f};
    request.game_before.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    const depthxr::FrameGenViewPlan lagged = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::vector<depthxr::FrameGenDispatch> lag;
    for (const auto& dispatch : lagged.dispatches) {
        if (dispatch.pass == FrameGenPass::kEngineReduce || dispatch.pass == FrameGenPass::kEngineSolve) lag.push_back(dispatch);
    }
    Expect(kinds(lag) == "R7 R8 S1 " && lag[0].inputs[5].kind == FrameGenTextureKind::kEngineRawPrevious &&
               lag[0].inputs[0].kind == FrameGenTextureKind::kEngineRaw && lag[1].constants.rot0 != lag[0].constants.rot0 &&
               lag[1].inputs[4].kind == FrameGenTextureKind::kNone && lag[1].constants.tuning == reduce.tuning &&
               (lag[2].constants.options[0] & (1u << 7)) != 0 && (lag[2].constants.options[0] & (1u << 8)) != 0,
           "Engine evidence: the field before and the pair before (" + kinds(lag) + ")");
    // A jittered convention: the cockpit (and lag) kinds first, the bias
    // from their sums (the solve's first stage), then the photometric kind
    // taking it out (bit 9, kEngineParams as its fifth input).
    convention.jitter = true;
    tuning.engine = convention;
    const depthxr::FrameGenViewPlan jittered = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::vector<depthxr::FrameGenDispatch> bias;
    for (const auto& dispatch : jittered.dispatches) {
        if (dispatch.pass == FrameGenPass::kEngineReduce || dispatch.pass == FrameGenPass::kEngineSolve) bias.push_back(dispatch);
    }
    Expect(kinds(bias) == "R5 S0 R2 R8 S1 " && bias[0].inputs[5].kind == FrameGenTextureKind::kEngineRawPrevious &&
               (bias[0].constants.options[0] & (1u << 9)) == 0 && bias[2].inputs[5].kind == FrameGenTextureKind::kEngineParams &&
               (bias[2].constants.options[0] & (1u << 9)) != 0 && (bias[4].constants.options[0] & (1u << 5)) != 0,
           "Engine evidence: a jittered convention's bias first (" + kinds(bias) + ")");
    convention.jitter = false;
    tuning.engine = convention;
    // None in capture, without engine_motion, or interpolating's cover alike.
    tuning.engine_mode = depthxr::FrameGenEngineMode::kCapture;
    Expect(depthxr::BuildFrameGenViewPlan(request, tuning).dispatches.size() == plain.dispatches.size(),
           "Engine evidence: none in capture");
    // The hardware level: the flow ends at the hardware's level.
    tuning.engine_mode = depthxr::FrameGenEngineMode::kOff;
    tuning.quality = depthxr::FrameGenFlowQuality::kQuality;
    Expect(depthxr::FrameGenFinalFlowLevel(tuning) == 2, "Engine evidence: Quality ends at level 2");
    tuning.hardware_level = true;
    Expect(depthxr::FrameGenFinalFlowLevel(tuning) == depthxr::kFrameGenHardwareFlowLevel,
           "Engine evidence: with hardware_level, at the hardware's level");
}

// Stage 5's plan: Observe's as before; Assist takes the evidence before the
// filter, then kEngineConvert, kEngineMerge and kEngineTiles, the filters
// reading the merged flow (kEngineFlow) and the world fit the measured one;
// the constants the shaders read; Interpolate capped at Assist; the
// filters' label rules and the confidence view's tint behind their switches.
void TestFrameGenEngineAssistPlan() {
    using depthxr::FrameGenEngineMode;
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    depthxr::FrameGenEngineConvention convention;
    convention.gain = {2.0f, -2.0f};
    convention.camera = depthxr::FrameGenEngineCamera::kRotationOnly;
    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = request.temporal = request.depth = true;
    request.newest.angle_left = request.previous.angle_left = -0.9f;
    request.newest.angle_right = request.previous.angle_right = 0.9f;
    request.newest.angle_up = request.previous.angle_up = 0.9f;
    request.newest.angle_down = request.previous.angle_down = -0.9f;
    request.newest.orientation = {0.0f, 0.01f, 0.0f, 0.99995f};
    request.engine_motion = true;
    request.engine_encoding.ext = true;
    request.engine_encoding.scale = {0.5f, 0.25f, 1.0f};
    request.engine_encoding.offset = {0.1f, -0.2f, 0.0f};
    request.engine_vectors = {800, 790};
    depthxr::FrameGenTuning tuning;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.analyze_only = true;
    tuning.engine_mode = FrameGenEngineMode::kObserve;
    tuning.engine = convention;
    const depthxr::FrameGenViewPlan observed = depthxr::BuildFrameGenViewPlan(request, tuning);
    tuning.engine_mode = FrameGenEngineMode::kAssist;
    const depthxr::FrameGenViewPlan assisted = depthxr::BuildFrameGenViewPlan(request, tuning);
    auto order = [](const depthxr::FrameGenViewPlan& plan) {
        std::string text;
        for (const auto& dispatch : plan.dispatches) {
            switch (dispatch.pass) {
            case FrameGenPass::kRefine: text += "refine "; break;
            case FrameGenPass::kFilter: text += "filter "; break;
            case FrameGenPass::kEngineReduce: text += "R "; break;
            case FrameGenPass::kEngineSolve: text += "S "; break;
            case FrameGenPass::kEngineConvert: text += "convert "; break;
            case FrameGenPass::kEngineMerge: text += "merge "; break;
            case FrameGenPass::kEngineTiles: text += "tiles "; break;
            case FrameGenPass::kWorldReduce: text += "W "; break;
            case FrameGenPass::kWorldSolve: text += "w "; break;
            default: break;
            }
        }
        return text;
    };
    Expect(order(observed) == "refine refine filter R S ",
           "Engine assist plan: Observe takes the evidence after the filter, as before (" + order(observed) + ")");
    Expect(order(assisted) == "refine refine R S convert merge tiles filter ",
           "Engine assist plan: Assist takes it first, then converts, merges and tiles before the filter (" +
               order(assisted) + ")");
    // Everything else as Observe's, the filter reading the merged flow.
    std::vector<depthxr::FrameGenDispatch> rest, observed_rest;
    auto engine_pass = [](FrameGenPass pass) {
        return pass == FrameGenPass::kEngineReduce || pass == FrameGenPass::kEngineSolve ||
               pass == FrameGenPass::kEngineConvert || pass == FrameGenPass::kEngineMerge || pass == FrameGenPass::kEngineTiles;
    };
    for (const auto& dispatch : assisted.dispatches) {
        if (!engine_pass(dispatch.pass)) rest.push_back(dispatch);
    }
    for (const auto& dispatch : observed.dispatches) {
        if (!engine_pass(dispatch.pass)) observed_rest.push_back(dispatch);
    }
    bool same = rest.size() == observed_rest.size();
    for (std::size_t i = 0; same && i < rest.size(); ++i) {
        depthxr::FrameGenDispatch expected = observed_rest[i];
        if (expected.pass == FrameGenPass::kFilter) expected.inputs[0] = {FrameGenTextureKind::kEngineFlow, 0};
        same = rest[i].pass == expected.pass &&
               std::memcmp(rest[i].inputs.data(), expected.inputs.data(), sizeof(rest[i].inputs)) == 0 &&
               std::memcmp(rest[i].outputs.data(), expected.outputs.data(), sizeof(rest[i].outputs)) == 0 &&
               std::memcmp(&rest[i].constants, &expected.constants, sizeof(depthxr::FrameGenConstants)) == 0;
    }
    Expect(same, "Engine assist plan: the rest is Observe's, the filter reading kEngineFlow in place of kFlow");
    auto find = [](const depthxr::FrameGenViewPlan& plan, FrameGenPass pass) {
        return *std::find_if(plan.dispatches.begin(), plan.dispatches.end(),
                             [&](const depthxr::FrameGenDispatch& dispatch) { return dispatch.pass == pass; });
    };
    const std::uint8_t level = assisted.final_flow_level;
    const depthxr::FrameGenExtent cells = assisted.levels[level];
    const depthxr::FrameGenDispatch convert = find(assisted, FrameGenPass::kEngineConvert);
    const depthxr::FrameGenDispatch merge = find(assisted, FrameGenPass::kEngineMerge);
    const depthxr::FrameGenDispatch tiles = find(assisted, FrameGenPass::kEngineTiles);
    // A and B of the convention and the extension's encoding; the camera in
    // bits 4-5; the level; the frame's verdict and bias from kEngineParams,
    // the other eye's verdict from its own.
    Expect(convert.inputs[0].kind == FrameGenTextureKind::kEngineRaw && convert.inputs[1].kind == FrameGenTextureKind::kDepth &&
               convert.inputs[2].kind == FrameGenTextureKind::kEngineParams &&
               convert.inputs[3].kind == FrameGenTextureKind::kNone && convert.inputs[4].kind == FrameGenTextureKind::kNone &&
               convert.inputs[5].kind == FrameGenTextureKind::kEngineParamsOther &&
               convert.outputs[0].kind == FrameGenTextureKind::kEngineCandidate &&
               convert.outputs[1].kind == FrameGenTextureKind::kEngineInfo &&
               convert.constants.tuning == depthxr::FrameGenEngineDecode(convention, request.engine_encoding) &&
               convert.constants.tuning == std::array<float, 4>{0.5f, 0.25f, 0.1f, -0.2f} &&
               convert.constants.options[0] == 1u << 4 && convert.constants.options[3] == level &&
               convert.constants.size == std::array<std::uint32_t, 4>{cells.width, cells.height, 512, 512} &&
               convert.width == cells.width && convert.height == cells.height &&
               (convert.constants.options[1] & depthxr::kFrameGenFlagDepth) != 0,
           "Engine assist plan: kEngineConvert's inputs, decode (A, B), camera bits and level");
    // REFINE's block at its match level: the scale in options.w.
    const std::uint8_t match = depthxr::FrameGenMatchLevel(level);
    Expect(level == 3 && match == 1 && merge.inputs[0].kind == FrameGenTextureKind::kLumaNewest && merge.inputs[0].level == match &&
               merge.inputs[1].kind == FrameGenTextureKind::kLumaPrevious && merge.inputs[1].level == match &&
               merge.inputs[2].kind == FrameGenTextureKind::kEngineCandidate &&
               merge.inputs[3].kind == FrameGenTextureKind::kEngineInfo &&
               merge.inputs[4].kind == FrameGenTextureKind::kFlow && merge.inputs[4].level == level &&
               merge.inputs[5].kind == FrameGenTextureKind::kNone &&
               merge.outputs[0].kind == FrameGenTextureKind::kEngineFlow &&
               merge.outputs[1].kind == FrameGenTextureKind::kEngineLabels &&
               merge.constants.options[0] == depthxr::kFrameGenEngineMergeAssist && merge.constants.options[2] == level &&
               merge.constants.options[3] == 4u,
           "Engine assist plan: kEngineMerge reads REFINE's luma and the measured flow, its block scale 4 at level 3");
    tuning.quality = depthxr::FrameGenFlowQuality::kQuality;
    Expect(find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kEngineMerge).constants.options[3] == 2u,
           "Engine assist plan: scale 2 at level 2");
    tuning.quality = depthxr::FrameGenFlowQuality::kPerformance;
    const depthxr::FrameGenDispatch coarse = find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kEngineMerge);
    Expect(coarse.constants.options[3] == 4u && coarse.inputs[0].level == 2, "Engine assist plan: level 4 matches at level 2");
    tuning.quality = depthxr::FrameGenFlowQuality::kBalanced;
    // The tiles: a thread each, the head's turn (degrees) in tuning.y.
    Expect(tiles.inputs[0].kind == FrameGenTextureKind::kEngineLabels && tiles.inputs[1].kind == FrameGenTextureKind::kNone &&
               tiles.outputs[0].kind == FrameGenTextureKind::kEngineTiles &&
               tiles.width == (cells.width + 7) / 8 && tiles.height == (cells.height + 7) / 8 &&
               tiles.constants.options[0] == 0u && std::abs(tiles.constants.tuning[1] - 1.146f) < 0.002f,
           "Engine assist plan: kEngineTiles per 8x8 cells, with the head's turn per frame (" +
               std::to_string(tiles.constants.tuning[1]) + " degrees)");
    request.engine_tiles = true;
    const depthxr::FrameGenViewPlan tiled = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(find(tiled, FrameGenPass::kEngineConvert).inputs[3].kind == FrameGenTextureKind::kEngineTilesPrevious &&
               find(tiled, FrameGenPass::kEngineConvert).constants.options[0] ==
                   ((1u << 4) | depthxr::kFrameGenEngineConvertTiles) &&
               find(tiled, FrameGenPass::kEngineTiles).inputs[1].kind == FrameGenTextureKind::kEngineTilesPrevious &&
               find(tiled, FrameGenPass::kEngineTiles).constants.options[0] == 1u,
           "Engine assist plan: the frame before's tiles where the backend has them");
    request.engine_tiles = false;
    // The filters' label rules behind their switch.
    Expect(find(assisted, FrameGenPass::kFilter).inputs[4].kind == FrameGenTextureKind::kNone &&
               (find(assisted, FrameGenPass::kFilter).constants.options[1] & depthxr::kFrameGenFlagEngine) == 0,
           "Engine assist plan: no label rules without VECTORXR_FRAMEGEN_ENGINE_FILTER");
    tuning.engine_filter = true;
    const depthxr::FrameGenDispatch labelled = find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kFilter);
    tuning.engine_cockpit = true;
    const depthxr::FrameGenDispatch cockpit = find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kFilter);
    Expect(labelled.inputs[4].kind == FrameGenTextureKind::kEngineLabels &&
               (labelled.constants.options[1] & depthxr::kFrameGenFlagEngine) != 0 && labelled.constants.options[0] == 0u &&
               cockpit.constants.options[0] == 1u,
           "Engine assist plan: the labels at the filter's input 4, options.x 1 for the cockpit's movers");
    tuning.engine_filter = tuning.engine_cockpit = false;
    // With world motion: fitted (or set) before the conversion, for the
    // classes, from the measured flow; the filter then reads it as before.
    request.world_motion = true;
    const depthxr::FrameGenViewPlan world = depthxr::BuildFrameGenViewPlan(request, tuning);
    bool fit_measured = true;
    for (const auto& dispatch : world.dispatches) {
        if (dispatch.pass == FrameGenPass::kWorldReduce) {
            fit_measured = fit_measured && dispatch.inputs[0].kind == FrameGenTextureKind::kFlow;
        }
    }
    Expect(order(world) == "refine refine R S W w W w W w convert merge tiles filter " && fit_measured &&
               find(world, FrameGenPass::kEngineConvert).inputs[4].kind == FrameGenTextureKind::kWorldParams &&
               (find(world, FrameGenPass::kEngineConvert).constants.options[0] & depthxr::kFrameGenEngineConvertWorld) != 0 &&
               find(world, FrameGenPass::kFilter).inputs[3].kind == FrameGenTextureKind::kWorldParams,
           "Engine assist plan: the world fit before the conversion, from the measured flow (" + order(world) + ")");
    // Interpolating just in time: the cover filter reads the merged flow
    // too, and the world fit (the cover's) comes first; Lead is capped at
    // Assist.
    tuning.mode = depthxr::FrameGenSynthesisMode::kInterpolate;
    tuning.cover = true;
    tuning.engine_mode = FrameGenEngineMode::kLead;
    const depthxr::FrameGenViewPlan interpolated = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::vector<depthxr::FrameGenDispatch> interpolated_filters;
    for (const auto& dispatch : interpolated.dispatches) {
        if (dispatch.pass == FrameGenPass::kFilter) interpolated_filters.push_back(dispatch);
    }
    Expect(depthxr::FrameGenEngineModeFor(tuning) == FrameGenEngineMode::kAssist &&
               order(interpolated) == "refine refine R S W w W w W w convert merge tiles filter filter " &&
               find(interpolated, FrameGenPass::kEngineMerge).constants.options[0] == depthxr::kFrameGenEngineMergeAssist &&
               interpolated_filters.size() == 2 && interpolated_filters[0].inputs[0].kind == FrameGenTextureKind::kEngineFlow &&
               interpolated_filters[1].inputs[0].kind == FrameGenTextureKind::kEngineFlow &&
               interpolated_filters[1].inputs[3].kind == FrameGenTextureKind::kWorldParams &&
               interpolated_filters[0].inputs[3].kind == FrameGenTextureKind::kNone,
           "Engine assist plan: interpolating, capped at Assist, both filters read the merged flow (" +
               order(interpolated) + ")");
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.cover = false;
    // Extrapolating in Lead, Lead's rule once the convention is locked
    // (stage 6); before, Assist's plan.
    Expect(depthxr::FrameGenEngineModeFor(tuning) == FrameGenEngineMode::kLead && !depthxr::FrameGenEngineLeads(request, tuning) &&
               find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kEngineMerge).constants.options[0] ==
                   depthxr::kFrameGenEngineMergeAssist,
           "Engine assist plan: extrapolating in Lead, Assist's rule until the convention is locked");
    tuning.engine_locked = true;
    Expect(depthxr::FrameGenEngineLeads(request, tuning) &&
               find(depthxr::BuildFrameGenViewPlan(request, tuning), FrameGenPass::kEngineMerge).constants.options[0] ==
                   depthxr::kFrameGenEngineMergeLead,
           "Engine assist plan: extrapolating, Lead's rule (stage 6)");
    tuning.engine_locked = false;
    tuning.engine_mode = FrameGenEngineMode::kAssist;
    request.world_motion = false;
    // None without the evidence (a gate, a view without vectors).
    request.engine_motion = false;
    const depthxr::FrameGenViewPlan without = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(order(without) == "refine refine filter " && find(without, FrameGenPass::kFilter).inputs[0].kind == FrameGenTextureKind::kFlow,
           "Engine assist plan: none without the frame's evidence, the filter on the measured flow");
    request.engine_motion = true;
    // The confidence view's tint: the labels at synthesis's input 5, never
    // in place of the motion map.
    tuning.diagnostic = depthxr::FrameGenDiagnosticView::kConfidence;
    auto synthesis = [&](const depthxr::FrameGenTuning& with) {
        const depthxr::FrameGenViewPlan plan = depthxr::BuildFrameGenSynthesisPlan(
            request, with, depthxr::FrameGenSynthesisMode::kExtrapolate, 0.5f, false);
        return plan.dispatches.back();
    };
    Expect(synthesis(tuning).inputs[5].kind == FrameGenTextureKind::kNone, "Engine assist plan: no tint without its switch");
    tuning.engine_tint = true;
    const depthxr::FrameGenDispatch tinted = synthesis(tuning);
    Expect(tinted.inputs[5].kind == FrameGenTextureKind::kEngineLabels &&
               (tinted.constants.options[1] & depthxr::kFrameGenFlagEngine) != 0,
           "Engine assist plan: the tint reads the labels at input 5");
    tuning.depth_splat = true;
    Expect(synthesis(tuning).inputs[5].kind == FrameGenTextureKind::kMotionMap &&
               (synthesis(tuning).constants.options[1] & depthxr::kFrameGenFlagEngine) == 0,
           "Engine assist plan: the motion map keeps input 5");
    tuning.depth_splat = false;
    tuning.diagnostic = depthxr::FrameGenDiagnosticView::kOff;
    Expect(synthesis(tuning).inputs[5].kind == FrameGenTextureKind::kNone, "Engine assist plan: the tint is the confidence view's");
}

// Stage 6's plan: where the vectors lead (Lead, the convention locked, their
// evidence asked for), the evidence right after the luma without measured
// flow, the frame's verdict for both eyes (the predicate), then exactly the
// downsample, search and refine passes predicated on it (but the downsample
// to the merge's match level), never the optical flow hardware's pack and
// unpack, the merge by Lead's rule and the filter on the merged flow; the
// world's motion set before the conversion, or fitted to the merged flow
// after it. With engine_predicate false no measured pass at all. Unlocked,
// or interpolating, Assist's plan. The flow's level holds between the frames
// the vectors lead and those measured without them (hardware_level): no
// view's filtered flow is made anew.
void TestFrameGenEngineLeadPlan() {
    using depthxr::FrameGenEngineMode;
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = request.temporal = request.depth = true;
    request.newest.angle_left = request.previous.angle_left = -0.9f;
    request.newest.angle_right = request.previous.angle_right = 0.9f;
    request.newest.angle_up = request.previous.angle_up = 0.9f;
    request.newest.angle_down = request.previous.angle_down = -0.9f;
    request.newest.orientation = {0.0f, 0.01f, 0.0f, 0.99995f};
    request.engine_motion = true;
    request.engine_vectors = {800, 790};
    depthxr::FrameGenTuning tuning;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.analyze_only = true;
    tuning.engine_mode = FrameGenEngineMode::kLead;
    tuning.engine_locked = true;
    // The hardware chosen, Quality: the flow at the hardware's level.
    tuning.hardware_flow = tuning.hardware_level = true;
    tuning.quality = depthxr::FrameGenFlowQuality::kQuality;
    // The passes in order (the depth's trust passes aside), a skippable one
    // marked "?", the predicate (kEngineSolve's stage 2) as "P".
    auto order = [](const depthxr::FrameGenViewPlan& plan) {
        std::string text;
        for (const auto& dispatch : plan.dispatches) {
            std::string name;
            switch (dispatch.pass) {
            case FrameGenPass::kLuma: name = "luma"; break;
            case FrameGenPass::kFlowPack: name = "pack"; break;
            case FrameGenPass::kFlowUnpack: name = "unpack"; break;
            case FrameGenPass::kDownsample: name = "down"; break;
            case FrameGenPass::kSearch: name = "search"; break;
            case FrameGenPass::kRefine: name = "refine"; break;
            case FrameGenPass::kFilter: name = "filter"; break;
            case FrameGenPass::kEngineReduce: name = "R"; break;
            case FrameGenPass::kEngineSolve: name = (dispatch.constants.options[0] & 15u) == 2u ? "P" : "S"; break;
            case FrameGenPass::kEngineConvert: name = "convert"; break;
            case FrameGenPass::kEngineMerge: name = "merge"; break;
            case FrameGenPass::kEngineTiles: name = "tiles"; break;
            case FrameGenPass::kWorldReduce: name = "W"; break;
            case FrameGenPass::kWorldSolve: name = "w"; break;
            case FrameGenPass::kWorldSet: name = "set"; break;
            default: continue;
            }
            text += name + (dispatch.predicate == depthxr::kFrameGenPredicateSkip ? "? " : " ");
        }
        return text;
    };
    auto find = [](const depthxr::FrameGenViewPlan& plan, FrameGenPass pass) {
        return *std::find_if(plan.dispatches.begin(), plan.dispatches.end(),
                             [&](const depthxr::FrameGenDispatch& dispatch) { return dispatch.pass == pass; });
    };
    auto same = [](const depthxr::FrameGenViewPlan& a, const depthxr::FrameGenViewPlan& b) {
        bool equal = a.dispatches.size() == b.dispatches.size();
        for (std::size_t i = 0; equal && i < a.dispatches.size(); ++i) {
            equal = a.dispatches[i].pass == b.dispatches[i].pass && a.dispatches[i].predicate == b.dispatches[i].predicate &&
                    std::memcmp(a.dispatches[i].inputs.data(), b.dispatches[i].inputs.data(), sizeof(a.dispatches[i].inputs)) == 0 &&
                    std::memcmp(a.dispatches[i].outputs.data(), b.dispatches[i].outputs.data(), sizeof(a.dispatches[i].outputs)) == 0 &&
                    std::memcmp(&a.dispatches[i].constants, &b.dispatches[i].constants, sizeof(depthxr::FrameGenConstants)) == 0;
        }
        return equal;
    };
    const depthxr::FrameGenViewPlan lead = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(lead.final_flow_level == depthxr::kFrameGenHardwareFlowLevel &&
               order(lead) == "luma R S P down? down? down? down? search? refine? refine? convert merge tiles filter ",
           "Engine lead plan: the evidence and predicate after the luma, the measured passes predicated, no optical flow "
           "hardware (" + order(lead) + ")");
    std::size_t predicated = 0, set = 0;
    bool measured_only = true;
    for (const auto& dispatch : lead.dispatches) {
        predicated += dispatch.predicate == depthxr::kFrameGenPredicateSkip ? 1 : 0;
        set += dispatch.predicate == depthxr::kFrameGenPredicateSet ? 1 : 0;
        measured_only = measured_only && (dispatch.predicate != depthxr::kFrameGenPredicateSkip ||
                                          dispatch.pass == FrameGenPass::kDownsample || dispatch.pass == FrameGenPass::kSearch ||
                                          dispatch.pass == FrameGenPass::kRefine);
    }
    Expect(predicated == 7 && set == 1 && measured_only,
           "Engine lead plan: exactly the downsample, search and refine passes predicated, one predicate");
    // The evidence without measured flow (Lead's photometric test, against
    // no motion); the predicate from both eyes' verdicts into kEngineVerdict;
    // the merge by Lead's rule, the measured flow there for a frame whose
    // verdict holds the vectors back.
    const depthxr::FrameGenDispatch reduce = find(lead, FrameGenPass::kEngineReduce);
    std::vector<depthxr::FrameGenDispatch> solves;
    for (const auto& dispatch : lead.dispatches) {
        if (dispatch.pass == FrameGenPass::kEngineSolve) solves.push_back(dispatch);
    }
    const depthxr::FrameGenDispatch merge = find(lead, FrameGenPass::kEngineMerge);
    Expect(reduce.inputs[4].kind == FrameGenTextureKind::kNone && (reduce.constants.options[0] & (1u << 8)) == 0 &&
               solves.size() == 2 && (solves[0].constants.options[0] & (1u << 6)) != 0 &&
               (solves[0].constants.options[0] & (1u << 4)) != 0 &&
               solves[1].inputs[3].kind == FrameGenTextureKind::kEngineParams &&
               solves[1].inputs[5].kind == FrameGenTextureKind::kEngineParamsOther &&
               solves[1].outputs[0].kind == FrameGenTextureKind::kEngineVerdict &&
               solves[1].predicate == depthxr::kFrameGenPredicateSet && solves[1].width == 8 && solves[1].height == 8 &&
               merge.constants.options[0] == depthxr::kFrameGenEngineMergeLead &&
               merge.inputs[4].kind == FrameGenTextureKind::kFlow && merge.inputs[4].level == 3 &&
               find(lead, FrameGenPass::kFilter).inputs[0].kind == FrameGenTextureKind::kEngineFlow,
           "Engine lead plan: no measured flow in the evidence, both eyes' verdicts the predicate, Lead's merge");
    // Balanced on the shader cores alike; Performance merges on level 2's
    // luma, whose downsample runs whatever the verdict.
    tuning.hardware_flow = tuning.hardware_level = false;
    tuning.quality = depthxr::FrameGenFlowQuality::kBalanced;
    Expect(order(depthxr::BuildFrameGenViewPlan(request, tuning)) ==
               "luma R S P down? down? down? down? search? refine? refine? convert merge tiles filter ",
           "Engine lead plan: Balanced alike");
    tuning.quality = depthxr::FrameGenFlowQuality::kPerformance;
    const depthxr::FrameGenViewPlan coarse = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(order(coarse) == "luma R S P down down? down? down? search? refine? convert merge tiles filter " &&
               find(coarse, FrameGenPass::kEngineMerge).inputs[0].level == 2,
           "Engine lead plan: Performance's downsample to level 2 is never skipped (" + order(coarse) + ")");
    // VECTORXR_FRAMEGEN_ENGINE_PREDICATE=0: no measured pass, no predicate;
    // the merge reads no measured flow.
    tuning.engine_predicate = false;
    const depthxr::FrameGenViewPlan unmeasured = depthxr::BuildFrameGenViewPlan(request, tuning);
    tuning.quality = depthxr::FrameGenFlowQuality::kBalanced;
    const depthxr::FrameGenViewPlan unmeasured_balanced = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(order(unmeasured) == "luma R S down convert merge tiles filter " &&
               order(unmeasured_balanced) == "luma R S convert merge tiles filter " &&
               find(unmeasured_balanced, FrameGenPass::kEngineMerge).constants.options[0] ==
                   (depthxr::kFrameGenEngineMergeLead | depthxr::kFrameGenEngineMergeUnmeasured) &&
               find(unmeasured_balanced, FrameGenPass::kEngineMerge).inputs[4].kind == FrameGenTextureKind::kNone,
           "Engine lead plan: with engine_predicate false, nothing measured (" + order(unmeasured) + ", " +
               order(unmeasured_balanced) + ")");
    tuning.engine_predicate = true;
    // Unlocked, interpolating, or without the evidence: Assist's plan (the
    // optical flow hardware's where it is chosen), byte for byte.
    tuning.hardware_flow = tuning.hardware_level = true;
    tuning.quality = depthxr::FrameGenFlowQuality::kQuality;
    tuning.engine_locked = false;
    depthxr::FrameGenTuning assist = tuning;
    assist.engine_mode = FrameGenEngineMode::kAssist;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), depthxr::BuildFrameGenViewPlan(request, assist)) &&
               order(depthxr::BuildFrameGenViewPlan(request, tuning)) == "luma pack unpack R S convert merge tiles filter ",
           "Engine lead plan: unlocked, Assist's plan with the hardware's flow");
    tuning.engine_locked = assist.engine_locked = true;
    tuning.mode = assist.mode = depthxr::FrameGenSynthesisMode::kInterpolate;
    tuning.cover = assist.cover = true;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), depthxr::BuildFrameGenViewPlan(request, assist)),
           "Engine lead plan: interpolating, Assist's plan");
    tuning.engine_interpolate = true;
    const depthxr::FrameGenViewPlan interpolated = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::vector<depthxr::FrameGenDispatch> interpolated_filters;
    for (const auto& dispatch : interpolated.dispatches) {
        if (dispatch.pass == FrameGenPass::kFilter) interpolated_filters.push_back(dispatch);
    }
    Expect(order(interpolated) == "luma R S P down? down? down? down? search? refine? refine? convert merge tiles filter filter " &&
               interpolated_filters.size() == 2 && interpolated_filters[1].inputs[0].kind == FrameGenTextureKind::kEngineFlow,
           "Engine lead plan: interpolating led with engine_interpolate, the cover on the merged flow (" +
               order(interpolated) + ")");
    tuning.engine_interpolate = false;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.cover = false;
    request.engine_motion = false;
    assist.engine_mode = FrameGenEngineMode::kOff;
    assist.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    assist.cover = false;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), depthxr::BuildFrameGenViewPlan(request, assist)),
           "Engine lead plan: without the evidence, the plan without the vectors");
    request.engine_motion = true;
    // The world's motion: fitted to the merged flow after the merge, or the
    // application's set before the conversion (its classes), on the
    // watchdog's frames the fit after the merge and the set again.
    request.world_motion = true;
    const depthxr::FrameGenViewPlan fitted = depthxr::BuildFrameGenViewPlan(request, tuning);
    bool fit_merged = true;
    for (const auto& dispatch : fitted.dispatches) {
        if (dispatch.pass == FrameGenPass::kWorldReduce) {
            fit_merged = fit_merged && dispatch.inputs[0].kind == FrameGenTextureKind::kEngineFlow;
        }
    }
    Expect(order(fitted) == "luma R S P down? down? down? down? search? refine? refine? convert merge tiles W w W w W w filter " &&
               fit_merged && find(fitted, FrameGenPass::kEngineConvert).inputs[4].kind == FrameGenTextureKind::kNone &&
               find(fitted, FrameGenPass::kFilter).inputs[3].kind == FrameGenTextureKind::kWorldParams,
           "Engine lead plan: the world fitted to the merged flow after the merge (" + order(fitted) + ")");
    request.app_delta = true;
    request.game_newest = request.newest;
    request.game_previous = request.previous;
    request.delta_position = {0.0f, 0.0f, 1.8f};
    tuning.world_delta = true;
    const depthxr::FrameGenViewPlan set_first = depthxr::BuildFrameGenViewPlan(request, tuning);
    tuning.delta_watchdog = true;
    const depthxr::FrameGenViewPlan watched = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(order(set_first) == "luma R S P down? down? down? down? search? refine? refine? set convert merge tiles filter " &&
               (find(set_first, FrameGenPass::kEngineConvert).constants.options[0] & depthxr::kFrameGenEngineConvertWorld) != 0 &&
               order(watched) ==
                   "luma R S P down? down? down? down? search? refine? refine? set convert merge tiles W w W w W w set filter ",
           "Engine lead plan: the application's motion set before the conversion, the watchdog's fit after the merge (" +
               order(set_first) + ", " + order(watched) + ")");
    tuning.world_delta = tuning.delta_watchdog = false;
    request.world_motion = request.app_delta = false;
    // The level the backend's views are set up for: the frames the vectors
    // lead (the hardware idle) and those measured without them (on it) end
    // at one level with hardware_level, so their filtered flow is never
    // made anew; without it a led frame would end at Quality's level 2.
    depthxr::FrameGenTuning led = tuning;
    led.hardware_flow = false;
    depthxr::FrameGenTuning measured = tuning;
    measured.engine_mode = FrameGenEngineMode::kAssist;
    depthxr::FrameGenHistoryState history;
    const unsigned first = history.Configure({1024, 1024}, 28, depthxr::FrameGenOutputKind::kSrgb8,
                                             depthxr::FrameGenFinalFlowLevel(led), 3);
    const unsigned fallback = history.Configure({1024, 1024}, 28, depthxr::FrameGenOutputKind::kSrgb8,
                                                depthxr::FrameGenFinalFlowLevel(measured), 3);
    const unsigned again = history.Configure({1024, 1024}, 28, depthxr::FrameGenOutputKind::kSrgb8,
                                             depthxr::FrameGenFinalFlowLevel(led), 3);
    led.hardware_level = false;
    const unsigned unpinned = history.Configure({1024, 1024}, 28, depthxr::FrameGenOutputKind::kSrgb8,
                                                depthxr::FrameGenFinalFlowLevel(led), 3);
    Expect(first == depthxr::FrameGenHistoryState::kAll && fallback == depthxr::FrameGenHistoryState::kNone &&
               again == depthxr::FrameGenHistoryState::kNone && unpinned == depthxr::FrameGenHistoryState::kFiltered,
           "Engine lead plan: Lead -> Assist -> Lead keeps the views' filtered flow at the hardware's level");
}

// Stage 8's plans. Interpolating from Assist with the seen test
// (engine_visibility) and the previous frame's depth, the solve's second
// stage writes both eyes' verdict (no predicate) right before the conversion,
// which reads it in place of the view's own parameters and the previous
// frame's depth in place of the other view's; the merge keeps the surfaces'
// depths in the labels. In Lead its predicate's verdict serves. Extrapolating,
// without the previous frame's depth or the switch, the plans as before.
// Interpolated frames read the labels at input 5 (with
// engine_visibility_synthesis), or with engine_bidir the forward map built for
// them from the previous frame's depth and filtered flow.
void TestFrameGenEngineVisibilityPlan() {
    using depthxr::FrameGenEngineMode;
    using depthxr::FrameGenPass;
    using depthxr::FrameGenSynthesisMode;
    using depthxr::FrameGenTextureKind;
    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = request.temporal = request.depth = request.depth_previous = true;
    for (auto* geometry : {&request.newest, &request.previous}) {
        geometry->angle_left = geometry->angle_down = -0.9f;
        geometry->angle_right = geometry->angle_up = 0.9f;
    }
    request.previous.orientation = {0.0f, 0.01f, 0.0f, 0.99995f};
    request.shift = {0.02f, 0.0f, 0.004f};
    request.engine_motion = true;
    request.engine_vectors = {800, 790};
    depthxr::FrameGenTuning tuning;
    tuning.mode = FrameGenSynthesisMode::kInterpolate;
    tuning.analyze_only = true;
    tuning.engine_mode = FrameGenEngineMode::kAssist;
    tuning.engine_locked = true;
    auto order = [](const depthxr::FrameGenViewPlan& plan) {
        std::string text;
        for (const auto& dispatch : plan.dispatches) {
            switch (dispatch.pass) {
            case FrameGenPass::kEngineReduce: text += "R "; break;
            case FrameGenPass::kEngineSolve: text += (dispatch.constants.options[0] & 15u) == 2u ? "P " : "S "; break;
            case FrameGenPass::kEngineConvert: text += "convert "; break;
            case FrameGenPass::kEngineMerge: text += "merge "; break;
            case FrameGenPass::kShiftClear: text += dispatch.outputs[1].kind == FrameGenTextureKind::kShiftMap ? "C " : "c "; break;
            case FrameGenPass::kShiftSplat: text += "s "; break;
            case FrameGenPass::kShiftResolve: text += "r "; break;
            case FrameGenPass::kShiftFill: text += "f "; break;
            case FrameGenPass::kShiftSplatEngine: text += "es "; break;
            case FrameGenPass::kShiftResolveEngine: text += "er "; break;
            case FrameGenPass::kShiftFillEngine: text += "ef "; break;
            case FrameGenPass::kSynthesizeUnorm8: text += "synth "; break;
            default: break;
            }
        }
        return text;
    };
    auto find = [](const depthxr::FrameGenViewPlan& plan, FrameGenPass pass) {
        return *std::find_if(plan.dispatches.begin(), plan.dispatches.end(),
                             [&](const depthxr::FrameGenDispatch& dispatch) { return dispatch.pass == pass; });
    };
    // kEngineSolve's second stage: both eyes' verdict.
    auto verdict_of = [](const depthxr::FrameGenViewPlan& plan) {
        return *std::find_if(plan.dispatches.begin(), plan.dispatches.end(), [](const depthxr::FrameGenDispatch& dispatch) {
            return dispatch.pass == FrameGenPass::kEngineSolve && (dispatch.constants.options[0] & 15u) == 2u;
        });
    };
    auto same = [](const depthxr::FrameGenViewPlan& a, const depthxr::FrameGenViewPlan& b) {
        bool equal = a.dispatches.size() == b.dispatches.size();
        for (std::size_t i = 0; equal && i < a.dispatches.size(); ++i) {
            equal = a.dispatches[i].pass == b.dispatches[i].pass && a.dispatches[i].predicate == b.dispatches[i].predicate &&
                    std::memcmp(a.dispatches[i].inputs.data(), b.dispatches[i].inputs.data(), sizeof(a.dispatches[i].inputs)) == 0 &&
                    std::memcmp(a.dispatches[i].outputs.data(), b.dispatches[i].outputs.data(), sizeof(a.dispatches[i].outputs)) == 0 &&
                    std::memcmp(&a.dispatches[i].constants, &b.dispatches[i].constants, sizeof(depthxr::FrameGenConstants)) == 0;
        }
        return equal;
    };

    // The analysis.
    const depthxr::FrameGenViewPlan plain = depthxr::BuildFrameGenViewPlan(request, tuning);
    tuning.engine_visibility = true;
    Expect(depthxr::FrameGenEngineVisibility(tuning), "Engine visibility plan: the seen test interpolating from Assist");
    const depthxr::FrameGenViewPlan seen = depthxr::BuildFrameGenViewPlan(request, tuning);
    const depthxr::FrameGenDispatch verdict = verdict_of(seen);
    const depthxr::FrameGenDispatch convert = find(seen, FrameGenPass::kEngineConvert);
    const depthxr::FrameGenDispatch merge = find(seen, FrameGenPass::kEngineMerge);
    Expect(order(plain) == "R S convert merge " && order(seen) == "R S P convert merge ",
           "Engine visibility plan: both eyes' verdict right before the conversion (" + order(seen) + ")");
    Expect(verdict.predicate == depthxr::kFrameGenPredicateNone && verdict.inputs[3].kind == FrameGenTextureKind::kEngineParams &&
               verdict.inputs[5].kind == FrameGenTextureKind::kEngineParamsOther &&
               verdict.outputs[0].kind == FrameGenTextureKind::kEngineVerdict && convert.inputs[2].kind == FrameGenTextureKind::kEngineVerdict &&
               convert.inputs[5].kind == FrameGenTextureKind::kDepthPrevious &&
               (convert.constants.options[0] & depthxr::kFrameGenEngineConvertSeen) != 0 &&
               merge.inputs[5].kind == FrameGenTextureKind::kDepth &&
               (merge.constants.options[0] & depthxr::kFrameGenEngineMergeDepths) != 0,
           "Engine visibility plan: the verdict without a predicate, the conversion's seen test on the previous frame's "
           "depth, the merge keeping the surfaces' depths");
    // Nothing else of the analysis changes.
    std::size_t changed = 0;
    for (std::size_t i = 0, j = 0; i < plain.dispatches.size() && j < seen.dispatches.size(); ++i, ++j) {
        if (seen.dispatches[j].pass == FrameGenPass::kEngineSolve && (seen.dispatches[j].constants.options[0] & 15u) == 2u) ++j;
        const bool equal = plain.dispatches[i].pass == seen.dispatches[j].pass &&
                           std::memcmp(&plain.dispatches[i].constants, &seen.dispatches[j].constants,
                                       sizeof(depthxr::FrameGenConstants)) == 0 &&
                           std::memcmp(plain.dispatches[i].inputs.data(), seen.dispatches[j].inputs.data(),
                                       sizeof(plain.dispatches[i].inputs)) == 0;
        changed += equal ? 0 : 1;
    }
    Expect(seen.dispatches.size() == plain.dispatches.size() + 1 && changed == 2,
           "Engine visibility plan: only the conversion and the merge change (" + std::to_string(changed) + ")");
    // Without the previous frame's depth, extrapolating, observing, or with
    // the switch off: as before.
    request.depth_previous = false;
    depthxr::FrameGenTuning switched_off = tuning;
    switched_off.engine_visibility = false;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), depthxr::BuildFrameGenViewPlan(request, switched_off)),
           "Engine visibility plan: without the previous frame's depth, as without the switch");
    request.depth_previous = true;
    depthxr::FrameGenTuning extrapolating = tuning, extrapolating_plain = tuning;
    extrapolating.mode = extrapolating_plain.mode = FrameGenSynthesisMode::kExtrapolate;
    extrapolating_plain.engine_visibility = false;
    Expect(!depthxr::FrameGenEngineVisibility(extrapolating) &&
               same(depthxr::BuildFrameGenViewPlan(request, extrapolating),
                    depthxr::BuildFrameGenViewPlan(request, extrapolating_plain)),
           "Engine visibility plan: extrapolating, as without the switch");
    depthxr::FrameGenTuning observing = tuning, observing_plain = tuning;
    observing.engine_mode = observing_plain.engine_mode = FrameGenEngineMode::kObserve;
    observing_plain.engine_visibility = false;
    Expect(!depthxr::FrameGenEngineVisibility(observing) &&
               same(depthxr::BuildFrameGenViewPlan(request, observing), depthxr::BuildFrameGenViewPlan(request, observing_plain)),
           "Engine visibility plan: observing, as without the switch");
    // Interpolating led (engine_interpolate): the predicate's verdict serves;
    // with no measured pass at all, the verdict is written for the test.
    tuning.engine_mode = FrameGenEngineMode::kLead;
    tuning.engine_interpolate = true;
    const depthxr::FrameGenViewPlan led = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::size_t verdicts = 0;
    for (const auto& dispatch : led.dispatches) {
        verdicts += dispatch.pass == FrameGenPass::kEngineSolve && (dispatch.constants.options[0] & 15u) == 2u ? 1 : 0;
    }
    Expect(verdicts == 1 && order(led) == "R S P convert merge " &&
               verdict_of(led).predicate == depthxr::kFrameGenPredicateSet &&
               find(led, FrameGenPass::kEngineConvert).inputs[2].kind == FrameGenTextureKind::kEngineVerdict &&
               find(led, FrameGenPass::kEngineConvert).inputs[5].kind == FrameGenTextureKind::kDepthPrevious,
           "Engine visibility plan: led, the predicate's verdict read by the conversion (" + order(led) + ")");
    tuning.engine_predicate = false;
    const depthxr::FrameGenViewPlan unmeasured = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(order(unmeasured) == "R S P convert merge " &&
               verdict_of(unmeasured).predicate == depthxr::kFrameGenPredicateNone,
           "Engine visibility plan: led with no measured pass, the verdict written for the test (" + order(unmeasured) + ")");
    tuning.engine_predicate = true;
    tuning.engine_interpolate = false;
    tuning.engine_mode = FrameGenEngineMode::kAssist;

    // The frames written just in time: the labels at input 5 with the
    // synthesis switch (the analysed request labelled with depth), never
    // extrapolating (cover frames); the forward map with engine_bidir.
    auto synthesis = [&](const depthxr::FrameGenTuning& with, FrameGenSynthesisMode mode, float fraction) {
        return depthxr::BuildFrameGenSynthesisPlan(request, with, mode, fraction, mode == FrameGenSynthesisMode::kExtrapolate);
    };
    depthxr::FrameGenTuning off = tuning;
    off.engine_visibility = false;
    const depthxr::FrameGenViewPlan written_plain = synthesis(off, FrameGenSynthesisMode::kInterpolate, 0.5f);
    const depthxr::FrameGenViewPlan written_merge = synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.5f);
    tuning.engine_visibility_synthesis = true;
    const depthxr::FrameGenViewPlan written = synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.5f);
    const depthxr::FrameGenDispatch& output = written.dispatches.back();
    Expect(same(written_merge, written_plain) && written.dispatches.size() == written_plain.dispatches.size() &&
               output.inputs[5].kind == FrameGenTextureKind::kEngineLabels &&
               output.constants.options[1] ==
                   (written_plain.dispatches.back().constants.options[1] | depthxr::kFrameGenFlagEngineVisibility),
           "Engine visibility plan: interpolated frames read the labels at input 5 with the synthesis switch alone");
    Expect(same(synthesis(tuning, FrameGenSynthesisMode::kExtrapolate, 0.5f),
                synthesis(off, FrameGenSynthesisMode::kExtrapolate, 0.5f)),
           "Engine visibility plan: cover frames (extrapolated) as without the switch");
    request.engine_motion = false;
    Expect(same(synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.5f), written_plain),
           "Engine visibility plan: a real frame without the vectors' labels, as without the switch");
    request.engine_motion = true;

    tuning.engine_bidir = true;
    const depthxr::FrameGenViewPlan forward = synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.25f);
    const depthxr::FrameGenDispatch& forward_splat = forward.dispatches[forward.dispatches.size() - 3];
    const depthxr::FrameGenDispatch& forward_resolve = forward.dispatches[forward.dispatches.size() - 2];
    const depthxr::FrameGenDispatch& forward_output = forward.dispatches.back();
    Expect(order(forward) == "C s r f c es er synth " &&
               forward_splat.constants.options[0] == depthxr::kFrameGenEngineMapForward &&
               forward_splat.constants.options[1] ==
                   (depthxr::kFrameGenFlagDepth | depthxr::kFrameGenFlagDepthSplat | depthxr::kFrameGenFlagEngine) &&
               forward_splat.constants.tuning[1] == 0.25f &&
               forward_splat.constants.rot0 == written_plain.dispatches.back().constants.rot0 &&
               forward_splat.inputs[0].kind == FrameGenTextureKind::kDepthPrevious &&
               forward_splat.inputs[1].kind == FrameGenTextureKind::kFilteredPrevious &&
               forward_splat.inputs[1].level == forward.final_flow_level &&
               forward_resolve.inputs[2].kind == FrameGenTextureKind::kShiftKeys &&
               forward_resolve.outputs[0].kind == FrameGenTextureKind::kMotionMap &&
               !depthxr::FrameGenEngineMapTime(forward),
           "Engine visibility plan: the forward map after the eye-shift map, from the previous frame's depth and "
           "filtered flow, its fraction, no engine motion map's time (" + order(forward) + ")");
    Expect(forward_output.inputs[5].kind == FrameGenTextureKind::kMotionMap &&
               (forward_output.constants.options[1] & depthxr::kFrameGenFlagEngineBidir) != 0 &&
               (forward_output.constants.options[1] & depthxr::kFrameGenFlagEngineVisibility) == 0,
           "Engine visibility plan: the forward map read at input 5 in place of the labels");
    // The previous frame's flow is the pair before's: moved the intervals'
    // ratio as far (clamped as the filters clamp it).
    depthxr::FrameGenTuning uneven = tuning;
    uneven.interval_ratio = 1.5f;
    const depthxr::FrameGenViewPlan forward_uneven = synthesis(uneven, FrameGenSynthesisMode::kInterpolate, 0.25f);
    uneven.interval_ratio = 9.0f;
    const depthxr::FrameGenViewPlan forward_clamped = synthesis(uneven, FrameGenSynthesisMode::kInterpolate, 0.25f);
    Expect(forward_uneven.dispatches[forward_uneven.dispatches.size() - 3].constants.tuning[1] == 0.375f &&
               forward_uneven.dispatches[forward_uneven.dispatches.size() - 2].constants.tuning[1] == 0.375f &&
               forward_uneven.dispatches.back().constants.tuning[0] == 0.25f &&
               forward_clamped.dispatches[forward_clamped.dispatches.size() - 3].constants.tuning[1] == 1.0f,
           "Engine visibility plan: the forward map moves the previous frame's flow by the fraction times the "
           "intervals' ratio");
    // A real frame whose vectors were held back or missing: no forward map,
    // its frames mk1's.
    request.engine_motion = false;
    Expect(same(synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.5f), written_plain),
           "Engine visibility plan: no forward map for a real frame without the vectors");
    request.engine_motion = true;
    request.depth_previous = false;
    Expect(same(synthesis(tuning, FrameGenSynthesisMode::kInterpolate, 0.5f), written),
           "Engine visibility plan: no forward map without the previous frame's depth (the labels read)");
    request.depth_previous = true;
    depthxr::FrameGenTuning bidir_observing = tuning;
    bidir_observing.engine_mode = FrameGenEngineMode::kObserve;
    Expect(same(synthesis(bidir_observing, FrameGenSynthesisMode::kInterpolate, 0.5f),
                synthesis(off, FrameGenSynthesisMode::kInterpolate, 0.5f)),
           "Engine visibility plan: observing, neither");

    // Written together (frames generated with their analysis): the same.
    tuning.analyze_only = false;
    tuning.generated = 2;
    tuning.fractions = {0.25f, 0.75f};
    tuning.engine_bidir = false;
    const depthxr::FrameGenViewPlan together = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::size_t reading = 0;
    for (const auto& dispatch : together.dispatches) {
        reading += dispatch.pass == FrameGenPass::kSynthesizeUnorm8 && dispatch.inputs[5].kind == FrameGenTextureKind::kEngineLabels &&
                           (dispatch.constants.options[1] & depthxr::kFrameGenFlagEngineVisibility) != 0
                       ? 1
                       : 0;
    }
    tuning.engine_bidir = true;
    const depthxr::FrameGenViewPlan together_forward = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::size_t forward_maps = 0, forward_reads = 0;
    for (const auto& dispatch : together_forward.dispatches) {
        forward_maps += dispatch.pass == FrameGenPass::kShiftSplatEngine &&
                                (dispatch.constants.options[0] & depthxr::kFrameGenEngineMapForward) != 0
                            ? 1
                            : 0;
        forward_reads += dispatch.pass == FrameGenPass::kSynthesizeUnorm8 &&
                                 (dispatch.constants.options[1] & depthxr::kFrameGenFlagEngineBidir) != 0
                             ? 1
                             : 0;
    }
    Expect(reading == 2 && forward_maps == 2 && forward_reads == 2 && together_forward.frames_in_order,
           "Engine visibility plan: written together, each frame reads the labels, or its own forward map");
}

// Stage 6's controller: a session in Lead starts at Assist and leads after
// 30 verdicts in a row that let the vectors be used; more than 10% of the
// last 30 frames held back hold it at Assist for 300 real frames, then 30
// in a row lead again. Verdicts read together count the held ones last. The
// validator counts the frames either eye's verdict held back.
void TestFrameGenEngineLeadControl() {
    depthxr::FrameGenEngineLeadControl control;
    control.AddVerdicts(29, 0);
    Expect(!control.Leads(), "Engine lead control: at Assist before 30 verdicts in a row");
    control.AddVerdicts(1, 0);
    Expect(control.Leads() && control.Demotions() == 0, "Engine lead control: leads after 30 in a row");
    control.AddVerdicts(3, 3);
    for (int i = 0; i < 10; ++i) control.AddFrame();
    Expect(control.Leads(), "Engine lead control: 3 of the last 30 held back (10%) keep it leading");
    control.AddVerdicts(1, 1);
    Expect(!control.Leads() && control.Demotions() == 1, "Engine lead control: a fourth holds it at Assist");
    control.AddVerdicts(60, 0);
    for (int i = 0; i < 299; ++i) control.AddFrame();
    Expect(!control.Leads(), "Engine lead control: at Assist for 300 frames, its verdicts as they may be");
    control.AddFrame();
    Expect(control.Leads() && control.Promotions() == 1, "Engine lead control: leads again after 300 frames");
    // Held back again, its verdicts held back through the 300 frames: then
    // 30 in a row more.
    control.AddVerdicts(4, 4);
    Expect(!control.Leads() && control.Demotions() == 2, "Engine lead control: held at Assist again");
    for (int i = 0; i < 300; ++i) {
        control.AddVerdicts(1, 1);
        control.AddFrame();
    }
    control.AddVerdicts(29, 0);
    Expect(!control.Leads(), "Engine lead control: after the 300 frames, 29 in a row do not lead");
    // Read together, the held one last: no streak.
    control.AddVerdicts(2, 1);
    Expect(!control.Leads(), "Engine lead control: verdicts read together count the held one last");
    control.AddVerdicts(30, 0);
    Expect(control.Leads() && control.Promotions() == 2, "Engine lead control: 30 in a row lead again");

    // The validator: a frame held back on either eye is one held frame.
    depthxr::FrameGenEngineValidator validator;
    depthxr::FrameGenEngineFrameEvidence frame;
    frame.views.resize(2);
    validator.Add(frame, 0.0);
    frame.views[1].verdict = depthxr::kFrameGenEngineVerdictStale;
    validator.Add(frame, 0.1);
    frame.views[0].verdict = depthxr::kFrameGenEngineVerdictPhotometric;
    validator.Add(frame, 0.2);
    depthxr::FrameGenEngineValidation report = validator.Report();
    Expect(report.frames == 3 && report.held == 2 && report.invalid == 3 && report.verdicts == 6 && report.lead_held == 2,
           "Engine lead control: the validator counts the frames either eye held back (" + std::to_string(report.held) + ", " +
               std::to_string(report.lead_held) + " as Lead judges them)");
    // As Lead judges them (lead_held): vectors Assist's test lets be used
    // (about as good as the measured flow) that beat no motion by less than
    // 15% while content moves hold a frame back; Assist's photometric reason
    // alone, the vectors beating no motion clearly, does not.
    frame.views[0].verdict = frame.views[1].verdict = 0;
    for (depthxr::FrameGenEngineEvidence& view : frame.views) {
        view.textured = 1000.0;
        view.moving = 500.0;
        view.zero = 100.0;       // 0.1 a sample
        view.variants[0] = 92.0; // 0.092: above 0.85 of it and the slack
    }
    validator.Add(frame, 0.3);
    report = validator.Report();
    Expect(report.held == 2 && report.lead_held == 3,
           "Engine lead control: vectors barely better than no motion are held back as Lead judges them (" +
               std::to_string(report.lead_held) + ")");
    frame.views[1].verdict = depthxr::kFrameGenEngineVerdictPhotometric;
    for (depthxr::FrameGenEngineEvidence& view : frame.views) view.variants[0] = 50.0;
    validator.Add(frame, 0.4);
    report = validator.Report();
    Expect(report.held == 3 && report.lead_held == 3,
           "Engine lead control: Assist's photometric reason alone does not hold a frame back as Lead judges it (" +
               std::to_string(report.lead_held) + ")");
    // Few moving samples: no worse than no motion (and the slack) passes.
    frame.views[1].verdict = 0;
    for (depthxr::FrameGenEngineEvidence& view : frame.views) {
        view.moving = 100.0;
        view.variants[0] = 104.0;
    }
    validator.Add(frame, 0.5);
    report = validator.Report();
    Expect(report.held == 3 && report.lead_held == 3,
           "Engine lead control: with 100 samples moving, vectors within the slack of no motion are not held back (" +
               std::to_string(report.lead_held) + ")");
}

// Engine motion validation on synthetic evidence: a game's vectors in a
// convention (the truth), the head turning both ways at changing speeds and
// leaning; each frame's evidence as the GPU sums it for the convention under
// test (the validator's own): C1 from still samples near and far, the pair
// before's fit, C2 against the measured flow, and photometric sums that
// favour the convention under test only where it is the truth.
struct EngineTruth {
    std::array<double, 2> gain{1.0, 1.0};
    depthxr::FrameGenEngineCamera camera{depthxr::FrameGenEngineCamera::kIncluded};
    double jitter_px{0.0}; // each frame's vectors offset by up to this (vector pixels)
    double offset_px{0.0}; // every frame's vectors offset by this (vector pixels)
    double noise{0.0};     // noise on the vectors, as a share of the head's movement
    bool late{false};      // a frame late: the vectors of the pair before
    bool swapped{false};   // x holds y's vector and y x's
    bool zero{false};      // the vectors are all zero
    bool pitch{true};      // the head pitches as well as turns
    bool still{true};      // still content to fit (none: on foot, nothing still)
    bool correlated{false}; // the head pitches as it turns
};

class EngineScenario {
  public:
    explicit EngineScenario(EngineTruth truth) : truth_(truth) {}
    // Frame `f`'s evidence, both views, for `tested` (what the GPU decoded
    // with), the frame calibrating.
    depthxr::FrameGenEngineFrameEvidence Frame(int f, const depthxr::FrameGenEngineConvention& tested, bool locked,
                                              std::uint32_t verdict = 0) const {
        depthxr::FrameGenEngineFrameEvidence frame;
        frame.convention = tested;
        frame.locked = locked;
        for (int v = 0; v < 2; ++v) {
            depthxr::FrameGenEngineEvidence view;
            view.calibrate = true;
            view.l1 = {2712, 2678};
            view.vectors = {4339, 4285};
            view.color = {5424, 5356};
            view.lag = true;
            view.verdict = verdict;
            const auto now = Head(f), before = Head(f - 1);
            const double jitter = truth_.jitter_px * (Hash(f, 3) * 2.0 - 1.0);
            for (int s = 0; s < 200 && truth_.still; ++s) {
                const double u = Hash(s, 5 + v), w = Hash(s, 7 + v);
                const double z = s % 3 == 0 ? 0.0 : 0.5 + 2.5 * Hash(s, 11); // 0: far
                auto predict = [&](const std::array<double, 3>& head, int axis, bool translation) {
                    const double rotation = head[axis] * (1.0 + 0.2 * (axis ? w : u));
                    return translation ? (z > 0.0 ? head[2] * (axis ? 0.3 : 1.0) / z : 0.0) : rotation;
                };
                std::array<double, 2> e{};
                for (int axis = 0; axis < 2; ++axis) {
                    const auto& head = truth_.late ? before : now;
                    const int source = truth_.swapped ? 1 - axis : axis;
                    const double r = predict(head, source, false), t = predict(head, source, true);
                    const double movement = truth_.camera == depthxr::FrameGenEngineCamera::kIncluded ? r + t
                                            : truth_.camera == depthxr::FrameGenEngineCamera::kRotationOnly ? r
                                                                                                        : 0.0;
                    const double noise = truth_.noise * (std::abs(r) + 0.01) * (Hash(s * 31 + f, 13 + axis) * 2.0 - 1.0) * 1.7;
                    const double offset =
                        (jitter * (axis ? 0.5 : 1.0) + truth_.offset_px) / (0.5 * (axis ? 4285.0 : 4339.0));
                    e[axis] = truth_.zero ? 0.0 : (movement + noise + offset) / truth_.gain[axis];
                }
                for (int axis = 0; axis < 2; ++axis) {
                    view.cockpit[axis].Add(e[axis], predict(now, axis, false), predict(now, axis, true));
                    view.cockpit_lag[axis].Add(e[axis], predict(before, axis, false), predict(before, axis, true));
                }
                view.swapped[0] += e[0] * predict(now, 1, false);
                view.swapped[1] += e[1] * predict(now, 0, false);
            }
            // C2: content moving on its own (h, NDC) besides the head (q =
            // h + p); vectors without the head (excluded) follow h.
            for (int s = 0; s < 200; ++s) {
                for (int axis = 0; axis < 2; ++axis) {
                    const double h = 0.01 * (Hash(s + 1000 * f, 17 + axis) * 2.0 - 1.0);
                    const double p = now[axis];
                    const double e = truth_.zero ? 0.0
                                     : truth_.camera == depthxr::FrameGenEngineCamera::kExcluded ? h / truth_.gain[axis]
                                                                                                  : (h + p) / truth_.gain[axis];
                    view.measured[axis].Add(e, h + p, h);
                }
            }
            // C3: the convention under test explains the image only where it
            // is the truth (and the vectors are the frame's own).
            const bool right = depthxr::SameFrameGenEngineConvention(tested, Truth()) && !truth_.late && !truth_.swapped &&
                               !truth_.zero && truth_.noise < 0.5;
            const double n = 1000.0;
            view.textured = view.with_measured = n;
            for (std::size_t c = 0; c < view.variants.size(); ++c) view.variants[c] = 0.05 * n;
            if (right) view.variants[0] = 0.01 * n;
            view.zero = 0.04 * n;
            view.measured_cost = 0.0105 * n;
            view.wins = (right ? 0.95 : 0.3) * n;
            view.moving = 0.3 * n;
            view.variant_moving = view.variants[0] * 0.3;
            view.zero_moving = 0.05 * 0.3 * n;
            view.lag_samples = n;
            view.lag_variant = view.variants[0];
            view.lag_cost = right ? 0.03 * n : view.variants[0];
            view.epe = {0.4 * n, 0.8 * n, 0.3 * n};
            view.epe_samples = {n, n, n};
            frame.views.push_back(view);
        }
        return frame;
    }
    depthxr::FrameGenEngineConvention Truth() const {
        depthxr::FrameGenEngineConvention convention;
        convention.gain = {static_cast<float>(truth_.gain[0]), static_cast<float>(truth_.gain[1])};
        convention.camera = truth_.camera;
        return convention;
    }
    EngineTruth& truth() { return truth_; }

  private:
    static double Hash(int a, int b) {
        std::uint32_t h = static_cast<std::uint32_t>(a) * 374761393u + static_cast<std::uint32_t>(b) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<double>((h ^ (h >> 16)) & 0xffffu) / 65535.0;
    }
    // The head's turn per frame (NDC, x and y) and lean (meters) at frame f:
    // changing speed and direction from frame to frame.
    std::array<double, 3> Head(int f) const {
        const double yaw = 0.012 * std::sin(0.9 * f) + 0.008 * (Hash(f, 1) * 2.0 - 1.0);
        const double pitch = !truth_.pitch    ? 0.0
                             : truth_.correlated ? 0.9 * yaw + 0.002 * (Hash(f, 2) * 2.0 - 1.0)
                                                 : 0.009 * std::cos(1.3 * f) + 0.006 * (Hash(f, 2) * 2.0 - 1.0);
        return {yaw, pitch, 0.004 * std::sin(0.7 * f)};
    }
    EngineTruth truth_;
};

// Runs a scenario through a validator from frame `first` for `frames`
// frames, `seconds_per_frame` apart; the frame it locked at (none: never).
std::optional<int> RunEngineScenario(depthxr::FrameGenEngineValidator& validator, EngineScenario& scenario, int first,
                                     int frames, double seconds_per_frame = 1.0 / 90.0) {
    std::optional<int> locked;
    for (int f = first; f < first + frames; ++f) {
        validator.Add(scenario.Frame(f, validator.Convention(), validator.Locked()), f * seconds_per_frame);
        if (!locked && validator.Locked()) locked = f;
    }
    return locked;
}

void TestFrameGenEngineValidator() {
    using depthxr::FrameGenEngineCamera;
    using State = depthxr::FrameGenEngineValidator::State;
    // Each canonical convention, the head included (and its rotation only,
    // and excluded, for one each), adopted within 30 frames.
    const auto canonical = depthxr::FrameGenEngineCanonicalGains({4339, 4285}, {5424, 5356});
    std::vector<EngineTruth> truths;
    for (const auto& gain : canonical) {
        EngineTruth truth;
        truth.gain = gain.gain;
        truths.push_back(truth);
    }
    EngineTruth rotation;
    rotation.camera = FrameGenEngineCamera::kRotationOnly;
    rotation.gain = {1.0, -1.0};
    truths.push_back(rotation);
    EngineTruth excluded;
    excluded.camera = FrameGenEngineCamera::kExcluded;
    excluded.gain = {2.0, -2.0};
    truths.push_back(excluded);
    // Excluded in vector pixels, with a little noise: still content's
    // vectors are near zero in NDC, not in their own units.
    excluded.gain = canonical[3].gain;
    excluded.noise = 0.02;
    truths.push_back(excluded);
    for (const EngineTruth& truth : truths) {
        EngineScenario scenario(truth);
        depthxr::FrameGenEngineValidator validator;
        const std::optional<int> locked = RunEngineScenario(validator, scenario, 0, 30);
        const auto report = validator.Report();
        const std::string what = std::to_string(truth.gain[0]) + "," + std::to_string(truth.gain[1]) + " " +
                                 depthxr::ToString(truth.camera) + ": " + depthxr::ToString(report.state) + ", " +
                                 report.reason;
        Expect(locked && *locked <= 15 && depthxr::SameFrameGenEngineConvention(validator.Convention(), scenario.Truth()) &&
                   !validator.Convention().jitter && validator.Convention().offset == std::array<float, 2>{0.0f, 0.0f},
               "Engine validator: the convention is adopted (" + what + ")");
    }

    // Never adopted: the head never pitching (no batch), the vectors noisy,
    // a frame late, axes swapped, axes swapped with the head pitching as it
    // turns (the other axis explains each better), all zero.
    auto never = [](EngineTruth truth, const std::string& what, const std::string& reason = "") {
        EngineScenario scenario(truth);
        depthxr::FrameGenEngineValidator validator;
        const std::optional<int> locked = RunEngineScenario(validator, scenario, 0, 300);
        Expect(!locked && validator.GetState() != State::kDisabled,
               "Engine validator: never adopted " + what + " (" + depthxr::ToString(validator.Report().state) + ", " +
                   validator.Report().reason + ")");
        Expect(reason.empty() || validator.Report().reason.find(reason) != std::string::npos,
               "Engine validator: " + what + ": " + validator.Report().reason);
        return validator.Report();
    };
    EngineTruth yaw_only;
    yaw_only.pitch = false;
    const auto unbatched = never(yaw_only, "without the head pitching");
    Expect(unbatched.batches == 0, "Engine validator: no batch completes without pitch");
    EngineTruth noisy;
    noisy.noise = 2.0;
    never(noisy, "noisy vectors", "corr2");
    EngineTruth late;
    late.late = true;
    const auto lagged = never(late, "late vectors");
    Expect(lagged.lag, "Engine validator: late vectors fit the pair before better");
    EngineTruth swapped;
    swapped.swapped = true;
    never(swapped, "swapped axes");
    swapped.correlated = true;
    never(swapped, "swapped axes of a head pitching as it turns", "swapped");
    EngineTruth zero;
    zero.zero = true;
    never(zero, "zero vectors");

    // A prior: one batch adopts it, sooner than three; never without a batch.
    {
        EngineScenario scenario(EngineTruth{});
        depthxr::FrameGenEngineValidator plain;
        const std::optional<int> without = RunEngineScenario(plain, scenario, 0, 40);
        depthxr::FrameGenEngineValidator primed;
        primed.SetPrior(scenario.Truth());
        const std::optional<int> with = RunEngineScenario(primed, scenario, 0, 40);
        Expect(with && without && *with < *without && primed.Report().prior,
               "Engine validator: a prior is adopted after one batch (" + std::to_string(with.value_or(-1)) + " vs " +
                   std::to_string(without.value_or(-1)) + ")");
        EngineScenario flat(yaw_only);
        depthxr::FrameGenEngineValidator unbatched_prior;
        unbatched_prior.SetPrior(flat.Truth());
        Expect(!RunEngineScenario(unbatched_prior, flat, 0, 300), "Engine validator: a prior alone never adopts");
    }

    // Fixed: only that convention locks.
    {
        EngineTruth uv;
        uv.gain = {2.0, -2.0};
        EngineScenario scenario(uv);
        depthxr::FrameGenEngineValidator fixed;
        fixed.Fix(depthxr::FrameGenEngineConvention{});
        Expect(!RunEngineScenario(fixed, scenario, 0, 200) && fixed.Report().fixed,
               "Engine validator: a fixed convention that is not the truth never locks");
        depthxr::FrameGenEngineValidator right;
        depthxr::FrameGenEngineConvention truth = scenario.Truth();
        right.Fix(truth);
        Expect(RunEngineScenario(right, scenario, 0, 40).has_value(), "Engine validator: the fixed truth locks");
    }

    // Jitter: each frame's vectors offset by up to 0.6 vector pixels.
    {
        EngineTruth jittered;
        jittered.jitter_px = 0.6;
        EngineScenario scenario(jittered);
        depthxr::FrameGenEngineValidator validator;
        const std::optional<int> locked = RunEngineScenario(validator, scenario, 0, 60);
        Expect(locked && validator.Convention().jitter && validator.Report().jitter_px[0] > 0.15 &&
                   validator.Convention().offset == std::array<float, 2>{0.0f, 0.0f},
               "Engine validator: jitter is found (" + std::to_string(validator.Report().jitter_px[0]) + " px)");
    }

    // An offset every frame's vectors share (0.5 vector pixels): no jitter,
    // the offset taken out (in e's units: NDC over the gain).
    {
        EngineTruth offset;
        offset.gain = {1.0, -1.0};
        offset.offset_px = 0.5;
        EngineScenario scenario(offset);
        depthxr::FrameGenEngineValidator validator;
        const std::optional<int> locked = RunEngineScenario(validator, scenario, 0, 60);
        const std::array<float, 2> found = validator.Convention().offset;
        const std::array<double, 2> expected{0.5 / (0.5 * 4339.0) / 1.0, 0.5 / (0.5 * 4285.0) / -1.0};
        Expect(locked && !validator.Convention().jitter && std::abs(found[0] - expected[0]) < 0.02 * std::abs(expected[0]) &&
                   std::abs(found[1] - expected[1]) < 0.02 * std::abs(expected[1]),
               "Engine validator: a constant offset is taken out (" + std::to_string(found[0]) + "/" +
                   std::to_string(found[1]) + ")");
    }

    // Contradiction: locked, then the vectors fit another convention.
    {
        EngineScenario scenario(EngineTruth{});
        depthxr::FrameGenEngineValidator validator;
        Expect(RunEngineScenario(validator, scenario, 0, 30).has_value(), "Engine validator: locked before the change");
        scenario.truth().gain = {1.0, -1.0};
        RunEngineScenario(validator, scenario, 30, 10);
        Expect(validator.GetState() == State::kSuspended, "Engine validator: a contradicting batch suspends (" +
                                                              std::string(depthxr::ToString(validator.GetState())) + ")");
        // 10 s on it calibrates again, and adopts the new convention.
        const std::optional<int> relocked = RunEngineScenario(validator, scenario, 40, 1200);
        Expect(relocked && depthxr::SameFrameGenEngineConvention(validator.Convention(), scenario.Truth()),
               "Engine validator: after 10 s suspended, calibrated again");
    }

    // Disabled: 3 suspensions within 5 minutes; spread wider, never.
    for (const double gap : {20.0, 200.0}) {
        EngineScenario scenario(EngineTruth{});
        depthxr::FrameGenEngineValidator validator;
        int frame = 0;
        for (int round = 0; round < 3; ++round) {
            scenario.truth().gain = round % 2 ? std::array<double, 2>{1.0, -1.0} : std::array<double, 2>{1.0, 1.0};
            RunEngineScenario(validator, scenario, frame, static_cast<int>(gap * 10), 0.1);
            frame += static_cast<int>(gap * 10);
        }
        scenario.truth().gain = {2.0, -2.0};
        RunEngineScenario(validator, scenario, frame, 100, 0.1);
        Expect((validator.GetState() == State::kDisabled) == (gap < 100.0),
               "Engine validator: 3 suspensions " + std::to_string(gap) + " s apart: " +
                   depthxr::ToString(validator.GetState()) + " (" + validator.Report().reason + ")");
    }

    // Disabled: no convention over 5 minutes of batches; but never without
    // still content (no batch, however long), and never by time without
    // batches: frames that stop (Frame Generation off, a pause) or that
    // have nothing still to fit.
    {
        EngineScenario scenario(noisy);
        depthxr::FrameGenEngineValidator validator;
        RunEngineScenario(validator, scenario, 0, 3100, 0.1);
        Expect(validator.GetState() == State::kDisabled, "Engine validator: no convention after 5 minutes disables");
        EngineTruth on_foot;
        on_foot.still = false;
        EngineScenario walking(on_foot);
        depthxr::FrameGenEngineValidator patient;
        RunEngineScenario(patient, walking, 0, 6000, 0.1);
        Expect(patient.GetState() != State::kDisabled && patient.Report().batches == 0,
               "Engine validator: no still content never disables");
        depthxr::FrameGenEngineValidator paused;
        RunEngineScenario(paused, scenario, 0, 6, 0.1);
        RunEngineScenario(paused, scenario, 3600, 6, 0.1);
        Expect(paused.GetState() != State::kDisabled && paused.Report().batches >= 3,
               "Engine validator: 6 minutes without frames do not disable (" +
                   std::string(depthxr::ToString(paused.GetState())) + ", " + paused.Report().reason + ")");
        depthxr::FrameGenEngineValidator stepped_off;
        RunEngineScenario(stepped_off, scenario, 0, 6, 0.1);
        EngineScenario off(noisy);
        off.truth().still = false;
        RunEngineScenario(stepped_off, off, 6, 3100, 0.1);
        RunEngineScenario(stepped_off, scenario, 3106, 6, 0.1);
        Expect(stepped_off.GetState() != State::kDisabled && stepped_off.Report().batches >= 3,
               "Engine validator: 5 minutes without still content after two batches do not disable (" +
                   std::string(depthxr::ToString(stepped_off.GetState())) + ", " + stepped_off.Report().reason + ")");
    }

    // A batch that cannot close (no pitch) starts over rather than holding
    // minutes of frames: once the head pitches, batches close as before.
    {
        EngineScenario flat(yaw_only);
        depthxr::FrameGenEngineValidator validator;
        RunEngineScenario(validator, flat, 0, 1000);
        EngineScenario scenario(EngineTruth{});
        const std::optional<int> locked = RunEngineScenario(validator, scenario, 1000, 40);
        Expect(locked && *locked <= 1015,
               "Engine validator: batches close after a long batch without pitch (" +
                   std::to_string(locked.value_or(-1)) + ")");
    }

    // Verdicts: locked, then most frames held back by their verdict suspends.
    {
        EngineScenario scenario(EngineTruth{});
        depthxr::FrameGenEngineValidator validator;
        Expect(RunEngineScenario(validator, scenario, 0, 30).has_value(), "Engine validator: locked");
        for (int f = 30; f < 130 && validator.Locked(); ++f) {
            validator.Add(scenario.Frame(f, validator.Convention(), true, f % 3 ? 0u : depthxr::kFrameGenEngineVerdictStale),
                          f / 90.0);
        }
        const auto report = validator.Report();
        Expect(validator.GetState() == State::kSuspended && report.reasons[1] > 20,
               "Engine validator: a third of the frames held back suspends (" + report.reason + ")");
    }

    // The evidence as the GPU writes it reads back into the same sums.
    {
        std::array<float, 4 * depthxr::kFrameGenEngineEvidenceTexels> evidence{};
        std::array<float, 4 * depthxr::kFrameGenEngineParamsTexels> params{};
        for (std::size_t i = 0; i < evidence.size(); ++i) evidence[i] = static_cast<float>(i + 1);
        params = {0.25f, -0.5f, 6.0f, 0.9f};
        params[11] = 1.0f;
        const auto read = depthxr::ReadFrameGenEngineEvidence(evidence.data(), params.data(), {});
        Expect(read.cockpit[0].Samples() == 1.0f && read.cockpit[1].Samples() == 11.0f && read.swapped[1] == 22.0f &&
                   read.measured[1].Samples() == 33.0f && read.head_gap == 43.0f && read.variants[0] == 45.0f &&
                   read.textured == 55.0f && read.epe_samples[2] == 68.0f && read.valid == 71.0f && read.flow_px == 72.0f &&
                   read.lag_cost == 73.0f &&
                   read.lag_moving == 78.0f && read.cockpit_lag[1].Samples() == 91.0f && read.lag &&
                   read.lag_photometric && read.bias == std::array<float, 2>{0.25f, -0.5f} && read.verdict == 6u && read.stale,
               "Engine validator: the evidence's texels read back in their places");
        // Summed as a convention reads the vectors, g (e - offset): read back
        // as decoded. The x axis's sums: n 1, p_r 2, p_t 3, e 4, ..., e p_r 8,
        // e^2 10; y's p_r 12; e_x p_r,y 21.
        depthxr::FrameGenEngineConvention convention;
        convention.gain = {2.0f, -0.5f};
        convention.offset = {0.5f, 0.0f};
        const auto unscaled = depthxr::ReadFrameGenEngineEvidence(evidence.data(), params.data(), convention);
        Expect(unscaled.cockpit[0].Intercept(0.0, 0.0) == 2.5 && unscaled.cockpit[0].ErrorRotation() == 5.0 &&
                   unscaled.cockpit[0].ErrorSquares() == 4.75 && unscaled.swapped[0] == 16.5 &&
                   unscaled.cockpit[1].Intercept(0.0, 0.0) == 14.0 / -0.5 / 11.0 &&
                   unscaled.measured[0].Intercept(0.0, 0.0) == (26.0 / 2.0 + 0.5 * 23.0) / 23.0 &&
                   unscaled.cockpit_lag[0].Samples() == 81.0 && unscaled.variants[0] == 45.0,
               "Engine validator: the cockpit's sums come back to the vectors as decoded");
    }
}

// The prior table: MSFS 2024's FB vectors on Direct3D 12; else Vulkan-style
// NDC (y down) for XR_EXT_frame_synthesis on any API and for
// XR_FB_space_warp from a Vulkan game; nothing for the rest.
void TestFrameGenEnginePrior() {
    const auto msfs = depthxr::FrameGenEnginePriorFor("FlightSimulator2024.exe", false, "D3D12");
    Expect(msfs && depthxr::SameFrameGenEngineConvention(*msfs, depthxr::FrameGenEngineConvention{}),
           "Engine prior: MSFS 2024 expects XR_FB_space_warp's convention");
    depthxr::FrameGenEngineConvention y_down;
    y_down.gain = {1.0f, -1.0f};
    for (const auto& [executable, ext, api] :
         {std::tuple<const char*, bool, const char*>{"FlightSimulator2024.exe", true, "D3D12"},
          {"StarCitizen.exe", true, "Vulkan"},
          {"StarCitizen.exe", false, "Vulkan"},
          {"SomeGame.exe", true, "D3D11"}}) {
        const auto prior = depthxr::FrameGenEnginePriorFor(executable, ext, api);
        Expect(prior && depthxr::SameFrameGenEngineConvention(*prior, y_down) && !prior->jitter,
               std::string("Engine prior: Vulkan-style NDC, y down, for ") + (ext ? "XR_EXT_frame_synthesis" : "XR_FB_space_warp") +
                   " from " + executable + " on " + api);
    }
    Expect(!depthxr::FrameGenEnginePriorFor("StarCitizen.exe", false, "D3D12") &&
               !depthxr::FrameGenEnginePriorFor("SomeGame.exe", false, "D3D11"),
           "Engine prior: none for XR_FB_space_warp from another Direct3D game");
    // The Vulkan bridge's switch.
    using depthxr::FrameGenEngineBridgeMode;
    Expect(depthxr::ParseFrameGenEngineBridgeMode("") == FrameGenEngineBridgeMode::kReleased &&
               depthxr::ParseFrameGenEngineBridgeMode("1") == FrameGenEngineBridgeMode::kReleased &&
               depthxr::ParseFrameGenEngineBridgeMode("0") == FrameGenEngineBridgeMode::kOff &&
               depthxr::ParseFrameGenEngineBridgeMode("Off") == FrameGenEngineBridgeMode::kOff &&
               depthxr::ParseFrameGenEngineBridgeMode("END") == FrameGenEngineBridgeMode::kEndFrame &&
               std::string(depthxr::ToString(FrameGenEngineBridgeMode::kEndFrame)) == "end",
           "Engine bridge: VECTORXR_FRAMEGEN_ENGINE_BRIDGE's values");
    // The lag test's acceleration: a steady turn is none, a change is.
    depthxr::FrameGenViewGeometry a, b, c;
    auto yaw = [](double degrees) {
        const double half = degrees * 3.14159265358979 / 360.0;
        return std::array<float, 4>{0.0f, static_cast<float>(std::sin(half)), 0.0f, static_cast<float>(std::cos(half))};
    };
    a.orientation = yaw(0.0);
    b.orientation = yaw(3.0);
    c.orientation = yaw(6.0);
    Expect(depthxr::FrameGenAngularAcceleration(c, b, a) < 1e-3, "Engine prior: a steady turn does not accelerate");
    c.orientation = yaw(4.0);
    Expect(std::abs(depthxr::FrameGenAngularAcceleration(c, b, a) - 2.0) < 1e-3, "Engine prior: 3 then 1 degree is 2");
}

// --- Stage 4: the world's motion from the application's movement ---------

using WorldVec = std::array<double, 3>;
using WorldQuat = std::array<double, 4>; // x, y, z, w
using WorldMatrix = std::array<std::array<double, 4>, 4>;

WorldQuat WorldAxisAngle(const WorldVec& axis, double radians) {
    const double length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const double s = std::sin(radians / 2.0) / length;
    return {axis[0] * s, axis[1] * s, axis[2] * s, std::cos(radians / 2.0)};
}

WorldQuat WorldMultiply(const WorldQuat& a, const WorldQuat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// A pose (unit quaternion, position) as a 4x4 matrix: camera to its space.
WorldMatrix WorldPose(const std::array<float, 4>& orientation, const std::array<float, 3>& position) {
    const double n = std::sqrt(static_cast<double>(orientation[0]) * orientation[0] +
                               static_cast<double>(orientation[1]) * orientation[1] +
                               static_cast<double>(orientation[2]) * orientation[2] +
                               static_cast<double>(orientation[3]) * orientation[3]);
    const double x = orientation[0] / n, y = orientation[1] / n, z = orientation[2] / n, w = orientation[3] / n;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), position[0]},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), position[1]},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), position[2]},
             {0.0, 0.0, 0.0, 1.0}}};
}

WorldMatrix WorldTimes(const WorldMatrix& a, const WorldMatrix& b) {
    WorldMatrix result{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k < 4; ++k) result[r][c] += a[r][k] * b[k][c];
        }
    }
    return result;
}

WorldMatrix WorldInverse(const WorldMatrix& m) {
    WorldMatrix result{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) result[r][c] = m[c][r];
        result[r][3] = -(m[0][r] * m[0][3] + m[1][r] * m[1][3] + m[2][r] * m[2][3]);
    }
    result[3][3] = 1.0;
    return result;
}

WorldVec WorldApply(const WorldMatrix& m, const WorldVec& p) {
    return {m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3],
            m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3],
            m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3]};
}

std::array<float, 4> WorldFloats(const WorldQuat& q) {
    return {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]), static_cast<float>(q[3])};
}

// A view of the scenes below: a Pimax-like field of view (wider outward).
depthxr::FrameGenViewGeometry WorldView(const WorldQuat& orientation, const WorldVec& position, int eye) {
    depthxr::FrameGenViewGeometry view;
    view.orientation = WorldFloats(orientation);
    view.position = {static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])};
    view.angle_left = eye ? -0.9f : -1.1f;
    view.angle_right = eye ? 1.1f : 0.9f;
    view.angle_up = 0.95f;
    view.angle_down = -1.0f;
    return view;
}

// One frame pair of a scene: per eye, the poses the game rendered the frame
// and the one before with (an eye 32 mm to each side of a head, canted
// outward by `cant`), the poses declared to Frame Generation (Pivot's change
// times the game's, `pivot_*` at each frame), and the application's movement.
struct WorldScene {
    std::array<depthxr::FrameGenViewGeometry, 2> game_newest, game_previous, newest, previous;
    std::array<float, 4> delta_orientation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 3> delta_position{};
};

struct WorldPivot {
    WorldQuat orientation{0.0, 0.0, 0.0, 1.0};
    WorldVec position{};
};

WorldScene MakeWorldScene(const WorldQuat& head_newest, const WorldVec& at_newest, const WorldQuat& head_previous,
                          const WorldVec& at_previous, double cant, const WorldPivot& pivot_newest,
                          const WorldPivot& pivot_previous, const WorldQuat& delta, const WorldVec& moved) {
    WorldScene scene;
    auto eye = [&](const WorldQuat& head, const WorldVec& at, int e) {
        const WorldQuat orientation = WorldMultiply(head, WorldAxisAngle({0.0, 1.0, 0.0}, e ? -cant : cant));
        const WorldMatrix rotation = WorldPose(WorldFloats(head), {0.0f, 0.0f, 0.0f});
        const WorldVec side = WorldApply(rotation, {e ? 0.032 : -0.032, 0.0, 0.0});
        return std::make_pair(orientation, WorldVec{at[0] + side[0], at[1] + side[1], at[2] + side[2]});
    };
    auto declared = [](const WorldPivot& pivot, const std::pair<WorldQuat, WorldVec>& game, int e) {
        const WorldMatrix change = WorldPose(WorldFloats(pivot.orientation), {0.0f, 0.0f, 0.0f});
        const WorldVec moved_by = WorldApply(change, game.second);
        return WorldView(WorldMultiply(pivot.orientation, game.first),
                         {moved_by[0] + pivot.position[0], moved_by[1] + pivot.position[1], moved_by[2] + pivot.position[2]},
                         e);
    };
    for (int e = 0; e < 2; ++e) {
        const auto now = eye(head_newest, at_newest, e);
        const auto before = eye(head_previous, at_previous, e);
        scene.game_newest[e] = WorldView(now.first, now.second, e);
        scene.game_previous[e] = WorldView(before.first, before.second, e);
        scene.newest[e] = declared(pivot_newest, now, e);
        scene.previous[e] = declared(pivot_previous, before, e);
    }
    scene.delta_orientation = WorldFloats(delta);
    scene.delta_position = {static_cast<float>(moved[0]), static_cast<float>(moved[1]), static_cast<float>(moved[2])};
    return scene;
}

// FrameGenEngineWorldParams against a reference from 4x4 pose matrices: a
// point y of the newest declared view (the head held) is at
// T_gN^-1 T_D T_gP T_dP^-1 T_dN y at the newest frame (T_g, T_d: the game's
// and the declared poses, T_D the application's movement). Then frames
// rendered from the poses: the flow Frame Generation measures (the image's
// correspondence with the declared head movement taken out, as
// MapToNewest takes it out) is the model's, exactly and as FG_PASS_FILTER's
// WorldFlow has it (small-angle).
void TestFrameGenEngineWorldParams() {
    constexpr double kDegree = 3.14159265358979 / 180.0;
    const WorldQuat head_newest = WorldMultiply(WorldAxisAngle({0.0, 1.0, 0.0}, 30.0 * kDegree),
                                                WorldAxisAngle({1.0, 0.0, 0.0}, -5.0 * kDegree));
    const WorldQuat head_previous = WorldMultiply(WorldAxisAngle({0.0, 1.0, 0.0}, 29.5 * kDegree),
                                                  WorldAxisAngle({1.0, 0.0, 0.0}, -4.8 * kDegree));
    const WorldVec at_newest{0.02, 1.2, 0.01}, at_previous{0.0, 1.21, 0.0};
    // 1.8 m a frame forward (world points come toward +Z in the
    // application's space), banking gently.
    const WorldQuat bank = WorldAxisAngle({0.1, 1.0, 0.2}, 0.3 * kDegree);
    const WorldVec forward{0.0, 0.0, 1.8};
    const WorldPivot pivot{WorldAxisAngle({0.0, 1.0, 0.0}, 15.0 * kDegree), {0.1, -0.05, 0.3}};
    const WorldPivot pivot_ramp{WorldAxisAngle({0.0, 1.0, 0.0}, 14.5 * kDegree), {0.09, -0.05, 0.29}};
    struct Case {
        const char* name;
        WorldPivot before;
        float scale;
    };
    const std::array<Case, 3> cases{{{"Pivot's change held", pivot, 1.0f},
                                     {"Pivot's change ramping", pivot_ramp, 1.0f},
                                     {"s_t 1.1", pivot, 1.1f}}};
    for (const Case& c : cases) {
        const WorldScene scene =
            MakeWorldScene(head_newest, at_newest, head_previous, at_previous, 10.0 * kDegree, pivot, c.before, bank, forward);
        for (int e = 0; e < 2; ++e) {
            const depthxr::FrameGenEngineWorld world = depthxr::FrameGenEngineWorldParams(
                scene.game_newest[e], scene.game_previous[e], scene.newest[e], scene.previous[e], scene.delta_orientation,
                scene.delta_position, 1, c.scale);
            const WorldMatrix reference = WorldTimes(
                WorldInverse(WorldPose(scene.game_newest[e].orientation, scene.game_newest[e].position)),
                WorldTimes(WorldPose(scene.delta_orientation, scene.delta_position),
                           WorldTimes(WorldPose(scene.game_previous[e].orientation, scene.game_previous[e].position),
                                      WorldTimes(WorldInverse(WorldPose(scene.previous[e].orientation, scene.previous[e].position)),
                                                 WorldPose(scene.newest[e].orientation, scene.newest[e].position)))));
            const WorldMatrix rotation = WorldPose(world.quaternion, {0.0f, 0.0f, 0.0f});
            double worst = 0.0;
            for (int r = 0; r < 3; ++r) {
                for (int k = 0; k < 3; ++k) worst = (std::max)(worst, std::abs(rotation[r][k] - reference[r][k]));
                worst = (std::max)(worst, std::abs(world.translation[r] - c.scale * reference[r][3]));
                worst = (std::max)(worst, std::abs(world.rotation[r] - 2.0 * world.quaternion[r]));
            }
            Expect(world.valid && world.quaternion[3] >= 0.0f && worst < 1e-5,
                   std::string("World delta: the model matches the 4x4 reference (") + c.name + ", eye " +
                       std::to_string(e) + ": " + std::to_string(worst) + ")");
        }
    }

    // No movement of the application's space: the world holds still in the
    // newest view whatever the head and a steady Pivot do.
    const WorldScene still = MakeWorldScene(head_newest, at_newest, head_previous, at_previous, 10.0 * kDegree, pivot, pivot,
                                            {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0});
    for (int e = 0; e < 2; ++e) {
        const depthxr::FrameGenEngineWorld world =
            depthxr::FrameGenEngineWorldParams(still.game_newest[e], still.game_previous[e], still.newest[e],
                                               still.previous[e], still.delta_orientation, still.delta_position, 1, 1.0f);
        double largest = 0.0;
        for (int i = 0; i < 3; ++i) {
            largest = (std::max)({largest, std::abs(static_cast<double>(world.rotation[i])),
                                  std::abs(static_cast<double>(world.translation[i]))});
        }
        Expect(world.valid && largest < 1e-6,
               "World delta: an identity delta gives no world motion (" + std::to_string(largest) + ")");
    }

    // Sign -1 is the delta's rotation conjugated.
    const WorldScene turning = MakeWorldScene(head_newest, at_newest, head_previous, at_previous, 10.0 * kDegree, pivot,
                                              pivot_ramp, bank, forward);
    const std::array<float, 4> conjugated{-turning.delta_orientation[0], -turning.delta_orientation[1],
                                          -turning.delta_orientation[2], turning.delta_orientation[3]};
    const depthxr::FrameGenEngineWorld minus = depthxr::FrameGenEngineWorldParams(
        turning.game_newest[0], turning.game_previous[0], turning.newest[0], turning.previous[0], turning.delta_orientation,
        turning.delta_position, -1, 1.0f);
    const depthxr::FrameGenEngineWorld conjugate = depthxr::FrameGenEngineWorldParams(
        turning.game_newest[0], turning.game_previous[0], turning.newest[0], turning.previous[0], conjugated,
        turning.delta_position, 1, 1.0f);
    const depthxr::FrameGenEngineWorld plus = depthxr::FrameGenEngineWorldParams(
        turning.game_newest[0], turning.game_previous[0], turning.newest[0], turning.previous[0], turning.delta_orientation,
        turning.delta_position, 1, 1.0f);
    Expect(minus.rotation == conjugate.rotation && minus.translation == conjugate.translation &&
               minus.quaternion == conjugate.quaternion && minus.rotation != plus.rotation,
           "World delta: sign -1 conjugates the delta's rotation");
    // Not finite: no model.
    const depthxr::FrameGenEngineWorld broken = depthxr::FrameGenEngineWorldParams(
        turning.game_newest[0], turning.game_previous[0], turning.newest[0], turning.previous[0], turning.delta_orientation,
        {std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.8f}, 1, 1.0f);
    Expect(!broken.valid, "World delta: a delta not finite gives no model");

    // Rendered: world points at 300 m to 5 km seen at the newest frame,
    // where the game rendered them the frame before; Frame Generation's
    // flow there is uv - MapToNewest(where they were, their depth) by the
    // declared poses. The model's flow is the point moved back by the world's
    // motion, projected (exact), or WorldRows' (the filter's, small-angle).
    for (const Case& c : {cases[0], cases[1]}) {
        const WorldScene scene =
            MakeWorldScene(head_newest, at_newest, head_previous, at_previous, 10.0 * kDegree, pivot, c.before, bank, forward);
        double exact_worst = 0.0, linear_worst = 0.0;
        for (int e = 0; e < 2; ++e) {
            const depthxr::FrameGenViewGeometry& game_now = scene.game_newest[e];
            const depthxr::FrameGenViewGeometry& game_then = scene.game_previous[e];
            const depthxr::FrameGenEngineWorld world = depthxr::FrameGenEngineWorldParams(
                game_now, game_then, scene.newest[e], scene.previous[e], scene.delta_orientation, scene.delta_position, 1, 1.0f);
            const auto mapping = depthxr::BuildFrameGenViewMapping(scene.newest[e], scene.previous[e], true);
            const WorldMatrix camera_now = WorldPose(game_now.orientation, game_now.position);
            const WorldMatrix camera_then = WorldPose(game_then.orientation, game_then.position);
            const WorldMatrix moved = WorldPose(scene.delta_orientation, scene.delta_position);
            const WorldMatrix motion = WorldPose(world.quaternion, world.translation);
            const double l = std::tan(game_now.angle_left), r = std::tan(game_now.angle_right);
            const double u = std::tan(game_now.angle_up), d = std::tan(game_now.angle_down);
            for (int y = 1; y < 10; y += 2) {
                for (int x = 1; x < 10; x += 2) {
                    for (const double depth : {300.0, 1000.0, 5000.0}) {
                        const std::array<float, 2> uv{x / 10.0f, y / 10.0f};
                        const WorldVec point{(l + (r - l) * uv[0]) * depth, (u + (d - u) * uv[1]) * depth, -depth};
                        // Where the game rendered it the frame before.
                        const WorldVec seen = WorldApply(
                            WorldInverse(camera_then), WorldApply(WorldInverse(moved), WorldApply(camera_now, point)));
                        const std::array<float, 2> before{static_cast<float>((seen[0] / -seen[2] - l) / (r - l)),
                                                          static_cast<float>((seen[1] / -seen[2] - u) / (d - u))};
                        std::array<float, 2> back{};
                        Expect(depthxr::FrameGenMapToNewest(mapping, before, static_cast<float>(depth), back),
                               "World delta: the point maps back to the newest view");
                        const double flow[2] = {uv[0] - back[0], uv[1] - back[1]};
                        // The model: the point moved back by the world's motion.
                        const WorldVec held = WorldApply(WorldInverse(motion), point);
                        const double exact[2] = {uv[0] - (held[0] / -held[2] - l) / (r - l),
                                                 uv[1] - (held[1] / -held[2] - u) / (d - u)};
                        exact_worst = (std::max)({exact_worst, std::abs(flow[0] - exact[0]), std::abs(flow[1] - exact[1])});
                        // WorldRows at the newest tangent and inverse depth.
                        const double ax = l + (r - l) * uv[0], ay = u + (d - u) * uv[1], inv_z = 1.0 / depth;
                        const auto& w = world.rotation;
                        const auto& t = world.translation;
                        const double dx = ax * ay * w[0] + (-1.0 - ax * ax) * w[1] - ay * w[2] + inv_z * t[0] + ax * inv_z * t[2];
                        const double dy = (1.0 + ay * ay) * w[0] - ax * ay * w[1] + ax * w[2] + inv_z * t[1] + ay * inv_z * t[2];
                        const double linear[2] = {dx / (r - l), dy / (d - u)};
                        // Within tangent 1 of the view's axis (further out
                        // the small-angle form drifts: 1.2e-4 uv at 1.6).
                        if (c.before.orientation == pivot.orientation && std::abs(ax) <= 1.0 && std::abs(ay) <= 1.0) {
                            linear_worst =
                                (std::max)({linear_worst, std::abs(flow[0] - linear[0]), std::abs(flow[1] - linear[1])});
                        }
                    }
                }
            }
        }
        Expect(exact_worst < 1e-5, std::string("World delta: rendered frames move by the model exactly (") + c.name + ": " +
                                       std::to_string(exact_worst) + " uv)");
        Expect(linear_worst < 1e-4, std::string("World delta: and by the filter's small-angle model within 1e-4 uv (") +
                                        c.name + ": " + std::to_string(linear_worst) + " uv)");
    }
}

// The plan: kWorldSet in place of the fit where the backend's calibration
// locked (FrameGenTuning::world_delta) and the request has the application's
// movement, after the fit on the watchdog's frames (delta_watchdog); the
// cover of just-in-time interpolation reads the same, and the
// sky flag goes on the extrapolating filters with world_sky. Without
// world_delta the plan is as without the movement, byte for byte.
void TestFrameGenWorldDeltaPlan() {
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    constexpr double kDegree = 3.14159265358979 / 180.0;
    const WorldScene scene = MakeWorldScene(WorldAxisAngle({0.0, 1.0, 0.0}, 30.0 * kDegree), {0.0, 1.2, 0.0},
                                            WorldAxisAngle({0.0, 1.0, 0.0}, 29.0 * kDegree), {0.0, 1.2, 0.0}, 0.0, {}, {},
                                            WorldAxisAngle({0.0, 1.0, 0.0}, 0.3 * kDegree), {0.0, 0.0, 1.8});
    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = request.temporal = request.depth = request.world_motion = true;
    request.newest = scene.newest[0];
    request.previous = scene.previous[0];
    request.game_newest = scene.game_newest[0];
    request.game_previous = scene.game_previous[0];
    depthxr::FrameGenTuning tuning;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.analyze_only = true;
    const depthxr::FrameGenViewPlan plain = depthxr::BuildFrameGenViewPlan(request, tuning);
    auto same = [](const depthxr::FrameGenViewPlan& a, const depthxr::FrameGenViewPlan& b) {
        bool equal = a.dispatches.size() == b.dispatches.size();
        for (std::size_t i = 0; equal && i < a.dispatches.size(); ++i) {
            equal = a.dispatches[i].pass == b.dispatches[i].pass &&
                    std::memcmp(a.dispatches[i].inputs.data(), b.dispatches[i].inputs.data(), sizeof(a.dispatches[i].inputs)) == 0 &&
                    std::memcmp(a.dispatches[i].outputs.data(), b.dispatches[i].outputs.data(), sizeof(a.dispatches[i].outputs)) == 0 &&
                    std::memcmp(&a.dispatches[i].constants, &b.dispatches[i].constants, sizeof(depthxr::FrameGenConstants)) == 0;
        }
        return equal;
    };
    auto count = [](const depthxr::FrameGenViewPlan& plan, FrameGenPass pass) {
        return std::count_if(plan.dispatches.begin(), plan.dispatches.end(),
                             [&](const depthxr::FrameGenDispatch& dispatch) { return dispatch.pass == pass; });
    };
    auto filters = [](const depthxr::FrameGenViewPlan& plan) {
        std::vector<depthxr::FrameGenDispatch> found;
        for (const auto& dispatch : plan.dispatches) {
            if (dispatch.pass == FrameGenPass::kFilter) found.push_back(dispatch);
        }
        return found;
    };
    request.app_delta = true;
    request.delta_orientation = scene.delta_orientation;
    request.delta_position = scene.delta_position;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), plain),
           "World delta plan: the movement alone changes nothing (the backend's calibration has not locked)");
    tuning.world_sky = true;
    tuning.world_delta = true;
    tuning.delta_sign = -1;
    tuning.delta_scale = 1.1f;
    const depthxr::FrameGenViewPlan set = depthxr::BuildFrameGenViewPlan(request, tuning);
    const auto set_filters = filters(set);
    const auto world_set = std::find_if(set.dispatches.begin(), set.dispatches.end(),
                                        [](const auto& dispatch) { return dispatch.pass == FrameGenPass::kWorldSet; });
    const depthxr::FrameGenEngineWorld expected = depthxr::FrameGenEngineWorldParams(
        request.game_newest, request.game_previous, request.newest, request.previous, request.delta_orientation,
        request.delta_position, -1, 1.1f);
    Expect(count(set, FrameGenPass::kWorldSet) == 1 && count(set, FrameGenPass::kWorldReduce) == 0 &&
               count(set, FrameGenPass::kWorldSolve) == 0 && count(plain, FrameGenPass::kWorldSolve) == 3 &&
               set.dispatches.size() + 5 == plain.dispatches.size(),
           "World delta plan: kWorldSet stands in for the fit's three iterations");
    Expect(world_set != set.dispatches.end() && world_set->outputs[0].kind == FrameGenTextureKind::kWorldParams &&
               world_set->width == 1 && world_set->height == 1 &&
               world_set->constants.fov_current ==
                   std::array<float, 4>{expected.rotation[0], expected.rotation[1], expected.rotation[2], 1.0f} &&
               world_set->constants.fov_previous ==
                   std::array<float, 4>{expected.translation[0], expected.translation[1], expected.translation[2], 1.0f} &&
               world_set->constants.rot0 == std::array<float, 4>{1.1f, 1.0f, -1.0f, 0.0f} &&
               world_set->constants.rot1 == expected.quaternion,
           "World delta plan: kWorldSet writes the model's rotation, translation, source, sign, scale and quaternion");
    Expect(set_filters.size() == 1 && set_filters[0].inputs[3].kind == FrameGenTextureKind::kWorldParams &&
               (set_filters[0].constants.options[1] & depthxr::kFrameGenFlagWorld) != 0 &&
               (set_filters[0].constants.options[1] & depthxr::kFrameGenFlagWorldSky) != 0 &&
               world_set < std::find_if(set.dispatches.begin(), set.dispatches.end(),
                                        [](const auto& dispatch) { return dispatch.pass == FrameGenPass::kFilter; }),
           "World delta plan: the filter reads it after it is set, the sky flag with world_sky");
    // The watchdog's frames: the fit runs as well, and the set after it, so
    // the filter reads the same motion as on every other frame.
    tuning.delta_watchdog = true;
    const depthxr::FrameGenViewPlan watched = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::size_t last_solve = 0, watched_set = 0, watched_filter = 0;
    for (std::size_t i = 0; i < watched.dispatches.size(); ++i) {
        const FrameGenPass pass = watched.dispatches[i].pass;
        if (pass == FrameGenPass::kWorldSolve) last_solve = i;
        if (pass == FrameGenPass::kWorldSet) watched_set = i;
        if (pass == FrameGenPass::kFilter && watched_filter == 0) watched_filter = i;
    }
    Expect(count(watched, FrameGenPass::kWorldSolve) == 3 && count(watched, FrameGenPass::kWorldSet) == 1 &&
               watched.dispatches.size() == plain.dispatches.size() + 1 && last_solve < watched_set &&
               watched_set < watched_filter &&
               std::memcmp(&watched.dispatches[watched_set].constants, &world_set->constants,
                           sizeof(depthxr::FrameGenConstants)) == 0,
           "World delta plan: on the watchdog's frames the fit runs, then the set, then the filter");
    tuning.delta_watchdog = false;
    tuning.world_sky = false;
    Expect((filters(depthxr::BuildFrameGenViewPlan(request, tuning))[0].constants.options[1] &
            depthxr::kFrameGenFlagWorldSky) == 0,
           "World delta plan: no sky flag without world_sky");
    // A request without the movement, or a model not finite: the fit.
    request.app_delta = false;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), plain), "World delta plan: the fit without the movement");
    tuning.delta_watchdog = true;
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), plain),
           "World delta plan: the fit alone without the movement, on the watchdog's frames too");
    tuning.delta_watchdog = false;
    request.app_delta = true;
    request.delta_position[0] = std::numeric_limits<float>::quiet_NaN();
    Expect(same(depthxr::BuildFrameGenViewPlan(request, tuning), plain), "World delta plan: the fit for a broken delta");
    request.delta_position = scene.delta_position;
    // Interpolating just in time: the main filter keeps the measured flow
    // (no world model, no sky); the cover reads the set motion, sky and all.
    tuning.mode = depthxr::FrameGenSynthesisMode::kInterpolate;
    tuning.cover = true;
    tuning.world_sky = true;
    const depthxr::FrameGenViewPlan interpolated = depthxr::BuildFrameGenViewPlan(request, tuning);
    const auto interpolated_filters = filters(interpolated);
    Expect(count(interpolated, FrameGenPass::kWorldSet) == 1 && count(interpolated, FrameGenPass::kWorldSolve) == 0 &&
               interpolated_filters.size() == 2 &&
               (interpolated_filters[0].constants.options[1] & (depthxr::kFrameGenFlagWorld | depthxr::kFrameGenFlagWorldSky)) == 0 &&
               interpolated_filters[1].outputs[0].kind == FrameGenTextureKind::kFilteredCover &&
               (interpolated_filters[1].constants.options[1] & depthxr::kFrameGenFlagWorldSky) != 0 &&
               interpolated_filters[1].inputs[3].kind == FrameGenTextureKind::kWorldParams,
           "World delta plan: interpolating, only the cover reads the set motion");
    tuning.world_delta = tuning.world_sky = false;
    request.app_delta = false;
    const depthxr::FrameGenViewPlan interpolated_plain = depthxr::BuildFrameGenViewPlan(request, tuning);
    Expect(count(interpolated_plain, FrameGenPass::kWorldSolve) == 3 &&
               (filters(interpolated_plain)[1].constants.options[1] & depthxr::kFrameGenFlagWorldSky) == 0,
           "World delta plan: interpolating without it, the cover's fit as before");
}

// The calibration of the application's movement against the world fit, on
// fits made from the model (a sign and a scale, a little noise): a sign
// and s_t found once 300 informative frames with turns came; none without
// turns, nor at a scale out of range; locked, the watchdog drops it back
// after 10 checks in a row 25% off, a check within resetting the count.
void TestFrameGenWorldDeltaCalibration() {
    using depthxr::FrameGenWorldDeltaMode;
    Expect(depthxr::ParseFrameGenWorldDeltaMode("calibrate") == FrameGenWorldDeltaMode::kCalibrate &&
               depthxr::ParseFrameGenWorldDeltaMode("Calibrate") == FrameGenWorldDeltaMode::kCalibrate &&
               depthxr::ParseFrameGenWorldDeltaMode("1") == FrameGenWorldDeltaMode::kOn &&
               depthxr::ParseFrameGenWorldDeltaMode("0") == FrameGenWorldDeltaMode::kOff &&
               depthxr::ParseFrameGenWorldDeltaMode("") == FrameGenWorldDeltaMode::kOff,
           "World delta calibration: the switch's modes parse");
    const auto fixed = depthxr::ParseFrameGenWorldDeltaFixed(" -1, 1.05");
    Expect(fixed && fixed->first == -1 && std::abs(fixed->second - 1.05f) < 1e-6f &&
               !depthxr::ParseFrameGenWorldDeltaFixed("1") && !depthxr::ParseFrameGenWorldDeltaFixed("2,1") &&
               !depthxr::ParseFrameGenWorldDeltaFixed("1,3") && !depthxr::ParseFrameGenWorldDeltaFixed("1,1,x"),
           "World delta calibration: a fixed sign and scale parse, others are refused");
    depthxr::FrameGenWorldDeltaCalibration fixed_calibration;
    fixed_calibration.Fix(-1, 1.05f);
    Expect(fixed_calibration.Locked() && fixed_calibration.Status().fixed && fixed_calibration.Sign() == -1 &&
               fixed_calibration.Scale() == 1.05f && fixed_calibration.Status().decisions == 1,
           "World delta calibration: fixed, locked from the start");

    constexpr double kDegree = 3.14159265358979 / 180.0;
    // Frame k: flying on 1.8 m a frame, banking 0.4 degrees a frame on
    // frames 100-159 when `turns`; the head still, no Pivot.
    auto frame_fits = [&](int k, bool turns, int sign, double scale, double off = 1.0) {
        const double turn = turns && k >= 100 && k < 160 ? 0.4 * kDegree : 0.0;
        const WorldQuat head = WorldAxisAngle({0.0, 1.0, 0.0}, 30.0 * kDegree);
        const WorldScene scene = MakeWorldScene(head, {0.0, 1.2, 0.0}, head, {0.0, 1.2, 0.0}, 10.0 * kDegree, {}, {},
                                                WorldAxisAngle({0.05, 1.0, 0.1}, turn), {0.0, 0.0, 1.8});
        std::vector<depthxr::FrameGenWorldFit> fits;
        for (int e = 0; e < 2; ++e) {
            depthxr::FrameGenWorldFit fit;
            fit.game_newest = scene.game_newest[e];
            fit.game_previous = scene.game_previous[e];
            fit.newest = scene.newest[e];
            fit.previous = scene.previous[e];
            fit.delta_orientation = scene.delta_orientation;
            fit.delta_position = scene.delta_position;
            const depthxr::FrameGenEngineWorld truth = depthxr::FrameGenEngineWorldParams(
                fit.game_newest, fit.game_previous, fit.newest, fit.previous, fit.delta_orientation, fit.delta_position,
                sign, static_cast<float>(scale));
            for (int i = 0; i < 3; ++i) {
                const double noise = std::sin(17.0 * k + 5.0 * i + 3.0 * e);
                fit.rotation[i] = truth.rotation[i] + static_cast<float>(1e-4 * noise);
                fit.translation[i] = static_cast<float>(off * truth.translation[i] + 0.02 * noise);
            }
            fit.valid = true;
            fits.push_back(fit);
        }
        return fits;
    };
    for (const int sign : {1, -1}) {
        depthxr::FrameGenWorldDeltaCalibration calibration;
        for (int k = 0; k < 299; ++k) calibration.Add(frame_fits(k, true, sign, 1.08));
        const bool early = calibration.Locked();
        calibration.Add(frame_fits(299, true, sign, 1.08));
        const depthxr::FrameGenWorldDeltaStatus& status = calibration.Status();
        Expect(!early && calibration.Locked() && calibration.Sign() == sign && std::abs(calibration.Scale() - 1.08f) < 0.01f &&
                   status.informative == 300 && status.turning == 60 && status.decisions == 1 &&
                   status.rotation_correlation[sign > 0 ? 0 : 1] > 0.99 && status.translation_correlation > 0.99,
               "World delta calibration: the sign and s_t lock after 300 informative frames (sign " + std::to_string(sign) +
                   ": " + status.reason + ")");
    }
    // No turns: the sign cannot be told.
    {
        depthxr::FrameGenWorldDeltaCalibration calibration;
        for (int k = 0; k < 400; ++k) calibration.Add(frame_fits(k, false, 1, 1.0));
        Expect(!calibration.Locked() && calibration.Status().informative == 400 && calibration.Status().turning == 0,
               "World delta calibration: straight flight alone never locks");
    }
    // s_t out of range (the depth or the delta far off): no lock.
    {
        depthxr::FrameGenWorldDeltaCalibration calibration;
        for (int k = 0; k < 400; ++k) calibration.Add(frame_fits(k, true, 1, 2.0));
        Expect(!calibration.Locked() && std::abs(calibration.Status().fitted_scale - 2.0) < 0.02,
               "World delta calibration: s_t 2 does not lock");
    }
    // Fits that did not solve count for nothing.
    {
        depthxr::FrameGenWorldDeltaCalibration calibration;
        auto fits = frame_fits(0, true, 1, 1.0);
        for (auto& fit : fits) fit.valid = false;
        calibration.Add(fits);
        Expect(calibration.Status().frames == 0, "World delta calibration: unsolved fits are left out");
    }
    // The watchdog.
    depthxr::FrameGenWorldDeltaCalibration calibration;
    calibration.Fix(1, 1.0f);
    for (int k = 0; k < 9; ++k) calibration.Add(frame_fits(k, false, 1, 1.0, 1.4));
    const bool held = calibration.Locked() && calibration.Status().failed == 9;
    calibration.Add(frame_fits(9, false, 1, 1.0));
    for (int k = 10; k < 19; ++k) calibration.Add(frame_fits(k, false, 1, 1.0, 1.4));
    const bool reset = calibration.Locked() && calibration.Status().checks == 19;
    calibration.Add(frame_fits(19, false, 1, 1.0, 1.4));
    const depthxr::FrameGenWorldDeltaStatus& dropped = calibration.Status();
    Expect(held && reset && !calibration.Locked() && !dropped.fixed && dropped.drops == 1 && dropped.decisions == 2 &&
               dropped.failed == 19 && std::abs(dropped.last_error - 0.4) < 0.05 && dropped.frames == 0,
           "World delta calibration: 10 checks in a row 25% off drop it back to the fit (" + dropped.reason + ")");
}

// Stage 7's plans: frames extrapolated from a real frame whose cells were
// labelled (its analysis merged the vectors in) build the engine motion map
// (the shift passes into kMotionMap with the labels and the world's motion),
// merged in place of the eye-shift map: one map per generated frame, four
// dispatches, and no eye-shift map; separate beside it. Read again for a
// later frame of the same real frame (no map built, its time in options.w's
// high half), and past one interval with engine_reach. Without labels, the
// switch off, interpolating or moved by an older frame's motion, the plans as
// before.
void TestFrameGenEngineMapPlan() {
    using depthxr::FrameGenEngineMap;
    using depthxr::FrameGenPass;
    using depthxr::FrameGenTextureKind;
    Expect(depthxr::ParseFrameGenEngineMap("merged") == FrameGenEngineMap::kMerged &&
               depthxr::ParseFrameGenEngineMap("Separate") == FrameGenEngineMap::kSeparate &&
               depthxr::ParseFrameGenEngineMap("1") == FrameGenEngineMap::kOff &&
               depthxr::ParseFrameGenEngineMap("") == FrameGenEngineMap::kOff &&
               std::string(depthxr::ToString(FrameGenEngineMap::kMerged)) == "merged",
           "Engine map plan: VECTORXR_FRAMEGEN_ENGINE_MAP parses separate and merged, anything else off");
    depthxr::FrameGenViewRequest request;
    request.extent = {1024, 1024};
    request.synthesize = request.temporal = request.depth = true;
    for (auto* geometry : {&request.newest, &request.previous}) {
        geometry->angle_left = geometry->angle_down = -0.9f;
        geometry->angle_right = geometry->angle_up = 0.9f;
    }
    request.previous.orientation = {0.0f, 0.01f, 0.0f, 0.99995f};
    request.shift = {0.02f, 0.0f, 0.004f};
    request.engine_motion = true;
    depthxr::FrameGenTuning tuning;
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.analyze_only = true;
    tuning.engine_mode = depthxr::FrameGenEngineMode::kAssist;
    auto synthesis = [&](const depthxr::FrameGenTuning& with, float fraction, float map_time = 0.0f, bool stale = false,
                         depthxr::FrameGenSynthesisMode mode = depthxr::FrameGenSynthesisMode::kExtrapolate) {
        return depthxr::BuildFrameGenSynthesisPlan(request, with, mode, fraction, false, stale, map_time);
    };
    auto order = [](const depthxr::FrameGenViewPlan& plan) {
        std::string text;
        for (const auto& dispatch : plan.dispatches) {
            switch (dispatch.pass) {
            case FrameGenPass::kShiftClear: text += dispatch.outputs[1].kind == FrameGenTextureKind::kShiftMap ? "C " : "c "; break;
            case FrameGenPass::kShiftSplat: text += "s "; break;
            case FrameGenPass::kShiftResolve: text += "r "; break;
            case FrameGenPass::kShiftFill: text += "f "; break;
            case FrameGenPass::kShiftSplatEngine: text += "es "; break;
            case FrameGenPass::kShiftResolveEngine: text += "er "; break;
            case FrameGenPass::kShiftFillEngine: text += "ef "; break;
            case FrameGenPass::kSynthesizeUnorm8: text += "synth "; break;
            default: text += "? "; break;
            }
        }
        return text;
    };
    auto same = [](const depthxr::FrameGenViewPlan& a, const depthxr::FrameGenViewPlan& b) {
        bool equal = a.dispatches.size() == b.dispatches.size();
        for (std::size_t i = 0; equal && i < a.dispatches.size(); ++i) {
            equal = a.dispatches[i].pass == b.dispatches[i].pass &&
                    std::memcmp(a.dispatches[i].inputs.data(), b.dispatches[i].inputs.data(), sizeof(a.dispatches[i].inputs)) == 0 &&
                    std::memcmp(a.dispatches[i].outputs.data(), b.dispatches[i].outputs.data(), sizeof(a.dispatches[i].outputs)) == 0 &&
                    std::memcmp(&a.dispatches[i].constants, &b.dispatches[i].constants, sizeof(depthxr::FrameGenConstants)) == 0;
        }
        return equal;
    };
    const depthxr::FrameGenViewPlan mk1 = synthesis(tuning, 0.5f);
    tuning.depth_splat = true;
    const depthxr::FrameGenViewPlan m3 = synthesis(tuning, 0.5f);
    tuning.depth_splat = false;
    Expect(order(mk1) == "C s r f synth " && order(m3) == "C s r f c s r f synth " &&
               !depthxr::FrameGenEngineMapTime(mk1) && !depthxr::FrameGenEngineMapTime(m3),
           "Engine map plan: without the switch the eye-shift map (and with the depth splat the motion map), as before (" +
               order(m3) + ")");

    // Merged: one map, the eye's offset in it, no eye-shift map.
    tuning.engine_map = FrameGenEngineMap::kMerged;
    const depthxr::FrameGenViewPlan merged = synthesis(tuning, 0.5f);
    bool shift_map = false;
    for (const auto& dispatch : merged.dispatches) {
        for (const auto& texture : dispatch.inputs) shift_map = shift_map || texture.kind == FrameGenTextureKind::kShiftMap;
        for (const auto& texture : dispatch.outputs) shift_map = shift_map || texture.kind == FrameGenTextureKind::kShiftMap;
    }
    const depthxr::FrameGenDispatch& splat = merged.dispatches[1];
    const depthxr::FrameGenDispatch& resolve = merged.dispatches[2];
    const depthxr::FrameGenDispatch& fill = merged.dispatches[3];
    const depthxr::FrameGenDispatch& output = merged.dispatches.back();
    const std::uint32_t map_flags = depthxr::kFrameGenFlagDepth | depthxr::kFrameGenFlagDepthSplat | depthxr::kFrameGenFlagEngine;
    Expect(order(merged) == "c es er ef synth " && !shift_map && merged.dispatches.size() == 5 &&
               merged.dispatches[0].outputs[0].kind == FrameGenTextureKind::kShiftKeys &&
               merged.dispatches[0].outputs[1].kind == FrameGenTextureKind::kMotionMap,
           "Engine map plan: merged, one map per generated frame (4 dispatches) and no eye-shift map (" + order(merged) + ")");
    Expect(splat.constants.rot0 == std::array<float, 4>{1.0f, 0.0f, 0.0f, -0.01f} &&
               splat.constants.rot1 == std::array<float, 4>{0.0f, 1.0f, 0.0f, 0.0f} &&
               splat.constants.rot2 == std::array<float, 4>{0.0f, 0.0f, 1.0f, -0.002f} &&
               splat.constants.fov_previous == splat.constants.fov_current &&
               splat.constants.options[0] == depthxr::kFrameGenEngineMapMerged && splat.constants.options[1] == map_flags &&
               splat.constants.tuning[1] == -0.5f && splat.constants.tuning[2] == 0.1f && splat.width == 512 &&
               splat.height == 512,
           "Engine map plan: merged, the eye-shift map's mapping (the eye's offset at the frame's time), the step, the "
           "motion limit and the engine flags");
    Expect(splat.inputs[0].kind == FrameGenTextureKind::kDepth &&
               splat.inputs[1].kind == FrameGenTextureKind::kFilteredNewest && splat.inputs[1].level == merged.final_flow_level &&
               splat.inputs[3].kind == FrameGenTextureKind::kNone && splat.inputs[4].kind == FrameGenTextureKind::kEngineLabels &&
               resolve.inputs[2].kind == FrameGenTextureKind::kShiftKeys &&
               resolve.inputs[4].kind == FrameGenTextureKind::kEngineLabels &&
               fill.inputs[0].kind == FrameGenTextureKind::kShiftKeys &&
               fill.inputs[1].kind == FrameGenTextureKind::kFilteredNewest,
           "Engine map plan: the labels at input 4, the filtered flow at 1, no world motion without the real frame's");
    Expect(output.inputs[4].kind == FrameGenTextureKind::kNone && output.inputs[5].kind == FrameGenTextureKind::kMotionMap &&
               (output.constants.options[1] & (depthxr::kFrameGenFlagDepthSplat | depthxr::kFrameGenFlagEngine)) ==
                   (depthxr::kFrameGenFlagDepthSplat | depthxr::kFrameGenFlagEngine) &&
               (output.constants.options[1] & depthxr::kFrameGenFlagShift) == 0 && output.constants.options[3] == 0u &&
               depthxr::FrameGenEngineMapTime(merged) == 0.5f,
           "Engine map plan: merged, synthesis reads the map at input 5, no eye shift of its own");
    request.engine_world = true;
    const depthxr::FrameGenViewPlan worlded = synthesis(tuning, 0.5f);
    request.engine_world = false;
    Expect(worlded.dispatches[1].inputs[3].kind == FrameGenTextureKind::kWorldParamsNewest &&
               worlded.dispatches[2].inputs[3].kind == FrameGenTextureKind::kWorldParamsNewest &&
               worlded.dispatches[1].constants.options[0] ==
                   (depthxr::kFrameGenEngineMapWorld | depthxr::kFrameGenEngineMapMerged),
           "Engine map plan: the real frame's world motion kept (kWorldParamsNewest) at input 3");

    // Separate: the eye-shift map, then the engine motion map from the
    // plan's own constants.
    tuning.engine_map = FrameGenEngineMap::kSeparate;
    const depthxr::FrameGenViewPlan separate = synthesis(tuning, 0.5f);
    const depthxr::FrameGenDispatch& separate_splat = separate.dispatches[5];
    const depthxr::FrameGenDispatch& separate_output = separate.dispatches.back();
    Expect(order(separate) == "C s r f c es er ef synth " && separate_splat.constants.options[0] == 0u &&
               separate_splat.constants.options[1] == map_flags && separate_splat.constants.tuning[1] == -0.5f &&
               separate_splat.constants.rot0 == m3.dispatches[5].constants.rot0 &&
               separate_splat.inputs[4].kind == FrameGenTextureKind::kEngineLabels &&
               separate_output.inputs[4].kind == FrameGenTextureKind::kShiftMap &&
               separate_output.inputs[5].kind == FrameGenTextureKind::kMotionMap &&
               (separate_output.constants.options[1] & depthxr::kFrameGenFlagEngine) == 0 &&
               (separate_output.constants.options[1] & depthxr::kFrameGenFlagShift) != 0,
           "Engine map plan: separate, the eye-shift map and the engine motion map (" + order(separate) + ")");

    // Read again: no map built, the time it was built for in options.w's high
    // half (separate: the eye-shift map's own time in the low half).
    tuning.engine_map = FrameGenEngineMap::kMerged;
    const depthxr::FrameGenViewPlan reused = synthesis(tuning, 0.5f, 0.25f);
    tuning.engine_map = FrameGenEngineMap::kSeparate;
    const depthxr::FrameGenViewPlan reused_separate = synthesis(tuning, 0.5f, 0.25f);
    Expect(order(reused) == "synth " && reused.dispatches.back().inputs[5].kind == FrameGenTextureKind::kMotionMap &&
               reused.dispatches.back().constants.options[3] == 0x3400u << 16 && !depthxr::FrameGenEngineMapTime(reused) &&
               order(reused_separate) == "C s r f synth " &&
               reused_separate.dispatches.back().constants.options[3] == ((0x3400u << 16) | 0x3800u),
           "Engine map plan: a map read again is not built; its time in options.w's high half (" + order(reused) + ")");

    // Past one interval with the reach: the frame's fraction (the eye's
    // offset and the map's step with it) up to engine_reach.
    tuning.engine_map = FrameGenEngineMap::kMerged;
    tuning.engine_reach = 1.5f;
    const depthxr::FrameGenViewPlan reached = synthesis(tuning, 1.4f);
    const depthxr::FrameGenViewPlan capped = synthesis(tuning, 2.5f);
    Expect(reached.dispatches.back().constants.tuning[0] == 1.4f && reached.dispatches[1].constants.tuning[1] == -1.4f &&
               std::abs(reached.dispatches[1].constants.rot0[3] + 0.028f) < 1e-6f &&
               capped.dispatches.back().constants.tuning[0] == 1.5f,
           "Engine map plan: past one interval up to the reach");
    request.engine_motion = false;
    const depthxr::FrameGenViewPlan unlabelled = synthesis(tuning, 1.4f);
    request.engine_motion = true;
    const depthxr::FrameGenViewPlan stale = synthesis(tuning, 1.4f, 0.0f, true);
    Expect(unlabelled.dispatches.back().constants.tuning[0] == 1.0f && order(unlabelled) == "C s r f synth " &&
               stale.dispatches.back().constants.tuning[0] == 1.0f && !depthxr::FrameGenEngineMapTime(stale),
           "Engine map plan: one interval at most without labels, or moved by an older frame's motion");
    tuning.engine_reach = 1.0f;

    // Without labels (Observe), interpolating, or the switch off: as before.
    tuning.engine_mode = depthxr::FrameGenEngineMode::kObserve;
    const depthxr::FrameGenViewPlan observed = synthesis(tuning, 0.5f);
    tuning.engine_mode = depthxr::FrameGenEngineMode::kAssist;
    request.engine_motion = false;
    const depthxr::FrameGenViewPlan without = synthesis(tuning, 0.5f);
    request.engine_motion = true;
    tuning.mode = depthxr::FrameGenSynthesisMode::kInterpolate;
    tuning.cover = true;
    tuning.engine_map = FrameGenEngineMap::kOff;
    const depthxr::FrameGenViewPlan interpolated_mk1 =
        synthesis(tuning, 0.5f, 0.0f, false, depthxr::FrameGenSynthesisMode::kInterpolate);
    tuning.engine_map = FrameGenEngineMap::kMerged;
    const depthxr::FrameGenViewPlan interpolated =
        synthesis(tuning, 0.5f, 0.0f, false, depthxr::FrameGenSynthesisMode::kInterpolate);
    // A cover frame (extrapolated from an interpolating analysis) takes it.
    const depthxr::FrameGenViewPlan cover = synthesis(tuning, 0.5f);
    tuning.mode = depthxr::FrameGenSynthesisMode::kExtrapolate;
    tuning.cover = false;
    Expect(same(observed, mk1) && same(without, mk1) && same(interpolated, interpolated_mk1) &&
               order(cover) == "c es er ef synth ",
           "Engine map plan: Observe, a frame without labels and interpolated frames as without the switch; cover frames "
           "take the map");

    // Written together (not just in time): a merged map per generated frame,
    // with the world motion the analysis set or fitted itself.
    tuning.analyze_only = false;
    tuning.generated = 2;
    tuning.fractions = {0.5f, 1.0f, 0.5f, 0.5f};
    request.world_motion = true;
    const depthxr::FrameGenViewPlan together = depthxr::BuildFrameGenViewPlan(request, tuning);
    std::size_t maps = 0, outputs = 0;
    bool shift_together = false, world_together = true;
    for (const auto& dispatch : together.dispatches) {
        if (dispatch.pass == FrameGenPass::kShiftClear) {
            maps += dispatch.outputs[1].kind == FrameGenTextureKind::kMotionMap ? 1 : 0;
            shift_together = shift_together || dispatch.outputs[1].kind == FrameGenTextureKind::kShiftMap;
        }
        if (dispatch.pass == FrameGenPass::kShiftSplatEngine) {
            world_together = world_together && dispatch.inputs[3].kind == FrameGenTextureKind::kWorldParams &&
                             (dispatch.constants.options[0] & depthxr::kFrameGenEngineMapWorld) != 0;
        }
        outputs += dispatch.pass == FrameGenPass::kSynthesizeUnorm8 ? 1 : 0;
    }
    Expect(maps == 2 && outputs == 2 && !shift_together && world_together && together.frames_in_order,
           "Engine map plan: written together, a merged map per generated frame with kWorldParams");
    // The merge keeps the depth of each label's surface for the map (the
    // resolved depth at input 5); without the map it reads none, as before.
    tuning.engine_map = FrameGenEngineMap::kOff;
    const depthxr::FrameGenViewPlan unmapped = depthxr::BuildFrameGenViewPlan(request, tuning);
    tuning.engine_map = FrameGenEngineMap::kMerged;
    auto merge_of = [](const depthxr::FrameGenViewPlan& plan) {
        const auto found = std::find_if(plan.dispatches.begin(), plan.dispatches.end(),
                                        [](const depthxr::FrameGenDispatch& dispatch) { return dispatch.pass == FrameGenPass::kEngineMerge; });
        return found != plan.dispatches.end() ? *found : depthxr::FrameGenDispatch{};
    };
    const depthxr::FrameGenDispatch mapped_merge = merge_of(together);
    const depthxr::FrameGenDispatch plain_merge = merge_of(unmapped);
    Expect(mapped_merge.pass == FrameGenPass::kEngineMerge && mapped_merge.inputs[5].kind == FrameGenTextureKind::kDepth &&
               (mapped_merge.constants.options[0] & depthxr::kFrameGenEngineMergeDepths) != 0 &&
               plain_merge.pass == FrameGenPass::kEngineMerge && plain_merge.inputs[5].kind == FrameGenTextureKind::kNone &&
               (plain_merge.constants.options[0] & depthxr::kFrameGenEngineMergeDepths) == 0,
           "Engine map plan: the merge keeps the labels' surface depths for the map, none without it");
    request.world_motion = false;
    tuning.analyze_only = true;
    tuning.generated = 1;

    // Read again only near the time it was built for, both within one
    // interval: past it the map's flow stopped at one interval while its
    // models went on, which no one scale keeps.
    using depthxr::FrameGenEngineMapReusable;
    Expect(FrameGenEngineMapReusable(0.5f, 0.25f) && FrameGenEngineMapReusable(0.75f, 1.0f) &&
               !FrameGenEngineMapReusable(0.75f, 0.25f) && !FrameGenEngineMapReusable(1.5f, 1.25f) &&
               !FrameGenEngineMapReusable(1.2f, 0.9f) && !FrameGenEngineMapReusable(0.5f, 0.0f),
           "Engine map plan: a map read again within 0.35 of its time, both within one interval");

    // The fraction up to the reach, extrapolating only.
    using depthxr::FrameGenFraction;
    using depthxr::FrameGenSynthesisMode;
    Expect(FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 100, 250) == 1.0f &&
               FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 100, 250, 1.5f) == 1.5f &&
               FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 100, 220, 1.5f) == 1.2f &&
               FrameGenFraction(FrameGenSynthesisMode::kExtrapolate, 0, 100, 400, 3.0f) == 2.0f &&
               FrameGenFraction(FrameGenSynthesisMode::kInterpolate, 0, 100, 250, 1.5f) == 1.0f,
           "Engine map plan: FrameGenFraction past one interval up to the reach (2 at most), extrapolating only");
}

// Research switches: the environment first, then framegen-research.txt for
// the engine motion research's switches only.
void TestResearchSwitches() {
#if defined(_WIN32)
    namespace fs = std::filesystem;
    const wchar_t* saved_local = _wgetenv(L"LOCALAPPDATA");
    const std::wstring local_before = saved_local ? saved_local : L"";
    const fs::path root =
        fs::temp_directory_path() /
        ("vectorxr-research-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "VectorXR", ignored);
    _wputenv_s(L"LOCALAPPDATA", root.wstring().c_str());
    for (const char* name : {"VECTORXR_FRAMEGEN_ENGINE_CUTS", "VECTORXR_FRAMEGEN_ENGINE_MV",
                             "VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK", "VECTORXR_FRAMEGEN_JIT",
                             "VECTORXR_FRAMEGEN_ENGINE_MV_STATE", "VECTORXR_FRAMEGEN_DEPTH_SPLAT",
                             "VECTORXR_FRAMEGEN_WORLD", "VECTORXR_FRAMEGEN_SYNTH_FLOW_VIEW"}) {
        _putenv_s(name, "");
    }
    Expect(!depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_CUTS"), "Research switches: none without a file");
    {
        // Notepad's byte order mark, spaces, a comment, a switch outside the
        // research's and one named twice.
        std::ofstream file(root / "VectorXR" / "framegen-research.txt", std::ios::binary);
        file << "\xEF\xBB\xBFVECTORXR_FRAMEGEN_ENGINE_MV=census\r\n"
             << "# VECTORXR_FRAMEGEN_ENGINE_MV_STATE=common\r\n"
             << "  VECTORXR_FRAMEGEN_ENGINE_CUTS = 0 \r\n"
             << "VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK=1\r\n"
             << "VECTORXR_FRAMEGEN_JIT=0\r\n"
             << "VECTORXR_FRAMEGEN_ENGINE_CUTS=1\r\n"
             << "VECTORXR_FRAMEGEN_DEPTH_SPLAT=1\r\n"
             << "VECTORXR_FRAMEGEN_WORLD=0\r\n"
             << "VECTORXR_FRAMEGEN_SYNTH_FLOW_VIEW=fine\r\n";
    }
    depthxr::TakeResearchFileSwitches(); // what earlier reads in the process left
    const std::uint64_t reads = depthxr::ResearchFileReads();
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV") == "census",
           "Research switches: the first line, after a byte order mark");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_CUTS") == "1",
           "Research switches: the last line naming a switch holds, trimmed");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK") == "1",
           "Research switches: the depth encoding check is one of them");
    Expect(!depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_STATE"), "Research switches: a commented line is not read");
    Expect(!depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_JIT"),
           "Research switches: switches outside the engine motion and depth research come from the environment only");
    Expect(depthxr::ResearchFileReads() == reads + 3, "Research switches: three reads answered by the file");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_DEPTH_SPLAT") == "1" &&
               depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_WORLD") == "0" && depthxr::ResearchFileReads() == reads + 5,
           "Research switches: the depth research's switches and world motion are among them");
    // The environment wins.
    _putenv_s("VECTORXR_FRAMEGEN_ENGINE_CUTS", "0");
    _putenv_s("VECTORXR_FRAMEGEN_JIT", "1");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_CUTS") == "0" &&
               depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_JIT") == "1" && depthxr::ResearchFileReads() == reads + 5,
           "Research switches: the environment wins");
    // Each switch taken from the file is told once.
    const std::vector<std::string> taken = depthxr::TakeResearchFileSwitches();
    Expect(taken == std::vector<std::string>{"VECTORXR_FRAMEGEN_ENGINE_MV=census", "VECTORXR_FRAMEGEN_ENGINE_CUTS=1",
                                             "VECTORXR_FRAMEGEN_DEPTH_ENCODING_CHECK=1",
                                             "VECTORXR_FRAMEGEN_DEPTH_SPLAT=1", "VECTORXR_FRAMEGEN_WORLD=0"},
           "Research switches: the switches taken from the file, in order");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV") == "census" &&
               depthxr::TakeResearchFileSwitches().empty(),
           "Research switches: a switch taken again is not told again");
    // A session's switches are told anew, an earlier session's or not; what
    // was read before the session and not told is not its.
    depthxr::ResetResearchFileSwitches();
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_DEPTH_SPLAT") == "1", "Research switches: read again");
    depthxr::ResetResearchFileSwitches();
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV") == "census" &&
               depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV") == "census" &&
               depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_WORLD") == "0" &&
               depthxr::TakeResearchFileSwitches() ==
                   std::vector<std::string>{"VECTORXR_FRAMEGEN_ENGINE_MV=census", "VECTORXR_FRAMEGEN_WORLD=0"},
           "Research switches: each session's are told, once each");
    Expect(depthxr::ReadResearchSwitch("VECTORXR_FRAMEGEN_SYNTH_FLOW_VIEW") == "fine",
           "Research switches: the synthesis research's switches are among them");
    for (const char* name : {"VECTORXR_FRAMEGEN_ENGINE_CUTS", "VECTORXR_FRAMEGEN_JIT"}) {
        _putenv_s(name, "");
    }
    _wputenv_s(L"LOCALAPPDATA", local_before.c_str());
    fs::remove_all(root, ignored);
#endif
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
    Expect(FirstBinding(result.document.turbo.metrics_bindings).type == depthxr::InputBindingType::Keyboard,
           "Turbo metrics binding should parse as keyboard");

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "any.exe");
    Expect(resolved.turbo.metrics_mode == depthxr::TurboMetricsMode::kBinding,
           "Turbo metrics mode should resolve through");
    Expect(FirstBinding(resolved.turbo.metrics_bindings).chord.size() == 2,
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
    Expect(default_result.document.turbo.metrics_bindings.empty(),
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
    session.timing_configuration = "Experimental Async; cap=45; trace=on";
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
    Expect(sessions[0].timing_configuration == session.timing_configuration,
           "Metrics lost the session's experimental timing label");
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
      "diagnosticVisualizationBinding": { "type": "keyboard", "chord": ["Ctrl", "F8"] },
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
            "eyeTrackingCorrection": "flip-z",
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
    Expect(resolved.quadviews.eye_tracking_correction == "flip-z", "Quadviews correction profile override lost");
    Expect(depthxr::ResolveRuntimeConfig(result.document, "other.exe").quadviews.eye_tracking_correction == "default",
           "Legacy Quadviews settings must retain full-direction correction");
    std::string invalid_correction = json;
    invalid_correction.replace(invalid_correction.find("flip-z"), 6, "invalid");
    Expect(!depthxr::ParseConfig(invalid_correction).ok, "Invalid eye tracking correction accepted");
    Expect(resolved.quadviews.diagnostic_visualization_binding.type == depthxr::InputBindingType::Keyboard &&
               resolved.quadviews.diagnostic_visualization_binding.chord.size() == 2,
           "Quadviews diagnostic visualization binding mismatch");
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
    Expect(!resolved.pivotxr.profiles[0].always_active && resolved.pivotxr.profiles[0].activation_bindings[0].behavior == depthxr::PivotActivationBehavior::Hold,
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
    Expect(result.document.depthxr.bindings.toggle_enabled_bindings.empty(),
           "DepthXR toggle binding should be None");
    Expect(result.warnings.empty(), "A none binding with only its type must not warn");

    // A none binding has only its type: anything beside it is ignored with a
    // warning, as on the other bindings.
    const depthxr::ParseResult extra = depthxr::ParseConfig(R"json(
{
  "version": 3,
  "core": { "enabled": true },
  "applications": [],
  "modules": {
    "depthxr": { "enabled": true, "bindings": { "toggleEnabled": { "type": "none", "chord": ["F8"] } } },
    "pivotxr": { "enabled": false },
    "turbo": { "enabled": true, "toggleBindings": [{ "type": "none", "sound": { "enabled": true } }] }
  }
}
)json");
    std::string reported;
    for (const std::string& warning : extra.warnings) reported += "\n  " + warning;
    Expect(extra.ok && extra.document.depthxr.bindings.toggle_enabled_bindings.empty() &&
               extra.document.turbo.toggle_bindings.empty(),
           "None bindings with extra keys must still parse as None: " + extra.error);
    Expect(extra.warnings == std::vector<std::string>{"Unknown field ignored: modules.depthxr.bindings.toggleEnabled.chord",
                                                      "Unknown field ignored: modules.turbo.toggleBindings[0].sound"},
           "Keys beside a none binding's type were not reported by their path:" + reported);
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
    Expect(result.document.depthxr.bindings.toggle_enabled_bindings.size() == 1, "Device binding was dropped");
    const depthxr::InputBinding& binding = result.document.depthxr.bindings.toggle_enabled_bindings.front();
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
    const depthxr::SoundFeedback& sound = FirstBinding(result.document.depthxr.bindings.toggle_enabled_bindings).sound;
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
    Expect(!FirstBinding(result.document.depthxr.bindings.toggle_enabled_bindings).sound.enabled,
           "Binding without sound block should default to disabled feedback");
}

// A toggle may have several bindings, one per device (a HOTAS's and a
// HOSAS's): every toggle takes a list under its plural key, and still reads
// its single legacy binding.
void TestToggleBindingLists() {
    const auto config = [](const std::string& osd, const std::string& depth_bindings, const std::string& turbo) {
        return depthxr::ParseConfig(
            R"({"version":3,"core":{"enabled":true,"logLevel":"info","osd":)" + osd +
            R"(},"applications":[],"modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},"bindings":)" +
            depth_bindings + R"(,"profiles":[]},"pivotxr":{"enabled":false,"defaults":{},"profiles":[]},"turbo":)" + turbo + "}}");
    };
    const std::string hotas = R"({"type":"device","deviceGuid":"{11111111-0000-0000-0000-000000000001}","inputPath":"button-23"})";
    const std::string hosas = R"({"type":"device","deviceGuid":"{22222222-0000-0000-0000-000000000002}","inputPath":"button-5"})";
    const std::string pair = "[" + hotas + "," + hosas + "]";
    const auto lists = config(R"({"toggleBindings":)" + pair + R"(,"cycleBindings":)" + pair + "}",
                              R"({"toggleEnabledBindings":)" + pair + R"(,"toggleAnchorBindings":)" + pair + "}",
                              R"({"enabled":false,"toggleBindings":)" + pair + R"(,"metricsBindings":)" + pair + "}");
    Expect(lists.ok, "Toggle binding lists were rejected: " + lists.error);
    const auto& document = lists.document;
    for (const auto* bindings : {&document.core.osd.toggle_bindings, &document.core.osd.cycle_bindings,
                                 &document.depthxr.bindings.toggle_enabled_bindings,
                                 &document.depthxr.bindings.toggle_anchor_bindings, &document.turbo.toggle_bindings,
                                 &document.turbo.metrics_bindings}) {
        Expect(bindings->size() == 2 && (*bindings)[0].input_path == "button-23" && (*bindings)[1].input_path == "button-5",
               "A toggle's binding list was not kept in order");
    }
    const auto resolved = depthxr::ResolveRuntimeConfig(document, "any.exe");
    Expect(resolved.turbo.toggle_bindings.size() == 2 && resolved.turbo.metrics_bindings.size() == 2,
           "Toggle binding lists did not resolve through");

    // Configs written before lists, and by the app for a single binding.
    const auto legacy = config(R"({"toggleBinding":)" + hotas + R"(,"cycleBinding":{"type":"none"}})",
                               R"({"toggleEnabled":)" + hotas + R"(,"toggleAnchor":{"type":"none"}})",
                               R"({"enabled":false,"toggleBinding":)" + hotas + "}");
    Expect(legacy.ok, "Single legacy bindings were rejected: " + legacy.error);
    Expect(legacy.document.core.osd.toggle_bindings.size() == 1 && legacy.document.core.osd.cycle_bindings.empty() &&
               legacy.document.depthxr.bindings.toggle_enabled_bindings.size() == 1 &&
               legacy.document.depthxr.bindings.toggle_anchor_bindings.empty() &&
               legacy.document.turbo.toggle_bindings.size() == 1 && legacy.document.turbo.metrics_bindings.empty(),
           "Single legacy bindings did not become one-binding lists");

    // An OSD without either key keeps its default keyboard bindings.
    const auto defaults = config("{}", R"({"toggleEnabled":{"type":"none"}})", R"({"enabled":false})");
    Expect(defaults.ok && FirstChordKey(defaults.document.core.osd.toggle_bindings, "Ctrl") &&
               FirstChordKey(defaults.document.core.osd.cycle_bindings, "Ctrl"),
           "The OSD's default bindings were lost: " + defaults.error);

    // A list and a legacy binding for the same toggle are ambiguous.
    const auto both = config(R"({"toggleBinding":)" + hotas + R"(,"toggleBindings":)" + pair + "}",
                             R"({"toggleEnabled":{"type":"none"}})", R"({"enabled":false})");
    Expect(!both.ok, "A toggle with both a list and a legacy binding was accepted");
    Expect(!config(R"({"customPresets":[{"name":"Bad","settings":{"toggleBindings":[]}}]})",
                   R"({"toggleEnabled":{"type":"none"}})", R"({"enabled":false})").ok,
           "An OSD preset with bindings was accepted");
    // Depth still requires one of its toggle's keys.
    Expect(!config("{}", R"({"toggleAnchorBindings":[]})", R"({"enabled":false})").ok,
           "Depth bindings without the toggle were accepted");
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
    first.test_started_at = 1000;
    first.runtime_name = "Oculus";
    first.runtime_version = "1.205.0";
    first.system_name = "Quest 3";
    first.vendor_id = 1;
    first.graphics_api = "D3D11";
    first.mode = depthxr::TurboPacingMode::kSequenced;
    first.source = "discovered";
    first.layer_version = "0.13.0";
    first.last_used_unix_seconds = 100;
    first.probe_timeouts = 3;
    first.stable_seconds = 60;
    bool recorded = depthxr::RecordRuntimePacingObservation(path, first, &record_error);
    Expect(recorded, "Failed to record first runtime pacing observation: " + record_error);

    depthxr::RuntimePacingObservation second;
    second.test_started_at = 1000;
    second.runtime_name = "SteamVR/OpenXR";
    second.mode = depthxr::TurboPacingMode::kAsync;
    second.source = "preset";
    second.last_used_unix_seconds = 200;
    recorded = depthxr::RecordRuntimePacingObservation(path, second, &record_error);
    Expect(recorded, "Failed to record second runtime pacing observation: " + record_error);

    depthxr::RuntimePacingObservation second_oculus_system = first;
    second_oculus_system.system_name = "Crystal";
    second_oculus_system.vendor_id = 2;
    second_oculus_system.mode = depthxr::TurboPacingMode::kAsync;
    second_oculus_system.last_used_unix_seconds = 250;
    recorded = depthxr::RecordRuntimePacingObservation(path, second_oculus_system, &record_error);
    Expect(recorded, "Failed to record second headset fingerprint for one runtime: " + record_error);

    // Upsert: a later verdict for the same runtime replaces the mode but
    // preserves first-used.
    first.mode = depthxr::TurboPacingMode::kAsync;
    first.last_used_unix_seconds = 300;
    recorded = depthxr::RecordRuntimePacingObservation(path, first, &record_error);
    Expect(recorded, "Failed to upsert runtime pacing observation: " + record_error);

    const auto observations = depthxr::ReadRuntimePacingObservations(path);
    Expect(observations.size() == 3, "Runtime pacing observation fingerprint count mismatch");

    const auto oculus = depthxr::FindRuntimePacingObservation(path, "Oculus");
    Expect(oculus.has_value(), "Oculus runtime pacing observation missing");
    Expect(oculus->mode == depthxr::TurboPacingMode::kAsync, "Upserted mode was not applied");
    Expect(oculus->first_used_unix_seconds == 100, "First-used timestamp was not preserved on upsert");
    Expect(oculus->last_used_unix_seconds == 300, "Last-used timestamp was not updated on upsert");
    Expect(oculus->runtime_version == "1.205.0", "Runtime version mismatch");
    Expect(oculus->source == "discovered", "Source mismatch");

    const auto quest = depthxr::FindRuntimePacingObservation(path, "Oculus", "Quest 3", 1, "D3D11");
    Expect(quest.has_value(), "Exact runtime/headset/graphics fingerprint was not found");
    Expect(quest->mode == depthxr::TurboPacingMode::kAsync, "Exact fingerprint upsert mode mismatch");
    const auto crystal = depthxr::FindRuntimePacingObservation(path, "Oculus", "Crystal", 2, "D3D11");
    Expect(crystal.has_value(), "Second headset fingerprint was collapsed into the first");

    const auto steam = depthxr::FindRuntimePacingObservation(path, "SteamVR/OpenXR");
    Expect(!steam.has_value(), "Legacy presets must not influence Auto");
    const auto reset_dir = test_directory / "runtime-pacing-resets";
    std::filesystem::create_directories(reset_dir);
    std::ofstream(reset_dir / "4f63756c7573.txt") << "2000";
    Expect(!depthxr::FindRuntimePacingObservation(path, "Oculus"), "A cleared decision must not be reused");
    first.last_used_unix_seconds = 400;
    Expect(depthxr::RecordRuntimePacingObservation(path, first, &record_error), "Stale writes should be ignored cleanly");
    Expect(!depthxr::FindRuntimePacingObservation(path, "Oculus"), "A running session must not undo a reset");
    first.test_started_at = 3000;
    Expect(depthxr::RecordRuntimePacingObservation(path, first, &record_error), "Fresh retest should persist");
    Expect(depthxr::FindRuntimePacingObservation(path, "Oculus").has_value(), "A fresh successful retest should be reusable");

    Expect(!depthxr::FindRuntimePacingObservation(path, "Varjo").has_value(),
           "Unknown runtime should have no observation");

    std::filesystem::remove_all(test_directory, error);
}

void TestRuntimeRelayControlAndStatusRoundTrip() {
    const std::filesystem::path test_directory =
        std::filesystem::current_path() / "build" / "vectorxr-test-runtime-relay";
    std::error_code filesystem_error;
    std::filesystem::remove_all(test_directory, filesystem_error);
    std::filesystem::create_directories(test_directory / "control", filesystem_error);
    Expect(!filesystem_error, "Failed to create runtime-relay test directory");

    const std::string session_id = "4242-1000-1";
    const std::filesystem::path control_path = depthxr::RuntimeControlPath(test_directory, session_id);
    {
        std::ofstream stream(control_path, std::ios::trunc);
        stream << "{\n"
               << "  \"protocolVersion\": 1,\n"
               << "  \"targetSessionId\": \"" << session_id << "\",\n"
               << "  \"revision\": 17,\n"
               << "  \"expiresAtUnixMilliseconds\": 9999999999999,\n"
               << "  \"desired\": { \"quadviewsDiagnosticVisualization\": true }\n"
               << "}\n";
    }

    depthxr::RuntimeControlDocument control;
    std::string relay_error;
    Expect(depthxr::ReadRuntimeControl(control_path, &control, &relay_error),
           "Failed to parse runtime control: " + relay_error);
    Expect(control.target_session_id == session_id, "Runtime control session target mismatch");
    Expect(control.revision == 17, "Runtime control revision mismatch");
    Expect(control.quadviews_diagnostic_visualization.value_or(false),
           "Runtime control desired state mismatch");

    depthxr::RuntimeStatusDocument status;
    status.session_id = session_id;
    status.process_id = 4242;
    status.application = "DCS.exe";
    status.updated_at_unix_milliseconds = 1000;
    status.acknowledged_revision = 17;
    status.quadviews_diagnostic_visualization_available = true;
    status.quadviews_diagnostic_visualization_enabled = true;
    const std::filesystem::path status_path = depthxr::RuntimeStatusPath(test_directory, session_id);
    Expect(depthxr::WriteRuntimeStatus(status_path, status, &relay_error),
           "Failed to write runtime status: " + relay_error);
    std::ifstream status_stream(status_path);
    std::ostringstream status_content;
    status_content << status_stream.rdbuf();
    Expect(status_content.str().find("\"acknowledgedRevision\": 17") != std::string::npos,
           "Runtime status acknowledgement was not serialized");
    Expect(status_content.str().find("\"framegen\":") == std::string::npos,
           "Runtime status wrote Frame Generation rates for a session without it");
    status_stream.close(); // an open reader keeps the atomic replace from taking the file

    // Frame Generation's rates as numbers: a game slower than a fixed 2x hold.
    depthxr::RuntimeFrameGenRates rates;
    rates.mode = "extrapolate";
    rates.frame_rate = "2x";
    rates.generating = true;
    rates.multiplier = 2;
    rates.refresh_hz = 80.0;
    rates.hold_fps = 40.0;
    rates.app_fps = 34.14;
    rates.output_fps = 80.0;
    rates.distinct_fps = 79.56;
    status.framegen_rates = rates;
    Expect(depthxr::WriteRuntimeStatus(status_path, status, &relay_error),
           "Failed to write runtime status with Frame Generation rates: " + relay_error);
    std::ifstream rates_stream(status_path);
    std::ostringstream rates_content;
    rates_content << rates_stream.rdbuf();
    Expect(rates_content.str().find("\"framegen\": { \"mode\": \"extrapolate\", \"frameRate\": \"2x\", "
                                    "\"generating\": true, \"multiplier\": 2, \"refreshHz\": 80.0, \"holdFps\": 40.0, "
                                    "\"appFps\": 34.1, \"outputFps\": 80.0, \"distinctFps\": 79.6 },") != std::string::npos,
           "Runtime status did not serialize Frame Generation's rates");
    Expect(rates_content.str().find("\"quadviewsDimensionsAt\": 0,") != std::string::npos,
           "Runtime status lost the fields after Frame Generation's rates");
    std::filesystem::remove_all(test_directory, filesystem_error);
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

void TestDepthSubmissionRestorePreservesApplicationDeltas() {
    const depthxr::ViewAdjustmentData native_views[2] = {
        {{-0.032, 0.0, 0.0}, {-1.0, 0.8, 0.9, -0.9}},
        {{0.032, 0.0, 0.0}, {-0.8, 1.0, 0.9, -0.9}},
    };
    depthxr::ViewAdjustmentData render_views[2] = {native_views[0], native_views[1]};
    depthxr::ApplyStereoBoost(render_views, 1.25, depthxr::ViewLayout::kStereo);
    depthxr::ApplyConvergence(render_views, 0.03, depthxr::ViewLayout::kStereo);

    depthxr::ViewAdjustmentData submitted = render_views[0];
    constexpr double kApplicationPositionDelta = 0.004;
    constexpr double kApplicationProjectionDelta = 0.012;
    submitted.position.x += kApplicationPositionDelta;
    submitted.fov.angle_left =
        std::atan(std::tan(submitted.fov.angle_left) + kApplicationProjectionDelta);
    submitted.fov.angle_right =
        std::atan(std::tan(submitted.fov.angle_right) + kApplicationProjectionDelta);

    depthxr::RestoreDepthSubmissionView(submitted, native_views[0], render_views[0]);

    Expect(std::abs(submitted.position.x -
                    (native_views[0].position.x + kApplicationPositionDelta)) < 0.000001,
           "Depth submission restore did not recover native eye position");
    Expect(std::abs((std::tan(submitted.fov.angle_left) -
                     std::tan(native_views[0].fov.angle_left)) -
                    kApplicationProjectionDelta) < 0.000001,
           "Depth submission restore lost the app's left projection delta");
    Expect(std::abs((std::tan(submitted.fov.angle_right) -
                     std::tan(native_views[0].fov.angle_right)) -
                    kApplicationProjectionDelta) < 0.000001,
           "Depth submission restore lost the app's right projection delta");
    Expect(std::abs(submitted.fov.angle_up - native_views[0].fov.angle_up) < 0.000001,
           "Depth submission restore changed an untouched vertical FOV boundary");

    depthxr::ViewAdjustmentData already_native = native_views[0];
    depthxr::RestoreDepthSubmissionView(already_native, native_views[0], render_views[0]);
    Expect(std::abs(already_native.position.x - native_views[0].position.x) < 0.000001,
           "Depth submission restore double-corrected an already-native eye position");
    Expect(std::abs(already_native.fov.angle_left - native_views[0].fov.angle_left) < 0.000001,
           "Depth submission restore double-corrected an already-native FOV");
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

void TestTurboTraceContinuousAndDrained() {
    const auto directory = std::filesystem::current_path() / "build" / "vectorxr-test-turbo-trace";
    std::filesystem::create_directories(directory);
    std::filesystem::path log_path;
    {
        depthxr::Logger logger;
        logger.Initialize(directory / "trace.log");
        logger.SetLevel(depthxr::LogLevel::Debug);
        log_path = logger.ActiveLogPath();
        depthxr::TurboTimingTrace trace;
        trace.Record("disabled");
        Expect(!trace.Enabled() && trace.NextId() == 0, "Disabled trace must be inert");
        trace.Start(logger);
        trace.Record("test.first", trace.NextId(), 11, 22, 33);
        for (int i = 0; i < 40000; ++i) {
            if (i % 1000 == 0) trace.Burst();
            if (i % 128 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            trace.Record("test.event", trace.NextId(), i);
        }
        trace.Stop();
        Expect(!trace.Enabled(), "Stopping trace left it active");
        trace.Record("after.stop");
        trace.SetDebugEnabled(logger, true);
        Expect(!trace.Enabled(), "Log-level update restarted an ended session");
    }
    std::ifstream input(log_path);
    std::string line;
    std::size_t events = 0;
    bool first = false;
    while (std::getline(input, line)) {
        if (line.find("Turbo-trace ns=") != std::string::npos) ++events;
        first = first || line.find("event=test.first a=11 b=22 c=33") != std::string::npos;
        Expect(line.find("event=disabled") == std::string::npos && line.find("event=after.stop") == std::string::npos,
               "Disabled tracing wrote events");
    }
    Expect(first && events > 30000, "Trace stopped at the old lifetime budget or failed to drain");
    input.close();
    std::filesystem::remove(log_path);
    for (unsigned part = 0; part < 2; ++part)
        std::filesystem::remove(depthxr::TurboTimingTrace::RecorderPath(log_path, part));
}

void TestTurboTraceFollowsDebugLevel() {
    const auto directory = std::filesystem::current_path() / "build" / "vectorxr-test-turbo-trace";
    std::filesystem::create_directories(directory);
    std::filesystem::path log_path;
    {
        depthxr::Logger logger;
        logger.Initialize(directory / "level.log");
        log_path = logger.ActiveLogPath();
        depthxr::TurboTimingTrace trace;
        trace.Start(logger);
        Expect(!trace.Enabled(), "Info logging unexpectedly enabled detailed timing");
        trace.Record("info.only");
        logger.SetLevel(depthxr::LogLevel::Debug);
        trace.SetDebugEnabled(logger, true);
        Expect(trace.Enabled(), "Debug did not enable detailed timing in the live session");
        const auto first_id = trace.NextId();
        logger.SetLevel(depthxr::LogLevel::Info);
        trace.SetDebugEnabled(logger, false);
        Expect(!trace.Enabled() && !trace.Capturing(), "Info did not pause detailed timing");
        trace.Record("info.paused");
        logger.SetLevel(depthxr::LogLevel::Debug);
        trace.SetDebugEnabled(logger, true);
        Expect(trace.NextId() > first_id, "Resuming Debug reset the session correlation IDs");
        trace.Record("debug.resumed");
        trace.Stop();
    }
    std::ifstream input(log_path);
    std::string line;
    bool resumed = false;
    while (std::getline(input, line)) {
        resumed = resumed || line.find("event=debug.resumed") != std::string::npos;
        Expect(line.find("event=info.") == std::string::npos, "Info interval leaked detailed timing");
        if (line.find("Turbo-trace ns=") != std::string::npos) {
            Expect(line.find("[debug]") != std::string::npos, "Timing trace was not a Debug-level record");
        }
    }
    Expect(resumed, "Resumed Debug trace did not drain");
    input.close();
    std::filesystem::remove(log_path);
    for (unsigned part = 0; part < 2; ++part)
        std::filesystem::remove(depthxr::TurboTimingTrace::RecorderPath(log_path, part));
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

#ifdef _WIN32
std::string ExtractRawShaderSource(const std::string& source_file, const std::string& function_marker) {
    const size_t function_position = source_file.find(function_marker);
    Expect(function_position != std::string::npos, "Shader function marker is missing: " + function_marker);
    const std::string raw_start_marker = std::string("return R") + static_cast<char>(34) + "(";
    const size_t raw_start = source_file.find(raw_start_marker, function_position);
    Expect(raw_start != std::string::npos, "Shader raw string start is missing: " + function_marker);
    const size_t source_start = raw_start + raw_start_marker.size();
    const std::string raw_end_marker = std::string(")") + static_cast<char>(34) + ";";
    const size_t source_end = source_file.find(raw_end_marker, source_start);
    Expect(source_end != std::string::npos, "Shader raw string end is missing: " + function_marker);
    return source_file.substr(source_start, source_end - source_start);
}

void ExpectShaderCompiles(const std::string& source,
                          const char* entry_point,
                          const char* target,
                          const std::string& label) {
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(source.data(),
                                     source.size(),
                                     label.c_str(),
                                     nullptr,
                                     nullptr,
                                     entry_point,
                                     target,
                                     0,
                                     0,
                                     &bytecode,
                                     &errors);
    std::string diagnostic;
    if (errors) {
        diagnostic.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (bytecode) {
        bytecode->Release();
    }
    Expect(SUCCEEDED(result), label + " failed to compile: " + diagnostic);
}

void TestD3D11SharpenShaderRegression() {
    const std::filesystem::path source_path =
        std::filesystem::path(VECTORXR_SOURCE_DIR) / "layer" / "src" / "openxr_layer.cpp";
    std::ifstream stream(source_path, std::ios::binary);
    Expect(stream.good(), "Failed to open the D3D11 shader source for regression testing");
    const std::string source_file((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    const std::string compositor = depthxr::QuadViewsD3D11ShaderSource();
    const std::array<std::pair<std::string, std::string>, 2> shaders{{
        {"QuadViewsD3D11ShaderSource()", compositor},
        {"D3D11FocusSharpenShaderSource()",
         ExtractRawShaderSource(source_file, "const char* D3D11FocusSharpenShaderSource()")},
    }};
    for (const auto& [label, shader] : shaders) {
        Expect(shader.find("focus.rgb = saturate(focus.rgb + detail);") != std::string::npos,
               label + " lost the extrema-preserving sharpen output");
        Expect(shader.find("focus.rgb = clamp(sharpened, lo, hi);") == std::string::npos,
               label + " regressed to the clamp that cancelled sharpening at extrema");
        ExpectShaderCompiles(shader, "VSMain", "vs_5_0", label + " vertex shader");
        ExpectShaderCompiles(shader, "PSMain", "ps_5_0", label + " pixel shader");
    }

    Expect(compositor.find("saturate(blendParams.z) * edgeAlpha") != std::string::npos,
           "Quadviews compositor sharpening no longer ramps with the focus feather");
    Expect(compositor.find("diagnosticParams.x < 0.5") != std::string::npos &&
               compositor.find("rawGazeMarker") != std::string::npos,
           "Quadviews compositor diagnostic visualization is missing");
}
#endif

void TestEyeGazeExtensionCompatibilityPolicy() {
    using depthxr::EyeGazeProbeState;
    using depthxr::EyeGazeRequestPolicyInput;
    using depthxr::EyeGazeRequestReason;

    Expect(depthxr::IsEyeGazeExtensionProbeKnownUnreliable(
               R"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json)"),
           "SteamVR manifests should retain the false-negative eye-gaze probe workaround");
    Expect(depthxr::IsEyeGazeExtensionProbeKnownUnreliable(
               R"(C:\Program Files\PimaxXR\pimax-openxr.json)"),
           "PimaxXR manifests should use the false-negative eye-gaze probe workaround");
    Expect(!depthxr::IsEyeGazeExtensionProbeKnownUnreliable(
               R"(C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json)"),
           "VDXR should use the generic indeterminate-probe policy instead of a manifest exception");

    const std::string_view d3d11_only[] = {"XR_KHR_D3D11_enable"};
    const std::string_view ordinary_forwarded[] = {
        "XR_KHR_D3D11_enable",
        "XR_KHR_visibility_mask",
        "XR_EXT_hand_tracking",
    };
    const std::string_view complete_advertisement[] = {
        "XR_KHR_D3D11_enable",
        "XR_KHR_visibility_mask",
        "XR_EXT_hand_tracking",
        "XR_EXT_debug_utils",
    };
    Expect(depthxr::IsRuntimeExtensionProbeStructurallyUnreliable(
               true, std::span<const std::string_view>{}, ordinary_forwarded),
           "A completed empty Pimax-style extension scan must be treated as structurally unreliable");
    Expect(depthxr::IsRuntimeExtensionProbeStructurallyUnreliable(
               true, d3d11_only, ordinary_forwarded),
           "A partial scan missing extensions already being forwarded must be treated as unreliable");
    Expect(!depthxr::IsRuntimeExtensionProbeStructurallyUnreliable(
               true, complete_advertisement, ordinary_forwarded),
           "A completed scan containing every forwarded extension should remain credible");
    Expect(!depthxr::IsRuntimeExtensionProbeStructurallyUnreliable(
               false, std::span<const std::string_view>{}, ordinary_forwarded),
           "An incomplete scan is indeterminate and should not also be labeled structurally unreliable");

    Expect(depthxr::ClassifyEyeGazeProbe(false, false) == EyeGazeProbeState::kIndeterminate,
           "An extension scan that could not complete must remain indeterminate");
    Expect(depthxr::ClassifyEyeGazeProbe(false, true) == EyeGazeProbeState::kIndeterminate,
           "An incomplete scan must not expose partial extension evidence as authoritative");
    Expect(depthxr::ClassifyEyeGazeProbe(true, true) == EyeGazeProbeState::kPresent,
           "A completed scan that advertises gaze must classify it as present");
    Expect(depthxr::ClassifyEyeGazeProbe(true, false) == EyeGazeProbeState::kAbsent,
           "Only a completed scan may classify gaze as absent");

    auto expect_decision = [](const EyeGazeRequestPolicyInput& input,
                              bool expected_injection,
                              EyeGazeRequestReason expected_reason,
                              const std::string& message) {
        const depthxr::EyeGazeRequestDecision decision = depthxr::DecideEyeGazeExtensionRequest(input);
        Expect(decision.inject_extension == expected_injection, message + " (injection)");
        Expect(decision.reason == expected_reason, message + " (reason)");
    };

    EyeGazeRequestPolicyInput input;
    input.app_requested_layer_owned_quadviews = true;

    input.pre_instance_probe = EyeGazeProbeState::kPresent;
    expect_decision(input, true, EyeGazeRequestReason::kRuntimeAdvertised,
                    "An advertised gaze extension should be injected for synthesized quadviews");

    input.pre_instance_probe = EyeGazeProbeState::kIndeterminate;
    expect_decision(input, true, EyeGazeRequestReason::kProbeIndeterminate,
                    "An unavailable VDXR-style probe should optimistically request gaze");

    input.pre_instance_probe = EyeGazeProbeState::kAbsent;
    expect_decision(input, false, EyeGazeRequestReason::kReliableNegative,
                    "A completed negative probe on an ordinary runtime should be trusted");

    input.pre_instance_probe_structurally_unreliable = true;
    expect_decision(input, true, EyeGazeRequestReason::kProbeInconsistent,
                    "A structurally inconsistent negative should optimistically request gaze");

    input.pre_instance_probe_structurally_unreliable = false;
    input.pre_instance_probe_known_unreliable = true;
    expect_decision(input, true, EyeGazeRequestReason::kRuntimeWorkaround,
                    "A completed false-negative on a known runtime should retain the workaround");

    input.pre_instance_probe_known_unreliable = false;
    input.forwarding_native_quadviews = true;
    input.pre_instance_probe = EyeGazeProbeState::kIndeterminate;
    expect_decision(input, false, EyeGazeRequestReason::kNativeQuadviews,
                    "Native Varjo quadviews should retain its runtime-owned gaze contract");

    input.forwarding_native_quadviews = false;
    input.downstream_already_has_eye_gaze = true;
    expect_decision(input, false, EyeGazeRequestReason::kApplicationRequested,
                    "The layer must not duplicate an application-requested gaze extension");

    input.downstream_already_has_eye_gaze = false;
    input.app_requested_layer_owned_quadviews = false;
    expect_decision(input, false, EyeGazeRequestReason::kQuadviewsNotRequested,
                    "Applications without layer-owned quadviews should not receive an injected extension");

    Expect(depthxr::EyeGazeProbeStateName(EyeGazeProbeState::kPresent) == "present" &&
               depthxr::EyeGazeProbeStateName(EyeGazeProbeState::kAbsent) == "absent" &&
               depthxr::EyeGazeProbeStateName(EyeGazeProbeState::kIndeterminate) == "indeterminate",
           "Probe state names must remain stable for startup diagnostics");
    Expect(depthxr::EyeGazeRequestReasonName(EyeGazeRequestReason::kProbeIndeterminate) ==
               "probe-indeterminate" &&
               depthxr::EyeGazeRequestReasonName(EyeGazeRequestReason::kProbeInconsistent) ==
                   "probe-inconsistent" &&
               depthxr::EyeGazeRequestReasonName(EyeGazeRequestReason::kRuntimeWorkaround) ==
                   "runtime-workaround" &&
               depthxr::EyeGazeRequestReasonName(EyeGazeRequestReason::kReliableNegative) ==
                   "reliable-negative",
           "Request reason names must remain stable for startup diagnostics");

    Expect(depthxr::ShouldRetryWithoutInjectedEyeGaze(true, true),
           "An injected gaze extension rejected by a runtime should use the safe retry");
    Expect(!depthxr::ShouldRetryWithoutInjectedEyeGaze(true, false),
           "Unrelated instance failures must not be hidden by the gaze retry");
    Expect(!depthxr::ShouldRetryWithoutInjectedEyeGaze(false, true),
           "Application-owned extension failures must not be retried behind its back");
}


void TestQuadViewsSessionActivationPolicy() {
    Expect(depthxr::ResolveQuadViewsSessionActive(true, std::nullopt),
           "Quadviews should follow enabled config before a session is latched");
    Expect(!depthxr::ResolveQuadViewsSessionActive(false, std::nullopt),
           "Quadviews should follow disabled config before a session is latched");
    Expect(depthxr::ResolveQuadViewsSessionActive(false, true),
           "A live enabled Quadviews session must ignore a mid-session disable");
    Expect(!depthxr::ResolveQuadViewsSessionActive(true, false),
           "A live stereo session must ignore a mid-session Quadviews enable");
}

void TestQuadViewsRecoveryStabilizationPolicy() {
    Expect(depthxr::QuadViewsRecoveryStabilizationDelay("VirtualDesktopXR").count() == 250,
           "VDXR recovery should wait for focus and reference-space events to settle");
    Expect(depthxr::QuadViewsRecoveryStabilizationDelay("virtualdesktopxr").count() == 250,
           "VDXR runtime matching should be case-insensitive");
    Expect(depthxr::QuadViewsRecoveryStabilizationDelay("SteamVR/OpenXR").count() == 0,
           "Other runtimes must retain the immediate recovery path");
}

void TestQuadViewsRecoveryStabilizer() {
    using namespace std::chrono_literals;
    using Stabilizer = depthxr::QuadViewsRecoveryStabilizer;

    Stabilizer stabilizer;
    const Stabilizer::TimePoint start{};
    stabilizer.Schedule(start, 250ms);
    Expect(stabilizer.Pending(), "A recovered gaze sample should schedule compositor recovery");
    Expect(!stabilizer.Ready(start + 249ms), "Recovery must wait for the full stabilization window");

    stabilizer.NoteUnstable(start + 200ms);
    Expect(!stabilizer.Ready(start + 449ms),
           "A renewed tracking failure must restart the stabilization window");
    Expect(stabilizer.Ready(start + 450ms),
           "Recovery should become ready after 250 ms of continuous stability");

    stabilizer.Reset();
    Expect(!stabilizer.Pending(), "Reset must cancel a pending compositor recovery");
    stabilizer.Schedule(start, 0ms);
    Expect(stabilizer.Ready(start), "Other runtimes must retain immediate recovery");
}

void TestQuadViewsFrameCacheKeepsGazeWithLocatedFrame() {
    struct Frame {
        bool gaze_valid{false};
        double raw_yaw{0.0};
        double smoothed_yaw{0.0};
    };

    depthxr::QuadViewsFrameCache<Frame> cache;
    cache.Store(1'000, Frame{true, 0.10, 0.08}, 180);
    // A later locate must not replace the gaze diagnostics associated with an
    // older frame that the application submits afterward.
    cache.Store(2'000, Frame{false, 0.40, 0.30}, 180);

    Frame selected;
    std::int64_t matched_time = 0;
    Expect(cache.FindNearest(1'000, 100, &selected, &matched_time),
           "Exact Quadviews frame-cache lookup should succeed");
    Expect(matched_time == 1'000 && selected.gaze_valid &&
               std::abs(selected.raw_yaw - 0.10) < 0.0001 &&
               std::abs(selected.smoothed_yaw - 0.08) < 0.0001,
           "Quadviews frame cache must return the gaze snapshot stored with the submitted frame");

    Expect(cache.FindNearest(1'040, 50, &selected, &matched_time) && matched_time == 1'000 &&
               selected.gaze_valid,
           "Nearest Quadviews frame lookup should preserve the matched frame's gaze validity");
    Expect(!cache.FindNearest(1'500, 100, &selected, &matched_time) && matched_time == 1'000,
           "Quadviews frame lookup should reject a nearest frame outside the match window");

    cache.Store(3'000, Frame{true, 0.50, 0.45}, 2);
    Expect(cache.Size() == 2,
           "Quadviews frame cache should enforce its bounded retention count");
    Expect(!cache.FindNearest(1'000, 0, &selected, &matched_time),
           "Quadviews frame cache should discard the oldest frame when retention is exceeded");

    cache.PruneThrough(2'000);
    Expect(cache.Size() == 1 && cache.FindNearest(3'000, 0, &selected, &matched_time),
           "Quadviews frame cache should prune submitted frames while keeping future frames");
}

void TestPivotLocateViewsRouting() {
    Expect(depthxr::ShouldDrivePivotFromLocateViews(true, false),
           "A world-space xrLocateViews query must drive Pivot frame state");
    Expect(!depthxr::ShouldDrivePivotFromLocateViews(true, true),
           "A VIEW-relative xrLocateViews query must remain pass-through");
    Expect(!depthxr::ShouldDrivePivotFromLocateViews(false, false),
           "A missing locate request must not drive Pivot frame state");

    using Time = std::int64_t;
    std::map<Time, int> pose_deltas;
    auto record_locate_result = [&](bool base_is_view, Time time, int pose_delta) {
        if (depthxr::ShouldDrivePivotFromLocateViews(true, base_is_view)) {
            depthxr::CachePivotPoseDeltaValue(pose_deltas, time, pose_delta);
        }
    };
    auto expect_delta = [&](Time time, int expected, const std::string& message) {
        int pose_delta = 0;
        Time matched_time = 0;
        const bool found = depthxr::FindPivotPoseDeltaValue(
            pose_deltas, time, 0, &pose_delta, &matched_time);
        Expect(found && matched_time == time && pose_delta == expected, message);
    };

    record_locate_result(false, 100, 42);
    record_locate_result(false, 100, 99);
    record_locate_result(true, 100, 0);
    expect_delta(100, 42,
                 "Repeated same-time queries must reuse the first logical Pivot frame delta");

    pose_deltas.clear();
    record_locate_result(true, 200, 0);
    record_locate_result(false, 200, 84);
    expect_delta(200, 84,
                 "A world-space query must drive the frame after an earlier VIEW-relative query");

    depthxr::PrunePivotPoseDeltaValues(pose_deltas, static_cast<Time>(200));
    record_locate_result(true, 300, 0);
    int pose_delta = -1;
    Time matched_time = -1;
    Expect(!depthxr::FindPivotPoseDeltaValue(
               pose_deltas, static_cast<Time>(300), 0, &pose_delta, &matched_time) &&
               pose_delta == 0 && matched_time == 0,
           "A VIEW-only frame must not reuse a pruned pose delta");

    const Time frame = 1'000'000'000;
    pose_deltas.clear();
    depthxr::CachePivotPoseDeltaValue(pose_deltas, frame, 42);
    std::optional<int> held_pose_delta;
    std::size_t consecutive_misses = 0;
    Expect(depthxr::ResolvePivotPoseDeltaValue(
               pose_deltas, frame, 0, true, &held_pose_delta,
               &consecutive_misses, &pose_delta, &matched_time) ==
               depthxr::PivotPoseDeltaSelection::Matched &&
               pose_delta == 42 && held_pose_delta == 42 && consecutive_misses == 0,
           "A matched Pivot delta must arm one-frame continuity");

    const Time missed_frame = frame + depthxr::kPivotPoseDeltaMatchWindow + 1;
    Expect(depthxr::ResolvePivotPoseDeltaValue(
               pose_deltas, missed_frame, 0, true, &held_pose_delta,
               &consecutive_misses, &pose_delta, &matched_time) ==
               depthxr::PivotPoseDeltaSelection::HeldPrevious &&
               pose_delta == 42 && consecutive_misses == 1,
           "The first cache miss must reuse the last matched Pivot delta");
    Expect(depthxr::ResolvePivotPoseDeltaValue(
               pose_deltas, missed_frame + 1, 0, true, &held_pose_delta,
               &consecutive_misses, &pose_delta, &matched_time) ==
               depthxr::PivotPoseDeltaSelection::None &&
               pose_delta == 0 && consecutive_misses == 2,
           "A repeated cache miss must stop holding a stale Pivot delta");

    depthxr::CachePivotPoseDeltaValue(pose_deltas, missed_frame + 2, 84);
    Expect(depthxr::ResolvePivotPoseDeltaValue(
               pose_deltas, missed_frame + 2, 0, true, &held_pose_delta,
               &consecutive_misses, &pose_delta, &matched_time) ==
               depthxr::PivotPoseDeltaSelection::Matched &&
               pose_delta == 84 && consecutive_misses == 0,
           "A recovered match must reset the continuity miss budget");
    Expect(depthxr::ResolvePivotPoseDeltaValue(
               pose_deltas, missed_frame + 3, 0, false, &held_pose_delta,
               &consecutive_misses, &pose_delta, &matched_time) ==
               depthxr::PivotPoseDeltaSelection::Matched &&
               !held_pose_delta.has_value() && consecutive_misses == 0,
           "Inactive Pivot routing must disarm the held correction");
}

void TestPivotPoseDeltaSpaceReexpression() {
    using Matrix = std::array<double, 9>;
    using Vector = std::array<double, 3>;
    const auto multiply = [](const Matrix& lhs, const Matrix& rhs) {
        Matrix result{};
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 3; ++column) {
                for (size_t inner = 0; inner < 3; ++inner) {
                    result[row * 3 + column] +=
                        lhs[row * 3 + inner] * rhs[inner * 3 + column];
                }
            }
        }
        return result;
    };
    const auto inverse_rotation = [](const Matrix& matrix) {
        return Matrix{
            matrix[0], matrix[3], matrix[6],
            matrix[1], matrix[4], matrix[7],
            matrix[2], matrix[5], matrix[8],
        };
    };
    const auto transform = [](const Matrix& matrix, const Vector& vector) {
        return Vector{
            matrix[0] * vector[0] + matrix[1] * vector[1] + matrix[2] * vector[2],
            matrix[3] * vector[0] + matrix[4] * vector[1] + matrix[5] * vector[2],
            matrix[6] * vector[0] + matrix[7] * vector[1] + matrix[8] * vector[2],
        };
    };
    const auto nearly_equal = [](const Vector& lhs, const Vector& rhs) {
        for (size_t index = 0; index < lhs.size(); ++index) {
            if (std::abs(lhs[index] - rhs[index]) > 0.000001) {
                return false;
            }
        }
        return true;
    };
    const auto yaw = [](double radians) {
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        return Matrix{
            cosine, 0.0, sine,
            0.0, 1.0, 0.0,
            -sine, 0.0, cosine,
        };
    };
    const auto pitch = [](double radians) {
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        return Matrix{
            1.0, 0.0, 0.0,
            0.0, cosine, -sine,
            0.0, sine, cosine,
        };
    };

    constexpr double kPi = 3.14159265358979323846;
    const Matrix source_in_target = yaw(-112.0 * kPi / 180.0);
    const Matrix source_pitch_delta = pitch(30.0 * kPi / 180.0);
    const Matrix target_delta = depthxr::ReexpressPivotPoseDelta(
        source_pitch_delta, source_in_target, multiply, inverse_rotation);
    const Vector source_vector{0.2, 0.7, -0.4};

    // Conjugation must make applying the delta before or after the coordinate
    // change equivalent. A raw source-space pitch fails this property when the
    // spaces differ by yaw (the IL-2 diagonal-roll signature).
    const Vector expected = transform(
        source_in_target, transform(source_pitch_delta, source_vector));
    const Vector converted = transform(
        target_delta, transform(source_in_target, source_vector));
    const Vector unconverted = transform(
        source_pitch_delta, transform(source_in_target, source_vector));
    Expect(nearly_equal(converted, expected),
           "A Pivot delta must be conjugated into the projection layer's space");
    Expect(!nearly_equal(unconverted, expected),
           "Applying an unconverted pitch across yawed spaces must expose the regression test");

    // Repeat the group property with a homogeneous 2D rigid transform so the
    // positional part of a Pivot/neck offset is covered as well as rotation.
    const auto rigid_2d = [](double radians, double x, double y) {
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        return Matrix{
            cosine, -sine, x,
            sine, cosine, y,
            0.0, 0.0, 1.0,
        };
    };
    const auto inverse_rigid_2d = [](const Matrix& matrix) {
        const double inverse_x = -(matrix[0] * matrix[2] + matrix[3] * matrix[5]);
        const double inverse_y = -(matrix[1] * matrix[2] + matrix[4] * matrix[5]);
        return Matrix{
            matrix[0], matrix[3], inverse_x,
            matrix[1], matrix[4], inverse_y,
            0.0, 0.0, 1.0,
        };
    };
    const Matrix source_pose_in_target = rigid_2d(-1.2, 1.4, -0.6);
    const Matrix source_rigid_delta = rigid_2d(0.4, 0.25, -0.1);
    const Matrix target_rigid_delta = depthxr::ReexpressPivotPoseDelta(
        source_rigid_delta, source_pose_in_target, multiply, inverse_rigid_2d);
    const Vector source_point{0.3, -0.8, 1.0};
    const Vector rigid_expected = transform(
        source_pose_in_target, transform(source_rigid_delta, source_point));
    const Vector rigid_converted = transform(
        target_rigid_delta, transform(source_pose_in_target, source_point));
    Expect(nearly_equal(rigid_converted, rigid_expected),
           "A full rigid Pivot delta must preserve rotation and translation across spaces");
}

void TestNumpadActivationKeys() {
    Expect(depthxr::ParseActivationKey("numpad0") == std::optional<std::string>("Numpad0"),
           "Numpad 0 should normalize to the canonical binding name");
    Expect(depthxr::ParseActivationKey("Numpad5") == std::optional<std::string>("Numpad5"),
           "Numpad 5 should be accepted as a keyboard binding");
    Expect(depthxr::ParseActivationKey("NUMPAD9") == std::optional<std::string>("Numpad9"),
           "Numpad 9 should parse case-insensitively");
    Expect(!depthxr::ParseActivationKey("Numpad10").has_value(),
           "Multi-digit numpad names must remain unsupported");
}

void TestDeviceInputPathsAndHatDirections() {
    const std::optional<depthxr::DeviceInputPath> button =
        depthxr::ParseDeviceInputPath("button-12");
    Expect(button.has_value() &&
               button->kind == depthxr::DeviceInputKind::Button &&
               button->index == 11,
           "Button input paths must retain their one-based serialized index");

    const std::optional<depthxr::DeviceInputPath> hat =
        depthxr::ParseDeviceInputPath("hat-1-left");
    Expect(hat.has_value() &&
               hat->kind == depthxr::DeviceInputKind::Hat &&
               hat->index == 0 &&
               hat->direction == 6,
           "HAT input paths must parse the switch index and direction");

    Expect(!depthxr::ParseDeviceInputPath("hat-5-up").has_value(),
           "DirectInput exposes at most four HAT switches");
    Expect(!depthxr::ParseDeviceInputPath("hat-1-forward").has_value(),
           "Unknown HAT direction names must be rejected");
    Expect(!depthxr::ParseDeviceInputPath("button-129").has_value(),
           "Out-of-range button paths must be rejected");

    Expect(depthxr::DirectInputHatDirection(0) == std::optional<std::size_t>(0),
           "DirectInput HAT up must normalize to the up octant");
    Expect(depthxr::DirectInputHatDirection(4'500) == std::optional<std::size_t>(1),
           "DirectInput diagonal HAT positions must remain bindable");
    Expect(depthxr::DirectInputHatDirection(27'000) == std::optional<std::size_t>(6),
           "DirectInput HAT left must normalize to the left octant");
    Expect(depthxr::DirectInputHatDirection(35'999) == std::optional<std::size_t>(0),
           "HAT values near 360 degrees must wrap to up");
    Expect(!depthxr::DirectInputHatDirection(UINT32_MAX).has_value(),
           "A centered HAT must not report a pressed direction");
    Expect(!depthxr::DirectInputHatDirection(0x0000FFFFu).has_value(),
           "A HAT whose driver reports centered as 0xFFFF must not read as up-left");

    Expect(std::string(depthxr::ToString(depthxr::InputBindingPollStage::SetCooperativeLevel)) ==
               "set-cooperative-level",
           "Input diagnostics must retain the failing DirectInput setup stage");
    Expect(std::string(depthxr::ToString(depthxr::InputBindingPollStage::GetDeviceState)) ==
               "get-device-state",
           "Input diagnostics must distinguish device-state reads from acquisition failures");

    const depthxr::InputBindingPollResult inactive = depthxr::PollInputBinding(depthxr::InputBinding{});
    Expect(!inactive.down && !inactive.device_poll_attempted,
           "An unbound input must not be reported as a DirectInput polling attempt");

#if defined(_WIN32)
    depthxr::InputBinding invalid_device_binding;
    invalid_device_binding.type = depthxr::InputBindingType::Device;
    invalid_device_binding.device_guid = "{11111111-2222-3333-4444-555555555555}";
    invalid_device_binding.input_path = "button-129";
    const depthxr::InputBindingPollResult invalid_device =
        depthxr::PollInputBinding(invalid_device_binding);
    Expect(invalid_device.device_poll_attempted &&
               invalid_device.diagnostic_stage == depthxr::InputBindingPollStage::ParseInputPath,
           "Invalid device bindings must report their failing stage before touching DirectInput");

    depthxr::InputBinding missing_device_binding;
    missing_device_binding.type = depthxr::InputBindingType::Device;
    missing_device_binding.device_guid = "{11111111-2222-3333-4444-555555555555}";
    missing_device_binding.input_path = "button-1";
    const depthxr::InputBindingPollResult connecting_missing_device =
        depthxr::PollInputBinding(missing_device_binding);
    Expect(connecting_missing_device.device_poll_attempted &&
               connecting_missing_device.device_connect_pending &&
               !connecting_missing_device.down &&
               connecting_missing_device.diagnostic_stage == depthxr::InputBindingPollStage::None,
           "Device setup must be handed to the background connector, never run on the polling thread");
    depthxr::DrainInputDeviceWork(std::chrono::seconds(5));
    const depthxr::InputBindingPollResult first_missing_device =
        depthxr::PollInputBinding(missing_device_binding);
    const depthxr::InputBindingPollResult repeated_missing_device =
        depthxr::PollInputBinding(missing_device_binding);
    Expect(first_missing_device.device_poll_attempted &&
               !first_missing_device.device_retry_deferred &&
               !first_missing_device.device_connect_pending &&
               first_missing_device.diagnostic_stage != depthxr::InputBindingPollStage::None &&
               first_missing_device.device_retry_delay_ms > 0 &&
               first_missing_device.device_retry_delay_ms <= 250,
           "A background setup failure must surface once as a real attempt with a reconnect deadline");
    Expect(repeated_missing_device.device_retry_deferred &&
               repeated_missing_device.diagnostic_stage == first_missing_device.diagnostic_stage &&
               repeated_missing_device.result_code == first_missing_device.result_code,
           "A repeated missing-device binding must reuse the failure without touching DirectInput");
#endif
}

// An input held when a device (re)connects must not look like a press: the
// callers primed their edge detectors while the connector was still running.
void TestInputConnectPriming() {
    depthxr::InputConnectPriming priming;
    const std::wstring hotas = L"{hotas-guid}";
    Expect(priming.Filter(hotas, "button:1", true), "Unpublished devices must pass state through");
    priming.DeviceConnected(hotas);
    Expect(!priming.Filter(hotas, "button:1", true), "A switch held at connect must read released");
    Expect(!priming.Filter(hotas, "button:1", true), "A held switch must stay released until it is released once");
    Expect(!priming.Filter(hotas, "button:1", false), "A released switch must read released");
    Expect(priming.Filter(hotas, "button:1", true), "A press after release must register");
    Expect(!priming.Filter(hotas, "button:2", false) && priming.Filter(hotas, "button:2", true),
           "An input released at connect must register its first press");
    priming.DeviceConnected(hotas);
    Expect(!priming.Filter(hotas, "button:1", true), "A reconnect must re-prime held inputs");
    Expect(!priming.Filter(hotas, "button:2", false) && priming.Filter(hotas, "button:2", true),
           "Released inputs must work immediately after a reconnect");
}

// A bound device that is not connected (DirectInput: not registered). In a
// session it makes no timed attempts: each attempt stalled the game (Star
// Citizen's frames, 48-70 ms every 2.07 s, 2026-10-18). It is tried once per
// rescan (a device's arrival, Rescan inputs, a session's start); outside a
// session the reconnect timer still runs, and a device that is there but
// failing keeps the timer in a session too.
void TestInputDeviceAbsentWaitsForRescan() {
    using Backoff = depthxr::InputDeviceRetryBackoff;
    using namespace std::chrono_literals;
    const std::wstring stecs = L"{stecs-guid}";
    const std::wstring vkb = L"{vkb-guid}";
    const Backoff::TimePoint start{};

    Backoff backoff(100ms, 400ms);
    backoff.SetSessionActive(true);
    backoff.RecordAbsent(stecs, start);
    Expect(backoff.WaitsForRescan(stecs) && !backoff.ShouldAttempt(stecs, start + 10min) &&
               backoff.RetryDelayRemaining(stecs, start + 10min) == 0ms,
           "In a session, an absent device must make no timed attempts, however long it waits");
    backoff.Rescan();
    Expect(!backoff.WaitsForRescan(stecs) && backoff.ShouldAttempt(stecs, start + 10min),
           "A rescan must let an absent device be tried once");
    backoff.RecordAbsent(stecs, start + 10min);
    Expect(backoff.WaitsForRescan(stecs) && !backoff.ShouldAttempt(stecs, start + 20min),
           "Still absent after its rescan attempt, the device must wait for the next rescan");

    backoff.RecordFailure(vkb, start);
    Expect(!backoff.WaitsForRescan(vkb) && !backoff.ShouldAttempt(vkb, start + 99ms) &&
               backoff.ShouldAttempt(vkb, start + 100ms),
           "A device that is there but failing must keep the reconnect timer in a session");

    backoff.SetSessionActive(false);
    Expect(!backoff.WaitsForRescan(stecs) && backoff.ShouldAttempt(stecs, start + 20min),
           "Outside a session, an absent device must retry on the reconnect timer");
    backoff.SetSessionActive(true);
    Expect(backoff.WaitsForRescan(stecs), "Back in a session, the absent device must wait for a rescan again");

    backoff.Rescan();
    backoff.RecordSuccess(stecs);
    Expect(!backoff.WaitsForRescan(stecs) && backoff.ShouldAttempt(stecs, start + 20min) &&
               backoff.ConsecutiveFailures(stecs) == 0,
           "A device found on a rescan must clear its absent state");
    backoff.RecordAbsent(stecs, start + 21min);
    backoff.RecordFailure(stecs, start + 21min);
    Expect(!backoff.WaitsForRescan(stecs) && backoff.ShouldAttempt(stecs, start + 21min + 400ms),
           "A device that is there again after being absent must use the reconnect timer");

    // Ten minutes in a session with ten bindings on an unplugged throttle,
    // polled every 30 ms, and one device arrival (another controller plugged
    // in) after five: the session's start and that arrival are the only
    // attempts. On the timer alone it was one every 2 s (300).
    Backoff production_policy;
    production_policy.SetSessionActive(true);
    production_policy.Rescan(); // the session's start
    std::size_t attempts = 0;
    for (int tick = 0; tick <= 20'000; ++tick) {
        const Backoff::TimePoint now = start + tick * 30ms;
        if (tick == 10'000) production_policy.Rescan();
        for (int binding = 0; binding < 10; ++binding) {
            if (production_policy.ShouldAttempt(stecs, now)) {
                ++attempts;
                production_policy.RecordAbsent(stecs, now);
            }
        }
    }
    Expect(attempts == 2, "An unplugged device in a ten-minute session must be tried only at the session's start "
                          "and on the one device arrival");
}

// The connector looks for a device off the frame thread (CreateDevice, 13-70
// ms) while the frame thread keeps counting arrivals. A controller whose HID
// collections arrive one after another starts an attempt on the first and
// may still be unknown to DirectInput at it: the second arrival, during the
// attempt, must get it tried again, or its bindings stay dead for the session
// (on the 2 s timer of 0.19.2 the next attempt found it).
void TestInputDeviceArrivalDuringAttemptIsKept() {
    using Backoff = depthxr::InputDeviceRetryBackoff;
    using namespace std::chrono_literals;
    const std::wstring stecs = L"{stecs-guid}";
    const Backoff::TimePoint start{};

    Backoff backoff(100ms, 400ms);
    backoff.SetSessionActive(true);
    backoff.RecordAbsent(stecs, start);
    backoff.Rescan(); // the controller's first collection arrives
    Expect(backoff.ShouldAttempt(stecs, start + 1s), "An arrival must let the absent device be tried");
    const std::uint64_t attempted_after = backoff.Rescans(); // the connector takes the attempt
    backoff.Rescan();                                        // the joystick's collection arrives meanwhile
    backoff.RecordAbsent(stecs, start + 1s, attempted_after); // DirectInput did not know it yet
    Expect(!backoff.WaitsForRescan(stecs) && backoff.ShouldAttempt(stecs, start + 1s),
           "An arrival while an attempt ran must let the device be tried once more");
    const std::uint64_t retried_after = backoff.Rescans();
    backoff.RecordAbsent(stecs, start + 2s, retried_after);
    Expect(backoff.WaitsForRescan(stecs) && !backoff.ShouldAttempt(stecs, start + 10min),
           "With no arrival during its attempt, an absent device must wait for the next rescan");

    // An attempt the frame thread makes itself (a read under the poller's
    // lock, so no rescan between its start and its end) waits as before.
    backoff.Rescan();
    backoff.RecordAbsent(stecs, start + 11min);
    Expect(backoff.WaitsForRescan(stecs), "An absent result with no rescan since must wait for the next rescan");
}

void TestInputDeviceRetryBackoff() {
    using Backoff = depthxr::InputDeviceRetryBackoff;
    using namespace std::chrono_literals;

    Backoff backoff(100ms, 400ms);
    const std::wstring foxtrot = L"{foxtrot-guid}";
    const std::wstring throttle = L"{throttle-guid}";
    const Backoff::TimePoint start{};

    Expect(backoff.ShouldAttempt(foxtrot, start),
           "A device with no failure history must be polled immediately");
    Expect(backoff.RecordFailure(foxtrot, start) == 100ms &&
               backoff.ConsecutiveFailures(foxtrot) == 1,
           "The first unavailable-device retry must use the initial delay");
    Expect(!backoff.ShouldAttempt(foxtrot, start + 99ms) &&
               backoff.RetryDelayRemaining(foxtrot, start + 40ms) == 60ms,
           "Bindings for one unavailable device must share its retry window");
    Expect(backoff.ShouldAttempt(foxtrot, start + 100ms),
           "An unavailable device must become eligible at its reconnect deadline");
    Expect(backoff.ShouldAttempt(throttle, start + 50ms),
           "One unavailable device must not delay a different device");

    Expect(backoff.RecordFailure(foxtrot, start + 100ms) == 200ms &&
               backoff.RecordFailure(foxtrot, start + 300ms) == 400ms &&
               backoff.RecordFailure(foxtrot, start + 700ms) == 400ms,
           "Repeated reconnect failures must double to, but never exceed, the cap");
    Expect(!backoff.ShouldAttempt(foxtrot, start + 1099ms) &&
               backoff.ShouldAttempt(foxtrot, start + 1100ms),
           "The capped retry window must remain deterministic");

    backoff.RecordSuccess(foxtrot);
    Expect(backoff.ShouldAttempt(foxtrot, start + 701ms) &&
               backoff.ConsecutiveFailures(foxtrot) == 0 &&
               backoff.RetryDelayRemaining(foxtrot, start + 701ms) == 0ms,
           "A successful device read must clear reconnect backoff immediately");

    Backoff clamped(0ms, 0ms);
    Expect(clamped.RecordFailure(foxtrot, start) == 1ms,
           "A misconfigured reconnect policy must retain a nonzero retry delay");

    // Model ten Pivot bindings sharing one unplugged controller at the layer's
    // 30ms polling interval. Before the negative cache this produced roughly
    // 3,340 DirectInput setup attempts in ten seconds; the production policy
    // should permit only the bounded reconnect probes.
    Backoff production_policy;
    std::size_t device_attempts = 0;
    std::size_t deferred_bindings = 0;
    for (int tick = 0; tick <= 333; ++tick) {
        const Backoff::TimePoint now = start + tick * 30ms;
        for (int binding = 0; binding < 10; ++binding) {
            if (production_policy.ShouldAttempt(foxtrot, now)) {
                ++device_attempts;
                production_policy.RecordFailure(foxtrot, now);
            } else {
                ++deferred_bindings;
            }
        }
    }
    Expect(device_attempts <= 8 && deferred_bindings > 3'300,
           "Reconnect backoff must collapse an unplugged multi-binding input storm");
}

void TestQuadViewsCanvasDimensionsMatchCompositionDensity() {
    const depthxr::QuadViewsCanvasDimensions supersampled =
        depthxr::ComputeQuadViewsCanvasDimensions(6240, 6280, 16384, 16384, 1.1);
    Expect(supersampled.width == 6864 && supersampled.height == 6908,
           "Quadviews prewarm dimensions did not match the density-scaled composition canvas");
    Expect(std::abs(supersampled.density - 1.1) < 0.0001,
           "Quadviews canvas density did not retain focus supersampling");

    const depthxr::QuadViewsCanvasDimensions fallback_maximum =
        depthxr::ComputeQuadViewsCanvasDimensions(6240, 6280, 0, 0, 1.1);
    Expect(fallback_maximum.width == 6864 && fallback_maximum.height == 6908,
           "Quadviews canvas fallback maximum unexpectedly clamped the scaled dimensions");

    const depthxr::QuadViewsCanvasDimensions native =
        depthxr::ComputeQuadViewsCanvasDimensions(6240, 6280, 16384, 16384, 0.8);
    Expect(native.width == 6240 && native.height == 6280 && std::abs(native.density - 1.0) < 0.0001,
           "Quadviews canvas dropped below runtime-native density");

    const depthxr::QuadViewsCanvasDimensions clamped =
        depthxr::ComputeQuadViewsCanvasDimensions(1000, 900, 1050, 920, 1.2);
    Expect(clamped.width == 1050 && clamped.height == 920,
           "Quadviews canvas dimensions exceeded the runtime maximum");
}

depthxr::FrameGenFov FovFromTangents(double left, double right, double up, double down) {
    return {static_cast<float>(std::atan(left)), static_cast<float>(std::atan(right)),
            static_cast<float>(std::atan(up)), static_cast<float>(std::atan(down))};
}

void TestFrameGenComposeEye() {
    const auto close_to = [](float value, double expected) {
        return std::abs(value - expected) <= 1e-5 * (std::max)(1.0, std::abs(expected));
    };
    const auto relative = [](float value, double expected) {
        return std::abs(value - expected) <= 1e-4 * std::abs(expected);
    };
    const depthxr::FrameGenFov full = FovFromTangents(-1.0, 1.0, 1.0, -1.0);
    const depthxr::FrameGenFov focus = FovFromTangents(-0.5, 0.25, 0.5, -0.25);

    const depthxr::FrameGenComposeEye eye =
        depthxr::BuildFrameGenComposeEye(full, focus, {2000, 1000}, {1200, 900}, 10.0, 50.0);
    Expect(eye.peripheral == 0 && eye.focus == 0, "The compose builder should leave the view indices to the caller");
    Expect(close_to(eye.focus_rect[0], 0.25) && close_to(eye.focus_rect[1], 0.25) &&
               close_to(eye.focus_rect[2], 0.625) && close_to(eye.focus_rect[3], 0.625),
           "The focus rectangle should lie where the focus view's tangents fall in the full view's");
    Expect(close_to(eye.feather[0], 0.0375) && close_to(eye.feather[1], 0.0375),
           "The blend band should be the transition's share of the focus rectangle");
    Expect(eye.sharpen == 0.5f, "Sharpness 50 should sharpen by half");
    Expect(eye.focus_texel[0] == 1.0f / 1200.0f && eye.focus_texel[1] == 1.0f / 900.0f,
           "The focus texel should be one over the focus view's extent");
    Expect(relative(eye.output_pixel_in_focus[0], (1.0 / 2000.0) / 0.375) &&
               relative(eye.output_pixel_in_focus[1], (1.0 / 1000.0) / 0.375),
           "One output pixel should be expressed in focus uv");

    // A focus view reaching past the full view's left and bottom edges.
    const depthxr::FrameGenComposeEye edge = depthxr::BuildFrameGenComposeEye(
        full, FovFromTangents(-1.5, 0.5, 0.5, -2.0), {2000, 1000}, {1200, 900}, 10.0, 50.0);
    Expect(edge.focus_rect[0] == 0.0f && edge.focus_rect[3] == 1.0f && close_to(edge.focus_rect[1], 0.25) &&
               close_to(edge.focus_rect[2], 0.75),
           "The focus rectangle should be clamped at the full view's edges");
    Expect(close_to(edge.feather[0], 0.075) && close_to(edge.feather[1], 0.075),
           "The blend band should follow the clamped focus rectangle");

    // The blend band is never under one output pixel.
    for (const double transition : {0.0, -20.0, 0.01}) {
        const depthxr::FrameGenComposeEye hard =
            depthxr::BuildFrameGenComposeEye(full, focus, {2000, 1000}, {1200, 900}, transition, 50.0);
        Expect(hard.feather[0] == static_cast<float>(1.0 / 2000.0) &&
                   hard.feather[1] == static_cast<float>(1.0 / 1000.0),
               "A transition under one output pixel should keep a one-pixel blend band");
    }
    const depthxr::FrameGenComposeEye empty = depthxr::BuildFrameGenComposeEye(full, focus, {0, 0}, {0, 0}, 0.0, 50.0);
    Expect(empty.feather[0] == 1.0f && empty.feather[1] == 1.0f && empty.focus_texel[0] == 1.0f &&
               empty.focus_texel[1] == 1.0f,
           "Empty extents should count as one pixel");
    const depthxr::FrameGenComposeEye wide =
        depthxr::BuildFrameGenComposeEye(full, focus, {2000, 1000}, {1200, 900}, 250.0, 50.0);
    Expect(close_to(wide.feather[0], 0.375) && close_to(wide.feather[1], 0.375),
           "A transition over 100% should blend across the whole focus rectangle");

    const std::array<std::pair<double, float>, 5> sharpness{{
        {-10.0, 0.0f},
        {0.0, 0.0f},
        {37.0, static_cast<float>(37.0 / 100.0)},
        {100.0, 1.0f},
        {250.0, 1.0f},
    }};
    for (const auto& [percent, expected] : sharpness) {
        Expect(depthxr::BuildFrameGenComposeEye(full, focus, {2000, 1000}, {1200, 900}, 10.0, percent).sharpen ==
                   expected,
               "Sharpness should be clamped to 0-100 and scaled to 0-1");
    }

    const depthxr::FrameGenConstants constants = depthxr::BuildFrameGenComposeConstants(eye, {2000, 1000});
    Expect(constants.fov_current == eye.focus_rect, "The compose constants should carry the focus rectangle");
    Expect(constants.fov_previous == std::array<float, 4>{eye.feather[0], eye.feather[1], eye.sharpen, 0.0f},
           "The compose constants should carry the blend band and sharpening");
    Expect(constants.rot0 == std::array<float, 4>{eye.focus_texel[0], eye.focus_texel[1], eye.output_pixel_in_focus[0],
                                                  eye.output_pixel_in_focus[1]},
           "The compose constants should carry the focus texel and one output pixel in focus uv");
    Expect(constants.size == std::array<std::uint32_t, 4>{2000, 1000, 0, 0},
           "The compose constants should carry the output extent");
    Expect(constants.options == std::array<std::uint32_t, 4>{} && constants.rot1 == std::array<float, 4>{} &&
               constants.rot2 == std::array<float, 4>{} && constants.tuning == std::array<float, 4>{},
           "The compose constants' other fields should be zero");

    // The diagnostic overlay: its flags, markers and the output pixel the
    // Direct3D 11 compositor computes (1 / the extent, in float).
    depthxr::FrameGenComposeEye overlaid = eye;
    overlaid.overlay.gaze_markers = {0.4f, 0.45f, 0.42f, 0.46f};
    overlaid.overlay.reference_markers = {0.5f, 0.5f, 0.55f, 0.48f};
    Expect(depthxr::BuildFrameGenComposeConstants(overlaid, {2000, 1000}).options == std::array<std::uint32_t, 4>{},
           "An overlay that is not shown should leave the compose constants as they are");
    overlaid.overlay.shown = true;
    const struct {
        bool eye_tracking, gaze;
        std::uint32_t flags;
    } overlay_cases[] = {
        {false, false, depthxr::kFrameGenComposeOverlay},
        {true, false, depthxr::kFrameGenComposeOverlay | depthxr::kFrameGenComposeOverlayEyeTracking},
        {true, true,
         depthxr::kFrameGenComposeOverlay | depthxr::kFrameGenComposeOverlayEyeTracking |
             depthxr::kFrameGenComposeOverlayGaze},
    };
    for (const auto& overlay_case : overlay_cases) {
        overlaid.overlay.eye_tracking = overlay_case.eye_tracking;
        overlaid.overlay.gaze = overlay_case.gaze;
        const depthxr::FrameGenConstants shown = depthxr::BuildFrameGenComposeConstants(overlaid, {2000, 1000});
        Expect(shown.options == std::array<std::uint32_t, 4>{overlay_case.flags, 0, 0, 0},
               "The compose constants should carry the overlay's flags");
        Expect(shown.rot1 == overlaid.overlay.gaze_markers && shown.rot2 == overlaid.overlay.reference_markers,
               "The compose constants should carry the overlay's markers");
        Expect(shown.tuning == std::array<float, 4>{1.0f / 2000.0f, 1.0f / 1000.0f, 0.0f, 0.0f},
               "The compose constants should carry one output pixel for the overlay's shapes");
        Expect(shown.fov_current == constants.fov_current && shown.fov_previous == constants.fov_previous &&
                   shown.rot0 == constants.rot0 && shown.size == constants.size,
               "The overlay should leave the compose itself as it is");
    }
}

// The quad views compositor's focus rectangle arithmetic before the shared
// builder: focus_rect, blend_params and focus_texel, 4 floats each.
std::array<float, 12> ReferenceFocusRectConstants(const depthxr::FrameGenFov& full_fov,
                                                  const depthxr::FrameGenFov& focus_fov,
                                                  std::uint32_t width,
                                                  std::uint32_t height,
                                                  std::uint32_t focus_width,
                                                  std::uint32_t focus_height,
                                                  double transition_thickness_percent,
                                                  double foveate_sharpness) {
    const auto clamp = [](double value, double min_value, double max_value) {
        return (std::max)(min_value, (std::min)(max_value, value));
    };
    const double full_left = std::tan(full_fov.left);
    const double full_right = std::tan(full_fov.right);
    const double full_down = std::tan(full_fov.down);
    const double full_up = std::tan(full_fov.up);
    const double focus_left = std::tan(focus_fov.left);
    const double focus_right = std::tan(focus_fov.right);
    const double focus_down = std::tan(focus_fov.down);
    const double focus_up = std::tan(focus_fov.up);
    const double full_width = (std::max)(0.0001, full_right - full_left);
    const double full_height = (std::max)(0.0001, full_up - full_down);

    std::array<float, 12> constants{};
    float* focus_rect = constants.data();
    float* blend_params = constants.data() + 4;
    float* focus_texel = constants.data() + 8;
    focus_rect[0] = static_cast<float>(clamp((focus_left - full_left) / full_width, 0.0, 1.0));
    focus_rect[2] = static_cast<float>(clamp((focus_right - full_left) / full_width, 0.0, 1.0));
    focus_rect[1] = static_cast<float>(clamp((full_up - focus_up) / full_height, 0.0, 1.0));
    focus_rect[3] = static_cast<float>(clamp((full_up - focus_down) / full_height, 0.0, 1.0));

    const double focus_rect_width = (std::max)(0.0001, static_cast<double>(focus_rect[2]) - focus_rect[0]);
    const double focus_rect_height = (std::max)(0.0001, static_cast<double>(focus_rect[3]) - focus_rect[1]);
    const double transition_fraction = clamp(transition_thickness_percent, 0.0, 100.0) / 100.0;
    blend_params[0] =
        static_cast<float>((std::max)(1.0 / std::max<std::uint32_t>(1, width), focus_rect_width * transition_fraction));
    blend_params[1] = static_cast<float>(
        (std::max)(1.0 / std::max<std::uint32_t>(1, height), focus_rect_height * transition_fraction));
    blend_params[2] = static_cast<float>(clamp(foveate_sharpness, 0.0, 100.0) / 100.0);
    blend_params[3] = 0.0f;
    focus_texel[0] = 1.0f / static_cast<float>(std::max<std::uint32_t>(1, focus_width));
    focus_texel[1] = 1.0f / static_cast<float>(std::max<std::uint32_t>(1, focus_height));
    focus_texel[2] = static_cast<float>((1.0 / std::max<std::uint32_t>(1, width)) / focus_rect_width);
    focus_texel[3] = static_cast<float>((1.0 / std::max<std::uint32_t>(1, height)) / focus_rect_height);
    return constants;
}

void TestFrameGenComposeEyeMatchesCompositor() {
    // A fixed-seed generator, so every run checks the same 50 pairs.
    std::uint64_t state = 0x5EED0F0CA1ULL;
    const auto random = [&state](double low, double high) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return low + (high - low) * static_cast<double>(state >> 11) / 9007199254740992.0;
    };
    int clamped = 0;
    for (int pair = 0; pair < 50; ++pair) {
        const depthxr::FrameGenFov full{static_cast<float>(random(-1.2, -0.5)), static_cast<float>(random(0.5, 1.2)),
                                        static_cast<float>(random(0.5, 1.1)), static_cast<float>(random(-1.2, -0.5))};
        // Some focus views reach past the full view's edges.
        const double center_x = random(-0.6, 0.6);
        const double center_y = random(-0.5, 0.5);
        const double half_width = random(0.1, 0.7);
        const double half_height = random(0.1, 0.7);
        const depthxr::FrameGenFov focus{
            static_cast<float>(center_x - half_width), static_cast<float>(center_x + half_width),
            static_cast<float>(center_y + half_height), static_cast<float>(center_y - half_height)};
        const auto width = static_cast<std::uint32_t>(random(500.0, 4000.0));
        const auto height = static_cast<std::uint32_t>(random(500.0, 4000.0));
        const auto focus_width = static_cast<std::uint32_t>(random(400.0, 3000.0));
        const auto focus_height = static_cast<std::uint32_t>(random(400.0, 3000.0));
        const double transition = random(-10.0, 110.0);
        const double sharpness = random(-10.0, 110.0);

        const depthxr::FrameGenComposeEye eye = depthxr::BuildFrameGenComposeEye(
            full, focus, {width, height}, {focus_width, focus_height}, transition, sharpness);
        const std::array<float, 12> shared{
            eye.focus_rect[0], eye.focus_rect[1], eye.focus_rect[2], eye.focus_rect[3],
            eye.feather[0], eye.feather[1], eye.sharpen, 0.0f,
            eye.focus_texel[0], eye.focus_texel[1], eye.output_pixel_in_focus[0], eye.output_pixel_in_focus[1]};
        const std::array<float, 12> reference = ReferenceFocusRectConstants(
            full, focus, width, height, focus_width, focus_height, transition, sharpness);
        for (std::size_t value = 0; value < shared.size(); ++value) {
            Expect(std::bit_cast<std::uint32_t>(shared[value]) == std::bit_cast<std::uint32_t>(reference[value]),
                   "The compose builder should match the quadviews compositor bit for bit (pair " +
                       std::to_string(pair) + ", value " + std::to_string(value) + ")");
        }
        if (std::any_of(eye.focus_rect.begin(), eye.focus_rect.end(),
                        [](float edge) { return edge == 0.0f || edge == 1.0f; })) {
            ++clamped;
        }
    }
    Expect(clamped > 0, "The compose builder comparison should include focus views clamped at an edge");
}

void TestSwapchainImageQueuePreservesFifo() {
    depthxr::SwapchainImageQueue queue;
    queue.Acquire(2);
    queue.Acquire(0);
    Expect(queue.Size() == 2, "Swapchain queue did not retain both acquired images");

    Expect(queue.WaitOldest() == 2, "First wait did not target the oldest acquired image");
    Expect(queue.WaitOldest() == 0, "Second wait did not advance to the next acquired image");
    Expect(!queue.WaitOldest().has_value(), "Wait succeeded without an un-waited acquired image");

    Expect(queue.ReleaseOldest() == 2, "First app release did not target the oldest waited image");
    Expect(queue.ReleaseOldest() == 0, "Second app release did not preserve FIFO order");
    Expect(queue.PendingDownstreamReleases() == 2,
           "Deferred downstream release count did not retain both app releases");

    Expect(queue.ReleaseDownstreamOldest() == 2,
           "First downstream release did not target the oldest app-released image");
    Expect(queue.Size() == 1, "Completed FIFO prefix was not retired");
    Expect(queue.ReleaseDownstreamOldest() == 0,
           "Second downstream release did not target the remaining image");
    Expect(queue.Size() == 0, "Fully released queue was not emptied");
}

constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;

// Extra yaw in degrees for a x2 multiplier, 8-degree deadzone and the given
// Deadzone Transition width.
double SoftDeadzoneExtraDegrees(double angle_degrees, double transition_degrees) {
    return depthxr::ComputePivotContinuousExtra(angle_degrees * kRadiansPerDegree, 2.0, 8.0, transition_degrees,
                                                180.0) /
           kRadiansPerDegree;
}

void TestPivotSoftDeadzoneKeepsOneToOneInsideTheDeadzone() {
    Expect(SoftDeadzoneExtraDegrees(7.0, 4.0) == 0.0 && SoftDeadzoneExtraDegrees(8.0, 4.0) == 0.0,
           "Soft deadzone must stay 1:1 up to the deadzone");
}

void TestPivotSoftDeadzoneIntegratesSmoothstepSensitivity() {
    Expect(std::abs(SoftDeadzoneExtraDegrees(12.0, 4.0) - 2.0) < 1e-9,
           "Soft deadzone must add half the transition's linear extra at the transition's far edge");
    Expect(std::abs(SoftDeadzoneExtraDegrees(20.0, 4.0) - 10.0) < 1e-9,
           "Beyond the transition the soft deadzone must give up transition/2 x (gain - 1) of reach");
    Expect(std::abs(SoftDeadzoneExtraDegrees(20.0, 0.0) - 12.0) < 1e-9,
           "A zero transition must reproduce the legacy hard deadzone");
    Expect(std::abs(SoftDeadzoneExtraDegrees(-20.0, 4.0) + SoftDeadzoneExtraDegrees(20.0, 4.0)) < 1e-9,
           "Soft deadzone must be symmetric for left and right");
}

void TestPivotSoftDeadzoneIsMonotonicAndSmooth() {
    double previous = 0.0;
    for (double angle = 8.0; angle < 180.0; angle += 0.1) {
        const double next = SoftDeadzoneExtraDegrees(angle, 4.0);
        Expect(next >= previous, "Soft deadzone extra must never decrease as the head turns further");
        previous = next;
    }
    constexpr double kStep = 0.0001;
    const double entry_slope =
        (SoftDeadzoneExtraDegrees(8.0 + kStep, 4.0) - SoftDeadzoneExtraDegrees(8.0, 4.0)) / kStep;
    Expect(std::abs(entry_slope) < 0.001, "Sensitivity must not step up where the transition starts");
    const double exit_slope =
        (SoftDeadzoneExtraDegrees(12.0 + kStep, 4.0) - SoftDeadzoneExtraDegrees(12.0 - kStep, 4.0)) / (2.0 * kStep);
    Expect(std::abs(exit_slope - 1.0) < 0.001, "Sensitivity must reach the full multiplier smoothly");
}

void TestPivotPositionAssistBasicCurve() {
    depthxr::PivotPositionAssist assist;
    assist.enabled = true;
    assist.yaw = {10.0, 4.0, -2.0, 0.0, 90.0};
    Expect(depthxr::PivotViewOffsetNearlyZero(depthxr::ComputePivotPositionAssist(0.0, 0.0, assist)),
           "Position Assist must not move the view at neutral");
    const depthxr::PivotViewOffset half = depthxr::ComputePivotPositionAssist(-45.0 * kRadiansPerDegree, 0.0, assist);
    Expect(std::abs(half.right_meters - 0.05) < 1e-9 && std::abs(half.up_meters - 0.02) < 1e-9,
           "Position Assist must ease to half its offsets midway between the start and full angles");
    const depthxr::PivotViewOffset full = depthxr::ComputePivotPositionAssist(-150.0 * kRadiansPerDegree, 0.0, assist);
    Expect(std::abs(full.right_meters - 0.1) < 1e-9 && std::abs(full.forward_meters + 0.02) < 1e-9,
           "Position Assist must hold its configured offsets beyond the full angle");
    const depthxr::PivotViewOffset left = depthxr::ComputePivotPositionAssist(45.0 * kRadiansPerDegree, 0.0, assist);
    Expect(std::abs(left.right_meters + 0.05) < 1e-9, "Basic sideways lean must mirror between left and right turns");
}

void TestPivotPositionAssistAdvancedDirectionsCompose() {
    depthxr::PivotPositionAssist assist;
    assist.enabled = true;
    assist.advanced_directions = true;
    assist.yaw_left = {-12.0, 2.0, 0.0, 0.0, 45.0};
    assist.pitch_up = {0.0, 8.0, 3.0, 0.0, 30.0};
    const depthxr::PivotViewOffset combined =
        depthxr::ComputePivotPositionAssist(45.0 * kRadiansPerDegree, 30.0 * kRadiansPerDegree, assist);
    Expect(std::abs(combined.right_meters + 0.12) < 1e-9 && std::abs(combined.up_meters - 0.1) < 1e-9,
           "Advanced Position Assist directions must add together");
}

void TestExpandedPivotSettingsResolve() {
    const depthxr::ParseResult parsed = depthxr::ParseConfig(R"json(
{
  "version": 3,
  "core": {},
  "applications": [],
  "modules": {
    "depthxr": {},
    "pivotxr": {
      "enabled": true,
      "defaults": {
        "applyMotionToSnapViews": true,
        "deadzoneTransitionDegrees": 4,
        "pitchDeadzoneTransitionDegrees": 2,
        "positionAssist": {
          "enabled": true,
          "yaw": { "rightCm": 10, "upCm": 4, "startAngleDegrees": 0, "fullAngleDegrees": 90 }
        }
      }
    }
  }
}
)json");
    Expect(parsed.ok, "Expanded Pivot settings must parse: " + parsed.error);
    const depthxr::PivotXrResolvedProfile profile =
        depthxr::ResolveRuntimeConfig(parsed.document, "FlightSimulator.exe").pivotxr.profiles[0];
    Expect(profile.apply_motion_to_snap_views, "Apply Enhanced Motion to Snap Views must resolve");
    Expect(profile.position_assist.enabled && std::abs(profile.position_assist.yaw.right_cm - 10.0) < 1e-9,
           "Position Assist must resolve");
    Expect(profile.yaw_positive.deadzone_transition_degrees == 4.0 &&
               profile.yaw_negative.deadzone_transition_degrees == 4.0,
           "The basic yaw Deadzone Transition must apply to both yaw directions");
    Expect(profile.pitch_positive.deadzone_transition_degrees == 2.0 &&
               profile.pitch_negative.deadzone_transition_degrees == 2.0,
           "The basic pitch Deadzone Transition must apply to both pitch directions");
}

void TestPerDirectionDeadzoneTransitionParsing() {
    const depthxr::ParseResult parsed = depthxr::ParseConfig(R"json(
{
  "version": 3,
  "core": {},
  "applications": [],
  "modules": {
    "depthxr": {},
    "pivotxr": {
      "enabled": true,
      "defaults": {
        "advancedAxes": true,
        "deadzoneTransitionDegrees": 9,
        "yawLeft": { "deadzoneTransitionDegrees": 1 },
        "yawRight": { "deadzoneTransitionDegrees": 2 },
        "pitchUp": { "deadzoneTransitionDegrees": 3 },
        "pitchDown": { "deadzoneTransitionDegrees": 4 }
      }
    }
  }
}
)json");
    Expect(parsed.ok, "Per-direction Deadzone Transition must parse: " + parsed.error);
    const depthxr::PivotXrResolvedProfile profile =
        depthxr::ResolveRuntimeConfig(parsed.document, "FlightSimulator.exe").pivotxr.profiles[0];
    Expect(profile.yaw_positive.deadzone_transition_degrees == 1.0, "Yaw Left Deadzone Transition was not parsed");
    Expect(profile.yaw_negative.deadzone_transition_degrees == 2.0, "Yaw Right Deadzone Transition was not parsed");
    Expect(profile.pitch_positive.deadzone_transition_degrees == 3.0, "Pitch Up Deadzone Transition was not parsed");
    Expect(profile.pitch_negative.deadzone_transition_degrees == 4.0, "Pitch Down Deadzone Transition was not parsed");

    const depthxr::ParseResult out_of_range = depthxr::ParseConfig(
        R"({"version":3,"core":{},"applications":[],"modules":{"depthxr":{},"pivotxr":{"defaults":{"advancedAxes":true,"pitchDown":{"deadzoneTransitionDegrees":181}}}}})");
    Expect(!out_of_range.ok && out_of_range.error.find("pitchDown") != std::string::npos,
           "An out-of-range per-direction Deadzone Transition must be rejected and name its direction");
}

void TestInvertedPositionAssistRangeRejected() {
    const depthxr::ParseResult parsed = depthxr::ParseConfig(
        R"({"version":3,"core":{},"applications":[],"modules":{"depthxr":{},"pivotxr":{"defaults":{"positionAssist":{"yaw":{"startAngleDegrees":60,"fullAngleDegrees":20}}}}}})");
    Expect(!parsed.ok, "A Position Assist full angle below its start angle must be rejected");
}

// DXGI's user-mode driver version: a.b.c.d in four 16-bit fields.
std::uint64_t UserModeDriverVersion(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) {
    return (a << 48) | (b << 32) | (c << 16) | d;
}

void TestSessionGraphicsDriverVersions() {
    using depthxr::DxgiDriverVersionText;
    using depthxr::VulkanDriverVersionText;
    constexpr std::uint32_t kNvidia = 0x10de;
    Expect(DxgiDriverVersionText(UserModeDriverVersion(32, 0, 15, 8142), kNvidia) == "581.42 (32.0.15.8142)",
           "NVIDIA's DXGI driver version must read as NVIDIA shows it: " +
               DxgiDriverVersionText(UserModeDriverVersion(32, 0, 15, 8142), kNvidia));
    Expect(DxgiDriverVersionText(UserModeDriverVersion(31, 0, 15, 5222), kNvidia) == "552.22 (31.0.15.5222)",
           "NVIDIA 552.22 must decode from 31.0.15.5222");
    Expect(DxgiDriverVersionText(UserModeDriverVersion(27, 21, 14, 5671), kNvidia) == "456.71 (27.21.14.5671)",
           "NVIDIA 456.71 must decode from 27.21.14.5671");
    Expect(DxgiDriverVersionText(UserModeDriverVersion(32, 0, 16, 5), kNvidia) == "600.05 (32.0.16.5)",
           "NVIDIA's hundredths must keep their leading zero: " +
               DxgiDriverVersionText(UserModeDriverVersion(32, 0, 16, 5), kNvidia));
    Expect(DxgiDriverVersionText(UserModeDriverVersion(32, 0, 15, 8142), 0x1002u) == "32.0.15.8142",
           "Other vendors' DXGI driver versions must read as DXGI has them");
    Expect(DxgiDriverVersionText(UserModeDriverVersion(10, 0, 26100, 4061), 0x1414u) == "10.0.26100.4061",
           "WARP's DXGI driver version must read as DXGI has it, with fields above 9999");
    Expect(DxgiDriverVersionText(UserModeDriverVersion(32, 0, 15, 8142), std::nullopt) == "32.0.15.8142",
           "An unknown vendor's DXGI driver version must not be read as NVIDIA's");

    // NVIDIA packs Vulkan's driverVersion as 10 bits major, 8 bits minor.
    constexpr std::uint32_t kVulkan58142 = (581u << 22) | (42u << 14);
    Expect(VulkanDriverVersionText(kVulkan58142, kNvidia) == "581.42 (0x914a8000)",
           "NVIDIA's Vulkan driver version must read as NVIDIA shows it: " +
               VulkanDriverVersionText(kVulkan58142, kNvidia));
    Expect(VulkanDriverVersionText((551u << 22) | (6u << 14) | (3u << 6) | 1u, kNvidia) == "551.06 (0x89c180c1)",
           "NVIDIA's Vulkan minor version must keep its leading zero: " +
               VulkanDriverVersionText((551u << 22) | (6u << 14) | (3u << 6) | 1u, kNvidia));
    Expect(VulkanDriverVersionText(0x00801234u, 0x1002u) == "0x00801234",
           "Other vendors' Vulkan driver versions must stay raw, in eight hex digits");
}

void TestSessionGraphicsVideoMemory() {
    using depthxr::VideoMemoryText;
    constexpr std::uint64_t kMiB = 1ull << 20;
    constexpr std::uint64_t kGiB = 1ull << 30;
    Expect(VideoMemoryText(0) == "0 MiB", "No dedicated video memory (WARP) must read 0 MiB");
    Expect(VideoMemoryText(512 * kMiB) == "512 MiB", "Video memory below 1 GiB must read in MiB");
    Expect(VideoMemoryText(kGiB) == "1.0 GiB", "1 GiB must read 1.0 GiB");
    Expect(VideoMemoryText(kGiB - kMiB / 4) == "1.0 GiB",
           "Just under 1 GiB rounds to 1024 MiB and must read 1.0 GiB: " + VideoMemoryText(kGiB - kMiB / 4));
    Expect(VideoMemoryText(kGiB - kMiB / 2 - 1) == "1023 MiB",
           "What rounds below 1024 MiB must stay in MiB: " + VideoMemoryText(kGiB - kMiB / 2 - 1));
    Expect(VideoMemoryText(32607 * kMiB) == "31.8 GiB",
           "32607 MiB must read 31.8 GiB: " + VideoMemoryText(32607 * kMiB));
    Expect(VideoMemoryText(kGiB + kGiB * 96 / 100) == "2.0 GiB",
           "Rounding must carry into the whole GiB: " + VideoMemoryText(kGiB + kGiB * 96 / 100));
}

void TestSessionGraphicsLine() {
    constexpr std::uint64_t kGiB = 1ull << 30;
    depthxr::SessionGraphics d3d12;
    d3d12.api = "Direct3D 12";
    d3d12.adapter = "NVIDIA GeForce RTX 5090";
    d3d12.vendor_id = 0x10de;
    d3d12.driver_version = UserModeDriverVersion(32, 0, 15, 8142);
    d3d12.dedicated_video_memory = 32607ull << 20;
    d3d12.budget = 29 * kGiB + kGiB * 6 / 10;
    d3d12.usage = kGiB + kGiB * 2 / 10;
    std::string line = depthxr::FormatSessionGraphics(d3d12, "PimaxXR", "1.2.3");
    Expect(line == "Session graphics: api=Direct3D 12, adapter=\"NVIDIA GeForce RTX 5090\", vendor=0x10de, "
                   "driver=581.42 (32.0.15.8142), dedicatedVideoMemory=31.8 GiB, budget=29.6 GiB "
                   "(process uses 1.2 GiB), runtime=\"PimaxXR\" 1.2.3",
           "A Direct3D 12 session's graphics line must name everything found: " + line);

    // A Vulkan device whose LUID could not be read, its adapter matched by
    // PCI IDs: DXGI's figures, and how the adapter was found.
    depthxr::SessionGraphics matched = d3d12;
    matched.api = "Vulkan";
    matched.vulkan_driver_version = (581u << 22) | (42u << 14);
    matched.problem = depthxr::AdapterIdMatchText(depthxr::AdapterIdMatch{std::size_t{0}, 1},
                                                  "the Vulkan device names no adapter LUID");
    line = depthxr::FormatSessionGraphics(matched, "SteamVR/OpenXR", "2.7.4");
    Expect(line == "Session graphics: api=Vulkan, adapter=\"NVIDIA GeForce RTX 5090\", vendor=0x10de, "
                   "driver=581.42 (32.0.15.8142), vulkanDriver=581.42 (0x914a8000), dedicatedVideoMemory=31.8 GiB, "
                   "budget=29.6 GiB (process uses 1.2 GiB), runtime=\"SteamVR/OpenXR\" 2.7.4; adapter matched by "
                   "PCI vendor and device ID, not by LUID: the Vulkan device names no adapter LUID",
           "A Vulkan session's graphics line must say when its adapter was matched by PCI IDs: " + line);

    // Neither the LUID nor the IDs: Vulkan's own figures, and why DXGI's are
    // missing.
    depthxr::SessionGraphics vulkan;
    vulkan.api = "Vulkan";
    vulkan.adapter = "NVIDIA GeForce RTX 5090";
    vulkan.vendor_id = 0x10de;
    vulkan.vulkan_driver_version = (581u << 22) | (42u << 14);
    vulkan.problem = depthxr::AdapterIdMatchText({}, "the Vulkan device names no adapter LUID");
    line = depthxr::FormatSessionGraphics(vulkan, "SteamVR/OpenXR", "2.7.4");
    Expect(line == "Session graphics: api=Vulkan, adapter=\"NVIDIA GeForce RTX 5090\", vendor=0x10de, driver=unknown, "
                   "vulkanDriver=581.42 (0x914a8000), dedicatedVideoMemory=unknown, budget=unknown, "
                   "runtime=\"SteamVR/OpenXR\" 2.7.4; DXGI has no adapter with the Vulkan device's PCI vendor and "
                   "device ID, and its LUID is unknown: the Vulkan device names no adapter LUID",
           "A Vulkan session's graphics line must say what is missing and why: " + line);

    // WARP: no dedicated video memory, a budget without its usage.
    depthxr::SessionGraphics warp;
    warp.api = "Direct3D 12 (Frame Generation's Direct3D 11 bridge)";
    warp.adapter = "Microsoft Basic Render Driver";
    warp.vendor_id = 0x1414;
    warp.dedicated_video_memory = 0;
    warp.budget = 512ull << 20;
    line = depthxr::FormatSessionGraphics(warp, "VectorXR stand-in runtime", "");
    Expect(line == "Session graphics: api=Direct3D 12 (Frame Generation's Direct3D 11 bridge), "
                   "adapter=\"Microsoft Basic Render Driver\", vendor=0x1414, driver=unknown, "
                   "dedicatedVideoMemory=0 MiB, budget=512 MiB, runtime=\"VectorXR stand-in runtime\"",
           "A bridge's graphics line must name the bridge, and a runtime without a version: " + line);

    // Nothing known, not even the runtime.
    depthxr::SessionGraphics none;
    none.api = "none";
    none.problem = "the session has no Direct3D 11, Direct3D 12 or Vulkan binding";
    line = depthxr::FormatSessionGraphics(none, "", "");
    Expect(line == "Session graphics: api=none, adapter=unknown, vendor=unknown, driver=unknown, "
                   "dedicatedVideoMemory=unknown, budget=unknown, runtime=unknown; the session has no "
                   "Direct3D 11, Direct3D 12 or Vulkan binding",
           "A session without a known binding must still get a graphics line: " + line);
}

// A Vulkan device whose LUID cannot be read safely: DXGI's first adapter with
// its PCI vendor and device ID, and how many have them.
void TestSessionGraphicsAdapterIds() {
    using depthxr::AdapterIdMatchText;
    using depthxr::MatchAdapterByIds;
    const std::vector<depthxr::PciIds> adapters{
        {0x10de, 0x2b85}, {0x1002, 0x13c0}, {0x1414, 0x008c}, {0x10de, 0x2b85}};
    depthxr::AdapterIdMatch match = MatchAdapterByIds(adapters, {0x1002, 0x13c0});
    Expect(match.first == std::size_t{1} && match.count == 1, "The one adapter with a device's IDs must be matched");
    match = MatchAdapterByIds(adapters, {0x10de, 0x2b85});
    Expect(match.first == std::size_t{0} && match.count == 2,
           "Of two adapters with a device's IDs, the first in DXGI's order must be matched, and both counted");
    match = MatchAdapterByIds(adapters, {0x10de, 0x2684});
    Expect(!match.first && match.count == 0, "The vendor's ID alone must not match an adapter");
    match = MatchAdapterByIds(adapters, {0x13c0, 0x1002});
    Expect(!match.first && match.count == 0, "A vendor ID must not match a device ID");
    match = MatchAdapterByIds({}, {0x10de, 0x2b85});
    Expect(!match.first && match.count == 0, "No adapters must match nothing");

    const std::string why = "the Vulkan device names no adapter LUID";
    std::string text = AdapterIdMatchText(depthxr::AdapterIdMatch{std::size_t{1}, 1}, why);
    Expect(text == "adapter matched by PCI vendor and device ID, not by LUID: " + why,
           "An adapter matched by PCI IDs must say so and why: " + text);
    text = AdapterIdMatchText(depthxr::AdapterIdMatch{std::size_t{0}, 2}, why);
    Expect(text == "adapter matched by PCI vendor and device ID (the first of 2 adapters with them), not by LUID: " +
                       why,
           "An adapter matched among several must say how many there were: " + text);
    text = AdapterIdMatchText({}, why);
    Expect(text == "DXGI has no adapter with the Vulkan device's PCI vendor and device ID, and its LUID is unknown: " +
                       why,
           "No adapter with a device's IDs must say so and why: " + text);
}

} // namespace

int main() {
    TestPivotSoftDeadzoneKeepsOneToOneInsideTheDeadzone();
    TestPivotSoftDeadzoneIntegratesSmoothstepSensitivity();
    TestPivotSoftDeadzoneIsMonotonicAndSmooth();
    TestPivotPositionAssistBasicCurve();
    TestPivotPositionAssistAdvancedDirectionsCompose();
    TestExpandedPivotSettingsResolve();
    TestPerDirectionDeadzoneTransitionParsing();
    TestInvertedPositionAssistRangeRejected();
    TestOsdConfig();
    TestTurboExperimentalConfig();
    TestUnknownKeysAreIgnoredWithWarnings();
    TestLegacySingularViewControlBindings();
    TestParseConfig();
    TestPivotActivationBindingModel();
    TestLogLevelCompatibility();
    TestResolveRuntimeConfig();
    TestDisabledProfileFallsBackToDefaults();
    TestPivotProfileResolution();
    TestPivotMultiProfileResolution();
    TestPivotResponseModeAndAdvancedAxes();
    TestPivotViewControlsParsingAndResolution();
    TestPivotViewTransitionMath();
    TestPivotSteppedGlideMath();
    TestTurboModuleResolution();
    TestTurboPacingModeParsing();
    TestTurboModuleOptional();
    TestFrameGenModuleOptional();
    TestFrameGenProfileResolution();
    TestFrameGenInvalidSettingsRejected();
    TestFrameGenToggleBindingList();
    TestFrameGenOsdRows();
    TestOsdPerformanceRow();
    TestFrameGenPlan();
    TestFrameGenHistoryState();
    TestFrameGenEngineImageTracker();
    TestFrameGenMotionCapturePlan();
    TestFrameGenViewMapping();
    TestFrameGenEngineCalibration();
    TestFrameGenEngineGates();
    TestFrameGenEngineCuts();
    TestFrameGenDeclaredChange();
    TestFrameGenEngineEvidencePlan();
    TestFrameGenEngineAssistPlan();
    TestFrameGenEngineLeadPlan();
    TestFrameGenEngineLeadControl();
    TestFrameGenEngineValidator();
    TestFrameGenEnginePrior();
    TestFrameGenEngineWorldParams();
    TestFrameGenWorldDeltaPlan();
    TestFrameGenWorldDeltaCalibration();
    TestFrameGenEngineMapPlan();
    TestFrameGenEngineVisibilityPlan();
    TestResearchSwitches();
    TestTurboMetricsSettingsParsing();
    TestTurboMetricsSessionRoundTrip();
    TestQuadViewsProfileResolution();
    TestEnabledProfileOverridesDisabledDefault();
    TestInvalidPivotActivationBindingRejected();
    TestNoneBindingParsed();
    TestDeviceBindingMetadataParsed();
    TestBindingSoundFeedbackParsed();
    TestBindingWithoutSoundDefaultsDisabled();
    TestToggleBindingLists();
    TestCoreSoundVolumeParsedAndClamped();
    TestExeMatch();
    TestSeenAppObservationRecording();
    TestRuntimePacingObservationRoundTrip();
    TestRuntimeRelayControlAndStatusRoundTrip();
    TestSessionGraphicsDriverVersions();
    TestSessionGraphicsVideoMemory();
    TestSessionGraphicsLine();
    TestSessionGraphicsAdapterIds();
    TestQuadViewStereoBoostKeepsInsetViewsInSync();
    TestStereoBoostScalesRotatedEyeBaseline();
    TestQuadViewConvergenceKeepsInsetOffsetsAligned();
    TestDepthSubmissionRestorePreservesApplicationDeltas();
    TestPivotYawAmplifiesBeyondDeadzone();
    TestPivotYawNoOpInsideDeadzone();
    TestSwapchainImageQueuePreservesFifo();
    TestLoggerCollapsesDuplicateMessages();
    TestTurboTraceContinuousAndDrained();
    TestTurboTraceFollowsDebugLevel();
    TestEyeGazeExtensionCompatibilityPolicy();
    TestQuadViewsSessionActivationPolicy();
    TestQuadViewsRecoveryStabilizationPolicy();
    TestQuadViewsRecoveryStabilizer();
    TestQuadViewsFrameCacheKeepsGazeWithLocatedFrame();
    TestQuadViewsCanvasDimensionsMatchCompositionDensity();
    TestFrameGenComposeEye();
    TestFrameGenComposeEyeMatchesCompositor();
    TestPivotLocateViewsRouting();
    TestPivotPoseDeltaSpaceReexpression();
    TestNumpadActivationKeys();
    TestDeviceInputPathsAndHatDirections();
    TestInputDeviceRetryBackoff();
    TestInputDeviceAbsentWaitsForRescan();
    TestInputDeviceArrivalDuringAttemptIsKept();
    TestInputConnectPriming();
#ifdef _WIN32
    TestD3D11SharpenShaderRegression();
#endif
    std::cout << "depthxr_layer_tests passed\n";
    return 0;
}
