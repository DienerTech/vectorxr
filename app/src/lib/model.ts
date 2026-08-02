export type LogLevel = 'info' | 'debug'
export type ActivationMode = 'toggle' | 'hold' | 'alwaysOn'
export type PivotActivationBehavior = 'toggle' | 'hold'
export type PivotResponseMode = 'continuous' | 'stepped'
export type PivotStepGlideMode = 'instant' | 'glide'
export type PivotQuickViewTurnDirection = 'left' | 'right'
export type PivotProfileBehavior = 'enhancedMotion' | 'snapViews' | 'legacyHybrid'
export type QuadViewsTrackingMode = 'head' | 'eye'
export type AppTab = 'home' | 'core' | 'registry' | 'layers' | 'about' | 'depthxr' | 'pivotxr' | 'quadviews' | 'turbo'
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
    label: 'Numpad Keys',
    options: Array.from({ length: 10 }, (_, index) => `Numpad${index}`),
  },
  {
    label: 'Special Keys',
    options: ['Space'],
  },
] as const

export const keyboardModifierKeys = ['Ctrl', 'Alt', 'Shift'] as const

// Optional audible feedback played when a binding's action activates or
// deactivates. An empty sound path means "use the bundled default WAV".
export interface SoundFeedback {
  enabled: boolean
  activateSound: string
  deactivateSound: string
}

export interface KeyboardBinding {
  type: 'keyboard'
  chord: string[]
  sound?: SoundFeedback
}

export interface DeviceBinding {
  type: 'device'
  deviceGuid: string
  inputPath: string
  productGuid?: string
  deviceName?: string
  inputLabel?: string
  sound?: SoundFeedback
}

export interface NoneBinding {
  type: 'none'
}

export type InputBinding = NoneBinding | KeyboardBinding | DeviceBinding

// Replacing the physical input must not reset per-binding audio preferences.
export function preserveBindingSound<T extends InputBinding>(current: InputBinding, replacement: T): T {
  if (current.type === 'none' || replacement.type === 'none' || !current.sound) {
    return replacement
  }

  return { ...replacement, sound: { ...current.sound } } as T
}

export interface PivotActivationBinding {
  behavior: PivotActivationBehavior
  binding: InputBinding
}

export interface PivotNudgeSettings {
  yawStepDegrees: number
  pitchStepDegrees: number
  transitionSeconds: number
  yawLeftBindings: InputBinding[]
  yawRightBindings: InputBinding[]
  pitchUpBindings: InputBinding[]
  pitchDownBindings: InputBinding[]
  centerBindings: InputBinding[]
}

export interface PivotNudgeSet {
  id: string
  name: string
  settings: PivotNudgeSettings
}

export interface PivotQuickView {
  id: string
  name: string
  yawDegrees: number
  pitchDegrees: number
  positionRightCm: number
  positionUpCm: number
  positionForwardCm: number
  transitionSeconds: number
  turnDirection: PivotQuickViewTurnDirection
  activationBindings: PivotActivationBinding[]
}

export interface PivotViewControls {
  nudges: PivotNudgeSettings
  quickViews: PivotQuickView[]
}

// Global feedback-sound settings shared by every binding's activate/deactivate cue.
export interface SoundSettings {
  volume: number
}

export interface CoreConfig {
  enabled: boolean
  logLevel: LogLevel
  logRetentionFiles: number
  trackSeenApps: boolean
  sound: SoundSettings
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
  stereoBoost: number
  convergence: number
  depthAnchor: boolean
}

export interface DepthXRProfileConfig {
  name: string
  enabled: boolean
  applicationIds: string[]
  settings: DepthXRSettings
}

export interface DepthXRBindings {
  toggleEnabled: InputBinding
  toggleAnchor: InputBinding
}

export interface DepthXRModuleConfig {
  enabled: boolean
  defaults: DepthXRSettings
  bindings: DepthXRBindings
  profiles: DepthXRProfileConfig[]
}

// Per-direction tuning used when advanced axes are enabled. "Left"/"up" are
// the positive yaw/pitch rotation directions.
export interface PivotAxisTuning {
  rotationMultiplier: number
  deadzoneDegrees: number
  maxExtraDegrees: number
}
export interface PivotStepTuning {
  deadzoneDegrees: number
  triggerDegrees: number
  amountDegrees: number
  hysteresisDegrees: number
  maxExtraDegrees: number
}

export interface PivotXRSettings {
  smoothing: number
  activationRampSeconds: number
  rotationMultiplier: number
  deadzoneDegrees: number
  maxExtraYawDegrees: number
  pitchRotationMultiplier: number
  pitchDeadzoneDegrees: number
  maxExtraPitchDegrees: number
  responseMode: PivotResponseMode
  stepGlideMode: PivotStepGlideMode
  stepGlideSeconds: number
  yawStep: PivotStepTuning
  pitchStep: PivotStepTuning
  // Shared by Continuous and Stepped. Each mode keeps its own direction data.
  advancedAxes: boolean
  yawLeft: PivotAxisTuning
  yawRight: PivotAxisTuning
  pitchUp: PivotAxisTuning
  pitchDown: PivotAxisTuning
  yawLeftStep: PivotStepTuning
  yawRightStep: PivotStepTuning
  pitchUpStep: PivotStepTuning
  pitchDownStep: PivotStepTuning
}

export interface PivotXRProfileConfig {
  // Stable identifier: keeps list rendering and profile references intact when
  // profiles are reordered (array order is runtime priority order).
  id: string
  name: string
  enabled: boolean
  applicationIds: string[]
  behavior: PivotProfileBehavior
  nudgeSetId: string
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
  // Optional origin bindings: set-origin captures the current head yaw/pitch as
  // Pivot's neutral forward (bind it alongside the game's own recenter);
  // release-origin restores the default HMD origin.
  setOriginBindings: InputBinding[]
  releaseOriginBindings: InputBinding[]
  settings: PivotXRSettings
  viewControls: PivotViewControls
}

