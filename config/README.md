# Config

The canonical VectorXR schema lives in `vectorxr.schema.json`.

The current file format supports:

- a shared `core` settings block
- a suite-level application registry
- local seen-app tracking preference
- module-specific settings under `modules`
- Depth defaults and per-application overrides
- Pivot behavior profiles (`enhancedMotion` or `snapViews`), shared activation bindings, and reusable linked Nudge Sets
- experimental Frame Generation under the optional `modules.framegen` block: defaults, per-application profiles, an A/B `toggleBinding`, and a module-level `diagnosticView`; configs without the block use the defaults (off)
- OSD `showFramegen` / `compactShowFramegen` and the `framegen` expanded row; saved `bodyOrder` lists without it get the row appended at the end
- OSD `showFramegenFps` (default on): Frame Generation's state and the headset's frame rate beside the FPS value, in both layouts
- OSD `showPerformance` (default on) / `compactShowPerformance` (default off) and the `performance` expanded row: CPU and GPU headroom against the frame budget and video memory; saved orders without it get the row appended at the end. Layers of 0.19.2 and earlier reject a `bodyOrder` that names a row they do not know, so the app keeps `bodyOrder` to the rows they know and writes the full order to `rowOrder` (layers since 0.19.3 read `rowOrder` first and skip unknown rows with a warning)
- Frame Generation's `frameMultiplier` defaults to `dynamic`; `auto`, `2x`, `3x`, and `4x` are the fixed rates

## Frame Generation motion source

Frame Generation stores these values in `modules.framegen.defaults` for the Default Profile
and `modules.framegen.profiles[].settings` for custom profiles:

- `motionEstimation`: `shader` (default) uses GPU shader cores; `hardware` requests the optical
  flow accelerator of a supported NVIDIA RTX GPU. Non-NVIDIA GPUs and systems where that
  accelerator is unavailable use shader flow.
- `interpolateFlow` applies only with `synthesisMode: "interpolate"`, when hardware motion
  estimation is selected. It works with `dynamic`, `auto`, and the fixed multipliers:

  | Value | Behavior |
  | --- | --- |
  | `auto` (default) | Start on hardware flow, fall back to shader flow when hardware is repeatedly late, and periodically retry hardware. |
  | `hardware` | Keep hardware flow without the timing-based fallback. Unavailable hardware still uses shader flow. |
  | `shader` | Keep shader flow. |

Selecting `motionEstimation: "shader"` always uses shader flow. In `extrapolate` mode,
`motionEstimation` chooses the source directly and `interpolateFlow` has no effect. These
settings apply during play after saving.

## App and OSD language

The app's **Settings > General > Language** offers **System (auto)** and 11 languages: English,
Simplified Chinese, Russian, Spanish (Spain), Portuguese (Brazil), German, Japanese, French,
Polish, Korean, and Traditional Chinese. The app preference is stored on this PC separately
from `settings.json`. System follows the Windows display language, with English as the fallback.
The first launch uses the installer language where supplied.

Under `core.osd`:

- `language`: `"app"` (default, **Same as app**) follows the app, or an explicit language code:
  `en`, `zh-CN`, `ru`, `es-ES`, `pt-BR`, `de`, `ja`, `fr`, `pl`, `ko`, or `zh-TW`.
- `appLanguage`: written by the app with its current language for the OSD to follow. Without
  this value the layer uses the Windows display language. An unsupported `language` falls
  back to the app language, then to the system language or English as needed.

Save Changes applies the OSD language during play. Choosing a new app language updates a saved
OSD set to follow the app. Layers from 0.19 and earlier ignore the language fields and draw in
English. The non-English app and OSD catalogs are machine translated and need native-speaker
review.

Config v3 moves executable matching into the shared application registry so profiles can target reusable app ids.
