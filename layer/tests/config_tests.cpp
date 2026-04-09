#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "depthxr/config_parser.h"
#include "depthxr/effects.h"
#include "depthxr/settings_resolver.h"

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
  "version": 2,
  "core": {
    "enabled": true,
    "logLevel": "debug",
    "logRetentionFiles": 9
  },
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.08
      },
      "profiles": [
        {
          "name": "Game",
          "match": { "exe": "Game.exe" },
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
        "activationMode": "toggle",
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0
      }
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid v2 config: " + result.error);
    Expect(result.document.version == 2, "Version was not normalized to 2");
    Expect(result.document.core.log_level == depthxr::LogLevel::Debug, "Core log level mismatch");
    Expect(result.document.core.log_retention_files == 9, "Core log retention mismatch");
    Expect(std::abs(result.document.depthxr.defaults.stereo_boost - 1.1) < 0.0001, "DepthXR stereoBoost mismatch");
    Expect(std::abs(result.document.depthxr.defaults.convergence - 0.08) < 0.0001, "DepthXR convergence mismatch");
    Expect(result.document.depthxr.profiles.size() == 1, "DepthXR profile count mismatch");
    Expect(result.document.depthxr.profiles[0].match.exe_name == "Game.exe", "DepthXR profile exe mismatch");
}

void TestResolveRuntimeConfig() {
    const std::string json = R"json(
{
  "version": 2,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.05,
        "convergence": 0.0
      },
      "profiles": [
        {
          "name": "DCS",
          "match": { "exe": "DCS.exe" },
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
        "activationMode": "hold",
        "rotationMultiplier": 1.7,
        "smoothing": 0.25,
        "deadzoneDegrees": 9.0
      }
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
    Expect(resolved.pivotxr.enabled, "PivotXR module enable was not resolved");
    Expect(resolved.pivotxr.activation_mode == depthxr::ActivationMode::Hold, "PivotXR activation mode mismatch");
}

void TestDisabledProfileFallsBackToDefaults() {
    const std::string json = R"json(
{
  "version": 2,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.05,
        "convergence": 0.01
      },
      "profiles": [
        {
          "name": "DCS",
          "match": { "exe": "DCS.exe" },
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
        "activationMode": "toggle",
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0
      }
    }
  }
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected disabled-profile config: " + result.error);

    const depthxr::ResolvedRuntimeConfig resolved = depthxr::ResolveRuntimeConfig(result.document, "DCS.exe");
    Expect(std::abs(resolved.depthxr.stereo_boost - 1.05) < 0.0001, "Disabled profile should fall back to defaults");
    Expect(std::abs(resolved.depthxr.convergence - 0.01) < 0.0001, "Disabled profile convergence should fall back to defaults");
}

void TestExeMatch() {
    Expect(depthxr::ExeNameMatches("DCS.exe", "dcs.exe"), "Case-insensitive exe match failed");
    Expect(depthxr::ExeNameMatches("C:\\Games\\DCS.exe", "dcs.exe"), "Basename exe match failed");
}

void TestQuadViewStereoBoostKeepsInsetViewsInSync() {
    depthxr::ViewAdjustmentData views[4] = {
        {{-0.03, 0.0, 0.0}, {-1.0, 0.8, 0.9, -0.9}},
        {{0.03, 0.0, 0.0}, {-0.8, 1.0, 0.9, -0.9}},
        {{-0.03, 0.0, 0.0}, {-0.4, 0.2, 0.3, -0.3}},
        {{0.03, 0.0, 0.0}, {-0.2, 0.4, 0.3, -0.3}},
    };

    depthxr::ApplyStereoBoost(views, 1.4, depthxr::ViewLayout::kStereoWithFoveatedInset);

    Expect(std::abs(views[0].position.x - views[2].position.x) < 0.0001,
           "Left outer/inset positions diverged under quad-view stereo boost");
    Expect(std::abs(views[1].position.x - views[3].position.x) < 0.0001,
           "Right outer/inset positions diverged under quad-view stereo boost");
    Expect(std::abs(views[0].position.x + 0.042) < 0.0001, "Left eye stereo boost value mismatch");
    Expect(std::abs(views[1].position.x - 0.042) < 0.0001, "Right eye stereo boost value mismatch");
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

} // namespace

int main() {
    TestParseConfig();
    TestResolveRuntimeConfig();
    TestDisabledProfileFallsBackToDefaults();
    TestExeMatch();
    TestQuadViewStereoBoostKeepsInsetViewsInSync();
    TestQuadViewConvergenceKeepsInsetOffsetsAligned();
    std::cout << "depthxr_layer_tests passed\n";
    return 0;
}
