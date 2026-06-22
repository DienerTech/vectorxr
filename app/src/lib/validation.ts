import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, InputBinding, PivotXRProfileConfig, PivotXRSettings, QuadViewsProfileConfig, QuadViewsSettings, RegisteredApplication, VectorXRConfig } from './model'
import { bindingLabel } from './model'

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
      if (!/^(Ctrl|Alt|Shift|Space|F([1-9]|1[0-2])|[A-Z]|[0-9])$/.test(key)) {
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

  if (Number.isNaN(settings.deadzoneDegrees) || settings.deadzoneDegrees < 0 || settings.deadzoneDegrees > 45) {
    errors.push(`${prefix}deadzoneDegrees must be between 0 and 45`)
  }

  if (Number.isNaN(settings.maxExtraYawDegrees) || settings.maxExtraYawDegrees < 0 || settings.maxExtraYawDegrees > 180) {
    errors.push(`${prefix}maxExtraYawDegrees must be between 0 and 180`)
  }

  if (Number.isNaN(settings.pitchRotationMultiplier) || settings.pitchRotationMultiplier < 1.0 || settings.pitchRotationMultiplier > 3.0) {
    errors.push(`${prefix}pitchRotationMultiplier must be between 1.0 and 3.0`)
  }

  if (Number.isNaN(settings.pitchDeadzoneDegrees) || settings.pitchDeadzoneDegrees < 0 || settings.pitchDeadzoneDegrees > 45) {
    errors.push(`${prefix}pitchDeadzoneDegrees must be between 0 and 45`)
  }

  if (Number.isNaN(settings.maxExtraPitchDegrees) || settings.maxExtraPitchDegrees < 0 || settings.maxExtraPitchDegrees > 180) {
    errors.push(`${prefix}maxExtraPitchDegrees must be between 0 and 180`)
  }

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

  errors.push(...validateInputBinding(`${prefix}activationBinding`, profile.activationBinding))
  errors.push(...validatePivotXRSettings(prefix, profile.settings))
  return errors
}

function validatePivotProfileConflicts(profiles: PivotXRProfileConfig[]): string[] {
  const errors: string[] = []

  profiles.forEach((profile, index) => {
    if (!profile.enabled || profile.activationBinding.type === 'none') {
      return
    }

    const thisLabel = bindingLabel(profile.activationBinding)

    profiles.forEach((other, otherIndex) => {
      if (otherIndex >= index || !other.enabled || other.activationBinding.type === 'none') {
        return
      }

      const otherLabel = bindingLabel(other.activationBinding)
      if (thisLabel !== otherLabel) {
        return
      }

      for (const applicationId of profile.applicationIds) {
        if (other.applicationIds.includes(applicationId)) {
          errors.push(
            `modules.pivotxr.profiles[${index}] and profiles[${otherIndex}] share binding ${thisLabel} for application ${applicationId}; first enabled profile executes`,
          )
          break
        }
      }
    })
  })

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
  errors.push(...validatePivotXRSettings('modules.pivotxr.defaults.', config.modules.pivotxr.defaults))
  errors.push(...validateInputBinding('modules.pivotxr.activationBinding', config.modules.pivotxr.activationBinding))
  errors.push(...validateQuadViewsSettings('modules.quadviews.defaults.', config.modules.quadviews.defaults))

  const applicationIds = new Set(config.applications.map((application) => application.id))

  config.modules.depthxr.profiles.forEach((profile, index) => {
    errors.push(...validateDepthXRProfile(profile, index, applicationIds))
  })
  errors.push(...validateDepthProfileConflicts(config.modules.depthxr.profiles))

  config.modules.pivotxr.profiles.forEach((profile, index) => {
    errors.push(...validatePivotXRProfile(profile, index, applicationIds))
  })
  errors.push(...validatePivotProfileConflicts(config.modules.pivotxr.profiles))

  config.modules.quadviews.profiles.forEach((profile, index) => {
    errors.push(...validateQuadViewsProfile(profile, index, applicationIds))
  })
  errors.push(...validateQuadViewsProfileConflicts(config.modules.quadviews.profiles))

  return errors
}
