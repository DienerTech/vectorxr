import { reactive, readonly } from 'vue'

import supporterCredits from '../../../config/supporter-credits.json' with { type: 'json' }

export function supportersFeedUrl({ repository, branch, path }: typeof supporterCredits.feed): string {
  const ref = branch.includes('/') ? ['refs', 'heads', ...branch.split('/')] : [branch]
  const segments = [...repository.split('/'), ...ref, ...path.split('/')]
  return 'https://raw.githubusercontent.com/' + segments.map(encodeURIComponent).join('/')
}

export const supportersUrl = supportersFeedUrl(supporterCredits.feed)
const cacheKey = 'vectorxr-supporters-v1'
const hour = 60 * 60 * 1000
const maxAge = 7 * 24 * hour
const retryInterval = 60 * 1000

export interface SupporterTier {
  label: string
  names: string[]
}

// 'unpublished': GitHub answered, but no supporter list is published at the
// feed's address (HTTP 404 or 410): nothing to credit yet, not a failure.
type Status = 'idle' | 'loading' | 'ready' | 'cached' | 'unavailable' | 'unpublished'
type CacheStorage = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>

function publicLabel(value: unknown): string | null {
  if (typeof value !== 'string' || /[\u0000-\u001f\u007f-\u009f\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069]/u.test(value)) return null
  const label = value.trim().normalize('NFC')
  return label && [...label].length <= 80 ? label : null
}

function parseTiers(document: unknown): SupporterTier[] | null {
  if (!document || typeof document !== 'object' || !('version' in document) || document.version !== 1
    || !('tiers' in document) || !Array.isArray(document.tiers)) return null
  const tiers: SupporterTier[] = []
  for (const tier of document.tiers) {
    const label = publicLabel(tier?.label)
    if (!label || !Array.isArray(tier.names)) return null
    const names: string[] = []
    for (const value of tier.names) {
      const name = publicLabel(value)
      if (name === null) return null
      names.push(name)
    }
    tiers.push({ label, names })
  }
  return tiers
}

function browserStorage(): CacheStorage | null {
  try {
    return typeof window === 'undefined' ? null : window.localStorage
  } catch {
    return null
  }
}

export function createSupportersStore(
  fetcher: typeof fetch = (...args) => fetch(...args),
  storage: CacheStorage | null = browserStorage(),
  timeoutMs = 8000,
) {
  const state = reactive<{ status: Status; tiers: SupporterTier[] }>({ status: 'idle', tiers: [] })
  let cachedAt: number | null = null
  let expiry: ReturnType<typeof setTimeout> | undefined
  let nextAttempt = 0
  let inFlight: Promise<void> | null = null

  function removeStored() {
    try { storage?.removeItem(cacheKey) } catch { /* Storage may be disabled. */ }
  }

  function discard() {
    clearTimeout(expiry)
    cachedAt = null
    state.tiers = []
    state.status = 'unavailable'
    removeStored()
  }

  function isCurrent(timestamp: unknown): timestamp is number {
    const now = Date.now()
    return typeof timestamp === 'number' && Number.isSafeInteger(timestamp) && timestamp <= now && now - timestamp < maxAge
  }

  function display(tiers: SupporterTier[], timestamp: number) {
    clearTimeout(expiry)
    cachedAt = timestamp
    state.tiers = tiers
    // A supporter page can stay open through an extended outage.
    expiry = setTimeout(discard, Math.max(0, timestamp + maxAge - Date.now()))
  }

  function restore() {
    try {
      const raw = storage?.getItem(cacheKey)
      if (!raw) return
      const saved = JSON.parse(raw)
      const tiers = parseTiers(saved)
      if (saved.source === supportersUrl && tiers !== null && isCurrent(saved.fetchedAt)) {
        display(tiers, saved.fetchedAt)
        return
      }
    } catch { /* Ignore unreadable or invalid cached data. */ }
    removeStored()
  }

  function load(force = false): Promise<void> {
    if (inFlight) return inFlight
    if (state.status === 'idle') restore()
    if (cachedAt !== null && !isCurrent(cachedAt)) discard()
    if (!force && Date.now() < nextAttempt) return Promise.resolve()
    state.status = 'loading'
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    inFlight = (async () => {
      try {
        const response = await fetcher(supportersUrl, {
          signal: controller.signal, cache: 'no-store', credentials: 'omit', referrerPolicy: 'no-referrer', redirect: 'error',
        })
        if (controller.signal.aborted) throw new Error('Request timed out')
        if (response.status === 404 || response.status === 410) {
          discard()
          state.status = 'unpublished'
          nextAttempt = Date.now() + retryInterval
          return
        }
        if (!response.ok) throw new Error('Feed unavailable')
        const tiers = parseTiers(await response.json())
        if (tiers === null || controller.signal.aborted) throw new Error('Invalid or incomplete feed')
        const timestamp = Date.now()
        display(tiers, timestamp)
        state.status = 'ready'
        nextAttempt = timestamp + hour
        try {
          storage?.setItem(cacheKey, JSON.stringify({ version: 1, source: supportersUrl, fetchedAt: timestamp, tiers }))
        } catch {
          // A previous list can contain a removed supporter.
          removeStored()
        }
      } catch {
        if (isCurrent(cachedAt)) state.status = 'cached'
        else discard()
        nextAttempt = Date.now() + retryInterval
      } finally {
        clearTimeout(timer)
      }
    })().finally(() => { inFlight = null })
    return inFlight
  }

  return { state: readonly(state), load }
}

let shared: ReturnType<typeof createSupportersStore> | undefined
export function useSupportersStore() {
  return shared ??= createSupportersStore()
}
