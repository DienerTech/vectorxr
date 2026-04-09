# VectorXR Phase 2 Engineering Roadmap

## 1. Objectives

This roadmap turns the high-level Phase 2 plan into an implementation sequence for the current repository.

Companion implementation detail lives in `phase2-refactor-blueprint.md`.

Primary goals:

- Reframe the product from a single-purpose DepthXR app into a modular VectorXR utility suite
- Preserve the proven DepthXR runtime behavior while restructuring config and UI around modules
- Establish a stable foundation for adding PivotXR and future modules
- Keep migration risk low for existing users and existing DepthXR configs

Non-goals for the first Phase 2 implementation:

- Full OpenXR loader detachment when disabled
- Smart or context-aware PivotXR activation in the first release
- Reintroducing World Scale or FoV controls during the platform migration

---

## 2. Current State

The repository is already in a good position for Phase 2 because the project has three useful seams:

- The OpenXR layer runtime is separated from the desktop app
- Settings resolution is already isolated from OpenXR interception
- Depth-related math is modular enough to keep evolving without rebuilding the whole layer

Current constraints:

- The app is still structured as if DepthXR is the whole product
- The config schema is still a single-module schema
- Logging is currently modeled as part of module/profile settings instead of core settings
- Runtime enablement disables behavior, but does not truly hide the layer from the OpenXR pipeline

---

## 3. Target Product Model

VectorXR becomes the host product. DepthXR and PivotXR become modules within a shared suite.

### 3.1 Product Layers

#### VectorXR Core

Responsibilities:

- App shell and navigation
- Shared config file management
- Global plugin settings
- Global logging policy
- Module registry and module status
- Shared save/reload/error state

#### DepthXR Module

Responsibilities:

- Stereo depth controls
- Convergence controls
- Per-game overrides for DepthXR only
- DepthXR-specific runtime math

Removed from active Phase 2 scope:

- World Scale
- FoV controls

#### PivotXR Module

Responsibilities:

- Rotation amplification behavior
- Module-specific activation and tuning settings
- Runtime transform logic that is compatible with quad views and DFR

### 3.2 Recommended Architecture Boundary

For Phase 2, "module" should describe product structure, config ownership, and UI organization, but not separate runtime stacks.

Recommended interpretation:

- VectorXR remains one OpenXR layer
- DepthXR and PivotXR are feature domains within that layer
- The app exposes them as separate tabs and separate settings areas
- The runtime applies them through one shared execution pipeline

This avoids the main long-term risk: splitting the codebase into independently acting systems that each want to own view or pose modification, compatibility logic, logging, and OpenXR interception state.

What should stay shared:

- OpenXR interception and dispatch ownership
- Runtime frame context
- QuadViews and DFR compatibility handling
- Config loading and process/profile resolution
- Diagnostics and logging infrastructure
- Ordering rules for multiple active features

What can stay feature-specific:

- Feature settings models
- Feature validation
- Feature UI
- Feature math and transform logic
- Feature-specific tests

### 3.3 Architecture Recommendation

Use a layered architecture with feature passes, not isolated runtime modules.

Recommended layers:

1. Core layer services
2. Shared runtime model
3. Feature passes
4. Product-facing module UI and config

#### Core layer services

Own:

- OpenXR function interception
- Loader lifecycle
- Config reload
- Process detection
- Log management
- Shared capability detection

#### Shared runtime model

Introduce a shared representation for the data that features may inspect or modify.

Examples:

- View layout classification
- Per-eye pose and FoV state
- Per-frame runtime flags
- Session compatibility information
- Active feature set and execution order

This shared model becomes the contract between the OpenXR layer and the features.

#### Feature passes

Each feature should plug into the shared runtime model as an ordered pass.

For example:

- DepthXR pass adjusts view position and projection-related data
- PivotXR pass adjusts orientation or pose-related data

The important design rule is that features do not independently hook OpenXR and do not privately implement their own compatibility model for stereo, quad views, or DFR.

