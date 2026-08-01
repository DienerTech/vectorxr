import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, InputBinding, PivotViewControls, PivotXRProfileConfig, PivotXRSettings, QuadViewsProfileConfig, QuadViewsSettings, RegisteredApplication, VectorXRConfig } from './model'

function validateCoreConfig(core: CoreConfig): string[] {
  const errors: string[] = []

  if (!Number.isInteger(core.logRetentionFiles) || core.logRetentionFiles < 1 || core.logRetentionFiles > 50) {
    errors.push('core.logRetentionFiles must be an integer between 1 and 50')
  }

  if (typeof core.trackSeenApps !== 'boolean') {
    errors.push('core.trackSeenApps must be a boolean')
  }

  if (!Number.isFinite(core.sound.volume) || core.sound.volume < 0 || core.sound.volume > 100) {
    errors.push('core.sound.volume must be between 0 and 100')
  }

  return errors
}

function validateDepthXRSettings(prefix: string, settings: DepthXRSettings): string[] {
  const errors: string[] = []

  if (typeof settings.depthAnchor !== 'boolean') {
    errors.push(`${prefix}depthAnchor must be a boolean`)
  }

  const bounded = [
    ['stereoBoost', settings.stereoBoost, 0.0, 2.0],
    ['convergence', settings.convergence, -0.25, 0.25],
  ] as const

  for (const [name, value, min, max] of bounded) {
    if (Number.isNaN(value) || value < min || value > max) {
      errors.push(`${prefix}${name} must be between ${min} and ${max}`)
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
      errors.push(`${prefix}.sound.${field} must point to a .wav file`)
    }
  }

  return errors
}

function validateInputBinding(prefix: string, binding: InputBinding): string[] {
  const errors: string[] = []

  if (binding.type === 'none') {
    return errors
  }

  if (binding.type === 'keyboard') {
    if (binding.chord.length === 0) {
      errors.push(`${prefix}.chord must include at least one key`)
    }

    const seen = new Set<string>()
    for (const key of binding.chord) {
      if (!/^(Ctrl|Alt|Shift|Space|F([1-9]|1[0-2])|Numpad[0-9]|[A-Z]|[0-9])$/.test(key)) {
        errors.push(`${prefix}.chord contains unsupported key: ${key}`)
      }
      if (seen.has(key)) {
        errors.push(`${prefix}.chord duplicates ${key}`)
      }
      seen.add(key)
    }

    const primaryKeys = binding.chord.filter((key) => key !== 'Ctrl' && key !== 'Alt' && key !== 'Shift')
    if (primaryKeys.length !== 1) {
      errors.push(`${prefix}.chord must include exactly one non-modifier key`)
    }

    return [...errors, ...validateSoundFeedback(prefix, binding)]
  }

  if (!binding.deviceGuid.trim()) {
    errors.push(`${prefix}.deviceGuid is required`)
  }

  if (!binding.inputPath.trim()) {
    errors.push(`${prefix}.inputPath is required`)
  } else if (!/^button-([1-9]|[1-9][0-9]|1[0-1][0-9]|12[0-8])$/.test(binding.inputPath.trim())) {
    errors.push(`${prefix}.inputPath must use button-1 through button-128`)
  }

  return [...errors, ...validateSoundFeedback(prefix, binding)]
}

function validateInputBindings(prefix: string, bindings: InputBinding[]): string[] {
  return bindings.flatMap((binding, index) => validateInputBinding(`${prefix}[${index}]`, binding))
}

