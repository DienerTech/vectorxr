import assert from 'node:assert/strict'
import test from 'node:test'

import {
  bindingsMatchRuntimeActivation,
  bindingsShareInput,
  defaultConfig,
  normalizeConfig,
  normalizeKeyboardKey,
  pivotBindingConflictWarnings,
  savedBindingConflictWarnings,
} from '../src/lib/model.ts'

const keyboard = (...chord) => ({ type: 'keyboard', chord })
const none = () => ({ type: 'none' })

test('keyboard normalization preserves numpad and modifier keys', () => {
  assert.equal(normalizeKeyboardKey('numpad5', ''), 'Numpad5')
  assert.equal(normalizeKeyboardKey('NUMPAD9', ''), 'Numpad9')
  assert.equal(normalizeKeyboardKey('control', ''), 'Ctrl')
  assert.equal(normalizeKeyboardKey('alt', ''), 'Alt')
  assert.equal(normalizeKeyboardKey('SHIFT', ''), 'Shift')
})

test('config normalization round-trips a modified numpad chord', () => {
  const config = defaultConfig()
  config.modules.pivotxr.activationBinding = keyboard('Ctrl', 'Numpad5')

  const normalized = normalizeConfig(config)

  assert.deepEqual(normalized.modules.pivotxr.activationBinding, keyboard('Ctrl', 'Numpad5'))
})

test('new Depth profiles enable Depth Lock by default', () => {
  assert.equal(defaultConfig().modules.depthxr.defaults.depthAnchor, true)
})

test('legacy Depth settings normalize with Depth Anchor disabled', () => {
  const config = defaultConfig()
  delete config.modules.depthxr.defaults.depthAnchor

  const normalized = normalizeConfig(config)

  assert.equal(normalized.modules.depthxr.defaults.depthAnchor, false)
})

test('legacy Depth bindings normalize with the Anchor toggle unbound', () => {
  const config = defaultConfig()
  delete config.modules.depthxr.bindings.toggleAnchor

  const normalized = normalizeConfig(config)

  assert.deepEqual(normalized.modules.depthxr.bindings.toggleAnchor, none())
})

test('saved binding warnings scan across modules and profiles without blocking', () => {
  const config = defaultConfig()
  config.modules.depthxr.bindings.toggleAnchor = keyboard('Shift', 'F8')
  config.modules.turbo.toggleBinding = keyboard('F8', 'Shift')
  config.modules.pivotxr.activationBinding = keyboard('Shift', 'F8')

  const warnings = savedBindingConflictWarnings(config, [
    config.modules.depthxr.bindings.toggleAnchor,
  ])

  assert.equal(warnings.length, 1)
  assert.match(warnings[0].message, /Depth: Depth Lock A\/B/)
  assert.match(warnings[0].message, /Turbo: A\/B toggle/)
  assert.match(warnings[0].message, /Pivot Default: Activate/)
  assert.match(warnings[0].message, /does not block saving/)
})

test('Pivot global warnings suppress conflicts already explained by the local warning', () => {
  const config = defaultConfig()
  config.modules.pivotxr.activationBinding = keyboard('Shift', 'F8')
  config.modules.pivotxr.setOriginBinding = keyboard('F8', 'Shift')
  const focus = [
    config.modules.pivotxr.activationBinding,
    config.modules.pivotxr.setOriginBinding,
    config.modules.pivotxr.releaseOriginBinding,
  ]

  assert.equal(savedBindingConflictWarnings(config, focus).length, 1)
  assert.equal(savedBindingConflictWarnings(config, focus, {
    suppressFocusOnlyConflicts: true,
  }).length, 0)

  config.modules.turbo.toggleBinding = keyboard('Shift', 'F8')
  const crossFeatureWarnings = savedBindingConflictWarnings(config, focus, {
    suppressFocusOnlyConflicts: true,
  })
  assert.equal(crossFeatureWarnings.length, 1)
  assert.match(crossFeatureWarnings[0].message, /Turbo: A\/B toggle/)
})

test('physical input sharing stays distinct from runtime activation arbitration', () => {
  const shiftThenCtrl = keyboard('Shift', 'Ctrl', 'F8')
  const ctrlThenShift = keyboard('Ctrl', 'Shift', 'F8')

  assert.equal(bindingsShareInput(shiftThenCtrl, ctrlThenShift), true)
  assert.equal(bindingsMatchRuntimeActivation(shiftThenCtrl, ctrlThenShift), false)
  assert.equal(bindingsMatchRuntimeActivation(shiftThenCtrl, keyboard('Shift', 'Ctrl', 'F8')), true)

  const leftDevice = { type: 'device', deviceGuid: '{ABC}', inputPath: 'button-1', deviceName: 'Stick A' }
  const rightDevice = { type: 'device', deviceGuid: '{ABC}', inputPath: 'button-1', deviceName: 'Stick B' }
  assert.equal(bindingsMatchRuntimeActivation(leftDevice, rightDevice), true)

  const hatLeft = { type: 'device', deviceGuid: '{ABC}', inputPath: 'hat-1-left', inputLabel: 'HAT 1 Left' }
  const sameHatLeft = { type: 'device', deviceGuid: '{abc}', inputPath: 'HAT-1-LEFT' }
  const hatRight = { type: 'device', deviceGuid: '{ABC}', inputPath: 'hat-1-right' }
  assert.equal(bindingsShareInput(hatLeft, sameHatLeft), true)
  assert.equal(bindingsShareInput(hatLeft, hatRight), false)
})

test('pivot binding warnings cover activation, set, and release conflicts', () => {
  const f8 = keyboard('F8')

  const activationSet = pivotBindingConflictWarnings('toggle', f8, keyboard('F8'), none())
  assert.equal(activationSet.length, 1)
  assert.equal(activationSet[0].title, 'Activation also sets the origin')
  assert.match(activationSet[0].message, /including deactivation/)

  const activationRelease = pivotBindingConflictWarnings('hold', f8, none(), keyboard('F8'))
  assert.equal(activationRelease[0].title, 'Activation also releases the origin')

  const setRelease = pivotBindingConflictWarnings('hold', none(), f8, keyboard('F8'))
  assert.equal(setRelease[0].title, 'Set Origin is immediately canceled')

  const allThree = pivotBindingConflictWarnings('toggle', f8, keyboard('F8'), keyboard('F8'))
  assert.equal(allThree.length, 1)
  assert.equal(allThree[0].title, 'One binding controls three conflicting actions')
})
