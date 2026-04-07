# Config

The canonical schema lives in `depthxr.schema.json`.

The current file format supports:
- a global settings block
- per-executable overrides
- optional per-effect enable flags

The layer is tolerant of omitted optional profile fields and will inherit from the global block.
