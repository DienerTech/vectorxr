import assert from 'node:assert/strict'
import test from 'node:test'
import { readFileSync } from 'node:fs'
import {
  applyPivotPositionAssistPreset,
  createPivotProfile,
  createPivotQuickView,
  defaultConfig,
  matchingPivotPositionAssistPreset,
  normalizeConfig,
  pivotSnapStackingStatus,
  pivotSnapViewMotionNote,
  pivotSnapViewsConfigured,
} from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

function expandedMotionSettings() {
  const settings = defaultConfig().modules.pivotxr.defaults
  settings.applyMotionToSnapViews = true
  settings.deadzoneTransitionDegrees = 4
  settings.pitchDeadzoneTransitionDegrees = 2
  settings.yawLeft.deadzoneTransitionDegrees = 7
  settings.positionAssist.enabled = true
  settings.positionAssist.advancedDirections = true
  Object.assign(settings.positionAssist.yaw, { rightCm: 10, upCm: 4, startAngleDegrees: 10, fullAngleDegrees: 80 })
  Object.assign(settings.positionAssist.pitchUp, { upCm: 12, forwardCm: -5 })
  return settings
}

function snapViewsProfile(applicationIds) {
  const profile = createPivotProfile(defaultConfig().modules.pivotxr.defaults)
  profile.behavior = 'snapViews'
  profile.applicationIds = applicationIds
  profile.viewControls.quickViews = [{ ...createPivotQuickView('Check Six'), yawDegrees: 180 }]
  return profile
}

test('expanded Pivot settings round-trip for the default and custom profiles', () => {
  const config = defaultConfig()
  const motion = expandedMotionSettings()
  config.modules.pivotxr.defaults = motion
  config.modules.pivotxr.profiles.push(createPivotProfile(motion))
  const restored = normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(restored.modules.pivotxr.defaults, motion)
  assert.deepEqual(restored.modules.pivotxr.profiles[0].settings, motion)
  assert.deepEqual(validateConfig(restored), [])
})

test('custom profiles copy Position Assist instead of sharing it with the defaults', () => {
  const config = defaultConfig()
  config.modules.pivotxr.defaults = expandedMotionSettings()
  config.modules.pivotxr.profiles.push(createPivotProfile(config.modules.pivotxr.defaults))
  const restored = normalizeConfig(JSON.parse(JSON.stringify(config)))
  restored.modules.pivotxr.profiles[0].settings.positionAssist.yaw.rightCm = 22
  assert.equal(restored.modules.pivotxr.defaults.positionAssist.yaw.rightCm, 10)
})

test('old Pivot configurations keep exclusive snaps and the hard deadzone', () => {
  const config = defaultConfig()
  const settings = config.modules.pivotxr.defaults
  delete settings.applyMotionToSnapViews
  delete settings.positionAssist
  delete settings.deadzoneTransitionDegrees
  delete settings.pitchDeadzoneTransitionDegrees
  for (const direction of ['yawLeft', 'yawRight', 'pitchUp', 'pitchDown']) delete settings[direction].deadzoneTransitionDegrees
  const restored = normalizeConfig(config).modules.pivotxr.defaults
  assert.equal(restored.applyMotionToSnapViews, false)
  assert.equal(restored.positionAssist.enabled, false)
  assert.equal(restored.deadzoneTransitionDegrees, 0)
  assert.equal(restored.yawLeft.deadzoneTransitionDegrees, 0)
})

test('out-of-range Position Assist and non-finite transitions are rejected before save', () => {
  const config = defaultConfig()
  const settings = config.modules.pivotxr.defaults
  settings.positionAssist.yaw.fullAngleDegrees = settings.positionAssist.yaw.startAngleDegrees
  settings.positionAssist.pitchUp.upCm = 101
  settings.deadzoneTransitionDegrees = Infinity
  const errors = validateConfig(config).join(' ')
  assert.match(errors, /full-offset/)
  assert.match(errors, /pitchUp.upCm/)
  assert.match(errors, /deadzoneTransitionDegrees/)
})

test('the public schema describes the expanded Pivot fields', () => {
  const schema = JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json', import.meta.url), 'utf8')).$defs
  const settings = Object.values(schema).find((definition) => definition.properties?.responseMode && definition.properties?.rotationMultiplier)
  for (const field of Object.keys(defaultConfig().modules.pivotxr.defaults)) assert.ok(field in settings.properties, field)
  assert.equal(settings.properties.applyMotionToSnapViews.default, false)
  assert.equal(schema.pivotAxisTuning.properties.deadzoneTransitionDegrees.default, 0)
})

test('the Default profile cannot stack Enhanced Motion on Snap Views', () => {
  const status = pivotSnapStackingStatus(defaultConfig(), 'default')
  assert.equal(status.available, false)
  assert.match(status.message, /custom Enhanced Motion profile/)
})

test('custom motion profiles can stack and say when no Snap Views profile shares an application', () => {
  const config = defaultConfig()
  const motion = createPivotProfile(config.modules.pivotxr.defaults)
  motion.applicationIds = ['dcs']
  config.modules.pivotxr.profiles.push(motion)
  const alone = pivotSnapStackingStatus(config, 0)
  assert.equal(alone.available, true)
  assert.match(alone.message, /No enabled Snap Views profile/)

  config.modules.pivotxr.profiles.push(snapViewsProfile(['dcs']))
  assert.deepEqual(pivotSnapStackingStatus(config, 0), { available: true, message: '' })
})

