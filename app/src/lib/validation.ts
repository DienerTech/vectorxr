import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, InputBinding, PivotXRProfileConfig, PivotXRSettings, RegisteredApplication, VectorXRConfig } from './model'
import { bindingLabel } from './model'

function validateCoreConfig(core: CoreConfig): string[] {
  const errors: string[] = []

  if (!Number.isInteger(core.logRetentionFiles) || core.logRetentionFiles < 1 || core.logRetentionFiles > 50) {
    errors.push('core.logRetentionFiles must be an integer between 1 and 50')
  }

  return errors
}

function validateDepthXRSettings(prefix: string, settings: DepthXRSettings): string[] {
  const errors: string[] = []

  const bounded = [
    ['stereoBoost', settings.stereoBoost, 0.5, 2.0],
    ['convergence', settings.convergence, -0.5, 0.5],
  ] as const

  for (const [name, value, min, max] of bounded) {
    if (Number.isNaN(value) || value < min || value > max) {
      errors.push(`${prefix}${name} must be between ${min} and ${max}`)
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

    return errors
  }

  if (!binding.deviceGuid.trim()) {
    errors.push(`${prefix}.deviceGuid is required`)
  }

  if (!binding.inputPath.trim()) {
    errors.push(`${prefix}.inputPath is required`)
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

  if (Number.isNaN(settings.deadzoneDegrees) || settings.deadzoneDegrees < 0 || settings.deadzoneDegrees > 45) {
    errors.push(`${prefix}deadzoneDegrees must be between 0 and 45`)
  }

  if (Number.isNaN(settings.maxExtraYawDegrees) || settings.maxExtraYawDegrees < 0 || settings.maxExtraYawDegrees > 180) {
    errors.push(`${prefix}maxExtraYawDegrees must be between 0 and 180`)
  }

  if (Number.isNaN(settings.pitchRotationMultiplier) || settings.pitchRotationMultiplier < 1.0 || settings.pitchRotationMultiplier > 3.0) {
    errors.push(`${prefix}pitchRotationMultiplier must be between 1.0 and 3.0`)
  }

  if (Number.isNaN(settings.pitchSmoothing) || settings.pitchSmoothing < 0 || settings.pitchSmoothing > 1) {
    errors.push(`${prefix}pitchSmoothing must be between 0 and 1`)
  }

  if (Number.isNaN(settings.pitchDeadzoneDegrees) || settings.pitchDeadzoneDegrees < 0 || settings.pitchDeadzoneDegrees > 45) {
    errors.push(`${prefix}pitchDeadzoneDegrees must be between 0 and 45`)
  }

  if (Number.isNaN(settings.maxExtraPitchDegrees) || settings.maxExtraPitchDegrees < 0 || settings.maxExtraPitchDegrees > 180) {
    errors.push(`${prefix}maxExtraPitchDegrees must be between 0 and 180`)
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

    const normalizedExe = application.match.exe.trim().toLowerCase()
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

  if (profile.applicationIds.length === 0) {
    errors.push(`${prefix}applicationIds must include at least one application`)
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

  if (profile.applicationIds.length === 0) {
    errors.push(`${prefix}applicationIds must include at least one application`)
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

  const applicationIds = new Set(config.applications.map((application) => application.id))

  config.modules.depthxr.profiles.forEach((profile, index) => {
    errors.push(...validateDepthXRProfile(profile, index, applicationIds))
  })
  errors.push(...validateDepthProfileConflicts(config.modules.depthxr.profiles))

  config.modules.pivotxr.profiles.forEach((profile, index) => {
    errors.push(...validatePivotXRProfile(profile, index, applicationIds))
  })
  errors.push(...validatePivotProfileConflicts(config.modules.pivotxr.profiles))

  return errors
}
