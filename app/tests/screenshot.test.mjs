import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'
import {
  defaultConfig,
  defaultScreenshotSettings,
  normalizeConfig,
  normalizeScreenshotSettings,
  savedBindingConflictWarnings,
  screenshotEyes,
} from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

const schema = JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json', import.meta.url), 'utf8'))
const keyboard = (...chord) => ({ type: 'keyboard', chord })

test('configs written before screenshots load with no binding, both eyes and the default folder', () => {
  const config = defaultConfig()
  delete config.core.screenshot
  const normalized = normalizeConfig(config)
  assert.deepEqual(normalized.core.screenshot, { bindings: [], eyes: 'both' })
  assert.deepEqual(normalized.core.screenshot, defaultScreenshotSettings())
  assert.ok(!('folder' in normalized.core.screenshot))
  assert.deepEqual(validateConfig(normalized), [])
  // Defaults are fresh objects, never shared between calls.
  defaultScreenshotSettings().bindings.push(keyboard('F9'))
  assert.deepEqual(defaultConfig().core.screenshot.bindings, [])
})

test('the screenshot block round-trips through normalization unchanged', () => {
  const config = defaultConfig()
  config.core.screenshot = {
    bindings: [
      { ...keyboard('Ctrl', 'Alt', 'F9'), sound: { enabled: true, activateSound: '', deactivateSound: '' } },
      { type: 'device', deviceGuid: 'guid-1', inputPath: 'button-7', productGuid: '', deviceName: 'Stick', inputLabel: 'Button 7' },
    ],
    eyes: 'left',
    folder: 'D:\\Shots',
  }
  const normalized = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(normalized.core.screenshot, config.core.screenshot)
  assert.deepEqual(validateConfig(normalized), [])
})

test('screenshot settings fall back on junk and trim the folder', () => {
  assert.deepEqual(normalizeScreenshotSettings(null), defaultScreenshotSettings())
  assert.deepEqual(normalizeScreenshotSettings({ bindings: 'F9', eyes: 'center', folder: 5 }), { bindings: [], eyes: 'both' })
  assert.deepEqual(normalizeScreenshotSettings({ eyes: 'right', folder: '  D:\\Shots  ' }), { bindings: [], eyes: 'right', folder: 'D:\\Shots' })
  for (const eyes of screenshotEyes) assert.equal(normalizeScreenshotSettings({ eyes }).eyes, eyes)
  // A single binding is read as a list of one; None entries are dropped.
  assert.deepEqual(normalizeScreenshotSettings({ bindings: keyboard('f9') }).bindings, [keyboard('F9')])
  assert.deepEqual(normalizeScreenshotSettings({ bindings: [{ type: 'none' }, keyboard('Shift', 'F9')] }).bindings, [keyboard('Shift', 'F9')])
})

test('the default folder is saved by leaving the folder out', () => {
  for (const folder of ['', '   ', undefined, null]) {
    const config = defaultConfig()
    config.core.screenshot = { bindings: [keyboard('F9')], eyes: 'both', folder }
    const saved = JSON.parse(JSON.stringify(normalizeConfig(config)))
    assert.deepEqual(saved.core.screenshot, { bindings: [keyboard('F9')], eyes: 'both' })
  }
  const config = defaultConfig()
  config.core.screenshot.folder = 'E:/VR/Shots'
  assert.equal(JSON.parse(JSON.stringify(normalizeConfig(config))).core.screenshot.folder, 'E:/VR/Shots')
})