test('Snap Views profiles name the Enhanced Motion profiles that keep working in their views', () => {
  const config = defaultConfig()
  assert.match(pivotSnapViewMotionNote(config, 'default'), /Not available on the Default profile/)

  const motion = createPivotProfile(config.modules.pivotxr.defaults)
  motion.name = 'Hornet Motion'
  motion.applicationIds = ['dcs']
  const otherGame = createPivotProfile(config.modules.pivotxr.defaults)
  otherGame.name = 'MSFS Motion'
  otherGame.applicationIds = ['msfs']
  otherGame.settings.applyMotionToSnapViews = true
  config.modules.pivotxr.profiles.push(motion, otherGame, snapViewsProfile(['dcs']))
  assert.match(pivotSnapViewMotionNote(config, 2), /can keep working in these Snap Views when it turns on/)

  motion.settings.applyMotionToSnapViews = true
  assert.equal(pivotSnapViewMotionNote(config, 2), 'Enhanced Motion from Hornet Motion keeps working in these Snap Views.')

  motion.enabled = false
  assert.match(pivotSnapViewMotionNote(config, 2), /can keep working/)
})

test('the OSD reserves a Snap View line only when Snap Views are configured', () => {
  const config = defaultConfig()
  assert.equal(pivotSnapViewsConfigured(config), false)
  config.modules.pivotxr.profiles.push(snapViewsProfile(['dcs']))
  assert.equal(pivotSnapViewsConfigured(config), true)
  config.modules.pivotxr.profiles[0].enabled = false
  assert.equal(pivotSnapViewsConfigured(config), false)
})

test('Position Assist presets fill turning in either direction mode and are recognized', () => {
  const assist = defaultConfig().modules.pivotxr.defaults.positionAssist
  assert.equal(matchingPivotPositionAssistPreset(assist), 'none')

  applyPivotPositionAssistPreset(assist, 'gentle')
  assert.deepEqual(assist.yaw, { rightCm: 12, upCm: 4, forwardCm: 0, startAngleDegrees: 20, fullAngleDegrees: 70 })
  assert.equal(assist.yawRight.rightCm, 12)
  assert.equal(assist.yawLeft.rightCm, -12)
  assert.equal(assist.pitchUp.upCm, 0)
  assert.equal(matchingPivotPositionAssistPreset(assist), 'gentle')
  assist.advancedDirections = true
  assert.equal(matchingPivotPositionAssistPreset(assist), 'gentle')

  assist.pitchDown.forwardCm = 4
  assert.equal(matchingPivotPositionAssistPreset(assist), null)
  applyPivotPositionAssistPreset(assist, 'strong')
  assert.equal(matchingPivotPositionAssistPreset(assist), 'strong')
  assert.equal(assist.pitchDown.forwardCm, 0)

  applyPivotPositionAssistPreset(assist, 'none')
  assert.equal(Object.is(assist.yawLeft.rightCm, -0), false)
  assert.equal(matchingPivotPositionAssistPreset(assist), 'none')

  const config = defaultConfig()
  applyPivotPositionAssistPreset(config.modules.pivotxr.defaults.positionAssist, 'strong')
  config.modules.pivotxr.defaults.positionAssist.enabled = true
  assert.equal(validateConfig(config).filter((error) => error.includes('positionAssist')).length, 0)
})

test('cleared number fields fail validation instead of saving as text', () => {
  // An emptied number input stores '', which compares as 0; the layer rejects
  // a config holding it and runs every module off.
  for (const cleared of ['', null, 'abc']) {
    const config = defaultConfig()
    const profile = createPivotProfile(config.modules.pivotxr.defaults)
    config.modules.pivotxr.profiles.push(profile)
    const { settings } = profile
    settings.stepGlideMode = 'instant'
    Object.assign(settings, { smoothing: cleared, maxExtraYawDegrees: cleared, stepGlideSeconds: cleared })
    Object.assign(settings.yawStep, { amountDegrees: cleared, maxExtraDegrees: cleared })
    settings.pitchStep.triggerDegrees = cleared
    settings.yawLeft.maxExtraDegrees = cleared
    settings.yawLeftStep.amountDegrees = cleared
    config.modules.pivotxr.defaults.yawStep.maxExtraDegrees = cleared
    config.modules.quadviews.defaults.focusScale = cleared
    config.modules.depthxr.defaults.stereoBoost = cleared

    const errors = validateConfig(config)
    for (const field of [
      'modules.pivotxr.profiles[0].smoothing',
      'modules.pivotxr.profiles[0].maxExtraYawDegrees',
      'modules.pivotxr.profiles[0].stepGlideSeconds',
      'modules.pivotxr.profiles[0].yawStep.amountDegrees',
      'modules.pivotxr.profiles[0].yawStep.maxExtraDegrees',
      'modules.pivotxr.profiles[0].pitchStep.triggerDegrees',
      'modules.pivotxr.profiles[0].yawLeft.maxExtraDegrees',
      'modules.pivotxr.profiles[0].yawLeftStep.amountDegrees',
      'modules.pivotxr.defaults.yawStep.maxExtraDegrees',
      'modules.quadviews.defaults.focusScale',
      'modules.depthxr.defaults.stereoBoost',
    ]) {
      assert.ok(errors.some((error) => error.startsWith(`${field} must be between`)), `${JSON.stringify(cleared)} ${field}`)
    }
    assert.equal(errors.some((error) => error.includes('pitchStep.hysteresisDegrees must be smaller')), false)
  }
})
