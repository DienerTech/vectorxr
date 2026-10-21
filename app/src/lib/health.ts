import type { LogSnapshot, OpenXrLayerEntry, OpenXrLayerSnapshot, SeenApplication } from './commands'
import type { VectorXRConfig } from './model'
import { t } from '../i18n/index.ts'

export type HealthCheckState = 'pass' | 'warn' | 'fail' | 'info'
export type HealthOverallState = 'ready' | 'attention' | 'inactive' | 'unknown'

export interface HealthCheckItem {
  loading?: boolean
  id: string
  label: string
  state: HealthCheckState
  detail: string
}

export interface HealthSummary {
  loading: boolean
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

export function vectorXrLayers(snapshot: OpenXrLayerSnapshot | null): OpenXrLayerEntry[] {
  return snapshot?.slices.flatMap((slice) => slice.layers).filter((layer) => layer.isVectorXr) ?? []
}

// A dev build or an old install can leave more than one VectorXR registration.
// Report the one OpenXR apps will load: an enabled entry whose files resolve,
// then any enabled entry, and a disabled one only when nothing is enabled.
export function findVectorXrLayer(snapshot: OpenXrLayerSnapshot | null): OpenXrLayerEntry | null {
  const layers = vectorXrLayers(snapshot)
  return layers.find((layer) => layer.enabled && layer.manifestExists && layer.libraryExists)
    ?? layers.find((layer) => layer.enabled)
    ?? layers[0]
    ?? null
}

function mostRecentSeenApp(seenApps: SeenApplication[]): SeenApplication | null {
  return [...seenApps].sort((lhs, rhs) => rhs.lastSeenUnixSeconds - lhs.lastSeenUnixSeconds)[0] ?? null
}

export function buildHealthSummary(input: BuildHealthSummaryInput): HealthSummary {
  const vectorXrLayer = findVectorXrLayer(input.openXrLayerSnapshot)
  // A registration whose manifest is gone cannot load, so it is no duplicate.
  const enabledVectorXrLayers = vectorXrLayers(input.openXrLayerSnapshot).filter((layer) => layer.enabled && layer.manifestExists)
  const lastSeenApp = mostRecentSeenApp(input.seenApps)
  const checks: HealthCheckItem[] = [
    {
      id: 'suite-enabled',
      label: t('health.checks.suiteEnabled.label'),
      state: input.config.core.enabled ? 'pass' : 'warn',
      detail: input.config.core.enabled
        ? t('health.checks.suiteEnabled.enabled')
        : t('health.checks.suiteEnabled.disabled'),
    },
    {
      id: 'layer-registered',
      label: t('health.checks.layerRegistered.label'),
      state: input.openXrLayersLoading ? 'info' : vectorXrLayer ? 'pass' : 'fail',
      detail: input.openXrLayersLoading
        ? t('health.checks.layerRegistered.loading')
        : vectorXrLayer
          ? t('health.checks.layerRegistered.registered', { slice: vectorXrLayer.slice })
          : t('health.checks.layerRegistered.missing'),
    },
    {
      id: 'layer-enabled',
      label: t('health.checks.layerEnabled.label'),
      state: input.openXrLayersLoading ? 'info' : vectorXrLayer?.enabled ? 'pass' : 'fail',
      detail: input.openXrLayersLoading
        ? t('health.checks.layerEnabled.loading')
        : vectorXrLayer?.enabled
          ? t('health.checks.layerEnabled.enabled')
          : t('health.checks.layerEnabled.disabled'),
    },
    ...(enabledVectorXrLayers.length > 1 && !input.openXrLayersLoading
      ? [{
          id: 'layer-duplicates',
          label: t('health.checks.layerDuplicates.label'),
          state: 'fail' as const,
          detail: t('health.checks.layerDuplicates.detail', {
            count: enabledVectorXrLayers.length,
            paths: enabledVectorXrLayers.map((layer) => layer.manifestPath).join('; '),
          }),
        }]
      : []),
    {
      id: 'layer-files',
      label: t('health.checks.layerFiles.label'),
      state: !vectorXrLayer ? 'info' : vectorXrLayer.manifestExists && vectorXrLayer.libraryExists ? 'pass' : 'fail',
      detail: !vectorXrLayer
        ? t('health.checks.layerFiles.pending')
        : vectorXrLayer.manifestExists && vectorXrLayer.libraryExists
          ? t('health.checks.layerFiles.resolved')
          : t('health.checks.layerFiles.missing'),
    },
    {
      id: 'config-loaded',
      label: t('health.checks.configLoaded.label'),
      state: input.configPath ? 'pass' : 'info',
      detail: input.configPath || t('health.checks.configLoaded.pending'),
    },
    {
      id: 'recent-log',
      label: t('health.checks.recentLog.label'),
      state: input.logSnapshot?.files.length ? 'pass' : 'warn',
      detail: input.logSnapshot?.files.length
        ? t('health.checks.recentLog.available', { count: input.logSnapshot.files.length })
        : t('health.checks.recentLog.none'),
    },
    {
      id: 'seen-apps',
      label: t('health.checks.seenApps.label'),
      state: input.seenApps.length ? 'pass' : 'info',
      detail: input.seenApps.length
        ? t('health.checks.seenApps.observed', { count: input.seenApps.length })
        : t('health.checks.seenApps.none'),
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

  const label = t(`health.overall.${overall}`)
  const description = t(`health.description.${overall}`)

  return {
    loading: input.openXrLayersLoading,
    overall,
    label,
    description,
    checks: checks.map(check => ({ ...check, loading: input.openXrLayersLoading && check.id.startsWith('layer-') })),
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
