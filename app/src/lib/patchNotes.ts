import patchNotesData from './patchNotes.json' with { type: 'json' }

export type PatchNoteItem =
  | string
  | {
      html: string
      items?: PatchNoteItem[]
    }

export interface PatchNoteEntry {
  version: string
  date: string
  title: string
  summary: string
  items: PatchNoteItem[]
}

function requireString(value: unknown, path: string): string {
  if (typeof value !== 'string' || value.trim().length === 0) {
    throw new Error(`Invalid patch notes: ${path} must be a non-empty string.`)
  }
  return value
}

function parseItem(value: unknown, path: string): PatchNoteItem {
  if (typeof value === 'string') {
    return requireString(value, path)
  }
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`Invalid patch notes: ${path} must be a string or nested list item.`)
  }

  const candidate = value as Record<string, unknown>
  const item: Exclude<PatchNoteItem, string> = {
    html: requireString(candidate.html, `${path}.html`),
  }
  if ('items' in candidate) {
    if (!Array.isArray(candidate.items) || candidate.items.length === 0) {
      throw new Error(`Invalid patch notes: ${path}.items must be a non-empty array when present.`)
    }
    item.items = candidate.items.map((child, index) => parseItem(child, `${path}.items[${index}]`))
  }
  return item
}

function parsePatchNotes(value: unknown): PatchNoteEntry[] {
  if (!Array.isArray(value) || value.length === 0) {
    throw new Error('Invalid patch notes: the JSON root must be a non-empty array.')
  }

  const versions = new Set<string>()
  return value.map((entry, index) => {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) {
      throw new Error(`Invalid patch notes: entries[${index}] must be an object.`)
    }
    const candidate = entry as Record<string, unknown>
    const version = requireString(candidate.version, `entries[${index}].version`)
    const date = requireString(candidate.date, `entries[${index}].date`)
    if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) {
      throw new Error(`Invalid patch notes: entries[${index}].date must use YYYY-MM-DD.`)
    }
    if (versions.has(version)) {
      throw new Error(`Invalid patch notes: duplicate version ${version}.`)
    }
    versions.add(version)
    if (!Array.isArray(candidate.items) || candidate.items.length === 0) {
      throw new Error(`Invalid patch notes: entries[${index}].items must be a non-empty array.`)
    }

    return {
      version,
      date,
      title: requireString(candidate.title, `entries[${index}].title`),
      summary: requireString(candidate.summary, `entries[${index}].summary`),
      items: candidate.items.map((item, itemIndex) =>
        parseItem(item, `entries[${index}].items[${itemIndex}]`),
      ),
    }
  })
}

// Patch-note data is bundled with the app, but keep v-html constrained anyway:
// only exact, attribute-free inline formatting tags survive escaping. Lists are
// represented structurally in JSON and rendered by Vue rather than accepted as HTML.
export function formatPatchNoteInlineHtml(value: string): string {
  return value
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
    .replace(/&lt;(\/?)(strong|em|code)&gt;/gi, (_: string, close: string, tag: string) =>
      `<${close}${tag.toLowerCase()}>`,
    )
    .replace(/&lt;br\s*\/?&gt;/gi, '<br>')
}

export const patchNotes = parsePatchNotes(patchNotesData)
