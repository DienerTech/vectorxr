<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'
import { clearTurboFaultLogs, clearTurboSafetyBlock, loadRuntimeStatus, type RuntimeStatusEnvelope } from '../../lib/commands'
import { errorText } from '../../lib/errors'
import { t } from '../../i18n/index.ts'
import I18nText from '../I18nText.vue'

import DefaultProfileExclusions from '../DefaultProfileExclusions.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import TurboProfileTiming from '../TurboProfileTiming.vue'
import { defaultTurboExperimental } from '../../lib/model'
import TurboDiagnosticsPage from '../TurboDiagnosticsPage.vue'
import TurboRuntimePage from '../TurboRuntimePage.vue'
import TurboSafetyPage from '../TurboSafetyPage.vue'
import TurboSafetyToggle from '../TurboSafetyToggle.vue'
import { turboSafetyBlocks } from '../../lib/turboSafety'
import { configWarnings } from '../../lib/validation'
import type { ActiveRuntimeInfo, OpenXrLayerSnapshot } from '../../lib/commands'
import {
  savedBindingConflictWarnings,
  type RegisteredApplication,
  type RuntimePacingObservation,
  type TurboMetricsSession,
  type VectorXRConfig,
} from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  savedConfig: VectorXRConfig
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
const runtimeStatus = ref<RuntimeStatusEnvelope>({ sessions: [], recovery: [], faults: [] })
const recoveryError = ref('')
const clearing = ref<string | null>(null)
const clearingLogs = ref(false)
let recoveryRevision = 0
const safetyBlocks = computed(() => turboSafetyBlocks(runtimeStatus.value.recovery))
async function clearSafety(fingerprint: string) {
  if (clearing.value !== null || clearingLogs.value) return
  ++recoveryRevision
  clearing.value = fingerprint
  try {
    await clearTurboSafetyBlock(fingerprint)
    runtimeStatus.value = await loadRuntimeStatus()
    recoveryError.value = ''
  } catch (error) {
    recoveryError.value = errorText(error, String(error))
  } finally { clearing.value = null }
}
async function clearFaultLogs() {
  if (clearing.value !== null || clearingLogs.value) return
  ++recoveryRevision
  clearingLogs.value = true
  try {
    await clearTurboFaultLogs()
    runtimeStatus.value = await loadRuntimeStatus()
    recoveryError.value = ''
  } catch (error) {
    recoveryError.value = errorText(error, String(error))
  } finally { clearingLogs.value = false }
}
let recoveryPoll: ReturnType<typeof setTimeout> | undefined
let disposed = false
async function refreshRecovery() {
  const revision = recoveryRevision
  try {
    if (clearing.value !== null || clearingLogs.value) return
    const status = await loadRuntimeStatus()
    if (revision === recoveryRevision) {
      runtimeStatus.value = status
      recoveryError.value = ''
    }
  } catch {
    if (revision === recoveryRevision) {
      runtimeStatus.value.sessions = []
      recoveryError.value = t('turbo.tab.recoveryUnavailable')
    }
  } finally {
    if (!disposed) recoveryPoll = setTimeout(refreshRecovery, 2000)
  }
}
onMounted(() => void refreshRecovery())
onUnmounted(() => { disposed = true; clearTimeout(recoveryPoll) })
const bindingSubPageOpen = ref(false)
const activeSubPage = ref<'runtime' | 'diagnostics' | 'safety' | null>(null)
let savedScrollTop = 0
const toggleBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, props.config.modules.turbo.toggleBindings))


const turboInUse = computed(
  () => props.config.modules.turbo.enabled || props.config.modules.turbo.profiles.some((profile) => profile.enabled),
)



// Frame Generation owns pacing in its sessions, so Turbo yields there.
const frameGenInUse = computed(
  () => props.config.modules.framegen.enabled || props.config.modules.framegen.profiles.some((profile) => profile.enabled),
)
const frameGenWarnings = computed(() => configWarnings(props.config))

const toolkitConflict = computed(() => {
  if (!turboInUse.value) {
    return false
  }
  return (props.layerSnapshot?.slices ?? []).some((slice) =>
    slice.layers.some((layer) => layer.enabled && layer.layerName.toLowerCase().includes('mbucchia_toolkit')),
  )
})

const pacingModeLabel = computed(() => {
  const mode = props.config.modules.turbo.pacingMode
  if (mode === 'async') {
    return t('turbo.pacingMode.forcedAsync')
  }
  if (mode === 'sequenced') {
    return t('turbo.pacingMode.forcedSequenced')
  }
  return t('turbo.pacingMode.auto')
})

