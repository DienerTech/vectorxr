# Bootstrap Notes

## Assumptions

- The layer targets Windows only.
- The config app and layer share a single JSON config file.
- Per-game matching is currently by executable file name only.
- Stereo boost, world scale, and FoV scale are applied in `xrLocateViews`.

## Milestone coverage

### Milestone 1

- DLL scaffold
- loader negotiation
- manifest
- process name detection
- logging
- instance creation interception

### Milestone 2

- shared config schema
- parser
- global and per-profile settings merge
- tests for parsing and profile matching

### Milestone 3

- Tauri/Vue scaffold
- global editor
- profile CRUD
- config path display
- validation and save flow

### Milestone 4

- dedicated effect modules
- `xrLocateViews` hook
- feature toggles and neutral-value bypass behavior
