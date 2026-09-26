<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'
import { clearTurboFaultLogs, clearTurboSafetyBlock, loadRuntimeStatus, type RuntimeStatusEnvelope } from '../../lib/commands'

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
    recoveryError.value = error instanceof Error ? error.message : String(error)
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
    recoveryError.value = error instanceof Error ? error.message : String(error)
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
      recoveryError.value = 'Recovery status is unavailable. Your saved safety setting still applies.'
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
const toggleBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  props.config.modules.turbo.toggleBinding,
]))


const turboInUse = computed(
  () => props.config.modules.turbo.enabled || props.config.modules.turbo.profiles.some((profile) => profile.enabled),
)



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
    return 'Forced Async'
  }
  if (mode === 'sequenced') {
    return 'Forced Sequenced'
  }
  return 'Auto · stability-first'
})

const pacingSummary = computed(() => {
  if (props.config.modules.turbo.pacingMode !== 'auto') {
    return 'A manual strategy is being used for every runtime.'
  }
  const environments = props.runtimePacing.length
  if (environments === 0) {
    return 'Auto picks and remembers a stable strategy per runtime. Try Async and Sequenced to find the fastest.'
  }
  return `${environments} runtime ${environments === 1 ? 'environment' : 'environments'} observed. Auto picks the most stable strategy for each, not necessarily the fastest.`
})

const metricsModeLabel = computed(() => {
  const mode = props.config.modules.turbo.metricsMode
  if (mode === 'binding') {
    return 'Controlled capture'
  }
  if (mode === 'off') {
    return 'Collection off'
  }
  return 'Automatic capture'
})

const metricsSummary = computed(() => {
  const count = props.turboMetrics.length
  if (count === 0) {
    return 'Capture Turbo on and off in the same flight to measure the gain for each strategy.'
  }
  const latest = props.turboMetrics[0]
  const app = latest.appName || 'unknown app'
  return `${count} ${count === 1 ? 'flight' : 'flights'} recorded. Latest: ${app}${latest.live ? ' (in progress)' : ''}.`
})

