import { reactive, readonly } from 'vue'

export const communityConfigUrl = 'https://raw.githubusercontent.com/DienerTech/vectorxr/master/community.json'

type CommunityStatus = 'idle' | 'loading' | 'ready' | 'unavailable' | 'error'

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

export function createCommunityStore(fetcher: typeof fetch = (...args) => fetch(...args), timeoutMs = 8000) {
  const state = reactive<{ status: CommunityStatus; discordInvite: string | null }>({
    status: 'idle',
    discordInvite: null,
  })
  let inFlight: Promise<void> | null = null

  function load(force = false): Promise<void> {
    if (inFlight) return inFlight
    if (!force && state.status !== 'idle') return Promise.resolve()

    state.status = 'loading'
    state.discordInvite = null
    const controller = new AbortController()
    const timer = setTimeout(() => controller.abort(), timeoutMs)
    inFlight = (async () => {
      try {
        const response = await fetcher(communityConfigUrl, {
          signal: controller.signal,
          cache: 'no-store',
          credentials: 'omit',
          referrerPolicy: 'no-referrer',
        })
        if (!response.ok) throw new Error(`Community config HTTP ${response.status}`)
        const config: unknown = await response.json()
        if (controller.signal.aborted) throw new Error('Community config timed out')
        state.discordInvite = parseDiscordInvite(config)
        state.status = state.discordInvite ? 'ready' : 'unavailable'
      } catch {
        state.discordInvite = null
        state.status = 'error'
      } finally {
        clearTimeout(timer)
      }
    })().finally(() => { inFlight = null })
    return inFlight
  }

  return { state: readonly(state), load }
}

// Home and About share one request and one result for this app session.
const communityStore = createCommunityStore()
export function useCommunityStore() {
  return communityStore
}
