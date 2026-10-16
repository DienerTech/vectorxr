import assert from 'node:assert/strict'
import test from 'node:test'
import {
  createFrameGenProfile,
  defaultConfig,
  defaultFrameGenModule,
  defaultFrameGenSettings,
  defaultOsdSettings,
  frameGenFixedMultipliers,
  moduleStateForApplication,
  normalizeConfig,
  normalizeFrameGenSettings,
  normalizeOsdSettings,
  savedBindingConflictWarnings,
} from '../src/lib/model.ts'
import { configWarnings, validateConfig } from '../src/lib/validation.ts'
import { osdCompactLayout } from '../src/lib/osdCompactLayout.ts'

const dcs = { id: 'dcs', name: 'DCS', enabled: true, match: { exe: 'DCS.exe' } }
const msfs = { id: 'msfs', name: 'MSFS', enabled: true, match: { exe: 'FlightSimulator.exe' } }

test('configs written before Frame Generation load with it off and default settings', () => {
  const config = defaultConfig()
  delete config.modules.framegen
  const normalized = normalizeConfig(config)
  assert.deepEqual(normalized.modules.framegen, defaultFrameGenModule())
  assert.deepEqual(normalized.modules.framegen.defaults, { synthesisMode: 'interpolate', flowQuality: 'balanced', motionEstimation: 'shader', frameMultiplier: 'dynamic', artifactSuppression: 50, motionLimitPercent: 10 })
  assert.deepEqual(normalized.modules.framegen.toggleBindings, [])
  assert.equal(normalized.modules.framegen.diagnosticView, 'off')
  assert.deepEqual(validateConfig(normalized), [])
  // Defaults are fresh objects, never shared between calls.
  defaultFrameGenModule().defaults.artifactSuppression = 1
  assert.equal(defaultFrameGenSettings().artifactSuppression, 50)
})

test('invalid imported values normalize safely: enums fall back, integers clamp', () => {
  assert.deepEqual(normalizeFrameGenSettings({ synthesisMode: 'warp', flowQuality: 'ultra', motionEstimation: 'cuda', frameMultiplier: '8x', artifactSuppression: 'high', motionLimitPercent: Number.NaN }), defaultFrameGenSettings())
  assert.deepEqual(normalizeFrameGenSettings({ synthesisMode: 'extrapolate', flowQuality: 'quality', motionEstimation: 'hardware', frameMultiplier: '3x', artifactSuppression: 140, motionLimitPercent: 1 }),
    { synthesisMode: 'extrapolate', flowQuality: 'quality', motionEstimation: 'hardware', frameMultiplier: '3x', artifactSuppression: 100, motionLimitPercent: 2 })
  assert.deepEqual(normalizeFrameGenSettings({ artifactSuppression: -5, motionLimitPercent: 99 }), { ...defaultFrameGenSettings(), artifactSuppression: 0, motionLimitPercent: 25 })
  assert.equal(normalizeFrameGenSettings({ artifactSuppression: 33.6 }).artifactSuppression, 34)
  assert.equal(normalizeFrameGenSettings({ motionLimitPercent: Infinity }).motionLimitPercent, 10)
  assert.deepEqual(normalizeFrameGenSettings(null), defaultFrameGenSettings())

  const config = defaultConfig()
  config.applications = [dcs]
  config.modules.framegen = { enabled: 'yes', toggleBindings: [{ type: 'gamepad' }], diagnosticView: 'heatmap',
    defaults: { synthesisMode: 'extrapolate', flowQuality: 7 }, profiles: [42, { applicationIds: ['dcs', 3] }] }
  const framegen = normalizeConfig(config).modules.framegen
  assert.equal(framegen.enabled, false)
  assert.deepEqual(framegen.toggleBindings, [])
  assert.equal(framegen.diagnosticView, 'off')
  assert.deepEqual(framegen.defaults, { ...defaultFrameGenSettings(), synthesisMode: 'extrapolate' })
  // Profiles without settings inherit the module defaults, like Quadviews profiles.
  assert.deepEqual(framegen.profiles, [
    { name: 'New Profile', enabled: true, applicationIds: [], settings: framegen.defaults },
    { name: 'New Profile', enabled: true, applicationIds: ['dcs'], settings: framegen.defaults },
  ])
  assert.notEqual(framegen.profiles[0].settings, framegen.defaults)
})

