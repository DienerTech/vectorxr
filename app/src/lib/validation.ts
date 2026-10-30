import { formatList, t } from '../i18n/index.ts'
import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, FrameGenProfileConfig, FrameGenSettings, InputBinding, PivotViewControls, PivotXRProfileConfig, PivotXRSettings, QuadViewsProfileConfig, QuadViewsSettings, RegisteredApplication, VectorXRConfig } from './model'

// Runtime value imports from './model' are avoided so node:test can load this
// module directly; keep these lists in sync with model.ts.
const osdBodyRowNames = ['graph', 'performance', 'turbo', 'framegen', 'pivot', 'modules']
const frameGenSynthesisModeNames = ['interpolate', 'extrapolate']
const frameGenFlowQualityNames = ['performance', 'balanced', 'quality']
const frameGenMotionEstimationNames = ['shader', 'hardware']
const frameGenFrameMultiplierNames = ['auto', 'dynamic', '2x', '3x', '4x']
const frameGenDiagnosticViewNames = ['off', 'flow', 'confidence', 'tint']

function validateCoreConfig(core: CoreConfig): string[] {
  const errors: string[] = []

  for (const [name, low, high] of [
    ['horizontalDegrees', -40, 40], ['verticalDegrees', -35, 35], ['distanceMeters', .5, 3],
    ['scale', 25, 150], ['opacity', 30, 100], ['updateHz', 1, 20],
  ] as const) {
    const value = core.osd[name]
    if (!Number.isFinite(value) || value < low || value > high ||
        ((name === 'opacity' || name === 'updateHz') && !Number.isInteger(value))) errors.push(t('validation.osd.between', { field: name, min: low, max: high }))
  }
  errors.push(...validateInputBindings('core.osd.toggleBindings', core.osd.toggleBindings), ...validateInputBindings('core.osd.cycleBindings', core.osd.cycleBindings))
  if (!/^#[0-9a-f]{6}$/i.test(core.osd.customColor)) errors.push(t('validation.osd.customColor'))
  if (!['teal', 'copper', 'blue', 'violet', 'rose', 'custom'].includes(core.osd.accent)) errors.push(t('validation.osd.accent'))
  if (!['12', '24'].includes(core.osd.clockFormat)) errors.push(t('validation.osd.clockFormat'))
  if (!['all', 'fps', 'frameTime', 'none'].includes(core.osd.compactMetrics)) errors.push(t('validation.osd.compactMetrics'))
  if (core.osd.bodyOrder.length !== osdBodyRowNames.length || new Set(core.osd.bodyOrder).size !== osdBodyRowNames.length || core.osd.bodyOrder.some(row => !osdBodyRowNames.includes(row))) errors.push(t('validation.osd.bodyOrder'))

  if (!Number.isInteger(core.logRetentionFiles) || core.logRetentionFiles < 1 || core.logRetentionFiles > 50) {
    errors.push(t('validation.field.integerBetween', { field: 'core.logRetentionFiles', min: 1, max: 50 }))
  }

  if (typeof core.trackSeenApps !== 'boolean') {
    errors.push(t('validation.field.boolean', { field: 'core.trackSeenApps' }))
  }

  if (!Number.isFinite(core.sound.volume) || core.sound.volume < 0 || core.sound.volume > 100) {
    errors.push(between('core.sound.volume', 0, 100))
  }

  return errors
}

function validateDepthXRSettings(prefix: string, settings: DepthXRSettings): string[] {
  const errors: string[] = []

  if (typeof settings.depthAnchor !== 'boolean') {
    errors.push(t('validation.field.boolean', { field: prefix + 'depthAnchor' }))
  }

  const bounded = [
    ['stereoBoost', settings.stereoBoost, 0.0, 2.0],
    ['convergence', settings.convergence, -0.25, 0.25],
  ] as const

  for (const [name, value, min, max] of bounded) {
    if (!Number.isFinite(value) || value < min || value > max) {
      errors.push(t('validation.field.between', { field: prefix + name, min, max }))
    }
  }

  return errors
}

function validateSoundFeedback(prefix: string, binding: InputBinding): string[] {
  if (binding.type === 'none' || !binding.sound) {
    return []
  }

  const errors: string[] = []
  for (const field of ['activateSound', 'deactivateSound'] as const) {
    const path = binding.sound[field].trim()
    if (path && !/\.wav$/i.test(path)) {
      errors.push(t('validation.field.wavFile', { field: `${prefix}.sound.${field}` }))
    }
  }

  return errors
}

const deviceInputPathPattern = /^(?:button-([1-9]|[1-9][0-9]|1[0-1][0-9]|12[0-8])|hat-[1-4]-(up|up-right|right|down-right|down|down-left|left|up-left))$/

function validateInputBinding(prefix: string, binding: InputBinding): string[] {
  const errors: string[] = []

  if (binding.type === 'none') {
    return errors
  }

  if (binding.type === 'keyboard') {
    const chord = prefix + '.chord'
    if (binding.chord.length === 0) {
      errors.push(t('validation.binding.chordEmpty', { field: chord }))
    }

    const seen = new Set<string>()
    for (const key of binding.chord) {
      if (!/^(Ctrl|Alt|Shift|Space|F([1-9]|1[0-2])|Numpad[0-9]|[A-Z]|[0-9])$/.test(key)) {
        errors.push(t('validation.binding.chordUnsupportedKey', { field: chord, key }))
      }
      if (seen.has(key)) {
        errors.push(t('validation.field.duplicates', { field: chord, value: key }))
      }
      seen.add(key)
    }

    const primaryKeys = binding.chord.filter((key) => key !== 'Ctrl' && key !== 'Alt' && key !== 'Shift')
    if (primaryKeys.length !== 1) {
      errors.push(t('validation.binding.chordPrimaryKey', { field: chord }))
    }

    return [...errors, ...validateSoundFeedback(prefix, binding)]
  }

  if (!binding.deviceGuid.trim()) {
    errors.push(t('validation.field.required', { field: prefix + '.deviceGuid' }))
  }

  if (!binding.inputPath.trim()) {
    errors.push(t('validation.field.required', { field: prefix + '.inputPath' }))
  } else if (!deviceInputPathPattern.test(binding.inputPath.trim())) {
    errors.push(t('validation.binding.inputPath', { field: prefix + '.inputPath' }))
  }

  return [...errors, ...validateSoundFeedback(prefix, binding)]
}

function validateInputBindings(prefix: string, bindings: InputBinding[]): string[] {
  return bindings.flatMap((binding, index) => validateInputBinding(`${prefix}[${index}]`, binding))
}

function validatePivotStepTuning(prefix: string, tuning: PivotXRSettings['yawStep'], deadzoneMax: number): string[] {
  const errors: string[] = []
  if (!Number.isFinite(tuning.deadzoneDegrees) || tuning.deadzoneDegrees < 0 || tuning.deadzoneDegrees > deadzoneMax) {
    errors.push(between(prefix + 'deadzoneDegrees', 0, deadzoneMax))
  }
  if (!Number.isFinite(tuning.triggerDegrees) || tuning.triggerDegrees < 1 || tuning.triggerDegrees > 45) {
    errors.push(between(prefix + 'triggerDegrees', 1, 45))
  }
  if (!Number.isFinite(tuning.amountDegrees) || tuning.amountDegrees < 0 || tuning.amountDegrees > 60) {
    errors.push(between(prefix + 'amountDegrees', 0, 60))
  }
  if (!Number.isFinite(tuning.hysteresisDegrees) || tuning.hysteresisDegrees < 0 || tuning.hysteresisDegrees > 20) {
    errors.push(between(prefix + 'hysteresisDegrees', 0, 20))
  }
  if (Number.isFinite(tuning.hysteresisDegrees) && Number.isFinite(tuning.triggerDegrees) &&
    tuning.hysteresisDegrees >= tuning.triggerDegrees) {
    errors.push(t('validation.field.smallerThan', { field: prefix + 'hysteresisDegrees', other: 'triggerDegrees' }))
  }
  if (!Number.isFinite(tuning.maxExtraDegrees) || tuning.maxExtraDegrees < 0 || tuning.maxExtraDegrees > 180) {
    errors.push(between(prefix + 'maxExtraDegrees', 0, 180))
  }
  return errors
}

// "<field> must be between <min> and <max>"; field is a settings path, which
// stays as written in settings.json.
function between(field: string, min: number | string, max: number | string): string {
  return t('validation.field.between', { field, min, max })
}

function required(field: string): string {
  return t('validation.field.required', { field })
}
function validatePivotXRSettings(prefix: string, settings: PivotXRSettings): string[] {
  const errors: string[] = []

  if (typeof settings.applyMotionToSnapViews !== 'boolean') {
    errors.push(t('validation.field.boolean', { field: prefix + 'applyMotionToSnapViews' }))
  }
  const transitions: Array<[string, number]> = [
    ['deadzoneTransitionDegrees', settings.deadzoneTransitionDegrees],
    ['pitchDeadzoneTransitionDegrees', settings.pitchDeadzoneTransitionDegrees],
    ...(['yawLeft', 'yawRight', 'pitchUp', 'pitchDown'] as const).map(
      (key): [string, number] => [key + '.deadzoneTransitionDegrees', settings[key].deadzoneTransitionDegrees],
    ),
  ]
  for (const [name, value] of transitions) {
    if (!Number.isFinite(value) || value < 0 || value > 180) {
      errors.push(between(prefix + name, '0', '180'))
    }
  }
  for (const key of ['yaw', 'yawLeft', 'yawRight', 'pitchUp', 'pitchDown'] as const) {
    const direction = settings.positionAssist[key]
    for (const field of ['rightCm', 'upCm', 'forwardCm'] as const) {
      if (!Number.isFinite(direction[field]) || Math.abs(direction[field]) > 100) {
        errors.push(t('validation.field.betweenCm', { field: prefix + 'positionAssist.' + key + '.' + field, min: '-100', max: '100' }))
      }
    }
    const { startAngleDegrees, fullAngleDegrees } = direction
    if (!Number.isFinite(startAngleDegrees) || !Number.isFinite(fullAngleDegrees) ||
      startAngleDegrees < 0 || fullAngleDegrees > 180 || fullAngleDegrees <= startAngleDegrees) {
      errors.push(t('validation.pivot.positionAssistAngles', { field: prefix + 'positionAssist.' + key }))
    }
  }

  if (!Number.isFinite(settings.rotationMultiplier) || settings.rotationMultiplier < 1.0 || settings.rotationMultiplier > 3.0) {
    errors.push(between(prefix + 'rotationMultiplier', '1.0', '3.0'))
  }

  if (!Number.isFinite(settings.smoothing) || settings.smoothing < 0 || settings.smoothing > 1) {
    errors.push(between(prefix + 'smoothing', '0', '1'))
  }

  if (!Number.isFinite(settings.activationRampSeconds) || settings.activationRampSeconds < 0 || settings.activationRampSeconds > 2) {
    errors.push(between(prefix + 'activationRampSeconds', '0', '2'))
  }

  if (!Number.isFinite(settings.deadzoneDegrees) || settings.deadzoneDegrees < 0 || settings.deadzoneDegrees > 180) {
    errors.push(between(prefix + 'deadzoneDegrees', '0', '180'))
  }

  if (!Number.isFinite(settings.maxExtraYawDegrees) || settings.maxExtraYawDegrees < 0 || settings.maxExtraYawDegrees > 180) {
    errors.push(between(prefix + 'maxExtraYawDegrees', '0', '180'))
  }

  if (!Number.isFinite(settings.pitchRotationMultiplier) || settings.pitchRotationMultiplier < 1.0 || settings.pitchRotationMultiplier > 3.0) {
    errors.push(between(prefix + 'pitchRotationMultiplier', '1.0', '3.0'))
  }

  if (!Number.isFinite(settings.pitchDeadzoneDegrees) || settings.pitchDeadzoneDegrees < 0 || settings.pitchDeadzoneDegrees > 90) {
    errors.push(between(prefix + 'pitchDeadzoneDegrees', '0', '90'))
  }

  if (!Number.isFinite(settings.maxExtraPitchDegrees) || settings.maxExtraPitchDegrees < 0 || settings.maxExtraPitchDegrees > 180) {
    errors.push(between(prefix + 'maxExtraPitchDegrees', '0', '180'))
  }

  if (settings.responseMode !== 'continuous' && settings.responseMode !== 'stepped') {
    errors.push(t('validation.pivot.responseMode', { field: prefix + 'responseMode' }))
  }

  if (settings.stepGlideMode !== 'instant' && settings.stepGlideMode !== 'glide') {
    errors.push(t('validation.pivot.stepGlideMode', { field: prefix + 'stepGlideMode' }))
  }

  const minimumGlideSeconds = settings.stepGlideMode === 'glide' ? 0.01 : 0
  if (!Number.isFinite(settings.stepGlideSeconds) || settings.stepGlideSeconds < minimumGlideSeconds || settings.stepGlideSeconds > 2) {
    errors.push(between(prefix + 'stepGlideSeconds', minimumGlideSeconds, '2'))
  }

  errors.push(...validatePivotStepTuning(prefix + 'yawStep.', settings.yawStep, 180))
  errors.push(...validatePivotStepTuning(prefix + 'pitchStep.', settings.pitchStep, 90))

  const tunings: Array<[string, typeof settings.yawLeft, number]> = [
    ['yawLeft', settings.yawLeft, 180],
    ['yawRight', settings.yawRight, 180],
    ['pitchUp', settings.pitchUp, 90],
    ['pitchDown', settings.pitchDown, 90],
  ]
  for (const [name, tuning, deadzoneMax] of tunings) {
    if (!Number.isFinite(tuning.rotationMultiplier) || tuning.rotationMultiplier < 1.0 || tuning.rotationMultiplier > 3.0) {
      errors.push(between(prefix + name + '.rotationMultiplier', '1.0', '3.0'))
    }
    if (!Number.isFinite(tuning.deadzoneDegrees) || tuning.deadzoneDegrees < 0 || tuning.deadzoneDegrees > deadzoneMax) {
      errors.push(between(prefix + name + '.deadzoneDegrees', '0', deadzoneMax))
    }
    if (!Number.isFinite(tuning.maxExtraDegrees) || tuning.maxExtraDegrees < 0 || tuning.maxExtraDegrees > 180) {
      errors.push(between(prefix + name + '.maxExtraDegrees', '0', '180'))
    }
  }

  const stepTunings: Array<[string, typeof settings.yawStep, number]> = [
    ['yawLeftStep', settings.yawLeftStep, 180],
    ['yawRightStep', settings.yawRightStep, 180],
    ['pitchUpStep', settings.pitchUpStep, 90],
    ['pitchDownStep', settings.pitchDownStep, 90],
  ]
  for (const [name, tuning, deadzoneMax] of stepTunings) {
    errors.push(...validatePivotStepTuning(`${prefix}${name}.`, tuning, deadzoneMax))
  }

  return errors
}

function validatePivotNudgeSettings(prefix: string, nudges: PivotViewControls['nudges']): string[] {
  const errors: string[] = []
  if (!Number.isFinite(nudges.yawStepDegrees) || nudges.yawStepDegrees < 1 || nudges.yawStepDegrees > 90) errors.push(between(prefix + 'yawStepDegrees', '1', '90'))
  if (!Number.isFinite(nudges.pitchStepDegrees) || nudges.pitchStepDegrees < 1 || nudges.pitchStepDegrees > 60) errors.push(between(prefix + 'pitchStepDegrees', '1', '60'))
  if (!Number.isFinite(nudges.transitionSeconds) || nudges.transitionSeconds < 0 || nudges.transitionSeconds > 2) errors.push(between(prefix + 'transitionSeconds', '0', '2'))
  errors.push(...validateInputBindings(prefix + 'yawLeftBindings', nudges.yawLeftBindings))
  errors.push(...validateInputBindings(prefix + 'yawRightBindings', nudges.yawRightBindings))
  errors.push(...validateInputBindings(prefix + 'pitchUpBindings', nudges.pitchUpBindings))
  errors.push(...validateInputBindings(prefix + 'pitchDownBindings', nudges.pitchDownBindings))
  errors.push(...validateInputBindings(prefix + 'centerBindings', nudges.centerBindings))
  return errors
}

function validatePivotViewControls(prefix: string, controls: PivotViewControls): string[] {
  const errors = validatePivotNudgeSettings(prefix + 'nudges.', controls.nudges)

  const ids = new Set<string>()
  controls.quickViews.forEach((view, index) => {
    const viewPrefix = `${prefix}quickViews[${index}].` // i18n-ignore
    if (!view.id.trim()) errors.push(required(viewPrefix + 'id'))
    if (ids.has(view.id)) errors.push(t('validation.pivot.quickViewDuplicateId', { field: viewPrefix + 'id' }))
    ids.add(view.id)
    if (!view.name.trim()) errors.push(required(viewPrefix + 'name'))
    if (!Number.isFinite(view.yawDegrees) || view.yawDegrees < -180 || view.yawDegrees > 180) errors.push(between(viewPrefix + 'yawDegrees', '-180', '180'))
    if (!Number.isFinite(view.pitchDegrees) || view.pitchDegrees < -85 || view.pitchDegrees > 85) errors.push(between(viewPrefix + 'pitchDegrees', '-85', '85'))
    if (!Number.isFinite(view.transitionSeconds) || view.transitionSeconds < 0 || view.transitionSeconds > 2) errors.push(between(viewPrefix + 'transitionSeconds', '0', '2'))
    const positions: Array<[string, number]> = [
      ['positionRightCm', view.positionRightCm],
      ['positionUpCm', view.positionUpCm],
      ['positionForwardCm', view.positionForwardCm],
    ]
    positions.forEach(([name, value]) => {
      if (!Number.isFinite(value) || value < -100 || value > 100) errors.push(between(viewPrefix + name, '-100', '100'))
    })
    errors.push(...validateInputBindings(viewPrefix + 'activationBindings', view.activationBindings.map((item) => item.binding)))
  })
  return errors
}

function validateQuadViewsSettings(prefix: string, settings: QuadViewsSettings): string[] {
  const errors: string[] = []
  const bounded = [
    ['focusHorizontalSizePercent', settings.focusHorizontalSizePercent, 5, 100],
    ['focusVerticalSizePercent', settings.focusVerticalSizePercent, 5, 100],
    ['focusScale', settings.focusScale, 0.5, 3.0],
    ['peripheralScale', settings.peripheralScale, 0.1, 1.5],
    ['foveateSharpness', settings.foveateSharpness, 0, 100],
    ['transitionThicknessPercent', settings.transitionThicknessPercent, 0, 50],
    ['horizontalOffsetDegrees', settings.horizontalOffsetDegrees, -45, 45],
    ['verticalOffsetDegrees', settings.verticalOffsetDegrees, -45, 45],
    ['gazeSmoothing', settings.gazeSmoothing, 0, 1],
    ['gazeDeadzoneDegrees', settings.gazeDeadzoneDegrees, 0, 10],
  ] as const

  if (settings.trackingMode !== 'head' && settings.trackingMode !== 'eye') {
    errors.push(t('validation.quadviews.trackingMode', { field: prefix + 'trackingMode' }))
  }

  for (const [name, value, min, max] of bounded) {
    if (!Number.isFinite(value) || value < min || value > max) {
      errors.push(between(prefix + name, min, max))
    }
  }

  return errors
}

function validateFrameGenSettings(prefix: string, settings: FrameGenSettings): string[] {
  const errors: string[] = []

  if (!frameGenSynthesisModeNames.includes(settings.synthesisMode)) {
    errors.push(t('validation.framegen.synthesisMode', { field: prefix + 'synthesisMode' }))
  }
  if (!frameGenFlowQualityNames.includes(settings.flowQuality)) {
    errors.push(t('validation.framegen.flowQuality', { field: prefix + 'flowQuality' }))
  }
  if (!frameGenMotionEstimationNames.includes(settings.motionEstimation)) {
    errors.push(t('validation.framegen.motionEstimation', { field: prefix + 'motionEstimation' }))
  }
  if (settings.interpolateFlow !== undefined && !['auto', 'hardware', 'shader'].includes(settings.interpolateFlow)) {
    errors.push(t('validation.framegen.interpolateFlow', { field: prefix + 'interpolateFlow' }))
  }
  if (!frameGenFrameMultiplierNames.includes(settings.frameMultiplier)) {
    errors.push(t('validation.framegen.frameMultiplier', { field: prefix + 'frameMultiplier' }))
  }

  if (settings.extendHeadPrediction !== undefined && typeof settings.extendHeadPrediction !== 'boolean') {
    errors.push(t('validation.field.trueOrFalse', { field: prefix + 'extendHeadPrediction' }))
  }

  const bounded = [
    ['artifactSuppression', settings.artifactSuppression, 0, 100],
    ['motionLimitPercent', settings.motionLimitPercent, 2, 25],
  ] as const
  for (const [name, value, min, max] of bounded) {
    if (!Number.isInteger(value) || value < min || value > max) {
      errors.push(t('validation.field.wholeNumberFromTo', { field: prefix + name, min, max }))
    }
  }

  return errors
}

function validateApplications(applications: RegisteredApplication[]): string[] {
  const errors: string[] = []
  const seenIds = new Set<string>()
  const seenExes = new Set<string>()

  applications.forEach((application, index) => {
    const prefix = `applications[${index}].`

    if (!application.id.trim()) {
      errors.push(required(prefix + 'id'))
    }

    if (!/^[a-z0-9][a-z0-9-]*$/.test(application.id)) {
      errors.push(t('validation.applications.idFormat', { field: prefix + 'id' }))
    }

    const normalizedId = application.id.trim().toLowerCase()
    if (seenIds.has(normalizedId)) {
      errors.push(t('validation.applications.duplicateId', { field: prefix + 'id' }))
    }
    seenIds.add(normalizedId)

    if (!application.name.trim()) {
      errors.push(required(prefix + 'name'))
    }

    if (!application.match.exe.trim()) {
      errors.push(required(prefix + 'match.exe'))
    }

    const normalizedExe = application.match.exe
      .trim()
      .split(/[/\\]/)
      .pop()
      ?.toLowerCase() ?? ''
    if (normalizedExe) {
      if (seenExes.has(normalizedExe)) {
        errors.push(t('validation.applications.duplicateExe', { field: prefix + 'match.exe' }))
      }
      seenExes.add(normalizedExe)
    }
  })

  return errors
}

function validateDepthXRProfile(profile: DepthXRProfileConfig, index: number, applicationIds: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.depthxr.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(required(prefix + 'name'))
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(t('validation.applications.unknownApplication', { field: prefix + 'applicationIds', application: applicationId }))
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(t('validation.field.duplicates', { field: prefix + 'applicationIds', value: applicationId }))
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateDepthXRSettings(prefix + 'settings.', profile.settings))
  return errors
}

