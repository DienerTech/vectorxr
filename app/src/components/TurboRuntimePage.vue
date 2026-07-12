<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'

import type { ActiveRuntimeInfo } from '../lib/commands'
import type { RuntimePacingObservation, TurboPacingMode, VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  runtimePacing: RuntimePacingObservation[]
  activeRuntime: ActiveRuntimeInfo | null
}>()

const emit = defineEmits<{
  close: []
  rediscoverRuntime: [runtimeName: string]
}>()

const pacingForced = computed(() => props.config.modules.turbo.pacingMode !== 'auto')

const steamVrActive = computed(() => {
  const active = props.activeRuntime
  if (!active) {
    return false
  }
  const identity = `${active.name} ${active.manifestPath}`.toLowerCase()
  return identity.includes('steamvr') || identity.includes('steamxr')
})

interface PacingRow {
  runtimeName: string
  runtimeVersion: string
  systemName: string
  vendorId: number
  graphicsApi: string
  mode: RuntimePacingObservation['mode'] | null
  source: RuntimePacingObservation['source'] | null
  lastUsedUnixSeconds: number
  isActive: boolean
}

const genericTokens = new Set(['openxr', 'runtime', 'windows', 'program', 'files'])

function isSteamVrRuntime(runtimeName: string): boolean {
  const normalized = runtimeName.toLowerCase()
  return normalized.includes('steamvr') || normalized.includes('steamxr')
}

function matchesActiveRuntime(runtimeName: string): boolean {
  const active = props.activeRuntime
  if (!active) {
    return false
  }
  const name = runtimeName.toLowerCase()
  if (active.name) {
    const activeName = active.name.toLowerCase()
    if (activeName.includes(name) || name.includes(activeName)) {
      return true
    }
  }
  const normalizedPath = active.manifestPath.toLowerCase().replace(/[^a-z0-9]+/g, '')
  return runtimeName
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((token) => token.length >= 4 && !genericTokens.has(token))
    .some((token) => normalizedPath.includes(token))
}

const pacingRows = computed<PacingRow[]>(() => {
  const rows: PacingRow[] = props.runtimePacing.map((observation) => ({
    runtimeName: observation.runtimeName,
    runtimeVersion: observation.runtimeVersion,
    systemName: observation.systemName,
    vendorId: observation.vendorId,
    graphicsApi: observation.graphicsApi,
    mode: observation.mode,
    source: observation.source,
    lastUsedUnixSeconds: observation.lastUsedUnixSeconds,
    isActive: matchesActiveRuntime(observation.runtimeName),
  }))

  for (const runtimeName of Object.keys(props.config.modules.turbo.runtimePins)) {
    if (!rows.some((row) => row.runtimeName === runtimeName)) {
      rows.push({
        runtimeName,
        runtimeVersion: '',
        systemName: '',
        vendorId: 0,
        graphicsApi: '',
        mode: null,
        source: null,
        lastUsedUnixSeconds: 0,
        isActive: matchesActiveRuntime(runtimeName),
      })
    }
  }

  return rows.sort((lhs, rhs) => rhs.lastUsedUnixSeconds - lhs.lastUsedUnixSeconds)
})

const activeRuntimeLabel = computed(() => {
  const active = props.activeRuntime
  if (!active) {
    return ''
  }
  if (active.name) {
    return active.name
  }
  const file = active.manifestPath.split(/[\\/]/).pop() ?? ''
  return file.replace(/\.json$/i, '')
})

const activeRuntimeHasRow = computed(() => pacingRows.value.some((row) => row.isActive))

function pinValue(runtimeName: string): TurboPacingMode | 'auto' {
  return props.config.modules.turbo.runtimePins[runtimeName] ?? 'auto'
}

function setPin(runtimeName: string, value: string) {
  const pins = props.config.modules.turbo.runtimePins
  if (value === 'async' || value === 'sequenced') {
    pins[runtimeName] = value
  } else {
    delete pins[runtimeName]
  }
}

