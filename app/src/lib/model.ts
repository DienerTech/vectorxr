export type LogLevel = 'off' | 'error' | 'info' | 'debug'
export type ActivationMode = 'toggle' | 'hold'
export type AppTab = 'core' | 'depthxr' | 'pivotxr'
export const pivotActivationKeyGroups = [
  {
    label: 'Function Keys',
    options: Array.from({ length: 12 }, (_, index) => `F${index + 1}`),
  },
  {
    label: 'Letter Keys',
    options: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split(''),
  },
  {
    label: 'Number Keys',
    options: '0123456789'.split(''),
  },
  {
    label: 'Special Keys',
    options: ['Space'],
  },
] as const

export interface CoreConfig {
  enabled: boolean
  logLevel: LogLevel
  logRetentionFiles: number
}

export interface RegisteredApplication {
  id: string
  name: string
  enabled: boolean
  match: {
    exe: string
  }
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
  applicationIds: string[]
  settings: DepthXRSettings
}

export interface DepthXRModuleConfig {
  enabled: boolean
  defaults: DepthXRSettings
  profiles: DepthXRProfileConfig[]
}

export interface PivotXRDefaults {
  activationMode: ActivationMode
  activationKey: string
  rotationMultiplier: number
  smoothing: number
  deadzoneDegrees: number
  maxExtraYawDegrees: number
  pitchRotationMultiplier: number
  pitchSmoothing: number
  pitchDeadzoneDegrees: number
  maxExtraPitchDegrees: number
}

export interface PivotXRModuleConfig {
  enabled: boolean
  defaults: PivotXRDefaults
}

export interface VectorXRConfig {
  version: 3
  core: CoreConfig
  applications: RegisteredApplication[]
  modules: {
    depthxr: DepthXRModuleConfig
    pivotxr: PivotXRModuleConfig
  }
}

export interface ConfigEnvelope {
  path: string
  config: VectorXRConfig
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

export function sanitizeApplicationId(value: string): string {
  const normalized = value
    .trim()
    .toLowerCase()
    .replace(/\.[^.]+$/, '')
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '')

  return normalized || 'application'
}

export function uniqueApplicationId(base: string, applications: RegisteredApplication[]): string {
  const stem = sanitizeApplicationId(base)
  const existing = new Set(applications.map((application) => application.id.toLowerCase()))

  if (!existing.has(stem)) {
    return stem
  }

  let index = 2
  while (existing.has(`${stem}-${index}`)) {
    index += 1
  }

  return `${stem}-${index}`
}

export function createApplication(exe = 'Game.exe', applications: RegisteredApplication[] = []): RegisteredApplication {
  const name = sanitizeProfileName(exe)

  return {
    id: uniqueApplicationId(name, applications),
    name,
    enabled: true,
    match: {
      exe,
    },
  }
}

export function defaultPivotXRDefaults(): PivotXRDefaults {
  return {
    activationMode: 'toggle',
    activationKey: 'F8',
    rotationMultiplier: 1.5,
    smoothing: 0.2,
    deadzoneDegrees: 8,
    maxExtraYawDegrees: 25,
    pitchRotationMultiplier: 1.0,
    pitchSmoothing: 0.2,
    pitchDeadzoneDegrees: 12,
    maxExtraPitchDegrees: 20,
  }
}

export function defaultConfig(): VectorXRConfig {
  return {
    version: 3,
    core: defaultCoreConfig(),
    applications: [],
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

export function createProfile(defaultSettings: DepthXRSettings, applicationIds: string[] = []): DepthXRProfileConfig {
  return {
    name: 'New Profile',
    enabled: true,
    applicationIds,
    settings: { ...defaultSettings },
  }
}

function isVectorXRConfig(value: unknown): value is VectorXRConfig {
  return isRecord(value) && value.version === 3 && 'core' in value && 'modules' in value
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
    activationKey: normalizePivotActivationKey(source.activationKey, fallback.activationKey),
    rotationMultiplier: normalizeNumber(source.rotationMultiplier, fallback.rotationMultiplier),
    smoothing: normalizeNumber(source.smoothing, fallback.smoothing),
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    maxExtraYawDegrees: normalizeNumber(source.maxExtraYawDegrees, fallback.maxExtraYawDegrees),
    pitchRotationMultiplier: normalizeNumber(source.pitchRotationMultiplier, fallback.pitchRotationMultiplier),
    pitchSmoothing: normalizeNumber(source.pitchSmoothing, fallback.pitchSmoothing),
    pitchDeadzoneDegrees: normalizeNumber(source.pitchDeadzoneDegrees, fallback.pitchDeadzoneDegrees),
    maxExtraPitchDegrees: normalizeNumber(source.maxExtraPitchDegrees, fallback.maxExtraPitchDegrees),
  }
}

function normalizePivotActivationKey(value: unknown, fallback: string): string {
  if (typeof value !== 'string') {
    return fallback
  }

  const trimmed = value.trim()
  if (/^[a-z]$/i.test(trimmed)) {
    return trimmed.toUpperCase()
  }

  if (/^\d$/.test(trimmed)) {
    return trimmed
  }

  if (/^f([1-9]|1[0-2])$/i.test(trimmed)) {
    return `F${trimmed.slice(1)}`
  }

  if (/^space$/i.test(trimmed)) {
    return 'Space'
  }

  return fallback
}

function normalizeApplication(value: unknown, fallbackId: string, existing: RegisteredApplication[]): RegisteredApplication {
  const source = isRecord(value) ? value : {}
  const match = isRecord(source.match) ? source.match : {}
  const exe = normalizeString(match.exe, 'Game.exe')
  const name = normalizeString(source.name, sanitizeProfileName(exe))
  const id = normalizeString(source.id, fallbackId).trim() || fallbackId

  return {
    id: uniqueApplicationId(id, existing),
    name,
    enabled: normalizeBoolean(source.enabled, true),
    match: {
      exe,
    },
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
  const applicationValues = Array.isArray(source.applications) ? source.applications : []
  const applications: RegisteredApplication[] = []

  applicationValues.forEach((applicationValue, index) => {
    applications.push(normalizeApplication(applicationValue, `application-${index + 1}`, applications))
  })

  return {
    version: 3,
    core: {
      enabled: normalizeBoolean(core.enabled, fallback.core.enabled),
      logLevel: normalizeLogLevel(core.logLevel),
      logRetentionFiles: normalizeNumber(core.logRetentionFiles, fallback.core.logRetentionFiles),
    },
    applications,
    modules: {
      depthxr: {
        enabled: normalizeBoolean(depthxr.enabled, fallback.modules.depthxr.enabled),
        defaults: normalizeDepthXRSettings(depthxr.defaults, fallback.modules.depthxr.defaults),
        profiles: profileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizeDepthXRSettings(profile.settings, fallback.modules.depthxr.defaults)
          const applicationIds = Array.isArray(profile.applicationIds)
            ? profile.applicationIds.filter((id): id is string => typeof id === 'string' && id.trim().length > 0)
            : []

          return {
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
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

export function normalizeConfig(config: unknown): VectorXRConfig {
  if (isVectorXRConfig(config)) {
    return normalizeVectorXRConfig(config)
  }

  return defaultConfig()
}

export function cloneConfig(config: VectorXRConfig): VectorXRConfig {
  return JSON.parse(JSON.stringify(config)) as VectorXRConfig
}
