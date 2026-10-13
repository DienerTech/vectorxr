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
- OSD `showFramegenFps` (default on): Frame Generation's state and the headset's frame rate beside the FPS value, in both layouts
- Frame Generation's `frameMultiplier` defaults to `dynamic`; `auto`, `2x`, `3x`, and `4x` are the fixed rates

Config v3 moves executable matching into the shared application registry so profiles can target reusable app ids.
