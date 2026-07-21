# Architecture

## Goals

VectorXR is organized as one shared OpenXR runtime pipeline with product-level feature separation in the app and config.

Design rules:

- keep OpenXR interception separate from feature math
- keep config parsing independent from the desktop app
- keep suite settings separate from feature settings
- let features compose through shared runtime state instead of isolated runtime stacks
- keep runtime logging useful in debug scenarios but quiet by default

## Product Structure

The app presents product modules:

- core suite controls (`core`)
- `Depth` feature editing (internal id `depthxr`)
- `Pivot` feature editing (internal id `pivotxr`)
- `Quadviews` feature editing

This "module" concept is a product and config boundary, not a separate runtime ownership boundary. User-facing labels are display-first (`Depth`, `Pivot`); internal ids, config keys, and C++ namespaces stay `depthxr`/`pivotxr`.

## Runtime Structure

### Shared core

Shared core code under `layer/` owns:

- config model and parser
- settings resolution and per-application profile matching
- path resolution
- process name resolution
- logger abstraction
- feature math (Depth stereo boost/convergence/submission restore, Pivot yaw/pitch recomposition, Quadviews composition)

### OpenXR layer

The Windows DLL owns:

- loader negotiation entry point
- API layer instance creation
- function dispatch
- OpenXR function pointer capture
- interception of the view/pose, session, swapchain, and view-configuration calls the features need

### App

The Tauri app edits the same VectorXR config file used by the layer.

The app owns:

- suite navigation
- validation
- working-copy save flow
- feature-specific editing surfaces

## Interception surface

Core entry points:

- `xrNegotiateLoaderApiLayerInterface`
- `xrCreateApiLayerInstance`
- `xrGetInstanceProcAddr`
- `xrDestroyInstance`

Feature interception (varies by which modules are active):

- `xrBeginSession`, `xrCreateSession`, `xrDestroySession`
- `xrLocateViews` — Depth stereo boost and convergence
- `xrEndFrame` — Pivot correction, optional Depth Anchor geometry restore, and Quadviews submission composition
- `xrLocateSpace` — Pivot yaw/pitch recomposition
- `xrEnumerateViewConfigurations` / `xrEnumerateViewConfigurationViews` and the swapchain
  lifecycle calls — Quadviews composition

The interception set is intentionally scoped to what the enabled features need rather than
hooking every call.
