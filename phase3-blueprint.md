# VectorXR Phase 3 Blueprint

## 1. Purpose

This blueprint turns `../phase3-outline.md` into a concrete implementation plan for the current VectorXR repository.

Phase 3 should do two things at the same time:

1. polish the app into something that feels public-facing
2. add the shared systems that the next round of features actually depends on

The important framing is that the biggest Phase 3 work is not visual. The biggest work is introducing:

- a shared application registry
- a shared input binding system
- clearer conflict rules for profile resolution

Those systems should land before the final UI polish pass so the polished surfaces are built on the right data model.

---

## 2. Review Feedback on the Outline

The outline is directionally strong. It focuses on the right product themes:

- UX polish
- system cohesion
- production readiness

It also correctly identifies that ad-hoc executable matching and ad-hoc key activation are no longer enough.

Recommended clarifications before implementation:

### 2.1 Naming should be display-first, not internal-first

Recommendation:

- rename user-facing labels from `DepthXR` to `Depth`
- rename user-facing labels from `PivotXR` to `Pivot`
- keep internal ids, config keys, and C++ namespaces as `depthxr` and `pivotxr` for Phase 3

Reason:

- this avoids a large churn across schema, parser, settings types, tests, and runtime logs
- it gives the product the cleaner naming you want without paying for a risky internal rename in the same phase

### 2.2 Application management is a true core system

This should not be treated as just a nicer profile picker.

It needs to become the canonical source for:

- known applications
- executable matching
- profile targeting
- future app metadata

That means profile models should stop owning raw executable strings directly once the migration is complete.

### 2.3 Keybinding work needs a runtime boundary decision up front

The current Pivot path uses a simple keyboard activation model. Your outline expands that into:

- keyboard shortcuts
- joystick and HOTAS support
- stable device identity by Windows GUID

That is a bigger architectural jump than a normal form-field addition.

Before implementation starts, we should treat the binding system as:

- a shared config model
- a shared validation model
- a shared UI capture/edit flow
- a shared runtime input abstraction

If bindings must work while the Tauri app is closed, the layer needs the binding resolution path, not just the desktop UI.

### 2.4 Import should not silently overwrite the working copy

The outline says `Validate -> Apply -> Save if valid`.

I recommend a slightly safer flow:

1. import file
2. validate and normalize it
3. show a brief summary of what changed
4. apply it to the working copy
5. mark the config dirty
6. let the normal Save action persist it

That keeps import aligned with the rest of the app's save model and reduces accidental writes.

### 2.5 Auto-detection should stay a stretch goal

It is worth designing for, but I would not make installed-title scanning part of the first Phase 3 slice.

Recommendation:

- Phase 3 ships with manual app registration first
- optional detection can be a follow-on task or a small post-blueprint spike

### 2.6 Patch notes should be app-owned content, not runtime config

Patch notes are product content, not layer behavior.

They should live in an app-owned data file and optional app preference state, not in the shared runtime config schema.

### 2.7 Theme preference should stay app-local

Light and dark mode are UX concerns, not runtime concerns.

Recommendation:

- store theme preference outside the shared layer config
- use app-local persisted state with values `system`, `light`, `dark`

---

## 3. Phase 3 Goals

Primary goals:

- replace dev-facing shell copy with product-facing copy
- make navigation, status, save flow, and theming feel complete
- introduce a shared application registry for both modules
- introduce a shared binding model for keyboard and device actions
- support multi-application profile targeting
- surface profile conflicts before runtime behavior becomes surprising
- make the app feel stable enough for public release

Non-goals for the first Phase 3 implementation:

- full automatic VR title discovery
- a wholesale rename of internal ids and namespaces
- a generic plugin or action framework
- deep installer or updater work

---

## 4. Recommended Product Decisions

### 4.1 Tabs and Labels

Recommended visible tabs:

- `Home`
- `Depth`
- `Pivot`

Recommended internal tab ids:

- `core`
- `depthxr`
- `pivotxr`

### 4.2 Theme Ownership

Theme preference should be owned by the app shell, not the shared JSON config.

Recommended preference model:

```ts
type ThemePreference = 'system' | 'light' | 'dark'
```

Recommended persistence:

- app-local storage in Phase 3
- Tauri-side persistence only if local storage becomes insufficient

### 4.3 Patch Notes Ownership

Recommended content source:

- versioned JSON file bundled with the app

Recommended app behavior:

- header button shows latest version title
- modal shows full patch note history
- app can track "last seen version" in local preference state

### 4.4 Application Registry Ownership

Recommended config principle:

- application definitions live at the suite level
- modules reference applications by id

Recommended first-pass application model:

```ts
interface RegisteredApplication {
  id: string
  name: string
  enabled: boolean
  match: {
    exe: string
  }
}
```

This is intentionally narrow. We can add install path, store, launch hints, or aliases later without blocking Phase 3.

### 4.5 Binding Ownership

Recommendation:

- use one shared binding data model
- keep bindings stored at the action owner, not in a top-level binding registry

That means:

- module-level actions can own module-level bindings
- Pivot profiles can own profile-level activation bindings
- validation and runtime resolution still come from one shared binding system

This keeps the data model simpler while still giving us one source of truth for binding semantics.

---

## 5. Target Data Model Changes

Phase 3 likely requires a config version bump from `2` to `3`.

Recommended shared config direction:

```json
{
  "version": 3,
  "core": {
    "enabled": true,
    "logLevel": "info",
    "logRetentionFiles": 7
  },
  "applications": [
    {
      "id": "dcs",
      "name": "DCS",
      "enabled": true,
      "match": {
        "exe": "DCS.exe"
      }
    }
  ],
  "modules": {
    "depthxr": {
      "enabled": true,
      "defaults": {
        "stereoBoostEnabled": true,
        "convergenceEnabled": true,
        "stereoBoost": 1.1,
        "convergence": 0.0
      },
      "bindings": {
        "toggleEnabled": {
          "type": "keyboard",
          "chord": ["Ctrl", "Alt", "D"]
        }
      },
      "profiles": [
        {
          "name": "DCS Default",
          "enabled": true,
          "applicationIds": ["dcs"],
          "settings": {
            "stereoBoostEnabled": true,
            "convergenceEnabled": true,
            "stereoBoost": 1.15,
            "convergence": 0.0
          }
        }
      ]
    },
    "pivotxr": {
      "enabled": false,
      "defaults": {
        "yawRotationMultiplier": 1.5,
        "yawSmoothing": 0.2,
        "yawDeadzoneDegrees": 8.0,
        "yawMaxExtraDegrees": 25.0,
        "pitchRotationMultiplier": 1.0,
        "pitchSmoothing": 0.2,
        "pitchDeadzoneDegrees": 12.0,
        "pitchMaxExtraDegrees": 20.0
      },
      "profiles": [
        {
          "name": "DCS Toggle Pivot",
          "enabled": true,
          "applicationIds": ["dcs"],
          "activationBinding": {
            "type": "keyboard",
            "chord": ["F8"]
          },
          "settings": {
            "yawRotationMultiplier": 1.7,
            "yawSmoothing": 0.2,
            "yawDeadzoneDegrees": 10.0,
            "yawMaxExtraDegrees": 60.0,
            "pitchRotationMultiplier": 1.2,
            "pitchSmoothing": 0.2,
            "pitchDeadzoneDegrees": 15.0,
            "pitchMaxExtraDegrees": 45.0
          }
        }
      ]
    }
  }
}
```

Notes:

- `applications` becomes the shared registry
- Depth profiles move from one `match.exe` to `applicationIds`
- Pivot gains real profiles instead of only module defaults
- the example uses keyboard bindings, but the binding model should also support device-based triggers

### 5.1 Shared Binding Shape

Recommended initial binding union:

```ts
interface KeyboardBinding {
  type: 'keyboard'
  chord: string[]
}

interface DeviceBinding {
  type: 'device'
  deviceGuid: string
  inputPath: string
}

type InputBinding = KeyboardBinding | DeviceBinding
```

Design rule:

- `inputPath` should represent a logical control like `button-1`, `hat-up`, or `axis+`
- do not encode transient device order

### 5.2 Compatibility Rule

Phase 3 does not need to preserve Phase 2 saved config files.

Recommended handling:

- v3 is the only active schema
- old saved configs may be discarded or replaced with a default v3 document
- keep the implementation focused on the new application registry instead of v2 migration support

---

## 6. Runtime and Resolution Rules

### 6.1 Application Resolution

Recommended matching rule:

1. detect active executable name from the current process
2. resolve a registered application by normalized executable name
3. resolve module profiles by `applicationIds`

This keeps matching logic centralized and avoids repeating executable strings across modules.

### 6.2 Depth Conflict Rule

Recommended rule, matching the outline:

- if multiple enabled Depth profiles target the same application, the first enabled profile wins

Required UX behavior:

- show a warning in the editor
- visually flag the losing profiles
- surface the same conflict in validation state

### 6.3 Pivot Conflict Rule

Recommended rule, matching the outline:

- multiple Pivot profiles may target the same application
- if two enabled profiles for the same application share the same activation binding, only the first enabled profile executes

Required UX behavior:

- warn on same-app same-binding collisions
- show the actual colliding binding in the warning text

### 6.4 Module Status Rule

Top-level status should reflect:

- module enabled state
- unresolved conflicts
- missing applications
- invalid bindings

This is more useful than a plain enabled or disabled badge.

---

## 7. App Architecture Blueprint

### 7.1 Shell Changes

Primary shell changes:

- convert the current large hero header into a tighter pinned app header
- keep header and module tabs visible while scrolling
- replace milestone language with product language
- move save state into a tighter persistent surface
- add latest patch notes entry point in the header

Recommended new shell pieces:

- `AppHeader.vue`
- `PatchNotesModal.vue`
- `ThemeToggle.vue`
- `StatusPill.vue`

### 7.2 Navigation Changes

Recommended navigation labels:

- `Home`
- `Depth`
- `Pivot`

Recommended behavior:

- each tab shows enabled state
- conflicted tabs show a warning accent
- tabs remain visible on all main editor screens

### 7.3 Home Tab

Recommended Home tab responsibilities:

- suite enabled state
- theme toggle
- patch notes summary
- application registry management
- module overview
- about footer
- shared config path
- log access

### 7.4 Depth Tab

Recommended Depth tab responsibilities:

- module enabled toggle
- module defaults section
- module-level bindings
- profile list
- per-profile application targeting
- conflict banners
- tighter slider behavior and invalid-range cleanup

### 7.5 Pivot Tab

Recommended Pivot tab responsibilities:

- module enabled toggle
- defaults section
- profile list with activation bindings
- tooltips for all user-facing settings
- consistent yaw and pitch layout
- expanded max ranges to 180 where approved

### 7.6 Save and Reload UX

Recommended behavior:

- sticky save surface remains visible
- unsaved changes are always obvious
- reload prompts before clobbering dirty edits
- import feeds the same working-copy flow as manual edits

---

## 8. Shared UI Components to Add

Recommended additions under `app/src/components/`:

- `AppHeader.vue`
- `PatchNotesModal.vue`
- `ThemeToggle.vue`
- `AppRegistryEditor.vue`
- `ApplicationPicker.vue`
- `BindingEditor.vue`
- `BindingCaptureModal.vue`
- `ConflictBanner.vue`
- `TooltipLabel.vue`
- `SectionHeader.vue`

Likely existing components to update:

- `TopNavTabs.vue`
- `StickySaveBar.vue`
- `ProfileEditor.vue`
- `EffectField.vue`
- `tabs/CoreTab.vue`
- `tabs/DepthXrTab.vue`
- `tabs/PivotXrTab.vue`

