import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import {
  createMirrorProfile,
  defaultConfig,
  defaultMirrorModule,
  defaultMirrorSettings,
  mirrorAspects,
  mirrorEyes,
  mirrorLimits,
  mirrorNumberKeys,
  mirrorFillsMonitor,
  mirrorMonitorMaxLength,
  mirrorWindowSize,
  monitorDisplayName,
  normalizeConfig,
  normalizeMirrorMonitor,
  normalizeMirrorSettings,
  savedBindingConflictWarnings,
} from '../src/lib/model.ts'
import { configWarnings, mirrorWarnings, turboFrameGenWarnings, validateConfig } from '../src/lib/validation.ts'

const schema = JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json', import.meta.url), 'utf8'))
const dcs = { id: 'dcs', name: 'DCS', enabled: true, match: { exe: 'DCS.exe' } }
const keyboard = (...chord) => ({ type: 'keyboard', chord })
const defaults = { smoothing: 60, levelHorizon: false, eye: 'left', margin: 15, aspect: '16:9', width: 1280, frameRate: 60, fillMonitor: false, monitor: '' }
const display2 = '\\\\.\\DISPLAY2'

test('configs written before the mirror window load with it off and the defaults', () => {
  const config = defaultConfig()
  delete config.modules.mirror
  const normalized = normalizeConfig(config)
  assert.deepEqual(normalized.modules.mirror, defaultMirrorModule())
  // Exactly what the app writes for a default mirror block.
  assert.deepEqual(JSON.parse(JSON.stringify(normalized.modules.mirror)), { enabled: false, bindings: [], defaults, profiles: [] })
  assert.deepEqual(validateConfig(normalized), [])
  assert.deepEqual(configWarnings(normalized), [])
  // Defaults are fresh objects, never shared between calls.
  defaultMirrorModule().defaults.smoothing = 9
  defaultMirrorModule().bindings.push(keyboard('F7'))
  defaultMirrorSettings().aspect = '1:1'
  assert.deepEqual(defaultMirrorSettings(), defaults)
  assert.deepEqual(defaultConfig().modules.mirror.bindings, [])
})

test('the mirror block round-trips through normalization unchanged', () => {
  const config = defaultConfig()
  config.applications.push(dcs)
  config.modules.mirror = {
    enabled: true,
    bindings: [
      { ...keyboard('Ctrl', 'Alt', 'M'), sound: { enabled: true, activateSound: '', deactivateSound: '' } },
      { type: 'device', deviceGuid: 'guid-1', inputPath: 'button-7', productGuid: '', deviceName: 'Stick', inputLabel: 'Button 7' },
    ],
    defaults: { smoothing: 0, levelHorizon: true, eye: 'right', margin: 40, aspect: '21:9', width: 3840, frameRate: 120, fillMonitor: true, monitor: display2 },
    profiles: [{ name: 'DCS', enabled: true, applicationIds: ['dcs'], settings: { smoothing: 100, levelHorizon: false, eye: 'left', margin: 0, aspect: '1:1', width: 320, frameRate: 15, fillMonitor: false, monitor: '' } }],
  }
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(normalized.modules.mirror, config.modules.mirror)
  assert.deepEqual(validateConfig(normalized), [])
  // The app never writes the legacy profile mode.
  assert.ok(!('mode' in normalized.modules.mirror.profiles[0]))
})

