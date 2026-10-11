# Real-runtime hardware probe

`vectorxr_hardware_probe` is an opt-in developer executable for exercising
VectorXR against the active OpenXR runtime and headset without relying on a
game to use a particular frame loop or reference-space arrangement. It is not
included in the VectorXR installer or the default CMake build.

The probe renders an asymmetric line scene through Direct3D 11: a ground
grid, colored axes, and two differently placed beacons. With `--graphics
vulkan` it renders through Vulkan instead (see [Vulkan mode](#vulkan-mode));
the line scene is Direct3D 11 only, so a Vulkan run shows each eye's clear
color and, with `--pattern`, the test pattern. Unless `--full-size` or
`--quad-views` is given, each eye is limited to 1280 pixels in either
dimension so the diagnostic does not allocate the headset's full recommended
render size.

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

The Vulkan mode's GPU load is compiled to SPIR-V at build time by the
pinned DXC release the main build fetches (`DEPTHXR_FRAMEGEN_SPIRV`, on by
default); without it the Vulkan mode runs without `--gpu-ms`.

The non-headset check of the pose math, option parsing, pattern geometry and
GPU load arithmetic can run in CI or from a desktop session:

```powershell
.\build\hardware-probe\layer\RelWithDebInfo\vectorxr_hardware_probe.exe --self-test
```

Two further checks need the desktop's GPU but no OpenXR runtime or headset:

- `--gpu-calibrate` checks the GPU load (see below).
- `--render-check` draws the probe's swapchain shapes (side by side with a
  depth swapchain, one swapchain per view, the quad layer) into images the
  probe makes itself, in the layout OpenXR hands swapchain images over in,
  for ten frames, twice with the swapchains recreated in between. It reads
  the pixels back, checks the clear colors, the pattern and (Vulkan) the
  depth, destroys its device, and fails on any debug layer or validation
  message, including objects validation finds undestroyed at
  `vkDestroyDevice`. On a machine that lacks what the check needs
  (`vulkan-1.dll`, a hardware GPU, the Direct3D 11 SDK layers of the Graphics
  Tools optional feature for `--d3d11-debug`, or the validation layer for
  `--vulkan-validation`) it exits with 77, which CTest reports as skipped;
  with `VECTORXR_TESTS_REQUIRE_HW=1` (the build's `DEPTHXR_REQUIRE_HW_TESTS`)
  it fails instead:

```powershell
$probe = '.\build\hardware-probe\layer\RelWithDebInfo\vectorxr_hardware_probe.exe'
& $probe --render-check --d3d11-debug
& $probe --render-check --graphics vulkan --vulkan-validation
& $probe --render-check --graphics vulkan --vulkan-validation --vk-late-submit --vulkan-api 1.0
```

With `DEPTHXR_BUILD_TESTS`, the probe's build tree runs the self-test and
these three render checks as CTests, and checks of the checks: a run with
`--vk-test-leak` (one `VkFence` left undestroyed) must fail on validation's
report of it, a run with `--vk-test-stuck-queue` (the queue held by a wait
that never ends, as a stuck Frame Generation bridge would hold it) must end
with an error within its 10-second waits instead of hanging (the check then
signals the wait from the CPU, so the process never exits with its queue
held), and a run without the validation layer must be skipped.

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
2. Choose **Turbo > Runtime Behavior > Mode > Async — always** in VectorXR, enable Turbo
   for the probe application, and save the settings. If a Safety block exists, inspect it
   on **Turbo Safety** and clear it before relaunching the probe.
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
compatible with that driver. Restore **Runtime Behavior > Mode** to **Auto (recommended)**
and save after the test.

## Vulkan mode

`--graphics vulkan` makes the probe a Vulkan game: the same frame loop,
threaded wait, pacing, GPU load, depth, swapchain recreation and sessions,
with Vulkan swapchains. It is stereo only (`--quad-views` needs Direct3D 11,
whose sessions are the only ones VectorXR composes quad views for).

| Option | Effect |
| --- | --- |
| `--graphics d3d11\|vulkan` | The graphics API (default: `d3d11`). |
| `--vulkan-enable 1\|2` | `XR_KHR_vulkan_enable2` (default): the runtime creates the `VkInstance` and `VkDevice` from the probe's create infos. `1`: `XR_KHR_vulkan_enable`: the probe creates them with the extensions the runtime names, and prints those names. |
| `--vulkan-api 1.0\|1.1\|1.2\|1.3` | The instance's API version (default 1.2), checked against the runtime's minimum and the device's version. The probe uses Vulkan 1.0 entry points only. |
| `--vulkan-validation` | The Khronos validation layer and a debug-utils messenger; its errors and validation warnings are reported at the end, as `--d3d11-debug` reports the Direct3D 11 debug layer's. The report comes after the swapchains, the session, the OpenXR instance and the Vulkan device are destroyed, so it includes what their teardown did (Frame Generation's bridge tears down in `xrDestroySession`) and the objects validation finds undestroyed at `vkDestroyDevice`. Without OpenXR (`--gpu-calibrate`, `--render-check`) synchronization validation is on too, and any message fails the check. |
| `--vk-late-submit` | Each view's image is released first, and the frame's rendering is submitted only after the last release, before `xrEndFrame`. |

Each view is a render pass over the view's pixels: its load clears the view
(and its depth), and the pattern's rectangles are cleared with
`vkCmdClearAttachments`, the same rectangles and colors as the Direct3D 11
pattern. With `--depth`, the pattern is also cleared into the depth
swapchain at 2 m, so the runtime and VectorXR receive depth with edges. The
swapchain images stay in the layout OpenXR hands them over in, and a view's
rendering (and its GPU load share) is submitted before its image is released.
`--vk-late-submit` breaks that rule on purpose: it shows what VectorXR does
with a game that submits its rendering after `xrReleaseSwapchainImage`.

The probe loads the validation layer from `VK_ADD_LAYER_PATH`, else from the
copy the build tree's test configuration makes (`layer\vulkan_layers`, with
`DEPTHXR_VULKAN_VALIDATION_LAYER` or the Vulkan SDK). With validation on, the
runtime's own Vulkan calls on the probe's device are validated too, so a
message may be the runtime's. Implicit Vulkan layers (overlays, capture tools)
can be kept out with `VK_LOADER_LAYERS_DISABLE=~implicit~`.

