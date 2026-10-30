<script setup lang="ts">
import { computed, nextTick, ref } from 'vue'

import { t } from '../../i18n/index.ts'
import DefaultProfileExclusions from '../DefaultProfileExclusions.vue'
import I18nText from '../I18nText.vue'
import PivotBindingsPage from '../PivotBindingsPage.vue'
import PivotBindingsPanel from '../PivotBindingsPanel.vue'
import PivotBehaviorPanel from '../PivotBehaviorPanel.vue'
import PivotNudgeSetPage from '../PivotNudgeSetPage.vue'
import PivotSettingsPage from '../PivotSettingsPage.vue'
import PivotSettingsSummary from '../PivotSettingsSummary.vue'
import PivotOriginPage from '../PivotOriginPage.vue'
import PivotOriginPanel from '../PivotOriginPanel.vue'
import PivotSnapBindingsPage from '../PivotSnapBindingsPage.vue'
import PivotSnapBindingsPanel from '../PivotSnapBindingsPanel.vue'
import PivotViewControlsPage from '../PivotViewControlsPage.vue'
import PivotViewControlsPanel from '../PivotViewControlsPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import type { InputBinding, PivotActivationBinding, RegisteredApplication, VectorXRConfig } from '../../lib/model'
import { bindingLabel, bindingsMatchRuntimeActivation, bindingsShareInput, createPivotNudgeSet, pivotBindingConflictWarnings, pivotSnapStackingStatus, pivotSnapViewMotionNote } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addPivotProfile: []
  removePivotProfile: [index: number]
  movePivotProfile: [index: number, direction: -1 | 1]
  syncPivotProfileName: [index: number]
}>()

const compatibilityInfoOpen = ref(false)
const centeringInfoOpen = ref(false)

// Bindings are edited on a dedicated sub-page (not a dialog) so the editors
// can grow vertically without overlaying scrolled content. 'default' targets
// the module's default profile; a number indexes a custom profile.
const bindingsTarget = ref<'default' | number | null>(null)

const bindingsSubject = computed(() => {
  if (bindingsTarget.value === null) {
    return null
  }
  if (bindingsTarget.value === 'default') {
    return props.config.modules.pivotxr
  }
  return props.config.modules.pivotxr.profiles[bindingsTarget.value] ?? null
})

const bindingsContextLabel = computed(() => profileContextLabel(bindingsTarget.value))

const originTarget = ref<'default' | number | null>(null)

const originSubject = computed(() => {
  if (originTarget.value === null) {
    return null
  }
  if (originTarget.value === 'default') {
    return props.config.modules.pivotxr
  }
  return props.config.modules.pivotxr.profiles[originTarget.value] ?? null
})

const originContextLabel = computed(() => profileContextLabel(originTarget.value))

// Rotation settings likewise get their own sub-page; the card shows a summary.
const settingsTarget = ref<'default' | number | null>(null)

const settingsSubject = computed(() => {
  if (settingsTarget.value === null) {
    return null
  }
  if (settingsTarget.value === 'default') {
    return props.config.modules.pivotxr.defaults
  }
  return props.config.modules.pivotxr.profiles[settingsTarget.value]?.settings ?? null
})

const settingsContextLabel = computed(() => profileContextLabel(settingsTarget.value))
const settingsSnapStacking = computed(() => pivotSnapStackingStatus(props.config, settingsTarget.value ?? 'default'))

const viewControlsTarget = ref<'default' | number | null>(null)
const viewControlsSubject = computed(() => {
  if (viewControlsTarget.value === null) return null
  if (viewControlsTarget.value === 'default') return props.config.modules.pivotxr
  return props.config.modules.pivotxr.profiles[viewControlsTarget.value] ?? null
})
const viewControlsContextLabel = computed(() => profileContextLabel(viewControlsTarget.value))

const snapBindingsTarget = ref<'default' | number | null>(null)
const snapBindingsSubject = computed(() => {
  if (snapBindingsTarget.value === null) return null
  if (snapBindingsTarget.value === 'default') return props.config.modules.pivotxr.viewControls
  return props.config.modules.pivotxr.profiles[snapBindingsTarget.value]?.viewControls ?? null
})
const snapBindingsContextLabel = computed(() => profileContextLabel(snapBindingsTarget.value))

