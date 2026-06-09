# Native Quadviews Design Notes

Status: control plane, OpenXR stereo-runtime bridge, eye-gaze focus path, and swapchain lifecycle diagnostics.

## Research Snapshot

- The current OpenXR registry is at OpenXR 1.1.60. OpenXR 1.1 promotes the Varjo-style quad view configuration into `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET`, with `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO` as the same enumerant.
- The Khronos 1.1.60 registry lists `XR_EXT_eye_gaze_interaction`, `XR_EXT_view_configuration_views_change`, `XR_FB_foveation`, `XR_FB_foveation_configuration`, `XR_FB_foveation_vulkan`, `XR_HTC_foveation`, and `XR_META_foveation_eye_tracked`. Those are adjacent capabilities, not all direct substitutes for quadview emulation.
- `XrFoveatedViewConfigurationViewVARJO` documents the key sizing contract for foveated views: applications chain it to `XrViewConfigurationView`, set foveated rendering active, and use quad views when creating the instance.
- API layers are explicitly an OpenXR interception mechanism, so a native VectorXR quadviews engine should expose extension support through layer-owned interception rather than depending on a companion sidecar.

Sources:
- https://registry.khronos.org/OpenXR/
- https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_VARJO_quad_views.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFoveatedViewConfigurationViewVARJO.html
- https://github.com/KhronosGroup/OpenXR-SDK/releases

## Engine Contract

The UI and runtime resolver now provide `modules.quadviews` with:

- module enable flag
- eye-tracking preference
- default profile
- per-application profiles through the existing application registry
- tracking mode: `head` or `eye`
- focus horizontal and vertical FOV
- focus and peripheral resolution scale
- horizontal and vertical foveal offsets
- gaze smoothing and deadzone

The native engine should consume only resolved settings from `ResolvedRuntimeConfig::quadviews`.

## Runtime Architecture

The first native renderer path is implemented as a stereo-runtime bridge:

- The manifest advertises `XR_VARJO_quad_views` and `XR_VARJO_foveated_rendering`.
- `xrCreateApiLayerInstance` removes layer-owned extension names before calling downstream so runtimes do not reject unsupported Varjo extensions.
- `xrEnumerateViewConfigurations` adds `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET` when Quadviews resolves enabled and stereo is available.
- `xrGetViewConfigurationProperties`, `xrEnumerateEnvironmentBlendModes`, and `xrEnumerateViewConfigurationViews` map quadview app queries to stereo runtime queries.
- `xrBeginSession` maps an app-facing quadview session to a downstream stereo session.
- `xrLocateViews` maps quadview locates to stereo runtime locates, then synthesizes peripheral left/right and foveal inset left/right views.
- When the runtime advertises `XR_EXT_eye_gaze_interaction`, VectorXR enables it downstream for the layer, creates a private eye-gaze pose action, appends that action set to app action attach/sync calls, and uses the located gaze pose to move the foveal inset.
- `xrEndFrame` splits a 4-view projection layer into two 2-view projection layers: peripheral first, foveal second.
- `xrGetSystemProperties` reports Varjo foveated rendering support when the app chains `XrSystemFoveatedRenderingPropertiesVARJO`.
- `xrCreateSwapchain`, `xrEnumerateSwapchainImages`, `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`, and `xrDestroySwapchain` pass through transparently while recording lifecycle diagnostics for initial headset testing.

The remaining post-prototype work is mostly around live runtime adaptation and broader device coverage:

- optional `XR_EXT_view_configuration_views_change` event handling if we need live foveal-view resizing
- runtime/device compatibility warnings in the UI after we have real-world logs
- graphics-aware validation if testing reveals runtimes that reject the transparent projection-layer split

The manifest must statically advertise layer-owned extensions because the loader discovers API-layer extensions from manifest metadata. The runtime-facing quad view configuration is still gated by resolved `modules.quadviews.enabled`; if Quadviews is disabled for an app, VectorXR does not add the foveated-inset view configuration.

## Pivot Compatibility

Pivot and Quadviews should share one focus-direction pipeline:

1. Resolve raw gaze from `XR_EXT_eye_gaze_interaction` when available, attached, synced, and enabled in the Quadviews profile.
2. Fall back to runtime VIEW pose when eye gaze is unavailable or disabled.
3. Apply Quadviews smoothing/deadzone/offset.
4. Apply Pivot's effective extra yaw/pitch to the same focus direction.
5. Generate foveal inset FOV and poses from that final direction.

This avoids the current failure mode where Pivot rotates the world but the foveal high-resolution zone remains tied to the natural, unpivoted head direction.

## FOV Tweak Compatibility

Future FOV reduction should be another resolved module that transforms the final view FOVs after quadview view generation but before DepthXR convergence adjustments. That ordering keeps FOV reduction from changing the focus-window size math and keeps DepthXR's projection-center work last.

Proposed per-frame order:

1. Resolve raw stereo or quadview views.
2. Apply Quadviews view synthesis if active.
3. Apply Pivot pose/focus correction.
4. Apply future FOV reduction.
5. Apply DepthXR stereo boost and convergence.
6. Recompose projection layers in `xrEndFrame` when pose correction requires it.

## Implementation Phases

1. Control plane: complete.
2. Extension advertisement and instance negotiation: complete.
3. Quadview view-configuration enumeration and sizing: complete.
4. `xrLocateViews` head-tracked foveal FOV generation: complete.
5. `xrEndFrame` projection-layer split: complete.
6. Eye-gaze action setup and fallback: complete.
7. Swapchain lifecycle interception and diagnostics: complete.
8. Initial headset/runtime testing.
9. Runtime diagnostics and compatibility warnings in the UI.

## Initial Testing Checklist

For the first headset pass, enable Quadviews for one known app profile and check the VectorXR log for:

- `Enabled XR_EXT_eye_gaze_interaction downstream` when the runtime exposes eye gaze.
- `Quadviews view configuration capabilities` showing `runtimeStereo=1`, `synthesizeQuad=1`, `preferQuadFirst=1`, and `exposedTypes=[primary_stereo_with_foveated_inset,...]` on stereo-only runtimes.
- `Session began with view configuration: primary_stereo_with_foveated_inset (runtime mapped to primary_stereo)`.
- `Swapchain created` and `Swapchain imagesEnumerated` entries with `quadviewsSession=1`.
- `LocateViews` diagnostics with `viewConfig=primary_stereo_with_foveated_inset`.
- `EndFrame quadviews projection split applied` with `unknownProjectionSwapchains=0`.

If eye gaze is unavailable, the expected fallback log is `Eye gaze resources unavailable; quadviews will use head/static focus offsets.`
