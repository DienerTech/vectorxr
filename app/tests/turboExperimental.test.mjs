import assert from 'node:assert/strict'
import test from 'node:test'
import { defaultConfig, normalizeConfig, defaultTurboExperimental, normalizeTurboExperimental } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

test('old configs retain normal Turbo timing with no experimental targets', () => {
  const config = defaultConfig()
  delete config.modules.turbo.experimental
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, defaultTurboExperimental())
})

test('experimental controls and diagnostic-only capture survive configuration round trip', () => {
  const config = defaultConfig()
  config.modules.turbo.experimental = { enabled: false, applicationIds: ['dcs'], waitForSubmit: true,
    sampleAtEntry: true, predictionPercent: 75, frameLimit: 45, timingTrace: true }
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, config.modules.turbo.experimental)
  config.modules.turbo.experimental.enabled = true
  assert.deepEqual(normalizeConfig(config).modules.turbo.experimental, config.modules.turbo.experimental)
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