function validateDepthProfileConflicts(profiles: DepthXRProfileConfig[]): string[] {
  const errors: string[] = []
  const firstProfileByApplication = new Map<string, number>()

  profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex !== undefined) {
        errors.push(profileConflict(`modules.depthxr.profiles[${index}]`, firstIndex, applicationId))
      } else {
        firstProfileByApplication.set(applicationId, index)
      }
    }
  })

  return errors
}

function validatePivotXRProfile(profile: PivotXRProfileConfig, index: number, applicationIds: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.pivotxr.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(required(prefix + 'name'))
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(t('validation.applications.unknownApplication', { field: prefix + 'applicationIds', application: applicationId }))
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(t('validation.field.duplicates', { field: prefix + 'applicationIds', value: applicationId }))
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateInputBindings(prefix + 'activationBindings', profile.activationBindings.map((item) => item.binding)))
  errors.push(...validateInputBindings(prefix + 'setOriginBindings', profile.setOriginBindings))
  errors.push(...validateInputBindings(prefix + 'releaseOriginBindings', profile.releaseOriginBindings))
  errors.push(...validatePivotXRSettings(prefix, profile.settings))
  errors.push(...validatePivotViewControls(prefix + 'viewControls.', profile.viewControls))
  return errors
}

function validateQuadViewsProfile(profile: QuadViewsProfileConfig, index: number, applicationIds: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.quadviews.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(required(prefix + 'name'))
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(t('validation.applications.unknownApplication', { field: prefix + 'applicationIds', application: applicationId }))
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(t('validation.field.duplicates', { field: prefix + 'applicationIds', value: applicationId }))
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateQuadViewsSettings(prefix + 'settings.', profile.settings))
  return errors
}

