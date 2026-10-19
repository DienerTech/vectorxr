import assert from 'node:assert/strict'
import test from 'node:test'

import { buildHealthSummary } from '../src/lib/health.ts'
import { defaultConfig } from '../src/lib/model.ts'
import { loadTrayEnabled, storeTrayEnabled, trayStatus } from '../src/lib/tray.ts'
import { createTaskQueue } from '../src/lib/taskQueue.ts'

function health(config) {
  return buildHealthSummary({
    config,
    configPath: 'C:/settings.json',
    logSnapshot: null,
    openXrLayerSnapshot: null,
    openXrLayersLoading: true,
    seenApps: [],
  })
}

test('the tray shows the saved switch, log level and health', () => {
  const saved = defaultConfig()
  saved.core.enabled = false
  saved.core.logLevel = 'debug'
  assert.deepEqual(trayStatus(saved, health(saved), true), {
    available: true,
    healthLabel: 'Needs attention',
    enabled: false,
    logLevel: 'debug',
  })
  saved.core.logLevel = 'verbose'
  assert.equal(trayStatus(saved, health(saved), false).logLevel, 'info')
  assert.equal(trayStatus(saved, health(saved), false).available, false)
})

test('the tray is on or off: on by default, and both older modes with an icon are on', () => {
  assert.equal(loadTrayEnabled(), true)
  const stored = new Map()
  globalThis.window = {
    localStorage: { getItem: (key) => stored.get(key) ?? null, setItem: (key, value) => stored.set(key, String(value)) },
  }
  try {
    for (const [value, enabled] of [['off', false], ['on', true], ['keepRunning', true], [null, true]]) {
      if (value === null) stored.delete('vectorxr-tray-mode')
      else stored.set('vectorxr-tray-mode', value)
      assert.equal(loadTrayEnabled(), enabled, `stored ${value}`)
    }
    storeTrayEnabled(false)
    assert.equal(stored.get('vectorxr-tray-mode'), 'off')
    storeTrayEnabled(true)
    assert.equal(stored.get('vectorxr-tray-mode'), 'on')
  } finally {
    delete globalThis.window
  }
})

test('queued saves run one at a time, in order, past a failure', async () => {
  const queue = createTaskQueue()
  const events = []
  const slow = queue(async () => {
    events.push('first start')
    await new Promise((resolve) => setTimeout(resolve, 20))
    events.push('first end')
    return 1
  })
  const failing = queue(async () => {
    events.push('second')
    throw new Error('disk full')
  })
  const last = queue(async () => {
    events.push('third')
    return 3
  })
  assert.equal(await slow, 1)
  await assert.rejects(failing, /disk full/)
  assert.equal(await last, 3)
  assert.deepEqual(events, ['first start', 'first end', 'second', 'third'])
})
