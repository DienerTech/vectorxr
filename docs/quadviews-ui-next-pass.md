# Quadviews UI Next Pass

Notes from the first native Quadviews performance sprint. The current UI is functional enough for testing, but the next dedicated UI pass should make the feature easier to tune without exposing users to confusing renderer internals.

## Goals

- Make render cost visible before the user launches a game.
- Help users choose safe values without needing to understand quadview pixel math.
- Keep advanced controls available for enthusiasts and troubleshooting.
- Preserve per-application Quadviews profiles as the primary tuning workflow.

## Planned Work

- Improve the estimated pixel budget display so it clearly explains the formula:
  `peripheralScale^2 + (focusWidthPercent * focusScale) * (focusHeightPercent * focusScale)`.
- Add visual budget severity states, such as Performance, Balanced, Quality, and Heavy.
- Warn when settings exceed 100% of stereo pixel cost.
- Add presets for common starting points:
  - Performance: smaller focus region, very low peripheral scale.
  - Balanced: eye-tracked focus that should be hard to notice during normal use.
  - Quality: larger focus region and higher focus scale for high-end GPUs.
- Revisit control labels so they map cleanly to user expectations and the QuadViews Companion app terminology.
- Consider compact helper text or tooltips for the difference between focus size, focus scale, peripheral scale, transition thickness, and sharpening.
- Add a native runtime status section once we have a telemetry bridge:
  - active application profile
  - actual app pixel budget from swapchain sizes
  - direct compositor path status
  - compositor CPU/GPU timing
  - copy fallback status
- Decide whether live runtime stats should be read from a small rolling JSON file under `AppData/Local/VectorXR` or sent through a Tauri event bridge.
- Tighten profile layout so default and per-app settings feel consistent without becoming a wall of numeric fields.

## Deferred From Performance Branch

- UI presets and deeper guidance were intentionally left out of `quadviews-performance`.
- The performance branch only added a minimal budget readout to support native testing.
- A future UI sprint should treat Quadviews as a first-class feature surface, not a small tweak panel.
