export type LogLevel = 'info' | 'debug'
export type ActivationMode = 'toggle' | 'hold' | 'alwaysOn'
export type PivotResponseMode = 'continuous' | 'stepped'
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
  compatibilityMode: boolean
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

// Per-direction tuning used when advanced axes are enabled. "Left"/"up" are
// the positive yaw/pitch rotation directions.
export interface PivotAxisTuning {
  rotationMultiplier: number
  deadzoneDegrees: number
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
  // Response shaping: continuous multiplier or discrete steps with hysteresis.
  responseMode: PivotResponseMode
  stepTriggerDegrees: number
  stepAmountDegrees: number
  stepHysteresisDegrees: number
  // When true, continuous mode tunes each direction independently.
  advancedAxes: boolean
  yawLeft: PivotAxisTuning
  yawRight: PivotAxisTuning
  pitchUp: PivotAxisTuning
  pitchDown: PivotAxisTuning
}

export interface PivotXRProfileConfig {
  // Stable identifier: keeps list rendering and profile references intact when
  // profiles are reordered (array order is runtime priority order).
  id: string
  name: string
  enabled: boolean
  applicationIds: string[]
  activationMode: ActivationMode
  activationBinding: InputBinding
  // Optional origin bindings: set-origin captures the current head yaw/pitch as
  // Pivot's neutral forward (bind it alongside the game's own recenter);
  // release-origin restores the default HMD origin.
  setOriginBinding: InputBinding
  releaseOriginBinding: InputBinding
  settings: PivotXRSettings
}