test('the frame multiplier defaults to Dynamic, keeps valid choices, and falls back on bad values', () => {
  assert.equal(defaultFrameGenSettings().frameMultiplier, 'dynamic')
  assert.deepEqual([...frameGenFixedMultipliers], ['auto', '2x', '3x', '4x'])
  for (const value of ['auto', 'dynamic', '2x', '3x', '4x']) {
    assert.equal(normalizeFrameGenSettings({ frameMultiplier: value }).frameMultiplier, value)
  }
  for (const value of ['8x', '1x', '2X', 'Auto', 'fixed', 3, null, true, {}]) {
    assert.equal(normalizeFrameGenSettings({ frameMultiplier: value }).frameMultiplier, 'dynamic', String(value))
  }

  // Settings saved without a multiplier load as Dynamic; profiles fall back to the module defaults.
  const config = defaultConfig()
  config.applications = [dcs]
  config.modules.framegen.defaults = { synthesisMode: 'extrapolate', flowQuality: 'balanced', artifactSuppression: 50, motionLimitPercent: 10 }
  config.modules.framegen.profiles = [{ name: 'DCS', enabled: true, applicationIds: ['dcs'], settings: { synthesisMode: 'extrapolate', frameMultiplier: 'turbo' } }]
  let framegen = normalizeConfig(config).modules.framegen
  assert.equal(framegen.defaults.frameMultiplier, 'dynamic')
  assert.equal(framegen.profiles[0].settings.frameMultiplier, 'dynamic')
  // A saved fixed rate, Auto included, stays as chosen.
  config.modules.framegen.defaults.frameMultiplier = 'auto'
  assert.equal(normalizeConfig(config).modules.framegen.defaults.frameMultiplier, 'auto')
  config.modules.framegen.defaults.frameMultiplier = '3x'
  framegen = normalizeConfig(config).modules.framegen
  assert.equal(framegen.profiles[0].settings.frameMultiplier, '3x')
  assert.deepEqual(validateConfig(normalizeConfig(config)), [])

  config.modules.framegen.defaults.frameMultiplier = 4
  assert.ok(validateConfig(config).includes('modules.framegen.defaults.frameMultiplier must be auto, dynamic, 2x, 3x, or 4x'))
})

test('validateConfig rejects out-of-range and malformed edits', () => {
  const config = defaultConfig()
  config.applications = [dcs]
  config.modules.framegen.defaults.artifactSuppression = 101
  config.modules.framegen.defaults.motionLimitPercent = 2.5
  config.modules.framegen.diagnosticView = 'heatmap'
  config.modules.framegen.toggleBindings = [{ type: 'keyboard', chord: [] }]
  const profile = createFrameGenProfile(defaultFrameGenSettings(), ['dcs', 'ghost'])
  profile.settings.synthesisMode = 'warp'
  profile.settings.flowQuality = 'ultra'
  profile.settings.motionEstimation = 'optical'
  profile.settings.frameMultiplier = '8x'
  profile.settings.motionLimitPercent = ''
  profile.name = ' '
  config.modules.framegen.profiles = [profile]
  const errors = validateConfig(config)
  for (const expected of [
    'modules.framegen.defaults.artifactSuppression must be a whole number from 0 to 100',
    'modules.framegen.defaults.motionLimitPercent must be a whole number from 2 to 25',
    'modules.framegen.diagnosticView must be off, flow, confidence, or tint',
    'modules.framegen.toggleBindings[0].chord must include at least one key',
    'modules.framegen.profiles[0].name is required',
    'modules.framegen.profiles[0].applicationIds references unknown application: ghost',
    'modules.framegen.profiles[0].settings.synthesisMode must be interpolate or extrapolate',
    'modules.framegen.profiles[0].settings.flowQuality must be performance, balanced, or quality',
    'modules.framegen.profiles[0].settings.motionEstimation must be shader or hardware',
    'modules.framegen.profiles[0].settings.frameMultiplier must be auto, dynamic, 2x, 3x, or 4x',
    'modules.framegen.profiles[0].settings.motionLimitPercent must be a whole number from 2 to 25',
  ]) assert.ok(errors.includes(expected), expected)

  const conflicting = defaultConfig()
  conflicting.applications = [dcs]
  conflicting.modules.framegen.profiles = [createFrameGenProfile(defaultFrameGenSettings(), ['dcs']), createFrameGenProfile(defaultFrameGenSettings(), ['dcs'])]
  assert.deepEqual(validateConfig(conflicting), ['modules.framegen.profiles[1] conflicts with profiles[0] for application dcs; first enabled profile wins'])
  conflicting.modules.framegen.profiles[1].enabled = false
  assert.deepEqual(validateConfig(conflicting), [])
})

