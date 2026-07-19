import assert from 'node:assert/strict'
import test from 'node:test'

import {
  bindingsMatchRuntimeActivation,
  bindingsShareInput,
  defaultConfig,
  normalizeConfig,
  normalizeKeyboardKey,
  pivotBindingConflictWarnings,
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

test('physical input sharing stays distinct from runtime activation arbitration', () => {
  const shiftThenCtrl = keyboard('Shift', 'Ctrl', 'F8')
  const ctrlThenShift = keyboard('Ctrl', 'Shift', 'F8')

  assert.equal(bindingsShareInput(shiftThenCtrl, ctrlThenShift), true)
  assert.equal(bindingsMatchRuntimeActivation(shiftThenCtrl, ctrlThenShift), false)
  assert.equal(bindingsMatchRuntimeActivation(shiftThenCtrl, keyboard('Shift', 'Ctrl', 'F8')), true)

  const leftDevice = { type: 'device', deviceGuid: '{ABC}', inputPath: 'button-1', deviceName: 'Stick A' }
  const rightDevice = { type: 'device', deviceGuid: '{ABC}', inputPath: 'button-1', deviceName: 'Stick B' }
  assert.equal(bindingsMatchRuntimeActivation(leftDevice, rightDevice), true)
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
