export interface PatchNoteEntry {
  version: string
  date: string
  title: string
  summary: string
  items: string[]
}

export const patchNotes: PatchNoteEntry[] = [
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
      'Added a read-only Home status for the VectorXR OpenXR layer so users can tell whether OpenXR tweaks can apply at runtime.',
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