Before the probe destroys swapchains (`--recreate-seconds`, `--sessions`, the
end) it waits until its queue is idle, for at most 10 seconds; the Vulkan
device outlives the session and the OpenXR instance. A queue that is not idle
by then (for example one waiting on a Frame Generation bridge's semaphore that
is never signaled) is reported with "the queue did not finish within 10 s
before ...", and the objects it may still use are left undestroyed rather
than destroyed under it, so the run ends with an error instead of hanging.

To run the probe against the runtime without the installed VectorXR layer,
`DISABLE_XR_APILAYER_DIENERTECH_VECTORXR=1` disables it for the probe's
process only. A build of the layer elsewhere can then be loaded as an
explicit layer with `XR_API_LAYER_PATH` (its folder) and
`XR_ENABLE_API_LAYERS=XR_APILAYER_DIENERTECH_VECTORXR`:

```powershell
$env:DISABLE_XR_APILAYER_DIENERTECH_VECTORXR = "1"
& $probe --graphics vulkan --vulkan-enable 1 --pattern --depth --seconds 20
& $probe --graphics vulkan --pattern --side-by-side --quad-layer --minimal-usage --vulkan-validation --seconds 20
```

## X-Plane's swapchain shapes

Three options, for either graphics API, give the probe the shapes X-Plane
gives its swapchains:

| Option | Effect |
| --- | --- |
| `--side-by-side` | One double-wide color swapchain (and one double-wide depth swapchain with `--depth`): each frame acquires it once, both eyes render into their half, and each projection view's `imageRect` names its half. Stereo only. |
| `--quad-layer` | A 2048x2048 quad layer swapchain (a panel a meter ahead of where the session started), re-rendered and released every frame and submitted after the projection layer, alpha-blended. |
| `--minimal-usage` | The color swapchains are created with usage 0 (depth swapchains keep the depth usage). |

## Frame Generation and the Direct3D 11 bridge

With Frame Generation enabled for the probe, its session goes through the
Direct3D 11 bridge (the runtime's session is Direct3D 12, and the probe
renders into images shared with it), as DCS's does. Further options shape the
probe like DCS, or stress the bridge:

| Option | Effect |
| --- | --- |
| `--quad-views` | Four views (`XR_VARJO_quad_views`) in four `color\|sampled` swapchains at full size, as DCS renders them; VectorXR composes them where the runtime lacks quad views. |
| `--full-size` | Recommended view sizes, without the 1280-pixel limit (implied by `--quad-views`). |
| `--pattern` | A grid and a sweeping bar fixed to each view (focus views in other colors), visible wherever the headset points, and motion for Frame Generation. |
| `--frame-ms N` | Each frame takes at least N ms (a game slower than the refresh rate). |
| `--gpu-ms N` | Each frame takes N ms (0-100) of GPU work, split between the views by pixel count (see [GPU load](#gpu-load)). |
| `--gpu-frames-ahead N` | At most N frames' GPU work is queued (0-4, default 2): each frame waits until the one N before it has run on the GPU; 0 waits for the frame's own work before `xrEndFrame`. |
| `--gpu-ms-fixed` | The calibrated GPU work stays fixed, so work competing for the GPU shows as a longer load. By default the load is re-measured every 30 frames to hold N ms. |
| `--gpu-chunks N` | Each view's share of `--gpu-ms` runs as N dispatches of equal size (1-256, default 1): the same GPU time, in many short dispatches as a game's frame is, instead of one long one. |
| `--gpu-calibrate` | No OpenXR: calibrates `--gpu-ms` on the default adapter, runs 200 frames of the load alone and reports what they measured. |
| `--depth` | Depth rendered into depth swapchains and submitted (`XR_KHR_composition_layer_depth`). |
| `--recreate-seconds N` | The swapchains are destroyed and recreated every N seconds. |
| `--sessions N` | N sessions one after another in one instance. |
| `--hitch-every N`, `--hitch-ms M` | Every N seconds one frame takes M ms longer (a stutter). |
| `--d3d11-debug` | The Direct3D 11 device has the debug layer; its errors and warnings are reported at the end. |
| `--vram-ballast-mb N` | N MB (0-131072) of video memory held on the session's adapter from before the session to the end of the run, so Frame Generation has as little room as on a card with less memory (see [VRAM ballast](#vram-ballast)). |
| `--graphics vulkan` and its options | See [Vulkan mode](#vulkan-mode). |
| `--side-by-side`, `--quad-layer`, `--minimal-usage` | See [X-Plane's swapchain shapes](#x-planes-swapchain-shapes). |

The probe reports its frame interval and the time its swapchain calls and
`xrEndFrame` took. Keep the probe's settings apart from your own with
`VECTORXR_CONFIG_PATH` (a copy of `settings.json` with Frame Generation and
Quadviews profiles for `vectorxr-hardware-probe`), and its log with
`VECTORXR_LOG_PATH`:

```powershell
$env:VECTORXR_CONFIG_PATH = "$env:TEMP\probe-settings.json"
$env:VECTORXR_LOG_PATH = "$env:TEMP\probe.log"
& $probe --mode source-exact --quad-views --pattern --threaded-wait --frame-ms 15 --seconds 60
```

Expected log evidence: "XR_KHR_D3D12_enable added to the instance's
extensions", "the session is Direct3D 12 and the game renders into images
shared with it", one "is shared" line per swapchain, "Frame Generation
enabled for this session: backend=D3D12", and at the end the bridge's
summary with `skippedCopies=0` and `unmatchedReleases=0`.
`VECTORXR_D3D11_BRIDGE_DEBUG=1` also enables the Direct3D 12 debug layer and
reports what it finds in the log; `VECTORXR_D3D11_BRIDGE=off` keeps the
session Direct3D 11.

An idle headset may make the runtime refresh far slower than it reports;
timings from such a run do not stand for a worn headset's.

### GPU load

`--frame-ms` makes the game slow on the CPU; `--gpu-ms N` makes it slow on
the GPU, as DCS usually is. Before the session the probe calibrates a compute
shader with timestamp queries until a frame of it, split as the views split
it, takes N ms (within 3%), and prints:

```text
GPU load: target 11.00 ms = 903078 iterations in 4 views (measured 11.08 ms)
```

Each frame then runs each view's share of the load right before the view
renders, so the view's release, and whatever VectorXR queues behind it, waits
for it on the GPU. A signal per frame (an event query in Direct3D 11, a fence
in Vulkan, where the share is recorded into the command buffer the view
renders with) stands for a game's own frame queue (`--gpu-frames-ahead`). At
the end the probe reports the load each frame measured and how often and how
long the frame queue held the probe:

```text
GPU load: target 11.00 ms, mean 11.01 ms, p50 10.89 ms, p99 12.10 ms (5400 frames timed), iterations 892566 (calibrated 903078, 180 adjustments)
GPU frames ahead 2: at most 1 earlier frames queued when a frame began, 67 stalls of 5400 frames, mean 2.62 ms, max 4.52 ms, total 175.67 ms
```

Measure Frame Generation's pacing against a game that is near the refresh
rate and against one well below it:

```powershell
& $probe --mode source-exact --quad-views --pattern --threaded-wait --gpu-ms 11 --seconds 60
& $probe --mode source-exact --quad-views --pattern --threaded-wait --gpu-ms 20 --seconds 60
```

Without Frame Generation, at 90 Hz, the first runs at about 80 fps, near the
refresh rate, and the second at about 45-50 fps, well below it: the two cases
Frame Generation's Dynamic multiplier must tell apart. Compare
`--gpu-frames-ahead` 0, 1 and 2 to see how far a game's GPU queue changes the
result.

By default each view's share is one long dispatch. A game's frame is many
short draws and dispatches instead, between which the GPU can run other work,
such as Frame Generation's own analysis of the frames. One dispatch of many
milliseconds per view can keep that work waiting and leave Frame Generation
with almost nothing to show. For Frame Generation checks, split the load with
`--gpu-chunks`, which keeps its total time:

```powershell
& $probe --mode source-exact --quad-views --pattern --threaded-wait --gpu-ms 20 --gpu-chunks 40 --seconds 60
```

Other GPU work (the runtime's compositor, Frame Generation, other
applications) can lengthen the load. By default the probe re-measures it
every 30 frames and adjusts the work by at most 10% a step, so the game's
frames keep taking about N ms; with `--gpu-ms-fixed` the work stays as
calibrated and the competition shows as a longer load.

To check the load without a headset:

```powershell
& $probe --gpu-calibrate --gpu-ms 11
& $probe --gpu-calibrate --gpu-ms 11 --graphics vulkan --vulkan-validation
& $probe --gpu-calibrate --gpu-ms 20 --gpu-chunks 40
```

It paces 200 frames at 90 Hz (or `--frame-ms`, when longer), split into two
views (four with `--quad-views`) and `--gpu-chunks` dispatches each, reports
as above, and exits with an error
when the frames' mean is more than 3% off the target or a frame began with
more frames queued than `--gpu-frames-ahead` allows (and, for Vulkan with
`--vulkan-validation`, on any validation message). Direct3D 11 uses the
default adapter, Vulkan the first discrete GPU (or the adapter
`FG_TEST_ADAPTER` names, see [Choosing the GPU](#choosing-the-gpu)). Other
applications using the GPU at the same time add spikes to the p99.

### VRAM ballast

Frame Generation sizes itself by the video memory its process uses against
the budget Windows gives the process on that adapter (DXGI's
`QueryVideoMemoryInfo`, local segment group). In a stereo session that
generates just in time, it keeps the last five real frames instead of three
only while the two extra frames and a tenth of the budget still fit. Where
each generated frame needs output images of its own, it generates fewer
frames per real frame once the usage comes within 5% of the budget. On a
large card neither limit comes near.

`--vram-ballast-mb N` allocates N MB (1 MB = 1,048,576 bytes, as Frame
Generation's log counts them) of committed Direct3D 12 buffers in the default
heap on the session's adapter, after the graphics device is created and
before the session, and holds them until the session has ended. Committed
resources are resident from their creation, so the process's usage rises by
about N MB, and Frame Generation, which counts the whole process's usage, has
that much less room left, as it would with a smaller card. The probe prints
the usage and budget before and after the allocation, and again at the end of
the run, before the session ends (Frame Generation's images included):

```text
Video memory before the VRAM ballast: usage 410 MB, budget 30400 MB (29990 MB to spare; this process, local segment group)
VRAM ballast: 17408 MB in 68 committed buffers on "NVIDIA GeForce RTX 5090"; the process's usage rose by 17410 MB
Video memory with the VRAM ballast: usage 17820 MB, budget 30400 MB (12580 MB to spare; this process, local segment group)
```

Both of Frame Generation's limits are fractions of the budget, so to stand
in for a card whose budget is `B_small` on one whose budget is `B` (the
budget the probe prints), use N of about 0.9 x (`B` - `B_small`) MB; a 12 GB
card's budget is somewhat under 12 GB. For example:

```powershell
& $probe --mode source-exact --quad-views --pattern --threaded-wait --gpu-ms 20 --gpu-chunks 40 --vram-ballast-mb 17408 --seconds 60
```

What the ballast does not simulate:

- The budget. Windows assigns it from the card's memory and from what other
  processes use; the probe's figures and Frame Generation's margins (a tenth
  and a twentieth of the budget) stay the large card's.
- Other processes. On a small card the runtime's compositor, the desktop and
  other applications use the same memory and shrink the budget; the ballast
  only adds to this process's usage.
- Going over the budget. Then Windows may move allocations, the ballast's
  among them, to system memory, which lowers the usage again instead of
  showing what a small card would do. Keep the ballast, the game and Frame
  Generation under the budget; the line at the end of the run shows whether
  they stayed there.
- A smaller card's bandwidth and speed.

On an integrated GPU the local segment group is system memory shared with
the processor, so the ballast takes system memory.

The ballast is for an OpenXR session only (not `--render-check` or
`--gpu-calibrate`); with `--graphics vulkan` it needs `--vulkan-api` 1.1 or
above, for the device's adapter LUID. It cannot run with
`VECTORXR_D3D11_BRIDGE_DEBUG=1` or `VECTORXR_VULKAN_BRIDGE_DEBUG=1`: Direct3D
12 keeps one device per adapter in a process, so the ballast's device is the
one the bridge uses, and the bridge enabling the debug layer once that device
exists would remove it.

## Choosing the GPU

On a machine with more than one GPU (for example a processor's integrated AMD
GPU beside an NVIDIA card), `FG_TEST_ADAPTER` picks the one the GPU tests and
the probes run on. Its value is matched, ignoring case and the spaces around
it, against each adapter's name (the DXGI description for Direct3D, the
`deviceName` for Vulkan), and the first hardware adapter whose name contains
it is used. Unset or empty, nothing changes.

```powershell
$env:FG_TEST_ADAPTER = "radeon"
```

| Run | Without `FG_TEST_ADAPTER` | When it names no adapter |
| --- | --- | --- |
| `depthxr_framegen_tests` on Direct3D hardware (`hw-d3d12`, `hw-d3d11`, `bench-d3d12`, `bench-d3d11`, `profile`, `contention`, `replay`, `nvof`) | Direct3D 12: the first hardware adapter that runs it; Direct3D 11: the default adapter | Fails |
| `depthxr_framegen_tests vulkan` (and `hw-vulkan`, `bench-vulkan`), `depthxr_osd_backend_tests vulkan` | The first Vulkan device | Skipped (77) |
| `depthxr_d3d11_bridge_tests hw`, `depthxr_d3d11_bridge_layer_tests hw` | The first adapter | Skipped (77) |
| `depthxr_vulkan_bridge_tests`, `depthxr_vulkan_bridge_layer_tests` | The first GPU whose Vulkan device and Direct3D 12 adapter are the same | Skipped (77); fails with `VECTORXR_TESTS_REQUIRE_HW=1` |
| `depthxr_bridge_probe` | The first high-performance hardware adapter | Aborts |
| `vectorxr_hardware_probe --render-check` | The default adapter (Vulkan: the first discrete GPU) | Skipped (77); fails with `VECTORXR_TESTS_REQUIRE_HW=1` |
| `vectorxr_hardware_probe --gpu-calibrate` | The same | Fails |

The message lists the adapters there are. The suites that run on WARP are not
affected. In the layer tests only the test's own devices follow
`FG_TEST_ADAPTER`; VectorXR takes the adapter of the game's device, as it does
in a session.

An OpenXR session renders on the adapter the runtime asks for, so the probe
cannot choose one there. With `FG_TEST_ADAPTER` set, a session run checks the
runtime's adapter against it and stops with an error when they differ, rather
than run on a GPU other than the one asked for.

## What this probe does not prove

- It does not replace game-specific functional testing or verify a game's own
  swapchain and rendering behavior. The X-Plane shapes are a stand-in for
  X-Plane's swapchains, not a verified copy of them.
- The Vulkan mode does not draw the line scene; use Direct3D 11 for the
  Pivot routing checks above.
- A single-threaded run does not exercise the async handoff race; use
  `--threaded-wait` explicitly.
- The probe reports errors from its own OpenXR calls, while VectorXR's routing
  verdict remains in the corresponding `%LOCALAPPDATA%\VectorXR\logs` file.
- The probe does not change VectorXR settings, bindings, or runtime selection.