#### Product-facing module UI and config

Keep the user-facing "module" abstraction because it is useful for:

- Discoverability
- Enablement
- Settings ownership
- Future expansion

But keep it decoupled from the runtime execution model so we can evolve how features compose without rewriting the app structure.

---

## 4. Target Config Architecture

The most important Phase 2 change is moving from a flat DepthXR config to a module-aware VectorXR config.

### 4.1 Config Design Principles

- One shared config file for the whole suite
- Core settings separated from module settings
- Module defaults separated from per-profile overrides
- Canonical runtime values stored in config
- UI may display friendlier scales, but runtime math uses normalized values
- Backward migration from existing DepthXR config must be supported

### 4.2 Proposed Config Shape

```json
{
  "version": 2,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.0
      },
      "profiles": [
        {
          "name": "DCS",
          "match": {
            "exe": "DCS.exe"
          },
          "enabled": true,
          "settings": {
            "stereoBoostEnabled": true,
            "convergenceEnabled": true,
            "stereoBoost": 1.15,
            "convergence": 0.0012
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "activationMode": "toggle",
        "rotationMultiplier": 1.5,
        "smoothing": 0.2,
        "deadzoneDegrees": 8.0
      }
    }
  }
}
```

### 4.3 Migration Rules

Config migration from v1 to v2 should follow these rules:

- `global.enabled` becomes `modules.depthxr.enabled`
- `global.logLevel` becomes `core.logLevel`
- `global.stereoBoost*` and `global.convergence*` become `modules.depthxr.defaults.*`
- `worldScale*` and `fovScale*` are dropped during migration
- Per-profile `logLevel` is dropped during migration
- Existing profile `enabled`, `stereoBoost*`, and `convergence*` values move into `modules.depthxr.profiles[*]`
- New profile `name` defaults to a sanitized executable-based label
- `core.enabled` defaults to `true` for the initial migration release

### 4.4 Compatibility Strategy

The app and layer should support this transition in two steps:

1. Read old v1 config and migrate in memory
2. Save only the new v2 format once the user writes config from the app

This approach minimizes breakage and avoids forcing users to hand-edit configs.

---

## 5. Target Runtime Architecture

Phase 2 should keep the current runtime stable while making it module-aware.

### 5.1 Runtime Resolution Model

Introduce explicit resolution layers:

- Core runtime settings
- Module enabled state
- Module defaults
- Module profile override
- Final resolved module settings for the current executable

DepthXR resolution rule:

- If `core.enabled == false`, all module effects are bypassed
- If `modules.depthxr.enabled == false`, DepthXR is bypassed
- If no matching profile exists, use module defaults
- If a matching profile exists but `profile.enabled == false`, use module defaults
- If a matching enabled profile exists, apply its override values onto module defaults

### 5.2 Layer Boundaries

Keep OpenXR interception thin. The layer should:

- Load config
- Resolve active module settings
- Apply module logic in a predictable order
- Log resolved state and important compatibility diagnostics

Recommended processing order inside the layer:

1. Resolve core settings
2. Resolve DepthXR settings
3. Resolve PivotXR settings
4. Build a shared frame/view context
5. Apply ordered feature passes against that shared context
6. Commit the final result back to OpenXR structures

### 5.2.1 Runtime Composition Rule

Do not let each feature mutate raw OpenXR structures independently.

Instead:

- Copy runtime state into a shared intermediate context
- Run feature passes in a known order
- Validate or clamp if needed
- Write final values back once

This is the safest way to support combined DepthXR and PivotXR behavior because it gives us one place to reason about interaction effects, ordering, and compatibility with quad-view layouts.

### 5.2.2 Recommended Initial Pass Model

The first Phase 2 refactor does not need a fully generic plugin system. A simple internal pass model is enough.

Suggested pass categories:

- Pose-related passes
- View-position passes
- Projection/FoV passes
- Diagnostics pass