test('saved settings round-trip unchanged and emit only the keys the layer accepts', () => {
  const config = defaultConfig()
  config.applications = [dcs, msfs]
  config.modules.framegen.enabled = true
  config.modules.framegen.diagnosticView = 'confidence'
  config.modules.framegen.toggleBindings = [
    { type: 'device', deviceGuid: '{stick}', inputPath: 'button-7', productGuid: '', deviceName: 'Stick', inputLabel: 'Button 7' },
    { type: 'device', deviceGuid: '{left-stick}', inputPath: 'button-3', productGuid: '', deviceName: 'Left stick', inputLabel: 'Button 3' },
  ]
  config.modules.framegen.defaults = { synthesisMode: 'extrapolate', flowQuality: 'performance', motionEstimation: 'hardware', frameMultiplier: '4x', artifactSuppression: 0, motionLimitPercent: 25 }
  const profile = createFrameGenProfile(config.modules.framegen.defaults, ['dcs'])
  profile.name = 'DCS'
  profile.settings.flowQuality = 'quality'
  profile.settings.frameMultiplier = '2x'
  config.modules.framegen.profiles = [profile, { ...createFrameGenProfile(defaultFrameGenSettings(), ['msfs']), enabled: false }]

  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(normalized.modules.framegen, config.modules.framegen)
  assert.deepEqual(normalizeConfig(normalized), normalized)
  assert.deepEqual(validateConfig(normalized), [])
  assert.deepEqual(Object.keys(normalized.modules.framegen), ['enabled', 'toggleBindings', 'diagnosticView', 'defaults', 'profiles'])
  assert.deepEqual(Object.keys(normalized.modules.framegen.profiles[0]), ['name', 'enabled', 'applicationIds', 'settings'])
  assert.deepEqual(Object.keys(normalized.modules.framegen.defaults), ['synthesisMode', 'flowQuality', 'motionEstimation', 'frameMultiplier', 'artifactSuppression', 'motionLimitPercent'])
  assert.equal(normalized.modules.framegen.defaults.frameMultiplier, '4x')
  assert.equal(normalized.modules.framegen.profiles[0].settings.frameMultiplier, '2x')

  // The layer accepts a legacy profile `mode`, but the app never writes it.
  const legacy = JSON.parse(JSON.stringify(config))
  legacy.modules.framegen.profiles[0].mode = 'custom'
  assert.equal('mode' in normalizeConfig(legacy).modules.framegen.profiles[0], false)
})

test('profiles own independent settings objects', () => {
  const defaults = defaultFrameGenSettings()
  const first = createFrameGenProfile(defaults, ['dcs'])
  const second = createFrameGenProfile(defaults, ['msfs'])
  first.settings.motionLimitPercent = 20
  second.settings.synthesisMode = 'extrapolate'
  assert.equal(defaults.motionLimitPercent, 10)
  assert.equal(defaults.synthesisMode, 'interpolate')
  assert.equal(second.settings.motionLimitPercent, 10)
  assert.equal(first.settings.synthesisMode, 'interpolate')

  const config = defaultConfig()
  config.applications = [dcs, msfs]
  config.modules.framegen.profiles = [{ name: 'A', enabled: true, applicationIds: ['dcs'] }, { name: 'B', enabled: true, applicationIds: ['msfs'] }]
  const normalized = normalizeConfig(config).modules.framegen
  normalized.profiles[0].settings.artifactSuppression = 90
  assert.equal(normalized.profiles[1].settings.artifactSuppression, 50)
  assert.equal(normalized.defaults.artifactSuppression, 50)
})

