<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'

import { t } from '../i18n/index.ts'
import ModuleBindingPage from './ModuleBindingPage.vue'
import { preferredTurboSession, turboSessionStrategy } from '../lib/turboDiagnostics'
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
const metricsBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, props.config.modules.turbo.metricsBindings))

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
  return preferredTurboSession(props.turboMetrics, selectedSessionId.value)
})

function formatSessionLabel(session: TurboMetricsSession): string {
  const when = session.startedUnixSeconds
    ? new Date(session.startedUnixSeconds * 1000).toLocaleString([], {
        month: 'short',
        day: 'numeric',
        hour: '2-digit',
        minute: '2-digit',
      })
    : t('turbo.diagnostics.session.unknownTime')
  // Strategy and runtime name (layer data), separated like the rest of the label.
  const details = [turboSessionStrategy(session), session.runtimeName].filter(Boolean).join(' · ')
  const params = { when, app: session.appName || t('turbo.unknownApp'), details }
  return session.live ? t('turbo.diagnostics.session.labelLive', params) : t('turbo.diagnostics.session.label', params)
}

function bucketLabel(state: string): string {
  if (state === 'off') {
    return t('turbo.diagnostics.bucket.off')
  }
  if (state === 'async') {
    return t('turbo.diagnostics.bucket.async')
  }
  if (state === 'sequenced') {
    return t('turbo.diagnostics.bucket.sequenced')
  }
  return state
}

