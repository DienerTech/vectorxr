<script setup lang="ts">
import { computed, nextTick, reactive, ref } from 'vue'

import { t } from '../../i18n/index.ts'
import ConvergenceField from '../ConvergenceField.vue'
import DefaultProfileExclusions from '../DefaultProfileExclusions.vue'
import DepthAnchorField from '../DepthAnchorField.vue'
import DepthBindingsPage from '../DepthBindingsPage.vue'
import DepthNetEffect from '../DepthNetEffect.vue'
import I18nText from '../I18nText.vue'
import ProfileShell from '../ProfileShell.vue'
import SetupGuide from '../SetupGuide.vue'
import StereoDepthField from '../StereoDepthField.vue'
import { toConvergenceDisplay, toStereoBoostDisplay } from '../../lib/display'
import { savedBindingConflictWarnings, type DepthXRSettings, type RegisteredApplication, type VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addProfile: []
  removeProfile: [index: number]
  syncProfileName: [index: number]
}>()

const depthInfoOpen = ref(false)
// Open the tuning guide for first-time setup; returning users with an enabled profile start with it closed.
const setupGuideOpen = !(props.config.modules.depthxr.enabled || props.config.modules.depthxr.profiles.some((profile) => profile.enabled))
const bindingsPageOpen = ref(false)
const stereoDepthDisplayLimits = reactive(new WeakMap<DepthXRSettings, number>())
const convergenceDisplayLimits = reactive(new WeakMap<DepthXRSettings, number>())
let savedScrollTop = 0

function stereoDepthDisplayLimit(settings: DepthXRSettings): number {
  return stereoDepthDisplayLimits.get(settings)
    ?? (Math.abs(toStereoBoostDisplay(settings.stereoBoost)) > 25 ? 100 : 25)
}

function setStereoDepthDisplayLimit(settings: DepthXRSettings, limit: number) {
  stereoDepthDisplayLimits.set(settings, limit > 25 ? 100 : 25)
}

function convergenceDisplayLimit(settings: DepthXRSettings): number {
  return convergenceDisplayLimits.get(settings)
    ?? (Math.abs(toConvergenceDisplay(settings.convergence)) > 5 ? 25 : 5)
}

function setConvergenceDisplayLimit(settings: DepthXRSettings, limit: number) {
  convergenceDisplayLimits.set(settings, limit > 5 ? 25 : 5)
}

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openBindings() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  bindingsPageOpen.value = true
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeBindings() {
  bindingsPageOpen.value = false
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

// Toggles with at least one binding ("n of 2 assigned"), however many each has.
const depthBindingCount = computed(() => [
  props.config.modules.depthxr.bindings.toggleEnabledBindings,
  props.config.modules.depthxr.bindings.toggleAnchorBindings,
].filter((bindings) => bindings.some((binding) => binding.type !== 'none')).length)

const depthBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  ...props.config.modules.depthxr.bindings.toggleEnabledBindings,
  ...props.config.modules.depthxr.bindings.toggleAnchorBindings,
]))

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  props.config.modules.depthxr.profiles.forEach((profile, index) => {
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
      const message = t('depth.customProfiles.duplicateApplication', { application: appName, profile: firstIndex + 1 })
      warnings.set(index, [...(warnings.get(index) ?? []), message])
    }
  })

  return warnings
})
</script>

