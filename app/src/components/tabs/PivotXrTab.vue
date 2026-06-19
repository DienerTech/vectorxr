<script setup lang="ts">
import { computed, ref } from 'vue'

import PivotActivationEditor from '../PivotActivationEditor.vue'
import PivotSettingsFields from '../PivotSettingsFields.vue'
import ProfileShell from '../ProfileShell.vue'
import type { RegisteredApplication, VectorXRConfig } from '../../lib/model'
import { bindingLabel } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addPivotProfile: []
  removePivotProfile: [index: number]
  syncPivotProfileName: [index: number]
}>()

const compatibilityInfoOpen = ref(false)

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  const enabledProfiles = props.config.modules.pivotxr.profiles
    .map((profile, index) => ({ profile, index }))
    .filter(({ profile }) => profile.enabled && profile.activationBinding.type !== 'none')

  for (let i = 0; i < enabledProfiles.length; i++) {
    for (let j = 0; j < i; j++) {
      const a = enabledProfiles[i]
      const b = enabledProfiles[j]
      const labelA = bindingLabel(a.profile.activationBinding)
      const labelB = bindingLabel(b.profile.activationBinding)

      if (labelA !== labelB) {
        continue
      }

      for (const applicationId of a.profile.applicationIds) {
        if (b.profile.applicationIds.includes(applicationId)) {
          const appName = applicationNameById.get(applicationId) ?? applicationId
          const message = `${appName} shares binding ${labelA} with Profile ${b.index + 1}. First active profile executes.`
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
  <div class="space-y-4">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Pivot</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Configure how Pivot activates and how much extra yaw or pitch it can add for each title. Use profiles to keep rotation behavior specific to the games and simulators that benefit from it.
          </p>
        </div>
        <div class="flex flex-wrap items-center gap-3">
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
          <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
            <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            Default Profile {{ config.modules.pivotxr.enabled ? 'On' : 'Off' }}
          </label>
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

        <PivotActivationEditor
          v-model:activation-mode="config.modules.pivotxr.activationMode"
          v-model:activation-binding="config.modules.pivotxr.activationBinding"
          class="mb-3 mt-3"
          description="Choose how the default Pivot profile activates when no custom profile targets the current application."
        />

        <PivotSettingsFields :settings="config.modules.pivotxr.defaults" />
      </details>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Override Pivot per application. The first enabled matching profile wins.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addPivotProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.pivotxr.profiles"
        :key="`pivot-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Pivot"
        warning-title="Binding conflict"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removePivotProfile', index)"
        @sync-name="$emit('syncPivotProfileName', index)"
      >
        <PivotActivationEditor
          v-model:activation-mode="profile.activationMode"
          v-model:activation-binding="profile.activationBinding"
          description="Choose a keyboard shortcut or capture a joystick button for this profile."
        />

        <PivotSettingsFields class="mt-3" :settings="profile.settings" />
      </ProfileShell>

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
  </div>
</template>
