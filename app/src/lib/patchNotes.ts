export interface PatchNoteEntry {
  version: string
  date: string
  title: string
  summary: string
  items: string[]
}

export const patchNotes: PatchNoteEntry[] = [
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
