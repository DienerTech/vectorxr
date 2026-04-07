# DepthXR bootstrap brief

Build a Windows-only prototype called `DepthXR`.

## Product
DepthXR is a lightweight OpenXR API layer plus a small Tauri desktop app.
It provides:
- Stereo Boost
- World Scale
- FoV scale
- global settings
- per-game overrides

The goal is to reproduce the class of effect seen in OpenXR Toolkit-based stereo/depth mods, but as a fresh minimal OpenXR API layer with no dependency on OpenXR Toolkit.

## Tech stack
- Layer: C++20 + CMake + Windows DLL
- App: Tauri 2 + Vue 3 + TypeScript + Tailwind CSS
- Config: shared JSON file

## Architecture requirements
- Keep the layer narrowly scoped
- No in-headset overlay UI
- No post-processing
- No upscaling
- No shader injection
- No OpenXR Toolkit fork
- The layer should only intercept the minimal OpenXR calls needed for settings resolution and camera/view/projection adjustment

## Repo structure
Create this structure:

- /docs
- /layer
- /layer/include/depthxr
- /layer/src
- /layer/src/math
- /layer/src/util
- /layer/manifest
- /layer/tests
- /app
- /app/src
- /app/src/components
- /app/src/stores
- /app/src/lib
- /app/src-tauri/src
- /config
- /examples
- /scripts

## Layer responsibilities
Implement a minimal OpenXR API layer that:
- participates in layer negotiation
- intercepts instance creation
- resolves the current process executable name
- loads a JSON config file
- resolves active settings from global + per-game override
- logs active settings
- later exposes hooks for view/projection adjustments

## Important design constraints
- Keep code modular
- Separate interception from math
- Separate config loading from settings resolution
- Make it easy to replace math implementations later
- Log enough data for debugging, but keep release logs quiet
- Prefer boring, explicit code over clever abstractions

## Initial milestones

### Milestone 1
Create the layer skeleton:
- DLL entry
- OpenXR layer negotiation stubs
- manifest JSON
- basic logging
- intercept xrCreateInstance
- detect current process name
- no view/projection modification yet

### Milestone 2
Create config system:
- shared JSON schema
- global settings
- per-game profile matching by exe name
- settings resolver
- unit tests for config parse and profile matching

### Milestone 3
Create app:
- Tauri app with Vue UI
- edit global settings
- add/edit/remove profiles
- save JSON config
- show current config path
- basic validation

### Milestone 4
Prepare for rendering modification:
- create placeholder modules for stereo_boost, world_scale, and fov_scale
- create hook files for view and projection interception
- add feature flags so individual effects can be enabled independently

## Settings model
Use a JSON file shaped like this:

{
  "version": 1,
  "global": {
    "enabled": true,
    "stereoBoost": 1.10,
    "worldScale": 1.00,
    "fovScale": 1.00,
    "logLevel": "info"
  },
  "profiles": [
    {
      "match": { "exe": "DCS.exe" },
      "enabled": true,
      "stereoBoost": 1.12,
      "worldScale": 1.10,
      "fovScale": 0.95
    }
  ]
}

## Quality bar
- Buildable in Visual Studio and via CMake CLI
- Clear README
- Comments explaining OpenXR layer-specific code
- No fake implementations presented as complete
- If uncertain about exact OpenXR call points, leave TODOs and mark assumptions explicitly

## Deliverables
Generate:
- repo scaffold
- initial CMake files
- Tauri scaffold
- JSON schema
- README
- bootstrap docs
- placeholder source files with clear TODOs
