# VectorXR

VectorXR is a modular OpenXR utility suite in progress.

Current active feature set:

- DepthXR stereo boost
- DepthXR convergence
- suite-level logging and config management
- per-game DepthXR overrides
- PivotXR config shell for upcoming runtime work

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

Phase 2 uses the VectorXR suite config schema in `config/vectorxr.schema.json`.

High-level shape:

```json
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
        "stereoBoost": 1.1,
        "convergence": 0.0
      },
      "profiles": []
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

Tauri backend:

```powershell
cd app\src-tauri
cargo check
```

## Runtime status

Implemented today:

- loader negotiation and manifest scaffold
- config parsing for the v2 VectorXR schema
- per-executable DepthXR settings resolution
- `xrLocateViews` adjustments for stereo boost and convergence
- quad-view-aware stereo/convergence behavior
- VectorXR suite shell with separate core, DepthXR, and PivotXR tabs

Deliberately not active yet:

- true loader detachment when disabled
- PivotXR runtime pass
- World Scale and FoV controls from the old prototype scope
