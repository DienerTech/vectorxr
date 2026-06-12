# Quadviews Development Tracker

Date: 2026-06-12
Primary branch: `quadviews`
Integration branch: `quadviews-fable`

## Purpose

This document consolidates the current Quadviews implementation state, the
validated fixes from `quadviews-fable`, deferred experiments from the stashed
work, and proposed future features. Use it as the working roadmap before and
after merging `quadviews-fable` back into `quadviews`.

## Current State

VectorXR provides a native Quadviews engine through the OpenXR API layer:

- Advertises the Varjo-style quadview and foveated-rendering extension surface.
- Strips layer-owned extension names before passing instance creation to stereo runtimes that do not support them directly.
- Exposes `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET` when Quadviews resolves enabled.
- Maps app-facing quadview sessions to downstream stereo sessions.
- Synthesizes four views from runtime stereo views: peripheral left/right plus focus left/right.
- Uses `XR_EXT_eye_gaze_interaction` when available, with head/static fallback when eye gaze is unavailable.
- Tracks D3D11 swapchains and composes four app projection views into a stereo projection layer through the D3D11 compositor.
- Keeps the runtime projection-layer split path as fallback/probe behavior, not the preferred production path.
- Logs view-configuration, swapchain, compositor, timing, and pixel-budget diagnostics for headset testing.

The UI and resolver expose:

- Quadviews module enable flag.
- Default settings and per-application profiles.
- Tracking mode: eye or head.
- Focus region width/height.
- Focus and peripheral resolution scale.
- Transition thickness, sharpening, offsets, smoothing, and deadzone.
- Estimated pixel budget in the profile UI.

## Fable Branch Merge Contents

The `quadviews-fable` branch is a good merge candidate after keeping the
validated subset below and leaving the broad experimental stash out of the
merge.

Validated changes to merge:

- `xrLocateSpace` Pivot fix for quadview sessions.
- Config reload throttling.
- Resolved-settings generation caching.
- Input binding poll throttling.
- Persistent logger handle and debug-log gating.
- Scratch-buffer reuse under the existing lock lifetime model.
- Same-`displayTime` eye-offset caching, so predicted eye offsets are reused only for repeated locates of the same frame.
- `EndFrame` layer-count fix when rebuilding the submitted layer array.

Headset-tested behavior:

- The FA18 Pivot + Quadviews HMD/helmet issue is fixed by removing the quad-session `xrLocateSpace` Pivot bypass.
- Rebuilding foveal FOVs with Pivot's applied extra yaw/pitch broke foveated tracking during Pivot and was reverted.
- Current working model: recomposed quadview poses already carry the foveated views with Pivot; do not add Pivot's extra yaw/pitch to `BuildFocusFov` in the current bridged-pose path.

## Current In-Flight Checks

Before merging back to `quadviews`:

- Build the layer and run `depthxr_layer_tests`.
- Headset-test DCS with Pivot + Quadviews enabled.
- Confirm the foveated focus region follows correctly during Pivot after reverting the failed focus-FOV experiment.
- Confirm logs show D3D11 compositor success for the target app:
  - `D3D11 graphics binding detected`
  - `D3D11 quadviews compositor initialized`
  - `EndFrame quadviews projection split applied`
  - `d3d11QuadCompositions=1`
  - `unknownProjectionSwapchains=0`
- Keep the stashed second batch stashed or discard it after extracting only documented follow-up ideas.

## Deferred Fable Experiments

These ideas came from the stashed `quadviews-fable` work but should not merge
as a single batch. Each needs a small branch, isolated tests, and headset
validation.

### Native Quad Passthrough

Potential feature:

- Detect runtimes that natively expose the foveated-inset view configuration.
- Optionally pass native quadview sessions through instead of forcing VectorXR's stereo bridge.

Risks:

- Can bypass VectorXR's foveation controls and compositor path.
- Changes session topology decisions across app, layer, and runtime.
- A broad default such as `preferNativeQuadviews=true` can surprise users who expect VectorXR-controlled behavior.

Recommended approach:

- Make this explicit and profile-scoped.
- Default to VectorXR bridge unless there is a strong compatibility reason.
- Log the selected path in the view-configuration capabilities line.

### D3D11 State Isolation

Potential feature:

- Use `ID3DDeviceContextState` to swap in a known compositor pipeline state and restore app state afterward.

Risks:

- Broad D3D11 behavior change while the compositor is still being validated.
- Needs fallback behavior for runtimes/devices where context-state blocks are unavailable.

Recommended approach:

- Test separately from compositor output changes.
- Compare against targeted manual save/restore first.

### Explicit Cull-None Rasterizer

Potential feature:

- Bind a compositor-owned cull-none rasterizer state for the fullscreen triangle.

Why it is promising:

- The compositor draw should not depend on app rasterizer state.
- If default or leaked state culls the triangle, stale output-swapchain frames can appear as doubled or mismatched scenes.

Recommended approach:

- Add this independently.
- Keep `ClearRenderTargetView` in place while testing.

### Clear Removal

Potential feature:

- Skip `ClearRenderTargetView` because the fullscreen composition draw should write every output pixel.

Risks:

- Without fully owned blend/depth/rasterizer state, stale output or leaked app state can survive.
- Failure paths become harder to diagnose.

Recommended approach:

- Defer until compositor state ownership is proven.
- Keep the clear during active headset validation.

