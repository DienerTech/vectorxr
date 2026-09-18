import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import { defaultConfig, defaultOsdSettings, normalizeConfig, normalizeOsdSettings, savedBindingConflictWarnings } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

test('old configs keep OSD off and acquire independent default bindings', () => {
  const config = defaultConfig()
  delete config.core.osd
  assert.deepEqual(normalizeConfig(config).core.osd, defaultOsdSettings())
  const first = defaultOsdSettings()
  first.toggleBinding.chord.push('X')
  assert.deepEqual(defaultOsdSettings().toggleBinding.chord, ['Ctrl', 'Alt', 'F10'])
})

test('all OSD settings and device bindings survive normalization and serialization', () => {
  const config = defaultConfig()
  config.core.osd = { enabled: true, visibleOnStart: false, compact: true, horizontalDegrees: -40,
    verticalDegrees: 35, distanceMeters: .5, scale: 150, opacity: 30, updateHz: 20,
    showGraph: false, showRuntime: false, showTurbo: false, showModules: false, showClock: false,
    accent: 'copper', toggleBinding: { type: 'device', deviceGuid: 'stick', inputPath: 'button-4' },
    cycleBinding: { type: 'none' } }
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.equal(normalized.core.osd.toggleBinding.inputPath, 'button-4')
  assert.deepEqual(normalizeConfig(normalized), normalized)
  for (const [key, value] of Object.entries(config.core.osd)) {
    if (key !== 'toggleBinding') assert.deepEqual(normalized.core.osd[key], value)
  }
  assert.deepEqual(validateConfig(normalized), [])
})

test('invalid OSD imports fall back safely and invalid edits prevent save', () => {
  const normalized = normalizeOsdSettings({ enabled: 'true', horizontalDegrees: Infinity, verticalDegrees: -90,
    distanceMeters: 0, opacity: 1, scale: 400, updateHz: 2.5, accent: 'red' })
  assert.deepEqual(normalized, defaultOsdSettings())
  const config = defaultConfig()
  config.core.osd.updateHz = 0
  config.core.osd.distanceMeters = NaN
  assert.equal(validateConfig(config).filter(error => error.startsWith('OSD')).length, 2)
})

test('OSD bindings participate in global binding conflicts', () => {
  const config = defaultConfig()
  config.core.osd.cycleBinding = structuredClone(config.core.osd.toggleBinding)
  assert.ok(savedBindingConflictWarnings(config, [config.core.osd.toggleBinding]).length > 0)
})

test('public schema describes every OSD field and keeps it optional for old configs', () => {
  const schema = JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json', import.meta.url), 'utf8'))
  assert.equal(schema.$defs.core.properties.osd.$ref, '#/$defs/osdSettings')
  assert.ok(!schema.$defs.core.required.includes('osd'))
  const definition = schema.$defs.osdSettings
  for (const key of Object.keys(defaultOsdSettings())) assert.ok(key in definition.properties, key)
  assert.equal(definition.properties.enabled.default, false)
  assert.equal(definition.properties.opacity.minimum, 30)
  assert.equal(definition.properties.updateHz.maximum, 20)
})