export interface PivotXRModuleConfig {
  enabled: boolean
  defaults: PivotXRSettings
  behavior: PivotProfileBehavior
  nudgeSetId: string
  nudgeSets: PivotNudgeSet[]
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
  setOriginBindings: InputBinding[]
  releaseOriginBindings: InputBinding[]
  viewControls: PivotViewControls
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
  diagnosticVisualizationBinding: InputBinding
  defaults: QuadViewsSettings
  profiles: QuadViewsProfileConfig[]
}

// Turbo mode: overrides runtime frame pacing. Binary per application —
// profiles carry no settings, only which applications they enable turbo for.
export interface TurboProfileConfig {
  id: string
  name: string
  enabled: boolean
  applicationIds: string[]
}

// How turbo sequences the real xrWaitFrame against the frame submit.
// 'async' overlaps the wait with the app's next-frame work; 'sequenced' joins
// it inside EndFrame right after the submit (safe on runtimes that interlock
// the wait with submission: Oculus, Varjo, PiOpenXR).
export type TurboPacingMode = 'async' | 'sequenced'
// 'auto' discovers the right mode per runtime and remembers the verdict in
// the layer-written runtime-pacing sidecar; forced modes disable discovery
// and runtime pins entirely.
export type TurboPacingSetting = 'auto' | TurboPacingMode

// When the layer captures frame-pacing metrics: 'always' whenever turbo
// applies to the running app, 'binding' only while the capture binding is
// armed (cuts loading screens/menus out of the data), 'off' never.
export type TurboMetricsMode = 'off' | 'always' | 'binding'

export interface TurboModuleConfig {
  enabled: boolean
  toggleBinding: InputBinding
  pacingMode: TurboPacingSetting
  // Per-runtime user overrides keyed by the exact OpenXR runtime name.
  // Only consulted when pacingMode is 'auto'.
  runtimePins: Record<string, TurboPacingMode>
  metricsMode: TurboMetricsMode
  metricsBinding: InputBinding
  profiles: TurboProfileConfig[]
}

// One row of the layer-written runtime-pacing.json sidecar: what Auto pacing
// learned about a runtime. Read-only facts; user intent lives in the config.
export interface RuntimePacingObservation {
  runtimeName: string
  runtimeVersion: string
  systemName: string
  vendorId: number
  graphicsApi: string
  mode: TurboPacingMode | 'unsupported'
  source: 'preset' | 'discovered'
  layerVersion: string
  firstUsedUnixSeconds: number
  lastUsedUnixSeconds: number
  probeTimeouts: number
  stableSeconds: number
}

// One state's aggregate within a layer-written turbo-metrics session:
// display-ready frame pacing stats for turbo off / async / sequenced.
export interface TurboMetricsBucket {
  state: 'off' | 'async' | 'sequenced' | string
  frames: number
  seconds: number
  avgFps: number
  avgFrameMs: number
  p99FrameMs: number
  maxFrameMs: number
  avgWaitBlockMs: number
  fabricatedWaits: number
  drainTimeouts: number
  discardedFrames: number
}

// One capture session from the layer-written turbo-metrics.json sidecar.
// Sessions arrive newest-first with a small retention cap.
export interface TurboMetricsSession {
  sessionId: string
  appName: string
  runtimeName: string
  layerVersion: string
  collectionMode: string
  live: boolean
  startedUnixSeconds: number
  updatedUnixSeconds: number
  buckets: TurboMetricsBucket[]
}

export interface VectorXRConfig {
  version: 3
  core: CoreConfig
  applications: RegisteredApplication[]
  modules: {
    depthxr: DepthXRModuleConfig
    pivotxr: PivotXRModuleConfig
    quadviews: QuadViewsModuleConfig
    turbo: TurboModuleConfig
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

function normalizeTurboPacingSetting(value: unknown): TurboPacingSetting {
  return value === 'async' || value === 'sequenced' ? value : 'auto'
}

function normalizeTurboMetricsMode(value: unknown): TurboMetricsMode {
  return value === 'off' || value === 'binding' ? value : 'always'
}

function normalizeTurboRuntimePins(value: unknown): Record<string, TurboPacingMode> {
  if (!isRecord(value)) {
    return {}
  }
  const pins: Record<string, TurboPacingMode> = {}
  for (const [runtimeName, pin] of Object.entries(value)) {
    if (runtimeName && (pin === 'async' || pin === 'sequenced')) {
      pins[runtimeName] = pin
    }
  }
  return pins
}

export function defaultCoreConfig(): CoreConfig {
  return {
    enabled: true,
    logLevel: 'info',
    logRetentionFiles: 7,
    trackSeenApps: true,
    sound: { volume: 100 },
  }
}

function normalizeVolume(value: unknown): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return 100
  }

  return Math.min(100, Math.max(0, Math.round(value)))
}

export function defaultDepthXRSettings(): DepthXRSettings {
  return {
    stereoBoost: 1.0,
    convergence: 0,
    depthAnchor: true,
  }
}

