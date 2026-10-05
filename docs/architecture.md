# Architecture

## Goals

VectorXR is organized as one shared OpenXR runtime pipeline with product-level feature separation in the app and config.

Design rules:

- keep OpenXR interception separate from feature math
- keep config parsing independent from the desktop app
- keep suite settings separate from feature settings
- let features compose through shared runtime state instead of isolated runtime stacks
- keep runtime logging useful in debug scenarios but quiet by default

## Product Structure

The app presents product modules:

- core suite controls (`core`)
- `Depth` feature editing (internal id `depthxr`)
- `Pivot` feature editing (internal id `pivotxr`)
- `Quadviews` feature editing
- `Turbo` frame pacing, runtime strategy selection, diagnostics, and Safety controls
- `Frame Generation` optical-flow frame synthesis (internal id `framegen`, experimental)

This "module" concept is a product and config boundary, not a separate runtime ownership boundary. User-facing labels are display-first (`Depth`, `Pivot`); internal ids, config keys, and C++ namespaces stay `depthxr`/`pivotxr`.

## Runtime Structure

### Shared core

Shared core code under `layer/` owns:

- config model and parser
- settings resolution and per-application profile matching
- path resolution
- process name resolution
- logger abstraction
- feature math (Depth stereo boost/convergence/submission restore, Pivot yaw/pitch recomposition, Quadviews composition)

### OpenXR layer

The Windows DLL owns:

- loader negotiation entry point
- API layer instance creation
- function dispatch
- OpenXR function pointer capture
- interception of the view/pose, session, swapchain, and view-configuration calls the features need

### App

The Tauri app edits the same VectorXR config file used by the layer.

The app owns:

- suite navigation
- validation
- working-copy save flow
- feature-specific editing surfaces

## Interception surface

Core entry points:

- `xrNegotiateLoaderApiLayerInterface`
- `xrCreateApiLayerInstance`
- `xrGetInstanceProcAddr`
- `xrDestroyInstance`

Feature interception (varies by which modules are active):

- `xrBeginSession`, `xrCreateSession`, `xrDestroySession`
- `xrLocateViews` — Depth stereo boost and convergence
- `xrEndFrame` — Pivot correction, optional Depth Anchor geometry restore, and Quadviews submission composition
- `xrLocateSpace` — Pivot yaw/pitch recomposition
- `xrEnumerateViewConfigurations` / `xrEnumerateViewConfigurationViews` and the swapchain
  lifecycle calls — Quadviews composition
- `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` — Turbo pacing, or Frame Generation pacing in
  sessions that start with Frame Generation on; swapchain creation and release also tell
  Frame Generation which image each projection view submitted

## Frame Generation

Frame Generation is latched per session in `xrCreateSession`. When it owns pacing,
`FrameGenPresenter` (`layer/src/framegen/framegen_presenter.cpp`) takes the application's
frame calls:

- At multiplier *N* (2x, 3x, 4x, or chosen automatically), the app's `xrWaitFrame` waits on
  runtime slot *k* but reports slot *k + N - 1* with an *N*-fold period, so the app renders
  for the time its frame is shown and runs at 1/*N* of the refresh rate.
- The app's `xrEndFrame` submits slots *k* to *k + N - 2* with generated frames, each after an
  extra runtime wait/begin, then slot *k + N - 1* with the app's own layers. Every submission
  goes through the layer's normal final submission, so it carries the OSD layer. Each slot is
  held until the runtime would have released its wait, measured from the last runtime wait
  that blocked, and at least most of a refresh after the previous submission, because some
  runtimes return waits inside a cycle at once and would show only the newest frame.
- Auto steps the multiplier up when more than 30% of the last second's app frames miss the
  cadence, and probes one step down after five steady seconds, doubling the wait between
  probes (10 s to 80 s) each time a probe fails. At 1x it passes frames straight through.
  When the late frames are inline cycles whose first slot was already past due, a higher
  multiplier would only hold the game's thread longer, so Auto drops to full rate for a
  minute instead.
- Threaded presentation (Direct3D 12, Extrapolate): the inline cycle above holds the game's
  frame thread for the generated slots and holds its next `xrWaitFrame` behind them, which
  serialises games that overlap frames (MSFS waits for frame *N + 1* while rendering frame
  *N*). Where the graphics queue is free-threaded, a presenter thread instead owns the
  runtime frame loop: one runtime wait/begin/end per refresh. The app's `xrWaitFrame` never
  reaches the runtime; it is released every *N*th slot the presenter thread publishes, and
  predicts the slot the frame will really reach from the app's measured wait-to-end time.
  The app's `xrBeginFrame` is a no-op. Its `xrEndFrame` asks for the first slot at or after
  that one, runs the runtime begin/end for it with the app's own layers, then generates the
  extrapolated frames for the following slots. Between real frames the presenter thread
  submits those, composed with a deep copy of the newest real frame's other layers, or that
  frame again until they are ready. Leaving the mode drains frames in flight; session
  changes stop the thread and drop frames that have not started ending. Games whose layers
  cannot be copied (unknown layer types) stay inline.
- A wait that arrives while an engaged frame is still ending is held, so runtime frame calls
  never overlap even for applications that wait on one thread while ending on another.
- Turbo yields for the session; the two features pace frames in opposite directions.

Generation runs on the session's own device and queue. An API-neutral plan
(`layer/src/framegen_plan.cpp`) describes every pass, and the Direct3D 11, Direct3D 12 and
Vulkan backends (`layer/src/framegen/`) only record copies, barriers and compute dispatches
for it. The passes live in one HLSL file (`layer/shaders/framegen.hlsl`): fxc builds DXBC for
both Direct3D backends and a pinned DXC release builds SPIR-V for Vulkan at build time.

Per view, the newest submitted image is copied into a history texture; luma pyramids are built
for it and for the previous frame rotated into the newest view (head rotation is the runtime's
job, so it is removed before matching); block matching runs coarse to fine; and synthesis either
interpolates between the two real frames or extrapolates past the newest, writing into a
layer-owned output swapchain. Direct3D 12 draws all of a view's generated frames straight into
their output images in one draw (a full-screen pixel shader with a render target per frame, so
the frames share their flow reads and sRGB is encoded by the render target); Direct3D 11 and
Vulkan write them by compute into an intermediate image and copy it.

`depthxr_framegen_tests profile [width height]` renders a stereo cockpit with depth at headset
size and prints the GPU time of every pass on the local GPU (Direct3D 12).

With Motion estimation set to hardware, Direct3D 12 measures motion with NVIDIA's optical flow
accelerator instead of block matching. It is built only when the NVIDIA Optical Flow SDK
headers are unpacked under `libraries/`; `nvofapi64.dll` comes with the driver. Each view has
a session that matches its half-resolution luma pair with a 4x4 grid (flow level 3) and keeps
its own temporal hints. The frame's command list is split:

1. The trust, luma and pack passes for every view.
2. The submission signals a fence the accelerator waits on.
3. The queue waits for the accelerator's fence.
4. Unpack (the vectors with our block cost as their confidence), filter and synthesis.

That wait would stall the game's GPU work behind the accelerator for its whole run (about 7 ms
at headset resolution), and the first generated frame would miss its refresh. So once the
accelerator is in use, every color capture at release also packs the frame's raw luma
(`kFlowPackRaw`, head movement and all) and starts the accelerator on it against the previous
capture's. The accelerator then runs while the game finishes its frame. Generation uses those
vectors when the previous capture was the frame it last generated from. The unpack maps the
previous position each vector points at back into the newest view (the inverse of the
head-movement mapping, with the trusted depth), which takes the head movement out. Otherwise it
packs and matches as above.

`depthxr_framegen_tests nvof` compares its vectors with ours on the scenarios' scenes and times
it at headset size. Generation works on whatever projection views reach the runtime,
so it follows Quadviews composition and Varjo-native quad views alike.

When a game submits depth with its projection views (`XR_KHR_composition_layer_depth`) and the
backend can read it (Direct3D 12 so far), the depth image is converted to linear view depth at
release, next to the color capture, and the previous frame is mapped into the newest view by the
full relative head pose instead of rotation alone. Content fixed in the head's reference space,
such as a cockpit, then lines up exactly and the flow carries only motion relative to that space.
Generated frames also keep the real frame's depth chain, so the runtime corrects head motion on
every frame alike.

Depth is only used once it is shown to be read correctly. A check pass warps view 0 into view 1
by the depth and the eyes' relative pose, over the samples both eyes see, and scores the depth
as decoded and with near and far swapped against no depth; `FrameGenDepthValidator` adopts a
reading that clearly lines the eyes up, and drops depth only once it no longer helps at all.

Each frame, before any view uses it, every depth texel gets a trust score in [-1, 1]:

- the other eye's evidence, where both eyes see the texel: does its content line up at the
  depth, or as infinitely far (scenery behind cockpit glass written to depth)?
- the previous frame's evidence: head movement between the two frames moves content at its depth
  differently from infinitely far content, so cockpit detail is confirmed even where one eye sees
  it alone;
- last frame's score at the texel's previous position (cockpit and glass stay put in the cockpit),
  so the score is steady and decisions carry across the view as the head turns.

Texels scored below -0.25 are negated and compensated for rotation only. Texels above +0.25 move
with the eye. Runtimes that only rotate submitted frames (Pimax among them) never correct head
translation, so a generated frame shown 11-33 ms after its real frame would show near cockpit
content from where the eye was, and that content would jump at the real frame rate. The presenter
asks the runtime where each eye will be at every generated frame's display time
(`FrameGenRuntimeCalls::locate`, in the eye's own view space). Synthesis then reads an eye-shift
map built per view:

1. Every half-resolution texel splats onto the texels around where it lands from the new eye
   position, with an atomic max on its inverse depth, so nearer content covers what it moves over.
2. The winners write the offset back to their source.
3. Texels nothing reached (content the movement uncovers) stretch the farther neighbour along the
   line content moves on.

The map is built for the generated frame furthest in time; the others scale it. Generated frames
are submitted with the eye's predicted position. Trusted cockpit content also drops sub-pixel
motion as matching noise.

When extrapolating, the filter pass also rates how steady each motion vector is against the
vector the same content had one real frame pair earlier, scaled by the ratio of the two pairs'
intervals; synthesis extrapolates unsteady motion less, so matching noise on still detail does
not make it jitter.

The interception set is intentionally scoped to what the enabled features need rather than
hooking every call.