function validateQuadViewsProfileConflicts(profiles: QuadViewsProfileConfig[]): string[] {
  const errors: string[] = []
  const firstProfileByApplication = new Map<string, number>()

  profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex !== undefined) {
        errors.push(profileConflict(`modules.quadviews.profiles[${index}]`, firstIndex, applicationId))
      } else {
        firstProfileByApplication.set(applicationId, index)
      }
    }
  })

  return errors
}

function validateFrameGenProfile(profile: FrameGenProfileConfig, index: number, applicationIds: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.framegen.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(required(prefix + 'name'))
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(t('validation.applications.unknownApplication', { field: prefix + 'applicationIds', application: applicationId }))
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(t('validation.field.duplicates', { field: prefix + 'applicationIds', value: applicationId }))
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateFrameGenSettings(prefix + 'settings.', profile.settings))
  return errors
}

function validateFrameGenProfileConflicts(profiles: FrameGenProfileConfig[]): string[] {
  const errors: string[] = []
  const firstProfileByApplication = new Map<string, number>()

  profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex !== undefined) {
        errors.push(profileConflict(`modules.framegen.profiles[${index}]`, firstIndex, applicationId))
      } else {
        firstProfileByApplication.set(applicationId, index)
      }
    }
  })

  return errors
}