test('validation checks the eye choice, the folder and the bindings', () => {
  const errorsFor = (screenshot) => {
    const config = defaultConfig()
    config.core.screenshot = { ...defaultScreenshotSettings(), ...screenshot }
    return validateConfig(config)
  }
  assert.deepEqual(errorsFor({}), [])
  assert.deepEqual(errorsFor({ eyes: 'center' }), ['core.screenshot.eyes must be both, left, or right'])

  for (const folder of ['D:\\Shots', 'D:/Shots', 'c:\\Users\\Pilot\\My Shots\\', '\\\\nas\\media\\Shots']) {
    assert.deepEqual(errorsFor({ folder }), [], folder)
  }
  for (const folder of ['Shots', '.\\Shots', 'D:Shots', '\\Shots', '\\\\nas', 'D:\\Shots\\a?b', 'D:\\Sh"ots', '']) {
    assert.deepEqual(errorsFor({ folder }), ['core.screenshot.folder must be a full folder path, such as D:\\Screenshots'], folder)
  }

  assert.deepEqual(errorsFor({ bindings: [keyboard('F9'), keyboard('Ctrl')] }), [
    'core.screenshot.bindings[1].chord must include exactly one non-modifier key',
  ])
  assert.deepEqual(errorsFor({ bindings: [{ ...keyboard('F9'), sound: { enabled: true, activateSound: 'shutter.mp3', deactivateSound: '' } }] }), [
    'core.screenshot.bindings[0].sound.activateSound must point to a .wav file',
  ])
  assert.deepEqual(errorsFor({ bindings: [{ type: 'device', deviceGuid: '', inputPath: 'button-200' }] }), [
    'core.screenshot.bindings[0].deviceGuid is required',
    'core.screenshot.bindings[0].inputPath must use button-1 through button-128 or hat-1 through hat-4 with a supported direction',
  ])
})

test('binding conflict warnings include the screenshot bindings', () => {
  const config = defaultConfig()
  config.core.screenshot.bindings = [keyboard('F9')]
  assert.deepEqual(savedBindingConflictWarnings(config, config.core.screenshot.bindings), [])

  // The OSD's default show/hide keys.
  config.core.screenshot.bindings = [keyboard('F9'), keyboard('Ctrl', 'Alt', 'F10')]
  const warnings = savedBindingConflictWarnings(config, config.core.screenshot.bindings)
  assert.equal(warnings.length, 1)
  assert.match(warnings[0].title, /Ctrl\+Alt\+F10/)
  assert.match(warnings[0].message, /OSD: show\/hide/)
  assert.match(warnings[0].message, /Capture & Mirror: take screenshot \(binding 2\)/)

  // Seen from the other action's page too.
  const fromOsd = savedBindingConflictWarnings(config, config.core.osd.toggleBindings)
  assert.equal(fromOsd.length, 1)
  assert.match(fromOsd[0].message, /Capture & Mirror: take screenshot/)

  // A device button shared with a module's toggle.
  const button = { type: 'device', deviceGuid: 'GUID-1', inputPath: 'button-3' }
  config.core.screenshot.bindings = [button]
  config.modules.turbo.toggleBindings = [{ ...button, deviceGuid: 'guid-1' }]
  const shared = savedBindingConflictWarnings(config, config.core.screenshot.bindings)
  assert.equal(shared.length, 1)
  assert.match(shared[0].message, /Turbo: A\/B toggle/)
  assert.match(shared[0].message, /Capture & Mirror: take screenshot/)
})

test('schema describes the screenshot block and keeps it optional', () => {
  const definition = schema.$defs.screenshotSettings
  assert.equal(schema.$defs.core.properties.screenshot.$ref, '#/$defs/screenshotSettings')
  assert.ok(!schema.$defs.core.required.includes('screenshot'))
  assert.equal(definition.additionalProperties, false)
  assert.equal(definition.required, undefined)
  for (const key of Object.keys({ ...defaultScreenshotSettings(), folder: 'D:\\Shots' })) {
    assert.ok(key in definition.properties, `schema does not allow ${key}`)
  }
  assert.equal(definition.properties.bindings.$ref, '#/$defs/inputBindings')
  assert.deepEqual(definition.properties.eyes.enum, [...screenshotEyes])
  assert.equal(definition.properties.eyes.default, defaultScreenshotSettings().eyes)
  const folder = new RegExp(definition.properties.folder.pattern)
  for (const path of ['D:\\Shots', 'D:/Shots', '\\\\nas\\media\\Shots']) assert.ok(folder.test(path), path)
  for (const path of ['Shots', 'D:Shots', '\\\\nas', '']) assert.ok(!folder.test(path), path)
})
