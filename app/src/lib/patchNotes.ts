export interface PatchNoteEntry {
  version: string
  date: string
  title: string
  summary: string
  items: string[]
}

export const patchNotes: PatchNoteEntry[] = [
  {
    version: '0.11.4',
    date: '2026-07-01',
    title: 'Quadviews Focus & Sharpness Improvements',
    summary: 'Delivers the Quadviews focus region at full resolution end-to-end for a noticeably sharper image, turns on focus sharpening by default, retunes the default Quadviews profile, and adds deeper debug logging for focus resolution and eye tracking.',
    items: [
      'Fixed the Quadviews focus region being downscaled during compositing. The high-resolution focus view is now delivered at full 1:1 resolution, restoring sharpness that was previously lost and making the Focus Scale setting effective.',
      'Dev Note: You may need to retune your Quadviews settings following this update. Previously, the Foveated Resolution setting was capped at 100%. Values above 100% will now apply correctly!',
      'Focus sharpening now works correctly and is enabled by default, with edge clamping to avoid halos on high-contrast detail such as cockpit text and instruments. Further changes may be made here based on user feedback.',
      'Retuned the default Quadviews profile with a larger focus region and lighter peripheral resolution for a sharper, more forgiving image out of the box at roughly a third of full-stereo pixel cost.',
      'Added detailed Quadviews debug logging, including a focus sampling ratio and canvas density to confirm 1:1 focus rendering, plus eye-tracking interaction-profile diagnostics to help pinpoint why gaze-driven focus may be unavailable.',
    ],
  },
  {
    version: '0.11.3',
    date: '2026-06-28',
    title: 'Varjo and Pivot diagnostics',
    summary: 'Improves Quadviews compatibility and troubleshooting with stronger Varjo/OpenXR contract handling, more reliable live config reloads, safer eye-gaze fallback behavior, and detailed Pivot debug diagnostics.',
    items: [
      'Hardened Varjo Quadviews emulation and OpenXR next-chain handling, including COMBINED_EYE_VARJO reference-space behavior and xrGetReferenceSpaceBoundsRect support.',
      'Improved eye-gaze setup and fallback behavior so Quadviews can continue with head/static focus when eye tracking resources are unavailable.',
      'Improved config file watching and reload behavior so profile changes are picked up more reliably during active sessions.',
      'Added debug-level Pivot diagnostics for tracking freshness, location flags, recomposition mode, eye offsets, and left/right pose deltas to help diagnose pivot drift or asymmetry.',
    ],
  },
  {
    version: '0.11.2',
    date: '2026-06-28',
    title: 'Varjo Quadviews fix',
    summary: 'Fixes a black-screen issue with Quadviews on Varjo headsets',
    items: [
      'Fixed a black screen on Varjo headsets when using VectorXR Quadviews; the focus and peripheral views are now composited correctly.',
      'Each session log now records the VectorXR layer version to make troubleshooting easier.',
    ],
  },
  {
    version: '0.11.1',
    date: '2026-06-27',
    title: 'Layer startup diagnostics',
    summary: 'Adds detailed runtime logging across the OpenXR layer chain during session startup to help diagnose headset- and runtime-specific issues, with no change to runtime behavior.',
    items: [
      'Logs the active OpenXR runtime, headset/system details, and the application’s enabled extensions at startup.',
      'Traces each key OpenXR call during session bring-up (session creation, reference spaces, swapchain format and creation, action-set attach, and session begin) with parameters and results.',
      'Surfaces previously-silent downstream failures in that startup window so the exact failing call is captured in logs and debug exports.',
    ],
  },
  {
    version: '0.11.0',
    date: '2026-06-19',
    title: 'Beta Release',
    summary: 'VectorXR enters beta with smoother Pivot easing, clearer and more honest Depth and Pivot controls, a color-coded Quadviews performance budget, optional sound cues for bindings, and headset runtime-compatibility guidance.',
    items: [
      'Pivot now eases smoothly in and out when toggled instead of snapping, with a tunable activation ramp and a single smoothing control shared by yaw and pitch.',
      'Synchronized the Pivot yaw and pitch defaults for a consistent out-of-the-box feel.',
      'Reworked the Depth controls with bipolar sliders centered at neutral and one value per effect, and fixed the runtime toggle to apply saved values instantly as a clean on/off switch.',
      'Added a Depth compatibility notice for headset-native OpenXR runtimes (such as Pimax) that can ignore Depth, with guidance to use the SteamVR pipeline.',
      'Quadviews now color-codes its performance budget (green/amber/red) and takes Foveate/Peripheral resolution as a percent of headset resolution.',
      'Added optional sound feedback for keybind and device bindings: play a short .wav on activate/deactivate using the bundled tones or your own, with a master volume control in Settings.',
      'Enhancement default profiles now ship off for consistency.',
    ],
  },
  {
    version: '0.10.0',
    date: '2026-06-16',
    title: 'Profile opt-in controls',
    summary: 'Per-application profiles can now opt apps into Depth, Pivot, or Quadviews even when the default profile is off, with a clearer dashboard and restored About tab for release information.',
    items: [
      'Changed profile resolution so the first enabled custom profile targeting an app turns that Enhancement on and applies its settings, even if the default profile is disabled.',
      'Retired the old per-profile disable mode from the app model and profile editor; profile participation is now controlled by a simple Profile On/Off toggle.',
      'Added an active Enhancement overview to Home with quick navigation into Quadviews, Pivot, and Depth settings.',
      'Restored the About tab as the home for project links, Ko-fi support, update checks, and full patch notes.',
      'Moved discovered-app tracking controls into Settings and made the Application Registry focus on unregistered apps first, with details available in a modal.',
      'Updated validation and runtime tests for the new custom-profile opt-in behavior.',
    ],
  },
  {
    version: '0.9.0',
    date: '2026-06-13',
    title: 'UI overhaul',
    summary: 'A refreshed app shell that separates a welcome dashboard from settings, sharpens the OpenXR layer manager, and unifies the Quadviews profile editors around the new "Enhancement" language.',
    items: [
      'Split the old Home tab into a Home dashboard (welcome, system health, patch notes, updates, and support) and a dedicated Settings tab, with Home as the default landing tab.',
      'Folded the former About tab into Home and rewrote the welcome hero to cover foveated rendering, head rotation, and stereo depth.',
      'Adopted "Enhancement" wording throughout the app for the OpenXR modifications.',
      'Made OpenXR layer enabled and disabled states far more distinct with solid, high-contrast status pills in both light and dark themes.',
      'Unified the Quadviews default and per-application profile editors on one shared layout and fixed field alignment in the Resolution group.',
      'Replaced the sidebar VXR text mark with the app logo and evened out the Pivot binding dropdown heights.',
    ],
  },
  {
    version: '0.8.0',
    date: '2026-06-12',
    title: 'Experimental Quadviews',
    summary: 'VectorXR now includes experimental native Quadviews support for OpenXR apps, with per-application profiles and compatibility with Depth and Pivot.',
    items: [
      'Added an experimental Quadviews module for foveal and peripheral rendering control.',
      'Added Quadviews defaults and per-application profiles for tracking mode, focus size, resolution scale, offsets, smoothing, and transition tuning.',
      'Added native Quadviews composition support that works alongside VectorXR Depth and Pivot.',
      'Improved Pivot compatibility for VectorXR-managed quadview sessions, including close cockpit/HMD geometry in supported apps.',
      'Added Quadviews diagnostics and development notes for performance tuning and future dynamic foveation work.',
    ],
  },
  {
    version: '0.7.1',
    date: '2026-06-09',
    title: 'UI polish',
    summary: 'VectorXR now has cleaner About and Health Check layouts with more consistent spacing and clearer support visibility.',
    items: [
      'Made the Ko-fi support link more visible and moved it alongside the app description.',
      'Aligned the About tab cards so the page spacing reads more cleanly.',
      'Improved Health Check card alignment, status chip sizing, and footer composition.',
    ],
  },
  {
    version: '0.7.0',
    date: '2026-05-03',
    title: 'Release readiness',
    summary: 'VectorXR now has in-app update checks, GitHub release automation, MIT licensing, and refreshed project documentation.',
    items: [
      'Added an About tab update checker that can detect newer published GitHub releases.',
      'Added a tag-driven GitHub Actions release workflow for building Windows installers.',
      'Added version/tag validation so release tags match the app version before a release build runs.',
      'Added MIT licensing and clearer branding language for DienerTech LLC.',
      'Refreshed the README with feature screenshots, installation guidance, acknowledgments, and release documentation.',
      'Improved the About tab layout with integrated release status, patch notes, and support information.',
    ],
  },
  {
    version: '0.6.0',
    date: '2026-04-26',
    title: 'OpenXR layer manager',
    summary: 'VectorXR can now inspect and manage implicit OpenXR API layers from inside the app.',
    items: [
      'Added an OpenXR Layers tab with registry-slice sub-tabs for machine-wide and per-user 64-bit and 32-bit implicit API layers.',
      'Added layer enable/disable controls, adjacent move up/down ordering, and details for manifest paths, DLL paths, and Windows signature status.',
      'Added VectorXR and Quad-Views-Foveated guidance so Pivot users can see when layer ordering may break quadviews compatibility.',
      'Added on-demand elevation for machine-wide OpenXR layer changes instead of requiring VectorXR to always run as administrator.',
      'Added a read-only Home status for the VectorXR OpenXR layer so users can tell whether OpenXR Enhancements  can apply at runtime.',
    ],
  },
  {
    version: '0.5.1',
    date: '2026-04-26',
    title: 'Launch prep and icon pipeline cleanup',
    summary: 'VectorXR now has improved binding management and an improved icon!',
    items: [
      'Improved the device binding UI to better handle disconected devices.',
      'Added a note regarding Pivot compatibility with quadviews supplied directly by the runtime, such as Pimax Play.',
      'Replaced the ad hoc Tauri icon-generation flow with a repo-local PowerShell script for predictable `icon.png` and multi-size `icon.ico` output.',
      'Updated the app icon assets to the latest VectorXR branding pass for continued launch evaluation.',
    ],
  },
  {
    version: '0.5.0',
    date: '2026-04-15',
    title: 'Windows installer milestone',
    summary: 'VectorXR now has a local production build that creates a Windows installer with OpenXR layer registration and cleanup.',
    items: [
      'Added a local installer build command that builds the OpenXR layer, stages the payload, and runs the Tauri NSIS bundle.',
      'Packaged the VectorXR OpenXR layer DLL and manifest with the desktop app installer.',
      'Added elevated per-machine installer hooks that register the layer under the HKLM OpenXR implicit API layer registry location.',
      'Added uninstall hooks that remove the installed VectorXR OpenXR layer registry entry.',
      'Validated install, launch, desktop icon, registry registration, and uninstall cleanup locally.',
    ],
  },
  {
    version: '0.4.0',
    date: '2026-04-14',
    title: 'Application discovery and portable profiles',
    summary: 'VectorXR can now remember OpenXR apps it has seen and turn them into profile targets when you are ready.',
    items: [
      'Added local OpenXR application discovery with a registry toggle and clear action.',
      'Added an Application Registry discovery panel with refresh, launch counts, first seen, and last seen details.',
      'Added one-click registration for discovered executables.',
      'Migrated imported profile executable matches into registered applications so shared configs can link Depth and Pivot profiles automatically.',
      'Simplified registered app cards with inline title editing, executable guidance, and a two-column layout on wide screens.',
    ],
  },
  {
    version: '0.3.0',
    date: '2026-04-10',
    title: 'Phase 3 shell foundation',
    summary: 'The app now has a cleaner home surface, theme support, and a built-in patch notes view.',
    items: [
      'Renamed front-end module labels to Home, Depth, and Pivot.',
      'Added system, light, and dark theme preferences.',
      'Moved release notes into an in-app log so recent updates stay easy to find.',
    ],
  },
  {
    version: '0.2.0',
    date: '2026-04-09',
    title: 'VectorXR suite shell',
    summary: 'Home, Depth, and Pivot now live inside one shared app shell with a unified save flow.',
    items: [
      'Split the suite into dedicated tabs.',
      'Added sticky save controls and shared validation status.',
      'Exposed the runtime log viewer from inside the app.',
    ],
  },
  {
    version: '0.1.0',
    date: '2026-04-08',
    title: 'Suite foundation',
    summary: 'VectorXR moved onto the shared suite config and runtime model.',
    items: [
      'Centralized suite-level config and logging.',
      'Kept stereo boost and convergence under one shared runtime layer.',
      'Prepared Pivot runtime settings for future expansion.',
    ],
  },
]