function formatSeconds(seconds: number): string {
  if (seconds >= 90) {
    const minutes = Math.floor(seconds / 60)
    const rest = Math.round(seconds % 60)
    return t('turbo.diagnostics.duration.minutes', { minutes, seconds: rest })
  }
  return t('turbo.diagnostics.duration.seconds', { seconds: seconds.toFixed(0) })
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
    return t('turbo.diagnostics.verdict.faster')
  }
  if (value <= -1) {
    return t('turbo.diagnostics.verdict.slower')
  }
  return t('turbo.diagnostics.verdict.within')
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
    :module-label="t('turbo.diagnostics.moduleLabel')"
    :bindings="config.modules.turbo.metricsBindings"
    :label="t('turbo.diagnostics.binding.label')"
    :description="t('turbo.diagnostics.binding.description')"
    :none-text="t('turbo.diagnostics.binding.noneText')"
    default-activate-sound="metrics-on.wav"
    default-deactivate-sound="metrics-off.wav"
    :warnings="metricsBindingWarnings"
    @update:bindings="config.modules.turbo.metricsBindings = $event"
    @close="closeMetricsBinding"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" :aria-label="t('turbo.breadcrumb')">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">{{ t('turbo.title') }}</button>
            <span aria-hidden="true">›</span>
            <span class="font-medium" style="color: var(--app-text)">{{ t('turbo.diagnostics.title') }}</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('turbo.diagnostics.title') }}</h2>
          <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
            {{ t('turbo.diagnostics.intro') }}
          </p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          :aria-label="t('turbo.diagnostics.closeAria')"
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
            <p class="text-sm font-semibold tracking-tight">{{ t('turbo.diagnostics.capture.title') }}</p>
            <p class="mt-1 text-sm leading-6 text-muted">
              {{ t('turbo.diagnostics.capture.intro') }}
            </p>
          </div>
          <label class="flex items-center gap-3 text-sm font-medium">
            {{ t('turbo.diagnostics.capture.collection') }}
            <select
              v-model="config.modules.turbo.metricsMode"
              class="rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
              style="border-color: var(--app-border)"
            >
              <option value="always">{{ t('turbo.diagnostics.capture.always') }}</option>
              <option value="binding">{{ t('turbo.diagnostics.capture.binding') }}</option>
              <option value="off">{{ t('turbo.diagnostics.capture.off') }}</option>
            </select>
          </label>
        </div>

        <p class="mt-3 text-xs leading-5 text-muted">
          <template v-if="config.modules.turbo.metricsMode === 'binding'">
            {{ t('turbo.diagnostics.capture.bindingNote') }}
          </template>
          <template v-else-if="config.modules.turbo.metricsMode === 'always'">
            {{ t('turbo.diagnostics.capture.alwaysNote') }}
          </template>
          <template v-else>
            {{ t('turbo.diagnostics.capture.offNote') }}
          </template>
        </p>

        <ModuleBindingPanel
          v-if="config.modules.turbo.metricsMode === 'binding'"
          class="mt-3"
          :heading="t('turbo.diagnostics.binding.label')"
          :bindings="config.modules.turbo.metricsBindings"
          :hint="t('turbo.diagnostics.binding.hint')"
          @edit="openMetricsBinding"
        />
      </section>

      <section class="mt-5 border-t pt-5" style="border-color: var(--app-border)">
        <div class="mb-4 flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.diagnostics.results.eyebrow') }}</p>
            <p class="mt-1 text-sm text-muted">{{ t('turbo.diagnostics.results.intro') }}</p>
          </div>
          <button
            v-if="turboMetrics.length > 0"
            class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs"
            type="button"
            :title="t('turbo.diagnostics.results.clearAllTitle')"
            @click="emit('clearTurboMetrics')"
          >
            {{ t('turbo.diagnostics.results.clearAll') }}
          </button>
        </div>

        <div v-if="turboMetrics.length === 0" class="rounded-[0.9rem] border border-dashed px-5 py-6 text-center surface-panel-soft">
          <p class="text-sm font-medium">{{ t('turbo.diagnostics.results.emptyTitle') }}</p>
          <p class="mt-1 text-sm text-muted">{{ t('turbo.diagnostics.results.emptyBody') }}</p>
        </div>

        <template v-else>
          <label class="mb-4 flex flex-wrap items-center gap-2 text-sm font-medium">
            {{ t('turbo.diagnostics.results.flight') }}
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

          <p v-if="selectedSession" class="mb-4 flex flex-wrap items-center gap-2 text-sm">
            <span class="text-muted">{{ t('turbo.diagnostics.results.strategy') }}</span>
            <span class="chip-accent rounded-full px-2.5 py-1 text-xs font-semibold" :title="selectedSession.timingConfiguration">{{ turboSessionStrategy(selectedSession) }}</span>
            <span v-if="selectedSession.timingConfiguration?.startsWith('Experimental')" class="text-xs text-muted">{{ selectedSession.timingConfiguration }}</span>
          </p>
          <p class="mb-4 text-xs text-muted">{{ t('turbo.diagnostics.results.accumulateNote') }}</p>
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
              <p class="mt-1 text-sm">{{ t('turbo.diagnostics.results.versusOff') }}</p>
              <p class="mt-3 text-xs text-muted">{{ t('turbo.diagnostics.results.fpsChange', { off: comparison.offFps.toFixed(1), turbo: comparison.turboFps.toFixed(1) }) }}</p>
            </article>
          </div>

          <div v-else class="rounded-[1rem] border px-4 py-4 surface-panel-strong">
            <p class="text-sm font-semibold">{{ t('turbo.diagnostics.results.needMoreTitle') }}</p>
            <p class="mt-1 text-sm leading-6 text-muted">
              {{ t('turbo.diagnostics.results.needMoreBody') }}
            </p>
            <div v-if="selectedBuckets.length > 0" class="mt-3 flex flex-wrap gap-2">
              <span v-for="bucket in selectedBuckets" :key="bucket.state" class="rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
                {{ bucketLabel(bucket.state) }} · {{ formatSeconds(bucket.seconds) }}
              </span>
            </div>
          </div>

          <p v-if="comparisons.length > 0" class="mt-3 text-xs leading-5 text-muted">
            {{ t('turbo.diagnostics.results.evidenceNote') }}
          </p>

          <details class="section-disclosure mt-5 border-t pt-4" style="border-color: var(--app-border)">
            <summary class="flex items-center gap-2">
              <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
                <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
              </svg>
              <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.diagnostics.detail.title') }}</span>
              <span class="text-xs text-muted">{{ t('turbo.diagnostics.detail.subtitle') }}</span>
            </summary>

            <div v-if="selectedBuckets.length === 0" class="mt-3 rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
              {{ t('turbo.diagnostics.detail.empty') }}
            </div>
            <div v-else class="mt-3 overflow-x-auto">
              <table class="w-full text-sm">
                <thead>
                  <tr class="text-left text-xs uppercase tracking-wide text-muted">
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.pacing') }}</th>
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.time') }}</th>
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.avgFps') }}</th>
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.onePercentLow') }}</th>
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.avgFrame') }}</th>
                    <th class="py-2 pr-3 font-medium">{{ t('turbo.diagnostics.detail.p99Frame') }}</th>
                    <th class="py-2 pr-3 font-medium" :title="t('turbo.diagnostics.detail.waitBlockedTitle')">{{ t('turbo.diagnostics.detail.waitBlocked') }}</th>
                    <th class="py-2 font-medium" :title="t('turbo.diagnostics.detail.stallsTitle')">{{ t('turbo.diagnostics.detail.stalls') }}</th>
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
              {{ selectedSession.live ? t('turbo.diagnostics.detail.live') : t('turbo.diagnostics.detail.complete') }}
              {{ selectedSession.collectionMode === 'binding' ? t('turbo.diagnostics.detail.collectionBinding') : t('turbo.diagnostics.detail.collectionAlways') }}
              <template v-if="selectedBuckets.some((bucket) => bucket.discardedFrames > 0)">{{ t('turbo.diagnostics.detail.hitchesExcluded') }}</template>
            </p>
          </details>
        </template>
      </section>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">{{ t('common.done') }}</button>
      </div>
    </article>
  </div>
</template>
