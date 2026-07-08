<script setup lang="ts">
import { computed, ref } from 'vue'

import ModuleBindingPage from '../ModuleBindingPage.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import type { ActiveRuntimeInfo, OpenXrLayerSnapshot } from '../../lib/commands'
import type {
  RegisteredApplication,
  RuntimePacingObservation,
  TurboMetricsBucket,
  TurboMetricsSession,
  TurboPacingMode,
  VectorXRConfig,
} from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
  runtimePacing: RuntimePacingObservation[]
  activeRuntime: ActiveRuntimeInfo | null
  layerSnapshot: OpenXrLayerSnapshot | null
  turboMetrics: TurboMetricsSession[]
}>()

const emit = defineEmits<{
  addTurboProfile: []
  removeTurboProfile: [index: number]
  syncTurboProfileName: [index: number]
  rediscoverRuntime: [runtimeName: string]
  clearTurboMetrics: []
}>()

const howItWorksOpen = ref(false)
const bindingSubPageOpen = ref(false)
const metricsBindingSubPageOpen = ref(false)

const pacingForced = computed(() => props.config.modules.turbo.pacingMode !== 'auto')

// Turbo counts as "in use" when the default profile is on or any enabled
// custom profile exists — the advisory should fire for either.
const turboInUse = computed(
  () => props.config.modules.turbo.enabled || props.config.modules.turbo.profiles.some((profile) => profile.enabled),
)

// Two frame-pacing layers fight each other: warn when OpenXR Toolkit is
// enabled anywhere while Turbo applies. (We cannot see whether OXRTK's own
// turbo is switched on, so this stays an advisory, not an error.)
const toolkitConflict = computed(() => {
  if (!turboInUse.value) {
    return false
  }
  return (props.layerSnapshot?.slices ?? []).some((slice) =>
    slice.layers.some((layer) => layer.enabled && layer.layerName.toLowerCase().includes('mbucchia_toolkit')),
  )
})

interface PacingRow {
  runtimeName: string
  runtimeVersion: string
  mode: RuntimePacingObservation['mode'] | null
  source: RuntimePacingObservation['source'] | null
  lastUsedUnixSeconds: number
  isActive: boolean
}