const tools = computed(() => [
  { page: 'runtime' as const, title: 'Runtime Behavior', chip: pacingModeLabel.value, summary: pacingSummary.value,
    icon: 'M3 5.5A2.5 2.5 0 0 1 5.5 3h9A2.5 2.5 0 0 1 17 5.5v4a2.5 2.5 0 0 1-2.5 2.5h-9A2.5 2.5 0 0 1 3 9.5v-4Zm2.5-.5a.5.5 0 0 0-.5.5v4a.5.5 0 0 0 .5.5h9a.5.5 0 0 0 .5-.5v-4a.5.5 0 0 0-.5-.5h-9ZM6 15a1 1 0 0 1 1-1h6a1 1 0 1 1 0 2H7a1 1 0 0 1-1-1Z' },
  { page: 'diagnostics' as const, title: 'Performance Diagnostics', chip: metricsModeLabel.value, summary: metricsSummary.value,
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
    module-label="Turbo"
    :binding="config.modules.turbo.toggleBinding"
    label="Turbo Toggle Binding"
    description="Flip Turbo on and off while in-game to compare fps and frame feel directly—and to disable it immediately if presentation breaks. The binding requires Turbo to be enabled for the application in its profile. It can also retry Turbo when recovery has blocked it or pacing trouble has suspended it. Switching it mid-session can cause a brief re-synchronization hitch."
    none-text="No binding assigned. Turbo starts automatically where enabled, unless recovery or pacing protection blocks it."
    default-activate-sound="turbo-on.wav"
    default-deactivate-sound="turbo-off.wav"
    :warnings="toggleBindingWarnings"
    @update:binding="config.modules.turbo.toggleBinding = $event"
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
            <h2 class="text-2xl font-semibold tracking-tight">Turbo</h2>
            <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">Advanced pacing override</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Moves runtime waiting out of a game's critical path when pacing is holding it back. Enable it only where an in-game A/B comparison proves that it helps.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">i</span>
          How Turbo Works
        </button>
      </div>

      <div
        v-if="toolkitConflict"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        OpenXR Toolkit is enabled alongside VectorXR Turbo. If OXRTK's Turbo Mode is also on, disable one of them so the two pacing layers do not compete.
      </div>

      <section class="border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3">
          <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Essentials</p>
          <p class="mt-1 text-sm text-muted">Leave the default off and enable Turbo per application with a custom profile, or turn it on broadly. Enablement applies on Save; the toggle binding switches Turbo off and on in the running game.</p>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight">Default Profile</p>
              <p class="mt-1 text-xs leading-5 text-muted">
                {{ config.modules.turbo.enabled
                  ? 'Turbo applies to applications without a custom profile.'
                  : 'Off unless an enabled custom profile turns it on.' }}
              </p>
            </div>
            <div class="flex flex-wrap items-center justify-end gap-2">
              <DefaultProfileExclusions
                :applications="applications"
                :profiles="config.modules.turbo.profiles"
              />
              <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
                <input v-model="config.modules.turbo.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
                Default {{ config.modules.turbo.enabled ? 'On' : 'Off' }}
              </label>
            </div>
          </div>
          <ModuleBindingPanel
            heading="In-game Turbo Toggle"
            :binding="config.modules.turbo.toggleBinding"
            hint="Flip Turbo on and off while in-game for a direct comparison."
            @edit="openToggleBinding"
          />
        </div>

        <div
          class="mt-3 flex flex-wrap items-center gap-3 rounded-[1rem] border px-4 py-3"
          :class="safetyBlocks.length ? 'chip-warning' : 'surface-panel-soft'"
          style="border-color: var(--app-border)"
        >
          <div class="min-w-0 flex-1">
            <p class="text-sm font-semibold tracking-tight">Turbo Safety</p>
            <p class="text-xs leading-5" :class="safetyBlocks.length ? '' : 'text-muted'">
              {{ safetyBlocks.length
                ? `Turbo is being held off for ${safetyBlocks.length} ${safetyBlocks.length === 1 ? 'setup' : 'setups'}. Review, then clear or retry with the toggle binding.`
                : 'Holds Turbo off after an interrupted session or repeated runtime fault.' }}
            </p>
          </div>
          <span class="rounded-full px-2.5 py-1 text-xs" :class="safetyBlocks.length ? 'surface-panel-strong' : 'chip-idle'" style="box-shadow: none">{{ safetyBlocks.length }} {{ safetyBlocks.length === 1 ? 'block' : 'blocks' }} · {{ runtimeStatus.faults.length }} {{ runtimeStatus.faults.length === 1 ? 'fault' : 'faults' }}</span>
          <TurboSafetyToggle v-model="config.modules.turbo.interruptedSessionRecovery" />
          <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="openSubPage('safety')">{{ safetyBlocks.length ? 'Review…' : 'Details…' }}</button>
          <p v-if="recoveryError" class="w-full text-xs chip-danger" role="status">{{ recoveryError }}</p>
        </div>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3">
          <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Tune &amp; Verify</p>
          <p class="mt-1 text-sm text-muted">Turbo results depend on the game, headset, runtime, and driver. To find what works best, compare each strategy against Turbo off in the same flight.</p>
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
            Finding the best strategy for a game
          </summary>
          <ol class="mt-3 list-decimal space-y-1.5 pl-5 text-sm leading-6 text-muted">
            <li>Create a custom profile for the game and assign the in-game Turbo toggle. Set Performance Diagnostics to use a capture binding.</li>
            <li>Launch the game with one strategy. Fly a repeatable scenario for about a minute with Turbo on, then toggle it off for at least 30 seconds.</li>
            <li>Note the gain versus Turbo off in Performance Diagnostics. Each flight is labeled with the strategy it used.</li>
            <li>Change the strategy, Save, relaunch, and repeat. Try Async and Sequenced in Runtime Behavior, then the Toolkit-style profile strategy.</li>
            <li>Keep the strategy with the best gain and 1% lows and no stalls. Auto remains a safe default, but it picks the most stable strategy, not necessarily the fastest.</li>
          </ol>
        </details>
      </section>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Choose applications and a Turbo strategy per profile. If enabled profiles overlap, the first matching profile takes priority.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addTurboProfile')"
        >
          Add Profile
        </button>
      </div>

      <div v-if="config.modules.turbo.enabled" class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
        <strong>Broad enablement is active.</strong> Turbo applies to applications without a custom profile, including ones you have not validated. The safer setup is Default Off with this list used as an application allowlist.
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
        <TurboProfileTiming v-model="profile.experimental" :pacing-label="pacingModeLabel" />
        <div v-if="config.modules.turbo.experimental.enabled && !config.modules.turbo.experimental.applicationIds.length" class="mt-3 rounded-lg border p-3 text-sm chip-warning">
          Previous experimental settings were saved without an application assignment.
          <button class="button-secondary ml-2 rounded-lg px-3 py-2 text-xs" @click="profile.experimental = { ...config.modules.turbo.experimental, applicationIds: [] }; config.modules.turbo.experimental = defaultTurboExperimental()">Use saved timing in this profile</button>
        </div>
        <label class="mt-3 flex items-start gap-2.5 rounded-[1rem] border px-4 py-3 text-sm surface-panel-soft" style="border-color: var(--app-border)">
          <input v-model="profile.disableSafety" type="checkbox" class="mt-1 h-4 w-4 accent-depthxr-copper" />
          <span><span class="font-medium" style="color: var(--app-text)">Bypass Turbo Safety for these applications</span>
            <span class="block text-xs leading-5 text-muted">Ignores persistent safety blocks. Live pacing fallback still suspends a failing session; use the toggle binding to retry.</span></span>
        </label>
      </ProfileShell>

      <div
        v-if="config.modules.turbo.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add one when an application needs different behavior from the default.
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="max-h-[85vh] w-full max-w-[720px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Turbo Mode</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">How Turbo Works</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">Close</button>
        </div>

        <div class="mt-5 space-y-5 text-sm leading-6">
          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">What it does</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              A game normally idles until the headset runtime signals the next frame. Turbo performs that wait out of the game's way, so the game can begin its next frame as soon as the previous one is submitted. VectorXR preserves downstream frame-call order, but changes when pacing occurs and may estimate the next display time.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Async</strong> (usually fastest) waits in the background. <strong>Sequenced</strong> (for runtimes that need it) keeps waiting and submission in step. <strong>Auto</strong> starts with Async and falls back to Sequenced if Async is unstable, then remembers the result per runtime. Auto is stability-first: it picks the strategy that works, not necessarily the fastest one.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Toolkit-style</strong> is an experimental, per-profile strategy that recreates the timing of OpenXR Toolkit's Turbo Mode, originally developed by mbucchia. It always uses Async and takes effect after the game restarts.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">What to expect</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Turbo is a targeted fix, not a general boost. It helps only when runtime pacing holds fps below what the hardware can deliver, and it cannot remove a runtime-enforced framerate lock. Use Performance Diagnostics with the in-game toggle to measure each title.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Reprojection compatibility:</strong> Turbo can interfere with SteamVR Motion Smoothing and other reprojection systems such as ASW. Disable Motion Smoothing when testing Turbo. Frame-time prediction may be less accurate, and switching Turbo mid-session can briefly hitch while the pipeline re-synchronizes.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Finding the best strategy</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Turbo relies on OpenXR timing behavior that the specification does not formally define, so results vary by hardware, driver, runtime, and game. Strategies cannot always be switched safely mid-session, so test one per launch: use the in-game toggle to capture Turbo on and Turbo off in the same flight, then compare the gain for each strategy in Performance Diagnostics.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Safety net</p>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              If pacing stalls even after Auto adapts, Turbo suspends itself for that session. With Turbo Safety enabled, the application and runtime setup stays blocked until you clear it or retry with the binding. Fault details remain on the Turbo Safety page. This catches pacing stalls, not every visual or driver failure; retry only after the session is stable.
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
