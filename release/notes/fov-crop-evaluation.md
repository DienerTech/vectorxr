# FOV / Performance Crop Evaluation

Date: 2026-08-01

Status: evaluation only; no implementation has been approved or made.

## Summary

VectorXR can implement a real performance-oriented field-of-view crop through its
OpenXR API layer, but the layer does not directly remove draw calls or cull game
content. It can instead report a narrower per-eye projection and a proportionally
smaller recommended image rectangle. A cooperating game then builds a smaller
frustum, may cull more scene content, and renders fewer pixels.

The feature can therefore produce meaningful GPU savings, especially in wide-FOV
cockpit games, but it cannot guarantee an FPS improvement. Results depend on the
application honoring OpenXR's view recommendations, the engine's culling behavior,
the runtime accepting mutable FOV submissions, and the application being GPU-bound.

## What the OpenXR layer can do

A performance crop needs two coordinated changes:

1. Intercept `xrLocateViews` and narrow each returned `XrFovf` in tangent space.
2. Intercept `xrEnumerateViewConfigurationViews` and reduce
   `recommendedImageRectWidth` and `recommendedImageRectHeight` in proportion to
   the retained horizontal and vertical projection spans.

The first change gives the application a smaller projection frustum. The second is
what encourages it to allocate smaller swapchains and render fewer pixels. Merely
narrowing the FOV while retaining the original render-target size concentrates the
same number of pixels into a smaller angular area and normally provides little
fill-rate benefit.

VectorXR already intercepts the required calls and tracks the related state:

- `xrGetViewConfigurationProperties`
- `xrEnumerateViewConfigurationViews`
- `xrLocateViews`
- swapchain creation and lifecycle calls
- `xrEndFrame` projection submissions
- `xrGetVisibilityMaskKHR`

No graphics-API-specific image processing is required for the cooperative path.
The application renders directly into the smaller swapchain and submits the cropped
projection to the runtime.

## Expected performance effects

| Work | Expected effect |
| --- | --- |
| Pixel/fragment shading | Largest and most reliable saving when render-target dimensions shrink |
| Resolution-dependent post-processing | Often significantly reduced |
| GPU bandwidth and swapchain memory | Reduced |
| Geometry processing | Reduced only where the engine culls against the narrower frustum |
| Render-thread CPU | May improve modestly if fewer objects and draw calls survive culling |
| Simulation/game-thread CPU | Essentially unchanged |
| Fixed frame overhead | Unchanged |
| Runtime composition | Runtime-dependent; may improve with a smaller submitted image rectangle |

An API layer cannot command an engine to omit particular objects. Some engines use
conservative stereo-union frustums, cache visibility, or render broad object lists,
so CPU and vertex savings may be small or absent even when pixel savings are real.
The feature is most useful when the game is limited by pixel shading, post-processing,
or bandwidth rather than simulation or submission CPU time.

Published reports from other FOV-crop layers show substantial gains in individual
wide-FOV cockpit configurations, but those measurements are not portable guarantees.
The maintained `OpenXR-Layer-crop-fov` project, for example, couples narrowed FOV with
reduced swapchain recommendations and reports application-specific GPU savings:

https://github.com/mledour/OpenXR-Layer-crop-fov

## Projection math

Cropping should be performed in tangent space rather than by multiplying the four
angles. For an original horizontal projection:

```text
left  = tan(angleLeft)
right = tan(angleRight)
span  = right - left

croppedLeft  = left  + leftCropFraction  * span
croppedRight = right - rightCropFraction * span
```

The vertical boundaries use the same construction. The cropped tangent values are
converted back with `atan`. This preserves asymmetric projections and gives crop
percentages a consistent image-space meaning on canted and wide-FOV headsets.

The approximate retained pixel fraction is:

```text
(1 - leftCrop - rightCrop) * (1 - topCrop - bottomCrop)
```

This is a pixel-budget estimate, not an expected FPS percentage. Fixed work and
non-pixel-bound stages remain.

## Runtime and standards constraint

`XrViewConfigurationProperties::fovMutable` indicates whether an application may
submit a modified FOV. A portable VectorXR implementation should query and preserve
the runtime's value, enable crop only when it is `XR_TRUE`, and report an unsupported
runtime when it is `XR_FALSE`.

VectorXR should not claim mutable FOV support on behalf of an immutable runtime.
Forcing a cropped submission may work on particular runtimes but would be outside a
sound cross-runtime contract and could cause rejection, distortion, or incorrect
reprojection.

An immutable-FOV fallback would require VectorXR to create a native full-FOV output
swapchain, render black margins, and place the cropped application image into the
correct angular subregion before submission. That path would be graphics-API-specific,
would add a full-screen composition pass, and would reduce the value of the feature.
It is not recommended for an initial implementation.

Relevant OpenXR references:

- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrView.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrViewConfigurationView.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrViewConfigurationProperties.html

## Application cooperation and diagnostics

