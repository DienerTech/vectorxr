# VectorXR

VectorXR is a modular OpenXR utility suite in progress.

Current active feature set:

- Depth stereo boost
- Depth convergence
- suite-level logging and config management
- suite-level application registry
- per-application Depth overrides
- Pivot runtime yaw amplification
- Pivot runtime pitch amplification

The repository currently ships one shared OpenXR layer plus one Tauri desktop app:

- the layer owns interception, config loading, and runtime effect application
- the app owns suite navigation, feature settings, validation, and save flow

## Repository layout

- `layer/`: C++20 OpenXR layer and shared runtime logic
- `app/`: Tauri 2 + Vue 3 desktop app
- `config/`: shared config schema and notes
- `docs/`: architecture and bootstrap notes
- `examples/`: sample config files
- `scripts/`: Windows helper scripts for manifest registration

## Current config model

VectorXR uses the v3 suite config schema in `config/vectorxr.schema.json`.

High-level shape:

```json
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7,
    "trackSeenApps": true
  },
  "applications": [
    {
      "id": "dcs",
      "name": "DCS",
      "enabled": true,
      "match": {
        "exe": "DCS.exe"
      }
    }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.0
      },
      "bindings": {
        "toggleEnabled": {
          "type": "keyboard",
          "chord": ["F7"]
        }
      },
      "profiles": [
        {
          "name": "DCS",
          "enabled": true,
          "applicationIds": ["dcs"],
          "settings": {
            "stereoBoostEnabled": true,
            "convergenceEnabled": true,
            "stereoBoost": 1.1,
            "convergence": 0.0
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "activationMode": "toggle",
        "activationBinding": {
          "type": "none"
        },
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
  }
}
```

## Config path

VectorXR resolves the shared JSON config in this order:

1. `VECTORXR_CONFIG_PATH`
2. `%LOCALAPPDATA%\\VectorXR\\config\\settings.json` on Windows
3. `./config/vectorxr.settings.json` as a fallback for development

The app and layer use the same path strategy.

## Build prerequisites

### Layer

The layer is Windows-only and expects:

- Visual Studio with Desktop C++ tools
- CMake 3.28+
- Either an OpenXR SDK that provides `OpenXRConfig.cmake`, or a vendored `OpenXR.Loader.*.nupkg` package in the repository root

### App

The app expects:

- Node.js 20+
- Rust toolchain with `cargo`
- Tauri 2 prerequisites for the target machine

## Build commands

Layer helper script:

```powershell
.\scripts\Build-Layer.ps1
```

Direct CMake flow:

```bash
cmake -S . -B build -DDEPTHXR_BUILD_LAYER=ON -DDEPTHXR_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Frontend app:

```powershell
cd app
npm ci
npm run build
```

Production Windows installer:

```powershell
cd app
npm run installer:build
```

This builds the OpenXR layer, stages the layer DLL and manifest for Tauri, and produces an elevated per-machine NSIS installer under `app\src-tauri\target\release\bundle\nsis`.

Tauri backend:

```powershell
cd app\src-tauri
cargo check
```

## Runtime status

Implemented today:

- loader negotiation and manifest scaffold
- config parsing for the v3 VectorXR schema
- per-application Depth settings resolution
- `xrLocateViews` adjustments for stereo boost and convergence
- quad-view-aware stereo/convergence behavior
- Pivot runtime path across `xrLocateSpace` and `xrLocateViews`
- VectorXR suite shell with separate Home, Depth, and Pivot tabs

Deliberately not active yet:

- true loader detachment when disabled
- World Scale and FoV controls from the old prototype scope
