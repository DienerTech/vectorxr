# OpenXR Layer Management Outline

## Purpose

VectorXR already depends on external OpenXR layer state in at least one important user-facing case: Pivot with quad views.

Current app guidance already tells users that layer order matters:

- `Quad-Views-Foveated` should appear above VectorXR
- if VectorXR is above the quad views layer, Pivot rendering can break

Today the user must leave VectorXR and use a separate third-party utility to inspect or correct that state. This makes an important compatibility dependency feel hidden and fragile.

This feature would let VectorXR expose and manage the small piece of the Windows OpenXR layer system that matters most for real users:

- list installed implicit OpenXR API layers
- show whether each layer is enabled
- show actual load order
- let the user enable or disable a layer
- let the user reorder layers within the supported scope
- show whether the backing DLL appears code-signed

This document intentionally does **not** propose a full "diagnose and fix all OpenXR problems" tool. The goal is a practical management surface, not a registry doctor.

## Why It Fits 1.0

This looks like a strong 1.0 feature for VectorXR because it closes a direct gap in the existing product:

- Pivot already has a known layer-order dependency
- VectorXR already installs and unregisters its own implicit OpenXR layer in `HKLM`
- the app already uses Tauri commands for small Windows-specific operations
- the app already has a home for XR-adjacent system management under the primary tab group

This is also functionality Windows does not expose anywhere obvious, and runtime vendors generally do not provide a neutral third-party layer manager.

## Product Goal

Give users a trustworthy, low-friction way to see and manage the OpenXR implicit layer registrations on their machine, while clearly steering them toward the mainstream machine-wide 64-bit configuration that most of the Windows PCVR ecosystem actually uses.

## Scope For 1.0

### In scope

- Read the 64-bit machine-wide implicit OpenXR API layer list from `HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
- Parse each referenced manifest
- Extract enough manifest data to show:
  - layer name
  - description
  - manifest path
  - library path
  - enabled or disabled state
  - current order
- Allow toggling enabled or disabled state
- Allow moving a layer up or down within the managed list
- Highlight VectorXR and known adjacent layers cleanly in UI
- Show a code-signing status for the backing DLL when it can be resolved
- Show clear copy when a write action requires elevation

### Explicitly out of scope

- A "Fix it" button
- A maintained database of vendor quirks or per-layer compatibility rules
- Automatic registry cleanup of stale manifests
- Layer installation or uninstallation flows
- Managing explicit API layers
- Managing per-user `HKCU` layers in 1.0
- Managing 32-bit layer registrations in 1.0
- Broad runtime diagnostics beyond a narrow layer-management surface

## Supported System Slice

VectorXR should treat these four implicit layer registration slices as first-class readable and writable data:

- 64-bit
- machine-wide
- per-user
- 32-bit machine-wide
- 32-bit per-user

That maps to the usual Windows/OpenXR registry locations:

- `HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKLM\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit`
- `HKCU\Software\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit`

Reasons:

- this matches VectorXR's own installation model today
- once one hive can be read and written, the others are low additional implementation cost
- advanced users may still want visibility into unusual or legacy installs
- exposing all four slices avoids the misleading impression that VectorXR sees the whole system when it only sees one bucket

However, the UI should still make a strong distinction between the common slice and the uncommon ones.

Recommended product stance:

- present `HKLM` 64-bit as the primary and recommended configuration
- show the other three slices with clear warnings that they are uncommon
- explain that mixing layer registrations across hives and bitness buckets can make behavior harder to reason about
- avoid implying that order is meaningful across different hives or different registry views

The important nuance is that managing all four slices is feasible, but interpreting them as one unified ordered list would be misleading.

If VectorXR detects additional layers in `HKCU` or 32-bit registry views later, the app can mention that broader layer registrations may exist without trying to manage them in the first slice.

## User Experience Direction

### Placement

This feature should likely live as a new primary app-management tab or section, not inside the Pivot tab itself.

Good candidate labels:

- `OpenXR Layers`
- `Layer Manager`

Why:

- the feature is broader than Pivot
- it is still an app/system management surface, similar in spirit to the Application Registry tab
- it may later help Depth, future modules, and general support workflows

### Core UI

The screen should be organized by registry slice, not as one flat combined list.

Suggested groups:

- `Machine-wide 64-bit (Recommended)`
- `Per-user 64-bit`
- `Machine-wide 32-bit`
- `Per-user 32-bit`

Each group should explain:

- whether it is commonly used
- whether VectorXR recommends using it
- whether writes in that group require elevation
- that ordering is meaningful only within that group

Each layer row should show:

- display name
- enabled or disabled state
- current order number
- short description
- manifest path
- resolved DLL path when available
- signature status

Primary actions:

- `Enable` or `Disable`
- `Move Up`
- `Move Down`
- `Open Manifest Folder`
- `Open DLL Folder` when resolvable

Recommended supporting UI:

- small explanation of what OpenXR implicit layers are
- note that ordering affects runtime behavior
- note that order does not merge cleanly across separate registry hives or separate 32-bit and 64-bit views
- note that some changes require elevation
- note that changes apply to future app launches, not already-running XR apps

## VectorXR-Specific Guidance

The first version does not need a general compatibility rules engine, but it should still help users with the one VectorXR-specific dependency we already know about.

Recommended UX:

- when both `Quad-Views-Foveated` and VectorXR are present, show a simple recommendation
- if VectorXR appears above `Quad-Views-Foveated`, show a clear warning
- offer a direct reorder action if possible

This should be framed as a narrow VectorXR compatibility note, not a generalized knowledge base.

## Data And Native Surface

### Tauri backend responsibilities

Add a small native layer-management surface in `app/src-tauri/src/main.rs` or a dedicated module.

Likely commands:

- `load_openxr_layers() -> OpenXrLayerSnapshot`
- `set_openxr_layer_enabled(manifest_path, enabled) -> OpenXrLayerSnapshot`
- `move_openxr_layer(manifest_path, direction) -> OpenXrLayerSnapshot`
- `openxr_layer_signature_status(dll_path) -> SignatureStatus`

Depending on implementation shape, the write commands may instead return a simple success result and the frontend can reload after mutation.

### Suggested response shape

Example shape:

```ts
type OpenXrLayerRegistrySlice =
  | 'hklm64'
  | 'hkcu64'
  | 'hklm32'
  | 'hkcu32'