const nudgeSetTarget = ref<string | null>(null)
const activeNudgeSet = computed(() => props.config.modules.pivotxr.nudgeSets.find((set) => set.id === nudgeSetTarget.value) ?? null)
const activeNudgeSetUsage = computed(() => {
  if (!activeNudgeSet.value) return 0
  const id = activeNudgeSet.value.id
  return Number(props.config.modules.pivotxr.nudgeSetId === id)
    + props.config.modules.pivotxr.profiles.filter((profile) => profile.nudgeSetId === id).length
})

function openNudgeSet(id: string) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  nudgeSetTarget.value = id
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function addNudgeSet(subject: { nudgeSetId: string }) {
  const source = props.config.modules.pivotxr.nudgeSets.find((set) => set.id === subject.nudgeSetId)
  const set = createPivotNudgeSet(
    t('pivot.tab.nudgeSetCopyName', { name: source?.name ?? t('pivot.tab.nudgeSetDefaultName') }),
    source?.settings,
    source?.allowWhileInactive ?? false,
  )
  props.config.modules.pivotxr.nudgeSets.push(set)
  subject.nudgeSetId = set.id
  openNudgeSet(set.id)
}

function deleteNudgeSet(id: string) {
  const sets = props.config.modules.pivotxr.nudgeSets
  if (sets.length <= 1) return
  const index = sets.findIndex((set) => set.id === id)
  if (index < 0) return
  const fallback = sets.find((set) => set.id !== id)
  if (!fallback) return
  if (props.config.modules.pivotxr.nudgeSetId === id) props.config.modules.pivotxr.nudgeSetId = fallback.id
  for (const profile of props.config.modules.pivotxr.profiles) {
    if (profile.nudgeSetId === id) profile.nudgeSetId = fallback.id
  }
  sets.splice(index, 1)
  closeSubPages()
}
function profileContextLabel(target: 'default' | number | null): string {
  if (target === 'default' || target === null) {
    return t('pivot.tab.defaultProfile')
  }
  const profile = props.config.modules.pivotxr.profiles[target]
  return profile?.name?.trim() || t('pivot.tab.profileNumber', { number: target + 1 })
}

