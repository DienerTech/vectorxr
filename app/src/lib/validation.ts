import type { CoreConfig, DepthXRProfileConfig, DepthXRSettings, PivotXRDefaults, VectorXRConfig } from './model'

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

function validateDepthXRProfile(profile: DepthXRProfileConfig, index: number, seen: Set<string>): string[] {
  const errors: string[] = []
  const prefix = `modules.depthxr.profiles[${index}].`

  if (!profile.name.trim()) {
    errors.push(`${prefix}name is required`)
  }

  if (!profile.match.exe.trim()) {
    errors.push(`${prefix}match.exe is required`)
  }

  const normalizedExe = profile.match.exe.trim().toLowerCase()
  if (normalizedExe) {
    if (seen.has(normalizedExe)) {
      errors.push(`${prefix}match.exe duplicates another profile`)
    }
    seen.add(normalizedExe)
  }

  errors.push(...validateDepthXRSettings(`${prefix}settings.`, profile.settings))
  return errors
}

export function validateConfig(config: VectorXRConfig): string[] {
  const errors: string[] = []

  if (config.version !== 2) {
    errors.push('version must equal 2')
  }

  errors.push(...validateCoreConfig(config.core))
  errors.push(...validateDepthXRSettings('modules.depthxr.defaults.', config.modules.depthxr.defaults))
  errors.push(...validatePivotXRDefaults(config.modules.pivotxr.defaults))

  const seen = new Set<string>()
  config.modules.depthxr.profiles.forEach((profile, index) => {
    errors.push(...validateDepthXRProfile(profile, index, seen))
  })

  return errors
}
