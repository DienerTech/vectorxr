import assert from 'node:assert/strict'
import test from 'node:test'

import {
  bindingsMatchRuntimeActivation,
  bindingsShareInput,
  defaultConfig,
  createPivotProfile,
  normalizeConfig,
  normalizeKeyboardKey,
  pivotBindingConflictWarnings,
  preserveBindingSound,
  savedBindingConflictWarnings,
} from '../src/lib/model.ts'

const keyboard = (...chord) => ({ type: 'keyboard', chord })
const none = () => ({ type: 'none' })
const activation = (behavior, binding) => ({ behavior, binding })

test('replacing a binding input preserves sound enablement and custom files', () => {
  const sound = {
    enabled: true,
    activateSound: 'C:\\sounds\\pivot-on.wav',
    deactivateSound: 'C:\\sounds\\pivot-off.wav',
  }
  const current = { type: 'keyboard', chord: ['F8'], sound }
  const replacement = {
    type: 'device',
    deviceGuid: '{device-guid}',
    inputPath: 'button-41',
    deviceName: 'Test HOTAS',
    inputLabel: 'Button 41',
  }

  const updated = preserveBindingSound(current, replacement)
  assert.deepEqual(updated, { ...replacement, sound })
  assert.notEqual(updated.sound, sound)
  assert.deepEqual(preserveBindingSound(current, none()), none())
})

test('keyboard normalization preserves numpad and modifier keys', () => {
  assert.equal(normalizeKeyboardKey('numpad5', ''), 'Numpad5')
  assert.equal(normalizeKeyboardKey('NUMPAD9', ''), 'Numpad9')
  assert.equal(normalizeKeyboardKey('control', ''), 'Ctrl')
  assert.equal(normalizeKeyboardKey('alt', ''), 'Alt')
  assert.equal(normalizeKeyboardKey('SHIFT', ''), 'Shift')
})

test('config normalization round-trips a modified numpad chord', () => {
  const config = defaultConfig()
  config.modules.pivotxr.activationBindings = [
    activation('toggle', keyboard('Ctrl', 'Numpad5')),
    activation('hold', keyboard('F10')),
  ]

  const normalized = normalizeConfig(config)

  assert.deepEqual(normalized.modules.pivotxr.activationBindings, [
    activation('toggle', keyboard('Ctrl', 'Numpad5')),
    activation('hold', keyboard('F10')),
  ])
})

test('legacy singular Pivot bindings normalize to canonical binding lists', () => {
  const config = defaultConfig()
  delete config.modules.pivotxr.activationBindings
  config.modules.pivotxr.activationBinding = keyboard('F8')

  const normalized = normalizeConfig(config)

  assert.deepEqual(normalized.modules.pivotxr.activationBindings, [activation('toggle', keyboard('F8'))])
  assert.equal('activationBinding' in normalized.modules.pivotxr, false)
})

test('legacy Pivot activation modes migrate to profile baseline and per-binding behavior', () => {
  const holdConfig = defaultConfig()
  delete holdConfig.modules.pivotxr.alwaysActive
  holdConfig.modules.pivotxr.activationMode = 'hold'
  holdConfig.modules.pivotxr.activationBindings = [keyboard('F9')]
  const hold = normalizeConfig(holdConfig).modules.pivotxr
  assert.equal(hold.alwaysActive, false)
  assert.deepEqual(hold.activationBindings, [activation('hold', keyboard('F9'))])

  const alwaysConfig = defaultConfig()
  delete alwaysConfig.modules.pivotxr.alwaysActive
  alwaysConfig.modules.pivotxr.activationMode = 'alwaysOn'
  alwaysConfig.modules.pivotxr.activationBindings = [keyboard('F10')]
  const always = normalizeConfig(alwaysConfig).modules.pivotxr
  assert.equal(always.alwaysActive, true)
  assert.deepEqual(always.activationBindings, [activation('toggle', keyboard('F10'))])
})

