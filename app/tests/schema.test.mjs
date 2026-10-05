import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import { createFrameGenProfile, createPivotQuickView, createTurboProfile, defaultConfig, defaultFrameGenSettings } from '../src/lib/model.ts'

const schema = JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json', import.meta.url), 'utf8'))

test('schema accepts generated Quadviews settings and both eye correction modes', () => {
  const definition = schema.$defs.quadviewsSettings
  assertSchemaDescribesObject(definition, defaultConfig().modules.quadviews.defaults)
  assert.deepEqual(definition.properties.eyeTrackingCorrection.enum, ['default', 'flip-z'])
  assert.equal(definition.properties.eyeTrackingCorrection.default, 'default')
})

test('schema permits Turbo settings and defaults interrupted-session recovery on', () => {
  assertSchemaDescribesObject(schema.$defs.turboModule, defaultConfig().modules.turbo)
  assertSchemaDescribesObject(schema.$defs.turboModule.properties.profiles.items, createTurboProfile())
  assert.equal(schema.$defs.turboModule.properties.interruptedSessionRecovery.default, true)
  assert.ok('turbo' in schema.$defs.modules.properties)
})

test('schema describes generated Frame Generation settings and keeps the module optional', () => {
  const framegen = defaultConfig().modules.framegen
  assertSchemaDescribesObject(schema.$defs.framegenModule, framegen)
  assertSchemaDescribesObject(schema.$defs.framegenSettings, framegen.defaults)
  assertSchemaDescribesObject(schema.$defs.framegenProfile, createFrameGenProfile(defaultFrameGenSettings(), ['dcs']))
  assert.equal(schema.$defs.modules.properties.framegen.$ref, '#/$defs/framegenModule')
  assert.ok(!schema.$defs.modules.required.includes('framegen'))

  const settings = schema.$defs.framegenSettings.properties
  for (const [key, value] of Object.entries(framegen.defaults)) assert.equal(settings[key].default, value, key)
  assert.deepEqual(settings.synthesisMode.enum, ['interpolate', 'extrapolate'])
  assert.deepEqual(settings.flowQuality.enum, ['performance', 'balanced', 'quality'])
  assert.deepEqual(settings.motionEstimation.enum, ['shader', 'hardware'])
  assert.deepEqual(settings.frameMultiplier.enum, ['auto', '2x', '3x', '4x'])
  assert.deepEqual([settings.artifactSuppression.minimum, settings.artifactSuppression.maximum], [0, 100])
  assert.deepEqual([settings.motionLimitPercent.minimum, settings.motionLimitPercent.maximum], [2, 25])
  assert.deepEqual(schema.$defs.framegenModule.properties.diagnosticView.enum, ['off', 'flow', 'confidence', 'tint'])
  assert.equal(schema.$defs.framegenModule.properties.diagnosticView.default, framegen.diagnosticView)

  const osd = schema.$defs.osdSettings.properties
  assert.deepEqual(osd.bodyOrder.default, defaultConfig().core.osd.bodyOrder)
  assert.deepEqual(osd.bodyOrder.items.enum, defaultConfig().core.osd.bodyOrder)
  assert.deepEqual(osd.customPresets.items.properties.settings.properties.bodyOrder.items.enum, defaultConfig().core.osd.bodyOrder)
  assert.equal(osd.showFramegen.default, true)
  assert.equal(osd.compactShowFramegen.default, false)
})

function assertSchemaDescribesObject(definition, value) {
  const valueKeys = Object.keys(value)
  for (const key of valueKeys) {
    assert.ok(key in definition.properties, `schema does not allow generated property ${key}`)
  }
  for (const key of definition.required ?? []) {
    assert.ok(key in value, `generated object is missing schema-required property ${key}`)
  }
}

test('schema describes generated Pivot Nudge Sets', () => {
  const nudgeSet = defaultConfig().modules.pivotxr.nudgeSets[0]
  const definition = schema.$defs.pivotNudgeSet

  assertSchemaDescribesObject(definition, nudgeSet)
  assert.equal(definition.properties.allowWhileInactive.type, 'boolean')
  assert.ok(definition.required.includes('allowWhileInactive'))
})

test('schema describes generated Quick Views while accepting the legacy turn direction', () => {
  const quickView = createPivotQuickView('Check Six')
  const definition = schema.$defs.pivotQuickView

  assertSchemaDescribesObject(definition, quickView)
  assert.ok(!definition.required.includes('turnDirection'))
  assert.deepEqual(definition.properties.turnDirection.enum, ['left', 'right'])
})
