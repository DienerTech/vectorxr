import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import { defaultConfig, defaultOsdSettings, normalizeConfig, normalizeOsdSettings, osdLayout, savedBindingConflictWarnings } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'
import { osdCompactLayout } from '../src/lib/osdCompactLayout.ts'

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

test('compact contents have independent defaults and survive presets', () => {
  const legacy = normalizeOsdSettings({ showRuntime: false, showClock: false, showTurbo: true, showPivot: true })
  assert.equal(legacy.compactShowRuntime, true)
  assert.equal(legacy.compactShowClock, true)
  assert.equal(legacy.compactShowTurbo, false)
  assert.equal(legacy.compactShowPivot, false)
  Object.assign(legacy, { compactShowRuntime: true, compactShowClock: true, compactShowTurbo: true, compactShowPivot: true, showTurbo: false, showPivot: false, compactShowBrand: false, compactShowApp: false, compactMetrics: 'fps' })
  legacy.customPresets.push({ name: 'Compact states', settings: osdLayout(legacy) })
  const restored = normalizeOsdSettings(JSON.parse(JSON.stringify(legacy)))
  assert.deepEqual(restored, legacy)
  assert.equal(restored.customPresets[0].settings.compactShowPivot, true)
  assert.equal(normalizeOsdSettings({ compactShowTurbo: 'true' }).compactShowTurbo, false)
  assert.equal(normalizeOsdSettings({ showRuntime: false, compactShowRuntime: true }).compactShowRuntime, true)
})

test('compact layout packs selected rows and preserves a small FPS-only footprint', () => {
  const settings = defaultOsdSettings()
  assert.equal(osdCompactLayout(settings).height, 136)
  Object.assign(settings, { compactShowBrand: false, compactShowApp: false, compactShowRuntime: false, compactShowClock: false, compactMetrics: 'fps' })
  assert.equal(osdCompactLayout(settings).width, 266)
  assert.equal(osdCompactLayout(settings).height, 92)
  settings.compactMetrics = 'frameTime'
  assert.equal(osdCompactLayout(settings).width, 356)
  settings.compactMetrics = 'none'
  assert.equal(osdCompactLayout(settings).empty, true)
  settings.compactShowClock = true
  assert.equal(osdCompactLayout(settings).headerY, 18)
  assert.equal(osdCompactLayout(settings).metricsY, -1)
  assert.equal(osdCompactLayout(settings).height, 68)
  for (const compactMetrics of ['all', 'fps', 'frameTime', 'none']) for (let bits = 0; bits < 64; bits++) {
    const flags = ['compactShowBrand', 'compactShowApp', 'compactShowRuntime', 'compactShowClock', 'compactShowTurbo', 'compactShowPivot']
    Object.assign(settings, { compactMetrics }, Object.fromEntries(flags.map((key, i) => [key, !!(bits & 1 << i)])))
    const layout = osdCompactLayout(settings)
    assert.ok(layout.width >= 240 && layout.width <= 960)
    assert.ok(layout.height >= 68 && layout.height <= 232)
    if (settings.compactShowTurbo && settings.compactShowPivot) {
      assert.equal(layout.pivotY, layout.turboY + 48)
      const turboOnly = osdCompactLayout({ ...settings, compactShowPivot: false })
      assert.equal(layout.width, turboOnly.width)
      assert.equal(layout.height, turboOnly.height + 48)
    }
    assert.equal(layout.empty, compactMetrics === 'none' && bits === 0)
    assert.ok(layout.fields.every(field => field.x + field.width <= layout.width - 28))
  }
  assert.equal(normalizeOsdSettings({ compactMetrics: 'bad' }).compactMetrics, 'all')
})

test('custom layouts retain small sizes, row order and colors without capturing controls or recursive presets', () => {
  const osd = defaultOsdSettings()
  Object.assign(osd, { enabled: true, scale: 25, accent: 'custom', customColor: '#aB12Ef', clockFormat: '12', bodyOrder: ['pivot', 'modules', 'graph', 'turbo'], showBrand: false, showApp: false, showRuntime: false, showClock: false })
  const layout = osdLayout(osd)
  osd.customPresets.push({ name: 'Small cockpit', settings: layout })
  osd.bodyOrder.reverse()
  assert.deepEqual(layout.bodyOrder, ['pivot', 'modules', 'graph', 'turbo'])
  for (const key of ['enabled', 'visibleOnStart', 'toggleBinding', 'cycleBinding', 'customPresets']) assert.ok(!(key in layout))
  assert.deepEqual(normalizeOsdSettings(JSON.parse(JSON.stringify(osd))), osd)
  assert.deepEqual(normalizeOsdSettings({ bodyOrder: ['pivot', 'pivot', 'unknown', 'turbo'] }).bodyOrder, ['pivot', 'turbo', 'graph', 'modules'])
  assert.equal(normalizeOsdSettings({ accent: 'toString' }).accent, 'teal')
  const config = defaultConfig(); config.core.osd = osd
  assert.deepEqual(validateConfig(config), [])
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