function rowModeLabel(row: PacingRow): string {
  const pinned = props.config.modules.turbo.runtimePins[row.runtimeName]
  if (pinned) {
    return pinned === 'async' ? 'Async' : 'Sequenced'
  }
  if (row.mode === 'async') {
    return 'Async'
  }
  if (row.mode === 'sequenced') {
    return 'Sequenced'
  }
  if (row.mode === 'unsupported') {
    return 'Not supported'
  }
  return 'Undiscovered'
}

function rowBadge(row: PacingRow): string {
  if (props.config.modules.turbo.runtimePins[row.runtimeName]) {
    return 'Pinned'
  }
  if (row.mode === 'unsupported') {
    return 'Suspended'
  }
  if (row.source === 'preset') {
    return 'Preset'
  }
  if (row.source === 'discovered') {
    return 'Discovered'
  }
  return ''
}

function formatDate(unixSeconds: number): string {
  return unixSeconds ? new Date(unixSeconds * 1000).toLocaleDateString() : '—'
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    emit('close')
  }
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">Turbo</button>
            <span aria-hidden="true">›</span>
            <span class="font-medium" style="color: var(--app-text)">Runtime Behavior</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Runtime Behavior</h2>
          <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
            Control how Turbo coordinates with each OpenXR runtime. Auto is the right choice for almost everyone, but it cannot make runtime reprojection compatible with pipelined timing.
          </p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          aria-label="Close runtime behavior and return to Turbo"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <div
        class="mt-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6"
        :class="steamVrActive ? 'chip-danger' : 'chip-warning'"
        style="border-color: var(--app-border)"
        role="note"
      >
        <template v-if="steamVrActive">
          <strong>SteamVR detected:</strong> Turbo prevents Motion Smoothing. DCS with synthesized Quadviews is hard-blocked from Turbo because SteamVR can reject its pipelined display times. A manual strategy cannot override that safety block.
        </template>
        <template v-else>
          <strong>Compatibility boundary:</strong> Auto chooses between supported pacing strategies and suspends repeated stalls, but Turbo may still conflict with runtime reprojection, frame synthesis, or a headset driver's presentation behavior.
        </template>
      </div>

      <section class="mt-5 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div class="max-w-2xl">
            <p class="text-sm font-semibold tracking-tight">Pacing strategy</p>
            <p class="mt-1 text-sm leading-6 text-muted">
              Auto tests the safe strategy, remembers the result for this runtime and headset, and adapts if the runtime cannot keep pace.
            </p>
          </div>
          <label class="flex items-center gap-3 text-sm font-medium">
            Mode
            <select
              v-model="config.modules.turbo.pacingMode"
              class="rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
              style="border-color: var(--app-border)"
            >
              <option value="auto">Auto (recommended)</option>
              <option value="async">Async — always</option>
              <option value="sequenced">Sequenced — always</option>
            </select>
          </label>
        </div>

        <div
          class="mt-4 rounded-[0.85rem] border px-4 py-3 text-sm leading-6"
          :class="pacingForced ? 'chip-warning' : 'chip-success'"
          style="border-color: var(--app-border)"
        >
          <template v-if="!pacingForced">
            <strong>Automatic protection is active.</strong> Async overlaps the runtime wait with game work; Sequenced supports runtimes that require wait and submission to remain interlocked.
          </template>
          <template v-else>
            <strong>Manual override is active.</strong> This strategy is forced wherever Turbo is permitted. Discovery and per-runtime decisions below are paused; hard compatibility blocks remain active, and repeated pacing stalls still suspend Turbo.
          </template>
        </div>
      </section>

      <section class="mt-5 border-t pt-5" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-end justify-between gap-2">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Runtime Decisions</p>
            <p class="mt-1 text-sm leading-6 text-muted">Review what Auto learned or override one runtime without affecting the rest.</p>
          </div>
          <span v-if="activeRuntimeLabel" class="rounded-full border px-3 py-1 text-xs text-muted" style="border-color: var(--app-border)">
            Active: <span class="font-medium" style="color: var(--app-text)">{{ activeRuntimeLabel }}</span>
          </span>
        </div>

        <div v-if="pacingRows.length === 0" class="rounded-[0.9rem] border border-dashed px-4 py-5 text-sm text-muted surface-panel-soft">
          Nothing to manage yet. Auto records a decision after Turbo's first session on a runtime.
        </div>

        <div v-else class="overflow-x-auto" :class="pacingForced ? 'pointer-events-none opacity-50' : ''">
          <table class="w-full text-sm">
            <thead>
              <tr class="text-left text-xs uppercase tracking-wide text-muted">
                <th class="py-2 pr-3 font-medium">Environment</th>
                <th class="py-2 pr-3 font-medium">Decision</th>
                <th class="py-2 pr-3 font-medium">Learned</th>
                <th class="py-2 pr-3 font-medium">Override</th>
                <th class="py-2 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              <tr
                v-for="row in pacingRows"
                :key="`${row.runtimeName}|${row.systemName}|${row.vendorId}|${row.graphicsApi}`"
                class="border-t"
                style="border-color: var(--app-border)"
              >
                <td class="py-3 pr-3">
                  <span :class="row.isActive ? 'font-semibold' : ''">{{ row.runtimeName }}</span>
                  <span v-if="row.runtimeVersion" class="ml-1 text-xs text-muted">{{ row.runtimeVersion }}</span>
                  <span v-if="row.isActive" class="ml-2 rounded-full border px-2 py-0.5 text-[0.65rem] uppercase tracking-wide" style="border-color: var(--app-border)">Active</span>
                  <span v-if="isSteamVrRuntime(row.runtimeName)" class="mt-1 block w-fit rounded-full px-2 py-0.5 text-[0.65rem] font-semibold uppercase tracking-wide chip-warning">No Motion Smoothing</span>
                  <span v-if="row.systemName || row.graphicsApi" class="mt-0.5 block text-xs text-muted">
                    {{ row.systemName || 'Unknown headset' }}<template v-if="row.graphicsApi"> · {{ row.graphicsApi }}</template>
                  </span>
                </td>
                <td class="py-3 pr-3">
                  <span class="font-medium">{{ rowModeLabel(row) }}</span>
                  <span v-if="rowBadge(row)" class="ml-2 rounded-full border px-2 py-0.5 text-xs" style="border-color: var(--app-border)">{{ rowBadge(row) }}</span>
                </td>
                <td class="py-3 pr-3 text-muted">{{ formatDate(row.lastUsedUnixSeconds) }}</td>
                <td class="py-3 pr-3">
                  <select
                    :value="pinValue(row.runtimeName)"
                    class="rounded-[0.5rem] border px-2 py-1 text-xs surface-panel-strong"
                    style="border-color: var(--app-border)"
                    @change="setPin(row.runtimeName, ($event.target as HTMLSelectElement).value)"
                  >
                    <option value="auto">Use Auto</option>
                    <option value="async">Pin Async</option>
                    <option value="sequenced">Pin Sequenced</option>
                  </select>
                </td>
                <td class="py-3 text-right">
                  <button
                    v-if="row.source"
                    class="button-secondary rounded-[0.5rem] px-2.5 py-1 text-xs"
                    type="button"
                    title="Forget this decision so Auto tests the runtime again"
                    @click="emit('rediscoverRuntime', row.runtimeName)"
                  >
                    Test again
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <p v-if="pacingForced && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          Runtime decisions are inactive while a global manual override is selected.
        </p>
        <p v-else-if="activeRuntimeLabel && !activeRuntimeHasRow && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          Auto will test {{ activeRuntimeLabel }} during the next Turbo session.
        </p>
      </section>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">Done</button>
      </div>
    </article>
  </div>
</template>