export interface PivotXRModuleConfig {
  enabled: boolean
  defaults: PivotXRSettings
  activationMode: ActivationMode
  activationBinding: InputBinding
  setOriginBinding: InputBinding
  releaseOriginBinding: InputBinding
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
    compatibilityMode: false,
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

export function defaultPivotAxisTuning(): PivotAxisTuning {
  return {
    rotationMultiplier: 1.5,
    deadzoneDegrees: 8,
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
    stepTriggerDegrees: 10,
    stepAmountDegrees: 10,
    stepHysteresisDegrees: 4,
    advancedAxes: false,
    yawLeft: defaultPivotAxisTuning(),
    yawRight: defaultPivotAxisTuning(),
    pitchUp: defaultPivotAxisTuning(),
    pitchDown: defaultPivotAxisTuning(),
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
        activationMode: 'toggle',
        activationBinding: defaultNoneBinding(),
        setOriginBinding: defaultNoneBinding(),
        releaseOriginBinding: defaultNoneBinding(),
        profiles: [],
      },
      quadviews: {
        enabled: false,
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
  activationMode: ActivationMode = 'toggle',
  activationBinding: InputBinding = defaultNoneBinding(),
): PivotXRProfileConfig {
  return {
    id: newPivotProfileId(),
    name: 'New Profile',
    enabled: true,
    applicationIds,
    activationMode,
    activationBinding: normalizeInputBinding(activationBinding, defaultNoneBinding()),
    setOriginBinding: defaultNoneBinding(),
    releaseOriginBinding: defaultNoneBinding(),
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
    compatibilityMode: normalizeBoolean(source.compatibilityMode, fallback.compatibilityMode),
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

function normalizePivotXRSettings(value: unknown, fallback: PivotXRSettings): PivotXRSettings {
  const source = isRecord(value) ? value : {}

  return {
    smoothing: normalizeNumber(source.smoothing, fallback.smoothing),
    activationRampSeconds: normalizeNumber(source.activationRampSeconds, fallback.activationRampSeconds),
    rotationMultiplier: normalizeNumber(source.rotationMultiplier, fallback.rotationMultiplier),
    deadzoneDegrees: normalizeNumber(source.deadzoneDegrees, fallback.deadzoneDegrees),
    maxExtraYawDegrees: normalizeNumber(source.maxExtraYawDegrees, fallback.maxExtraYawDegrees),
    pitchRotationMultiplier: normalizeNumber(source.pitchRotationMultiplier, fallback.pitchRotationMultiplier),
    pitchDeadzoneDegrees: normalizeNumber(source.pitchDeadzoneDegrees, fallback.pitchDeadzoneDegrees),
    maxExtraPitchDegrees: normalizeNumber(source.maxExtraPitchDegrees, fallback.maxExtraPitchDegrees),
    responseMode: source.responseMode === 'stepped' ? 'stepped' : fallback.responseMode,
    stepTriggerDegrees: normalizeNumber(source.stepTriggerDegrees, fallback.stepTriggerDegrees),
    stepAmountDegrees: normalizeNumber(source.stepAmountDegrees, fallback.stepAmountDegrees),
    stepHysteresisDegrees: normalizeNumber(source.stepHysteresisDegrees, fallback.stepHysteresisDegrees),
    advancedAxes: normalizeBoolean(source.advancedAxes, fallback.advancedAxes),
    yawLeft: normalizePivotAxisTuning(source.yawLeft, fallback.yawLeft),
    yawRight: normalizePivotAxisTuning(source.yawRight, fallback.yawRight),
    pitchUp: normalizePivotAxisTuning(source.pitchUp, fallback.pitchUp),
    pitchDown: normalizePivotAxisTuning(source.pitchDown, fallback.pitchDown),
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

export interface PivotBindingWarning {
  title: string
  message: string
}

export function pivotBindingConflictWarnings(
  mode: ActivationMode,
  activationBinding: InputBinding,
  setOriginBinding: InputBinding,
  releaseOriginBinding: InputBinding,
): PivotBindingWarning[] {
  const activationSetsOrigin = bindingsShareInput(activationBinding, setOriginBinding)
  const activationReleasesOrigin = bindingsShareInput(activationBinding, releaseOriginBinding)
  const setAlsoReleasesOrigin = bindingsShareInput(setOriginBinding, releaseOriginBinding)
  const action = mode === 'toggle' ? 'Every Toggle press, including deactivation,' : 'Every press'

  if (activationSetsOrigin && activationReleasesOrigin) {
    return [{
      title: 'One binding controls three conflicting actions',
      message: `Activation, Set Origin, and Release Origin all use ${bindingLabel(activationBinding)}. ${action} requests a new neutral forward and then immediately clears it, so the origin is not captured. Give Set Origin or Release Origin its own binding.`,
    }]
  }

  const warnings: PivotBindingWarning[] = []
  if (activationSetsOrigin) {
    warnings.push({
      title: 'Activation also sets the origin',
      message: `Activation and Set Origin both use ${bindingLabel(activationBinding)}. ${action} recaptures the current head direction as Pivot's neutral forward. Keep this only if it is intentional; otherwise give Set Origin its own binding, normally your in-game recenter control.`,
    })
  }

  if (activationReleasesOrigin) {
    warnings.push({
      title: 'Activation also releases the origin',
      message: `Activation and Release Origin both use ${bindingLabel(activationBinding)}. ${action} clears any captured Pivot origin. Keep this only if it is intentional; otherwise give Release Origin its own binding.`,
    })
  }

  if (setAlsoReleasesOrigin) {
    warnings.push({
      title: 'Set Origin is immediately canceled',
      message: `Set Origin and Release Origin both use ${bindingLabel(setOriginBinding)}. A press requests a new neutral forward and then immediately clears it, so the origin is not captured. Give one action a different binding.`,
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
        activationMode: normalizeActivationMode(pivotxr.activationMode),
        activationBinding: normalizeInputBinding(pivotxr.activationBinding, fallback.modules.pivotxr.activationBinding),
        setOriginBinding: normalizeInputBinding(pivotxr.setOriginBinding, defaultNoneBinding()),
        releaseOriginBinding: normalizeInputBinding(pivotxr.releaseOriginBinding, defaultNoneBinding()),
        profiles: pivotProfileValues.map((profileValue) => {
          const profile = isRecord(profileValue) ? profileValue : {}
          const settings = normalizePivotXRSettings(profile.settings, pivotDefaults)
          const applicationIds = applicationIdsFromProfile(profile, applications)
          const activationMode = normalizeActivationMode(profile.activationMode)
          const id = normalizeString(profile.id, '').trim() || newPivotProfileId()

          return {
            id,
            name: normalizeString(profile.name, 'New Profile'),
            enabled: normalizeBoolean(profile.enabled, true),
            applicationIds,
            activationMode,
            activationBinding: normalizeInputBinding(profile.activationBinding, defaultNoneBinding()),
            setOriginBinding: normalizeInputBinding(profile.setOriginBinding, defaultNoneBinding()),
            releaseOriginBinding: normalizeInputBinding(profile.releaseOriginBinding, defaultNoneBinding()),
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
