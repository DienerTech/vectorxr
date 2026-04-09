export type LogLevel = 'off' | 'error' | 'info' | 'debug'
export type ActivationMode = 'toggle' | 'hold'

export interface CoreConfig {
  enabled: boolean
  logLevel: LogLevel
  logRetentionFiles: number
}

export interface DepthXRSettings {
  stereoBoostEnabled: boolean
  convergenceEnabled: boolean
  stereoBoost: number
  convergence: number
}

export interface DepthXRProfileConfig {
  name: string
  enabled: boolean
  match: {
    exe: string
  }
  settings: DepthXRSettings
}

export interface DepthXRModuleConfig {
  enabled: boolean
  defaults: DepthXRSettings
  profiles: DepthXRProfileConfig[]
}

export interface PivotXRDefaults {
  activationMode: ActivationMode
  rotationMultiplier: number
  smoothing: number
  deadzoneDegrees: number
}

export interface PivotXRModuleConfig {
  enabled: boolean
  defaults: PivotXRDefaults
}

export interface VectorXRConfig {
  version: 2
  core: CoreConfig
  modules: {
    depthxr: DepthXRModuleConfig
    pivotxr: PivotXRModuleConfig
  }
}

export interface ConfigEnvelope {
  path: string
  config: VectorXRConfig
}

export type AppTab = 'core' | 'depthxr' | 'pivotxr'

interface LegacyGlobalSettings {
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

interface LegacyProfileConfig extends LegacyGlobalSettings {
  match: {
    exe: string
  }
}

interface LegacyDepthXRConfig {
  version: 1
  global: LegacyGlobalSettings
  profiles: LegacyProfileConfig[]
}

type UnknownRecord = Record<string, unknown>

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === 'object' && value !== null
}

function normalizeLogLevel(value: unknown): LogLevel {
  if (value === 'off' || value === 'error' || value === 'info' || value === 'debug') {
    return value
  }

  return 'info'
}

function normalizeBoolean(value: unknown, fallback: boolean): boolean {
  return typeof value === 'boolean' ? value : fallback
}

