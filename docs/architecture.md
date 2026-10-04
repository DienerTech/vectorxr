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
- `Turbo` frame pacing, runtime strategy selection, diagnostics, and Safety controls
- `Frame Generation` optical-flow frame synthesis (internal id `framegen`, experimental)

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
- `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` — Turbo pacing, or Frame Generation pacing in
  sessions that start with Frame Generation on; swapchain creation and release also tell
  Frame Generation which image each projection view submitted

## Frame Generation

Frame Generation is latched per session in `xrCreateSession`. When it owns pacing,
`FrameGenPresenter` (`layer/src/framegen/framegen_presenter.cpp`) takes the application's
frame calls:

- The app's `xrWaitFrame` waits on runtime slot *k* but reports slot *k + 1* with a doubled
  period, so the app renders for the time its frame is shown and runs at half the refresh rate.
- The app's `xrEndFrame` submits slot *k* with a generated frame, then makes one extra runtime
  wait/begin/end for slot *k + 1* with the app's own layers. Both submissions go through the
  layer's normal final submission, so they carry the OSD layer.
- A wait that arrives while an engaged frame is still ending is held, so runtime frame calls
  never overlap even for applications that wait on one thread while ending on another.
- Turbo yields for the session; the two features pace frames in opposite directions.

Generation runs on the session's own device and queue. An API-neutral plan
(`layer/src/framegen_plan.cpp`) describes every pass, and the Direct3D 11, Direct3D 12 and
Vulkan backends (`layer/src/framegen/`) only record copies, barriers and compute dispatches
for it. The passes live in one HLSL file (`layer/shaders/framegen.hlsl`): fxc builds DXBC for
both Direct3D backends and a pinned DXC release builds SPIR-V for Vulkan at build time.

Per view, the newest submitted image is copied into a history texture; luma pyramids are built
for it and for the previous frame rotated into the newest view (head rotation is the runtime's
job, so it is removed before matching); block matching runs coarse to fine; and synthesis either
interpolates between the two real frames or extrapolates past the newest, writing into a
layer-owned output swapchain. Generation works on whatever projection views reach the runtime,
so it follows Quadviews composition and Varjo-native quad views alike.

The interception set is intentionally scoped to what the enabled features need rather than
hooking every call.
