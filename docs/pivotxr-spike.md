# PivotXR Spike Notes

## Current prototype

Milestone 5 now uses an experimental PivotXR runtime path built around `xrLocateSpace` plus `xrLocateViews` recomposition.

Current model:

- create internal `VIEW`/`LOCAL` reference spaces for the session
- track app-created `VIEW` reference spaces
- manipulate view-space poses in `xrLocateSpace`
- cache eye offsets in internal `VIEW` space
- recompose final eye poses in `xrLocateViews`
- gate the extra pivot factor behind a configurable desktop activation key
- preserve DepthXR composition by running PivotXR before stereo boost and convergence

## Why this changed

The original `xrLocateViews`-only spike proved the yaw math worked, but DCS still produced black edge masking even after the layer stopped rewriting eye translations directly in `xrLocateViews`.

Comparing against the original XrNeckSafer implementation showed that a viable neck/pivot effect likely needs a broader interception surface:

- `xrLocateSpace` for manipulating the head/view relationship
- `xrLocateViews` for recomposing eyes from cached offsets
- possibly `xrEndFrame` later to keep reprojection happy

## Current limits

This spike is intentionally honest about what it does not do yet:

- activation binding support is currently keyboard polling only
- smoothing is call-based rather than time-based
- the current pose manipulation only applies extra yaw, not the fuller translation model used by XRNeckSafer
- there is not yet an `xrEndFrame` reprojection-compensation path
- DCS still needs validation with this broader hook surface before PivotXR can be called production-ready

## What this spike proves

- PivotXR can be represented as a feature in the shared runtime path
- a deadzone-based yaw amplifier can be moved out of the `xrLocateViews`-only spike
- VectorXR can now follow the same broad architectural direction as XRNeckSafer while keeping DepthXR intact

## What to validate next

- behavior in DCS with the new locate-space path
- interaction quality with quad views and DFR
- whether `xrEndFrame` compensation is required to eliminate remaining edge masking
- whether activation should stay explicit, move to dynamic activation, or become a hybrid model
