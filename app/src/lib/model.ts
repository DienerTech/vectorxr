export type LogLevel = 'info' | 'debug'
export type ActivationMode = 'toggle' | 'hold'
export type QuadViewsTrackingMode = 'head' | 'eye'
export type AppTab = 'core' | 'registry' | 'layers' | 'about' | 'depthxr' | 'pivotxr' | 'quadviews'
export const keyboardBindingKeyGroups = [
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

export const keyboardModifierKeys = ['Ctrl', 'Alt', 'Shift'] as const

export interface KeyboardBinding {
  type: 'keyboard'
  chord: string[]
}

export interface DeviceBinding {
  type: 'device'
  deviceGuid: string
  inputPath: string
  productGuid?: string
  deviceName?: string
  inputLabel?: string
}

export interface NoneBinding {
  type: 'none'
}

export type InputBinding = NoneBinding | KeyboardBinding | DeviceBinding

export interface CoreConfig {
  enabled: boolean
  logLevel: LogLevel
  logRetentionFiles: number
  trackSeenApps: boolean
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

export interface DepthXRBindings {
  toggleEnabled: InputBinding
}

export interface DepthXRModuleConfig {
  enabled: boolean
  defaults: DepthXRSettings
  bindings: DepthXRBindings
  profiles: DepthXRProfileConfig[]
}

export interface PivotXRSettings {
  rotationMultiplier: number
  smoothing: number
  deadzoneDegrees: number
  maxExtraYawDegrees: number
  pitchRotationMultiplier: number
  pitchSmoothing: number
  pitchDeadzoneDegrees: number
  maxExtraPitchDegrees: number
}

export interface PivotXRProfileConfig {
  name: string
  enabled: boolean
  applicationIds: string[]
  activationMode: ActivationMode
  activationBinding: InputBinding
  settings: PivotXRSettings
}

export interface PivotXRModuleConfig {
  enabled: boolean
  defaults: PivotXRSettings
  activationMode: ActivationMode
  activationBinding: InputBinding
  profiles: PivotXRProfileConfig[]
}

export interface QuadViewsSettings {
  trackingMode: QuadViewsTrackingMode
  focusHorizontalSizePercent: number
  focusVerticalSizePercent: number
  focusScale: number
  peripheralScale: number
  foveateSharpness: number
  transitionThicknessPercent: number
  horizontalOffsetDegrees: number
  verticalOffsetDegrees: number
  gazeSmoothing: number
  gazeDeadzoneDegrees: number
}

export interface QuadViewsProfileConfig {
  name: string
  enabled: boolean
  applicationIds: string[]
  settings: QuadViewsSettings
}

export interface QuadViewsModuleConfig {
  enabled: boolean
  defaults: QuadViewsSettings
  profiles: QuadViewsProfileConfig[]
}

export interface VectorXRConfig {
  version: 3
  core: CoreConfig
  applications: RegisteredApplication[]
  modules: {
    depthxr: DepthXRModuleConfig
    pivotxr: PivotXRModuleConfig
    quadviews: QuadViewsModuleConfig
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
  if (value === 'info' || value === 'debug') {
    return value
  }

  if (value === 'none' || value === 'off' || value === 'error') {
    return 'info'
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
    trackSeenApps: true,
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

export function defaultDepthXRBindings(): DepthXRBindings {
  return {
    toggleEnabled: defaultNoneBinding(),
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

export function defaultPivotXRSettings(): PivotXRSettings {
  return {
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

export function defaultQuadViewsSettings(): QuadViewsSettings {
  return {
    trackingMode: 'eye',
    focusHorizontalSizePercent: 32,
    focusVerticalSizePercent: 32,
    focusScale: 1.1,
    peripheralScale: 0.45,
    foveateSharpness: 0,
    transitionThicknessPercent: 25,
    horizontalOffsetDegrees: 0,
    verticalOffsetDegrees: 0,
    gazeSmoothing: 0.15,
    gazeDeadzoneDegrees: 1.5,
  }
}

export function defaultNoneBinding(): NoneBinding {
  return {
    type: 'none',
  }
}

export function defaultKeyboardBinding(primaryKey = 'F8'): KeyboardBinding {
  return {
    type: 'keyboard',
    chord: [primaryKey],
  }
}

export function defaultDeviceBinding(): DeviceBinding {
  return {
    type: 'device',
    deviceGuid: '',
    inputPath: 'button-1',
    productGuid: '',
    deviceName: '',
    inputLabel: 'Button 1',
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
        bindings: defaultDepthXRBindings(),
        profiles: [],
      },
      pivotxr: {
        enabled: false,
        defaults: defaultPivotXRSettings(),
        activationMode: 'toggle',
        activationBinding: defaultNoneBinding(),
        profiles: [],
      },
      quadviews: {
        enabled: false,
        defaults: defaultQuadViewsSettings(),
        profiles: [],
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

export function createPivotProfile(
  defaultSettings: PivotXRSettings,
  applicationIds: string[] = [],
  activationMode: ActivationMode = 'toggle',
  activationBinding: InputBinding = defaultNoneBinding(),
): PivotXRProfileConfig {
  return {
    name: 'New Profile',
    enabled: true,
    applicationIds,
    activationMode,
    activationBinding: normalizeInputBinding(activationBinding, defaultNoneBinding()),
    settings: { ...defaultSettings },
  }
}

export function createQuadViewsProfile(defaultSettings: QuadViewsSettings, applicationIds: string[] = []): QuadViewsProfileConfig {
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

function normalizePivotXRSettings(value: unknown, fallback: PivotXRSettings): PivotXRSettings {
  const source = isRecord(value) ? value : {}

  return {
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

function normalizeQuadViewsTrackingMode(value: unknown, fallback: QuadViewsTrackingMode): QuadViewsTrackingMode {
  return value === 'eye' || value === 'head' ? value : fallback
}

function normalizeQuadViewsSettings(value: unknown, fallback: QuadViewsSettings): QuadViewsSettings {
  const source = isRecord(value) ? value : {}

  return {
    trackingMode: normalizeQuadViewsTrackingMode(source.trackingMode, fallback.trackingMode),
    focusHorizontalSizePercent: normalizeNumber(source.focusHorizontalSizePercent, fallback.focusHorizontalSizePercent),
    focusVerticalSizePercent: normalizeNumber(source.focusVerticalSizePercent, fallback.focusVerticalSizePercent),
    focusScale: normalizeNumber(source.focusScale, fallback.focusScale),
    peripheralScale: normalizeNumber(source.peripheralScale, fallback.peripheralScale),
    foveateSharpness: normalizeNumber(source.foveateSharpness, fallback.foveateSharpness),
    transitionThicknessPercent: normalizeNumber(source.transitionThicknessPercent, fallback.transitionThicknessPercent),
    horizontalOffsetDegrees: normalizeNumber(source.horizontalOffsetDegrees, fallback.horizontalOffsetDegrees),
    verticalOffsetDegrees: normalizeNumber(source.verticalOffsetDegrees, fallback.verticalOffsetDegrees),
    gazeSmoothing: normalizeNumber(source.gazeSmoothing, fallback.gazeSmoothing),
    gazeDeadzoneDegrees: normalizeNumber(source.gazeDeadzoneDegrees, fallback.gazeDeadzoneDegrees),
  }
}

export function normalizeKeyboardKey(value: unknown, fallback: string): string {
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

function normalizeKeyboardChord(value: unknown, fallback: string[]): string[] {
  const source = Array.isArray(value) ? value : fallback
  const normalized: string[] = []

  for (const item of source) {
    const key = normalizeKeyboardKey(item, '')
    if (key && !normalized.includes(key)) {
      normalized.push(key)
    }
  }

  return normalized.length > 0 ? normalized : fallback
}

export function normalizeInputBinding(value: unknown, fallback: InputBinding): InputBinding {
  const source = isRecord(value) ? value : {}

  if (source.type === 'none') {
    return defaultNoneBinding()
  }

  if (source.type === 'device') {
    return {
      type: 'device',
      deviceGuid: normalizeString(source.deviceGuid, fallback.type === 'device' ? fallback.deviceGuid : ''),
      inputPath: normalizeString(source.inputPath, fallback.type === 'device' ? fallback.inputPath : 'button-1'),
      productGuid: normalizeString(source.productGuid, fallback.type === 'device' ? fallback.productGuid ?? '' : ''),
      deviceName: normalizeString(source.deviceName, fallback.type === 'device' ? fallback.deviceName ?? '' : ''),
      inputLabel: normalizeString(source.inputLabel, fallback.type === 'device' ? fallback.inputLabel ?? '' : ''),
    }
  }

  return {
    type: 'keyboard',
    chord: normalizeKeyboardChord(source.chord, fallback.type === 'keyboard' ? fallback.chord : ['F8']),
  }
}

export function bindingLabel(binding: InputBinding): string {
  if (binding.type === 'device') {
    const device = binding.deviceName?.trim() || binding.deviceGuid.trim() || 'Unassigned device'
    const input = binding.inputLabel?.trim() || binding.inputPath.trim() || 'unassigned input'
    return `${device} / ${input}`
  }

  if (binding.type === 'none') {
    return 'None'
  }

  return binding.chord.join('+')
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
    enabled: true,
    match: {
      exe,
    },
  }
}

function normalizeExeName(value: string): string {
  return value
    .trim()
    .split(/[/\\]/)
    .pop()
    ?.toLowerCase() ?? ''
}

function findApplicationIdByExe(applications: RegisteredApplication[], exe: string): string | null {
  const normalizedExe = normalizeExeName(exe)
  if (!normalizedExe) {
    return null
  }

  return applications.find((application) => normalizeExeName(application.match.exe) === normalizedExe)?.id ?? null
}

function createImportedApplication(exe: string, profileName: unknown, applications: RegisteredApplication[]): RegisteredApplication {
  const application = createApplication(exe, applications)
  const normalizedName = normalizeString(profileName, '').trim()
  if (normalizedName && normalizedName !== 'New Profile') {
    application.name = normalizedName
    application.id = uniqueApplicationId(normalizedName, applications)
  }
  return application
}

function applicationIdsFromProfile(profile: UnknownRecord, applications: RegisteredApplication[]): string[] {
  const applicationIds = Array.isArray(profile.applicationIds)
    ? profile.applicationIds.filter((id): id is string => typeof id === 'string' && id.trim().length > 0)
    : []

  if (applicationIds.length > 0) {
    return applicationIds
  }

  const match = isRecord(profile.match) ? profile.match : {}
  const exe = normalizeString(match.exe, '').trim()
  if (!exe) {
    return []
  }

  const existingId = findApplicationIdByExe(applications, exe)
  if (existingId) {
    return [existingId]
  }

  const application = createImportedApplication(exe, profile.name, applications)
  applications.push(application)
  return [application.id]
}

function normalizeVectorXRConfig(value: unknown): VectorXRConfig {
  const fallback = defaultConfig()
  const source = isRecord(value) ? value : {}
  const core = isRecord(source.core) ? source.core : {}
  const modules = isRecord(source.modules) ? source.modules : {}
  const depthxr = isRecord(modules.depthxr) ? modules.depthxr : {}
  const pivotxr = isRecord(modules.pivotxr) ? modules.pivotxr : {}
  const quadviews = isRecord(modules.quadviews) ? modules.quadviews : {}
  const depthProfileValues = Array.isArray(depthxr.profiles) ? depthxr.profiles : []
  const pivotProfileValues = Array.isArray(pivotxr.profiles) ? pivotxr.profiles : []
  const quadViewsProfileValues = Array.isArray(quadviews.profiles) ? quadviews.profiles : []
  const applicationValues = Array.isArray(source.applications) ? source.applications : []
  const applications: RegisteredApplication[] = []

  applicationValues.forEach((applicationValue, index) => {
    applications.push(normalizeApplication(applicationValue, `application-${index + 1}`, applications))
  })

  const pivotDefaults = normalizePivotXRSettings(pivotxr.defaults, fallback.modules.pivotxr.defaults)
  const quadViewsDefaults = normalizeQuadViewsSettings(quadviews.defaults, fallback.modules.quadviews.defaults)

  return {
    version: 3,
    core: {
      enabled: normalizeBoolean(core.enabled, fallback.core.enabled),
      logLevel: normalizeLogLevel(core.logLevel),
      logRetentionFiles: normalizeNumber(core.logRetentionFiles, fallback.core.logRetentionFiles),
      trackSeenApps: normalizeBoolean(core.trackSeenApps, fallback.core.trackSeenApps),
    },
    applications,
    modules: {
      depthxr: {
        enabled: normalizeBoolean(depthxr.enabled, fallback.modules.depthxr.enabled),
        defaults: normalizeDepthXRSettings(depthxr.defaults, fallback.modules.depthxr.defaults),
        bindings: {
          toggleEnabled: normalizeInputBinding(
            isRecord(depthxr.bindings) ? depthxr.bindings.toggleEnabled : undefined,
            fallback.modules.depthxr.bindings.toggleEnabled,
          ),
        },
        profiles: depthProfileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizeDepthXRSettings(profile.settings, fallback.modules.depthxr.defaults)
          const applicationIds = applicationIdsFromProfile(profile, applications)

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
        defaults: pivotDefaults,
        activationMode: pivotxr.activationMode === 'hold' ? 'hold' : 'toggle',
        activationBinding: normalizeInputBinding(pivotxr.activationBinding, fallback.modules.pivotxr.activationBinding),
        profiles: pivotProfileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizePivotXRSettings(profile.settings, pivotDefaults)
          const applicationIds = applicationIdsFromProfile(profile, applications)
          const activationMode = profile.activationMode === 'hold' ? 'hold' : 'toggle'

          return {
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
            activationMode,
            activationBinding: normalizeInputBinding(profile.activationBinding, defaultNoneBinding()),
            settings,
          }
        }),
      },
      quadviews: {
        enabled: normalizeBoolean(quadviews.enabled, fallback.modules.quadviews.enabled),
        defaults: quadViewsDefaults,
        profiles: quadViewsProfileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizeQuadViewsSettings(profile.settings, quadViewsDefaults)
          const applicationIds = applicationIdsFromProfile(profile, applications)

          return {
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
            settings,
          }
        }),
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
