import assert from 'node:assert/strict'
import test from 'node:test'
import supporterCredits from '../../config/supporter-credits.json' with { type: 'json' }
import { createSupportersStore, supportersFeedUrl, supportersUrl } from '../src/stores/supportersStore.ts'

const week = 7 * 24 * 60 * 60 * 1000
const feed = tiers => ({ version: 1, tiers })
const original = feed([{ label: 'Launch crew', names: ['Aurora', 'Orbit'] }, { label: 'Pilots', names: ['Arc'] }])
const response = document => Response.json(document)
const offline = async () => { throw new Error('offline') }

function fixture(t) {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: Date.UTC(2026, 0, 1) })
  const records = new Map()
  const storage = {
    getItem: key => records.get(key) ?? null,
    setItem: (key, value) => records.set(key, value),
    removeItem: key => records.delete(key),
  }
  return { storage, records }
}

test('nested feed branches use an explicit heads ref and preserve file subdirectories', () => {
  assert.equal(supportersFeedUrl({ repository: 'sample/project', branch: 'data/credits', path: 'public/list.json' }),
    'https://raw.githubusercontent.com/sample/project/refs/heads/data/credits/public/list.json')
})

test('the page shares a fixed public, credential-free request and caches only public labels and names', async t => {
  const { storage, records } = fixture(t)
  let complete
  let calls = 0
  const store = createSupportersStore((url, options) => {
    calls++
    const destination = new URL(url)
    assert.equal(destination.origin, 'https://raw.githubusercontent.com')
    const { repository, branch, path } = supporterCredits.feed
    assert.deepEqual(destination.pathname.slice(1).split('/').map(decodeURIComponent), [
      ...repository.split('/'), ...(branch.includes('/') ? ['refs', 'heads'] : []), ...branch.split('/'), ...path.split('/'),
    ])
    assert.equal(url, supportersUrl)
    assert.equal(options.credentials, 'omit')
    assert.equal(options.referrerPolicy, 'no-referrer')
    assert.equal(options.cache, 'no-store')
    assert.equal(options.redirect, 'error')
    assert.equal(options.headers, undefined)
    return new Promise(resolve => { complete = resolve })
  }, storage)
  const first = store.load()
  assert.equal(store.state.status, 'loading')
  assert.equal(store.load(true), first)
  complete(response({ ...feed([
    { label: ' Crew ', names: [' e\u0301 ', '🧑‍🚀', '🧑‍🚀'], internal: 'unpublished' },
    { label: 'Second', names: [] },
  ]), internal: 'unpublished' }))
  await first
  assert.equal(store.state.status, 'ready')
  const tiers = [{ label: 'Crew', names: ['é', '🧑‍🚀', '🧑‍🚀'] }, { label: 'Second', names: [] }]
  assert.deepEqual(store.state.tiers, tiers)
  assert.deepEqual(JSON.parse([...records.values()][0]), { version: 1, source: supportersUrl, fetchedAt: Date.now(), tiers })
  await store.load()
  assert.equal(calls, 1)
  t.mock.timers.tick(60 * 60 * 1000)
  const later = store.load()
  assert.equal(calls, 2)
  complete(response(feed([])))
  await later
})

test('offline launches reuse the validated list and malformed refreshes leave its contents and expiry intact', async t => {
  const { storage, records } = fixture(t)
  await createSupportersStore(async () => response(original), storage).load()
  const saved = [...records.values()][0]
  t.mock.timers.tick(week - 60 * 1000)
  const store = createSupportersStore(offline, storage)
  const pending = store.load()
  assert.deepEqual(store.state.tiers, original.tiers)
  await pending
  assert.equal(store.state.status, 'cached')
  for (const bad of [
    null, [], { version: 2, tiers: [] }, {}, feed({}), feed([null]),
    feed([{ label: 'Crew', names: 'Name' }]), feed([{ label: '   ', names: [] }]),
    feed([{ label: 'Crew', names: [1] }]), feed([{ label: 'Crew', names: ['two\nlines'] }]),
    feed([{ label: 'Crew', names: ['bad\u202etext'] }]), feed([{ label: 'Crew', names: ['x'.repeat(81)] }]),
  ]) {
    const reload = createSupportersStore(async () => response(bad), storage)
    await reload.load()
    assert.equal(reload.state.status, 'cached')
    assert.deepEqual(reload.state.tiers, original.tiers)
    assert.equal([...records.values()][0], saved)
  }
  t.mock.timers.tick(60 * 1000)
  assert.equal(store.state.status, 'unavailable')
  assert.deepEqual(store.state.tiers, [])
  assert.equal(records.size, 0)
})

