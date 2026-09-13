import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import { createPivotQuickView, createTurboProfile, defaultConfig } from '../src/lib/model.ts'

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
