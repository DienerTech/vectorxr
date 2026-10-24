<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'

import { t } from '../../i18n/index.ts'
import { loadRuntimeStatus, type RuntimeStatusSession } from '../../lib/commands'
import I18nText from '../I18nText.vue'
import DefaultProfileExclusions from '../DefaultProfileExclusions.vue'
import FrameGenSettingsFields from '../FrameGenSettingsFields.vue'
import { useGpuCapabilities } from '../../lib/gpuCapabilities'
import { motionSourceState } from '../../lib/motionSource'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import SetupGuide from '../SetupGuide.vue'
import {
  savedBindingConflictWarnings,
  type AppTab,
  type FrameGenDiagnosticView,
  type FrameGenSettings,
  type RegisteredApplication,
  type VectorXRConfig,
} from '../../lib/model'
import { configWarnings } from '../../lib/validation'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addFrameGenProfile: []
  removeFrameGenProfile: [index: number]
  syncFrameGenProfileName: [index: number]
  navigate: [tab: AppTab]
}>()

const framegen = computed(() => props.config.modules.framegen)
const howItWorksOpen = ref(false)
const bindingSubPageOpen = ref(false)
let savedScrollTop = 0

// Open the setup guide for first-time setup; returning users with an enabled profile start with it closed.
const setupGuideOpen = !(props.config.modules.framegen.enabled || props.config.modules.framegen.profiles.some((profile) => profile.enabled))
const toggleBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, framegen.value.toggleBindings))
const turboWarnings = computed(() => configWarnings(props.config))

const restartNote = computed(() => t('framegen.restartNote'))

const diagnosticViews = computed<{ value: FrameGenDiagnosticView; label: string; description: string }[]>(() => [
  { value: 'off', label: t('framegen.diagnostic.views.off.label'), description: t('framegen.diagnostic.views.off.description') },
  { value: 'flow', label: t('framegen.diagnostic.views.flow.label'), description: t('framegen.diagnostic.views.flow.description') },
  { value: 'confidence', label: t('framegen.diagnostic.views.confidence.label'), description: t('framegen.diagnostic.views.confidence.description') },
  { value: 'tint', label: t('framegen.diagnostic.views.tint.label'), description: t('framegen.diagnostic.views.tint.description') },
])
const diagnosticDescription = computed(() => diagnosticViews.value.find((view) => view.value === framegen.value.diagnosticView)?.description ?? '')

// Live session status relayed by the layer.
const sessions = ref<RuntimeStatusSession[]>([])
const statusError = ref(false)
let disposed = false
let poll: ReturnType<typeof setTimeout> | undefined
async function refresh() {
  try {
    const status = await loadRuntimeStatus()
    if (!disposed) { sessions.value = status.sessions; statusError.value = false }
  } catch { if (!disposed) statusError.value = true }
  if (!disposed) poll = setTimeout(refresh, 2000)
}
onMounted(refresh)
onUnmounted(() => { disposed = true; if (poll) clearTimeout(poll) })

// The layer's state and a short reason, in the words the OSD uses.
function sessionStatus(session: RuntimeStatusSession): { label: string; chip: string } {
  const reason = session.state.framegenReason?.trim()
  // The reason comes from the layer and stays as it reads there.
  const withReason = (status: string, colon = false) =>
    reason ? t(colon ? 'framegen.session.withReasonColon' : 'framegen.session.withReason', { status, reason }) : status
  switch (session.state.framegenState) {
    case 'active': return { label: withReason(t('framegen.session.active')), chip: 'chip-success' }
    case 'standby': return { label: withReason(t('framegen.session.standby')), chip: 'chip-idle' }
    case 'waiting': return { label: withReason(t('framegen.session.waiting')), chip: 'chip-idle' }
    case 'paused': return { label: withReason(t('framegen.session.paused')), chip: 'chip-warning' }
    case 'unavailable': return { label: withReason(t('framegen.session.unavailable'), true), chip: 'chip-warning' }
    case 'off': return { label: withReason(t('framegen.session.off')), chip: 'chip-idle' }
    case undefined: return { label: t('framegen.session.needsLayer'), chip: 'chip-idle' }
    default: return { label: withReason(t('framegen.session.unknown')), chip: 'chip-idle' }
  }
}

// The game's depth helps generated frames keep near objects steady as the
// head moves. Shown only while it is in use or when the player can act on it.
function depthStatus(session: RuntimeStatusSession): { label: string; title: string; chip: string } | null {
  switch (session.state.framegenDepth) {
    case 'in-use': return { label: t('framegen.gameDepth.inUse.label'), title: t('framegen.gameDepth.inUse.title'), chip: 'chip-success' }
    case 'empty': return { label: t('framegen.gameDepth.empty.label'), title: t('framegen.gameDepth.empty.title'), chip: 'chip-warning' }
    case 'unusable': return { label: t('framegen.gameDepth.unusable.label'), title: t('framegen.gameDepth.unusable.title'), chip: 'chip-idle' }
    default: return null
  }
}

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openToggleBinding() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  bindingSubPageOpen.value = true
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeToggleBinding() {
  bindingSubPageOpen.value = false
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  framegen.value.profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex === undefined) {
        firstProfileByApplication.set(applicationId, index)
        continue
      }

      const appName = applicationNameById.get(applicationId) ?? applicationId
      warnings.set(index, [
        ...(warnings.get(index) ?? []),
        t('framegen.customProfiles.duplicateApplication', { application: appName, profile: firstIndex + 1 }),
      ])
    }
  })

  return warnings
})

