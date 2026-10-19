import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import { defaultConfig, defaultOsdSettings, normalizeConfig, normalizeOsdSettings, osdLayout, savedBindingConflictWarnings, savedOsdRowOrder } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'
import { compactPerformanceWidth, osdCompactLayout } from '../src/lib/osdCompactLayout.ts'
import { osdDiagnostics, osdDiagnosticsTagWidth } from '../src/lib/osdDiagnostics.ts'

test('old configs keep OSD off and acquire independent default bindings', () => {
  const config = defaultConfig()
  delete config.core.osd
  assert.deepEqual(normalizeConfig(config).core.osd, defaultOsdSettings())
  const first = defaultOsdSettings()
  first.toggleBindings[0].chord.push('X')
  assert.deepEqual(defaultOsdSettings().toggleBindings[0].chord, ['Ctrl', 'Alt', 'F10'])
})

test('all OSD settings and device bindings survive normalization and serialization', () => {
  const config = defaultConfig()
  config.core.osd = { enabled: true, visibleOnStart: false, compact: true, horizontalDegrees: -40,
    verticalDegrees: 35, distanceMeters: .5, scale: 150, opacity: 30, updateHz: 20,
    showGraph: false, showRuntime: false, showTurbo: false, showModules: false, showClock: false,
    accent: 'copper', toggleBinding: { type: 'device', deviceGuid: 'stick', inputPath: 'button-4' },
    cycleBinding: { type: 'none' } }
  // One binding per toggle, as configs from before binding lists hold them.
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.equal(normalized.core.osd.toggleBindings[0].inputPath, 'button-4')
  assert.deepEqual(normalized.core.osd.cycleBindings, [])
  assert.deepEqual(normalizeConfig(normalized), normalized)
  for (const [key, value] of Object.entries(config.core.osd)) {
    if (key !== 'toggleBinding' && key !== 'cycleBinding') assert.deepEqual(normalized.core.osd[key], value)
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
  config.core.osd.cycleBindings = structuredClone(config.core.osd.toggleBindings)
  assert.ok(savedBindingConflictWarnings(config, config.core.osd.toggleBindings).length > 0)
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
  // The tallest compact panel: header (18 + 44), metrics (68), Performance,
  // Turbo and Frame Gen rows (48 each), Pivot with its snap line (80), and 6
  // below. The layer's OSD texture is 768 rows tall.
  const worstCompactHeight = 18 + 44 + 68 + 48 + 48 + 48 + 80 + 6
  assert.ok(worstCompactHeight <= 768)
  let tallest = 0
  for (const compactMetrics of ['all', 'fps', 'frameTime', 'none']) for (let bits = 0; bits < 512; bits++) {
    const flags = ['compactShowBrand', 'compactShowApp', 'compactShowRuntime', 'compactShowClock', 'compactShowTurbo', 'compactShowPivot', 'compactShowFramegen']
    Object.assign(settings, { compactMetrics, compactShowPerformance: !!(bits & 256) }, Object.fromEntries(flags.map((key, i) => [key, !!(bits & 1 << i)])))
    const options = { framegenSession: !!(bits & 128) }
    const layout = osdCompactLayout(settings, options)
    const snapLayout = osdCompactLayout(settings, { ...options, pivotSnapLine: true })
    tallest = Math.max(tallest, snapLayout.height)
    assert.ok(layout.width >= 240 && layout.width <= 960)
    assert.ok(layout.height >= 68 && snapLayout.height <= worstCompactHeight)
    // The Snap View line only grows a shown Pivot row.
    assert.equal(snapLayout.height, layout.height + (settings.compactShowPivot ? 32 : 0))
    assert.equal(snapLayout.width, layout.width)
    if (settings.compactShowTurbo && settings.compactShowPivot) {
      assert.equal(layout.pivotY, layout.turboY + (settings.compactShowFramegen ? 96 : 48))
      const turboOnly = osdCompactLayout({ ...settings, compactShowPivot: false }, options)
      assert.equal(layout.width, turboOnly.width)
      assert.equal(layout.height, turboOnly.height + 48)
      assert.equal(snapLayout.height, turboOnly.height + 80)
    }
    assert.equal(layout.empty, compactMetrics === 'none' && (bits & (127 | 256)) === 0)
    assert.ok(layout.fields.every(field => field.x + field.width <= layout.width - 28))
  }
  assert.equal(tallest, worstCompactHeight)
  assert.equal(normalizeOsdSettings({ compactMetrics: 'bad' }).compactMetrics, 'all')
})

test('custom layouts retain small sizes, row order and colors without capturing controls or recursive presets', () => {
  const osd = defaultOsdSettings()
  Object.assign(osd, { enabled: true, scale: 25, accent: 'custom', customColor: '#aB12Ef', clockFormat: '12', bodyOrder: ['pivot', 'modules', 'framegen', 'graph', 'turbo', 'performance'], showBrand: false, showApp: false, showRuntime: false, showClock: false })
  const layout = osdLayout(osd)
  osd.customPresets.push({ name: 'Small cockpit', settings: layout })
  osd.bodyOrder.reverse()
  assert.deepEqual(layout.bodyOrder, ['pivot', 'modules', 'framegen', 'graph', 'turbo', 'performance'])
  for (const key of ['enabled', 'visibleOnStart', 'toggleBindings', 'cycleBindings', 'customPresets']) assert.ok(!(key in layout))
  assert.deepEqual(normalizeOsdSettings(JSON.parse(JSON.stringify(osd))), osd)
  assert.deepEqual(normalizeOsdSettings({ bodyOrder: ['pivot', 'pivot', 'unknown', 'turbo'] }).bodyOrder, ['pivot', 'turbo', 'graph', 'performance', 'framegen', 'modules'])
  assert.equal(normalizeOsdSettings({ accent: 'toString' }).accent, 'teal')
  const config = defaultConfig(); config.core.osd = osd
  assert.deepEqual(validateConfig(config), [])
})

test('the performance row shows in the expanded layout by default and widens a compact panel to fit its line', () => {
  const osd = defaultOsdSettings()
  assert.equal(osd.showPerformance, true)
  assert.equal(osd.compactShowPerformance, false)
  assert.equal(osd.bodyOrder[1], 'performance')
  assert.equal(normalizeOsdSettings({ showPerformance: false }).showPerformance, false)
  assert.equal(normalizeOsdSettings({ showPerformance: 'no' }).showPerformance, true)
  assert.equal(normalizeOsdSettings({ compactShowPerformance: true }).compactShowPerformance, true)
  assert.equal(normalizeOsdSettings({ compactShowPerformance: 1 }).compactShowPerformance, false)
  const restored = normalizeOsdSettings(JSON.parse(JSON.stringify({ ...osd, showPerformance: false, compactShowPerformance: true })))
  assert.equal(restored.showPerformance, false)
  assert.equal(restored.compactShowPerformance, true)

  // Compact: first of the status rows, as the layer stacks them, and 760 wide
  // (kOsdCompactPerformanceWidth) so its widest line fits.
  const minimal = { ...osd, compactShowBrand: false, compactShowApp: false, compactShowRuntime: false, compactShowClock: false, compactMetrics: 'none' }
  const only = osdCompactLayout({ ...minimal, compactShowPerformance: true })
  assert.equal(only.empty, false)
  assert.equal(only.statusY, only.performanceY)
  assert.equal(only.width, compactPerformanceWidth)
  assert.equal(compactPerformanceWidth, 760)
  const stacked = osdCompactLayout({ ...minimal, compactShowPerformance: true, compactShowTurbo: true, compactShowFramegen: true })
  assert.equal(stacked.turboY, stacked.performanceY + 48)
  assert.equal(stacked.framegenY, stacked.turboY + 48)
  assert.equal(osdCompactLayout({ ...minimal, compactShowTurbo: true }).performanceY, -1)
  assert.equal(osdCompactLayout({ ...minimal, compactShowTurbo: true }).width, 520)

  const config = defaultConfig()
  config.core.osd.compactShowPerformance = true
  assert.deepEqual(validateConfig(config), [])
  config.core.osd.bodyOrder = ['graph', 'turbo', 'framegen', 'pivot', 'modules']
  assert.ok(validateConfig(config).includes('OSD body order must contain each row once'))
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

test('debug logging and Frame Generation diagnostic views name themselves after the OSD brand', () => {
  const config = defaultConfig()
  assert.equal(osdDiagnostics(config), '')
  config.core.logLevel = 'debug'
  assert.equal(osdDiagnostics(config), 'DEBUG LOG')
  config.modules.framegen.diagnosticView = 'tint'
  // A view only shows where Frame Generation is on.
  assert.equal(osdDiagnostics(config), 'DEBUG LOG')
  config.modules.framegen.enabled = true
  assert.equal(osdDiagnostics(config), 'DEBUG LOG \u00b7 TINT VIEW')
  config.core.logLevel = 'info'
  config.modules.framegen.diagnosticView = 'confidence'
  assert.equal(osdDiagnostics(config), 'CONFIDENCE VIEW')
  assert.equal(osdDiagnosticsTagWidth(''), 0)

  // Compact: the tag takes a slot after the brand, or leads the row without
  // it, and keeps the header row with every header item hidden.
  const settings = defaultOsdSettings()
  Object.assign(settings, { compactShowBrand: true, compactShowApp: true, compactShowRuntime: false, compactShowClock: false })
  const plain = osdCompactLayout(settings), tagged = osdCompactLayout(settings, { diagnosticsWidth: 200 })
  assert.equal(plain.diagnostics, null)
  assert.deepEqual(tagged.diagnostics, { x: 28 + 155 + 16, width: 200 })
  assert.equal(tagged.fields[1].x, plain.fields[1].x + 216)
  assert.equal(tagged.height, plain.height)
  Object.assign(settings, { compactShowBrand: false, compactShowApp: false })
  const alone = osdCompactLayout(settings, { diagnosticsWidth: 200 })
  assert.deepEqual(alone.diagnostics, { x: 28, width: 200 })
  assert.equal(alone.headerY, 18)
  assert.equal(alone.height, osdCompactLayout(settings).height + 44)
  // With every field shown, the longest tag narrows the runtime's column (and
  // the app's) instead of passing the panel's edge, as the layer does.
  Object.assign(settings, { compactShowBrand: true, compactShowApp: true, compactShowRuntime: true, compactShowClock: true })
  for (const width of [200, 300, 400]) {
    const crowded = osdCompactLayout(settings, { diagnosticsWidth: width })
    assert.ok(crowded.width <= 960)
    for (const field of crowded.fields) assert.ok(field.width >= 80)
  }
})

test('saved OSD row orders stay readable by 0.19.0-0.19.2 layers, and rowOrder wins on load', () => {
  const full = ['pivot', 'performance', 'graph', 'turbo', 'framegen', 'modules']
  const saved = savedOsdRowOrder({ ...defaultOsdSettings(), bodyOrder: full })
  assert.deepEqual(saved.bodyOrder, ['pivot', 'graph', 'turbo', 'framegen', 'modules'])
  assert.deepEqual(saved.rowOrder, full)
  assert.equal('rowOrder' in savedOsdRowOrder({ ...defaultOsdSettings(), bodyOrder: ['modules', 'graph'] }), false)
  // Read back as written: the full order, not the legacy list with the new row appended.
  assert.deepEqual(normalizeOsdSettings(saved).bodyOrder, full)
  assert.equal('rowOrder' in normalizeOsdSettings(saved), false)
  const preset = normalizeOsdSettings({ customPresets: [{ name: 'Cockpit', settings: savedOsdRowOrder({ bodyOrder: full }) }] }).customPresets[0]
  assert.deepEqual(preset.settings.bodyOrder, full)
})
