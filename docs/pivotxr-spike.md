# PivotXR Spike Notes

## Current prototype

Milestone 5 introduces an experimental PivotXR runtime path inside `xrLocateViews`.

Current model:

- read the returned eye poses
- estimate headset center from the stereo eye positions
- extract current yaw from the returned view orientation
- keep motion inside the deadzone unchanged
- amplify yaw beyond the deadzone by `rotationMultiplier`
- rotate all eye poses around the headset center by the extra yaw
- gate the extra pivot factor behind a configurable desktop activation key
- preserve DepthXR composition by running PivotXR before stereo boost and convergence

## Why `xrLocateViews` first

This is the safest first spike because:

- the repository already has a stable `xrLocateViews` interception path
- PivotXR can be prototyped without expanding interception ownership yet
- coexistence with DepthXR and quad-view handling stays easier to reason about

## Current limits

This spike is intentionally honest about what it does not do yet:

- activation binding support is currently keyboard polling only
- smoothing is call-based rather than time-based
- the spike assumes view orientation is a workable approximation of head yaw for early testing
- some titles may still require a broader interception surface such as space-location hooks

## What this spike proves

- PivotXR can be represented as a feature in the shared runtime path
- PivotXR can mutate eye pose orientation and position without interfering with DepthXR math structure
- a deadzone-based yaw amplifier can be implemented with the current shared layer architecture

## What to validate next

- behavior in at least one real title
- interaction quality with quad views and DFR
- whether `xrLocateViews` alone is sufficient for the final feel
- whether activation should stay explicit, move to dynamic activation, or become a hybrid model
