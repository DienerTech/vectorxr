<script setup lang="ts">
import { computed, nextTick, ref } from 'vue'

import PivotBindingsPage from '../PivotBindingsPage.vue'
import PivotBindingsPanel from '../PivotBindingsPanel.vue'
import PivotSettingsPage from '../PivotSettingsPage.vue'
import PivotSettingsSummary from '../PivotSettingsSummary.vue'
import ProfileShell from '../ProfileShell.vue'
import type { RegisteredApplication, VectorXRConfig } from '../../lib/model'
import { bindingLabel, bindingsMatchRuntimeActivation, pivotBindingConflictWarnings } from '../../lib/model'

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

function profileContextLabel(target: 'default' | number | null): string {
  if (target === 'default' || target === null) {
    return 'Default Profile'
  }
  const profile = props.config.modules.pivotxr.profiles[target]
  return profile?.name?.trim() || `Profile ${target + 1}`
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

function openSettings(target: 'default' | number) {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  settingsTarget.value = target
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeSubPages() {
  bindingsTarget.value = null
  settingsTarget.value = null
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

const defaultBindingWarnings = computed(() => pivotBindingConflictWarnings(
  props.config.modules.pivotxr.activationMode,
  props.config.modules.pivotxr.activationBinding,
  props.config.modules.pivotxr.setOriginBinding,
  props.config.modules.pivotxr.releaseOriginBinding,
))

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  const enabledProfiles = props.config.modules.pivotxr.profiles
    .map((profile, index) => ({ profile, index }))
    .filter(({ profile }) => profile.enabled)
  for (const { profile, index } of enabledProfiles) {
    const conflictMessages = pivotBindingConflictWarnings(
      profile.activationMode,
      profile.activationBinding,
      profile.setOriginBinding,
      profile.releaseOriginBinding,
    ).map((warning) => warning.message)
    if (conflictMessages.length > 0) {
      warnings.set(index, conflictMessages)
    }
  }

  const activatableProfiles = enabledProfiles.filter(({ profile }) => profile.activationBinding.type !== 'none')

  for (let i = 0; i < activatableProfiles.length; i++) {
    for (let j = 0; j < i; j++) {
      const a = activatableProfiles[i]
      const b = activatableProfiles[j]
      const labelA = bindingLabel(a.profile.activationBinding)

      if (!bindingsMatchRuntimeActivation(a.profile.activationBinding, b.profile.activationBinding)) {
        continue
      }

      for (const applicationId of a.profile.applicationIds) {
        if (b.profile.applicationIds.includes(applicationId)) {
          const appName = applicationNameById.get(applicationId) ?? applicationId
          const message = a.profile.activationMode === 'alwaysOn'
            ? `For ${appName}, binding ${labelA} is owned by the higher-priority "${b.profile.name}" (Profile ${b.index + 1}). Always On still engages automatically, but this binding cannot suspend or resume it. Reorder the profiles or pick a different binding.`
            : `For ${appName}, binding ${labelA} is owned by the higher-priority "${b.profile.name}" (Profile ${b.index + 1}); it will not activate this profile. Reorder the profiles or pick a different binding.`
          warnings.set(a.index, [...(warnings.get(a.index) ?? []), message])
          break
        }
      }
    }
  }

  return warnings
})
</script>

<template>
  <PivotBindingsPage
    v-if="bindingsSubject"
    :subject="bindingsSubject"
    :context-label="bindingsContextLabel"
    :config="config"
    @close="closeSubPages"
  />
  <PivotSettingsPage
    v-else-if="settingsSubject"
    :settings="settingsSubject"
    :context-label="settingsContextLabel"
    @close="closeSubPages"
  />
  <div v-else class="space-y-4">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Pivot</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Configure how Pivot activates and how much extra yaw or pitch it can add for each title. Use profiles to keep rotation behavior specific to the games and simulators that benefit from it.
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
            Quadviews Compatibility
          </button>
          <button
            class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="centeringInfoOpen = true"
          >
            <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
              i
            </span>
            Centering &amp; Alignment
          </button>
        </div>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>

        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.pivotxr.enabled ? 'On' : 'Off' }}
        </label>

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

          <PivotBindingsPanel
            :activation-mode="config.modules.pivotxr.activationMode"
            :activation-binding="config.modules.pivotxr.activationBinding"
            :set-origin-binding="config.modules.pivotxr.setOriginBinding"
            :release-origin-binding="config.modules.pivotxr.releaseOriginBinding"
            class="mb-3"
            @edit="openBindings('default')"
          />

          <PivotSettingsSummary :settings="config.modules.pivotxr.defaults" @edit="openSettings('default')" />
        </div>
        <div v-else class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          The default profile is off and has no effect — applications without an enabled custom profile get no Pivot. Enabled custom profiles below still apply to their assigned applications.
        </div>
      </details>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <!-- Sticky so Add Profile stays reachable while scrolling a long profile list. -->
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">
            Override Pivot per application. Several profiles can target the same title with different bindings — the binding you press picks the profile. Order sets priority when bindings collide.
          </p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addPivotProfile')"
        >
          Add Profile
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
          warning-title="Binding warning"
          :warnings="profileWarnings.get(index)"
          reorderable
          :profile-count="config.modules.pivotxr.profiles.length"
          @remove="$emit('removePivotProfile', index)"
          @move="$emit('movePivotProfile', index, $event)"
          @sync-name="$emit('syncPivotProfileName', index)"
        >
          <PivotBindingsPanel
            :activation-mode="profile.activationMode"
            :activation-binding="profile.activationBinding"
            :set-origin-binding="profile.setOriginBinding"
            :release-origin-binding="profile.releaseOriginBinding"
            @edit="openBindings(index)"
          />

          <PivotSettingsSummary class="mt-3" :settings="profile.settings" @edit="openSettings(index)" />
        </ProfileShell>
      </TransitionGroup>

      <div
        v-if="config.modules.pivotxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to bind Pivot rotation to a specific application.
      </div>
    </section>

    <div v-if="compatibilityInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Pivot Compatibility</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">A Special Note About Quadviews</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="compatibilityInfoOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-success" style="border-color: var(--app-border)">
            VectorXR Quadviews is fully compatible with Pivot and Depth when all three modules are managed by VectorXR.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Pivot is NOT currently compatible with quadviews supplied directly by the runtime, such as Pimax Play.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Pivot also works with external OpenXR quadviews layers, including mbucchia's `Quad-Views-Foveated`, when layer order is configured correctly.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            When using quadviews, OpenXR layer order matters. `Quad-Views-Foveated` should be first in the Windows OpenXR API layers list, with VectorXR second. If VectorXR is above the quadviews layer, Pivot rotation will cause the scene to break, rendering no content in areas outside your head's natural field of view.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            You can inspect and change that order with the OpenXR Layer Manager tab in VectorXR.
          </div>
        </div>
      </div>
    </div>

    <div v-if="centeringInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Pivot Centering</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">Keeping Pivot Aligned</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="centeringInfoOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Pivot rotates around your headset's center point, not the game's. These are two separate concepts of "forward," and by default Pivot uses the HMD one.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            If Pivot rotation feels off-center or skewed to one side, your HMD center and your in-game center have drifted apart.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            The best fix: assign the optional <strong>Set Origin</strong> binding (under Edit Bindings) to the same button you use to recenter the view in-game. Every recenter then updates both origins together, so they can never drift apart.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Without a Set Origin binding, you can re-align manually: recenter your HMD first, then recenter your view in the game. Doing both, in that order, re-aligns the two centers so Pivot rotates evenly around where you are actually looking.
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