function validatePivotStepTuning(prefix: string, tuning: PivotXRSettings['yawStep'], deadzoneMax: number): string[] {
  const errors: string[] = []
  if (Number.isNaN(tuning.deadzoneDegrees) || tuning.deadzoneDegrees < 0 || tuning.deadzoneDegrees > deadzoneMax) {
    errors.push(`${prefix}deadzoneDegrees must be between 0 and ${deadzoneMax}`)
  }
  if (Number.isNaN(tuning.triggerDegrees) || tuning.triggerDegrees < 1 || tuning.triggerDegrees > 45) {
    errors.push(`${prefix}triggerDegrees must be between 1 and 45`)
  }
  if (Number.isNaN(tuning.amountDegrees) || tuning.amountDegrees < 0 || tuning.amountDegrees > 60) {
    errors.push(`${prefix}amountDegrees must be between 0 and 60`)
  }
  if (Number.isNaN(tuning.hysteresisDegrees) || tuning.hysteresisDegrees < 0 || tuning.hysteresisDegrees > 20) {
    errors.push(`${prefix}hysteresisDegrees must be between 0 and 20`)
  }
  if (tuning.hysteresisDegrees >= tuning.triggerDegrees) {
    errors.push(`${prefix}hysteresisDegrees must be smaller than triggerDegrees`)
  }
  if (Number.isNaN(tuning.maxExtraDegrees) || tuning.maxExtraDegrees < 0 || tuning.maxExtraDegrees > 180) {
    errors.push(`${prefix}maxExtraDegrees must be between 0 and 180`)
  }
  return errors
}
function validatePivotXRSettings(prefix: string, settings: PivotXRSettings): string[] {
  const errors: string[] = []

  if (Number.isNaN(settings.rotationMultiplier) || settings.rotationMultiplier < 1.0 || settings.rotationMultiplier > 3.0) {
    errors.push(`${prefix}rotationMultiplier must be between 1.0 and 3.0`)
  }

  if (Number.isNaN(settings.smoothing) || settings.smoothing < 0 || settings.smoothing > 1) {
    errors.push(`${prefix}smoothing must be between 0 and 1`)
  }

  if (Number.isNaN(settings.activationRampSeconds) || settings.activationRampSeconds < 0 || settings.activationRampSeconds > 2) {
    errors.push(`${prefix}activationRampSeconds must be between 0 and 2`)
  }

  if (Number.isNaN(settings.deadzoneDegrees) || settings.deadzoneDegrees < 0 || settings.deadzoneDegrees > 180) {
    errors.push(`${prefix}deadzoneDegrees must be between 0 and 180`)
  }

  if (Number.isNaN(settings.maxExtraYawDegrees) || settings.maxExtraYawDegrees < 0 || settings.maxExtraYawDegrees > 180) {
    errors.push(`${prefix}maxExtraYawDegrees must be between 0 and 180`)
  }

  if (Number.isNaN(settings.pitchRotationMultiplier) || settings.pitchRotationMultiplier < 1.0 || settings.pitchRotationMultiplier > 3.0) {
    errors.push(`${prefix}pitchRotationMultiplier must be between 1.0 and 3.0`)
  }

  if (Number.isNaN(settings.pitchDeadzoneDegrees) || settings.pitchDeadzoneDegrees < 0 || settings.pitchDeadzoneDegrees > 90) {
    errors.push(`${prefix}pitchDeadzoneDegrees must be between 0 and 90`)
  }

  if (Number.isNaN(settings.maxExtraPitchDegrees) || settings.maxExtraPitchDegrees < 0 || settings.maxExtraPitchDegrees > 180) {
    errors.push(`${prefix}maxExtraPitchDegrees must be between 0 and 180`)
  }

  if (settings.responseMode !== 'continuous' && settings.responseMode !== 'stepped') {
    errors.push(`${prefix}responseMode must be "continuous" or "stepped"`)
  }

  if (settings.stepGlideMode !== 'instant' && settings.stepGlideMode !== 'glide') {
    errors.push(`${prefix}stepGlideMode must be "instant" or "glide"`)
  }

  const minimumGlideSeconds = settings.stepGlideMode === 'glide' ? 0.01 : 0
  if (Number.isNaN(settings.stepGlideSeconds) || settings.stepGlideSeconds < minimumGlideSeconds || settings.stepGlideSeconds > 2) {
    errors.push(`${prefix}stepGlideSeconds must be between ${minimumGlideSeconds} and 2`)
  }

  errors.push(...validatePivotStepTuning(`${prefix}yawStep.`, settings.yawStep, 180))
  errors.push(...validatePivotStepTuning(`${prefix}pitchStep.`, settings.pitchStep, 90))

  const tunings: Array<[string, typeof settings.yawLeft, number]> = [
    ['yawLeft', settings.yawLeft, 180],
    ['yawRight', settings.yawRight, 180],
    ['pitchUp', settings.pitchUp, 90],
    ['pitchDown', settings.pitchDown, 90],
  ]
  for (const [name, tuning, deadzoneMax] of tunings) {
    if (Number.isNaN(tuning.rotationMultiplier) || tuning.rotationMultiplier < 1.0 || tuning.rotationMultiplier > 3.0) {
      errors.push(`${prefix}${name}.rotationMultiplier must be between 1.0 and 3.0`)
    }
    if (Number.isNaN(tuning.deadzoneDegrees) || tuning.deadzoneDegrees < 0 || tuning.deadzoneDegrees > deadzoneMax) {
      errors.push(`${prefix}${name}.deadzoneDegrees must be between 0 and ${deadzoneMax}`)
    }
    if (Number.isNaN(tuning.maxExtraDegrees) || tuning.maxExtraDegrees < 0 || tuning.maxExtraDegrees > 180) {
      errors.push(`${prefix}${name}.maxExtraDegrees must be between 0 and 180`)
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

function validatePivotViewControls(prefix: string, controls: PivotViewControls): string[] {
  const errors: string[] = []
  const nudges = controls.nudges
  if (!Number.isFinite(nudges.yawStepDegrees) || nudges.yawStepDegrees < 1 || nudges.yawStepDegrees > 90) errors.push(`${prefix}nudges.yawStepDegrees must be between 1 and 90`)
  if (!Number.isFinite(nudges.pitchStepDegrees) || nudges.pitchStepDegrees < 1 || nudges.pitchStepDegrees > 60) errors.push(`${prefix}nudges.pitchStepDegrees must be between 1 and 60`)
  if (!Number.isFinite(nudges.transitionSeconds) || nudges.transitionSeconds < 0 || nudges.transitionSeconds > 2) errors.push(`${prefix}nudges.transitionSeconds must be between 0 and 2`)
  errors.push(...validateInputBindings(`${prefix}nudges.yawLeftBindings`, nudges.yawLeftBindings))
  errors.push(...validateInputBindings(`${prefix}nudges.yawRightBindings`, nudges.yawRightBindings))
  errors.push(...validateInputBindings(`${prefix}nudges.pitchUpBindings`, nudges.pitchUpBindings))
  errors.push(...validateInputBindings(`${prefix}nudges.pitchDownBindings`, nudges.pitchDownBindings))
  errors.push(...validateInputBindings(`${prefix}nudges.centerBindings`, nudges.centerBindings))

  const ids = new Set<string>()
  controls.quickViews.forEach((view, index) => {
    const viewPrefix = `${prefix}quickViews[${index}].`
    if (!view.id.trim()) errors.push(`${viewPrefix}id is required`)
    if (ids.has(view.id)) errors.push(`${viewPrefix}id duplicates another Quick View`)
    ids.add(view.id)
    if (!view.name.trim()) errors.push(`${viewPrefix}name is required`)
    if (!Number.isFinite(view.yawDegrees) || view.yawDegrees < -180 || view.yawDegrees > 180) errors.push(`${viewPrefix}yawDegrees must be between -180 and 180`)
    if (!Number.isFinite(view.pitchDegrees) || view.pitchDegrees < -85 || view.pitchDegrees > 85) errors.push(`${viewPrefix}pitchDegrees must be between -85 and 85`)
    if (!Number.isFinite(view.transitionSeconds) || view.transitionSeconds < 0 || view.transitionSeconds > 2) errors.push(`${viewPrefix}transitionSeconds must be between 0 and 2`)
    const positions: Array<[string, number]> = [
      ['positionRightCm', view.positionRightCm],
      ['positionUpCm', view.positionUpCm],
      ['positionForwardCm', view.positionForwardCm],
    ]
    positions.forEach(([name, value]) => {
      if (!Number.isFinite(value) || value < -100 || value > 100) errors.push(`${viewPrefix}${name} must be between -100 and 100`)
    })
    errors.push(...validateInputBindings(`${viewPrefix}activationBindings`, view.activationBindings.map((item) => item.binding)))
  })
  return errors
}

function validateQuadViewsSettings(prefix: string, settings: QuadViewsSettings): string[] {
  const errors: string[] = []
  const bounded = [
    ['focusHorizontalSizePercent', settings.focusHorizontalSizePercent, 5, 100],
    ['focusVerticalSizePercent', settings.focusVerticalSizePercent, 5, 100],
    ['focusScale', settings.focusScale, 0.5, 2.0],
    ['peripheralScale', settings.peripheralScale, 0.1, 1.5],
    ['foveateSharpness', settings.foveateSharpness, 0, 100],
    ['transitionThicknessPercent', settings.transitionThicknessPercent, 0, 50],
    ['horizontalOffsetDegrees', settings.horizontalOffsetDegrees, -45, 45],
    ['verticalOffsetDegrees', settings.verticalOffsetDegrees, -45, 45],
    ['gazeSmoothing', settings.gazeSmoothing, 0, 1],
    ['gazeDeadzoneDegrees', settings.gazeDeadzoneDegrees, 0, 10],
  ] as const

  if (settings.trackingMode !== 'head' && settings.trackingMode !== 'eye') {
    errors.push(`${prefix}trackingMode must be head or eye`)
  }

  for (const [name, value, min, max] of bounded) {
    if (Number.isNaN(value) || value < min || value > max) {
      errors.push(`${prefix}${name} must be between ${min} and ${max}`)
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
      errors.push(`${prefix}id is required`)
    }

    if (!/^[a-z0-9][a-z0-9-]*$/.test(application.id)) {
      errors.push(`${prefix}id must use lowercase letters, numbers, and hyphens`)
    }

    const normalizedId = application.id.trim().toLowerCase()
    if (seenIds.has(normalizedId)) {
      errors.push(`${prefix}id duplicates another application`)
    }
    seenIds.add(normalizedId)

    if (!application.name.trim()) {
      errors.push(`${prefix}name is required`)
    }

    if (!application.match.exe.trim()) {
      errors.push(`${prefix}match.exe is required`)
    }

    const normalizedExe = application.match.exe
      .trim()
      .split(/[/\\]/)
      .pop()
      ?.toLowerCase() ?? ''
    if (normalizedExe) {
      if (seenExes.has(normalizedExe)) {
        errors.push(`${prefix}match.exe duplicates another application`)
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
    errors.push(`${prefix}name is required`)
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds references unknown application: ${applicationId}`)
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds duplicates ${applicationId}`)
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateDepthXRSettings(`${prefix}settings.`, profile.settings))
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
        errors.push(`modules.depthxr.profiles[${index}] conflicts with profiles[${firstIndex}] for application ${applicationId}; first enabled profile wins`)
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
    errors.push(`${prefix}name is required`)
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds references unknown application: ${applicationId}`)
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds duplicates ${applicationId}`)
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateInputBindings(`${prefix}activationBindings`, profile.activationBindings.map((item) => item.binding)))
  errors.push(...validateInputBindings(`${prefix}setOriginBindings`, profile.setOriginBindings))
  errors.push(...validateInputBindings(`${prefix}releaseOriginBindings`, profile.releaseOriginBindings))
  errors.push(...validatePivotXRSettings(prefix, profile.settings))
  errors.push(...validatePivotViewControls(`${prefix}viewControls.`, profile.viewControls))
  return errors
}

function validateQuadViewsProfile(profile: QuadViewsProfileConfig, index: number, applicationIds: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.quadviews.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(`${prefix}name is required`)
  }

  const seenProfileApplicationIds = new Set<string>()
  for (const applicationId of profile.applicationIds) {
    if (!applicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds references unknown application: ${applicationId}`)
    }
    if (seenProfileApplicationIds.has(applicationId)) {
      errors.push(`${prefix}applicationIds duplicates ${applicationId}`)
    }
    seenProfileApplicationIds.add(applicationId)
  }

  errors.push(...validateQuadViewsSettings(`${prefix}settings.`, profile.settings))
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
        errors.push(`modules.quadviews.profiles[${index}] conflicts with profiles[${firstIndex}] for application ${applicationId}; first enabled profile wins`)
      } else {
        firstProfileByApplication.set(applicationId, index)
      }
    }
  })

  return errors
}

export function validateConfig(config: VectorXRConfig): string[] {
  const errors: string[] = []

  if (config.version !== 3) {
    errors.push('version must equal 3')
  }

  errors.push(...validateCoreConfig(config.core))
  errors.push(...validateApplications(config.applications))
  errors.push(...validateDepthXRSettings('modules.depthxr.defaults.', config.modules.depthxr.defaults))
  errors.push(...validateInputBinding('modules.depthxr.bindings.toggleEnabled', config.modules.depthxr.bindings.toggleEnabled))
  errors.push(...validateInputBinding('modules.depthxr.bindings.toggleAnchor', config.modules.depthxr.bindings.toggleAnchor))
  errors.push(...validatePivotXRSettings('modules.pivotxr.defaults.', config.modules.pivotxr.defaults))
  errors.push(...validateInputBindings('modules.pivotxr.activationBindings', config.modules.pivotxr.activationBindings.map((item) => item.binding)))
  errors.push(...validateInputBindings('modules.pivotxr.setOriginBindings', config.modules.pivotxr.setOriginBindings))
  errors.push(...validateInputBindings('modules.pivotxr.releaseOriginBindings', config.modules.pivotxr.releaseOriginBindings))
  errors.push(...validatePivotViewControls('modules.pivotxr.viewControls.', config.modules.pivotxr.viewControls))
  errors.push(...validateQuadViewsSettings('modules.quadviews.defaults.', config.modules.quadviews.defaults))

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

  errors.push(...validateInputBinding('modules.turbo.toggleBinding', config.modules.turbo.toggleBinding))
  config.modules.turbo.profiles.forEach((profile, index) => {
    const prefix = `modules.turbo.profiles[${index}].`
    if (!profile.name.trim()) {
      errors.push(`${prefix}name is required`)
    }
    for (const applicationId of profile.applicationIds) {
      if (!applicationIds.has(applicationId)) {
        errors.push(`${prefix}applicationIds references unknown application: ${applicationId}`)
      }
    }
  })

  return errors
}
