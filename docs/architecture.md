# Architecture

## Goals

DepthXR is structured around a small shared core plus a thin Windows OpenXR API layer.

Design rules:
- keep OpenXR interception code separate from config and math
- keep config parsing independent from the desktop app
- make effect math replaceable without touching negotiation and dispatch
- keep runtime logging useful in debug scenarios but quiet by default

## Modules

### Shared core

Shared core code lives under `layer/` because the layer is the primary product today, but it is split so tests can run without the Windows DLL:

- config model and parser
- settings resolver
- path resolution
- process name resolution
- effect math
- logger abstraction

### OpenXR layer

The Windows DLL adds:

- loader negotiation entry point
- API layer instance creation
- function dispatch
- OpenXR function pointer capture
- `xrLocateViews` interception

### App

The Tauri app edits the same JSON file used by the layer. It does not talk to the layer directly.

## Interception plan

Current interception surface:

- `xrNegotiateLoaderApiLayerInterface`
- `xrCreateApiLayerInstance`
- `xrGetInstanceProcAddr`
- `xrDestroyInstance`
- `xrLocateViews`

This is deliberately minimal. If later experiments show that world scale needs broader space handling, the next likely additions are `xrLocateSpace` and selected reference-space calls.
