# DepthXR

DepthXR is a lightweight OpenXR API layer plus a small Tauri desktop app for experimenting with stronger stereo depth in VR titles.

Current scope:
- stereo boost
- convergence
- world scale
- FoV scale
- global settings
- per-game overrides

The repository is intentionally narrow. It does not include an in-headset UI, post-processing, shader injection, or any dependency on OpenXR-Toolkit.

## Repository layout

- `layer/`: C++20 OpenXR layer and shared core logic
- `app/`: Tauri 2 + Vue 3 configuration app
- `config/`: JSON schema and shared config notes
- `docs/`: architecture and bootstrap notes
- `examples/`: sample config files
- `scripts/`: Windows helper scripts for manifest registration

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

## CMake

The simplest Windows flow is the helper script:

```powershell
.\scripts\Build-Layer.ps1
```

It will:
- find CMake even when it is not on `PATH`
- detect the newest installed Visual Studio generator with C++ tools
- run a fresh configure to avoid stale cache issues after moving the repo between machines
- build the layer and tests
- run `ctest` by default

Optional flags:

```powershell
.\scripts\Build-Layer.ps1 -Configuration Debug
.\scripts\Build-Layer.ps1 -SkipTests
.\scripts\Build-Layer.ps1 -CleanScratch
```

To build and register the OpenXR layer in one go:

```powershell
.\scripts\Build-And-Install-Layer.ps1
```

To register or unregister an existing build directly:

```powershell
.\scripts\Install-Layer.ps1
.\scripts\Uninstall-Layer.ps1
```

These scripts default to the built manifest at `build/layer/Release/XR_APILAYER_NOVENDOR_depthxr.json` and accept `-BuildDir` and `-Configuration` overrides when needed.

If you prefer to invoke CMake directly, configure and build from the repository root:

```bash
cmake -S . -B build -DDEPTHXR_BUILD_LAYER=ON -DDEPTHXR_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

On non-Windows hosts, the shared core library and tests build, but the OpenXR DLL target is skipped.

## Config path

DepthXR resolves the shared JSON config in this order:

1. `DEPTHXR_CONFIG_PATH`
2. `%LOCALAPPDATA%\DepthXR\config\settings.json` on Windows
3. `./config/depthxr.settings.json` as a fallback for development and tests

The Tauri app uses the same resolution logic and will create a default config when missing.

## Current implementation status

- loader negotiation and manifest scaffold
- `xrCreateApiLayerInstance` and `xrGetInstanceProcAddr` interception
- config parsing and per-exe settings resolution
- `xrLocateViews` adjustments for stereo boost, convergence, world scale, and FoV scale
- placeholder effect modularization for future tuning

World scale is currently implemented as a first-pass positional scaling of located eye views in `xrLocateViews`. That is enough for experimentation, but a fuller implementation may need broader space interception later.