test('mirror numbers clamp to whole numbers in range; junk falls back', () => {
  assert.deepEqual(
    normalizeMirrorSettings({ smoothing: 140, levelHorizon: 'yes', eye: 'center', margin: -3, aspect: '5:4', width: 100, frameRate: 240 }),
    { ...defaults, smoothing: 100, margin: 0, width: 320, frameRate: 120 },
  )
  assert.deepEqual(
    normalizeMirrorSettings({ smoothing: 12.6, margin: 40.4, width: 5000, frameRate: 14.2 }),
    { ...defaults, smoothing: 13, margin: 40, width: 3840, frameRate: 15 },
  )
  assert.deepEqual(normalizeMirrorSettings({ smoothing: Number.NaN, margin: '20', width: null, frameRate: Infinity }), defaults)
  assert.deepEqual(normalizeMirrorSettings(null), defaults)
  assert.deepEqual(normalizeMirrorSettings({}), defaults)
  for (const eye of mirrorEyes) assert.equal(normalizeMirrorSettings({ eye }).eye, eye)
  for (const aspect of mirrorAspects) assert.equal(normalizeMirrorSettings({ aspect }).aspect, aspect)
  // Missing values in a profile take the module's defaults.
  const moduleDefaults = { ...defaults, smoothing: 30, eye: 'right', aspect: '4:3', levelHorizon: true }
  assert.deepEqual(normalizeMirrorSettings({ smoothing: 80 }, moduleDefaults), { ...moduleDefaults, smoothing: 80 })
  // What a saved file holds is always something the layer accepts.
  const config = defaultConfig()
  config.modules.mirror.defaults = { ...defaults, smoothing: 250, margin: 41, width: 319, frameRate: 121, eye: 'both', aspect: '16x9' }
  config.modules.mirror.bindings = keyboard('f7')
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(normalized.modules.mirror.defaults, { ...defaults, smoothing: 100, margin: 40, width: 320, frameRate: 120 })
  // A single binding is read as a list of one; None entries are dropped.
  assert.deepEqual(normalized.modules.mirror.bindings, [keyboard('F7')])
  config.modules.mirror.bindings = [{ type: 'none' }, keyboard('Shift', 'F7')]
  assert.deepEqual(normalizeConfig(JSON.parse(JSON.stringify(config))).modules.mirror.bindings, [keyboard('Shift', 'F7')])
  assert.deepEqual(validateConfig(normalized), [])
})

test('the window opens at the width, with the height the aspect gives', () => {
  assert.deepEqual(mirrorWindowSize(defaults), { width: 1280, height: 720 })
  assert.deepEqual(mirrorWindowSize({ aspect: '16:10', width: 1280 }), { width: 1280, height: 800 })
  assert.deepEqual(mirrorWindowSize({ aspect: '4:3', width: 1024 }), { width: 1024, height: 768 })
  assert.deepEqual(mirrorWindowSize({ aspect: '1:1', width: 320 }), { width: 320, height: 320 })
  assert.deepEqual(mirrorWindowSize({ aspect: '21:9', width: 3840 }), { width: 3840, height: 1646 })
  assert.deepEqual([...mirrorNumberKeys], ['smoothing', 'margin', 'width', 'frameRate'])
  for (const key of mirrorNumberKeys) {
    assert.ok(defaults[key] >= mirrorLimits[key].min && defaults[key] <= mirrorLimits[key].max, key)
  }
})

