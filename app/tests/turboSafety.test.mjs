import assert from 'node:assert/strict'
import test from 'node:test'
import { createTurboProfile, defaultConfig, normalizeConfig } from '../src/lib/model.ts'
import { turboSafetyApplies, turboSafetyBlocks, turboFaultReason } from '../src/lib/turboSafety.ts'
import { buildHealthSummary } from '../src/lib/health.ts'

test('safety profile opt-out survives normalization and applies only to its enabled applications', () => {
  const config = defaultConfig()
  config.applications = [{ id: 'dcs', enabled: true, match: { exe: 'DCS.exe' } }]
  const profile = createTurboProfile(['dcs'])
  assert.equal(profile.disableSafety, false)
  profile.disableSafety = true
  config.modules.turbo.profiles = [profile]
  assert.equal(normalizeConfig(config).modules.turbo.profiles[0].disableSafety, true)
  assert.equal(turboSafetyApplies(config, 'dcs.EXE'), false)
  assert.equal(turboSafetyApplies(config, 'other.exe'), true)
  profile.enabled = false
  assert.equal(turboSafetyApplies(config, 'DCS.exe'), true)
  config.modules.turbo.interruptedSessionRecovery = false
  assert.equal(turboSafetyApplies(config, 'other.exe'), false)
  delete profile.disableSafety
  assert.equal(normalizeConfig(config).modules.turbo.profiles[0].disableSafety, false)
})

test('blocked setups group repeated faults by complete setup and retain the latest', () => {
  const records = [{ fingerprint: 'dcs:steamvr', updatedAtUnixMilliseconds: 10 },
    { fingerprint: 'dcs:steamvr', updatedAtUnixMilliseconds: 30 },
    { fingerprint: 'dcs:pimax', updatedAtUnixMilliseconds: 20 }]
  assert.deepEqual(turboSafetyBlocks(records), [records[1], records[2]])
  assert.match(turboFaultReason({ reason: '' }), /exact cause and crash time are unknown/)
})

test('health loading clears when asynchronous layer information arrives', () => {
  const input = { config: defaultConfig(), configPath: 'config.json', logSnapshot: null,
    openXrLayerSnapshot: null, openXrLayersLoading: true, seenApps: [] }
  const pending = buildHealthSummary(input)
  assert.equal(pending.loading, true)
  assert.ok(pending.checks.filter(check => check.id.startsWith('layer-')).every(check => check.loading))
  const complete = buildHealthSummary({ ...input, openXrLayersLoading: false, openXrLayerSnapshot: { slices: [{ layers: [{ isVectorXr: true, enabled: true, manifestExists: true, libraryExists: true }] }] } })
  assert.equal(complete.loading, false)
  assert.ok(complete.checks.every(check => !check.loading))
  assert.equal(complete.checks.find(check => check.id === 'layer-registered').state, 'pass')
})
