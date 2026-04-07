import type { DepthXRConfig, GlobalSettings, ProfileConfig } from './model'

function validateSettingsBlock(prefix: string, settings: GlobalSettings | ProfileConfig): string[] {
  const errors: string[] = []

  const bounded = [
    ['stereoBoost', settings.stereoBoost, 0.5, 2.0],
    ['convergence', settings.convergence, -0.5, 0.5],
    ['worldScale', settings.worldScale, 0.5, 2.0],
    ['fovScale', settings.fovScale, 0.5, 1.5],
  ] as const

  for (const [name, value, min, max] of bounded) {
    if (Number.isNaN(value) || value < min || value > max) {
      errors.push(`${prefix}${name} must be between ${min} and ${max}`)
    }
  }

  return errors
}

export function validateConfig(config: DepthXRConfig): string[] {
  const errors: string[] = []

  if (config.version !== 1) {
    errors.push('version must equal 1')
  }

  errors.push(...validateSettingsBlock('global.', config.global))

  const seen = new Set<string>()
  config.profiles.forEach((profile, index) => {
    const prefix = `profiles[${index}].`
    if (!profile.match.exe.trim()) {
      errors.push(`${prefix}match.exe is required`)
    }

    const normalizedExe = profile.match.exe.trim().toLowerCase()
    if (seen.has(normalizedExe)) {
      errors.push(`${prefix}match.exe duplicates another profile`)
    }
    seen.add(normalizedExe)

    errors.push(...validateSettingsBlock(prefix, profile))
  })

  return errors
}