### Eye-Gaze Self-Sync Timing

Potential feature:

- Always call `xrSyncActions` for the layer eye-gaze action immediately before locating gaze.

Why it is promising:

- Could reduce gaze staleness and improve foveal placement during fast eye/head motion.

Risks:

- Adds per-frame OpenXR action-sync cost.
- Could conflict with app action timing assumptions on some runtimes.

Recommended approach:

- Test as a profile/runtime diagnostic toggle.
- Measure compositor timing and visible focus stability.

### EndFrame Lock Lifetime

Potential feature:

- Release the layer mutex before calling downstream `xrEndFrame`.

Why it is promising:

- Could reduce stalls for app threads concurrently calling `xrLocateSpace` or `xrLocateViews`.

Risks:

- Current scratch buffers are member-owned and protected by the lock.
- Needs proof that `xrEndFrame` is externally synchronized per session and that downstream code does not retain invalid pointers past the call.

Recommended approach:

- Defer until scratch-buffer lifetime is made obviously safe.

### Projection-Layer Fallback

Current stance:

- Keep the triple projection-layer fallback diagnostic-only.

Reason:

- If native D3D composition fails, submitting focus/peripheral/focus projection layers to a stereo runtime is inherently runtime-sensitive and can produce doubled foveal artifacts.

## Proposed Future Features

### Gaze Prediction

Lead the smoothed gaze target by angular velocity multiplied by estimated
pipeline latency, likely around 30-50 ms.

Goal:

- Keep the focus inset centered during saccades and fast vestibulo-ocular motion.
- Let users run a smaller focus region for the same perceived quality.
- Turn foveal tracking accuracy directly into GPU savings.

Implementation sketch:

- Track recent raw gaze yaw/pitch samples with timestamps.
- Estimate angular velocity after deadzone handling but before heavy smoothing.
- Predict `target = current + angularVelocity * latencySeconds`.
- Clamp prediction magnitude to avoid overshoot after noisy samples.
- Blend prediction down when gaze confidence/action state is unavailable.
- Add diagnostics for raw gaze, smoothed gaze, predicted gaze, velocity, and chosen latency.

Open questions:

- Should latency be a fixed profile setting, runtime-estimated, or adaptive?
- Should prediction apply only to eye tracking, or also to head-tracked fallback?
- What clamp prevents overshoot without losing the benefit during fast saccades?

### Dynamic Foveation

Use compositor timing and pixel-budget diagnostics to automatically tune focus
region size and/or resolution against a target frame time.

Goal:

- Make Quadviews adaptive instead of static.
- Maintain target frame time under load.
- Increase quality when there is spare GPU headroom.
- Differentiate VectorXR from existing quadview companion tools.

Existing groundwork:

- D3D11 compositor GPU timing queries already exist.
- Logs include `completedGpuMs` when timing resolves.
- Logs include `appPixelBudget`, based on app swapchain sizes and selected focus/peripheral scales.
- UI already has an estimated pixel-budget readout.

Implementation sketch:

- Add a per-profile dynamic mode: off, conservative, balanced, aggressive.
- Track rolling compositor GPU time and frame-time pressure.
- Choose target frame time from refresh rate when available, or profile setting when not.
- Scale focus size, focus scale, or peripheral scale within user-defined min/max bounds.
- Prefer small, slow adjustments to avoid visible quality pumping.
- Log decisions: target frame time, measured timing, old/new settings, reason.

Open questions:

- Which parameter should move first: peripheral scale, focus scale, or focus size?
- Should dynamic changes be runtime-only or reflected back into UI recommendations?
- How should CPU-bound frames be detected so GPU scaling does not chase the wrong bottleneck?

### Estimated GPU Savings UI

Promote the current pixel-budget calculation into a clearer UI feature.

Goal:

- Help users tune profiles before launching a game.
- Make performance tradeoffs understandable without exposing renderer internals.
- Turn the existing `appPixelBudget` log line into a profile-level confidence signal.

Implementation sketch:

- Show estimated stereo pixel cost for each profile.
- Add a plain-language savings readout, for example `Estimated 42% fewer app pixels than stereo`.
- Show severity bands: Performance, Balanced, Quality, Heavy.
- Warn when settings exceed 100% of stereo pixel cost.
- When runtime telemetry becomes available, compare estimated budget against observed `appPixelBudget`.
- Add optional advanced details for focus pixels, peripheral pixels, and compositor timing.

Related UI work:

- `docs/quadviews-ui-next-pass.md` already tracks presets, helper text, live runtime stats, and profile layout improvements.

## Diagnostics To Improve

- Add a concise runtime path summary:
  - bridged stereo runtime
  - native quad passthrough
  - compositor path active
  - fallback/probe path active
- Add compositor health hints:
  - GPU timing unavailable
  - suspiciously low completed GPU time
  - unknown projection swapchains
  - repeated D3D composition failure
- Add Pivot + Quadviews diagnostics only if future headset testing shows an actual foveal offset:
  - app-facing quadview poses
  - recomposed poses
  - focus FOV centers
  - cached Pivot delta
  - matched `displayTime`

## Merge Readiness

`quadviews-fable` is ready to merge back into `quadviews` when:

- The working tree includes only the validated runtime fixes and tracking docs.
- The broad stashed experiment batch is not applied.
- Layer build and `depthxr_layer_tests` pass.
- Headset validation confirms Pivot + Quadviews foveated tracking remains good.

