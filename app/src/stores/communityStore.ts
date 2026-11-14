import { reactive, readonly } from 'vue'

export const communityConfigUrl = 'https://raw.githubusercontent.com/DienerTech/vectorxr/master/community.json'

type CommunityStatus = 'idle' | 'loading' | 'ready' | 'unavailable' | 'error'

// The Patreon promotion. Shown only while the published config turns it on
// with a valid Patreon URL. message is optional plain text: one string, or
// one per language code ({ "en": "...", "de": "..." }), English as fallback.
export interface PatreonPromo {
  url: string
  message: string | Record<string, string> | null
}

const patreonMessageMax = 400

function parsePatreonMessage(value: unknown): PatreonPromo['message'] {
  const clean = (text: unknown) => (typeof text === 'string' && text.trim() ? text.trim().slice(0, patreonMessageMax) : null)
  if (typeof value === 'string') return clean(value)
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null
  const messages: Record<string, string> = {}
  for (const [locale, text] of Object.entries(value)) {
    const message = clean(text)
    if (message && /^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$/.test(locale)) messages[locale] = message
  }
  return Object.keys(messages).length ? messages : null
}

export function parsePatreonPromo(config: unknown): PatreonPromo | null {
  if (!config || typeof config !== 'object' || !('patreon' in config)) return null
  const patreon = config.patreon
  if (!patreon || typeof patreon !== 'object' || Array.isArray(patreon)) return null
  const entry = patreon as Record<string, unknown>
  if (entry.enabled !== true || typeof entry.url !== 'string' || !entry.url.trim()) return null
  try {
    const url = new URL(entry.url.trim())
    const host = url.hostname === 'patreon.com' || url.hostname === 'www.patreon.com'
    if (url.protocol !== 'https:' || !host || url.username || url.password || url.port || url.pathname === '/') return null
    return { url: url.href, message: parsePatreonMessage(entry.message) }
  } catch {
    return null
  }
}

// The message for a language: an exact match, then its base language, then English.
export function patreonMessageFor(promo: PatreonPromo | null, locale: string): string | null {
  const message = promo?.message
  if (!message || typeof message === 'string') return message ?? null
  return message[locale] ?? message[locale.split('-')[0]] ?? message.en ?? null
}

// A local file whose fields replace the published config's, so a build can
// preview a change (such as turning Patreon on) before it is published.
export type CommunityOverrideLoader = () => Promise<string | null>

export function parseDiscordInvite(config: unknown): string | null {
  if (!config || typeof config !== 'object' || !('discordInvite' in config)) return null
  const invite = config.discordInvite
  if (typeof invite !== 'string' || !invite.trim()) return null
  try {
    const url = new URL(invite.trim())
    if (url.protocol !== 'https:' || url.username || url.password || url.port || url.search || url.hash) return null
    const validPath = url.hostname === 'discord.gg'
      ? /^\/[A-Za-z0-9-]+\/?$/.test(url.pathname)
      : url.hostname === 'discord.com' && /^\/invite\/[A-Za-z0-9-]+\/?$/.test(url.pathname)
    return validPath ? url.href : null
  } catch {
    return null
  }
}

export function createCommunityStore(
  fetcher: typeof fetch = (...args) => fetch(...args),
  timeoutMs = 8000,
  loadOverride: CommunityOverrideLoader = async () => null,
) {
  const state = reactive<{ status: CommunityStatus; discordInvite: string | null; patreon: PatreonPromo | null }>({
    status: 'idle',
    discordInvite: null,
    patreon: null,
  })

  async function readOverride(): Promise<Record<string, unknown> | null> {
    try {
      const text = await loadOverride()
      if (!text) return null
      const parsed: unknown = JSON.parse(text)
      return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed as Record<string, unknown> : null
    } catch {
      return null
    }
  }

  let inFlight: Promise<void> | null = null

  function load(force = false): Promise<void> {
    if (inFlight) return inFlight
    if (!force && state.status !== 'idle') return Promise.resolve()

    state.status = 'loading'
    state.discordInvite = null
    state.patreon = null
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    // The override is read alongside the request, never in its way.
    const overrideRead = readOverride()
    let override: Record<string, unknown> | null = null
    inFlight = (async () => {
      try {
        const request = fetcher(communityConfigUrl, {
          signal: controller.signal,
          cache: 'no-store',
          credentials: 'omit',
          referrerPolicy: 'no-referrer',
        })
        override = await overrideRead
        const response = await request
        if (!response.ok) throw new Error(`Community config HTTP ${response.status}`)
        const published: unknown = await response.json()
        if (controller.signal.aborted) throw new Error('Community config timed out')
        const config = override && published && typeof published === 'object' ? { ...published, ...override } : override ?? published
        state.discordInvite = parseDiscordInvite(config)
        state.patreon = parsePatreonPromo(config)
        state.status = state.discordInvite ? 'ready' : 'unavailable'
      } catch {
        // The published config failed: only a local override can still show anything.
        override = override ?? await overrideRead
        state.discordInvite = parseDiscordInvite(override)
        state.patreon = parsePatreonPromo(override)
        state.status = state.discordInvite ? 'ready' : 'error'
      } finally {
        clearTimeout(timer)
      }
    })().finally(() => { inFlight = null })
    return inFlight
  }

  return { state: readonly(state), load }
}

// Home and About share one request and one result for this app session.
// Loaded on demand so this module stays free of the Tauri bridge.
const communityStore = createCommunityStore(undefined, undefined, async () => (await import('../lib/commands')).loadCommunityOverride())
export function useCommunityStore() {
  return communityStore
}
