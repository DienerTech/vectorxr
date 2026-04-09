# Bootstrap Notes

## Current assumptions

- The layer targets Windows only.
- The app and layer share one VectorXR JSON config file.
- Per-game matching is currently by executable file name only.
- DepthXR currently applies stereo boost and convergence in `xrLocateViews`.
- PivotXR config exists in the suite model, but the runtime feature path is not active yet.

## Phase 2 progress

### Milestone 1

- VectorXR v2 config model
- shared app/layer config contract
- parser and settings resolution updates
- tests for v2 parsing and DepthXR profile fallback behavior

### Milestone 2

- VectorXR suite shell
- separate `VectorXR`, `DepthXR`, and `PivotXR` tabs
- working-copy save model
- sticky save bar
- friendlier DepthXR value presentation

### Milestone 3

- DepthXR scope cleanup to stereo boost and convergence only
- removal of World Scale and FoV from active config/build/runtime paths
- profile naming and disabled-profile fallback UX

## Next step

Introduce the shared runtime pass model described in the Phase 2 planning docs so future features can compose through one coordinated pipeline.
