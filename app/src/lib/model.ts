export type LogLevel = 'off' | 'error' | 'info' | 'debug'

export interface GlobalSettings {
  enabled: boolean
  stereoBoostEnabled: boolean
  convergenceEnabled: boolean
  worldScaleEnabled: boolean
  fovScaleEnabled: boolean
  stereoBoost: number
  convergence: number
  worldScale: number
  fovScale: number
  logLevel: LogLevel
}

export interface ProfileConfig extends GlobalSettings {
  match: {
    exe: string
  }
}

export interface DepthXRConfig {
  version: 1
  global: GlobalSettings
  profiles: ProfileConfig[]
}

export interface ConfigEnvelope {
  path: string
  config: DepthXRConfig
}

export function defaultGlobalSettings(): GlobalSettings {
  return {
    enabled: true,
    stereoBoostEnabled: true,
    convergenceEnabled: true,
    worldScaleEnabled: true,
    fovScaleEnabled: true,
    stereoBoost: 1.1,
    convergence: 0,
    worldScale: 1.0,
    fovScale: 1.0,
    logLevel: 'info',
  }
}

export function defaultConfig(): DepthXRConfig {
  return {
    version: 1,
    global: defaultGlobalSettings(),
    profiles: [],
  }
}

export function createProfile(globalSettings: GlobalSettings): ProfileConfig {
  return {
    match: {
      exe: 'Game.exe',
    },
    enabled: globalSettings.enabled,
    stereoBoostEnabled: globalSettings.stereoBoostEnabled,
    convergenceEnabled: globalSettings.convergenceEnabled,
    worldScaleEnabled: globalSettings.worldScaleEnabled,
    fovScaleEnabled: globalSettings.fovScaleEnabled,
    stereoBoost: globalSettings.stereoBoost,
    convergence: globalSettings.convergence,
    worldScale: globalSettings.worldScale,
    fovScale: globalSettings.fovScale,
    logLevel: globalSettings.logLevel,
  }
}
