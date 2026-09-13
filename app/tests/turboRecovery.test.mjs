import assert from 'node:assert/strict'
import test from 'node:test'
import { defaultConfig, normalizeConfig } from '../src/lib/model.ts'
import { preferredTurboSession } from '../src/lib/turboDiagnostics.ts'

test('Turbo recovery defaults on for old configs and preserves opt-out', () => {
  const config = defaultConfig()
  assert.equal(config.modules.turbo.interruptedSessionRecovery, true)
  delete config.modules.turbo.interruptedSessionRecovery
  assert.equal(normalizeConfig(config).modules.turbo.interruptedSessionRecovery, true)
  config.modules.turbo.interruptedSessionRecovery = false
  assert.equal(normalizeConfig(config).modules.turbo.interruptedSessionRecovery, false)
})

test('Turbo diagnostics prefer live or comparable sessions and honor manual selection', () => {
  const short = { sessionId: 'short', buckets: [{ state: 'off', seconds: 3 }] }
  const compared = { sessionId: 'compared', buckets: [{ state: 'off', seconds: 40 }, { state: 'async', seconds: 90 }] }
  const live = { sessionId: 'live', live: true, buckets: [] }
  assert.equal(preferredTurboSession([short, compared]), compared)
  assert.equal(preferredTurboSession([short, compared, live]), live)
  assert.equal(preferredTurboSession([short, compared, live], 'short'), short)
  assert.equal(preferredTurboSession([]), null)
})