test('a matching custom profile enables Frame Generation even with the default off', () => {
  const config = defaultConfig()
  config.applications = [dcs, msfs]
  config.modules.framegen.profiles = [{ ...createFrameGenProfile(defaultFrameGenSettings(), ['dcs']), name: 'DCS' }]
  assert.deepEqual(moduleStateForApplication(config, 'framegen', 'dcs'), { kind: 'custom', profileName: 'DCS', profileIndex: 0 })
  assert.deepEqual(moduleStateForApplication(config, 'framegen', 'msfs'), { kind: 'default-off' })
  config.modules.framegen.enabled = true
  assert.deepEqual(moduleStateForApplication(config, 'framegen', 'msfs'), { kind: 'default' })
})

test('the A/B toggle participates in global binding conflict detection', () => {
  const config = defaultConfig()
  config.modules.turbo.toggleBindings = [{ type: 'keyboard', chord: ['Ctrl', 'F7'] }]
  config.modules.framegen.toggleBindings = [
    { type: 'device', deviceGuid: '{throttle}', inputPath: 'button-23' },
    { type: 'keyboard', chord: ['F7', 'Ctrl'] },
  ]
  const warnings = savedBindingConflictWarnings(config, config.modules.framegen.toggleBindings)
  assert.equal(warnings.length, 1)
  assert.match(warnings[0].message, /Frame Generation: A\/B toggle \(binding 2\)/)
  assert.match(warnings[0].message, /Turbo: A\/B toggle/)
})

test('a config holding one A/B toggle binding, as older versions wrote it, loads as a one-binding list', () => {
  const config = defaultConfig()
  delete config.modules.framegen.toggleBindings
  config.modules.framegen.toggleBinding = { type: 'keyboard', chord: ['Ctrl', 'F8'] }
  const framegen = normalizeConfig(config).modules.framegen
  assert.deepEqual(framegen.toggleBindings, [{ type: 'keyboard', chord: ['Ctrl', 'F8'] }])
  assert.ok(!('toggleBinding' in framegen))
})

test('Turbo overlap produces a non-blocking warning naming the affected applications', () => {
  const config = defaultConfig()
  config.applications = [dcs, msfs]
  assert.deepEqual(configWarnings(config), [])
  config.modules.turbo.profiles = [{ id: 'turbo-dcs', name: 'DCS', enabled: true, applicationIds: ['dcs', 'msfs'], disableSafety: false,
    experimental: { enabled: false, applicationIds: [], waitForSubmit: false, sampleAtEntry: false, predictionPercent: 100, frameLimit: 0 } }]
  config.modules.framegen.profiles = [createFrameGenProfile(defaultFrameGenSettings(), ['dcs'])]
  const [warning] = configWarnings(config)
  assert.equal(warning, 'Turbo and Frame Generation are both on for DCS. Frame Generation takes over frame pacing; Turbo stays off in sessions where Frame Generation runs.')
  assert.deepEqual(validateConfig(config), [], 'the overlap never blocks saving')

  config.modules.framegen.profiles[0].enabled = false
  assert.deepEqual(configWarnings(config), [])

  config.modules.turbo.enabled = true
  config.modules.framegen.enabled = true
  assert.match(configWarnings(config)[0], /both on for DCS, MSFS and applications without a custom profile\./)
})

