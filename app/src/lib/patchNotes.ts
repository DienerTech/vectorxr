export interface PatchNoteEntry {
  version: string
  date: string
  title: string
  summary: string
  items: string[]
}

export const patchNotes: PatchNoteEntry[] = [
  {
    version: '0.11.0',
    date: '2026-06-19',
    title: 'Pivot easing and Depth clarity',
    summary: 'Pivot now eases smoothly on and off instead of snapping, Depth and Pivot tuning are cleaner and more honest, Quadviews gained a clearer performance budget, Depth includes runtime-compatibility guidance, and bindings can now play optional activate/deactivate sound cues.',
    items: [
      'Pivot now eases in and out when toggled on or off instead of snapping the view, with a tunable activation ramp (default 0.35s) in a new Pivot "General" section.',
      'Unified Pivot smoothing into one setting shared by yaw and pitch, renamed "Pitch Assist" to "Pitch", and refreshed the Pivot tooltips and layout to match Quadviews.',
      'Synchronized the default Pivot yaw and pitch values (multiplier 1.5, deadzone 8°, max extra 120°) for a consistent out-of-the-box feel.',
      'Reworked the Depth controls with bipolar sliders centered at neutral, consistent percentage scaling, clearer descriptions, and a single value per effect (removed the redundant per-effect enable checkboxes).',
      'Fixed the Depth runtime toggle so it applies your saved values immediately and acts as a clean universal on/off switch.',
      'Added a Depth Compatibility notice explaining that some headset-native OpenXR runtimes (such as Pimax) can ignore Depth, with guidance to switch to the SteamVR pipeline.',
      'Default profiles for all Enhancements now ship off for consistency.',
      'Quadviews now color-codes its performance budget (green/amber/red) with a blinking "Detrimental" warning above 100%, and Foveate/Peripheral resolution are entered as a percent of headset resolution.',
      'Tightened the Active overview table alignment and trimmed page spacing so settings fit without an unnecessary scrollbar.',
      'Added optional sound feedback for keybind and device bindings: play a short .wav when a binding activates or deactivates (such as the Depth toggle or Pivot activation), using the bundled default tones or your own .wav chosen per state.',
      'Added a master Sound Feedback Volume control in Settings to scale every VectorXR cue, with a Test button to preview the level.',
      'Relicensed VectorXR under the Mozilla Public License 2.0 (previously MIT) ahead of the public release.',
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