// Mirrors the layer resolver: the first enabled profile targeting the
// application turns the module on; otherwise the default profile decides.
function moduleOnForApplication(module: { enabled: boolean; profiles: Array<{ enabled: boolean; applicationIds: string[] }> }, applicationId: string): { on: boolean; custom: boolean } {
  const profile = module.profiles.find((candidate) => candidate.enabled && candidate.applicationIds.includes(applicationId))
  return profile ? { on: true, custom: true } : { on: module.enabled, custom: false }
}

// Applications where Turbo and Frame Generation would both start. Frame
// Generation owns pacing in those sessions, so Turbo stays off. Applications
// covered only by both default profiles are reported once, as `defaults`.
export function frameGenTurboOverlap(config: VectorXRConfig): { applications: string[]; defaults: boolean } {
  const { turbo, framegen } = config.modules
  const applications = config.applications.flatMap((application) => {
    const turboState = moduleOnForApplication(turbo, application.id)
    const frameGenState = moduleOnForApplication(framegen, application.id)
    return turboState.on && frameGenState.on && (turboState.custom || frameGenState.custom) ? [application.name || application.id] : []
  })
  return { applications, defaults: turbo.enabled && framegen.enabled }
}

// Non-blocking advice: shown on the affected pages, never prevents Save.
export function configWarnings(config: VectorXRConfig): string[] {
  const overlap = frameGenTurboOverlap(config)
  const targets = overlap.applications.length > 3
    ? [...overlap.applications.slice(0, 3), t('validation.warnings.moreApplications', { count: overlap.applications.length - 3 })]
    : [...overlap.applications]
  if (overlap.defaults) targets.push(t('validation.warnings.applicationsWithoutProfile'))
  if (targets.length === 0) return []

  return [t('validation.warnings.turboFrameGenOverlap', { targets: formatList(targets) })]
}

