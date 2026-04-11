# Config

The canonical VectorXR schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- a suite-level application registry
- module-specific settings under `modules`
- Depth defaults and per-application overrides
- Pivot defaults including activation mode and shared activation bindings

Config v3 moves executable matching into the shared application registry so profiles can target reusable app ids.
