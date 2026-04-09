# Architecture

## Goals

VectorXR Phase 2 is organized as one shared OpenXR runtime pipeline with product-level feature separation in the app and config.

Design rules:

- keep OpenXR interception separate from feature math
- keep config parsing independent from the desktop app
- keep suite settings separate from feature settings
- let features compose through shared runtime state instead of isolated runtime stacks
- keep runtime logging useful in debug scenarios but quiet by default

## Product Structure

The app presents:

- `VectorXR` core controls
- `DepthXR` feature editing
- `PivotXR` feature editing

This "module" concept is a product and config boundary, not a separate runtime ownership boundary.

## Runtime Structure

### Shared core

Shared core code under `layer/` currently owns:

- config model and parser
- settings resolution
- path resolution
- process name resolution
- logger abstraction
- stereo boost and convergence math

### OpenXR layer

The Windows DLL owns:

- loader negotiation entry point
- API layer instance creation
- function dispatch
- OpenXR function pointer capture
- `xrLocateViews` interception

### App

The Tauri app edits the same VectorXR config file used by the layer.

The app owns:

- suite navigation
- validation
- working-copy save flow
- feature-specific editing surfaces

## Current interception plan

Current interception surface:

- `xrNegotiateLoaderApiLayerInterface`
- `xrCreateApiLayerInstance`
- `xrGetInstanceProcAddr`
- `xrDestroyInstance`
- `xrBeginSession`
- `xrLocateViews`

This remains intentionally minimal while DepthXR stays focused on stereo boost and convergence.

## Planned direction

The next runtime step is to introduce a shared intermediate runtime context and ordered feature passes so DepthXR and PivotXR can compose cleanly inside one pipeline.