test('the OSD Frame Gen rows default sensibly and old saved orders gain the row at the end', () => {
  const osd = defaultOsdSettings()
  assert.deepEqual(osd.bodyOrder, ['graph', 'performance', 'turbo', 'framegen', 'pivot', 'modules'])
  assert.equal(osd.showFramegen, true)
  assert.equal(osd.compactShowFramegen, false)
  assert.equal(osd.showFramegenFps, true)
  assert.equal(normalizeOsdSettings({ showFramegenFps: false }).showFramegenFps, false)
  assert.equal(normalizeOsdSettings({ showFramegenFps: 'no' }).showFramegenFps, true)

  const legacy = normalizeOsdSettings({ bodyOrder: ['pivot', 'modules', 'graph', 'turbo'], showTurbo: false })
  assert.deepEqual(legacy.bodyOrder, ['pivot', 'modules', 'graph', 'turbo', 'performance', 'framegen'])
  assert.equal(legacy.showFramegen, true)
  assert.equal(normalizeOsdSettings({ showFramegen: 'no', compactShowFramegen: 1 }).showFramegen, true)
  assert.equal(normalizeOsdSettings({ showFramegen: false, compactShowFramegen: true }).compactShowFramegen, true)
  const preset = normalizeOsdSettings({ customPresets: [{ name: 'Old', settings: { bodyOrder: ['modules', 'graph'] } }] }).customPresets[0]
  assert.deepEqual(preset.settings.bodyOrder, ['modules', 'graph', 'performance', 'turbo', 'framegen', 'pivot'])
  assert.equal(preset.settings.showFramegen, true)

  const config = defaultConfig()
  config.core.osd.bodyOrder = ['graph', 'performance', 'turbo', 'pivot', 'modules']
  assert.ok(validateConfig(config).includes('OSD body order must contain each row once'))
  assert.deepEqual(validateConfig(normalizeConfig(config)), [])
})

test('the compact Frame Gen row sits between Turbo and Pivot and keeps values aligned', () => {
  const settings = { ...defaultOsdSettings(), compactShowTurbo: true, compactShowPivot: true }
  const withoutFrameGen = osdCompactLayout(settings)
  assert.equal(withoutFrameGen.framegenY, -1)
  assert.equal(withoutFrameGen.statusValueX, 128)

  const layout = osdCompactLayout({ ...settings, compactShowFramegen: true })
  assert.equal(layout.framegenY, layout.turboY + 48)
  assert.equal(layout.pivotY, layout.framegenY + 48)
  assert.equal(layout.height, withoutFrameGen.height + 48)
  assert.equal(layout.statusValueX, 150)

  const only = osdCompactLayout({ ...defaultOsdSettings(), compactShowBrand: false, compactShowApp: false, compactShowRuntime: false,
    compactShowClock: false, compactMetrics: 'none', compactShowFramegen: true })
  assert.equal(only.empty, false)
  assert.equal(only.statusY, only.framegenY)
  assert.equal(only.width, 520)
})

test('Frame Generation beside FPS widens only the compact FPS column, only when it can show', () => {
  const settings = { ...defaultOsdSettings(), compactShowBrand: false, compactShowApp: false, compactShowRuntime: false, compactShowClock: false }
  const plain = osdCompactLayout(settings)
  const session = osdCompactLayout(settings, { framegenSession: true })
  assert.equal(plain.framegenBadge, false)
  assert.equal(plain.frameTimeX, 262)
  assert.equal(session.framegenBadge, true)
  assert.equal(session.frameTimeX, plain.frameTimeX + 50)
  assert.equal(session.width, plain.width + 50)
  assert.equal(session.height, plain.height)
  // Off, or without an FPS value, the layout is the plain one.
  assert.deepEqual(osdCompactLayout({ ...settings, showFramegenFps: false }, { framegenSession: true }), plain)
  const frameTimeOnly = { ...settings, compactMetrics: 'frameTime' }
  assert.deepEqual(osdCompactLayout(frameTimeOnly, { framegenSession: true }), osdCompactLayout(frameTimeOnly))
  // The FPS-only display grows by the badge's room alone.
  assert.equal(osdCompactLayout({ ...settings, compactMetrics: 'fps' }, { framegenSession: true }).width, 266 + 50)
  // The Snap View line is a separate option: it never widens the FPS column.
  assert.equal(osdCompactLayout(settings, { pivotSnapLine: true }).framegenBadge, false)
  // The old positional boolean meant different things on each branch; refuse it.
  assert.throws(() => osdCompactLayout(settings, true), TypeError)
})