`recommendedImageRectWidth` and `recommendedImageRectHeight` are recommendations,
not mandatory allocation sizes. Applications may use custom dimensions or ignore a
changed recommendation. VectorXR should therefore compare observed `xrCreateSwapchain`
dimensions against the cropped recommendation and expose a per-session result such as:

- crop active and reduced recommendation honored;
- crop active but application retained a full-sized swapchain;
- runtime does not support mutable FOV;
- application did not make the expected view-configuration query.

Settings that change the pixel budget should be considered restart-required. Once an
application has created its swapchains, changing only the located FOV can change what
is visible but generally cannot reclaim the existing texture allocation. Live editing
could be useful for finding comfortable boundaries, but the final performance result
should be confirmed after restarting the application.

VectorXR should alter only recommended dimensions, not silently rewrite dimensions in
`xrCreateSwapchain`. An application creates graphics resources and viewports based on
the dimensions it requested; changing that contract underneath it is unsafe.

## Interaction with existing VectorXR features

### Depth and Depth Anchor

Crop should be represented separately from Depth's render/submission geometry. Apply
the crop after Depth's render-time FOV adjustment, while ensuring Depth Anchor removes
only the Depth delta at submission. VectorXR's current tangent-space restoration is
designed to preserve unrelated projection changes, which is a suitable basis.

Regression coverage should verify that Depth Anchor restores convergence changes
without restoring or double-applying the crop boundaries.

### Pivot

Pivot and crop are conceptually compatible when composed inside VectorXR. Keeping both
transformations in one layer avoids the compounded `xrLocateViews` transforms and
submission mismatches that can occur when separate FOV and pose layers are stacked.

Testing is still required for large Pivot angles, late reprojection, head-locked UI,
and runtimes that validate submitted pose/FOV pairs strictly.

### Quadviews

Quadviews is the largest complication and should not be enabled with the first crop
implementation.

The two outer views represent the full peripheral projection while the two focus views
are smaller, potentially moving projections. Applying the same crop percentage to all
four views would incorrectly shrink the focus views. Correct behavior would crop the
outer views and intersect each focus view with the retained outer region, leaving an
unaffected focus view unchanged when it already lies wholly inside that region.

VectorXR's synthesized D3D11 Quadviews compositor also reconstructs a stereo output at
the cached native full-FOV resolution. Crop support would need to make its output canvas,
focus placement, density calculations, and submitted projection crop-aware; otherwise
the game might render fewer source pixels only for VectorXR to recreate a full-sized
output pass.

Native Varjo/foveated-inset behavior needs separate runtime testing because focus FOVs
can move each frame.

### Visibility masks

The runtime visibility mask is defined for the runtime's native projection. Engines may
use it to avoid rendering pixels hidden by the lens profile. After changing the FOV,
VectorXR must verify whether the mask coordinates remain valid. It may need to transform
the mask into the cropped projection or disable the mask for cropped views. Passing an
unadjusted mask risks removing visible pixels or retaining an invalid optimization.

### UI, overlays, and other API layers

In-game HUD elements based on the projection may move or be clipped. Independent quad
or cylinder composition layers can remain visible outside the cropped projection,
depending on how they are positioned and on API-layer ordering.

Other layers that rewrite FOV should be treated as incompatible. Consecutive transforms
can compound the crop, displace HUD elements, and produce projection deformation.

## Proposed product shape

The feature should be presented as **Performance Crop** or **Crop**, rather than as a
generic FOV control, to distinguish real pixel-budget reduction from a projection-only
FOV override.

Suggested per-application controls:

- top and bottom crop;
- left-eye outer-left and right-eye outer-right crop;
- optional advanced inner/nose-side crop per eye;
- linked horizontal and vertical controls;
- retained-FOV and estimated retained-pixel indicators;
- presets such as Mild, Cockpit, and Sim Racing;
- explicit restart-required state;
- runtime and application compatibility/status reporting.

The UI and documentation should warn about black borders, reduced peripheral awareness,
HUD clipping, possible discomfort, layer conflicts, and the absence of guaranteed FPS
gains.

## Recommended implementation scope

An initial implementation should be deliberately narrow:

1. Support primary stereo view configurations only.
2. Require `fovMutable == XR_TRUE`.
3. Apply tangent-space FOV cropping in `xrLocateViews`.
4. Scale recommended view dimensions in `xrEnumerateViewConfigurationViews`.
5. Keep the path graphics-API-agnostic and avoid an internal compositor.
6. Use per-application, restart-required profiles.
7. Diagnose whether observed swapchain dimensions honored the recommendation.
8. Disable the feature when VectorXR Quadviews is active.
9. Add explicit compatibility checks for other known FOV-modifying layers.

Only after stereo behavior is validated across runtimes and representative engines
should crop-aware synthesized and native Quadviews paths be considered.

## Product claim

A defensible description is:

> Performance Crop reduces the field of view and pixel budget requested from the game.
> Performance gains depend on the game honoring OpenXR view recommendations and on the
> current workload being GPU-bound.

It should not be described as universal rendering culling or a guaranteed FPS boost.
