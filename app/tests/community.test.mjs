import assert from 'node:assert/strict'
import test from 'node:test'
import { createCommunityStore, communityConfigUrl, parseDiscordInvite, parsePatreonPromo, patreonMessageFor } from '../src/stores/communityStore.ts'

const invite = 'https://discord.gg/UyCt3M4n7N'
const response = (value) => new Response(JSON.stringify(value))

test('community config accepts only HTTPS Discord invite destinations', () => {
  assert.equal(parseDiscordInvite({ discordInvite: invite }), invite)
  assert.equal(parseDiscordInvite({ discordInvite: 'https://discord.com/invite/abc-123' }), 'https://discord.com/invite/abc-123')
  for (const value of [null, {}, [], { discordInvite: null }, { discordInvite: 1 }, ...[
    '', 'not a URL', 'http://discord.gg/code', 'https://discord.gg.evil.test/code',
    'https://evil.test/discord.gg/code', 'https://discord.gg/', 'https://discord.com/channels/123',
    'https://user:password@discord.gg/code', 'https://discord.gg/code?target=elsewhere',
    'https://discord.gg/code#fragment', 'https://discord.gg:444/code',
  ].map(discordInvite => ({ discordInvite }))]) {
    assert.equal(parseDiscordInvite(value), null, JSON.stringify(value))
  }
})

test('both consumers share a pending request and a session result', async () => {
  let resolve
  let calls = 0
  const store = createCommunityStore((url, options) => {
    calls++
    assert.equal(url, communityConfigUrl)
    assert.equal(options.cache, 'no-store')
    return new Promise(done => { resolve = done })
  })
  const first = store.load()
  assert.equal(store.state.status, 'loading')
  assert.equal(store.state.discordInvite, null)
  assert.equal(store.load(), first)
  resolve(response({ discordInvite: invite }))
  await first
  assert.equal(store.state.status, 'ready')
  assert.equal(store.state.discordInvite, invite)
  await store.load()
  assert.equal(calls, 1)
})

test('null documents and missing, null, or invalid properties disable joining', async () => {
  for (const value of [null, {}, { discordInvite: null }, { discordInvite: '' }, { discordInvite: 'https://example.com' }]) {
    const store = createCommunityStore(async () => response(value))
    await store.load()
    assert.equal(store.state.status, 'unavailable')
    assert.equal(store.state.discordInvite, null)
  }
})

test('HTTP errors, network failures, invalid JSON, and null responses disable joining', async () => {
  for (const fetcher of [
    async () => new Response('', { status: 404 }),
    async () => new Response('', { status: 500 }),
    async () => { throw new Error('offline') },
    async () => new Response('{bad json'),
    async () => null,
  ]) {
    const store = createCommunityStore(fetcher)
    await store.load()
    assert.equal(store.state.status, 'error')
    assert.equal(store.state.discordInvite, null)
  }
})

test('timeout aborts the request and permits a successful retry', async () => {
  let calls = 0
  const store = createCommunityStore(async (_url, { signal }) => {
    if (++calls > 1) return response({ discordInvite: invite })
    return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(new Error('aborted')), { once: true }))
  }, 10)
  await store.load()
  assert.equal(store.state.status, 'error')
  assert.equal(store.state.discordInvite, null)
  await store.load(true)
  assert.equal(store.state.status, 'ready')
})

test('a refresh clears a previous invite during loading and after failure or revocation', async () => {
  let next = response({ discordInvite: invite })
  const store = createCommunityStore(async () => next)
  await store.load()
  next = new Response('', { status: 503 })
  const refresh = store.load(true)
  assert.equal(store.state.status, 'loading')
  assert.equal(store.state.discordInvite, null)
  await refresh
  assert.equal(store.state.status, 'error')
  assert.equal(store.state.discordInvite, null)
  next = response({ discordInvite: invite })
  await store.load(true)
  next = response({ discordInvite: null })
  await store.load(true)
  assert.equal(store.state.status, 'unavailable')
  assert.equal(store.state.discordInvite, null)
})

const patreonUrl = 'https://www.patreon.com/dienertech'