For the current plan, that likely means:

- PivotXR operates in the pose-related stage
- DepthXR stereo boost operates in the view-position stage
- DepthXR convergence operates in the projection stage

This keeps responsibilities crisp and reduces the chance that one feature accidentally undoes another.

### 5.3 Runtime Note on Plugin Enable

The proposed `core.enabled` checkbox should be implemented in Phase 2 as:

- A hard runtime bypass for all module behavior
- Clear UI text that this disables effects, not loader registration

True attach/detach from the OpenXR pipeline should be tracked as a separate follow-on milestone because it involves loader registration and lifecycle behavior outside normal in-app settings.

---

## 6. App Architecture Plan

The app should be reorganized around an app shell and module editors.

### 6.1 Frontend Structure

Recommended structure:

- `AppShell`
- `TopNavTabs`
- `SaveBar`
- `CoreOverviewTab`
- `DepthXRTab`
- `PivotXRTab`
- Shared form components for toggles, sliders, panels, and profile rows

### 6.2 Store Responsibilities

Refactor the current config store into a suite-aware store:

- Load and save the full VectorXR config envelope
- Track dirty state
- Track validation state
- Track load/save status
- Expose per-tab view models
- Provide migration and normalization on load

### 6.3 Core Tab Requirements

The `VectorXR` tab should include:

- Core plugin enabled toggle
- Global log level
- Log retention setting
- View logs action
- Shared config path
- Module status overview cards
- About section

### 6.4 DepthXR Tab Requirements

The `DepthXR` tab should include:

- Module enabled toggle
- Default settings panel
- Stereo Boost and Convergence only
- UX-scaled values for display
- Profile list with rename support
- Enabled checkbox per profile
- Disabled-but-editable visual treatment

### 6.5 PivotXR Tab Requirements

The first PivotXR UI should include:

- Module enabled toggle
- Activation mode
- Rotation multiplier
- Smoothing
- Deadzone threshold
- Short compatibility notes

### 6.6 Save UX

Implement a persistent save surface after the app shell refactor:

- Sticky footer save bar
- Unsaved changes indicator
- Save button
- Reload or discard action
- Validation summary

---

## 7. Runtime Workstreams

### 7.1 Config and Schema Workstream

Tasks:

- Create a v2 VectorXR schema under `config/`
- Replace the app TS model with a suite-aware model
- Replace the layer C++ settings model with core and per-module structures
- Add migration helpers for v1 to v2
- Update example configs and README references

Deliverable:

- The app and layer both load the same v2 config format

### 7.2 DepthXR Refactor Workstream

Tasks:

- Extract DepthXR settings into module-specific structures
- Remove World Scale and FoV settings from config, UI, and resolution logic
- Keep stereo boost and convergence behavior unchanged
- Preserve quad-view compatibility coverage
- Update logs to reflect module-aware resolved settings

Deliverable:

- Existing DepthXR behavior continues to work under the new VectorXR model

### 7.3 App Shell Refactor Workstream

Tasks:

- Replace the single-page DepthXR app layout with a tabbed VectorXR shell
- Introduce shared cards, section panels, and consistent checkbox-driven controls
- Add dirty-state tracking and sticky save bar
- Move validation into module-aware sections

Deliverable:

- The app feels like a suite host rather than a one-off module editor

### 7.4 Logging and Diagnostics Workstream

Tasks:

- Move logging policy to core settings
- Add log retention cleanup
- Add a log viewing command and simple read-only viewer
- Keep error visibility strong even when general logging is reduced

Deliverable:

- Logging is centralized, predictable, and easier to inspect

### 7.5 PivotXR Spike Workstream

Tasks:

- Identify the correct OpenXR interception points for controlled head rotation amplification
- Validate compatibility assumptions with quad views and DFR
- Prototype the simplest functional activation model first
- Keep UI and config minimal until runtime behavior is proven

