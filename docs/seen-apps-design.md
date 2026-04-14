# Seen XR Apps Design

## Purpose

VectorXR already has a user-managed application registry. Profiles target registered app ids, and the layer resolves those ids by comparing the current process executable name against each registered application's `match.exe`.

The seen-apps capability adds a separate discovery loop: when the OpenXR layer attaches to an XR process, it records a local observation of that executable. The desktop app can then show those observations and let the user create a registered app from one of them.

This should make profile setup feel guided without letting runtime observations mutate profile behavior by surprise.

## Current App Model

Registered apps live in the canonical v3 settings document:

```json
{
  "applications": [
    {
      "id": "dcs",
      "name": "DCS",
      "enabled": true,
      "match": {
        "exe": "DCS.exe"
      }
    }
  ]
}
```

Important current behavior:

- The Vue model defines `RegisteredApplication` in `app/src/lib/model.ts`.
- The Tauri backend mirrors that type in `app/src-tauri/src/main.rs`.
- The layer mirrors it in `layer/include/depthxr/settings.h`.
- The parser requires root `applications` in `layer/src/config_parser.cpp`.
- Runtime matching happens in `layer/src/settings_resolver.cpp`.
- The app registry UI edits only `name` and `match.exe`; ids are generated when apps are created.
- Removing a registered app also removes that id from Depth and Pivot profiles.

The matching key is already the executable basename, case-insensitive. This is a good fit for seen apps because the layer can collect the same value through `GetCurrentExecutableName()` without storing paths.

## Design Principles

- Keep observations local.
- Store observations separately from `settings.json`.
- Never automatically convert seen apps into registered apps.
- Avoid executable paths in the first version.
- Do not store runtime names until VectorXR has a concrete use for them.
- Treat seen-apps data as diagnostic/product-owned data, not runtime configuration.
- Make clearing observations easy.
- Keep the layer write path small and failure-tolerant.

## Storage

Add a separate JSON file beside config:

- Windows: `%LOCALAPPDATA%\VectorXR\config\seen-apps.json`
- Development fallback: `./config/seen-apps.json`
- Environment override: `VECTORXR_SEEN_APPS_PATH`

Keeping this under `config` rather than `logs` makes it easy for the app to load and clear, while still keeping it outside the canonical settings schema.

Proposed file shape:

```json
{
  "version": 1,
  "observations": [
    {
      "exe": "DCS.exe",
      "firstSeenUnixSeconds": 1776121112,
      "lastSeenUnixSeconds": 1776204418,
      "launchCount": 4
    }
  ]
}
```

Derived UI-only fields:

- `registeredApplicationId`: present when the seen executable matches an existing registered app.
- `registeredApplicationName`: display label for the match.
- `isRegistered`: convenience boolean from the fields above.

These derived fields should not be written by the layer.

## Layer Behavior

Record one observation during `OpenXrLayer::OnInstanceCreated`, after `current_exe_name_ = GetCurrentExecutableName()` and before or after config reload. This is the earliest point where VectorXR knows it has attached to an OpenXR process.

The layer should record observations only when the user-facing tracking preference is enabled. Add a canonical config flag for this so the layer can honor the app checkbox:

```json
{
  "core": {
    "trackSeenApps": true
  }
}
```

The default should be `true` for the guided setup experience, but the Application Registry UI must make the setting visible and explain that seen apps are stored locally only.

Update rules:

- Normalize for lookup by executable basename, case-insensitive.
- Preserve the most recently observed display spelling in `exe`, or keep the first spelling. Either is acceptable; prefer preserving first spelling for stable UI.
- If the exe is new, set `firstSeenUnixSeconds`, `lastSeenUnixSeconds`, and `launchCount = 1`.
- If the exe already exists, update `lastSeenUnixSeconds` and increment `launchCount`.
- Ignore empty executable names.
- Never read or write profile settings from this code path.
- Log failures at debug/info level only; seen-app writes must not prevent the layer from attaching.

Concurrency expectation:

Multiple XR processes could attach close together. The first implementation can use a simple read-modify-write with a best-effort file lock or atomic replace. If lock-free read-modify-write is used, keep the code isolated so it can be upgraded later.

Suggested C++ ownership:

- Add `ResolveSeenAppsPath()` to `config_path`.
- Add `seen_apps.h/.cpp` with a tiny model and `RecordSeenApp(exe)`.
- Reuse existing JSON parser only if it stays simple. If writing JSON becomes awkward in C++, use a narrowly scoped writer for this file shape.

## App Behavior

The app should load seen apps independently from the main settings document.

Tauri commands:

- `load_seen_apps() -> SeenAppsEnvelope`
- `clear_seen_apps() -> String`

Frontend model additions:

```ts
export interface SeenApplication {
  exe: string
  firstSeenUnixSeconds: number
  lastSeenUnixSeconds: number
  launchCount: number
}

export interface SeenApplicationView extends SeenApplication {
  isRegistered: boolean
  registeredApplicationId?: string
  registeredApplicationName?: string
}
```