test('out-of-range and unknown mirror values block saving with the field named', () => {
  const config = defaultConfig()
  config.applications.push(dcs)
  config.modules.mirror.defaults = { smoothing: 101, levelHorizon: 'no', eye: 'both', margin: 41, aspect: '5:4', width: 1280.5, frameRate: 14, fillMonitor: 1, monitor: 'M'.repeat(65) }
  config.modules.mirror.profiles.push({ ...createMirrorProfile(defaultMirrorSettings(), ['dcs']), name: 'DCS' })
  config.modules.mirror.profiles[0].settings.width = 3841
  config.modules.mirror.profiles[0].settings.smoothing = Number.NaN
  config.modules.mirror.profiles[0].settings.frameRate = ''
  assert.deepEqual(validateConfig(config), [
    'modules.mirror.defaults.smoothing must be between 0 and 100',
    'modules.mirror.defaults.margin must be between 0 and 40',
    'modules.mirror.defaults.width must be an integer between 320 and 3840',
    'modules.mirror.defaults.frameRate must be an integer between 15 and 120',
    'modules.mirror.defaults.levelHorizon must be true or false',
    'modules.mirror.defaults.eye must be left or right',
    'modules.mirror.defaults.aspect must be 16:9, 16:10, 4:3, 1:1, or 21:9',
    'modules.mirror.defaults.fillMonitor must be true or false',
    'modules.mirror.defaults.monitor must be text of at most 64 characters',
    'modules.mirror.profiles[0].settings.smoothing must be between 0 and 100',
    'modules.mirror.profiles[0].settings.width must be an integer between 320 and 3840',
    'modules.mirror.profiles[0].settings.frameRate must be an integer between 15 and 120',
  ])
  // The limits themselves are fine.
  config.modules.mirror.defaults = { smoothing: 100, levelHorizon: true, eye: 'right', margin: 40, aspect: '21:9', width: 3840, frameRate: 120, fillMonitor: true, monitor: 'M'.repeat(64) }
  config.modules.mirror.profiles[0].settings = { smoothing: 0, levelHorizon: false, eye: 'left', margin: 0, aspect: '1:1', width: 320, frameRate: 15, fillMonitor: false, monitor: '' }
  assert.deepEqual(validateConfig(config), [])
})

test('mirror profiles report unknown applications and conflicts; bindings are checked', () => {
  const config = defaultConfig()
  config.applications.push(dcs)
  config.modules.mirror.profiles.push(
    { ...createMirrorProfile(defaultMirrorSettings(), ['dcs']), name: 'A' },
    { ...createMirrorProfile(defaultMirrorSettings(), ['dcs', 'ghost']), name: 'B' },
    { ...createMirrorProfile(defaultMirrorSettings(), []), name: ' ' },
  )
  const errors = validateConfig(config)
  assert.ok(errors.some((error) => error.includes('modules.mirror.profiles[1]') && error.includes('ghost')))
  assert.ok(errors.some((error) => error.includes('modules.mirror.profiles[1] conflicts with profiles[0]')))
  assert.ok(errors.some((error) => error.includes('modules.mirror.profiles[2].name')))
  assert.equal(errors.length, 3)
  // A new profile copies the settings; it never shares them with the defaults.
  const profile = createMirrorProfile(config.modules.mirror.defaults, ['dcs'])
  profile.settings.smoothing = 5
  assert.equal(config.modules.mirror.defaults.smoothing, 60)
  assert.deepEqual({ ...profile, settings: undefined }, { name: 'New Profile', enabled: true, applicationIds: ['dcs'], settings: undefined })

  const bindings = defaultConfig()
  bindings.modules.mirror.bindings = [keyboard('F7'), keyboard('Ctrl'), { type: 'device', deviceGuid: '', inputPath: 'button-200' }]
  assert.deepEqual(validateConfig(bindings), [
    'modules.mirror.bindings[1].chord must include exactly one non-modifier key',
    'modules.mirror.bindings[2].deviceGuid is required',
    'modules.mirror.bindings[2].inputPath must use button-1 through button-128 or hat-1 through hat-4 with a supported direction',
  ])
})