Deliverable:

- A technical proof of PivotXR viability before deep UX work

---

## 8. Milestone Plan

### Milestone 1: Config Foundation

Scope:

- Introduce v2 config schema
- Introduce migration from current DepthXR config
- Update app and layer models to understand v2

Acceptance criteria:

- Existing v1 configs load successfully
- New saves write v2 config only
- Tests cover migration and resolution rules

### Milestone 2: VectorXR Shell

Scope:

- Build tabbed app shell
- Add core tab
- Move current DepthXR UI into a module tab
- Add sticky save bar and dirty-state tracking

Acceptance criteria:

- App launches into `VectorXR` tab
- `DepthXR` tab edits module settings correctly
- Validation and save behavior work across tabs

### Milestone 3: DepthXR Phase 2 Cleanup

Scope:

- Remove World Scale and FoV controls from runtime and UI
- Remove per-profile logging
- Add profile names
- Apply new friendly display scales for Stereo Boost and Convergence

Acceptance criteria:

- DepthXR controls are limited to supported Phase 2 scope
- Profile enable and fallback behavior match the plan
- Existing quad-view behavior remains stable

### Milestone 4: Core Logging UX

Scope:

- Add log retention
- Add log viewing flow
- Surface log path and logging state in the core tab

Acceptance criteria:

- Log files are pruned to configured retention
- User can inspect recent logs from the app

### Milestone 5: PivotXR Technical Spike

Scope:

- Prototype runtime interception and math path
- Validate that PivotXR can coexist with DepthXR
- Lock down the first shippable settings set

Acceptance criteria:

- Prototype works in at least one target title
- Interception approach is documented
- Known technical limits are captured before full UI work

### Milestone 6: PivotXR MVP

Scope:

- Add PivotXR config and UI
- Add basic activation model and tuning controls
- Add module status to the core overview

Acceptance criteria:

- PivotXR can be enabled and configured from the app
- Runtime behavior is stable enough for iterative tuning

---

## 9. Testing Strategy

### 9.1 Layer Tests

Add or extend tests for:

- Config migration
- Module resolution order
- Profile disabled fallback behavior
- Removal of World Scale and FoV from active resolution
- DepthXR quad-view behavior regression coverage
- Future PivotXR transform math where feasible

### 9.2 App Validation

Add or extend validation for:

- v2 config shape
- Module-specific ranges
- Required core fields
- Profile naming and executable matching rules

### 9.3 Manual Test Matrix

Maintain a lightweight manual matrix for:

- Standard stereo titles
- QuadViews/DFR titles
- No-profile fallback behavior
- Disabled profile fallback behavior
- Runtime bypass with `core.enabled = false`
- Combined DepthXR and PivotXR scenarios once PivotXR exists

---

## 10. Risks and Design Notes

### 10.1 Highest-Risk Areas

- Config migration bugs that silently change runtime behavior
- PivotXR interception point selection
- Regressions in quad-view compatibility
- Confusion between "plugin disabled" and "layer detached"

### 10.2 Design Guardrails

- Do not mix core and module settings again
- Do not split OpenXR interception ownership across features
- Do not duplicate quad-view or DFR compatibility logic per feature
- Do not let features write directly to raw OpenXR structs in arbitrary order
- Do not store UI-only scale values in config
- Do not ship PivotXR UX polish before runtime viability is proven
- Do not claim true plugin detachment until registration/lifecycle behavior is actually implemented

---

## 11. Recommended Implementation Order

Build in this sequence:

1. Add the new config model and migration path
2. Update layer settings resolution to be module-aware
3. Refactor the app shell into VectorXR with tabs
4. Move DepthXR into module form and remove out-of-scope controls
5. Add sticky save UX and centralized logging UX
6. Run a PivotXR runtime spike
7. Ship PivotXR MVP after the spike validates the design

This order keeps the proven DepthXR runtime intact while building the platform needed for the rest of Phase 2.