// Generic tokens that appear in nearly every OpenXR manifest path and would
// make the active-runtime match meaningless.
const genericTokens = new Set(['openxr', 'runtime', 'windows', 'program', 'files'])

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
    mode: observation.mode,
    source: observation.source,
    lastUsedUnixSeconds: observation.lastUsedUnixSeconds,
    isActive: matchesActiveRuntime(observation.runtimeName),
  }))
  // Pins for runtimes without an observation still deserve a row.
  for (const runtimeName of Object.keys(props.config.modules.turbo.runtimePins)) {
    if (!rows.some((row) => row.runtimeName === runtimeName)) {
      rows.push({
        runtimeName,
        runtimeVersion: '',
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
  if (!unixSeconds) {
    return '—'
  }
  return new Date(unixSeconds * 1000).toLocaleDateString()
}

// --- Session metrics (layer-written turbo-metrics.json) ---

const selectedSessionId = ref('')

// Sessions arrive newest-first; fall back to the newest when nothing (or a
// since-evicted session) is selected.
const selectedSession = computed<TurboMetricsSession | null>(() => {
  if (props.turboMetrics.length === 0) {
    return null
  }
  return props.turboMetrics.find((session) => session.sessionId === selectedSessionId.value) ?? props.turboMetrics[0]
})

function formatSessionLabel(session: TurboMetricsSession): string {
  const when = session.startedUnixSeconds
    ? new Date(session.startedUnixSeconds * 1000).toLocaleString([], {
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
      })
    : 'Unknown time'
  const runtime = session.runtimeName ? ` · ${session.runtimeName}` : ''
  return `${when} — ${session.appName || 'unknown app'}${runtime}${session.live ? ' (live)' : ''}`
}

function bucketLabel(state: string): string {
  if (state === 'off') {
    return 'Turbo off'
  }
  if (state === 'async') {
    return 'Turbo — async'
  }
  if (state === 'sequenced') {
    return 'Turbo — sequenced'
  }
  return state
}

function formatSeconds(seconds: number): string {
  if (seconds >= 90) {
    const minutes = Math.floor(seconds / 60)
    const rest = Math.round(seconds % 60)
    return `${minutes}m ${rest}s`
  }
  return `${seconds.toFixed(0)}s`
}

function onePercentLowFps(bucket: TurboMetricsBucket): string {
  return bucket.p99FrameMs > 0 ? (1000 / bucket.p99FrameMs).toFixed(1) : '—'
}

const bucketOrder: Record<string, number> = { off: 0, async: 1, sequenced: 2 }

const selectedBuckets = computed<TurboMetricsBucket[]>(() => {
  const session = selectedSession.value
  if (!session) {
    return []
  }
  return [...session.buckets]
    .filter((bucket) => bucket.frames > 0)
    .sort((lhs, rhs) => (bucketOrder[lhs.state] ?? 9) - (bucketOrder[rhs.state] ?? 9))
})

// Honest comparison gate: both states need meaningful captured time in the
// same session before an improvement number means anything.
const kComparisonMinSeconds = 30

interface MetricsComparison {
  label: string
  offFps: number
  turboFps: number
  deltaPercent: number
}

const comparisons = computed<MetricsComparison[]>(() => {
  const buckets = selectedBuckets.value
  const off = buckets.find((bucket) => bucket.state === 'off')
  if (!off || off.seconds < kComparisonMinSeconds || off.avgFps <= 0) {
    return []
  }
  return buckets
    .filter((bucket) => bucket.state !== 'off' && bucket.seconds >= kComparisonMinSeconds && bucket.avgFps > 0)
    .map((bucket) => ({
      label: bucketLabel(bucket.state),
      offFps: off.avgFps,
      turboFps: bucket.avgFps,
      deltaPercent: ((bucket.avgFps - off.avgFps) / off.avgFps) * 100,
    }))
})

function formatDeltaPercent(value: number): string {
  return `${value >= 0 ? '+' : ''}${value.toFixed(1)}%`
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    module-label="Turbo"
    :binding="config.modules.turbo.toggleBinding"
    label="Turbo Toggle Binding"
    description="Flip Turbo on and off while in-game to compare fps and frame feel directly. Turbo starts enabled whenever it applies to the running application. Note: switching Turbo on mid-session can cause a brief hitch while the frame pipeline re-synchronizes."
    none-text="No binding assigned. Turbo stays on for applications it applies to."
    default-activate-sound="turbo-on.wav"
    default-deactivate-sound="turbo-off.wav"
    @update:binding="config.modules.turbo.toggleBinding = $event"
    @close="bindingSubPageOpen = false"
  />
  <ModuleBindingPage
    v-else-if="metricsBindingSubPageOpen"
    module-label="Turbo"
    :binding="config.modules.turbo.metricsBinding"
    label="Metrics Capture Binding"
    description="Start and stop frame-pacing metric collection while in-game. Arm it once you are actually flying and pause it before loading screens or menus, so the recorded numbers reflect real play."
    none-text="No binding assigned. In keybinding mode, no metrics are recorded until a binding is set."
    default-activate-sound="metrics-on.wav"
    default-deactivate-sound="metrics-off.wav"
    @update:binding="config.modules.turbo.metricsBinding = $event"
    @close="metricsBindingSubPageOpen = false"
  />
  <div v-else class="space-y-4">
    <!-- Turbo module: frame pacing first (it decides how Turbo behaves),
         then the toggle binding, then the default profile. -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Turbo</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Frees your game from the headset runtime's frame pacing so the GPU sets the pace. On some systems the runtime throttles frames at an unfortunate moment, capping fps well below what the hardware can deliver — Turbo removes that wait. If your fps is already stable, Turbo changes nothing worth having; it is a targeted fix, not a general boost.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          How Turbo Works
        </button>
      </div>

      <div
        v-if="toolkitConflict"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        OpenXR Toolkit is enabled alongside VectorXR Turbo. Two frame-pacing layers fight each other — if OXRTK's own Turbo Mode is switched on, disable it there or here, never both.
      </div>

      <!-- Frame pacing strategy -->
      <div class="border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3">
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Frame Pacing</span>
          <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
            How Turbo sequences the runtime's frame wait. Auto picks the right strategy per runtime and remembers what works — leave it on Auto unless you are debugging.
          </p>
        </div>

        <label class="flex flex-wrap items-center gap-3 text-sm font-medium">
        Pacing mode
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
      <p class="mt-2 text-xs leading-5 text-muted">
        <template v-if="!pacingForced">
          Async overlaps the runtime wait with the next frame's work; Sequenced performs it right after each submit for runtimes that interlock the wait with submission (Oculus, Varjo, Pimax). Auto probes async first and falls back automatically.
        </template>
        <template v-else>
          Forced mode: this strategy is used on every runtime. Per-runtime discovery and pins below are paused, and Turbo suspends itself (instead of adapting) if the runtime cannot keep pace.
        </template>
      </p>

      <div class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-2 flex flex-wrap items-center justify-between gap-2">
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Runtimes</span>
          <span v-if="activeRuntimeLabel" class="text-xs text-muted">
            Active runtime: <span class="font-medium">{{ activeRuntimeLabel }}</span>
          </span>
        </div>

        <div v-if="pacingRows.length === 0" class="rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
          No runtimes observed yet — discoveries appear after Turbo's first session on each runtime.
        </div>

        <div v-else class="overflow-x-auto" :class="pacingForced ? 'opacity-50 pointer-events-none' : ''">
          <table class="w-full text-sm">
            <thead>
              <tr class="text-left text-xs uppercase tracking-wide text-muted">
                <th class="py-2 pr-3 font-medium">Runtime</th>
                <th class="py-2 pr-3 font-medium">Mode</th>
                <th class="py-2 pr-3 font-medium">Source</th>
                <th class="py-2 pr-3 font-medium">Last used</th>
                <th class="py-2 pr-3 font-medium">Override</th>
                <th class="py-2 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              <tr
                v-for="row in pacingRows"
                :key="row.runtimeName"
                class="border-t"
                style="border-color: var(--app-border)"
              >
                <td class="py-2.5 pr-3">
                  <span :class="row.isActive ? 'font-semibold' : ''">{{ row.runtimeName }}</span>
                  <span v-if="row.runtimeVersion" class="ml-1 text-xs text-muted">{{ row.runtimeVersion }}</span>
                  <span
                    v-if="row.isActive"
                    class="ml-2 rounded-full border px-2 py-0.5 text-[0.65rem] uppercase tracking-wide"
                    style="border-color: var(--app-border)"
                  >Active</span>
                </td>
                <td class="py-2.5 pr-3">{{ rowModeLabel(row) }}</td>
                <td class="py-2.5 pr-3">
                  <span
                    v-if="rowBadge(row)"
                    class="rounded-full border px-2 py-0.5 text-xs"
                    style="border-color: var(--app-border)"
                  >{{ rowBadge(row) }}</span>
                </td>
                <td class="py-2.5 pr-3 text-muted">{{ formatDate(row.lastUsedUnixSeconds) }}</td>
                <td class="py-2.5 pr-3">
                  <select
                    :value="pinValue(row.runtimeName)"
                    class="rounded-[0.5rem] border px-2 py-1 text-xs surface-panel-strong"
                    style="border-color: var(--app-border)"
                    @change="setPin(row.runtimeName, ($event.target as HTMLSelectElement).value)"
                  >
                    <option value="auto">Auto</option>
                    <option value="async">Pin Async</option>
                    <option value="sequenced">Pin Sequenced</option>
                  </select>
                </td>
                <td class="py-2.5 text-right">
                  <button
                    v-if="row.source"
                    class="button-secondary rounded-[0.5rem] px-2.5 py-1 text-xs"
                    type="button"
                    title="Clear this runtime's recorded verdict so Auto probes it again at the next session"
                    @click="emit('rediscoverRuntime', row.runtimeName)"
                  >
                    Re-discover
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <p v-if="pacingForced && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          Forced mode in effect for all runtimes — pins and discovery are paused. Switch back to Auto to use them.
        </p>
        <p v-else-if="activeRuntimeLabel && !activeRuntimeHasRow && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          No verdict yet for {{ activeRuntimeLabel }} — Auto will discover it on the next Turbo session.
        </p>
        </div>
      </div>

      <!-- Module-level binding — applies regardless of which profile enabled turbo -->
      <div class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <ModuleBindingPanel
          heading="Turbo Toggle Binding"
          :binding="config.modules.turbo.toggleBinding"
          hint="Flip Turbo on and off while in-game to compare fps and frame feel directly. Turbo starts enabled whenever it applies to the running application."
          @edit="bindingSubPageOpen = true"
        />
      </div>

      <!-- Frame pacing metrics: measured effect of each pacing strategy -->
      <div class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
          <div>
            <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Metrics</span>
            <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
              Measures what Turbo actually does to your frame pacing: fps, frame times, and time spent blocked on runtime pacing, recorded separately for Turbo off, async, and sequenced. Flip the Turbo toggle mid-flight to collect both sides of the comparison.
            </p>
          </div>
        </div>

        <label class="flex flex-wrap items-center gap-3 text-sm font-medium">
          Collection
          <select
            v-model="config.modules.turbo.metricsMode"
            class="rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
            style="border-color: var(--app-border)"
          >
            <option value="always">Always while in-game</option>
            <option value="binding">Only while capture binding is armed</option>
            <option value="off">Off</option>
          </select>
        </label>
        <p class="mt-2 text-xs leading-5 text-muted">
          <template v-if="config.modules.turbo.metricsMode === 'binding'">
            Nothing is recorded until you press the capture binding in-game; press it again to pause. Use it to cut loading screens and menus out of the data.
          </template>
          <template v-else-if="config.modules.turbo.metricsMode === 'always'">
            Records whenever Turbo applies to the running application. Frame hitches over one second are excluded automatically, but menus and loading screens still count — use the capture binding mode for clean A/B comparisons.
          </template>
          <template v-else>
            No metrics are recorded.
          </template>
        </p>

        <div v-if="config.modules.turbo.metricsMode === 'binding'" class="mt-3">
          <ModuleBindingPanel
            heading="Metrics Capture Binding"
            :binding="config.modules.turbo.metricsBinding"
            hint="Arm capture once you are actually flying; pause it before loading screens or menus."
            @edit="metricsBindingSubPageOpen = true"
          />
        </div>

        <div class="mt-4">
          <div v-if="turboMetrics.length === 0" class="rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
            No metrics captured yet — they appear here after the first Turbo-enabled session (updated every ~15 seconds while playing).
          </div>

          <template v-else>
            <div class="mb-2 flex flex-wrap items-center justify-between gap-2">
              <label class="flex flex-wrap items-center gap-2 text-xs font-medium text-muted">
                Session
                <select
                  :value="selectedSession?.sessionId ?? ''"
                  class="rounded-[0.5rem] border px-2 py-1 text-xs surface-panel-strong"
                  style="border-color: var(--app-border)"
                  @change="selectedSessionId = ($event.target as HTMLSelectElement).value"
                >
                  <option v-for="session in turboMetrics" :key="session.sessionId" :value="session.sessionId">
                    {{ formatSessionLabel(session) }}
                  </option>
                </select>
              </label>
              <button
                class="button-secondary rounded-[0.5rem] px-2.5 py-1 text-xs"
                type="button"
                title="Delete all recorded metric sessions"
                @click="emit('clearTurboMetrics')"
              >
                Clear metrics
              </button>
            </div>

            <div
              v-for="comparison in comparisons"
              :key="comparison.label"
              class="mb-2 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong"
            >
              <span class="font-semibold">{{ formatDeltaPercent(comparison.deltaPercent) }} average fps</span>
              with {{ comparison.label }} vs Turbo off ({{ comparison.offFps.toFixed(1) }} → {{ comparison.turboFps.toFixed(1) }} fps).
              <span class="text-muted">Both states measured in this session; the comparison is only as fair as the scenes were similar.</span>
            </div>

            <div v-if="selectedBuckets.length === 0" class="rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
              This session has no captured frames yet.
            </div>
            <div v-else class="overflow-x-auto">
              <table class="w-full text-sm">
                <thead>
                  <tr class="text-left text-xs uppercase tracking-wide text-muted">
                    <th class="py-2 pr-3 font-medium">Pacing</th>
                    <th class="py-2 pr-3 font-medium">Time</th>
                    <th class="py-2 pr-3 font-medium">Avg fps</th>
                    <th class="py-2 pr-3 font-medium">1% low fps</th>
                    <th class="py-2 pr-3 font-medium">Avg frame</th>
                    <th class="py-2 pr-3 font-medium">p99 frame</th>
                    <th class="py-2 pr-3 font-medium" title="Average per-frame time the game spent blocked on runtime pacing">Wait blocked</th>
                    <th class="py-2 font-medium" title="Frame-pacing stalls counted by the safety circuit">Stalls</th>
                  </tr>
                </thead>
                <tbody>
                  <tr
                    v-for="bucket in selectedBuckets"
                    :key="bucket.state"
                    class="border-t"
                    style="border-color: var(--app-border)"
                  >
                    <td class="py-2.5 pr-3 font-medium">{{ bucketLabel(bucket.state) }}</td>
                    <td class="py-2.5 pr-3 text-muted">{{ formatSeconds(bucket.seconds) }}</td>
                    <td class="py-2.5 pr-3">{{ bucket.avgFps.toFixed(1) }}</td>
                    <td class="py-2.5 pr-3">{{ onePercentLowFps(bucket) }}</td>
                    <td class="py-2.5 pr-3">{{ bucket.avgFrameMs.toFixed(2) }} ms</td>
                    <td class="py-2.5 pr-3">{{ bucket.p99FrameMs.toFixed(2) }} ms</td>
                    <td class="py-2.5 pr-3">{{ bucket.avgWaitBlockMs.toFixed(2) }} ms</td>
                    <td class="py-2.5">{{ bucket.drainTimeouts }}</td>
                  </tr>
                </tbody>
              </table>
            </div>
            <p v-if="selectedSession" class="mt-2 text-xs text-muted">
              {{ selectedSession.live ? 'Session in progress — updates every ~15s.' : 'Session complete.' }}
              Collection mode: {{ selectedSession.collectionMode === 'binding' ? 'capture binding' : 'always' }}.
              <template v-if="selectedBuckets.some((bucket) => bucket.discardedFrames > 0)">
                Frame hitches over 1s are excluded from the stats.
              </template>
            </p>
          </template>
        </div>
      </div>

      <details class="section-disclosure mt-4 border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>

        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.turbo.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.turbo.enabled ? 'On' : 'Off' }}
        </label>

        <div v-if="!config.modules.turbo.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          The default profile is off — Turbo does not apply to applications without an enabled custom profile. Enabled custom profiles below still turn Turbo on for their assigned applications.
        </div>
      </details>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Enable Turbo for specific applications while leaving the default off.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addTurboProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.turbo.profiles"
        :key="profile.id"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Turbo"
        @remove="$emit('removeTurboProfile', index)"
        @sync-name="$emit('syncTurboProfileName', index)"
      >
        <div class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          Turbo is on for this profile's applications. There is nothing further to tune — pacing is either overridden or it isn't.
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.turbo.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to enable Turbo for a specific application.
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Turbo Mode</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">How Turbo Works</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Normally, an OpenXR game must wait for the headset runtime to say "start the next frame now." Some runtime and GPU combinations time that signal badly, forcing the game to idle and capping fps below what the hardware can do.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            With Turbo, VectorXR performs that wait out of the game's way and lets it begin its next frame immediately after submitting the previous one — one frame of pipelining. The headset runtime still receives a fully correct frame sequence; only the game's pacing is decoupled.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Runtimes disagree about where that wait may happen. <strong>Async</strong> pacing runs it in the background, overlapping it with the game's next frame — the most effective form, proven on SteamVR. <strong>Sequenced</strong> pacing performs it right after each frame submit, which is what runtimes like Oculus, Varjo, and Pimax require. <strong>Auto</strong> starts from what is known about your runtime, verifies it against live frame timing, adapts if needed, and remembers the verdict per runtime — so the right strategy is applied from the first frame of your next session.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Trade-offs: frame timing prediction becomes slightly less accurate, which can show up as marginally increased latency or judder in some titles. Turning Turbo on mid-session can occasionally cause a one-second hitch while the frame pipeline re-synchronizes.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Know the limit: on runtimes that require Sequenced pacing (Pimax, Oculus, Varjo), Turbo cannot raise a framerate the runtime enforces through its frame wait — a locked or half-rate cap stays locked. What Turbo reclaims there is the time your game spends blocked on pacing, which you can see directly in the Metrics section.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Turbo works alongside all of VectorXR's other enhancements, including the Varjo quadviews compatibility path — VectorXR sequences its frame handling so the two never conflict.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Safety net: if frame pacing stalls even after Auto has adapted, VectorXR suspends Turbo for the session (you'll hear the turbo-off cue) and records the runtime as unsupported so the stutter never replays on launch. Press the toggle binding to retry — a clean run overwrites the verdict.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Tip: assign the optional Turbo Toggle binding and flip it in-game while watching an fps counter. If Turbo does not clearly help a title, leave it off there.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