---

## 9. Backend and Layer Work

### 9.1 App Backend

Likely Tauri-side work in `app/src-tauri/src/main.rs`:

- load bundled patch notes data
- support import file reading if browser-only file input is not enough
- persist app-local preferences if we outgrow local storage

### 9.2 Layer Config Work

Likely shared config work:

- `config/vectorxr.schema.json`
- `layer/include/depthxr/settings.h`
- `layer/src/settings.cpp`
- `layer/src/config_parser.cpp`
- `layer/src/settings_resolver.cpp`
- `layer/tests/config_tests.cpp`

### 9.3 Input Runtime Work

Likely new native input support area under `layer/`:

- shared binding types
- binding normalization and parsing
- keyboard chord support
- device GUID based binding support
- runtime polling or event ingestion path used when the layer is active

This is the highest-risk new engineering area in the phase.

---

## 10. File-by-File Work Map

Likely app files to change:

- `app/src/App.vue`
- `app/src/components/TopNavTabs.vue`
- `app/src/components/StickySaveBar.vue`
- `app/src/components/ProfileEditor.vue`
- `app/src/components/tabs/CoreTab.vue`
- `app/src/components/tabs/DepthXrTab.vue`
- `app/src/components/tabs/PivotXrTab.vue`
- `app/src/lib/model.ts`
- `app/src/lib/validation.ts`
- `app/src/lib/commands.ts`
- `app/src/stores/configStore.ts`
- `app/src/style.css`
- `app/src-tauri/src/main.rs`

Likely new app files:

- `app/src/components/AppHeader.vue`
- `app/src/components/PatchNotesModal.vue`
- `app/src/components/ThemeToggle.vue`
- `app/src/components/AppRegistryEditor.vue`
- `app/src/components/ApplicationPicker.vue`
- `app/src/components/BindingEditor.vue`
- `app/src/components/BindingCaptureModal.vue`
- `app/src/components/ConflictBanner.vue`
- `app/src/lib/theme.ts`
- `app/src/lib/patchNotes.ts`

Likely layer files to change:

- `config/vectorxr.schema.json`
- `examples/vectorxr.settings.example.json`
- `layer/include/depthxr/settings.h`
- `layer/include/depthxr/settings_resolver.h`
- `layer/src/settings.cpp`
- `layer/src/config_parser.cpp`
- `layer/src/settings_resolver.cpp`
- `layer/tests/config_tests.cpp`

Likely new layer files:

- `layer/include/depthxr/input_binding.h`
- `layer/include/depthxr/application_registry.h`
- `layer/src/input_binding.cpp`
- `layer/src/application_registry.cpp`
- `layer/src/platform/input_state.cpp`
- `layer/src/platform/input_state.h`

The exact input file layout can move, but the feature boundary should remain clear.

---

## 11. Implementation Sequence

### Step 1: Product Label and Shell Cleanup

Deliver:

- display-name rename to `Home`, `Depth`, and `Pivot`
- pinned header and tabs
- removal of milestone and dev-facing copy
- tighter save state presentation
- about section refresh

Acceptance criteria:

- no user-facing Phase 1 or Phase 2 wording remains
- header and tabs stay visible while scrolling
- tab states can show enabled or warning conditions

### Step 2: Theme System

Deliver:

- `system`, `light`, and `dark` theme preference
- visual tokens for both themes
- save bar, tabs, cards, and modals verified in both themes

Acceptance criteria:

- app defaults to system theme
- user theme choice persists across launches
- all key surfaces remain readable in both themes

### Step 3: Patch Notes System

Deliver:

- bundled patch-notes data source
- header entry point
- modal log view
- latest version summary in the header

Acceptance criteria:

- latest entry is visible without opening the full log
- full history is scrollable and readable

### Step 4: Application Registry Foundation

Deliver:

- config v3 application registry
- migration from Depth executable matches
- Home-tab application manager
- profile editor support for selecting one or more applications

