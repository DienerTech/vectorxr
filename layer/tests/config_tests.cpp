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
  "version": 1,
  "global": {
    "enabled": true,
    "stereoBoostEnabled": true,
    "convergenceEnabled": true,
    "worldScaleEnabled": true,
    "fovScaleEnabled": true,
    "stereoBoost": 1.1,
    "convergence": 0.08,
    "worldScale": 1.0,
    "fovScale": 0.95,
    "logLevel": "debug"
  },
  "profiles": [
    {
      "match": { "exe": "Game.exe" },
      "stereoBoost": 1.2,
      "fovScale": 0.9
    }
  ]
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid config: " + result.error);
    Expect(result.document.version == 1, "Version was not parsed");
    Expect(std::abs(result.document.global.stereo_boost - 1.1) < 0.0001, "Global stereoBoost mismatch");
    Expect(std::abs(result.document.global.convergence - 0.08) < 0.0001, "Global convergence mismatch");
    Expect(result.document.profiles.size() == 1, "Profile count mismatch");
    Expect(result.document.profiles[0].match.exe_name == "Game.exe", "Profile exe match mismatch");
}

void TestResolveSettings() {
    const std::string json = R"json(
{
  "version": 1,
  "global": {
    "enabled": true,
    "stereoBoostEnabled": true,
    "convergenceEnabled": true,
    "worldScaleEnabled": true,
    "fovScaleEnabled": true,
    "stereoBoost": 1.05,
    "convergence": 0.0,
    "worldScale": 1.0,
    "fovScale": 1.0,
    "logLevel": "info"
  },
  "profiles": [
    {
      "match": { "exe": "DCS.exe" },
      "enabled": true,
      "stereoBoost": 1.15,
      "convergence": 0.12,
      "worldScale": 1.1
    }
  ]
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid resolve test config: " + result.error);

    const depthxr::ResolvedSettings resolved = depthxr::ResolveSettings(result.document, "dcs.exe");
    Expect(std::abs(resolved.stereo_boost - 1.15) < 0.0001, "Profile stereoBoost override was not applied");
    Expect(std::abs(resolved.convergence - 0.12) < 0.0001, "Profile convergence override was not applied");
    Expect(std::abs(resolved.world_scale - 1.1) < 0.0001, "Profile worldScale override was not applied");
    Expect(std::abs(resolved.fov_scale - 1.0) < 0.0001, "Global fovScale should have been inherited");
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
    TestResolveSettings();
    TestExeMatch();
    TestQuadViewStereoBoostKeepsInsetViewsInSync();
    TestQuadViewConvergenceKeepsInsetOffsetsAligned();
    std::cout << "depthxr_layer_tests passed\n";
    return 0;
}
