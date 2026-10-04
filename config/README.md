# Config

The canonical VectorXR schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- a suite-level application registry
- local seen-app tracking preference
- module-specific settings under `modules`
- Depth defaults and per-application overrides
- Pivot behavior profiles (`enhancedMotion` or `snapViews`), shared activation bindings, and reusable linked Nudge Sets
- experimental Frame Generation under the optional `modules.framegen` block: defaults, per-application profiles, an A/B `toggleBinding`, and a module-level `diagnosticView`; configs without the block use the defaults (off)
- OSD `showFramegen` / `compactShowFramegen` and the `framegen` expanded row; saved `bodyOrder` lists without it get the row appended at the end

Config v3 moves executable matching into the shared application registry so profiles can target reusable app ids.
