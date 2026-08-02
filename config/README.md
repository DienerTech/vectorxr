# Config

The canonical VectorXR schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- a suite-level application registry
- local seen-app tracking preference
- module-specific settings under `modules`
- Depth defaults and per-application overrides
- Pivot behavior profiles (`enhancedMotion` or `snapViews`), shared activation bindings, and reusable linked Nudge Sets

Config v3 moves executable matching into the shared application registry so profiles can target reusable app ids.
