# VectorXR Phase 2 Refactor Blueprint

## 1. Purpose

This blueprint translates the Phase 2 roadmap into a concrete refactor shape for the repository.

It is intended to answer:

- What code should stay where
- What new types and layers we should introduce
- How DepthXR and PivotXR should compose at runtime
- How to sequence implementation without destabilizing the working DepthXR path

This document assumes the architecture choice captured in `phase2-roadmap.md`:

- "Modules" remain a product, config, and UI concept
- Runtime behavior remains a single coordinated OpenXR layer
- Features execute through a shared pass pipeline rather than isolated runtime stacks

---

## 2. Architectural Summary

Target runtime shape:

1. One OpenXR layer owns interception and dispatch
2. One shared runtime context is built from OpenXR data
3. Ordered feature passes inspect and modify that context
4. The layer commits the final result back to OpenXR once

Target app shape:

1. One VectorXR shell owns load/save/navigation/status
2. Feature tabs edit feature-owned config sections
3. Shared validation and dirty-state tracking stay centralized

---

## 3. Repository Refactor Target

### 3.1 Layer Layout

Recommended target layout under `layer/`:

- `include/vectorxr/`
- `include/vectorxr/runtime/`
- `include/vectorxr/features/`
- `src/runtime/`
- `src/features/depthxr/`
- `src/features/pivotxr/`
- `src/platform/`
- `src/util/`
- `tests/`

Notes:

- The existing `depthxr` include namespace can be preserved temporarily during Milestone 1 to avoid a large rename blast radius.
- A namespace rename to `vectorxr` can happen after the new config/runtime model is stable.
- The current `math/` files should move under feature-owned folders once the pass pipeline is introduced.

### 3.2 App Layout

Recommended target layout under `app/src/`:

- `app/`
- `app/layout/`
- `features/core/`
- `features/depthxr/`
- `features/pivotxr/`
- `shared/components/`
- `shared/forms/`
- `shared/lib/`
- `stores/`

Notes:

- App shell concerns should not live in feature folders
- Shared form controls should not be duplicated per feature
- Validation helpers can stay centralized while feature-specific rules live near feature models

---

## 4. Runtime Design

### 4.1 Runtime Ownership

Keep one orchestrator that owns:

- Config loading and reload checks
- Session and view-configuration tracking
- QuadViews or stereo layout classification
- Feature resolution for the active executable
- Feature pass ordering
- Diagnostics and logging

This orchestrator should replace the current "single resolved settings blob" model with a "resolved runtime bundle" model.

### 4.2 Shared Runtime Context

Introduce an intermediate runtime context that represents the mutable state for one intercepted operation.

Suggested types:

```cpp
struct RuntimeCapabilities {
    bool has_quad_views{false};
    bool has_foveated_inset{false};
};

enum class ViewLayout {
    Mono,
    Stereo,
    StereoWithFoveatedInset,
};

struct EyePoseState {
    XrVector3f position;
    XrQuaternionf orientation;
};

struct EyeFovState {
    float angle_left{0.0f};
    float angle_right{0.0f};
    float angle_up{0.0f};
    float angle_down{0.0f};
};

struct EyeViewState {
    EyePoseState pose;
    EyeFovState fov;
};

struct FrameViewContext {
    ViewLayout layout{ViewLayout::Mono};
    std::vector<EyeViewState> views;
    RuntimeCapabilities capabilities;
};
```

Design rules:

- Build this from raw OpenXR structs once
- Feature passes operate on this shared context
- Commit back to OpenXR once after all passes finish

### 4.3 Resolved Runtime Bundle

Replace the current flat resolved settings concept with a structured resolved bundle.

Suggested shape:

```cpp
struct CoreRuntimeSettings {
    bool enabled{true};
    LogLevel log_level{LogLevel::Info};
    int log_retention_files{7};
};

struct DepthXrResolvedSettings {
    bool enabled{true};
    bool stereo_boost_enabled{true};
    bool convergence_enabled{true};
    double stereo_boost{1.0};
    double convergence{0.0};
};

struct PivotXrResolvedSettings {
    bool enabled{false};
    ActivationMode activation_mode{ActivationMode::Toggle};
    double rotation_multiplier{1.5};
    double smoothing{0.2};
    double deadzone_degrees{8.0};
};

struct ResolvedRuntimeConfig {
    CoreRuntimeSettings core;
    DepthXrResolvedSettings depthxr;
    PivotXrResolvedSettings pivotxr;
};
```