const gpuCapabilities = useGpuCapabilities()

function settingsSummary(settings: FrameGenSettings) {
  const mode = settings.synthesisMode === 'extrapolate' ? t('framegen.settings.mode.extrapolate.label') : t('framegen.settings.mode.interpolate.label')
  const rate = settings.frameMultiplier === 'dynamic'
    ? t('framegen.summary.dynamic')
    : settings.frameMultiplier === 'auto' ? t('framegen.summary.fixedAuto') : t('framegen.summary.fixed', { multiplier: settings.frameMultiplier })
  // The source in use: Shader while interpolating in Dynamic, or with no card that has the hardware.
  const motion = motionSourceState(settings, gpuCapabilities.value).effective === 'hardware'
    ? t('framegen.settings.motionSource.hardware.label')
    : t('framegen.settings.motionSource.shader.label')
  return t('framegen.summary.format', { mode, rate, motion })
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    :module-label="t('framegen.title')"
    :bindings="framegen.toggleBindings"
    :label="t('framegen.toggle.label')"
    :description="t('framegen.toggle.description')"
    :none-text="t('framegen.toggle.noneText')"
    :warnings="toggleBindingWarnings"
    @update:bindings="framegen.toggleBindings = $event"
    @close="closeToggleBinding"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-start justify-between gap-4">
        <div>
          <div class="flex flex-wrap items-center gap-2">
            <h2 class="text-2xl font-semibold tracking-tight">{{ t('framegen.title') }}</h2>
            <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">{{ t('framegen.experimental') }}</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            {{ t('framegen.intro') }}
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">i</span>
          {{ t('framegen.howItWorksTitle') }}
        </button>
      </div>

      <div
        v-for="warning in turboWarnings"
        :key="warning"
        class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        {{ warning }}
      </div>

      <SetupGuide class="mb-3" :title="t('framegen.setupGuide.title')" :summary="t('framegen.setupGuide.summary')" :open="setupGuideOpen">
        <ol class="list-decimal space-y-1 pl-5 text-sm leading-6">
          <I18nText keypath="framegen.setupGuide.smoothing.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.smoothing.label') }}</strong></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.refreshRate.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.refreshRate.label') }}</strong></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.halfRate.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.halfRate.label') }}</strong></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.oneGame.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.oneGame.label') }}</strong></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.restart.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.restart.label') }}</strong></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.check.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.check.label') }}</strong></template>
            <template #osd><button class="underline underline-offset-2" type="button" @click="$emit('navigate', 'osd')">{{ t('framegen.setupGuide.check.osd') }}</button></template>
          </I18nText>
          <I18nText keypath="framegen.setupGuide.compare.body" tag="li">
            <template #label><strong>{{ t('framegen.setupGuide.compare.label') }}</strong></template>
          </I18nText>
        </ol>
        <p class="mt-3 text-xs leading-5 text-muted">
          {{ t('framegen.setupGuide.turboNote') }}
        </p>
      </SetupGuide>

      <p class="mb-4 flex items-start gap-2 text-xs leading-5 text-muted" role="note">
        <span class="restart-required-mark" aria-hidden="true">&#8635;</span>
        <I18nText keypath="framegen.restartToApply.body">
          <template #label><strong class="font-semibold">{{ t('framegen.restartToApply.label') }}</strong></template>
        </I18nText>
      </p>

      <div class="mb-4 space-y-3 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-center gap-2">
          <p class="eyebrow mr-1 text-xs uppercase tracking-[0.18em]">{{ t('framegen.controls.heading') }}</p>
          <span v-if="statusError" class="text-xs text-muted">{{ t('framegen.controls.statusUnavailable') }}</span>
          <span v-else-if="!sessions.length" class="chip-idle rounded-full px-2.5 py-1 text-xs">{{ t('framegen.controls.noSession') }}</span>
          <template v-else>
            <template v-for="session in sessions" :key="session.sessionId">
              <span class="rounded-full px-2.5 py-1 text-xs" :class="sessionStatus(session).chip">
                <strong class="font-semibold">{{ session.application }}</strong> · {{ sessionStatus(session).label }}
              </span>
              <span v-if="depthStatus(session)" class="rounded-full px-2.5 py-1 text-xs" :class="depthStatus(session)!.chip" :title="depthStatus(session)!.title">
                {{ depthStatus(session)!.label }}
              </span>
            </template>
          </template>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <ModuleBindingPanel
            :heading="t('framegen.toggle.heading')"
            :bindings="framegen.toggleBindings"
            :hint="t('framegen.toggle.hint')"
            @edit="openToggleBinding"
          />
          <div class="rounded-[1rem] border p-4 surface-panel-soft">
            <label class="flex flex-wrap items-center justify-between gap-3">
              <span class="text-sm font-semibold tracking-tight">{{ t('framegen.diagnostic.heading') }}</span>
              <select v-model="framegen.diagnosticView" class="app-input manual-input-select rounded-[0.75rem] px-3 py-1.5 text-sm">
                <option v-for="view in diagnosticViews" :key="view.value" :value="view.value">{{ view.label }}</option>
              </select>
            </label>
            <p class="mt-2 text-xs leading-5 text-muted">{{ t('framegen.diagnostic.hint', { description: diagnosticDescription }) }}</p>
            <p v-if="framegen.diagnosticView !== 'off'" class="mt-2 rounded-[0.75rem] border px-3 py-2 text-xs leading-5 chip-warning" style="border-color: var(--app-border)" role="note">
              {{ t('framegen.diagnostic.onWarning') }}
            </p>
          </div>
        </div>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.defaultProfile.heading') }}</span>
          <span class="text-xs text-muted">{{ t('framegen.defaultProfile.appliesTo') }}</span>
          <span v-if="framegen.enabled" class="ml-auto rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-accent">
            {{ settingsSummary(framegen.defaults) }}
          </span>
        </summary>
        <div class="mt-3 flex flex-wrap items-center gap-2">
          <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium" :title="restartNote">
            <input v-model="framegen.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            {{ framegen.enabled ? t('framegen.defaultProfile.toggleOn') : t('framegen.defaultProfile.toggleOff') }}
            <span class="restart-required-mark" :title="restartNote">&#8635;</span>
          </label>
          <DefaultProfileExclusions :applications="applications" :profiles="framegen.profiles" />
        </div>
        <p v-if="!framegen.enabled" class="mt-2 text-xs text-muted">
          {{ t('framegen.defaultProfile.offNote') }}
        </p>
        <template v-else>
          <I18nText keypath="framegen.defaultProfile.onWarning.body" tag="div" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
            <template #label><strong>{{ t('framegen.defaultProfile.onWarning.label') }}</strong></template>
          </I18nText>
          <FrameGenSettingsFields class="mt-3" :settings="framegen.defaults" />
        </template>
      </details>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">{{ t('framegen.customProfiles.heading') }}</h2>
          <p class="text-sm text-muted">{{ t('framegen.customProfiles.description') }}</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addFrameGenProfile')"
        >
          {{ t('framegen.customProfiles.add') }}
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in framegen.profiles"
        :key="`framegen-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        :module-label="t('framegen.title')"
        :enabled-change-note="restartNote"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeFrameGenProfile', index)"
        @sync-name="$emit('syncFrameGenProfileName', index)"
      >
        <template #badges>
          <span v-if="profile.enabled" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">{{ settingsSummary(profile.settings) }}</span>
        </template>

        <FrameGenSettingsFields :settings="profile.settings" />
      </ProfileShell>

      <div
        v-if="framegen.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        {{ t('framegen.customProfiles.empty') }}
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="max-h-[85vh] w-full max-w-[720px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('framegen.title') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('framegen.howItWorksTitle') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">{{ t('common.close') }}</button>
        </div>

        <div class="mt-5 space-y-5 text-sm leading-6">
          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.howItWorks.whatItDoes.heading') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('framegen.howItWorks.whatItDoes.body') }}
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.howItWorks.mode.heading') }}</p>
            <I18nText keypath="framegen.howItWorks.mode.interpolate" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #name><strong>{{ t('framegen.settings.mode.interpolate.label') }}</strong></template>
            </I18nText>
            <I18nText keypath="framegen.howItWorks.mode.extrapolate" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #name><strong>{{ t('framegen.settings.mode.extrapolate.label') }}</strong></template>
            </I18nText>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.howItWorks.frameRate.heading') }}</p>
            <I18nText keypath="framegen.howItWorks.frameRate.dynamic" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #name><strong>{{ t('framegen.settings.frameRate.dynamic.label') }}</strong></template>
            </I18nText>
            <I18nText keypath="framegen.howItWorks.frameRate.fixed" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #name><strong>{{ t('framegen.settings.frameRate.fixed.label') }}</strong></template>
              <template #x2><strong>2x</strong></template>
              <template #x3><strong>3x</strong></template>
              <template #x4><strong>4x</strong></template>
              <template #auto><strong>{{ t('common.auto') }}</strong></template>
            </I18nText>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.howItWorks.beforeYouStart.heading') }}</p>
            <I18nText keypath="framegen.howItWorks.beforeYouStart.smoothing" tag="div" class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <template #label><strong>{{ t('framegen.setupGuide.smoothing.label') }}</strong></template>
            </I18nText>
            <I18nText keypath="framegen.howItWorks.beforeYouStart.turbo.body" tag="div" class="rounded-[1rem] border px-4 py-3 surface-panel">
              <template #label><strong>{{ t('framegen.howItWorks.beforeYouStart.turbo.label') }}</strong></template>
            </I18nText>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('framegen.howItWorks.beforeYouStart.apis') }}
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('framegen.howItWorks.knownLimits.heading') }}</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              {{ t('framegen.howItWorks.knownLimits.body') }}
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