// Sub-page navigation preserves the main page's scroll position, so returning
// lands on the profile that was being edited instead of the top of the page.
let savedScrollTop = 0

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openBindings(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  bindingsTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function openOrigin(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  originTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function openSettings(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  settingsTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function openViewControls(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  viewControlsTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function openSnapBindings(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  snapBindingsTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeSubPages() {
  bindingsTarget.value = null
  settingsTarget.value = null
  originTarget.value = null
  viewControlsTarget.value = null
  snapBindingsTarget.value = null
  nudgeSetTarget.value = null
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

const defaultBindingWarnings = computed(() => [
  ...pivotBindingConflictWarnings(
    props.config.modules.pivotxr.behavior === 'snapViews' ? false : props.config.modules.pivotxr.alwaysActive,
    props.config.modules.pivotxr.behavior === 'snapViews' ? [] : props.config.modules.pivotxr.activationBindings,
    props.config.modules.pivotxr.setOriginBindings,
    props.config.modules.pivotxr.releaseOriginBindings,
  ),
  ...(props.config.modules.pivotxr.behavior !== 'snapViews' && !props.config.modules.pivotxr.alwaysActive && props.config.modules.pivotxr.activationBindings.length === 0
    ? [{ title: t('pivot.warnings.noActivationTitle'), message: t('pivot.warnings.noActivationMessage') }]
    : []),
])

// Keeps the first of each physical input, so comparing the conflict warnings
// with and without duplicates tells whether a list repeats an input.
function withoutDuplicateInputs(bindings: InputBinding[]): InputBinding[] {
  return bindings.filter((binding, index) => !bindings.slice(0, index).some((earlier) => bindingsShareInput(earlier, binding)))
}

function withoutDuplicateActivationInputs(bindings: PivotActivationBinding[]): PivotActivationBinding[] {
  return bindings.filter((item, index) => !bindings.slice(0, index).some((earlier) => bindingsShareInput(earlier.binding, item.binding)))
}

const profileWarningState = computed(() => {
  const warnings = new Map<number, string[]>()
  // Profiles whose warnings include an activation overlap: a shadowed binding,
  // a shared Always active title, or a repeated input.
  const overlaps = new Set<number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))
  const enabledProfiles = props.config.modules.pivotxr.profiles
    .map((profile, index) => ({ profile, index }))
    .filter(({ profile }) => profile.enabled)

  for (const { profile, index } of enabledProfiles) {
    if (profile.behavior === 'enhancedMotion' && profile.alwaysActive) {
      for (const applicationId of profile.applicationIds) {
        const owner = enabledProfiles.find((candidate) => candidate.index < index && candidate.profile.behavior === 'enhancedMotion' && candidate.profile.alwaysActive && candidate.profile.applicationIds.includes(applicationId))
        if (owner) {
          const appName = applicationNameById.get(applicationId) ?? applicationId
          warnings.set(index, [...(warnings.get(index) ?? []), t('pivot.warnings.bothAlwaysActive', { owner: owner.profile.name, app: appName })])
          overlaps.add(index)
        }
      }
    }
    const conflictAlwaysActive = profile.behavior === 'snapViews' ? false : profile.alwaysActive
    const conflictActivationBindings = profile.behavior === 'snapViews' ? [] : profile.activationBindings
    const conflictMessages = pivotBindingConflictWarnings(
      conflictAlwaysActive,
      conflictActivationBindings,
      profile.setOriginBindings,
      profile.releaseOriginBindings,
    ).map((warning) => warning.message)
    if (conflictMessages.length > 0) {
      warnings.set(index, [...(warnings.get(index) ?? []), ...conflictMessages])
      const withoutDuplicates = pivotBindingConflictWarnings(
        conflictAlwaysActive,
        withoutDuplicateActivationInputs(conflictActivationBindings),
        withoutDuplicateInputs(profile.setOriginBindings),
        withoutDuplicateInputs(profile.releaseOriginBindings),
      )
      if (withoutDuplicates.length < conflictMessages.length) overlaps.add(index)
    }

    if (profile.behavior !== 'snapViews' && !profile.alwaysActive && profile.activationBindings.length === 0) {
      warnings.set(index, [...(warnings.get(index) ?? []), t('pivot.warnings.profileNoActivation')])
    }

    for (const activationBinding of profile.behavior === 'snapViews' ? [] : profile.activationBindings) {
      const binding = activationBinding.binding
      for (const applicationId of profile.applicationIds) {
        const owner = enabledProfiles.find((candidate) => (
          candidate.index < index
          && candidate.profile.behavior === 'enhancedMotion'
          && candidate.profile.applicationIds.includes(applicationId)
          && candidate.profile.activationBindings.some((candidateBinding) => (
            bindingsMatchRuntimeActivation(binding, candidateBinding.binding)
          ))
        ))
        if (!owner) continue

        const appName = applicationNameById.get(applicationId) ?? applicationId
        const label = bindingLabel(binding)
        const params = { app: appName, binding: label, owner: owner.profile.name, number: owner.index + 1 }
        const message = profile.alwaysActive
          ? t('pivot.warnings.bindingOwnedAlwaysOn', params)
          : t('pivot.warnings.bindingOwned', params)
        warnings.set(index, [...(warnings.get(index) ?? []), message])
        overlaps.add(index)
      }
    }
  }

  return { warnings, overlaps }
})
const profileWarnings = computed(() => profileWarningState.value.warnings)
const profileOverviewWarningIndexes = computed(() => profileWarningState.value.overlaps)
</script>

<template>
  <PivotSnapBindingsPage
    v-if="snapBindingsSubject"
    :view-controls="snapBindingsSubject"
    :context-label="snapBindingsContextLabel"
    :config="config"
    @close="closeSubPages"
  />
  <PivotBindingsPage
    v-else-if="bindingsSubject"
    :subject="bindingsSubject"
    :context-label="bindingsContextLabel"
    :config="config"
    @close="closeSubPages"
  />
  <PivotOriginPage
    v-else-if="originSubject"
    :subject="originSubject"
    :context-label="originContextLabel"
    :config="config"
    @close="closeSubPages"
  />
  <PivotNudgeSetPage
    v-else-if="activeNudgeSet"
    :nudge-set="activeNudgeSet"
    :usage-count="activeNudgeSetUsage"
    :can-delete="config.modules.pivotxr.nudgeSets.length > 1"
    @delete="deleteNudgeSet(activeNudgeSet.id)"
    @close="closeSubPages"
  />
  <PivotViewControlsPage
    v-else-if="viewControlsSubject"
    :subject="viewControlsSubject"
    :context-label="viewControlsContextLabel"
    :config="config"
    @close="closeSubPages"
  />
  <PivotSettingsPage
    v-else-if="settingsSubject"
    :settings="settingsSubject"
    :context-label="settingsContextLabel"
    :snap-stacking="settingsSnapStacking"
    @close="closeSubPages"
  />
  <div v-else class="space-y-4">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Pivot</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            {{ t('pivot.tab.intro') }}
          </p>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <button
            class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="compatibilityInfoOpen = true"
          >
            <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
              i
            </span>
            {{ t('pivot.tab.quadviewsCompatibilityButton') }}
          </button>
          <button
            class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="centeringInfoOpen = true"
          >
            <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
              i
            </span>
            {{ t('pivot.tab.centeringButton') }}
          </button>
        </div>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('pivot.tab.defaultProfile') }}</span>
          <span class="text-xs text-muted">{{ t('pivot.tab.defaultProfileScope') }}</span>
        </summary>

        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ config.modules.pivotxr.enabled ? t('pivot.tab.defaultProfileOn') : t('pivot.tab.defaultProfileOff') }}
        </label>
        <DefaultProfileExclusions
          class="ml-2 mt-3"
          :applications="applications"
          :profiles="config.modules.pivotxr.profiles"
        />

        <div v-if="config.modules.pivotxr.enabled" class="mt-3">
          <div
            v-for="warning in defaultBindingWarnings"
            :key="warning.title"
            class="mb-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
            style="border-color: var(--app-border)"
          >
            <p class="font-medium">{{ warning.title }}</p>
            <p class="mt-1">{{ warning.message }}</p>
          </div>

          <PivotBehaviorPanel
            :subject="config.modules.pivotxr"
            :nudge-sets="config.modules.pivotxr.nudgeSets"
            :settings="config.modules.pivotxr.defaults"
            :snap-stacking="pivotSnapStackingStatus(config, 'default')"
            :snap-view-motion-note="pivotSnapViewMotionNote(config, 'default')"
            class="mb-3"
            @edit-nudges="openNudgeSet"
            @add-nudge-set="addNudgeSet(config.modules.pivotxr)"
          />
          <PivotSettingsSummary
            v-if="config.modules.pivotxr.behavior !== 'snapViews'"
            :settings="config.modules.pivotxr.defaults"
            class="mb-3"
            @edit="openSettings('default')"
          />
          <PivotViewControlsPanel
            v-if="config.modules.pivotxr.behavior !== 'enhancedMotion'"
            :view-controls="config.modules.pivotxr.viewControls"
            class="mb-3"
            @edit="openViewControls('default')"
          />
          <PivotBindingsPanel
            v-if="config.modules.pivotxr.behavior !== 'snapViews'"
            :always-active="config.modules.pivotxr.alwaysActive"
            :activation-bindings="config.modules.pivotxr.activationBindings"
            @edit="openBindings('default')"
          />
          <PivotSnapBindingsPanel
            v-else
            :view-controls="config.modules.pivotxr.viewControls"
            @edit="openSnapBindings('default')"
          />
          <PivotOriginPanel
            :set-origin-bindings="config.modules.pivotxr.setOriginBindings"
            :release-origin-bindings="config.modules.pivotxr.releaseOriginBindings"
            class="mt-3"
            @edit="openOrigin('default')"
          />

        </div>
        <p v-else class="mt-2 text-xs text-muted">
          {{ t('pivot.tab.defaultProfileOffNote') }}
        </p>
      </details>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <!-- Sticky so Add Profile stays reachable while scrolling a long profile list. -->
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">{{ t('pivot.tab.customProfiles') }}</h2>
          <p class="text-sm text-muted">
            {{ t('pivot.tab.customProfilesIntro') }}
          </p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addPivotProfile')"
        >
          {{ t('pivot.tab.addProfile') }}
        </button>
      </div>

      <!-- TransitionGroup animates the cards swapping places on priority reorder
           (FLIP on the stable profile ids). -->
      <TransitionGroup name="pivot-profile" tag="div" class="flex flex-col gap-3">
        <ProfileShell
          v-for="(profile, index) in config.modules.pivotxr.profiles"
          :key="profile.id"
          :index="index"
          :profile="profile"
          :applications="applications"
          module-label="Pivot"
          :warning-title="t('pivot.tab.bindingWarning')"
          :warnings="profileWarnings.get(index)"
          reorderable
          :profile-count="config.modules.pivotxr.profiles.length"
          @remove="$emit('removePivotProfile', index)"
          @move="$emit('movePivotProfile', index, $event)"
          @sync-name="$emit('syncPivotProfileName', index)"
        >
          <template #badges>
            <span v-if="profileOverviewWarningIndexes.has(index)" class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em] chip-warning">
              {{ t('pivot.tab.activationOverlap') }}
            </span>
          </template>
          <PivotBehaviorPanel
            :subject="profile"
            :nudge-sets="config.modules.pivotxr.nudgeSets"
            :settings="profile.settings"
            :snap-stacking="pivotSnapStackingStatus(config, index)"
            :snap-view-motion-note="pivotSnapViewMotionNote(config, index)"
            @edit-nudges="openNudgeSet"
            @add-nudge-set="addNudgeSet(profile)"
          />
          <PivotSettingsSummary v-if="profile.behavior !== 'snapViews'" class="mt-3" :settings="profile.settings" @edit="openSettings(index)" />
          <PivotViewControlsPanel
            v-if="profile.behavior !== 'enhancedMotion'"
            :view-controls="profile.viewControls"
            class="mt-3"
            @edit="openViewControls(index)"
          />
          <PivotBindingsPanel
            v-if="profile.behavior !== 'snapViews'"
            :always-active="profile.alwaysActive"
            :activation-bindings="profile.activationBindings"
            class="mt-3"
            @edit="openBindings(index)"
          />
          <PivotSnapBindingsPanel
            v-else
            :view-controls="profile.viewControls"
            class="mt-3"
            @edit="openSnapBindings(index)"
          />
          <PivotOriginPanel
            :set-origin-bindings="profile.setOriginBindings"
            :release-origin-bindings="profile.releaseOriginBindings"
            class="mt-3"
            @edit="openOrigin(index)"
          />

        </ProfileShell>
      </TransitionGroup>

      <div
        v-if="config.modules.pivotxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        {{ t('pivot.tab.noCustomProfiles') }}
      </div>
    </section>

    <div v-if="compatibilityInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('pivot.compatibility.eyebrow') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('pivot.compatibility.title') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="compatibilityInfoOpen = false">
            {{ t('common.close') }}
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-success" style="border-color: var(--app-border)">
            {{ t('pivot.compatibility.managed') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            {{ t('pivot.compatibility.runtime') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('pivot.compatibility.external') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('pivot.compatibility.layerOrder') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('pivot.compatibility.layerManager') }}
          </div>
        </div>
      </div>
    </div>

    <div v-if="centeringInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('pivot.centering.eyebrow') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('pivot.centering.title') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="centeringInfoOpen = false">
            {{ t('common.close') }}
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('pivot.centering.center') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            {{ t('pivot.centering.drift') }}
          </div>

          <I18nText keypath="pivot.centering.bestFix" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel">
            <template #setOrigin><strong>{{ t('pivot.centering.setOrigin') }}</strong></template>
          </I18nText>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('pivot.centering.manual') }}
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* Reorder animation: TransitionGroup applies this class while a card FLIPs to
   its new slot after a priority move. */
.pivot-profile-move {
  transition: transform 0.3s ease;
}

@media (prefers-reduced-motion: reduce) {
  .pivot-profile-move {
    transition: none;
  }
}
</style>