const pacingSummary = computed(() => {
  if (props.config.modules.turbo.pacingMode !== 'auto') {
    return t('turbo.tab.pacingSummary.manual')
  }
  const environments = props.runtimePacing.length
  if (environments === 0) {
    return t('turbo.tab.pacingSummary.empty')
  }
  return t('turbo.tab.pacingSummary.observed', { count: environments })
})

const metricsModeLabel = computed(() => {
  const mode = props.config.modules.turbo.metricsMode
  if (mode === 'binding') {
    return t('turbo.tab.metricsMode.binding')
  }
  if (mode === 'off') {
    return t('turbo.tab.metricsMode.off')
  }
  return t('turbo.tab.metricsMode.always')
})

const metricsSummary = computed(() => {
  const count = props.turboMetrics.length
  if (count === 0) {
    return t('turbo.tab.metricsSummary.empty')
  }
  const latest = props.turboMetrics[0]
  const app = latest.appName || t('turbo.unknownApp')
  return latest.live ? t('turbo.tab.metricsSummary.recordedLive', { count, app }) : t('turbo.tab.metricsSummary.recorded', { count, app })
})

const tools = computed(() => [
  { page: 'runtime' as const, title: t('turbo.runtime.title'), chip: pacingModeLabel.value, summary: pacingSummary.value,
    icon: 'M3 5.5A2.5 2.5 0 0 1 5.5 3h9A2.5 2.5 0 0 1 17 5.5v4a2.5 2.5 0 0 1-2.5 2.5h-9A2.5 2.5 0 0 1 3 9.5v-4Zm2.5-.5a.5.5 0 0 0-.5.5v4a.5.5 0 0 0 .5.5h9a.5.5 0 0 0 .5-.5v-4a.5.5 0 0 0-.5-.5h-9ZM6 15a1 1 0 0 1 1-1h6a1 1 0 1 1 0 2H7a1 1 0 0 1-1-1Z' },
  { page: 'diagnostics' as const, title: t('turbo.diagnostics.title'), chip: metricsModeLabel.value, summary: metricsSummary.value,
    icon: 'M3 15a1 1 0 0 1 1-1h1.5V9.5a1 1 0 0 1 2 0V14H9V5a1 1 0 0 1 2 0v9h1.5V7.5a1 1 0 0 1 2 0V14H16a1 1 0 1 1 0 2H4a1 1 0 0 1-1-1Z' },
])

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openSubPage(page: 'runtime' | 'diagnostics' | 'safety') {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  activeSubPage.value = page
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function openToggleBinding() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  bindingSubPageOpen.value = true
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeSubPage() {
  bindingSubPageOpen.value = false
  activeSubPage.value = null
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    :module-label="t('turbo.title')"
    :bindings="config.modules.turbo.toggleBindings"
    :label="t('turbo.tab.toggleBinding.label')"
    :description="t('turbo.tab.toggleBinding.description')"
    :none-text="t('turbo.tab.toggleBinding.noneText')"
    default-activate-sound="turbo-on.wav"
    default-deactivate-sound="turbo-off.wav"
    :warnings="toggleBindingWarnings"
    @update:bindings="config.modules.turbo.toggleBindings = $event"
    @close="closeSubPage"
  />
  <TurboRuntimePage
    v-else-if="activeSubPage === 'runtime'"
    :config="config"
    :runtime-pacing="runtimePacing"
    :active-runtime="activeRuntime"
    @rediscover-runtime="emit('rediscoverRuntime', $event)"
    @close="closeSubPage"
  />
  <TurboSafetyPage
    v-else-if="activeSubPage === 'safety'"
    :config="config" :saved-config="savedConfig" :status="runtimeStatus" :error="recoveryError" :clearing="clearing"
    :clearing-logs="clearingLogs" @clear-logs="clearFaultLogs" @clear="clearSafety" @close="closeSubPage"
  />

  <TurboDiagnosticsPage
    v-else-if="activeSubPage === 'diagnostics'"
    :config="config"
    :turbo-metrics="turboMetrics"
    @clear-turbo-metrics="emit('clearTurboMetrics')"
    @close="closeSubPage"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-start justify-between gap-4">
        <div>
          <div class="flex flex-wrap items-center gap-2">
            <h2 class="text-2xl font-semibold tracking-tight">{{ t('turbo.title') }}</h2>
            <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">{{ t('turbo.tab.badge') }}</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            {{ t('turbo.tab.intro') }}
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">i</span>
          {{ t('turbo.tab.howItWorksButton') }}
        </button>
      </div>

      <div
        v-if="toolkitConflict"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        {{ t('turbo.tab.toolkitConflict') }}
      </div>

      <div
        v-for="warning in frameGenWarnings"
        :key="warning"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        {{ warning }}
      </div>
      <p
        v-if="frameGenInUse && frameGenWarnings.length === 0"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-soft"
        style="border-color: var(--app-border)"
        role="note"
      >
        {{ t('turbo.tab.frameGenNote') }}
      </p>

      <section class="border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3">
          <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.tab.essentials.eyebrow') }}</p>
          <p class="mt-1 text-sm text-muted">{{ t('turbo.tab.essentials.intro') }}</p>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight">{{ t('turbo.tab.essentials.defaultProfile') }}</p>
              <p class="mt-1 text-xs leading-5 text-muted">
                {{ config.modules.turbo.enabled
                  ? t('turbo.tab.essentials.defaultOnHint')
                  : t('turbo.tab.essentials.defaultOffHint') }}
              </p>
            </div>
            <div class="flex flex-wrap items-center justify-end gap-2">
              <DefaultProfileExclusions
                :applications="applications"
                :profiles="config.modules.turbo.profiles"
              />
              <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
                <input v-model="config.modules.turbo.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
                {{ config.modules.turbo.enabled ? t('turbo.tab.essentials.defaultOn') : t('turbo.tab.essentials.defaultOff') }}
              </label>
            </div>
          </div>
          <ModuleBindingPanel
            :heading="t('turbo.tab.essentials.toggleHeading')"
            :bindings="config.modules.turbo.toggleBindings"
            :hint="t('turbo.tab.essentials.toggleHint')"
            @edit="openToggleBinding"
          />
        </div>

        <div
          class="mt-3 flex flex-wrap items-center gap-3 rounded-[1rem] border px-4 py-3"
          :class="safetyBlocks.length ? 'chip-warning' : 'surface-panel-soft'"
          style="border-color: var(--app-border)"
        >
          <div class="min-w-0 flex-1">
            <p class="text-sm font-semibold tracking-tight">{{ t('turbo.safety.title') }}</p>
            <p class="text-xs leading-5" :class="safetyBlocks.length ? '' : 'text-muted'">
              {{ safetyBlocks.length
                ? t('turbo.tab.safety.heldOff', { count: safetyBlocks.length })
                : t('turbo.tab.safety.idle') }}
            </p>
          </div>
          <span class="rounded-full px-2.5 py-1 text-xs" :class="safetyBlocks.length ? 'surface-panel-strong' : 'chip-idle'" style="box-shadow: none">{{ t('turbo.tab.safety.blockCount', { count: safetyBlocks.length }) }} · {{ t('turbo.tab.safety.faultCount', { count: runtimeStatus.faults.length }) }}</span>
          <TurboSafetyToggle v-model="config.modules.turbo.interruptedSessionRecovery" />
          <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="openSubPage('safety')">{{ safetyBlocks.length ? t('turbo.tab.safety.review') : t('turbo.tab.safety.details') }}</button>
          <p v-if="recoveryError" class="w-full text-xs chip-danger" role="status">{{ recoveryError }}</p>
        </div>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3">
          <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.tab.tune.eyebrow') }}</p>
          <p class="mt-1 text-sm text-muted">{{ t('turbo.tab.tune.intro') }}</p>
        </div>

        <div class="grid gap-3 md:grid-cols-2">
          <button
            v-for="tool in tools"
            :key="tool.page"
            class="group flex items-center gap-3 rounded-[1rem] border p-3 text-left transition surface-panel-soft hover:shadow-panel"
            type="button"
            @click="openSubPage(tool.page)"
          >
            <span class="inline-flex h-9 w-9 shrink-0 items-center justify-center rounded-[0.7rem] border surface-panel-strong" style="border-color: var(--app-border); box-shadow: none">
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor"><path :d="tool.icon" /></svg>
            </span>
            <span class="min-w-0 flex-1">
              <span class="flex flex-wrap items-center gap-2">
                <span class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ tool.title }}</span>
                <span class="rounded-full border px-2 py-0.5 text-[11px] font-medium" style="border-color: var(--app-border)">{{ tool.chip }}</span>
              </span>
              <span class="mt-0.5 block text-xs leading-5 text-muted">{{ tool.summary }}</span>
            </span>
            <span aria-hidden="true" class="text-muted transition group-hover:translate-x-1">→</span>
          </button>
        </div>

        <details class="section-disclosure mt-3 rounded-[1rem] border px-4 py-3 surface-panel-soft" style="border-color: var(--app-border)">
          <summary class="flex items-center gap-2 text-sm font-medium" style="color: var(--app-text)">
            <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
            {{ t('turbo.tab.tune.guideSummary') }}
          </summary>
          <ol class="mt-3 list-decimal space-y-1.5 pl-5 text-sm leading-6 text-muted">
            <li>{{ t('turbo.tab.tune.guideStep1') }}</li>
            <li>{{ t('turbo.tab.tune.guideStep2') }}</li>
            <li>{{ t('turbo.tab.tune.guideStep3') }}</li>
            <li>{{ t('turbo.tab.tune.guideStep4') }}</li>
            <li>{{ t('turbo.tab.tune.guideStep5') }}</li>
          </ol>
        </details>
      </section>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">{{ t('turbo.tab.profiles.title') }}</h2>
          <p class="text-sm text-muted">{{ t('turbo.tab.profiles.intro') }}</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addTurboProfile')"
        >
          {{ t('turbo.tab.profiles.add') }}
        </button>
      </div>

      <I18nText v-if="config.modules.turbo.enabled" keypath="turbo.tab.profiles.broad" tag="div" class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
        <template #strong><strong>{{ t('turbo.tab.profiles.broadStrong') }}</strong></template>
      </I18nText>

      <ProfileShell
        v-for="(profile, index) in config.modules.turbo.profiles"
        :key="profile.id"
        :index="index"
        :profile="profile"
        :applications="applications"
        :module-label="t('turbo.title')"
        @remove="$emit('removeTurboProfile', index)"
        @sync-name="$emit('syncTurboProfileName', index)"
      >
        <TurboProfileTiming v-model="profile.experimental" :pacing-label="pacingModeLabel" />
        <div v-if="config.modules.turbo.experimental.enabled && !config.modules.turbo.experimental.applicationIds.length" class="mt-3 rounded-lg border p-3 text-sm chip-warning">
          {{ t('turbo.tab.profiles.legacyExperimental') }}
          <button class="button-secondary ml-2 rounded-lg px-3 py-2 text-xs" @click="profile.experimental = { ...config.modules.turbo.experimental, applicationIds: [] }; config.modules.turbo.experimental = defaultTurboExperimental()">{{ t('turbo.tab.profiles.useSavedTiming') }}</button>
        </div>
        <label class="mt-3 flex items-start gap-2.5 rounded-[1rem] border px-4 py-3 text-sm surface-panel-soft" style="border-color: var(--app-border)">
          <input v-model="profile.disableSafety" type="checkbox" class="mt-1 h-4 w-4 accent-depthxr-copper" />
          <span><span class="font-medium" style="color: var(--app-text)">{{ t('turbo.tab.profiles.bypassSafety') }}</span>
            <span class="block text-xs leading-5 text-muted">{{ t('turbo.tab.profiles.bypassSafetyHint') }}</span></span>
        </label>
      </ProfileShell>

      <div
        v-if="config.modules.turbo.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        {{ t('turbo.tab.profiles.empty') }}
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="max-h-[85vh] w-full max-w-[720px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('turbo.howItWorks.eyebrow') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('turbo.howItWorks.title') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">{{ t('common.close') }}</button>
        </div>

        <div class="mt-5 space-y-5 text-sm leading-6">
          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.howItWorks.whatItDoes') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('turbo.howItWorks.whatItDoesBody') }}
            </div>
            <I18nText keypath="turbo.howItWorks.strategies" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #async><strong>{{ t('turbo.strategy.async') }}</strong></template>
              <template #sequenced><strong>{{ t('turbo.strategy.sequenced') }}</strong></template>
              <template #auto><strong>{{ t('turbo.strategy.auto') }}</strong></template>
            </I18nText>
            <I18nText keypath="turbo.howItWorks.toolkit" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #toolkit><strong>{{ t('turbo.strategy.toolkit') }}</strong></template>
            </I18nText>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.howItWorks.whatToExpect') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('turbo.howItWorks.whatToExpectBody') }}
            </div>
            <I18nText keypath="turbo.howItWorks.reprojection" tag="div" class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <template #label><strong>{{ t('turbo.howItWorks.reprojectionLabel') }}</strong></template>
            </I18nText>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.howItWorks.bestStrategy') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('turbo.howItWorks.bestStrategyBody') }}
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('turbo.howItWorks.safetyNet') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              {{ t('turbo.howItWorks.safetyNetBody') }}
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
