export type LogLevel = 'off' | 'error' | 'info' | 'debug'

export interface GlobalSettings {
  enabled: boolean
  stereoBoostEnabled: boolean
  worldScaleEnabled: boolean
  fovScaleEnabled: boolean
  stereoBoost: number
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
    worldScaleEnabled: true,
    fovScaleEnabled: true,
    stereoBoost: 1.1,
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
    worldScaleEnabled: globalSettings.worldScaleEnabled,
    fovScaleEnabled: globalSettings.fovScaleEnabled,
    stereoBoost: globalSettings.stereoBoost,
    worldScale: globalSettings.worldScale,
    fovScale: globalSettings.fovScale,
    logLevel: globalSettings.logLevel,
  }
}