### 4.4 Pass Interface

The first version should be simple and internal, not a generic plugin framework.

Suggested interface:

```cpp
struct PassContext {
    const ResolvedRuntimeConfig& config;
    const RuntimeCapabilities& capabilities;
    std::string_view exe_name;
};

class IRuntimePass {
  public:
    virtual ~IRuntimePass() = default;
    virtual const char* Name() const = 0;
    virtual bool IsEnabled(const ResolvedRuntimeConfig& config) const = 0;
    virtual void Apply(FrameViewContext& frame, const PassContext& context) const = 0;
};
```

This is enough for Phase 2 because:

- Pass count is small
- Ordering is explicit
- Shared context stays under one owner
- Testing becomes easier

### 4.5 Pass Ordering

Start with a fixed ordering table.

Recommended initial order:

1. PivotXR pose-related pass
2. DepthXR stereo baseline pass
3. DepthXR convergence/projection pass
4. Diagnostics pass

Design rule:

- A later pass may depend on earlier pass output
- No pass should assume it exclusively owns OpenXR state

### 4.6 Commit Strategy

Create a dedicated commit step:

- Read raw `XrView[]`
- Build `FrameViewContext`
- Run passes
- Clamp or validate if needed
- Write final values back to `XrView[]`

Do not let individual passes write back to raw `XrView[]`.

---

## 5. Config Design Blueprint

### 5.1 File Strategy

Use one shared suite config file.

Recommended file name for Phase 2:

- `vectorxr.settings.json`

Compatibility note:

- The loader can continue checking the old path first during migration if that reduces friction
- The app should display the exact resolved path and save target

### 5.2 C++ Config Types

Introduce config document types that match product boundaries.

Suggested shape:

```cpp
struct CoreConfig {
    bool enabled{true};
    LogLevel log_level{LogLevel::Info};
    int log_retention_files{7};
};

struct DepthXrSettingsOverride {
    std::optional<bool> stereo_boost_enabled;
    std::optional<bool> convergence_enabled;
    std::optional<double> stereo_boost;
    std::optional<double> convergence;
};

struct DepthXrProfile {
    std::string name;
    ProfileMatch match;
    bool enabled{true};
    DepthXrSettingsOverride settings;
};

struct DepthXrModuleConfig {
    bool enabled{true};
    DepthXrResolvedSettings defaults;
    std::vector<DepthXrProfile> profiles;
};

struct PivotXrModuleConfig {
    bool enabled{false};
    PivotXrResolvedSettings defaults;
};

struct VectorXrConfigDocument {
    int version{2};
    CoreConfig core;
    DepthXrModuleConfig depthxr;
    PivotXrModuleConfig pivotxr;
};
```

Notes:

- Use separate override structs for profile overrides
- Do not put global logging back inside feature configs
- Use explicit module config types instead of one generic dictionary during the initial refactor

### 5.3 Migration Layer

Add explicit migration helpers:

- `ParseV1DepthXrConfig`
- `MigrateV1ToV2`
- `NormalizeV2Config`

Rules:

- Migration logic should be centralized
- Runtime code should consume normalized v2 types only
- The app store should also operate on normalized v2 types only after load

---

## 6. App Design Blueprint

### 6.1 Store Model

Refactor the current store into a suite-oriented store.

Suggested state:

```ts
interface AppState {
  loading: boolean
  saving: boolean
  path: string
  originalConfig: VectorXRConfig | null
  workingConfig: VectorXRConfig
  activeTab: 'core' | 'depthxr' | 'pivotxr'
  status: string
}
```

Key behavior:

- `originalConfig` is the last loaded or saved baseline
- `workingConfig` is the live editable state
- dirty state is derived from comparing those two

### 6.2 Frontend Composition

Suggested shell:

- `AppShell.vue`
- `TopNavTabs.vue`
- `StickySaveBar.vue`

Suggested feature roots:

- `features/core/CoreTab.vue`
- `features/depthxr/DepthXrTab.vue`
- `features/pivotxr/PivotXrTab.vue`

Suggested shared controls:

- `ToggleField.vue`
- `SliderField.vue`
- `NumericField.vue`
- `SectionCard.vue`
- `ProfileCard.vue`

### 6.3 UI Scale Mapping

Store canonical runtime values in config and map to user-friendly scales in the UI layer only.

Examples:

- Stereo Boost display scale can be shown as delta or labeled "SB" scale
- Convergence display scale can be shown with higher precision and a friendlier multiplier

Do not persist the display scale representation.

---

## 7. Concrete File-by-File Refactor Plan

### 7.1 Milestone 1 Files

Primary files likely to change:

- `config/depthxr.schema.json`
- new `config/vectorxr.schema.json`
- `app/src/lib/model.ts`
- `app/src/lib/validation.ts`
- `app/src/stores/configStore.ts`
- `app/src/lib/commands.ts`
- `layer/include/depthxr/settings.h`
- `layer/include/depthxr/settings_resolver.h`
- `layer/src/settings.cpp`
- `layer/src/settings_resolver.cpp`
- `layer/src/config_parser.cpp`
- `layer/tests/config_tests.cpp`
- `examples/depthxr.settings.example.json`

Likely new files:

- `layer/include/depthxr/runtime_types.h`
- `layer/include/depthxr/runtime_pass.h`
- `layer/src/runtime/frame_view_context.cpp`
- `layer/src/runtime/pass_runner.cpp`
- `layer/src/features/depthxr/depthxr_pass.cpp`

### 7.2 Milestone 2 Files

Primary files likely to change:

- `app/src/App.vue`
- new shell and tab components
- feature-specific UI components
- shared form components

### 7.3 Milestone 3 Files

Primary files likely to change:

- Current DepthXR math and config files
- Profile editor UI
- Validation rules
- Logging settings and removal of old fields

### 7.4 PivotXR Spike Files

Likely new files:

- `layer/src/features/pivotxr/pivotxr_pass.cpp`
- `layer/include/depthxr/pivotxr_types.h`
- `layer/tests/pivotxr_tests.cpp`

Exact interception additions will depend on the spike findings.

---

## 8. Implementation Sequence

### Step 1: Stabilize the Config Model

Deliver:

- New v2 config types in app and layer
- Migration from old config
- Tests for migration and profile fallback

Do not:

- Change active runtime math behavior yet

### Step 2: Introduce Shared Runtime Context

Deliver:

- `FrameViewContext`
- Build-and-commit helpers
- Pass interface

Do not:

- Add PivotXR yet

### Step 3: Port DepthXR into Passes

Deliver:

- Stereo boost pass
- Convergence pass
- Existing quad-view behavior preserved through shared context

Do not:

- Change user-facing behavior beyond removed features planned for Phase 2

### Step 4: Rebuild the App Shell

Deliver:

- VectorXR tab shell
- DepthXR tab extraction
- Dirty-state and sticky save bar

### Step 5: Add Logging UX

Deliver:

- Core logging settings
- Log retention
- Log viewing surface

### Step 6: Run PivotXR Spike

Deliver:

- Runtime proof
- Known interception points
- Compatibility notes

---

## 9. Guardrails for the Refactor

### 9.1 Do

- Keep the current working `xrLocateViews` behavior stable until the pass system is ready
- Introduce new structures beside the old ones when needed, then cut over cleanly
- Add regression tests before removing old fields
- Keep feature ownership clear in code and UI

### 9.2 Do Not

- Attempt a namespace rename and architecture rewrite in the same change
- Add a generic plugin system we do not yet need
- Split compatibility logic per feature
- Let app terminology dictate runtime ownership boundaries

---

## 10. Definition of Ready for Implementation

The project is ready to start Milestone 1 when:

- The roadmap is accepted
- The runtime boundary decision is accepted
- We agree to treat "module" as a product/config concept rather than an isolated runtime boundary

With that decision made, the next implementation target should be:

1. v2 config schema
2. app and layer config model update
3. migration tests

That is the safest first slice because it unlocks the rest of Phase 2 without forcing early runtime churn.