interface OpenXrLayerEntry {
  slice: OpenXrLayerRegistrySlice
  manifestPath: string
  layerName: string
  description: string
  libraryPath?: string
  enabled: boolean
  order: number
  signatureStatus: 'signed' | 'unsigned' | 'invalid' | 'unknown'
  isVectorXr: boolean
}

interface OpenXrLayerSnapshot {
  slices: Array<{
    id: OpenXrLayerRegistrySlice
    registryPath: string
    recommended: boolean
    uncommon: boolean
    requiresElevationForWrites: boolean
    layers: OpenXrLayerEntry[]
  }>
}
```

This is intentionally product-shaped rather than a full manifest mirror.

## Ordering Notes

Enumeration is easy. Ordering is the trickiest part of this feature and should be validated carefully during implementation.

Important constraints:

- ordering only makes sense within one registry hive and registry view
- the app must not pretend that four registry slices form one global sortable list
- the app must show the actual effective order separately for each slice
- reorder operations should stay conservative and explicit

Implementation should prefer simple `Move Up` and `Move Down` actions over drag-and-drop in the first slice. That keeps intent clear and reduces accidental state churn.

## Elevation Model

VectorXR does **not** need to run elevated all the time.

Expected behavior:

- reading from all four managed layer slices is available to a normal app session
- writing `HKLM` changes requires elevation
- writing `HKCU` changes should not require elevation
- writing reorder changes follows the same rule as any other write in that slice

Recommended model for 1.0:

- launch the app unelevated
- show the current layer state normally
- trigger an elevated helper flow only when the user performs a write action in an `HKLM` slice

This keeps the day-to-day app experience normal while still allowing self-management of layer state when needed.

## Signature Check

A lightweight signature check is a good fit for this feature.

Desired behavior:

- resolve the layer DLL path from the manifest when possible
- ask Windows whether the DLL has a valid Authenticode signature
- report a simple user-facing status

Suggested statuses:

- `Signed`
- `Unsigned`
- `Signature invalid`
- `Unknown`

This gives users a helpful trust signal without turning the feature into a security scanner.

## Failure Modes

The UI should be resilient when:

- a registry value points to a missing manifest
- a manifest exists but is malformed
- a manifest references a missing DLL
- a signature check fails unexpectedly
- a write action is canceled at the UAC prompt

Recommended behavior:

- show broken entries honestly
- keep the rest of the list usable
- never block the whole screen on one bad layer
- surface write failures as clear status text
- if layers exist in uncommon slices, explain that they may be valid but are outside the usual ecosystem setup

## Open Questions To Validate During Implementation

- What is the most reliable way to represent and preserve actual implicit layer order in the managed registry slice?
- What is the most reliable way to represent and preserve actual implicit layer order within each managed registry slice?
- Should reorder actions be limited to adjacent swaps in 1.0?
- Should the first shipping UI live as its own top-level tab or as a section under `Application Registry`?
- Do we want to cache signature status in the snapshot response, or resolve it lazily per row?
- How strongly should VectorXR warn when the same or related layers are spread across multiple slices?

## Proposed Sprint Checklist

1. Add a docs-backed implementation note confirming the supported registry slice and the order model.
2. Add Tauri read commands for the managed OpenXR implicit layer list.
3. Parse manifest name, description, and library path.
4. Add frontend types and command wrappers.
5. Add a new app surface for listing layers and showing their state.
6. Add native write commands for enable or disable.
7. Add native write commands for move up or move down.
8. Add elevation flow for write operations only.
9. Add Windows signature status support for resolved DLLs.
10. Add VectorXR-specific quad views guidance when relevant layers are present.
11. Validate behavior with VectorXR plus `Quad-Views-Foveated` on a real machine.
12. Verify that canceled UAC prompts and malformed manifests degrade cleanly.

## Shipping Position

This feature is small enough in concept to be realistic for 1.0 **if** VectorXR keeps the scope disciplined:

- one clear management surface
- grouped handling of all four implicit layer registry slices
- read, toggle, reorder, and inspect
- strong UX guidance toward `HKLM` 64-bit
- one narrow VectorXR compatibility hint

That would deliver genuinely important functionality without turning launch scope into an ecosystem-maintenance project.