Acceptance criteria:

- profiles no longer need to own raw executable strings
- one app definition can be reused across modules
- validation catches duplicate or malformed application definitions

### Step 5: Shared Binding Model

Deliver:

- shared binding config types
- binding editor and capture flow
- keyboard chord validation
- schema support for device bindings

Acceptance criteria:

- bindings can be edited consistently across modules
- invalid or incomplete bindings are surfaced before save

### Step 6: Runtime Input Support

Deliver:

- runtime keyboard chord support
- stable device-guid based binding parsing
- initial device polling support for joystick and HOTAS inputs

Acceptance criteria:

- bindings still work when the app is not open
- device order changes do not break the saved association

### Step 7: Depth Module Upgrade

Deliver:

- `Depth Enabled` module toggle label
- module-level toggle binding
- separate global/default settings language
- multi-application profile targeting
- first-enabled conflict warnings
- slider tuning and invalid-negative cleanup

Acceptance criteria:

- Depth conflicts are visible before save
- a profile can target multiple applications
- module toggle binding can be configured and validated

### Step 8: Pivot Module Upgrade

Deliver:

- `Pivot` display rename
- profile-based config ownership
- tooltips for all settings
- yaw and pitch layout consistency
- max limit updates to approved values
- same-app same-binding conflict warnings

Acceptance criteria:

- Pivot profiles can target multiple applications
- per-profile activation bindings are editable
- conflicting bindings are surfaced clearly

### Step 9: Import, Reload, and Final Polish

Deliver:

- import flow into working copy
- guarded reload flow
- final copy cleanup
- visual consistency pass
- production-ready wording review

Acceptance criteria:

- import does not bypass validation
- reload does not accidentally discard unsaved work
- remaining UI copy reads like a public product

---

## 12. Testing Strategy

### 12.1 Layer Tests

Add or extend tests for:

- v3 application registry parsing
- application registry resolution
- multi-application profile matching
- Depth first-enabled conflict behavior
- Pivot same-binding conflict behavior
- binding parsing and normalization
- device-guid persistence behavior where feasible

### 12.2 App Validation Tests

Add or extend tests for:

- theme preference persistence
- application registry validation
- binding validation
- import normalization
- module conflict surfacing

### 12.3 Manual QA Matrix

Cover at minimum:

- light theme
- dark theme
- system theme handoff
- import valid config
- import invalid config
- reload with dirty state
- Depth profile conflict
- Pivot binding conflict
- keyboard binding behavior
- joystick or HOTAS device reassignment by GUID

---

## 13. Risks

Highest-risk areas:

- input-device identity and runtime polling
- schema migration from executable strings to application ids
- keeping conflict rules understandable in the UI
- preserving current Pivot behavior while moving to profile-based ownership

Guardrails:

- do not rename internal schema keys in the same phase as the new shared systems
- do not bury application conflict information in secondary UI
- do not make device bindings depend on transient device order
- do not put theme or patch-note state into the shared runtime config

---

## 14. Approval Checklist

Before implementation starts, I want explicit sign-off on these decisions:

1. keep internal ids as `depthxr` and `pivotxr` for Phase 3
  A: Yep that's fine. I just want the front-end labels changed
2. bump shared config from v2 to v3
  A: sounds good
3. ship manual app registration first, not auto-detection first
  A: sure, this is a nice to have
4. keep theme preference app-local, not in shared config
  A: agreed, the .dll doesn't care
5. use working-copy import flow rather than auto-save on import
  A: yep sounds good, I trust your judgement here
6. treat runtime input support as part of Phase 3, not just UI plumbing
  A: sounds good. This is a nice to have we can implement later

If those decisions look right to you, the safest implementation order is:

1. shell and theme foundation
2. patch notes
3. application registry
4. shared bindings plus runtime input support
5. Depth and Pivot upgrades
6. import, reload, and final polish

A: Looks like a good plan!