test('the chosen monitor is kept exactly as written; junk and overlong names fall back', () => {
  assert.equal(display2.length, 12)
  assert.deepEqual(normalizeMirrorSettings({ fillMonitor: true, monitor: display2 }), { ...defaults, fillMonitor: true, monitor: display2 })
  // Free text as far as the layer goes: nothing is trimmed, re-cased or dropped
  // because the monitor is not connected.
  assert.equal(normalizeMirrorMonitor('  HDMI TV '), '  HDMI TV ')
  assert.equal(normalizeMirrorMonitor('M'.repeat(mirrorMonitorMaxLength)), 'M'.repeat(64))
  assert.equal(normalizeMirrorMonitor('M'.repeat(65)), '')
  assert.equal(normalizeMirrorMonitor('M'.repeat(65), display2), display2)
  for (const junk of [null, undefined, 2, {}, ['x']]) assert.equal(normalizeMirrorMonitor(junk), '')
  assert.deepEqual(normalizeMirrorSettings({ fillMonitor: 'yes', monitor: 7 }), defaults)
  // A profile without the keys takes the module's defaults.
  const moduleDefaults = { ...defaults, fillMonitor: true, monitor: display2 }
  assert.deepEqual(normalizeMirrorSettings({ smoothing: 10 }, moduleDefaults), { ...moduleDefaults, smoothing: 10 })
  assert.deepEqual(normalizeMirrorSettings({ fillMonitor: false, monitor: '' }, moduleDefaults), { ...defaults })
  // Configs from before Fill a monitor load with it off and nothing chosen.
  const config = defaultConfig()
  delete config.modules.mirror.defaults.fillMonitor
  delete config.modules.mirror.defaults.monitor
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.equal(normalized.modules.mirror.defaults.fillMonitor, false)
  assert.equal(normalized.modules.mirror.defaults.monitor, '')
  assert.deepEqual(validateConfig(normalized), [])

  // Shown without the device prefix; the full name is what is saved.
  assert.equal(monitorDisplayName(display2), 'DISPLAY2')
  assert.equal(monitorDisplayName('HDMI TV'), 'HDMI TV')
  assert.equal(monitorDisplayName(''), '')
  // The picture fills a monitor only with the switch on and one chosen.
  assert.ok(mirrorFillsMonitor({ fillMonitor: true, monitor: display2 }))
  assert.ok(!mirrorFillsMonitor({ fillMonitor: true, monitor: '' }))
  assert.ok(!mirrorFillsMonitor({ fillMonitor: true, monitor: '  ' }))
  assert.ok(!mirrorFillsMonitor({ fillMonitor: false, monitor: display2 }))
})

test('Fill a monitor with no monitor chosen warns without blocking', () => {
  const config = defaultConfig()
  config.applications.push(dcs)
  assert.deepEqual(mirrorWarnings(config), [])

  // Off Default Profile: nothing opens, so nothing to warn about.
  config.modules.mirror.defaults.fillMonitor = true
  assert.deepEqual(mirrorWarnings(config), [])
  config.modules.mirror.enabled = true
  const [fromDefaults] = mirrorWarnings(config)
  assert.match(fromDefaults, /Fill a monitor is on in the Default Profile with no monitor chosen\./)
  assert.deepEqual(validateConfig(config), [])
  // Shown on the mirror page, not with the other pages' advice.
  assert.deepEqual(turboFrameGenWarnings(config), [])
  assert.deepEqual(configWarnings(config), [fromDefaults])

  config.modules.mirror.defaults.monitor = display2
  assert.deepEqual(mirrorWarnings(config), [])

  // Enabled profiles count, by name; disabled ones do not.
  config.modules.mirror.profiles.push(
    { ...createMirrorProfile({ ...defaults, fillMonitor: true }, ['dcs']), name: 'DCS' },
    { ...createMirrorProfile({ ...defaults, fillMonitor: true }, []), name: 'Parked', enabled: false },
    { ...createMirrorProfile({ ...defaults, fillMonitor: true, monitor: display2 }, []), name: 'TV' },
  )
  const warnings = mirrorWarnings(config)
  assert.equal(warnings.length, 1)
  assert.match(warnings[0], /Fill a monitor is on in the profile DCS with no monitor chosen\./)
  assert.deepEqual(validateConfig(config), [])
})