test('canonical Pivot activation bindings preserve mixed Toggle and Hold behavior', () => {
  const config = defaultConfig()
  config.modules.pivotxr.alwaysActive = true
  config.modules.pivotxr.activationBindings = [
    activation('toggle', keyboard('F8')),
    activation('hold', keyboard('F9')),
  ]

  const normalized = normalizeConfig(config).modules.pivotxr
  assert.equal(normalized.alwaysActive, true)
  assert.deepEqual(normalized.activationBindings, config.modules.pivotxr.activationBindings)
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
  config.modules.pivotxr.activationBindings = [activation('toggle', keyboard('Shift', 'F8'))]

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
  config.modules.pivotxr.activationBindings = [activation('toggle', keyboard('Shift', 'F8'))]
  config.modules.pivotxr.setOriginBindings = [keyboard('F8', 'Shift')]
  const focus = [
    ...config.modules.pivotxr.activationBindings.map((item) => item.binding),
    ...config.modules.pivotxr.setOriginBindings,
    ...config.modules.pivotxr.releaseOriginBindings,
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

  const activationSet = pivotBindingConflictWarnings(false, [activation('toggle', f8)], [keyboard('F8')], [])
  assert.equal(activationSet.length, 1)
  assert.equal(activationSet[0].title, 'Activation also sets the origin')
  assert.match(activationSet[0].message, /activation-control press/)

  const activationRelease = pivotBindingConflictWarnings(false, [activation('hold', f8)], [], [keyboard('F8')])
  assert.equal(activationRelease[0].title, 'Activation also releases the origin')

  const setRelease = pivotBindingConflictWarnings(false, [], [f8], [keyboard('F8')])
  assert.equal(setRelease[0].title, 'Set Origin is immediately canceled')

  const allThree = pivotBindingConflictWarnings(false, [activation('toggle', f8)], [keyboard('F8')], [keyboard('F8')])
  assert.equal(allThree.length, 1)
  assert.equal(allThree[0].title, 'One binding controls three conflicting actions')

  const duplicateActivation = pivotBindingConflictWarnings(false, [activation('toggle', f8), activation('hold', keyboard('F8'))], [], [])
  assert.equal(duplicateActivation.length, 1)
  assert.equal(duplicateActivation[0].title, 'F8 is duplicated')
})

test('legacy shared stepped settings migrate into both axes and glide', () => {
  const config = defaultConfig()
  const settings = config.modules.pivotxr.defaults
  settings.responseMode = 'stepped'
  settings.smoothing = 0.4
  settings.deadzoneDegrees = 18
  settings.pitchDeadzoneDegrees = 14
  settings.maxExtraYawDegrees = 90
  settings.maxExtraPitchDegrees = 55
  settings.stepTriggerDegrees = 12
  settings.stepAmountDegrees = 16
  settings.stepHysteresisDegrees = 5
  delete settings.stepGlideMode
  delete settings.stepGlideSeconds
  delete settings.yawStep
  delete settings.pitchStep
  delete settings.yawLeftStep
  delete settings.yawRightStep
  delete settings.pitchUpStep
  delete settings.pitchDownStep

  const migrated = normalizeConfig(config).modules.pivotxr.defaults

  assert.deepEqual(migrated.yawStep, {
    deadzoneDegrees: 18,
    triggerDegrees: 12,
    amountDegrees: 16,
    hysteresisDegrees: 5,
    maxExtraDegrees: 90,
  })
  assert.deepEqual(migrated.pitchStep, {
    deadzoneDegrees: 14,
    triggerDegrees: 12,
    amountDegrees: 16,
    hysteresisDegrees: 5,
    maxExtraDegrees: 55,
  })
  assert.deepEqual(migrated.yawLeftStep, migrated.yawStep)
  assert.deepEqual(migrated.yawRightStep, migrated.yawStep)
  assert.deepEqual(migrated.pitchUpStep, migrated.pitchStep)
  assert.deepEqual(migrated.pitchDownStep, migrated.pitchStep)
  assert.equal(migrated.stepGlideMode, 'glide')
  assert.equal(migrated.stepGlideSeconds, 0.08)
  assert.equal('stepTriggerDegrees' in migrated, false)
  assert.equal('stepAmountDegrees' in migrated, false)
  assert.equal('stepHysteresisDegrees' in migrated, false)
})

test('zero legacy smoothing migrates stepped response to Instant', () => {
  const config = defaultConfig()
  const settings = config.modules.pivotxr.defaults
  settings.responseMode = 'stepped'
  settings.smoothing = 0
  delete settings.stepGlideMode
  delete settings.stepGlideSeconds

  const migrated = normalizeConfig(config).modules.pivotxr.defaults

  assert.equal(migrated.stepGlideMode, 'instant')
  assert.equal(migrated.stepGlideSeconds, 0)
})

test('canonical stepped settings preserve basic and per-direction values', () => {
  const config = defaultConfig()
  const settings = config.modules.pivotxr.defaults
  settings.responseMode = 'stepped'
  settings.stepGlideMode = 'glide'
  settings.stepGlideSeconds = 0.18
  settings.advancedAxes = true
  settings.yawStep = { deadzoneDegrees: 7, triggerDegrees: 11, amountDegrees: 17, hysteresisDegrees: 3, maxExtraDegrees: 80 }
  settings.pitchStep = { deadzoneDegrees: 9, triggerDegrees: 13, amountDegrees: 19, hysteresisDegrees: 4, maxExtraDegrees: 60 }
  settings.yawLeftStep = { deadzoneDegrees: 6, triggerDegrees: 10, amountDegrees: 21, hysteresisDegrees: 2, maxExtraDegrees: 85 }
  settings.yawRightStep = { deadzoneDegrees: 8, triggerDegrees: 14, amountDegrees: 18, hysteresisDegrees: 5, maxExtraDegrees: 70 }
  settings.pitchUpStep = { deadzoneDegrees: 5, triggerDegrees: 9, amountDegrees: 15, hysteresisDegrees: 2, maxExtraDegrees: 45 }
  settings.pitchDownStep = { deadzoneDegrees: 12, triggerDegrees: 16, amountDegrees: 22, hysteresisDegrees: 6, maxExtraDegrees: 40 }

  const normalized = normalizeConfig(config).modules.pivotxr.defaults

  assert.equal(normalized.stepGlideMode, 'glide')
  assert.equal(normalized.stepGlideSeconds, 0.18)
  assert.deepEqual(normalized.yawStep, settings.yawStep)
  assert.deepEqual(normalized.pitchStep, settings.pitchStep)
  assert.deepEqual(normalized.yawLeftStep, settings.yawLeftStep)
  assert.deepEqual(normalized.yawRightStep, settings.yawRightStep)
  assert.deepEqual(normalized.pitchUpStep, settings.pitchUpStep)
  assert.deepEqual(normalized.pitchDownStep, settings.pitchDownStep)
})

test('new Pivot stepped response defaults to a short conservative glide', () => {
  const settings = defaultConfig().modules.pivotxr.defaults

  assert.equal(settings.stepGlideMode, 'glide')
  assert.equal(settings.stepGlideSeconds, 0.12)

})
test('new Pivot profiles own independent nested tuning objects', () => {
  const defaults = defaultConfig().modules.pivotxr.defaults
  const profile = createPivotProfile(defaults)

  profile.settings.yawStep.triggerDegrees = 22
  profile.settings.pitchDownStep.amountDegrees = 24
  profile.settings.yawLeft.rotationMultiplier = 2.2

  assert.equal(defaults.yawStep.triggerDegrees, 10)
  assert.equal(defaults.pitchDownStep.amountDegrees, 10)
  assert.equal(defaults.yawLeft.rotationMultiplier, 1.5)
})

test('explicit Continuous response overrides stepped profile defaults', () => {
  const config = defaultConfig()
  config.modules.pivotxr.defaults.responseMode = 'stepped'
  const profile = createPivotProfile(config.modules.pivotxr.defaults)
  profile.settings.responseMode = 'continuous'
  config.modules.pivotxr.profiles = [profile]

  const normalized = normalizeConfig(config)

  assert.equal(normalized.modules.pivotxr.profiles[0].settings.responseMode, 'continuous')
})

test('saved glide durations are normalized to the UI precision', () => {
  const config = defaultConfig()
  config.modules.pivotxr.defaults.stepGlideSeconds = 0.083764719824402

  const normalized = normalizeConfig(config)

  assert.equal(normalized.modules.pivotxr.defaults.stepGlideSeconds, 0.08)
})
