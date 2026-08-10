# OpenXR CTS compatibility testing

VectorXR uses the Khronos OpenXR Conformance Test Suite (CTS) as a differential compatibility test. The CTS is designed for runtimes, not API layers, so a VectorXR result is interpreted by comparing the same runtime and hardware with VectorXR disabled and enabled.

The harness pins the approved x64 CTS release `1.1.61.0`, verifies its published SHA-256, and stores it under the ignored `build-openxr-cts/` directory. Results go under the ignored `logs/openxr-cts/` directory. It does not change the active runtime, the OpenXR layer registry, or VectorXR settings.

## First-time setup

Download and verify the pinned CTS package without starting VR:

```powershell
.\scripts\Invoke-OpenXrCts.ps1 -DownloadOnly
```

The CTS uses the active system OpenXR runtime. Have the headset and both controllers connected. Its interactive tests show instructions and a reference image in the headset; use the controller actions requested by the test to mark each result pass or fail.

The CTS executable is `conformance_cli.exe`. Run it once with VectorXR enabled if it is not already in **Applications > Seen OpenXR apps**, then register it as an application. For a Pivot run, target `conformance_cli.exe` with an enabled, always-active Pivot profile. Keep Depth, Quadviews, and Turbo disabled while isolating Pivot. Confirm the VectorXR log contains `VectorXR attached to process: conformance_cli.exe` and reports the intended resolved profile.

## Required A/B procedure

Use the same headset connection, active runtime version, graphics plugin, room setup, and controller bindings for both runs. Do not insert a mirror API layer; use a runtime-native mirror or headset recording when video is needed.

1. Disable the VectorXR OpenXR layer and run the runtime baseline:

   ```powershell
   .\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel meta-link-baseline
   ```

2. Enable VectorXR, configure the `conformance_cli.exe` Pivot profile, and run the same selection. Point `-CompareTo` at the baseline result directory printed by the first command:

   ```powershell
   .\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel meta-link-pivot -CompareTo .\logs\openxr-cts\YYYYMMDD-HHMMSS-meta-link-baseline-pivot-d3d11
   ```

3. Review `comparison.json`, `summary.json`, `metadata.json`, `console.log`, and `results.xml` in the new result directory. A failure already present in the baseline is normally a runtime issue. A new failure, crash, hang, changed visual defect, or different error with VectorXR enabled is a VectorXR regression.

Repeat the pair after any runtime, headset transport, or graphics API change. Name labels accordingly, such as `steamvr-baseline`, `steamvr-pivot`, `virtualdesktop-baseline`, or `virtualdesktop-pivot`.

## Suites

`Pivot` is the focused default and runs:

- `xrLocateSpace_xrLocateViews`, which checks the relationship between view poses and `XR_REFERENCE_SPACE_TYPE_VIEW`;
- `XrCompositionLayerQuad`, including pose and submission validation;
- `QuadPoses`, which composes an `XrSpace` pose with a quad-layer pose in different orders;
- `QuadHands`, which attaches quads to controller grip spaces;
- `QuadProjectionQuad` and `ProjectionQuadProjection`, which reveal inconsistent motion between projection and quad layers;
- `SpaceOffsets`, an interactive scenario covering reference, action, and offset spaces.

Other selections are available:

```powershell
# Four-view extension and OpenXR 1.1 foveated-inset behavior
.\scripts\Invoke-OpenXrCts.ps1 -Suite QuadViews -RunLabel runtime-quadviews

# Every human-evaluated composition test
.\scripts\Invoke-OpenXrCts.ps1 -Suite Composition -RunLabel runtime-composition

# Full non-interactive CTS pass
.\scripts\Invoke-OpenXrCts.ps1 -Suite Automated -RunLabel runtime-automated
```

VectorXR's synthesized Quadviews path currently targets D3D11, which is the harness default. Use `-GraphicsPlugin D3D12`, `Vulkan`, `Vulkan2`, or `OpenGL` only when testing a path supported by the selected runtime and VectorXR configuration.

Use `-Plan` to print the resolved executable, test selection, and result location without launching CTS. Additional CTS/Catch2 options can be passed as an array:

```powershell
.\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel quick-exercise -AdditionalArguments @('--autoSkipTimeout', '3000')
```

Auto-skip exercises code paths but does not replace human evaluation. `SpaceOffsets` is tagged `no_auto` and still requires interaction.

## Updating the pinned CTS

Update the version, release tag, archive name, URL, and SHA-256 constants together at the top of `scripts/Invoke-OpenXrCts.ps1`. Use only an approved Khronos release and verify the digest from the GitHub release metadata. Because test behavior and expected images can change, baseline results from different CTS versions are not comparable.
