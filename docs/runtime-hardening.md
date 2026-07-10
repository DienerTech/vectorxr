# Runtime Hardening Notes

This document records the compatibility boundaries introduced on `sol-review`.
The goal is to preserve VectorXR's existing Quadviews, Pivot, and Turbo behavior
while making unsupported paths explicit and frame ownership deterministic.

## Quadviews backends

- Native Varjo extensions are forwarded whenever the active runtime supports
  them, independently of whether a VectorXR Quadviews profile is enabled.
- VectorXR settings modify native Varjo view sizes and sharpening only when the
  resolved profile is enabled. Disabled profiles are transparent.
- Synthesized quadviews is exposed only to D3D11 OpenXR applications. Other
  graphics APIs pass through without advertising an emulation backend that
  cannot composite reliably.
- The D3D11 compositor preserves complete application context state on D3D11.1
  devices and retains the previous manual state path as a compatibility fallback.
- Swapchain acquisition/wait/release tracking follows FIFO ordering. Array
  swapchains are flattened one selected slice at a time through the existing
  copy fallback, and submitted subimage rectangles are respected by the shader.

## Turbo lifecycle

- Async pacing uses one persistent worker. The one-frame overlap and the
  existing Async/Sequenced behavior are unchanged.
- `xrEndSession` is intercepted. Any frame pre-owned by Turbo is balanced before
  the downstream session end, and frame/tracking state is reset before restart.
- Runtime pacing observations are keyed by runtime, headset system/vendor, and
  graphics API. This prevents a SteamVR verdict learned through one headset
  driver from being applied blindly to another.

## Pivot and gaze

- Pivot normally derives eye offsets from the runtime views already located for
  the frame. The extra runtime `xrLocateViews` remains as a validity fallback.
- Eye-gaze focus holds the last valid sample across short dropouts and then eases
  back to center instead of snapping on a blink.

## Deliberate remaining boundaries

- D3D12 and Vulkan synthesized-quadviews backends are not yet implemented.
- VectorXR still owns one instance/session state object. Additional instances or
  sessions are rejected safely instead of overwriting the active dispatch state;
  a full handle-to-instance dispatch map is future architectural work.
- A deterministic mock OpenXR runtime should be added for end-to-end frame-loop
  ordering tests. Pure swapchain FIFO behavior is covered in the current native
  test executable.
