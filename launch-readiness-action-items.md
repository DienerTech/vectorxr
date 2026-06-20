# VectorXR Launch-Readiness Action Items

Living tracker of open items found during launch-readiness testing (started 2026-06-19).

## VXR code — not yet built
- [ ] **PiOpenXR runtime-detection warning.** Detect the Pimax runtime and warn (log line + DepthXR tab note) when depth is enabled on a runtime that won't honor submitted per-eye geometry. Scope to affected versions once the 2.0 beta result is known.
- [ ] **Single-source UI bounds from the validator.** Slider/validator min-max are currently hand-matched numbers; derive UI bounds from the validator constants so they can't drift. Low priority.

## Cleanups (once things settle)
- [ ] **Remove the EndFrame submitted-pose diagnostic** added for the Pimax depth investigation (debug-only scaffolding in `openxr_layer.cpp` EndFrame).
- [ ] **Drop legacy depth `stereoBoostEnabled`/`convergenceEnabled` tolerance** from the C++ parser allowlist once existing configs have re-saved without them.

## Documentation
- [ ] **Document PiOpenXR as a known-limited runtime for depth**, with the SteamVR-pipeline workaround, for launch.

## External / user-owned
- [ ] **Test depth on Pimax Play 2.0 beta** (blocked on Pimax granting beta access).
- [ ] **Report the depth-normalization regression to Pimax** (PiOpenXR overrides submitted per-eye IPD/convergence; SteamVR honors it; VXR output proven correct).

## Done (2026-06-19, second pass)
- [x] Pivot yaw/pitch defaults synchronized for a consistent feel (both: multiplier 1.5, deadzone 8, max extra 120).
- [x] Depth default profile now ships OFF, consistent with pivot/quadviews.
- [x] Quadviews budget chip color-coded (green/yellow/red), pixel % made prominent, new blinking ⚠ "Detrimental" state when budget > 100%.
- [x] Default-profile enable toggle moved inside each module's Default Profile collapse section (depth/pivot/quadviews).
- [x] Active overview table columns right-aligned under their headers.
- [x] Fixed: Rust `PivotXRSettings` was missing `activation_ramp_seconds` and still had `pitch_smoothing` (prior turn missed main.rs) — would have dropped the new ramp setting on save.

## Done this session (2026-06-19)
- [x] Pivot UI quality pass: unified smoothing, new General section (smoothing + activation-ramp-seconds, default 0.35) exposed as a per-profile setting, "Pitch Assist" → "Pitch", refreshed tooltips, shared `PivotSettingsFields.vue` component (mirrors QuadViews layout), multiplier max 3 confirmed.
- [x] Pivot smooth on/off activation envelope (0.35s smoothstep) — headset-validated.
- [x] Depth/pivot toggle edge-detector priming (depth now defaults ON when enabled+set; no spurious first-press flip).
- [x] Depth input overhaul: unified ×100 scaling, bipolar negatives, centered sliders, refreshed descriptions, removed per-effect enabled flags.
- [x] Root-caused depth "regression" to PiOpenXR normalizing submitted geometry (not VXR).
