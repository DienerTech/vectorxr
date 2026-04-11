import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, PivotXRDefaults, RegisteredApplication, VectorXRConfig } from './model'

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

function validatePivotXRDefaults(defaults: PivotXRDefaults): string[] {
  const errors: string[] = []

  if (Number.isNaN(defaults.rotationMultiplier) || defaults.rotationMultiplier < 1.0 || defaults.rotationMultiplier > 3.0) {
    errors.push('modules.pivotxr.defaults.rotationMultiplier must be between 1.0 and 3.0')
  }

  if (Number.isNaN(defaults.smoothing) || defaults.smoothing < 0 || defaults.smoothing > 1) {
    errors.push('modules.pivotxr.defaults.smoothing must be between 0 and 1')
  }

  if (Number.isNaN(defaults.deadzoneDegrees) || defaults.deadzoneDegrees < 0 || defaults.deadzoneDegrees > 45) {
    errors.push('modules.pivotxr.defaults.deadzoneDegrees must be between 0 and 45')
  }

  if (Number.isNaN(defaults.maxExtraYawDegrees) || defaults.maxExtraYawDegrees < 0 || defaults.maxExtraYawDegrees > 45) {
    errors.push('modules.pivotxr.defaults.maxExtraYawDegrees must be between 0 and 45')
  }

  if (Number.isNaN(defaults.pitchRotationMultiplier) || defaults.pitchRotationMultiplier < 1.0 || defaults.pitchRotationMultiplier > 3.0) {
    errors.push('modules.pivotxr.defaults.pitchRotationMultiplier must be between 1.0 and 3.0')
  }

  if (Number.isNaN(defaults.pitchSmoothing) || defaults.pitchSmoothing < 0 || defaults.pitchSmoothing > 1) {
    errors.push('modules.pivotxr.defaults.pitchSmoothing must be between 0 and 1')
  }

  if (Number.isNaN(defaults.pitchDeadzoneDegrees) || defaults.pitchDeadzoneDegrees < 0 || defaults.pitchDeadzoneDegrees > 45) {
    errors.push('modules.pivotxr.defaults.pitchDeadzoneDegrees must be between 0 and 45')
  }

  if (Number.isNaN(defaults.maxExtraPitchDegrees) || defaults.maxExtraPitchDegrees < 0 || defaults.maxExtraPitchDegrees > 45) {
    errors.push('modules.pivotxr.defaults.maxExtraPitchDegrees must be between 0 and 45')
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

export function validateConfig(config: VectorXRConfig): string[] {
  const errors: string[] = []

  if (config.version !== 3) {
    errors.push('version must equal 3')
  }

  errors.push(...validateCoreConfig(config.core))
  errors.push(...validateApplications(config.applications))
  errors.push(...validateDepthXRSettings('modules.depthxr.defaults.', config.modules.depthxr.defaults))
  errors.push(...validatePivotXRDefaults(config.modules.pivotxr.defaults))

  const applicationIds = new Set(config.applications.map((application) => application.id))
  config.modules.depthxr.profiles.forEach((profile, index) => {
    errors.push(...validateDepthXRProfile(profile, index, applicationIds))
  })
  errors.push(...validateDepthProfileConflicts(config.modules.depthxr.profiles))

  return errors
}