function normalizeNumber(value: unknown, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

function normalizeString(value: unknown, fallback: string): string {
  return typeof value === 'string' ? value : fallback
}

export function defaultCoreConfig(): CoreConfig {
  return {
    enabled: true,
    logLevel: 'info',
    logRetentionFiles: 7,
  }
}

export function defaultDepthXRSettings(): DepthXRSettings {
  return {
    stereoBoostEnabled: true,
    convergenceEnabled: true,
    stereoBoost: 1.1,
    convergence: 0,
  }
}

export function defaultPivotXRDefaults(): PivotXRDefaults {
  return {
    activationMode: 'toggle',
    rotationMultiplier: 1.5,
    smoothing: 0.2,
    deadzoneDegrees: 8,
  }
}

export function defaultConfig(): VectorXRConfig {
  return {
    version: 2,
    core: defaultCoreConfig(),
    modules: {
      depthxr: {
        enabled: true,
        defaults: defaultDepthXRSettings(),
        profiles: [],
      },
      pivotxr: {
        enabled: false,
        defaults: defaultPivotXRDefaults(),
      },
    },
  }
}

export function sanitizeProfileName(exe: string): string {
  const trimmed = exe.trim()
  if (!trimmed) {
    return 'New Profile'
  }

  const basename = trimmed.split(/[/\\]/).pop() ?? trimmed
  const dot = basename.lastIndexOf('.')
  if (dot > 0) {
    return basename.slice(0, dot)
  }

  return basename
}

export function createProfile(defaultSettings: DepthXRSettings): DepthXRProfileConfig {
  return {
    name: sanitizeProfileName('Game.exe'),
    enabled: true,
    match: {
      exe: 'Game.exe',
    },
    settings: { ...defaultSettings },
  }
}

function isLegacyDepthXRConfig(value: unknown): value is LegacyDepthXRConfig {
  return isRecord(value) && value.version === 1 && 'global' in value && 'profiles' in value
}

function isVectorXRConfig(value: unknown): value is VectorXRConfig {
  return isRecord(value) && value.version === 2 && 'core' in value && 'modules' in value
}

function normalizeDepthXRSettings(value: unknown, fallback: DepthXRSettings): DepthXRSettings {
  const source = isRecord(value) ? value : {}

  return {
    stereoBoostEnabled: normalizeBoolean(source.stereoBoostEnabled, fallback.stereoBoostEnabled),
    convergenceEnabled: normalizeBoolean(source.convergenceEnabled, fallback.convergenceEnabled),
    stereoBoost: normalizeNumber(source.stereoBoost, fallback.stereoBoost),
    convergence: normalizeNumber(source.convergence, fallback.convergence),
  }
}

function normalizePivotXRDefaults(value: unknown, fallback: PivotXRDefaults): PivotXRDefaults {
  const source = isRecord(value) ? value : {}
  const activationMode = source.activationMode === 'hold' ? 'hold' : fallback.activationMode

  return {
    activationMode,
    rotationMultiplier: normalizeNumber(source.rotationMultiplier, fallback.rotationMultiplier),
    smoothing: normalizeNumber(source.smoothing, fallback.smoothing),
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
  }
}

function normalizeVectorXRConfig(value: unknown): VectorXRConfig {
  const fallback = defaultConfig()
  const source = isRecord(value) ? value : {}
  const core = isRecord(source.core) ? source.core : {}
  const modules = isRecord(source.modules) ? source.modules : {}
  const depthxr = isRecord(modules.depthxr) ? modules.depthxr : {}
  const pivotxr = isRecord(modules.pivotxr) ? modules.pivotxr : {}
  const profileValues = Array.isArray(depthxr.profiles) ? depthxr.profiles : []

  return {
    version: 2,
    core: {
      enabled: normalizeBoolean(core.enabled, fallback.core.enabled),
      logLevel: normalizeLogLevel(core.logLevel),
      logRetentionFiles: normalizeNumber(core.logRetentionFiles, fallback.core.logRetentionFiles),
    },
    modules: {
      depthxr: {
        enabled: normalizeBoolean(depthxr.enabled, fallback.modules.depthxr.enabled),
        defaults: normalizeDepthXRSettings(depthxr.defaults, fallback.modules.depthxr.defaults),
        profiles: profileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const match = isRecord(profile.match) ? profile.match : {}
          const settings = normalizeDepthXRSettings(profile.settings, fallback.modules.depthxr.defaults)

          return {
            name: normalizeString(profile.name, sanitizeProfileName(normalizeString(match.exe, 'Game.exe'))),
            enabled: normalizeBoolean(profile.enabled, true),
            match: {
              exe: normalizeString(match.exe, 'Game.exe'),
            },
            settings,
          }
        }),
      },
      pivotxr: {
        enabled: normalizeBoolean(pivotxr.enabled, fallback.modules.pivotxr.enabled),
        defaults: normalizePivotXRDefaults(pivotxr.defaults, fallback.modules.pivotxr.defaults),
      },
    },
  }
}

export function migrateLegacyConfig(config: LegacyDepthXRConfig): VectorXRConfig {
  return {
    version: 2,
    core: {
      enabled: true,
      logLevel: normalizeLogLevel(config.global.logLevel),
      logRetentionFiles: 7,
    },
    modules: {
      depthxr: {
        enabled: config.global.enabled,
        defaults: {
          stereoBoostEnabled: config.global.stereoBoostEnabled,
          convergenceEnabled: config.global.convergenceEnabled,
          stereoBoost: config.global.stereoBoost,
          convergence: config.global.convergence,
        },
        profiles: config.profiles.map((profile) => ({
          name: sanitizeProfileName(profile.match.exe),
          enabled: profile.enabled,
          match: {
            exe: profile.match.exe,
          },
          settings: {
            stereoBoostEnabled: profile.stereoBoostEnabled,
            convergenceEnabled: profile.convergenceEnabled,
            stereoBoost: profile.stereoBoost,
            convergence: profile.convergence,
          },
        })),
      },
      pivotxr: {
        enabled: false,
        defaults: defaultPivotXRDefaults(),
      },
    },
  }
}

export function normalizeConfig(config: unknown): VectorXRConfig {
  if (isVectorXRConfig(config)) {
    return normalizeVectorXRConfig(config)
  }

  if (isLegacyDepthXRConfig(config)) {
    return normalizeVectorXRConfig(migrateLegacyConfig(config))
  }

  return defaultConfig()
}

export function cloneConfig(config: VectorXRConfig): VectorXRConfig {
  return JSON.parse(JSON.stringify(config)) as VectorXRConfig
}