The app should derive registration status by comparing `seen.exe` to `application.match.exe` with the same basename and case-insensitive behavior used by the layer.

## Registry Flow

Add a "Seen apps" area to the Application Registry tab.

Add a "Track seen XR apps" checkbox in the Application Registry tab. This edits `core.trackSeenApps` and should include short copy explaining that VectorXR stores only executable names and timestamps locally. When disabled, the layer should not update `seen-apps.json`.

For each seen executable:

- Show executable name.
- Show first seen and last seen times in local time.
- Show launch count.
- Show registered status.
- If not registered, offer "Add to registry".

"Add to registry" should:

- Create a normal `RegisteredApplication` using the seen executable.
- Reuse existing `createApplication(exe, applications)` behavior so id/name conventions stay consistent.
- Mark the settings working copy dirty like any other registry edit.
- Not automatically create Depth or Pivot profiles.
- Not remove the seen-app observation.

Clear action:

- Add "Clear seen apps".
- It clears the observations file only.
- It must not modify registered apps or profiles.

Per-entry removal is intentionally out of scope for the first slice. Seen apps are helpers for app registration, not long-lived user-authored records.

## Import Registration Migration

Config sharing should work even when a useful profile comes from an older or partially migrated file that still stores the executable match on the profile itself.

During import normalization, VectorXR should migrate profile-local executable matches into the shared application registry:

```json
{
  "modules": {
    "depthxr": {
      "profiles": [
        {
          "name": "DCS",
          "match": {
            "exe": "DCS.exe"
          },
          "settings": {}
        }
      ]
    }
  }
}
```

Target normalized shape:

```json
{
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
      "profiles": [
        {
          "name": "DCS",
          "enabled": true,
          "applicationIds": ["dcs"],
          "settings": {}
        }
      ]
    }
  }
}
```

Migration rules:

- Apply to Depth and Pivot profiles.
- If a profile already has valid `applicationIds`, preserve them.
- If a profile has no `applicationIds` and has `match.exe`, create or reuse a registered application for that executable.
- Reuse an existing registered app when its `match.exe` matches by basename, case-insensitive.
- Generate new app ids with the existing `createApplication` / `uniqueApplicationId` conventions.
- Prefer the profile name for the registered app name when the profile has a useful name; otherwise derive the app name from the executable.
- Do not create duplicate registered apps for repeated `DCS.exe` profiles.
- Remove or ignore profile-local `match` in the normalized output.
- Run this migration in the frontend import path and in the Tauri backend normalization path so imported files and saved files converge on the same v3 shape.

This makes exported configs portable: if one user exports a known-good Depth or Pivot profile for `DCS.exe`, another user should be able to import it and get both the registered application and the module profile linkage without hand-editing ids.

## Privacy

First version stores only executable basenames and timestamps. It does not store full paths, command lines, user names, window titles, or process arguments.

If future work needs paths, that should require a separate design pass and an explicit UI explanation.

## Validation And Failure Modes

The canonical settings schema should add `core.trackSeenApps` with a default of `true`.

Seen-app file normalization should tolerate:

- Missing file: return an empty list.
- Malformed JSON: return an empty list and a non-fatal status message in the app.
- Unknown fields: ignore them.
- Missing timestamps or counts: normalize to safe defaults.
- Duplicate executable entries: merge by normalized executable name.

The layer should keep running if:

- The seen-apps path cannot be resolved.
- The parent directory cannot be created.
- The file cannot be read.
- The file cannot be written.

## Sprint Checklist

1. Add shared seen-apps path resolution in the layer and Tauri backend.
2. Add `core.trackSeenApps` to the app model, Tauri model, C++ model, parser, schema, defaults, and validation.
3. Add layer-side observation writer and call it from `OnInstanceCreated` only when tracking is enabled.
4. Add unit coverage for path-independent observation merge behavior.
5. Add import migration for profile-local `match.exe` to registered app ids.
6. Add tests for importing a Depth/Pivot profile with `DCS.exe` and getting a reusable registered application.
7. Add Tauri commands to load and clear observations.
8. Add frontend types and command wrappers.
9. Add a seen-apps section to `AppRegistryEditor.vue`.
10. Add store actions for loading, clearing, and adding a seen executable to the registry.
11. Add UI copy that clearly distinguishes "seen" from "registered" and states that seen apps are stored locally only.
12. Verify with a fake seen-apps file and with an actual layer attach log.

## Decisions

- Do not add `runtimeName` yet. VectorXR does not currently use it, so it should wait until there is a concrete product need.
- Store `seen-apps.json` under the existing config directory.
- Support clearing the full seen-apps list in the first slice; skip per-entry removal for now.
- Add a user-facing Application Registry checkbox that controls seen-app tracking. Store only executable basenames and timestamps locally.