function profileConflict(profile: string, first: number, application: string): string {
  return t('validation.profiles.conflict', { profile, first, application })
}

export function validateConfig(config: VectorXRConfig): string[] {
  const errors: string[] = []

  if (config.version !== 3) {
    errors.push(t('validation.version', { version: 3 }))
  }

  errors.push(...validateCoreConfig(config.core))
  errors.push(...validateApplications(config.applications))
  errors.push(...validateDepthXRSettings('modules.depthxr.defaults.', config.modules.depthxr.defaults))
  errors.push(...validateInputBindings('modules.depthxr.bindings.toggleEnabledBindings', config.modules.depthxr.bindings.toggleEnabledBindings))
  errors.push(...validateInputBindings('modules.depthxr.bindings.toggleAnchorBindings', config.modules.depthxr.bindings.toggleAnchorBindings))
  errors.push(...validatePivotXRSettings('modules.pivotxr.defaults.', config.modules.pivotxr.defaults))
  errors.push(...validateInputBindings('modules.pivotxr.activationBindings', config.modules.pivotxr.activationBindings.map((item) => item.binding)))
  errors.push(...validateInputBindings('modules.pivotxr.setOriginBindings', config.modules.pivotxr.setOriginBindings))
  errors.push(...validateInputBindings('modules.pivotxr.releaseOriginBindings', config.modules.pivotxr.releaseOriginBindings))
  errors.push(...validatePivotViewControls('modules.pivotxr.viewControls.', config.modules.pivotxr.viewControls))
  const nudgeSetIds = new Set<string>()
  config.modules.pivotxr.nudgeSets.forEach((set, index) => {
    const prefix = `modules.pivotxr.nudgeSets[${index}].` // i18n-ignore
    if (!set.id.trim()) errors.push(required(prefix + 'id'))
    if (set.id === 'none') errors.push(t('validation.pivot.nudgeSetReservedId', { field: prefix + 'id' }))
    if (nudgeSetIds.has(set.id)) errors.push(t('validation.pivot.nudgeSetDuplicateId', { field: prefix + 'id' }))
    nudgeSetIds.add(set.id)
    if (!set.name.trim()) errors.push(required(prefix + 'name'))
    errors.push(...validatePivotNudgeSettings(prefix + 'settings.', set.settings))
  })
  if (config.modules.pivotxr.nudgeSetId !== 'none' && !nudgeSetIds.has(config.modules.pivotxr.nudgeSetId)) {
    errors.push(t('validation.pivot.unknownNudgeSet', { field: 'modules.pivotxr.nudgeSetId' }))
  }
  config.modules.pivotxr.profiles.forEach((profile, index) => {
    if (profile.nudgeSetId !== 'none' && !nudgeSetIds.has(profile.nudgeSetId)) {
      errors.push(t('validation.pivot.unknownNudgeSet', { field: `modules.pivotxr.profiles[${index}].nudgeSetId` })) // i18n-ignore
    }
  })
  errors.push(...validateQuadViewsSettings('modules.quadviews.defaults.', config.modules.quadviews.defaults))
  errors.push(...validateInputBinding('modules.quadviews.diagnosticVisualizationBinding', config.modules.quadviews.diagnosticVisualizationBinding))

  const applicationIds = new Set(config.applications.map((application) => application.id))

  config.modules.depthxr.profiles.forEach((profile, index) => {
    errors.push(...validateDepthXRProfile(profile, index, applicationIds))
  })
  errors.push(...validateDepthProfileConflicts(config.modules.depthxr.profiles))

  // Shadowed pivot bindings (two profiles sharing a binding for the same app)
  // are legitimate priority-order behavior, surfaced as a warning on the Pivot
  // tab — they deliberately do not block saving.
  config.modules.pivotxr.profiles.forEach((profile, index) => {
    errors.push(...validatePivotXRProfile(profile, index, applicationIds))
  })

  config.modules.quadviews.profiles.forEach((profile, index) => {
    errors.push(...validateQuadViewsProfile(profile, index, applicationIds))
  })
  errors.push(...validateQuadViewsProfileConflicts(config.modules.quadviews.profiles))

  errors.push(...validateInputBindings('modules.turbo.toggleBindings', config.modules.turbo.toggleBindings))
  errors.push(...validateInputBindings('modules.turbo.metricsBindings', config.modules.turbo.metricsBindings))
  for (const experiment of [config.modules.turbo.experimental, ...config.modules.turbo.profiles.map(profile => profile.experimental)]) {
  if (!Number.isInteger(experiment.predictionPercent) || experiment.predictionPercent < 50 || experiment.predictionPercent > 100) {
    errors.push(t('validation.turbo.predictionPercent'))
  }
  if (!Number.isInteger(experiment.frameLimit) || (experiment.frameLimit !== 0 && (experiment.frameLimit < 20 || experiment.frameLimit > 240))) {
    errors.push(t('validation.turbo.frameLimit'))
  }
  }
  errors.push(...validateInputBindings('modules.framegen.toggleBindings', config.modules.framegen.toggleBindings))
  if (!frameGenDiagnosticViewNames.includes(config.modules.framegen.diagnosticView)) {
    errors.push(t('validation.framegen.diagnosticView', { field: 'modules.framegen.diagnosticView' }))
  }
  errors.push(...validateFrameGenSettings('modules.framegen.defaults.', config.modules.framegen.defaults))
  config.modules.framegen.profiles.forEach((profile, index) => {
    errors.push(...validateFrameGenProfile(profile, index, applicationIds))
  })
  errors.push(...validateFrameGenProfileConflicts(config.modules.framegen.profiles))

  config.modules.turbo.profiles.forEach((profile, index) => {
    const prefix = `modules.turbo.profiles[${index}].`
    if (!profile.name.trim()) {
      errors.push(required(prefix + 'name'))
    }
    for (const applicationId of profile.applicationIds) {
      if (!applicationIds.has(applicationId)) {
        errors.push(t('validation.applications.unknownApplication', { field: prefix + 'applicationIds', application: applicationId }))
      }
    }
  })

  return errors
}
