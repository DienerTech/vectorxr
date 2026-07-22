<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'

import ModuleBindingPage from './ModuleBindingPage.vue'
import ModuleBindingPanel from './ModuleBindingPanel.vue'
import { savedBindingConflictWarnings, type TurboMetricsBucket, type TurboMetricsSession, type VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  turboMetrics: TurboMetricsSession[]
}>()

const emit = defineEmits<{
  close: []
  clearTurboMetrics: []
}>()

const metricsBindingOpen = ref(false)
const selectedSessionId = ref('')
const metricsBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  props.config.modules.turbo.metricsBinding,
]))

let savedScrollTop = 0

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openMetricsBinding() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  metricsBindingOpen.value = true
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeMetricsBinding() {
  metricsBindingOpen.value = false
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

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

const comparisonMinSeconds = 30

interface MetricsComparison {
  label: string
  offFps: number
  turboFps: number
  deltaPercent: number
}

const comparisons = computed<MetricsComparison[]>(() => {
  const buckets = selectedBuckets.value
  const off = buckets.find((bucket) => bucket.state === 'off')
  if (!off || off.seconds < comparisonMinSeconds || off.avgFps <= 0) {
    return []
  }
  return buckets
    .filter((bucket) => bucket.state !== 'off' && bucket.seconds >= comparisonMinSeconds && bucket.avgFps > 0)
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

function comparisonTone(value: number): string {
  if (value >= 1) {
    return 'chip-success'
  }
  if (value <= -1) {
    return 'chip-warning'
  }
  return 'surface-panel-strong'
}

function comparisonVerdict(value: number): string {
  if (value >= 1) {
    return 'Faster'
  }
  if (value <= -1) {
    return 'Slower'
  }
  return 'Within 1%'
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape' && !metricsBindingOpen.value) {
    emit('close')
  }
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <ModuleBindingPage
    v-if="metricsBindingOpen"
    module-label="Turbo Diagnostics"
    :binding="config.modules.turbo.metricsBinding"
    label="Metrics Capture Binding"
    description="Start and stop metric collection while in-game. Arm it once you are actually flying and pause it before loading screens or menus, so the recorded numbers reflect real play."
    none-text="No binding assigned. In capture-binding mode, no metrics are recorded until a binding is set."
    default-activate-sound="metrics-on.wav"
    default-deactivate-sound="metrics-off.wav"
    :warnings="metricsBindingWarnings"
    @update:binding="config.modules.turbo.metricsBinding = $event"
    @close="closeMetricsBinding"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">Turbo</button>
            <span aria-hidden="true">›</span>
            <span class="font-medium" style="color: var(--app-text)">Performance Diagnostics</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Performance Diagnostics</h2>
          <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
            Measure Turbo in the scenario that matters to you. VectorXR compares Turbo on and off only after both sides have enough in-game capture time.
          </p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          aria-label="Close diagnostics and return to Turbo"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <section class="mt-5 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div class="max-w-2xl">
            <p class="text-sm font-semibold tracking-tight">Capture behavior</p>
            <p class="mt-1 text-sm leading-6 text-muted">
              Capture only representative gameplay for the fairest result. Loading screens and menus can overwhelm the effect you are trying to measure.
            </p>
          </div>
          <label class="flex items-center gap-3 text-sm font-medium">
            Collection
            <select
              v-model="config.modules.turbo.metricsMode"
              class="rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
              style="border-color: var(--app-border)"
            >
              <option value="always">Always while in-game</option>
              <option value="binding">Use a capture binding</option>
              <option value="off">Off</option>
            </select>
          </label>
        </div>

        <p class="mt-3 text-xs leading-5 text-muted">
          <template v-if="config.modules.turbo.metricsMode === 'binding'">
            Recording starts and pauses with your capture binding. This is the recommended mode for controlled comparisons.
          </template>
          <template v-else-if="config.modules.turbo.metricsMode === 'always'">
            Recording runs whenever Turbo applies. Hitches over one second are excluded, but menus and loading screens still count.
          </template>
          <template v-else>
            Metrics collection is disabled. Existing results remain available below.
          </template>
        </p>

        <ModuleBindingPanel
          v-if="config.modules.turbo.metricsMode === 'binding'"
          class="mt-3"
          heading="Metrics Capture Binding"
          :binding="config.modules.turbo.metricsBinding"
          hint="Arm capture once you are actually flying; pause it before loading screens or menus."
          @edit="openMetricsBinding"
        />
      </section>

      <section class="mt-5 border-t pt-5" style="border-color: var(--app-border)">
        <div class="mb-4 flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Results</p>
            <p class="mt-1 text-sm text-muted">Choose a flight, read the verdict, then open the detailed measurements only if you need them.</p>
          </div>
          <button
            v-if="turboMetrics.length > 0"
            class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs"
            type="button"
            title="Delete all recorded metric sessions"
            @click="emit('clearTurboMetrics')"
          >
            Clear all results
          </button>
        </div>

        <div v-if="turboMetrics.length === 0" class="rounded-[0.9rem] border border-dashed px-5 py-6 text-center surface-panel-soft">
          <p class="text-sm font-medium">No performance checks yet</p>
          <p class="mt-1 text-sm text-muted">Run Turbo in a game and results will update here about every 15 seconds.</p>
        </div>

        <template v-else>
          <label class="mb-4 flex flex-wrap items-center gap-2 text-sm font-medium">
            Flight
            <select
              :value="selectedSession?.sessionId ?? ''"
              class="min-w-0 max-w-full rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
              style="border-color: var(--app-border)"
              @change="selectedSessionId = ($event.target as HTMLSelectElement).value"
            >
              <option v-for="session in turboMetrics" :key="session.sessionId" :value="session.sessionId">
                {{ formatSessionLabel(session) }}
              </option>
            </select>
          </label>

          <div v-if="comparisons.length > 0" class="grid gap-3 md:grid-cols-2">
            <article
              v-for="comparison in comparisons"
              :key="comparison.label"
              class="rounded-[1rem] border p-4"
              :class="comparisonTone(comparison.deltaPercent)"
              style="border-color: var(--app-border)"
            >
              <div class="flex flex-wrap items-center justify-between gap-2">
                <span class="text-xs font-semibold uppercase tracking-[0.18em]">{{ comparisonVerdict(comparison.deltaPercent) }}</span>
                <span class="rounded-full border px-2.5 py-1 text-xs" style="border-color: var(--app-border)">{{ comparison.label }}</span>
              </div>
              <p class="mt-3 text-3xl font-semibold tracking-tight">{{ formatDeltaPercent(comparison.deltaPercent) }}</p>
              <p class="mt-1 text-sm">Average fps versus Turbo off</p>
              <p class="mt-3 text-xs text-muted">{{ comparison.offFps.toFixed(1) }} → {{ comparison.turboFps.toFixed(1) }} fps in this flight</p>
            </article>
          </div>

          <div v-else class="rounded-[1rem] border px-4 py-4 surface-panel-strong">
            <p class="text-sm font-semibold">More comparable flight time needed</p>
            <p class="mt-1 text-sm leading-6 text-muted">
              Capture at least 30 seconds with Turbo off and 30 seconds with Turbo on in similar conditions. Use the Turbo toggle between passes.
            </p>
            <div v-if="selectedBuckets.length > 0" class="mt-3 flex flex-wrap gap-2">
              <span v-for="bucket in selectedBuckets" :key="bucket.state" class="rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
                {{ bucketLabel(bucket.state) }} · {{ formatSeconds(bucket.seconds) }}
              </span>
            </div>
          </div>

          <p v-if="comparisons.length > 0" class="mt-3 text-xs leading-5 text-muted">
            Use the percentage as evidence, not a promise: scene complexity, weather, traffic, and background work still affect the comparison.
          </p>

          <details class="section-disclosure mt-5 border-t pt-4" style="border-color: var(--app-border)">
            <summary class="flex items-center gap-2">
              <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
                <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
              </svg>
              <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Detailed Measurements</span>
              <span class="text-xs text-muted">Frame times, runtime wait, and stalls</span>
            </summary>

            <div v-if="selectedBuckets.length === 0" class="mt-3 rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
              This flight has no captured frames yet.
            </div>
            <div v-else class="mt-3 overflow-x-auto">
              <table class="w-full text-sm">
                <thead>
                  <tr class="text-left text-xs uppercase tracking-wide text-muted">
                    <th class="py-2 pr-3 font-medium">Pacing</th>
                    <th class="py-2 pr-3 font-medium">Time</th>
                    <th class="py-2 pr-3 font-medium">Avg fps</th>
                    <th class="py-2 pr-3 font-medium">1% low</th>
                    <th class="py-2 pr-3 font-medium">Avg frame</th>
                    <th class="py-2 pr-3 font-medium">p99 frame</th>
                    <th class="py-2 pr-3 font-medium" title="Average per-frame time the game spent blocked on runtime pacing">Wait blocked</th>
                    <th class="py-2 font-medium" title="Frame-pacing stalls counted by the safety circuit">Stalls</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="bucket in selectedBuckets" :key="bucket.state" class="border-t" style="border-color: var(--app-border)">
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
              {{ selectedSession.live ? 'Flight in progress — updates every ~15 seconds.' : 'Flight complete.' }}
              Collection: {{ selectedSession.collectionMode === 'binding' ? 'capture binding' : 'always' }}.
              <template v-if="selectedBuckets.some((bucket) => bucket.discardedFrames > 0)">Hitches over one second were excluded.</template>
            </p>
          </details>
        </template>
      </section>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">Done</button>
      </div>
    </article>
  </div>
</template>
