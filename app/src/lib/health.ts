import type { LogSnapshot, OpenXrLayerEntry, OpenXrLayerSnapshot, SeenApplication } from './commands'
import type { VectorXRConfig } from './model'

export type HealthCheckState = 'pass' | 'warn' | 'fail' | 'info'
export type HealthOverallState = 'ready' | 'attention' | 'inactive' | 'unknown'

export interface HealthCheckItem {
  id: string
  label: string
  state: HealthCheckState
  detail: string
}

export interface HealthSummary {
  overall: HealthOverallState
  label: string
  description: string
  checks: HealthCheckItem[]
  vectorXrLayer: OpenXrLayerEntry | null
  lastSeenApp: SeenApplication | null
}

interface BuildHealthSummaryInput {
  config: VectorXRConfig
  configPath: string
  logSnapshot: LogSnapshot | null
  openXrLayerSnapshot: OpenXrLayerSnapshot | null
  openXrLayersLoading: boolean
  seenApps: SeenApplication[]
}

function findVectorXrLayer(snapshot: OpenXrLayerSnapshot | null): OpenXrLayerEntry | null {
  return snapshot?.slices.flatMap((slice) => slice.layers).find((layer) => layer.isVectorXr) ?? null
}

function mostRecentSeenApp(seenApps: SeenApplication[]): SeenApplication | null {
  return [...seenApps].sort((lhs, rhs) => rhs.lastSeenUnixSeconds - lhs.lastSeenUnixSeconds)[0] ?? null
}

export function buildHealthSummary(input: BuildHealthSummaryInput): HealthSummary {
  const vectorXrLayer = findVectorXrLayer(input.openXrLayerSnapshot)
  const lastSeenApp = mostRecentSeenApp(input.seenApps)
  const checks: HealthCheckItem[] = [
    {
      id: 'suite-enabled',
      label: 'Suite runtime switch',
      state: input.config.core.enabled ? 'pass' : 'warn',
      detail: input.config.core.enabled
        ? 'VectorXR runtime effects are allowed to run.'
        : 'VectorXR is disabled from the Home runtime switch.',
    },
    {
      id: 'layer-registered',
      label: 'OpenXR layer registration',
      state: input.openXrLayersLoading ? 'info' : vectorXrLayer ? 'pass' : 'fail',
      detail: input.openXrLayersLoading
        ? 'VectorXR is still reading OpenXR layer registrations.'
        : vectorXrLayer
          ? `Registered in ${vectorXrLayer.slice}.`
          : 'VectorXR was not found in the OpenXR implicit API layer registry.',
    },
    {
      id: 'layer-enabled',
      label: 'OpenXR layer enabled',
      state: input.openXrLayersLoading ? 'info' : vectorXrLayer?.enabled ? 'pass' : 'fail',
      detail: input.openXrLayersLoading
        ? 'Waiting for OpenXR layer status.'
        : vectorXrLayer?.enabled
          ? 'The VectorXR API layer is enabled for future OpenXR launches.'
          : 'The VectorXR API layer is disabled or unavailable.',
    },
    {
      id: 'layer-files',
      label: 'Layer files',
      state: !vectorXrLayer ? 'info' : vectorXrLayer.manifestExists && vectorXrLayer.libraryExists ? 'pass' : 'fail',
      detail: !vectorXrLayer
        ? 'Layer files can be checked after the layer registration is found.'
        : vectorXrLayer.manifestExists && vectorXrLayer.libraryExists
          ? 'Manifest and DLL paths both resolve.'
          : 'The registered manifest or DLL path is missing.',
    },
    {
      id: 'config-loaded',
      label: 'Config path',
      state: input.configPath ? 'pass' : 'info',
      detail: input.configPath || 'Config path will appear after settings load.',
    },
    {
      id: 'recent-log',
      label: 'Runtime logs',
      state: input.logSnapshot?.files.length ? 'pass' : 'warn',
      detail: input.logSnapshot?.files.length
        ? `${input.logSnapshot.files.length} recent log file${input.logSnapshot.files.length === 1 ? '' : 's'} available.`
        : 'No VectorXR runtime logs are available yet.',
    },
    {
      id: 'seen-apps',
      label: 'Application discovery',
      state: input.seenApps.length ? 'pass' : 'info',
      detail: input.seenApps.length
        ? `${input.seenApps.length} OpenXR app${input.seenApps.length === 1 ? '' : 's'} observed.`
        : 'No OpenXR applications have been observed yet.',
    },
  ]

  const hasFail = checks.some((check) => check.state === 'fail')
  const hasWarn = checks.some((check) => check.state === 'warn')
  const overall: HealthOverallState = hasFail
    ? 'inactive'
    : hasWarn
      ? 'attention'
      : input.openXrLayersLoading
        ? 'unknown'
        : 'ready'

  const label = overall === 'ready'
    ? 'Ready'
    : overall === 'attention'
      ? 'Needs attention'
      : overall === 'inactive'
        ? 'Not active'
        : 'Checking'

  const description = overall === 'ready'
    ? 'VectorXR looks ready for future OpenXR app launches.'
    : overall === 'attention'
      ? 'VectorXR can run, but one or more diagnostics may need review.'
      : overall === 'inactive'
        ? 'VectorXR is not currently ready to apply OpenXR Enhancements .'
        : 'VectorXR is still collecting system status.'

  return {
    overall,
    label,
    description,
    checks,
    vectorXrLayer,
    lastSeenApp,
  }
}

export function healthChipClass(overall: HealthOverallState): string {
  switch (overall) {
    case 'ready':
      return 'chip-success'
    case 'attention':
      return 'chip-warning'
    case 'inactive':
      return 'chip-danger'
    default:
      return 'chip-idle'
  }
}