<template>
  <DepthBindingsPage v-if="bindingsPageOpen" :config="config" @close="closeBindings" />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <!-- Module header -->
      <div class="mb-4 flex flex-wrap items-start justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">{{ t('depth.title') }}</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            {{ t('depth.intro') }}
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="depthInfoOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          {{ t('depth.troubleshooting.title') }}
        </button>
      </div>

      <SetupGuide class="mb-3" :title="t('depth.tuningGuide.title')" :summary="t('depth.tuningGuide.summary')" :open="setupGuideOpen">
      <div class="grid gap-3 lg:grid-cols-3" role="note" :aria-label="t('depth.tuningGuide.ariaLabel')">
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">{{ t('depth.tuningGuide.scale.step') }}</p>
          <p class="mt-2 text-sm font-semibold">{{ t('depth.tuningGuide.scale.name') }}</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            {{ t('depth.tuningGuide.scale.body') }}
          </p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">{{ t('depth.tuningGuide.place.step') }}</p>
          <p class="mt-2 text-sm font-semibold">{{ t('depth.tuningGuide.place.name') }}</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            {{ t('depth.tuningGuide.place.body') }}
          </p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">{{ t('depth.tuningGuide.compare.step') }}</p>
          <p class="mt-2 text-sm font-semibold">{{ t('depth.tuningGuide.compare.name') }}</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            {{ t('depth.tuningGuide.compare.body') }}
          </p>
        </div>
      </div>
      </SetupGuide>
      <I18nText
        keypath="depth.ipdNote.body"
        tag="p"
        class="mb-3 rounded-[0.9rem] border px-4 py-2.5 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        <template #lead><strong class="font-semibold">{{ t('depth.ipdNote.lead') }}</strong></template>
        <template #setting><strong>{{ t('depth.ipdNote.forceIpdDistance') }}</strong></template>
      </I18nText>

      <!-- Module-level binding — applies regardless of which profile is active -->
      <button
        class="group mb-5 w-full rounded-[1rem] border px-4 py-3 text-left transition surface-panel-soft hover:-translate-y-0.5 hover:shadow-panel"
        type="button"
        @click="openBindings"
      >
        <div class="flex flex-wrap items-center justify-between gap-4">
          <div>
            <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">{{ t('depth.bindingsCard.eyebrow') }}</p>
            <h3 class="mt-1 text-base font-semibold tracking-tight">{{ t('depth.bindingsCard.heading') }}</h3>
            <p class="mt-1 text-sm text-muted">{{ t('depth.bindingsCard.description') }}</p>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <span class="rounded-full border px-3 py-1 text-xs font-medium" style="border-color: var(--app-border)">
              {{ t('depth.bindingsCard.assigned', { count: depthBindingCount, total: 2 }) }}
            </span>
            <span v-if="depthBindingWarnings.length" class="rounded-full px-3 py-1 text-xs font-medium chip-warning">
              {{ t('depth.bindingsCard.duplicateInputs', { count: depthBindingWarnings.length }) }}
            </span>
            <span aria-hidden="true" class="transition group-hover:translate-x-1">&rarr;</span>
          </div>
        </div>
      </button>

      <!-- Default Profile -->
      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('depth.defaultProfile.heading') }}</span>
          <span class="text-xs text-muted">{{ t('depth.defaultProfile.appliesTo') }}</span>
        </summary>
        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ config.modules.depthxr.enabled ? t('depth.defaultProfile.toggleOn') : t('depth.defaultProfile.toggleOff') }}
        </label>
        <DefaultProfileExclusions
          class="ml-2 mt-3"
          :applications="applications"
          :profiles="config.modules.depthxr.profiles"
        />
        <p v-if="!config.modules.depthxr.enabled" class="mt-2 text-xs text-muted">
          {{ t('depth.defaultProfile.offNote') }}
        </p>
        <div v-else class="mt-3 space-y-3">
          <div class="grid gap-3 lg:grid-cols-2">
            <StereoDepthField
              v-model:value="config.modules.depthxr.defaults.stereoBoost"
              :display-limit="stereoDepthDisplayLimit(config.modules.depthxr.defaults)"
              @update:display-limit="setStereoDepthDisplayLimit(config.modules.depthxr.defaults, $event)"
            />
            <ConvergenceField
              v-model:value="config.modules.depthxr.defaults.convergence"
              :display-limit="convergenceDisplayLimit(config.modules.depthxr.defaults)"
              @update:display-limit="setConvergenceDisplayLimit(config.modules.depthxr.defaults, $event)"
            />
          </div>
          <DepthAnchorField v-model:value="config.modules.depthxr.defaults.depthAnchor" />
          <DepthNetEffect
            :stereo-boost="config.modules.depthxr.defaults.stereoBoost"
            :stereo-depth-limit="stereoDepthDisplayLimit(config.modules.depthxr.defaults)"
            :convergence="config.modules.depthxr.defaults.convergence"
            :convergence-limit="convergenceDisplayLimit(config.modules.depthxr.defaults)"
            :depth-lock="config.modules.depthxr.defaults.depthAnchor"
            @update:stereo-boost="config.modules.depthxr.defaults.stereoBoost = $event"
            @update:convergence="config.modules.depthxr.defaults.convergence = $event"
          />
        </div>
      </details>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">{{ t('depth.customProfiles.heading') }}</h2>
          <p class="text-sm text-muted">{{ t('depth.customProfiles.description') }}</p>
        </div>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('addProfile')">
          {{ t('depth.customProfiles.add') }}
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.depthxr.profiles"
        :key="`depth-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        :module-label="t('depth.title')"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeProfile', index)"
        @sync-name="$emit('syncProfileName', index)"
      >
        <div class="space-y-3">
          <div class="grid gap-3 lg:grid-cols-2">
            <StereoDepthField
              v-model:value="profile.settings.stereoBoost"
              :display-limit="stereoDepthDisplayLimit(profile.settings)"
              :muted="!profile.enabled"
              @update:display-limit="setStereoDepthDisplayLimit(profile.settings, $event)"
            />
            <ConvergenceField
              v-model:value="profile.settings.convergence"
              :display-limit="convergenceDisplayLimit(profile.settings)"
              :muted="!profile.enabled"
              @update:display-limit="setConvergenceDisplayLimit(profile.settings, $event)"
            />
          </div>
          <DepthAnchorField v-model:value="profile.settings.depthAnchor" :muted="!profile.enabled" />
          <DepthNetEffect
            :stereo-boost="profile.settings.stereoBoost"
            :stereo-depth-limit="stereoDepthDisplayLimit(profile.settings)"
            :convergence="profile.settings.convergence"
            :convergence-limit="convergenceDisplayLimit(profile.settings)"
            :depth-lock="profile.settings.depthAnchor"
            :muted="!profile.enabled"
            collapsible
            @update:stereo-boost="profile.settings.stereoBoost = $event"
            @update:convergence="profile.settings.convergence = $event"
          />
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.depthxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        {{ t('depth.customProfiles.empty') }}
      </div>
    </section>

    <div v-if="depthInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('depth.troubleshooting.title') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('depth.troubleshooting.heading') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="depthInfoOpen = false">
            {{ t('common.close') }}
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            {{ t('depth.troubleshooting.override') }}
          </div>

          <I18nText keypath="depth.troubleshooting.dcs.body" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel">
            <template #label><strong>{{ t('depth.troubleshooting.dcs.label') }}</strong></template>
            <template #setting><strong>{{ t('depth.ipdNote.forceIpdDistance') }}</strong></template>
          </I18nText>

          <I18nText keypath="depth.troubleshooting.expected.body" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel">
            <template #label><strong>{{ t('depth.troubleshooting.expected.label') }}</strong></template>
          </I18nText>

          <I18nText keypath="depth.troubleshooting.convergence.body" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel">
            <template #label><strong>{{ t('depth.troubleshooting.convergence.label') }}</strong></template>
          </I18nText>

          <I18nText keypath="depth.troubleshooting.depthLock.body" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel">
            <template #label><strong>{{ t('depth.troubleshooting.depthLock.label') }}</strong></template>
          </I18nText>
        </div>
      </div>
    </div>
  </div>
</template>
