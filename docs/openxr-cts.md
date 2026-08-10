# OpenXR CTS compatibility testing

VectorXR uses the Khronos OpenXR Conformance Test Suite (CTS) as a differential compatibility test. The CTS is designed for runtimes, not API layers, so a VectorXR result is interpreted by comparing the same runtime and hardware with VectorXR disabled and enabled.

The harness pins the approved x64 CTS release `1.1.61.0`, verifies its published SHA-256, and stores it under the ignored `build-openxr-cts/` directory. Results go under the ignored `logs/openxr-cts/` directory. It does not change the active runtime, the OpenXR layer registry, or VectorXR settings.

## First-time setup

Download and verify the pinned CTS package without starting VR:

```powershell
.\scripts\Invoke-OpenXrCts.ps1 -DownloadOnly
```

The CTS uses the active system OpenXR runtime. `-RunLabel` only names the captured result; it does not select or change the runtime. The harness prints the active runtime manifest before launching, so verify it matches the runtime you intend to test. Have the headset and both controllers connected. For Meta Link, enter the Link session and keep the headset awake before starting the command. For Pimax OpenXR, start the applicable Pimax VR session and verify the headset is visible there. The harness passes CTS `--pollGetSystem`, allowing runtime initialization to finish instead of failing on the first temporary "no HMD" response.

The default `Pivot` suite is hands-off. It runs the deterministic assertions normally and exercises each interactive composition scene for three seconds before CTS marks that scene auto-skipped. Each focused case runs in a separate CTS process, so a runtime crash or a session that fails to clean up cannot prevent the remaining baseline cases from running. Use `PivotInteractive` only when a person will compare the scenes to their reference images and record pass/fail with the controllers.

The CTS executable is `conformance_cli.exe`. Run it once with VectorXR enabled if it is not already in **Applications > Seen OpenXR apps**, then register it as an application. For a Pivot run, target `conformance_cli.exe` with an enabled, always-active Pivot profile. Keep Depth, Quadviews, and Turbo disabled while isolating Pivot. Confirm the VectorXR log contains `VectorXR attached to process: conformance_cli.exe` and reports the intended resolved profile.

## Required A/B procedure

Use the same headset connection, active runtime version, graphics plugin, room setup, and controller bindings for both runs. Do not insert a mirror API layer; use a runtime-native mirror or headset recording when video is needed.

1. Disable the VectorXR OpenXR layer and run the runtime baseline:

   ```powershell
   .\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel meta-link-baseline -ExpectedLayerState Disabled
   ```

2. Enable VectorXR, configure the `conformance_cli.exe` Pivot profile, and run the same selection. Point `-CompareTo` at the baseline result directory printed by the first command:

   ```powershell
   .\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel meta-link-pivot -ExpectedLayerState Enabled -CompareTo .\logs\openxr-cts\YYYYMMDD-HHMMSS-meta-link-baseline-pivot-d3d11
   ```

3. Review `comparison.json`, `summary.json`, `metadata.json`, and the `results-*.xml` files in the new result directory. CTS console progress remains attached to the terminal so interactive prompts are visible immediately. A failure already present in the baseline is normally a runtime issue. A new failure, crash, hang, changed visual defect, or different error with VectorXR enabled is a VectorXR regression.

The isolated Pivot runner continues after a native CTS/runtime crash and records the exit code for each phase in `metadata.json`. A crashing baseline is therefore still usable: run the VectorXR-enabled side with `-CompareTo`, and the comparison only reports a regression if the enabled result is worse or a previously produced case disappears. Timed visual scenes are counted as `autoSkipped` in `summary.json`; they exercised the API path but were not visually judged.

Repeat the pair after any runtime, headset transport, or graphics API change. Name labels accordingly, such as `steamvr-baseline`, `steamvr-pivot`, `virtualdesktop-baseline`, or `virtualdesktop-pivot`.

## Suites

`Pivot` is the focused, hands-off default and runs:

- `xrLocateSpace`, which automatically checks space handles, inputs, and transform math;
- `xrLocateSpace_xrLocateViews`, which checks the relationship between view poses and `XR_REFERENCE_SPACE_TYPE_VIEW`;
- `XrCompositionLayerQuad`, including pose and submission validation;
- `QuadPoses`, which composes an `XrSpace` pose with a quad-layer pose in different orders;
- `QuadHands`, which attaches quads to controller grip spaces;
- `QuadProjectionQuad` and `ProjectionQuadProjection`, which reveal inconsistent motion between projection and quad layers.

The composition scenes in this mode are deliberately reported as auto-skipped after three seconds. They still expose crashes, hangs, validation messages, and API errors, but they cannot automatically detect a purely visual mismatch.

The full human-evaluated form adds `SpaceOffsets`, an interactive scenario covering reference, action, and offset spaces:

```powershell
.\scripts\Invoke-OpenXrCts.ps1 -Suite PivotInteractive -RunLabel runtime-pivot-visual
```

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
.\scripts\Invoke-OpenXrCts.ps1 -Suite Pivot -RunLabel verbose-check -AdditionalArguments @('--success')
```

Auto-skip exercises code paths but does not replace human evaluation. `SpaceOffsets` is tagged `no_auto` and still requires interaction.

## Updating the pinned CTS

Update the version, release tag, archive name, URL, and SHA-256 constants together at the top of `scripts/Invoke-OpenXrCts.ps1`. Use only an approved Khronos release and verify the digest from the GitHub release metadata. Because test behavior and expected images can change, baseline results from different CTS versions are not comparable.
