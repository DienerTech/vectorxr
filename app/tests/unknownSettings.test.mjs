import assert from 'node:assert/strict'
import test from 'node:test'
import { defaultConfig, normalizeConfig } from '../src/lib/model.ts'

// Settings another edition of VectorXR (or a later version) writes to the
// same file are kept when this app saves, not dropped.
test('settings the app does not know are kept through normalization', () => {
  const config = JSON.parse(JSON.stringify(defaultConfig()))
  config.laterTopLevel = { kept: true }
  config.modules.crop = { enabled: true, defaults: { top: 10 } }
  config.modules.framegen.defaults.predictionDampening = 50
  config.modules.framegen.profiles = [
    { name: 'DCS', enabled: true, applicationIds: [], settings: { ...config.modules.framegen.defaults, predictionDampening: 25 } },
  ]
  const normalized = normalizeConfig(config)
  assert.deepEqual(normalized.laterTopLevel, { kept: true })
  assert.deepEqual(normalized.modules.crop, { enabled: true, defaults: { top: 10 } })
  assert.equal(normalized.modules.framegen.defaults.predictionDampening, 50)
  assert.equal(normalized.modules.framegen.profiles[0].settings.predictionDampening, 25)
  // Known settings are still normalized, not copied as they are.
  const off = JSON.parse(JSON.stringify(defaultConfig()))
  off.modules.framegen.defaults.extendHeadPrediction = false
  assert.equal('extendHeadPrediction' in normalizeConfig(off).modules.framegen.defaults, false)
})
