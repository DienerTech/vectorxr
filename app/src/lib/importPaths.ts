import type { VectorXRConfig } from './model'

// File and folder paths a settings file can carry. The layer opens them inside
// every game, so a path on another computer (\\server\share\...) in a file
// someone else made would have each game contact that computer, and Windows
// sign in to it. An imported file's network paths are therefore left out and
// named in the import preview; the user can choose them again by hand.
export interface ClearedImportPath {
  field: string
  path: string
}

// \\server\share, //server/share, \\?\UNC\server\share and device paths
// (\\.\...). A drive path, also in its long form (\\?\D:\...), is local. A
// drive letter mapped to a share can't be told apart here and is kept.
export function isNetworkPath(value: unknown): boolean {
  if (typeof value !== 'string') return false
  const path = value.trim().replace(/\//g, '\\')
  if (!path.startsWith('\\\\')) return false
  return !/^\\\\\?\\[A-Za-z]:(?:\\|$)/.test(path)
}

const pathKeys = new Set(['activateSound', 'deactivateSound'])

// Returns a copy of the config without its network paths, and the paths left
// out: the screenshot folder (back to the default folder), and every binding's sound files (back to the built-in
// sounds).
export function clearNetworkPaths(config: VectorXRConfig): { config: VectorXRConfig; cleared: ClearedImportPath[] } {
  const copy = JSON.parse(JSON.stringify(config)) as VectorXRConfig
  const cleared: ClearedImportPath[] = []

  const screenshot = copy.core?.screenshot
  if (screenshot && isNetworkPath(screenshot.folder)) {
    cleared.push({ field: 'core.screenshot.folder', path: screenshot.folder as string })
    delete screenshot.folder
  }

  const visit = (value: unknown, field: string) => {
    if (Array.isArray(value)) {
      value.forEach((item, index) => visit(item, `${field}[${index}]`))
      return
    }
    if (!value || typeof value !== 'object') return
    const record = value as Record<string, unknown>
    for (const [key, child] of Object.entries(record)) {
      const childField = field ? `${field}.${key}` : key
      if (pathKeys.has(key) && isNetworkPath(child)) {
        cleared.push({ field: childField, path: child as string })
        record[key] = ''
      } else {
        visit(child, childField)
      }
    }
  }
  visit(copy, '')

  return { config: copy, cleared }
}
