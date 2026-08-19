# Real-runtime hardware probe

`vectorxr_hardware_probe` is an opt-in developer executable for exercising
VectorXR against the active OpenXR runtime and headset without relying on a
game to use a particular frame loop or reference-space arrangement. It is not
included in the VectorXR installer or the default CMake build.

The probe renders an asymmetric line scene through D3D11: a ground grid,
colored axes, and two differently placed beacons. Each eye is limited to
1280 pixels in either dimension so the diagnostic does not allocate the
headset's full recommended render size.

## Build

Configure a dedicated build tree from the repository root:

```powershell
cmake -S . -B build/hardware-probe `
  -DDEPTHXR_BUILD_HARDWARE_PROBES=ON `
  -DDEPTHXR_BUILD_TESTS=ON
cmake --build build/hardware-probe --config RelWithDebInfo `
  --target vectorxr_hardware_probe --parallel
```

The executable and a matching OpenXR loader DLL are written to:

```text
build\hardware-probe\layer\RelWithDebInfo\vectorxr_hardware_probe.exe
```

The non-headset pose-math check can run in CI or from a desktop session:

```powershell
.\build\hardware-probe\layer\RelWithDebInfo\vectorxr_hardware_probe.exe --self-test
```

## Prepare VectorXR

1. Enable the VectorXR OpenXR layer and set logging to **Debug**.
2. Ensure Pivot applies to `vectorxr_hardware_probe.exe`. A default Pivot
   profile is sufficient, or add an application-specific profile after the
   probe first appears in the Application Registry.
3. Use a Pivot action with both yaw and pitch. A combined Snap View makes a
   space-axis error much easier to see than pure yaw.
4. Leave Turbo on **Auto** for the Pivot-only runs.

The probe creates two distinct `LOCAL` reference spaces. The source has an
identity offset. The target is translated and yawed 90 degrees. Using two
`LOCAL` handles avoids depending on optional `STAGE` support while still
requiring a real coordinate conversion.

## Pivot routing modes

Run one mode per OpenXR session so each produces a clean VectorXR log:

```powershell
$probe = '.\build\hardware-probe\layer\RelWithDebInfo\vectorxr_hardware_probe.exe'

& $probe --mode source-exact --seconds 20
& $probe --mode cross-space --seconds 20
& $probe --mode cross-space-cached --seconds 20
```

Press Escape in the console window to end a run early.

| Mode | LocateViews calls | Projection space | Expected resolution |
| --- | --- | --- | --- |
| `source-exact` | Source `LOCAL` | Source `LOCAL` | `source_exact` |
| `cross-space` | Source `LOCAL` | Offset `LOCAL` | `canonical_view_locate` |
| `cross-space-cached` | Source and offset `LOCAL` at the same display time | Offset `LOCAL` | `located_views_cache` |

The cross-space modes render from the source-space views, obtain the runtime's
source-to-target relation, and submit equivalent poses in the target space.
The scene should therefore look the same in all three modes. With Pivot active,
an unconverted pitch correction would become diagonal or roll-like in the
90-degree-yawed target space; translation mistakes tend to look like an orbit
or lateral jump.

### Log pass criteria

A cross-space run should contain an information line similar to:

```text
PivotXR space-aware projection correction active: ...
resolution=canonical_view_locate
```

Its EndFrame diagnostics should show:

```text
spaceConversions=1
spaceConversionFailures=0
```

The cached run should instead show `resolution=located_views_cache`, and its
LocateViews diagnostics should eventually show `logicalFrameReused=1`. This
confirms that a second world-space LocateViews query for one display time reused
the first logical Pivot update and cached the correction in the second space.

The source-exact run is the control. It should remain `resolution=source_exact`
with zero space-relation queries and conversions.

For all modes, treat any of these as failures:

- `PivotXR projection-space conversion failed`
- `spaceConversionFailures` greater than zero
- repeated continuity misses during steady Pivot motion
- a visible axis change, orbit, or jump between the control and cross-space runs
- an OpenXR or swapchain error printed by the probe

An isolated activation-boundary continuity miss followed by an exact recovery
on the next frame is recorded separately from a sustained routing failure.

## Async handoff mode

`--threaded-wait` uses a persistent application wait thread. It requests
`xrWaitFrame(N+1)` immediately before the render thread submits
`xrEndFrame(N)`, reproducing the legal overlapping call pattern used by DCS.

To exercise VectorXR's async handoff shield:

1. Fully close any active OpenXR application.
2. Force **Turbo > Frame Pacing > Async** in VectorXR.
3. Run a cross-space mode with threaded waiting:

```powershell
& $probe --mode cross-space --threaded-wait --seconds 30
```

Expected debug evidence includes:

```text
Turbo pacing: async (forced in settings...)
Turbo-diag: async handoff shield armed
Turbo-diag: app xrWaitFrame intercepted by async handoff shield
Turbo-diag: async handoff published runtime wait
Frame pacing ... pacing=async ... asyncHandoff ...
```

Healthy handoff windows have increasing armed/wait/begin counters and no
cancellations. PiOpenXR is known to interlock an off-thread `xrWaitFrame` with
submission, so forced Async may produce approximately 250 ms stalls and
auto-suspend after five drain timeouts. That outcome still exercises the
handoff and safety path, but it does not establish that sustained Async is
compatible with that driver. Restore Frame Pacing to **Auto** after the test.

## What this probe does not prove

- It does not replace game-specific functional testing or verify a game's own
  swapchain and rendering behavior.
- A single-threaded run does not exercise the async handoff race; use
  `--threaded-wait` explicitly.
- The probe reports errors from its own OpenXR calls, while VectorXR's routing
  verdict remains in the corresponding `%LOCALAPPDATA%\VectorXR\logs` file.
- The probe does not change VectorXR settings, bindings, or runtime selection.