test('binding conflict warnings include the mirror window bindings', () => {
  const config = defaultConfig()
  config.modules.mirror.bindings = [keyboard('F7')]
  assert.deepEqual(savedBindingConflictWarnings(config, config.modules.mirror.bindings), [])

  // The OSD's default show/hide keys.
  config.modules.mirror.bindings = [keyboard('F7'), keyboard('Ctrl', 'Alt', 'F10')]
  const warnings = savedBindingConflictWarnings(config, config.modules.mirror.bindings)
  assert.equal(warnings.length, 1)
  assert.match(warnings[0].title, /Ctrl\+Alt\+F10/)
  assert.match(warnings[0].message, /OSD: show\/hide/)
  assert.match(warnings[0].message, /Capture & Mirror: show or hide mirror window \(binding 2\)/)

  // Seen from the other action's page too, and against the screenshot binding.
  const fromOsd = savedBindingConflictWarnings(config, config.core.osd.toggleBindings)
  assert.equal(fromOsd.length, 1)
  assert.match(fromOsd[0].message, /Capture & Mirror: show or hide mirror window/)
  config.modules.mirror.bindings = [keyboard('F9')]
  config.core.screenshot.bindings = [keyboard('F9')]
  const shared = savedBindingConflictWarnings(config, config.core.screenshot.bindings)
  assert.equal(shared.length, 1)
  assert.match(shared[0].message, /Capture & Mirror: take screenshot/)
  assert.match(shared[0].message, /Capture & Mirror: show or hide mirror window/)
})

test('schema describes the generated mirror module, keeps it optional and bounds its values', () => {
  const mirror = defaultConfig().modules.mirror
  assert.equal(schema.$defs.modules.properties.mirror.$ref, '#/$defs/mirrorModule')
  assert.ok(!schema.$defs.modules.required.includes('mirror'))
  const module = schema.$defs.mirrorModule
  assert.equal(module.additionalProperties, false)
  assert.equal(module.required, undefined)
  assert.deepEqual(Object.keys(module.properties), Object.keys(mirror))
  assert.equal(module.properties.enabled.default, mirror.enabled)
  assert.equal(module.properties.bindings.$ref, '#/$defs/inputBindings')
  assert.equal(module.properties.defaults.$ref, '#/$defs/mirrorSettings')
  assert.equal(module.properties.profiles.items.$ref, '#/$defs/mirrorProfile')

  const profile = createMirrorProfile(defaultMirrorSettings(), ['dcs'])
  for (const key of Object.keys(profile)) assert.ok(key in schema.$defs.mirrorProfile.properties, key)
  for (const key of schema.$defs.mirrorProfile.required) assert.ok(key in profile, key)
  assert.equal(schema.$defs.mirrorProfile.properties.settings.$ref, '#/$defs/mirrorSettings')

  const definition = schema.$defs.mirrorSettings
  const settings = definition.properties
  assert.equal(definition.additionalProperties, false)
  assert.deepEqual(Object.keys(settings), Object.keys(mirror.defaults))
  // The defaults are accepted: each is the schema's default and inside its bounds.
  for (const [key, value] of Object.entries(mirror.defaults)) assert.equal(settings[key].default, value, key)
  // Values the layer rejects are outside the schema too.
  for (const key of mirrorNumberKeys) {
    assert.deepEqual([settings[key].minimum, settings[key].maximum], [mirrorLimits[key].min, mirrorLimits[key].max], key)
  }
  assert.equal(settings.smoothing.type, 'number')
  assert.equal(settings.margin.type, 'number')
  assert.equal(settings.width.type, 'integer')
  assert.equal(settings.frameRate.type, 'integer')
  assert.equal(settings.levelHorizon.type, 'boolean')
  assert.deepEqual(settings.eye.enum, [...mirrorEyes])
  assert.deepEqual(settings.aspect.enum, [...mirrorAspects])
  assert.equal(settings.fillMonitor.type, 'boolean')
  assert.equal(settings.monitor.type, 'string')
  assert.equal(settings.monitor.maxLength, mirrorMonitorMaxLength)
  assert.equal(settings.monitor.pattern, undefined)
  assert.ok(!settings.eye.enum.includes('both'))
  assert.ok(!settings.aspect.enum.includes('5:4'))
})
