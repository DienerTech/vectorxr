import { shallowRef } from 'vue'

import type { AppTab } from './model'

// The in-app Guides page. Each guide's text lives in the guides catalog under
// items.<id>; this list gives the order, the group card it sits in, the page
// it explains (for its "Open" button) and its badge.
export type GuideId =
  | 'start'
  | 'anticheat'
  | 'quadviews'
  | 'turbo'
  | 'framegen'
  | 'pivot'
  | 'depth'
  | 'osd'

export type GuideGroup = 'start' | 'enhancements' | 'tools'
export type GuideBadge = 'experimental'

export interface GuideEntry {
  id: GuideId
  group: GuideGroup
  tab?: AppTab
  badges: GuideBadge[]
}

export const guideGroups: GuideGroup[] = ['start', 'enhancements', 'tools']

// Enhancements follow the sidebar's order.
export const guides: GuideEntry[] = [
  { id: 'start', group: 'start', tab: 'core', badges: [] },
  { id: 'anticheat', group: 'start', tab: 'layers', badges: [] },
  { id: 'quadviews', group: 'enhancements', tab: 'quadviews', badges: [] },
  { id: 'turbo', group: 'enhancements', tab: 'turbo', badges: [] },
  { id: 'framegen', group: 'enhancements', tab: 'framegen', badges: [] },
  { id: 'pivot', group: 'enhancements', tab: 'pivotxr', badges: [] },
  { id: 'depth', group: 'enhancements', tab: 'depthxr', badges: [] },
  { id: 'osd', group: 'tools', tab: 'osd', badges: [] },
]

export function guideEntry(id: GuideId): GuideEntry | undefined {
  return guides.find((guide) => guide.id === id)
}

// The guide the Guides page shows; null shows the list. Kept outside the page
// so another page's "Read the guide" link can open one directly.
export const openGuideId = shallowRef<GuideId | null>(null)

// The page a guide was opened from by its "Read the guide" link, so the guide
// can go back to it; null when it was opened from the Guides page.
export const guideOrigin = shallowRef<AppTab | null>(null)

export function showGuide(id: GuideId | null, from: AppTab | null = null) {
  openGuideId.value = id
  guideOrigin.value = id ? from : null
}

// Splits "**bold**" markup in a guide message into plain and strong runs, so
// translators keep emphasis inside one message.
export function emphasisRuns(text: string): Array<{ text: string; strong: boolean }> {
  return text
    .split(/(\*\*[^*]+\*\*)/)
    .filter((part) => part !== '')
    .map((part) => (part.startsWith('**') && part.endsWith('**') && part.length > 4 ? { text: part.slice(2, -2), strong: true } : { text: part, strong: false }))
}
