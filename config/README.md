# Config

The canonical Phase 2 schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- module-specific settings under `modules`
- DepthXR defaults and per-executable overrides
- placeholder PivotXR defaults for forward compatibility

Phase 2 no longer treats the old DepthXR-only schema as the active source of truth.