test('a new valid list replaces dropped names and a valid empty list survives an offline relaunch', async t => {
  const { storage } = fixture(t)
  let document = original
  const store = createSupportersStore(async () => response(document), storage)
  await store.load()
  document = feed([{ label: 'Pilots', names: ['Arc'] }])
  await store.load(true)
  assert.deepEqual(store.state.tiers, document.tiers)
  const restarted = createSupportersStore(offline, storage)
  await restarted.load()
  assert.deepEqual(restarted.state.tiers, document.tiers)
  document = feed([])
  await store.load(true)
  assert.equal(store.state.status, 'ready')
  assert.deepEqual(store.state.tiers, [])
  const empty = createSupportersStore(offline, storage)
  await empty.load()
  assert.equal(empty.state.status, 'cached')
  assert.deepEqual(empty.state.tiers, [])
})

test('temporary HTTP errors keep credits while a removed feed clears even the on-disk fallback', async t => {
  const { storage, records } = fixture(t)
  for (const removed of [404, 410]) {
    await createSupportersStore(async () => response(original), storage).load()
    let status = 503
    const store = createSupportersStore(async () => new Response(null, { status }), storage)
    await store.load()
    assert.deepEqual(store.state.tiers, original.tiers)
    assert.equal(store.state.status, 'cached')
    status = removed
    await store.load(true)
    // GitHub answered: nothing is published there, which is not a failure.
    assert.equal(store.state.status, 'unpublished')
    assert.deepEqual(store.state.tiers, [])
    assert.equal(records.size, 0)
    const restart = createSupportersStore(offline, storage)
    await restart.load()
    assert.equal(restart.state.status, 'unavailable')
  }
})

test('a cache from an earlier preview or a different feed is cleared even when offline', async t => {
  const { storage, records } = fixture(t)
  await createSupportersStore(async () => response(original), storage).load()
  const [key] = records.keys()
  for (const source of [undefined, supportersUrl + '.old']) {
    storage.setItem(key, JSON.stringify({ ...original, fetchedAt: Date.now(), source }))
    const store = createSupportersStore(offline, storage)
    const pending = store.load()
    assert.deepEqual(store.state.tiers, [])
    await pending
    assert.equal(store.state.status, 'unavailable')
    assert.equal(records.size, 0)
  }
})

test('expired, future-dated and corrupted cache files never supply names', async t => {
  const { storage, records } = fixture(t)
  await createSupportersStore(async () => response(original), storage).load()
  const [key] = records.keys()
  for (const raw of ['{broken', 'null', ...[Date.now() - week, Date.now() + 1000, 'yesterday']
    .map(fetchedAt => JSON.stringify({ ...original, source: supportersUrl, fetchedAt }))]) {
    storage.setItem(key, raw)
    const store = createSupportersStore(offline, storage)
    const pending = store.load()
    assert.deepEqual(store.state.tiers, [])
    await pending
    assert.equal(store.state.status, 'unavailable')
    assert.equal(records.size, 0)
  }
})

test('if persisting a replacement fails, an earlier list cannot be restored at next launch', async t => {
  const { storage, records } = fixture(t)
  let document = original
  const store = createSupportersStore(async () => response(document), storage)
  await store.load()
  storage.setItem = () => { throw new Error('quota exceeded') }
  document = feed([])
  await store.load(true)
  assert.equal(store.state.status, 'ready')
  assert.deepEqual(store.state.tiers, [])
  assert.equal(records.size, 0)
  const offlineLaunch = createSupportersStore(offline, storage)
  await offlineLaunch.load()
  assert.equal(offlineLaunch.state.status, 'unavailable')
})

test('a slow request times out, then a retry works when cache storage throws', async t => {
  fixture(t)
  const denied = () => { throw new Error('storage disabled') }
  let signal
  const store = createSupportersStore((_url, options) => {
    if (signal) return Promise.resolve(response(original))
    signal = options.signal
    return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(new Error('timeout'))))
  }, { getItem: denied, setItem: denied, removeItem: denied }, 10)
  const pending = store.load()
  t.mock.timers.tick(10)
  await pending
  assert.equal(signal.aborted, true)
  assert.equal(store.state.status, 'unavailable')
  await store.load(true)
  assert.equal(store.state.status, 'ready')
  assert.deepEqual(store.state.tiers, original.tiers)
})