test('the Patreon promotion shows only when enabled with an HTTPS Patreon page', () => {
  assert.deepEqual(parsePatreonPromo({ patreon: { enabled: true, url: patreonUrl } }), { url: patreonUrl, message: null })
  assert.equal(parsePatreonPromo({ patreon: { enabled: true, url: 'https://patreon.com/c/dienertech' } }).url, 'https://patreon.com/c/dienertech')
  for (const patreon of [
    undefined, null, true, [], {}, { enabled: false, url: patreonUrl }, { enabled: 'true', url: patreonUrl }, { url: patreonUrl },
    { enabled: true }, { enabled: true, url: null }, { enabled: true, url: '' }, { enabled: true, url: 'http://www.patreon.com/dienertech' },
    { enabled: true, url: 'https://evil.test/www.patreon.com' }, { enabled: true, url: 'https://patreon.com.evil.test/x' },
    { enabled: true, url: 'https://user:pw@www.patreon.com/x' }, { enabled: true, url: 'https://www.patreon.com:444/x' },
    { enabled: true, url: 'https://www.patreon.com/' },
  ]) {
    assert.equal(parsePatreonPromo({ patreon }), null, JSON.stringify(patreon))
  }
})

test('the Patreon message is plain text, one string or one per language with English as fallback', () => {
  const one = parsePatreonPromo({ patreon: { enabled: true, url: patreonUrl, message: '  Early access builds  ' } })
  assert.equal(patreonMessageFor(one, 'de'), 'Early access builds')
  const many = parsePatreonPromo({ patreon: { enabled: true, url: patreonUrl, message: { en: 'Hello', de: 'Hallo', 'pt-BR': 'Olá', bad: 3, '<x>': 'no' } } })
  assert.deepEqual(many.message, { en: 'Hello', de: 'Hallo', 'pt-BR': 'Olá' })
  assert.equal(patreonMessageFor(many, 'de'), 'Hallo')
  assert.equal(patreonMessageFor(many, 'pt-BR'), 'Olá')
  assert.equal(patreonMessageFor(many, 'ja'), 'Hello')
  assert.equal(patreonMessageFor(parsePatreonPromo({ patreon: { enabled: true, url: patreonUrl, message: { de: 'Hallo' } } }), 'fr'), null)
  assert.equal(parsePatreonPromo({ patreon: { enabled: true, url: patreonUrl, message: 'x'.repeat(1000) } }).message.length, 400)
  assert.equal(patreonMessageFor(null, 'en'), null)
})

test('the store hides Patreon when the config is disabled, missing or fails', async () => {
  for (const fetcher of [
    async () => response({ discordInvite: invite, patreon: { enabled: false, url: patreonUrl } }),
    async () => response({ discordInvite: invite }),
    async () => new Response('', { status: 404 }),
    async () => { throw new Error('offline') },
  ]) {
    const store = createCommunityStore(fetcher)
    await store.load()
    assert.equal(store.state.patreon, null)
  }
  const store = createCommunityStore(async () => response({ discordInvite: invite, patreon: { enabled: true, url: patreonUrl } }))
  await store.load()
  assert.equal(store.state.patreon.url, patreonUrl)
  assert.equal(store.state.discordInvite, invite)
})

test('a local override replaces published fields, and still applies when the request fails', async () => {
  const override = async () => JSON.stringify({ patreon: { enabled: true, url: patreonUrl, message: 'Preview' } })
  const merged = createCommunityStore(async () => response({ discordInvite: invite, patreon: { enabled: false } }), 8000, override)
  await merged.load()
  assert.equal(merged.state.status, 'ready')
  assert.equal(merged.state.discordInvite, invite)
  assert.equal(merged.state.patreon.message, 'Preview')

  const offline = createCommunityStore(async () => { throw new Error('offline') }, 8000, override)
  await offline.load()
  assert.equal(offline.state.status, 'error')
  assert.equal(offline.state.discordInvite, null)
  assert.equal(offline.state.patreon.url, patreonUrl)

  for (const broken of [async () => '{bad json', async () => '[]', async () => { throw new Error('no file') }, async () => null]) {
    const store = createCommunityStore(async () => response({ discordInvite: invite }), 8000, broken)
    await store.load()
    assert.equal(store.state.status, 'ready')
    assert.equal(store.state.patreon, null)
  }
})
