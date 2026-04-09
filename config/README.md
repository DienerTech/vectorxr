# Config

The canonical Phase 2 schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- module-specific settings under `modules`
- DepthXR defaults and per-executable overrides
- PivotXR defaults including activation mode and activation key bindings

Phase 2 no longer treats the old DepthXR-only schema as the active source of truth.