export function defaultDepthXRBindings(): DepthXRBindings {
  return {
    toggleEnabled: defaultNoneBinding(),
    toggleAnchor: defaultNoneBinding(),
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

export function defaultPivotAxisTuning(): PivotAxisTuning {
  return {
    rotationMultiplier: 1.5,
    deadzoneDegrees: 8,
    maxExtraDegrees: 120,
  }
}

export function defaultPivotStepTuning(): PivotStepTuning {
  return {
    deadzoneDegrees: 8,
    triggerDegrees: 10,
    amountDegrees: 10,
    hysteresisDegrees: 4,
    maxExtraDegrees: 120,
  }
}

export function defaultPivotXRSettings(): PivotXRSettings {
  return {
    smoothing: 0.2,
    activationRampSeconds: 0.35,
    rotationMultiplier: 1.5,
    deadzoneDegrees: 8,
    maxExtraYawDegrees: 120,
    pitchRotationMultiplier: 1.5,
    pitchDeadzoneDegrees: 8,
    maxExtraPitchDegrees: 120,
    responseMode: 'continuous',
    stepGlideMode: 'glide',
    stepGlideSeconds: 0.12,
    yawStep: defaultPivotStepTuning(),
    pitchStep: defaultPivotStepTuning(),
    advancedAxes: false,
    yawLeft: defaultPivotAxisTuning(),
    yawRight: defaultPivotAxisTuning(),
    pitchUp: defaultPivotAxisTuning(),
    pitchDown: defaultPivotAxisTuning(),
    yawLeftStep: defaultPivotStepTuning(),
    yawRightStep: defaultPivotStepTuning(),
    pitchUpStep: defaultPivotStepTuning(),
    pitchDownStep: defaultPivotStepTuning(),
  }
}

export function defaultPivotNudgeSettings(): PivotNudgeSettings {
  return {
    yawStepDegrees: 30,
    pitchStepDegrees: 20,
    transitionSeconds: 0.12,
    yawLeftBindings: [],
    yawRightBindings: [],
    pitchUpBindings: [],
    pitchDownBindings: [],
    centerBindings: [],
  }
}

export function newPivotNudgeSetId(): string {
  return `pivot-nudges-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`
}

export function createPivotNudgeSet(name = 'Standard Nudges', settings = defaultPivotNudgeSettings()): PivotNudgeSet {
  return {
    id: newPivotNudgeSetId(),
    name,
    settings: normalizePivotNudgeSettings(settings, defaultPivotNudgeSettings()),
  }
}

export function newPivotQuickViewId(): string {
  return `pivot-view-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`
}

export function createPivotQuickView(name = 'New Quick View'): PivotQuickView {
  return {
    id: newPivotQuickViewId(),
    name,
    yawDegrees: 0,
    pitchDegrees: 0,
    positionRightCm: 0,
    positionUpCm: 0,
    positionForwardCm: 0,
    transitionSeconds: 0.18,
    turnDirection: 'right',
    activationBindings: [],
  }
}

export function defaultPivotViewControls(): PivotViewControls {
  return {
    nudges: defaultPivotNudgeSettings(),
    quickViews: [],
  }
}

export function defaultQuadViewsSettings(): QuadViewsSettings {
  return {
    trackingMode: 'eye',
    focusHorizontalSizePercent: 40,
    focusVerticalSizePercent: 40,
    focusScale: 1.1,
    peripheralScale: 0.35,
    foveateSharpness: 50,
    transitionThicknessPercent: 25,
    horizontalOffsetDegrees: 0,
    verticalOffsetDegrees: 0,
    gazeSmoothing: 0.15,
    gazeDeadzoneDegrees: 1.5,
  }
}

export function defaultSoundFeedback(): SoundFeedback {
  return {
    enabled: false,
    activateSound: '',
    deactivateSound: '',
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
        enabled: false,
        defaults: defaultDepthXRSettings(),
        bindings: defaultDepthXRBindings(),
        profiles: [],
      },
      pivotxr: {
        enabled: false,
        defaults: defaultPivotXRSettings(),
        behavior: 'enhancedMotion',
        nudgeSetId: 'pivot-nudges-standard',
        nudgeSets: [{ id: 'pivot-nudges-standard', name: 'Standard Nudges', settings: defaultPivotNudgeSettings() }],
        alwaysActive: false,
        activationBindings: [],
        setOriginBindings: [],
        releaseOriginBindings: [],
        viewControls: defaultPivotViewControls(),
        profiles: [],
      },
      quadviews: {
        enabled: false,
        diagnosticVisualizationBinding: defaultNoneBinding(),
        defaults: defaultQuadViewsSettings(),
        profiles: [],
      },
      turbo: {
        enabled: false,
        toggleBinding: defaultNoneBinding(),
        pacingMode: 'auto',
        runtimePins: {},
        metricsMode: 'always',
        metricsBinding: defaultNoneBinding(),
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

export function newPivotProfileId(): string {
  return `pivot-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`
}

export function createPivotProfile(
  defaultSettings: PivotXRSettings,
  applicationIds: string[] = [],
  alwaysActive = false,
  activationBindings: PivotActivationBinding[] = [],
  defaultViewControls: PivotViewControls = defaultPivotViewControls(),
  behavior: PivotProfileBehavior = 'enhancedMotion',
  nudgeSetId = 'pivot-nudges-standard',
): PivotXRProfileConfig {
  return {
    id: newPivotProfileId(),
    name: 'New Profile',
    enabled: true,
    applicationIds,
    behavior,
    nudgeSetId,
    alwaysActive,
    activationBindings: normalizePivotActivationBindings(activationBindings),
    setOriginBindings: [],
    releaseOriginBindings: [],
    settings: normalizePivotXRSettings(defaultSettings, defaultPivotXRSettings()),
    viewControls: normalizePivotViewControls(defaultViewControls, defaultPivotViewControls()),
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

export function newTurboProfileId(): string {
  return `turbo-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`
}

export function createTurboProfile(applicationIds: string[] = []): TurboProfileConfig {
  return {
    id: newTurboProfileId(),
    name: 'New Profile',
    enabled: true,
    applicationIds,
  }
}

function isVectorXRConfig(value: unknown): value is VectorXRConfig {
  return isRecord(value) && value.version === 3 && 'core' in value && 'modules' in value
}

function normalizeDepthXRSettings(value: unknown, fallback: DepthXRSettings): DepthXRSettings {
  const source = isRecord(value) ? value : {}

  return {
    stereoBoost: normalizeNumber(source.stereoBoost, fallback.stereoBoost),
    convergence: normalizeNumber(source.convergence, fallback.convergence),
    // Depth Lock is on for new profiles, but configs written before 0.14.0 did
    // not have this field. Preserve their rendered image instead of silently
    // changing it during an upgrade.
    depthAnchor: Object.prototype.hasOwnProperty.call(source, 'depthAnchor')
      ? normalizeBoolean(source.depthAnchor, fallback.depthAnchor)
      : false,
  }
}

function normalizePivotAxisTuning(value: unknown, fallback: PivotAxisTuning): PivotAxisTuning {
  const source = isRecord(value) ? value : {}

  return {
    rotationMultiplier: normalizeNumber(source.rotationMultiplier, fallback.rotationMultiplier),
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    maxExtraDegrees: normalizeNumber(source.maxExtraDegrees, fallback.maxExtraDegrees),
  }
}

function normalizePivotStepTuning(value: unknown, fallback: PivotStepTuning): PivotStepTuning {
  const source = isRecord(value) ? value : {}
  return {
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    triggerDegrees: normalizeNumber(source.triggerDegrees, fallback.triggerDegrees),
    amountDegrees: normalizeNumber(source.amountDegrees, fallback.amountDegrees),
    hysteresisDegrees: normalizeNumber(source.hysteresisDegrees, fallback.hysteresisDegrees),
    maxExtraDegrees: normalizeNumber(source.maxExtraDegrees, fallback.maxExtraDegrees),
  }
}

function migratedStepGlideSeconds(smoothing: number): number {
  if (smoothing <= 0) return 0
  // Match the old 90 Hz exponential response through its 99.9% settle point.
  const perFrameError = Math.min(Math.max(smoothing, 0.000001), 0.95)
  const seconds = Math.log(0.001) / (90 * Math.log(perFrameError))
  const bounded = Math.min(2, Math.max(0.01, seconds))
  return Math.round(bounded * 100) / 100
}

function normalizePivotXRSettings(value: unknown, fallback: PivotXRSettings): PivotXRSettings {
  const source = isRecord(value) ? value : {}
  const smoothing = normalizeNumber(source.smoothing, fallback.smoothing)
  const legacyTrigger = normalizeNumber(source.stepTriggerDegrees, fallback.yawStep.triggerDegrees)
  const legacyAmount = normalizeNumber(source.stepAmountDegrees, fallback.yawStep.amountDegrees)
  const legacyHysteresis = normalizeNumber(source.stepHysteresisDegrees, fallback.yawStep.hysteresisDegrees)
  const yawStepFallback: PivotStepTuning = {
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    triggerDegrees: legacyTrigger,
    amountDegrees: legacyAmount,
    hysteresisDegrees: legacyHysteresis,
    maxExtraDegrees: normalizeNumber(source.maxExtraYawDegrees, fallback.maxExtraYawDegrees),
  }
  const pitchStepFallback: PivotStepTuning = {
    deadzoneDegrees: normalizeNumber(source.pitchDeadzoneDegrees, fallback.pitchDeadzoneDegrees),
    triggerDegrees: legacyTrigger,
    amountDegrees: legacyAmount,
    hysteresisDegrees: legacyHysteresis,
    maxExtraDegrees: normalizeNumber(source.maxExtraPitchDegrees, fallback.maxExtraPitchDegrees),
  }
  const yawStep = normalizePivotStepTuning(source.yawStep, yawStepFallback)
  const pitchStep = normalizePivotStepTuning(source.pitchStep, pitchStepFallback)
  const hasCanonicalGlide = source.stepGlideMode === 'instant' || source.stepGlideMode === 'glide'
  const stepGlideMode = hasCanonicalGlide
    ? source.stepGlideMode as PivotStepGlideMode
    : smoothing <= 0 ? 'instant' : 'glide'
  const rawStepGlideSeconds = source.stepGlideSeconds !== undefined
    ? normalizeNumber(source.stepGlideSeconds, fallback.stepGlideSeconds)
    : hasCanonicalGlide ? fallback.stepGlideSeconds : migratedStepGlideSeconds(smoothing)
  const stepGlideSeconds = Math.round(rawStepGlideSeconds * 100) / 100

  return {
    smoothing,
    activationRampSeconds: normalizeNumber(source.activationRampSeconds, fallback.activationRampSeconds),
    rotationMultiplier: normalizeNumber(source.rotationMultiplier, fallback.rotationMultiplier),
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    maxExtraYawDegrees: normalizeNumber(source.maxExtraYawDegrees, fallback.maxExtraYawDegrees),
    pitchRotationMultiplier: normalizeNumber(source.pitchRotationMultiplier, fallback.pitchRotationMultiplier),
    pitchDeadzoneDegrees: normalizeNumber(source.pitchDeadzoneDegrees, fallback.pitchDeadzoneDegrees),
    maxExtraPitchDegrees: normalizeNumber(source.maxExtraPitchDegrees, fallback.maxExtraPitchDegrees),
    responseMode: source.responseMode === 'continuous' || source.responseMode === 'stepped'
      ? source.responseMode
      : fallback.responseMode,
    stepGlideMode,
    stepGlideSeconds,
    yawStep,
    pitchStep,
    advancedAxes: normalizeBoolean(source.advancedAxes, fallback.advancedAxes),
    yawLeft: normalizePivotAxisTuning(source.yawLeft, fallback.yawLeft),
    yawRight: normalizePivotAxisTuning(source.yawRight, fallback.yawRight),
    pitchUp: normalizePivotAxisTuning(source.pitchUp, fallback.pitchUp),
    pitchDown: normalizePivotAxisTuning(source.pitchDown, fallback.pitchDown),
    yawLeftStep: normalizePivotStepTuning(source.yawLeftStep, yawStep),
    yawRightStep: normalizePivotStepTuning(source.yawRightStep, yawStep),
    pitchUpStep: normalizePivotStepTuning(source.pitchUpStep, pitchStep),
    pitchDownStep: normalizePivotStepTuning(source.pitchDownStep, pitchStep),
  }
}

function normalizePivotNudgeSettings(value: unknown, fallback: PivotNudgeSettings): PivotNudgeSettings {
  const source = isRecord(value) ? value : {}
  return {
    yawStepDegrees: normalizeNumber(source.yawStepDegrees, fallback.yawStepDegrees),
    pitchStepDegrees: normalizeNumber(source.pitchStepDegrees, fallback.pitchStepDegrees),
    transitionSeconds: normalizeNumber(source.transitionSeconds, fallback.transitionSeconds),
    yawLeftBindings: normalizeInputBindings(source.yawLeftBindings ?? fallback.yawLeftBindings),
    yawRightBindings: normalizeInputBindings(source.yawRightBindings ?? fallback.yawRightBindings),
    pitchUpBindings: normalizeInputBindings(source.pitchUpBindings ?? fallback.pitchUpBindings),
    pitchDownBindings: normalizeInputBindings(source.pitchDownBindings ?? fallback.pitchDownBindings),
    centerBindings: normalizeInputBindings(source.centerBindings ?? fallback.centerBindings),
  }
}

function normalizePivotQuickView(value: unknown, fallbackName: string): PivotQuickView {
  const source = isRecord(value) ? value : {}
  return {
    id: normalizeString(source.id, '').trim() || newPivotQuickViewId(),
    name: normalizeString(source.name, fallbackName),
    yawDegrees: normalizeNumber(source.yawDegrees, 0),
    pitchDegrees: normalizeNumber(source.pitchDegrees, 0),
    positionRightCm: normalizeNumber(source.positionRightCm, 0),
    positionUpCm: normalizeNumber(source.positionUpCm, 0),
    positionForwardCm: normalizeNumber(source.positionForwardCm, 0),
    transitionSeconds: normalizeNumber(source.transitionSeconds, 0.18),
    turnDirection: source.turnDirection === 'left' ? 'left' : 'right',
    activationBindings: normalizePivotActivationBindings(source.activationBindings),
  }
}

function normalizePivotViewControls(value: unknown, fallback: PivotViewControls): PivotViewControls {
  const source = isRecord(value) ? value : {}
  const quickViews = Array.isArray(source.quickViews) ? source.quickViews : fallback.quickViews
  return {
    nudges: normalizePivotNudgeSettings(source.nudges, fallback.nudges),
    quickViews: quickViews.map((quickView, index) => normalizePivotQuickView(quickView, `Quick View ${index + 1}`)),
  }
}

function normalizeActivationMode(value: unknown): ActivationMode {
  if (value === 'hold' || value === 'alwaysOn') {
    return value
  }
  return 'toggle'
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

  if (/^numpad[0-9]$/i.test(trimmed)) {
    return `Numpad${trimmed.slice(-1)}`
  }

  if (/^(ctrl|control)$/i.test(trimmed)) {
    return 'Ctrl'
  }

  if (/^alt$/i.test(trimmed)) {
    return 'Alt'
  }

  if (/^shift$/i.test(trimmed)) {
    return 'Shift'
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

function normalizeSoundFeedback(value: unknown): SoundFeedback | undefined {
  if (!isRecord(value)) {
    return undefined
  }

  return {
    enabled: normalizeBoolean(value.enabled, false),
    activateSound: normalizeString(value.activateSound, ''),
    deactivateSound: normalizeString(value.deactivateSound, ''),
  }
}

// Drops sound feedback that carries no signal, so configs that never use it stay clean.
function withOptionalSound<T extends KeyboardBinding | DeviceBinding>(binding: T, sound: SoundFeedback | undefined): T {
  if (!sound || (!sound.enabled && !sound.activateSound && !sound.deactivateSound)) {
    return binding
  }

  return { ...binding, sound }
}

export function normalizeInputBinding(value: unknown, fallback: InputBinding): InputBinding {
  const source = isRecord(value) ? value : {}
  if (source.type !== 'none' && source.type !== 'keyboard' && source.type !== 'device') {
    return normalizeInputBinding(fallback, defaultNoneBinding())
  }

  if (source.type === 'none') {
    return defaultNoneBinding()
  }

  const sound = normalizeSoundFeedback(source.sound)

  if (source.type === 'device') {
    return withOptionalSound({
      type: 'device',
      deviceGuid: normalizeString(source.deviceGuid, fallback.type === 'device' ? fallback.deviceGuid : ''),
      inputPath: normalizeString(source.inputPath, fallback.type === 'device' ? fallback.inputPath : 'button-1'),
      productGuid: normalizeString(source.productGuid, fallback.type === 'device' ? fallback.productGuid ?? '' : ''),
      deviceName: normalizeString(source.deviceName, fallback.type === 'device' ? fallback.deviceName ?? '' : ''),
      inputLabel: normalizeString(source.inputLabel, fallback.type === 'device' ? fallback.inputLabel ?? '' : ''),
    }, sound)
  }

  return withOptionalSound({
    type: 'keyboard',
    chord: normalizeKeyboardChord(source.chord, fallback.type === 'keyboard' ? fallback.chord : ['F8']),
  }, sound)
}

export function normalizeInputBindings(value: unknown, legacyValue?: unknown): InputBinding[] {
  const source = Array.isArray(value)
    ? value
    : value !== undefined
      ? [value]
      : legacyValue !== undefined
        ? [legacyValue]
        : []

  return source
    .map((binding) => normalizeInputBinding(binding, defaultNoneBinding()))
    .filter((binding) => binding.type !== 'none')
}

export function normalizePivotActivationBindings(
  value: unknown,
  legacyValue?: unknown,
  legacyMode: ActivationMode = 'toggle',
): PivotActivationBinding[] {
  const source = Array.isArray(value)
    ? value
    : value !== undefined
      ? [value]
      : legacyValue !== undefined
        ? [legacyValue]
        : []

  return source.flatMap((item) => {
    const wrapper = isRecord(item) && 'binding' in item
    const binding = normalizeInputBinding(wrapper ? item.binding : item, defaultNoneBinding())
    if (binding.type === 'none') return []
    const behavior: PivotActivationBehavior = wrapper && item.behavior === 'hold'
      ? 'hold'
      : wrapper && item.behavior === 'toggle'
        ? 'toggle'
        : legacyMode === 'hold' ? 'hold' : 'toggle'
    return [{ behavior, binding }]
  })
}

export function bindingsShareInput(left: InputBinding, right: InputBinding): boolean {
  if (left.type === 'none' || right.type === 'none' || left.type !== right.type) {
    return false
  }

  if (left.type === 'keyboard' && right.type === 'keyboard') {
    const leftChord = left.chord.map((key) => key.toLowerCase()).sort()
    const rightChord = right.chord.map((key) => key.toLowerCase()).sort()
    if (leftChord.length === 0 || rightChord.length === 0) {
      return false
    }

    return leftChord.length === rightChord.length && leftChord.every((key, index) => key === rightChord[index])
  }

  if (left.type === 'device' && right.type === 'device') {
    if (!left.deviceGuid.trim() || !right.deviceGuid.trim()) {
      return false
    }

    return left.deviceGuid.trim().toLowerCase() === right.deviceGuid.trim().toLowerCase()
      && left.inputPath.trim().toLowerCase() === right.inputPath.trim().toLowerCase()
  }

  return false
}

// Mirrors settings_resolver.cpp's activation arbitration exactly. Keep this
// separate from bindingsShareInput: physical chords are order-independent, but
// changing the runtime's serialized-order behavior would alter existing profile
// priority for ambiguous hand-authored configurations.
export function bindingsMatchRuntimeActivation(left: InputBinding, right: InputBinding): boolean {
  if (left.type === 'none' || right.type === 'none' || left.type !== right.type) {
    return false
  }

  if (left.type === 'keyboard' && right.type === 'keyboard') {
    return left.chord.length === right.chord.length
      && left.chord.every((key, index) => key === right.chord[index])
  }

  if (left.type === 'device' && right.type === 'device') {
    return left.deviceGuid === right.deviceGuid
      && left.inputPath === right.inputPath
  }

  return false
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

export function bindingListLabel(bindings: InputBinding[]): string {
  if (bindings.length === 0) return 'None'
  if (bindings.length <= 2) return bindings.map(bindingLabel).join(' or ')

  const remaining = bindings.length - 1
  return `${bindingLabel(bindings[0])} or ${remaining} other${remaining === 1 ? '' : 's'}`
}

export interface PivotBindingWarning {
  title: string
  message: string
}

interface SavedBindingAssignment {
  id: string
  label: string
  binding: InputBinding
}

function pushBindingAssignments(
  assignments: SavedBindingAssignment[],
  id: string,
  label: string,
  bindings: InputBinding[],
) {
  bindings.forEach((binding, index) => {
    assignments.push({
      id: `${id}.${index}`,
      label: bindings.length > 1 ? `${label} (binding ${index + 1})` : label,
      binding,
    })
  })
}


function savedBindingAssignments(config: VectorXRConfig): SavedBindingAssignment[] {
  const assignments: SavedBindingAssignment[] = [
    { id: 'depth.toggle', label: 'Depth: A/B toggle', binding: config.modules.depthxr.bindings.toggleEnabled },
    { id: 'depth.lock', label: 'Depth: Depth Lock A/B', binding: config.modules.depthxr.bindings.toggleAnchor },
    { id: 'quadviews.diagnostics', label: 'Quadviews: diagnostic visualization', binding: config.modules.quadviews.diagnosticVisualizationBinding },
    { id: 'turbo.toggle', label: 'Turbo: A/B toggle', binding: config.modules.turbo.toggleBinding },
    { id: 'turbo.metrics', label: 'Turbo: metrics capture', binding: config.modules.turbo.metricsBinding },
  ]

  const pivot = config.modules.pivotxr
  pivot.nudgeSets.forEach((set) => {
    const prefix = `pivot.nudge-set.${set.id}`
    const label = `Pivot ${set.name}`
    pushBindingAssignments(assignments, `${prefix}.left`, `${label}: Nudge Left`, set.settings.yawLeftBindings)
    pushBindingAssignments(assignments, `${prefix}.right`, `${label}: Nudge Right`, set.settings.yawRightBindings)
    pushBindingAssignments(assignments, `${prefix}.up`, `${label}: Nudge Up`, set.settings.pitchUpBindings)
    pushBindingAssignments(assignments, `${prefix}.down`, `${label}: Nudge Down`, set.settings.pitchDownBindings)
    pushBindingAssignments(assignments, `${prefix}.center`, `${label}: Center Nudge Offset`, set.settings.centerBindings)
  })
  pushBindingAssignments(assignments, 'pivot.default.activate', 'Pivot Default: Activate', pivot.activationBindings.map((item) => item.binding))
  pushBindingAssignments(assignments, 'pivot.default.set-origin', 'Pivot Default: Set Origin', pivot.setOriginBindings)
  pushBindingAssignments(assignments, 'pivot.default.release-origin', 'Pivot Default: Release Origin', pivot.releaseOriginBindings)

  pivot.viewControls.quickViews.forEach((quickView) => {
    pushBindingAssignments(assignments, `pivot.default.quick-view.${quickView.id}`, `Pivot Default: ${quickView.name}`, quickView.activationBindings.map((item) => item.binding))
  })

  pivot.profiles.forEach((profile, index) => {
    const context = profile.name.trim() || `Profile ${index + 1}`
    pushBindingAssignments(assignments, `pivot.${profile.id}.activate`, `Pivot ${context}: Activate`, profile.activationBindings.map((item) => item.binding))
    pushBindingAssignments(assignments, `pivot.${profile.id}.set-origin`, `Pivot ${context}: Set Origin`, profile.setOriginBindings)
    pushBindingAssignments(assignments, `pivot.${profile.id}.release-origin`, `Pivot ${context}: Release Origin`, profile.releaseOriginBindings)

    profile.viewControls.quickViews.forEach((quickView) => {
      pushBindingAssignments(assignments, `pivot.${profile.id}.quick-view.${quickView.id}`, `Pivot ${context}: ${quickView.name}`, quickView.activationBindings.map((item) => item.binding))
    })
  })

  return assignments
}

function bindingInputKey(binding: InputBinding): string | null {
  if (binding.type === 'none') return null
  if (binding.type === 'keyboard') {
    const chord = binding.chord.map((key) => key.trim().toLowerCase()).filter(Boolean).sort()
    return chord.length > 0 ? `keyboard:${chord.join('+')}` : null
  }
  const guid = binding.deviceGuid.trim().toLowerCase()
  const path = binding.inputPath.trim().toLowerCase()
  return guid && path ? `device:${guid}:${path}` : null
}

export function savedBindingConflictWarnings(
  config: VectorXRConfig,
  focusBindings: InputBinding[],
  options: { suppressFocusOnlyConflicts?: boolean } = {},
): PivotBindingWarning[] {
  const focusBindingSet = new Set(focusBindings)
  const focusKeys = new Set(focusBindings.map(bindingInputKey).filter((key): key is string => key !== null))
  if (focusKeys.size === 0) return []

  const groups = new Map<string, SavedBindingAssignment[]>()
  for (const assignment of savedBindingAssignments(config)) {
    const key = bindingInputKey(assignment.binding)
    if (!key) continue
    groups.set(key, [...(groups.get(key) ?? []), assignment])
  }

  return [...groups.entries()]
    .filter(([key, assignments]) => (
      focusKeys.has(key) &&
      assignments.length > 1 &&
      (!options.suppressFocusOnlyConflicts ||
        !assignments.every((assignment) => focusBindingSet.has(assignment.binding)))
    ))
    .map(([, assignments]) => ({
      title: `${bindingLabel(assignments[0].binding)} is assigned more than once`,
      message: `This input is assigned to ${assignments.map((assignment) => assignment.label).join('; ')}. A press may trigger multiple actions, or a higher-priority action may shadow another. This warning does not block saving.`,
    }))
}

function firstSharedBinding(left: InputBinding[], right: InputBinding[]): InputBinding | undefined {
  for (const binding of left) {
    const match = right.find((candidate) => bindingsShareInput(binding, candidate))
    if (match) return binding
  }
  return undefined
}

function duplicateBindingWarnings(actionLabel: string, bindings: InputBinding[]): PivotBindingWarning[] {
  const seen = new Set<string>()
  const warnings: PivotBindingWarning[] = []
  for (const binding of bindings) {
    const key = bindingInputKey(binding)
    if (!key) continue
    if (seen.has(key)) {
      warnings.push({
        title: `${bindingLabel(binding)} is duplicated`,
        message: `${actionLabel} contains the same physical input more than once. The duplicate is redundant and only the first copy is used at runtime.`,
      })
    } else {
      seen.add(key)
    }
  }
  return warnings
}

export function pivotBindingConflictWarnings(
  alwaysActive: boolean,
  activationBindings: PivotActivationBinding[],
  setOriginBindings: InputBinding[],
  releaseOriginBindings: InputBinding[],
): PivotBindingWarning[] {
  const physicalActivationBindings = activationBindings.map((item) => item.binding)
  const activationSetsOrigin = firstSharedBinding(physicalActivationBindings, setOriginBindings)
  const activationReleasesOrigin = firstSharedBinding(physicalActivationBindings, releaseOriginBindings)
  const setAlsoReleasesOrigin = firstSharedBinding(setOriginBindings, releaseOriginBindings)
  const action = alwaysActive ? 'Every suspend-control press' : 'Every activation-control press'

  if (activationSetsOrigin && activationReleasesOrigin
      && bindingsShareInput(activationSetsOrigin, activationReleasesOrigin)) {
    return [{
      title: 'One binding controls three conflicting actions',
      message: `Activation, Set Origin, and Release Origin all use ${bindingLabel(activationSetsOrigin)}. ${action} requests a new neutral forward and then immediately clears it, so the origin is not captured. Give Set Origin or Release Origin its own binding.`,
    }]
  }

  const warnings: PivotBindingWarning[] = [
    ...duplicateBindingWarnings(alwaysActive ? 'Suspend controls' : 'Activation', physicalActivationBindings),
    ...duplicateBindingWarnings('Set Origin', setOriginBindings),
    ...duplicateBindingWarnings('Release Origin', releaseOriginBindings),
  ]
  if (activationSetsOrigin) {
    warnings.push({
      title: 'Activation also sets the origin',
      message: `Activation and Set Origin both use ${bindingLabel(activationSetsOrigin)}. ${action} recaptures the current head direction as Pivot's neutral forward. Keep this only if it is intentional; otherwise give Set Origin its own binding, normally your in-game recenter control.`,
    })
  }

  if (activationReleasesOrigin) {
    warnings.push({
      title: 'Activation also releases the origin',
      message: `Activation and Release Origin both use ${bindingLabel(activationReleasesOrigin)}. ${action} clears any captured Pivot origin. Keep this only if it is intentional; otherwise give Release Origin its own binding.`,
    })
  }

  if (setAlsoReleasesOrigin) {
    warnings.push({
      title: 'Set Origin is immediately canceled',
      message: `Set Origin and Release Origin both use ${bindingLabel(setAlsoReleasesOrigin)}. A press requests a new neutral forward and then immediately clears it, so the origin is not captured. Give one action a different binding.`,
    })
  }

  return warnings
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
  const turbo = isRecord(modules.turbo) ? modules.turbo : {}
  const depthProfileValues = Array.isArray(depthxr.profiles) ? depthxr.profiles : []
  const pivotProfileValues = Array.isArray(pivotxr.profiles) ? pivotxr.profiles : []
  const quadViewsProfileValues = Array.isArray(quadviews.profiles) ? quadviews.profiles : []
  const turboProfileValues = Array.isArray(turbo.profiles) ? turbo.profiles : []
  const applicationValues = Array.isArray(source.applications) ? source.applications : []
  const applications: RegisteredApplication[] = []

  applicationValues.forEach((applicationValue, index) => {
    applications.push(normalizeApplication(applicationValue, `application-${index + 1}`, applications))
  })

  const pivotDefaults = normalizePivotXRSettings(pivotxr.defaults, fallback.modules.pivotxr.defaults)
  const pivotViewControls = normalizePivotViewControls(pivotxr.viewControls, fallback.modules.pivotxr.viewControls)
  const nudgeSetValues = Array.isArray(pivotxr.nudgeSets) ? pivotxr.nudgeSets : []
  const pivotNudgeSets: PivotNudgeSet[] = nudgeSetValues.map((value, index) => {
    const set = isRecord(value) ? value : {}
    return {
      id: normalizeString(set.id, `pivot-nudges-${index + 1}`),
      name: normalizeString(set.name, `Nudge Set ${index + 1}`),
      settings: normalizePivotNudgeSettings(set.settings, defaultPivotNudgeSettings()),
    }
  })
  if (pivotNudgeSets.length === 0) {
    pivotNudgeSets.push({ id: 'pivot-nudges-standard', name: 'Standard Nudges', settings: pivotViewControls.nudges })
  }
  const requestedDefaultNudgeSetId = normalizeString(pivotxr.nudgeSetId, '')
  const defaultNudgeSetId = pivotNudgeSets.some((set) => set.id === requestedDefaultNudgeSetId)
    ? requestedDefaultNudgeSetId
    : pivotNudgeSets[0].id
  const quadViewsDefaults = normalizeQuadViewsSettings(quadviews.defaults, fallback.modules.quadviews.defaults)

  return {
    version: 3,
    core: {
      enabled: normalizeBoolean(core.enabled, fallback.core.enabled),
      logLevel: normalizeLogLevel(core.logLevel),
      logRetentionFiles: normalizeNumber(core.logRetentionFiles, fallback.core.logRetentionFiles),
      trackSeenApps: normalizeBoolean(core.trackSeenApps, fallback.core.trackSeenApps),
      sound: { volume: normalizeVolume(isRecord(core.sound) ? core.sound.volume : undefined) },
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
          toggleAnchor: normalizeInputBinding(
            isRecord(depthxr.bindings) ? depthxr.bindings.toggleAnchor : undefined,
            fallback.modules.depthxr.bindings.toggleAnchor,
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
        behavior: pivotxr.behavior === 'snapViews' ? 'snapViews' : pivotxr.behavior === 'enhancedMotion' ? 'enhancedMotion' : pivotViewControls.quickViews.length > 0 ? 'legacyHybrid' : 'enhancedMotion',
        nudgeSetId: defaultNudgeSetId,
        nudgeSets: pivotNudgeSets,
        alwaysActive: normalizeBoolean(pivotxr.alwaysActive, normalizeActivationMode(pivotxr.activationMode) === 'alwaysOn'),
        activationBindings: normalizePivotActivationBindings(pivotxr.activationBindings, pivotxr.activationBinding, normalizeActivationMode(pivotxr.activationMode)),
        setOriginBindings: normalizeInputBindings(pivotxr.setOriginBindings, pivotxr.setOriginBinding),
        releaseOriginBindings: normalizeInputBindings(pivotxr.releaseOriginBindings, pivotxr.releaseOriginBinding),
        viewControls: pivotViewControls,
        profiles: pivotProfileValues.flatMap((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizePivotXRSettings(profile.settings, pivotDefaults)
          const applicationIds = applicationIdsFromProfile(profile, applications)
          const activationMode = normalizeActivationMode(profile.activationMode)
          const id = normalizeString(profile.id, '').trim() || newPivotProfileId()
          const viewControls = normalizePivotViewControls(profile.viewControls, pivotViewControls)
          let nudgeSetId = normalizeString(profile.nudgeSetId, '').trim()
          if (!pivotNudgeSets.some((set) => set.id === nudgeSetId)) {
            const sharesDefault = JSON.stringify(viewControls.nudges) === JSON.stringify(pivotViewControls.nudges)
            nudgeSetId = sharesDefault ? defaultNudgeSetId : `${id}-nudges`
            if (!sharesDefault) {
              pivotNudgeSets.push({ id: nudgeSetId, name: `${normalizeString(profile.name, 'Profile')} Nudges`, settings: viewControls.nudges })
            }
          }

          const normalized: PivotXRProfileConfig = {
            id,
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
            behavior: profile.behavior === 'snapViews' ? 'snapViews' : 'enhancedMotion',
            nudgeSetId,
            alwaysActive: normalizeBoolean(profile.alwaysActive, activationMode === 'alwaysOn'),
            activationBindings: normalizePivotActivationBindings(profile.activationBindings, profile.activationBinding, activationMode),
            setOriginBindings: normalizeInputBindings(profile.setOriginBindings, profile.setOriginBinding),
            releaseOriginBindings: normalizeInputBindings(profile.releaseOriginBindings, profile.releaseOriginBinding),
            settings,
            viewControls,
          }
          const hasExplicitBehavior = profile.behavior === 'enhancedMotion' || profile.behavior === 'snapViews'
          if (hasExplicitBehavior || viewControls.quickViews.length === 0) return [normalized]

          return [
            { ...normalized, viewControls: { ...viewControls, quickViews: [] } },
            {
              ...normalized,
              id: `${id}-snap-views`,
              name: `${normalized.name} Snap Views`,
              behavior: 'snapViews',
              alwaysActive: false,
              activationBindings: [],
              setOriginBindings: [],
              releaseOriginBindings: [],
            },
          ]
        }),
      },
      quadviews: {
        enabled: normalizeBoolean(quadviews.enabled, fallback.modules.quadviews.enabled),
        diagnosticVisualizationBinding: normalizeInputBinding(
          quadviews.diagnosticVisualizationBinding,
          fallback.modules.quadviews.diagnosticVisualizationBinding,
        ),
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
      turbo: {
        enabled: normalizeBoolean(turbo.enabled, fallback.modules.turbo.enabled),
        toggleBinding: normalizeInputBinding(turbo.toggleBinding, fallback.modules.turbo.toggleBinding),
        pacingMode: normalizeTurboPacingSetting(turbo.pacingMode),
        runtimePins: normalizeTurboRuntimePins(turbo.runtimePins),
        metricsMode: normalizeTurboMetricsMode(turbo.metricsMode),
        metricsBinding: normalizeInputBinding(turbo.metricsBinding, fallback.modules.turbo.metricsBinding),
        profiles: turboProfileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const applicationIds = applicationIdsFromProfile(profile, applications)
          const id = normalizeString(profile.id, '').trim() || newTurboProfileId()

          return {
            id,
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
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

export type ModuleId = 'depthxr' | 'pivotxr' | 'quadviews' | 'turbo'

export const moduleLabels: Record<ModuleId, string> = {
  depthxr: 'Depth',
  pivotxr: 'Pivot',
  quadviews: 'Quadviews',
  turbo: 'Turbo',
}

export interface ModuleApplicationState {
  kind: 'default-off' | 'default' | 'custom'
  profileName?: string
  profileIndex?: number
}

// Mirrors the layer's resolver: the first enabled profile targeting the app wins.
export function moduleStateForApplication(config: VectorXRConfig, moduleId: ModuleId, applicationId: string): ModuleApplicationState {
  const module = config.modules[moduleId]

  const profiles: Array<DepthXRProfileConfig | PivotXRProfileConfig | QuadViewsProfileConfig | TurboProfileConfig> = module.profiles
  for (const [index, profile] of profiles.entries()) {
    if (!profile.enabled || !profile.applicationIds.includes(applicationId)) {
      continue
    }

    return {
      kind: 'custom',
      profileName: profile.name,
      profileIndex: index,
    }
  }

  return module.enabled ? { kind: 'default' } : { kind: 'default-off' }
}
