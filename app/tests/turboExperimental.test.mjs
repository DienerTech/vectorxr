import assert from 'node:assert/strict'
import test from 'node:test'
import { defaultConfig, normalizeConfig, defaultTurboExperimental, normalizeTurboExperimental, toolkitInspiredTurboExperimental, createTurboProfile } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

test('old configs retain normal Turbo timing with no experimental targets', () => {
  const config = defaultConfig()
  delete config.modules.turbo.experimental
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, defaultTurboExperimental())
})

test('orphaned legacy controls remain available for explicit profile assignment', () => {
  const config = defaultConfig()
  config.modules.turbo.experimental = { enabled: false, applicationIds: ['dcs'], waitForSubmit: true,
    sampleAtEntry: true, predictionPercent: 75, frameLimit: 45 }
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, { ...config.modules.turbo.experimental, applicationIds: [] })
  config.modules.turbo.experimental.enabled = true
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, { ...config.modules.turbo.experimental, applicationIds: [] })
})

test('invalid imported experimental numbers return to neutral values and editor validation rejects them', () => {
  assert.deepEqual(normalizeTurboExperimental({ enabled: 'yes', applicationIds: [3, 'dcs', 'dcs'],
    predictionPercent: 200, frameLimit: 3.5 }), { ...defaultTurboExperimental(), applicationIds: ['dcs'] })
  const config = defaultConfig()
  config.modules.turbo.experimental.predictionPercent = 101
  config.modules.turbo.experimental.frameLimit = 1
  const errors = validateConfig(config)
  assert.ok(errors.some(value => value.includes('prediction dampening')))
  assert.ok(errors.some(value => value.includes('frame cap')))
})

test('legacy trace toggle is discarded without changing the central log level', () => {
  const config = defaultConfig()
  config.modules.turbo.experimental.timingTrace = true
  const normalized = normalizeConfig(config)
  assert.equal(normalized.core.logLevel, 'info')
  assert.equal('timingTrace' in normalized.modules.turbo.experimental, false)
})

test('Toolkit starting point keeps application scope and resets optional adjustments', () => {
  const config = defaultConfig()
  const ids = ['dcs']
  config.modules.turbo.experimental = toolkitInspiredTurboExperimental(ids)
  ids.push('other')
  assert.deepEqual(config.modules.turbo.experimental, { ...defaultTurboExperimental(),
    applicationIds: ['dcs'], enabled: true, waitForSubmit: true, sampleAtEntry: true })
  assert.equal(config.modules.turbo.enabled, false)
  assert.equal(config.core.logLevel, 'info')
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, { ...config.modules.turbo.experimental, applicationIds: [] })
})


test('custom profiles keep independent timing through save round-trip', () => {
  const config = defaultConfig()
  const first = createTurboProfile(['dcs']), second = createTurboProfile(['dcs'])
  first.experimental = toolkitInspiredTurboExperimental()
  second.experimental = { ...toolkitInspiredTurboExperimental(), predictionPercent: 80, frameLimit: 45 }
  config.modules.turbo.profiles = [first, second]
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(normalized.modules.turbo.profiles, config.modules.turbo.profiles)
  normalized.modules.turbo.profiles[0].experimental.predictionPercent = 99
  assert.equal(normalized.modules.turbo.profiles[1].experimental.predictionPercent, 80)
  normalized.modules.turbo.profiles[1].experimental.frameLimit = 1
  assert.ok(validateConfig(normalized).some(error => error.includes('frame cap')))
})

test('legacy scope migrates without enabling timing for unrelated profile applications', () => {
  const config = defaultConfig()
  config.applications = [{ id: 'dcs', name: 'DCS', enabled: true, match: { exe: 'DCS.exe' } }, { id: 'other', name: 'Other', enabled: true, match: { exe: 'Other.exe' } }]
  config.modules.turbo.experimental = toolkitInspiredTurboExperimental(['dcs'])
  const profile = createTurboProfile(['dcs', 'other']); delete profile.experimental
  config.modules.turbo.profiles = [profile]
  const normalized = normalizeConfig(config)
  const profiles = normalized.modules.turbo.profiles
  assert.equal(profiles.length, 2)
  assert.deepEqual(profiles.find(p => p.experimental.enabled).applicationIds, ['dcs'])
  assert.deepEqual(profiles.find(p => !p.experimental.enabled).applicationIds, ['other'])
  assert.deepEqual(normalizeConfig(normalized), normalized)
  assert.deepEqual(normalized.modules.turbo.experimental, defaultTurboExperimental())
  normalized.modules.turbo.profiles = []
  assert.deepEqual(normalizeConfig(normalized).modules.turbo.profiles, [])
})

test('legacy unassigned timing is retained without silently applying it to a game', () => {
  const config = defaultConfig(); config.modules.turbo.experimental = toolkitInspiredTurboExperimental()
  const profile = createTurboProfile(['dcs']); delete profile.experimental
  config.modules.turbo.profiles = [profile]
  const normalized = normalizeConfig(config)
  assert.equal(normalized.modules.turbo.profiles[0].experimental.enabled, false)
  assert.equal(normalized.modules.turbo.experimental.enabled, true)
})

test('diagnostics label flights by Turbo strategy', async () => {
  const { turboSessionStrategy } = await import('../src/lib/turboDiagnostics.ts')
  const bucket = state => ({ state })
  assert.equal(turboSessionStrategy({ timingConfiguration: 'Normal timing; trace=x', buckets: [bucket('off'), bucket('async')] }), 'VectorXR · Async')
  assert.equal(turboSessionStrategy({ buckets: [bucket('sequenced')] }), 'VectorXR · Sequenced')
  assert.equal(turboSessionStrategy({ timingConfiguration: 'Experimental Async; submit=wait; sample=entry; prediction=100%; clock=available; cap=0; trace=x', buckets: [] }), 'Toolkit-style')
  assert.equal(turboSessionStrategy({ timingConfiguration: 'Experimental Async; submit=wait; sample=entry; prediction=90%; clock=available; cap=0; trace=x', buckets: [] }), 'Custom')
  assert.equal(turboSessionStrategy({ buckets: [bucket('off')] }), 'Unknown strategy')
})
