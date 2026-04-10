# VectorXR project outline

This repository started as a narrower DepthXR prototype and is now in Phase 2 as VectorXR.

Current repository direction:

- `VectorXR` is the suite host
- `DepthXR` is the active runtime feature domain
- `PivotXR` is represented in config and UI, with runtime work still ahead

Key architecture decisions:

- one shared OpenXR layer owns interception and runtime coordination
- one shared config file owns suite and feature settings
- features are separated in product/UI/config terms, not as isolated runtime stacks

Current active DepthXR scope:

- stereo boost
- convergence
- per-game overrides
- suite-level logging and enablement

Out of active Phase 2 scope:

- World Scale
- FoV controls
- true loader detachment when disabled
- PivotXR runtime pass until the dedicated spike

Planning details live in:

- `phase2-plan.md`
- `phase2-roadmap.md`
- `phase2-refactor-blueprint.md`
