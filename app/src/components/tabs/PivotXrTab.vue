<script setup lang="ts">
import { computed, ref } from 'vue'

import PivotActivationEditor from '../PivotActivationEditor.vue'
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

const editingProfileName = ref<number | null>(null)

function finishProfileNameEdit() {
  editingProfileName.value = null
}

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
          const message = `${appName} shares binding ${labelA} with Profile ${b.index + 1}. First enabled profile executes.`
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
  <div class="space-y-6">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Pivot</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Configure how Pivot activates and how much extra yaw or pitch it can add for each title. Use profiles to keep rotation behavior specific to the games and simulators that benefit from it.
          </p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Pivot Enabled
        </label>
      </div>

      <p class="eyebrow mb-3 text-xs uppercase tracking-[0.24em]">Default Profile</p>
      <p class="mb-4 text-sm leading-6 text-muted">
        These values apply when no custom profile targets the current application. New profiles are initialized from these defaults.
      </p>

      <PivotActivationEditor
        v-model:activation-mode="config.modules.pivotxr.activationMode"
        v-model:activation-binding="config.modules.pivotxr.activationBinding"
        class="mb-3"
        description="Choose how the default Pivot profile activates when no custom profile targets the current application."
      />

      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Multiplier
              <span title="Amplifies head yaw (left/right) outside the deadzone. 1.0 = no change. Good range: 1.3–2.0." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.rotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1" max="3" step="0.05" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Smoothing
              <span title="Low-pass filter on yaw output. 0 = instant response, higher values feel slower. Good range: 0.05–0.2." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.smoothing"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="1" step="0.01" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Deadzone
              <span title="Degrees of head yaw ignored before amplification activates. Prevents jitter at rest. Good range: 2–8°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.deadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="45" step="0.5" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Max Extra
              <span title="Maximum extra yaw degrees Pivot can add on top of natural head movement. Good range: 20–60°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraYawDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="180" step="0.5" type="number"
            />
          </label>
        </div>
      </div>

      <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch Assist</p>
        <p class="mb-3 mt-2 text-sm leading-6 text-muted">
          Independent pitch controls let you look over the nose without affecting yaw feel.
        </p>
        <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Multiplier
              <span title="Amplifies head pitch (up/down) outside the deadzone. 1.0 = no change. Good range: 1.2–1.8." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchRotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1" max="3" step="0.05" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Smoothing
              <span title="Low-pass filter on pitch output. 0 = instant response, higher values feel slower. Good range: 0.05–0.2." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchSmoothing"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="1" step="0.01" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Deadzone
              <span title="Degrees of head pitch ignored before amplification activates. Good range: 2–8°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchDeadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="45" step="0.5" type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Max Extra
              <span title="Maximum extra pitch degrees Pivot can add. Good range: 10–30°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraPitchDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="180" step="0.5" type="number"
            />
          </label>
        </div>
      </div>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-4">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profiles</p>
          <h2 class="text-2xl font-semibold tracking-tight">Custom Profiles</h2>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addPivotProfile')"
        >
          Add Profile
        </button>
      </div>

      <article
        v-for="(profile, index) in config.modules.pivotxr.profiles"
        :key="`pivot-profile-${index}`"
        class="rounded-[1rem] border p-5 shadow-panel transition"
        :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft'"
      >
        <div class="mb-4 space-y-2">
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profile {{ index + 1 }}</p>
          <div class="flex flex-wrap items-center justify-between gap-3">
            <div class="flex flex-wrap items-center gap-2">
              <h3 v-if="editingProfileName !== index" class="text-xl font-semibold tracking-tight">{{ profile.name }}</h3>
              <input
                v-else
                v-model="profile.name"
                class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-xl font-semibold tracking-tight"
                placeholder="DCS"
                type="text"
                @blur="finishProfileNameEdit"
                @keydown.enter="finishProfileNameEdit"
              />
              <button
                v-if="editingProfileName !== index"
                class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]"
                type="button"
                aria-label="Edit profile name"
                @click="editingProfileName = index"
              >
                <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                  <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
                </svg>
              </button>
              <label class="pill-toggle inline-flex items-center gap-2 rounded-full px-3 py-1.5 text-xs font-medium">
                <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
                {{ profile.enabled ? 'Profile Enabled' : 'Profile Disabled' }}
              </label>
            </div>
            <button
              class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
              type="button"
              @click="$emit('removePivotProfile', index)"
            >
              Remove
            </button>
          </div>
        </div>

        <div
          v-if="profileWarnings.get(index) && profileWarnings.get(index)!.length > 0"
          class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
          style="border-color: var(--app-border)"
        >
          <p class="font-medium">Binding conflict</p>
          <ul class="mt-2 space-y-1">
            <li v-for="warning in profileWarnings.get(index)" :key="warning">{{ warning }}</li>
          </ul>
        </div>

        <div
          v-if="!profile.enabled"
          class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong"
        >
          This profile is disabled and will not activate at runtime.
        </div>

        <div class="grid gap-3">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Applications</span>
            <div class="overflow-x-auto rounded-[0.75rem] border p-3 surface-panel-strong">
              <div class="flex min-w-max gap-3">
              <label
                v-for="application in applications"
                :key="application.id"
                class="flex min-w-[13rem] max-w-[16rem] items-start gap-3 rounded-[0.65rem] border px-3 py-3 text-sm surface-panel-soft"
              >
                <input
                  v-model="profile.applicationIds"
                  class="mt-0.5 h-4 w-4 accent-depthxr-copper"
                  type="checkbox"
                  :value="application.id"
                  @change="$emit('syncPivotProfileName', index)"
                />
                <span>
                  <span class="block font-medium">{{ application.name }}</span>
                  <span class="block font-mono text-xs text-muted">{{ application.match.exe }}</span>
                </span>
              </label>
              </div>
              <p v-if="applications.length === 0" class="text-sm text-muted">
                Add an application on the Application Registry tab before assigning this profile.
              </p>
            </div>
          </label>
        </div>

        <PivotActivationEditor
          v-model:activation-mode="profile.activationMode"
          v-model:activation-binding="profile.activationBinding"
          class="mt-4"
          description="Choose a keyboard shortcut or capture a joystick button for this profile."
        />

        <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
          <div class="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Multiplier
                <span title="Amplifies head yaw (left/right) outside the deadzone. 1.0 = no change. Good range: 1.3–2.0." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.rotationMultiplier"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="1" max="3" step="0.05" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Smoothing
                <span title="Low-pass filter on yaw output. 0 = instant response, higher values feel slower. Good range: 0.05–0.2." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.smoothing"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="1" step="0.01" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Deadzone
                <span title="Degrees of head yaw ignored before amplification activates. Good range: 2–8°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.deadzoneDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="45" step="0.5" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Max Extra
                <span title="Maximum extra yaw degrees Pivot can add. Good range: 20–60°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.maxExtraYawDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="180" step="0.5" type="number"
              />
            </label>
          </div>
        </div>

        <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch Assist</p>
          <div class="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Multiplier
                <span title="Amplifies head pitch (up/down) outside the deadzone. 1.0 = no change. Good range: 1.2–1.8." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.pitchRotationMultiplier"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="1" max="3" step="0.05" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Smoothing
                <span title="Low-pass filter on pitch output. 0 = instant response, higher values feel slower. Good range: 0.05–0.2." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.pitchSmoothing"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="1" step="0.01" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Deadzone
                <span title="Degrees of head pitch ignored before amplification activates. Good range: 2–8°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.pitchDeadzoneDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="45" step="0.5" type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Max Extra
                <span title="Maximum extra pitch degrees Pivot can add. Good range: 10–30°." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input
                v-model.number="profile.settings.maxExtraPitchDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="180" step="0.5" type="number"
              />
            </label>
          </div>
        </div>
      </article>

      <div
        v-if="config.modules.pivotxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to bind Pivot rotation to a specific application.
      </div>
    </section>
  </div>
</template>
