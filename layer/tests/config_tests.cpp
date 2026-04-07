#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#include "depthxr/config_parser.h"
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
    "worldScaleEnabled": true,
    "fovScaleEnabled": true,
    "stereoBoost": 1.1,
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
    "worldScaleEnabled": true,
    "fovScaleEnabled": true,
    "stereoBoost": 1.05,
    "worldScale": 1.0,
    "fovScale": 1.0,
    "logLevel": "info"
  },
  "profiles": [
    {
      "match": { "exe": "DCS.exe" },
      "enabled": true,
      "stereoBoost": 1.15,
      "worldScale": 1.1
    }
  ]
}
)json";

    const depthxr::ParseResult result = depthxr::ParseConfig(json);
    Expect(result.ok, "Config parser rejected valid resolve test config: " + result.error);

    const depthxr::ResolvedSettings resolved = depthxr::ResolveSettings(result.document, "dcs.exe");
    Expect(std::abs(resolved.stereo_boost - 1.15) < 0.0001, "Profile stereoBoost override was not applied");
    Expect(std::abs(resolved.world_scale - 1.1) < 0.0001, "Profile worldScale override was not applied");
    Expect(std::abs(resolved.fov_scale - 1.0) < 0.0001, "Global fovScale should have been inherited");
}

void TestExeMatch() {
    Expect(depthxr::ExeNameMatches("DCS.exe", "dcs.exe"), "Case-insensitive exe match failed");
    Expect(depthxr::ExeNameMatches("C:\\Games\\DCS.exe", "dcs.exe"), "Basename exe match failed");
}

} // namespace

int main() {
    TestParseConfig();
    TestResolveSettings();
    TestExeMatch();
    std::cout << "depthxr_layer_tests passed\n";
    return 0;
}
