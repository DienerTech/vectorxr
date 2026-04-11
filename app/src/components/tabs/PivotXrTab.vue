<script setup lang="ts">
import { computed } from 'vue'

import BindingEditor from '../BindingEditor.vue'
import type { PivotXRProfileConfig, RegisteredApplication, VectorXRConfig } from '../../lib/model'
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
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Pivot</p>
          <h2 class="text-2xl font-semibold tracking-tight">Default rotation settings</h2>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Pivot Enabled
        </label>
      </div>

      <div class="mb-4 rounded-[0.9rem] border border-dashed px-4 py-3 text-sm leading-6 surface-panel-soft">
        These values are copied into new profiles as a starting point. Pivot activates at runtime only when an enabled profile targets the current application.
      </div>

      <div class="mt-4 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
        <div class="mt-4 grid gap-3 lg:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Yaw Multiplier</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.rotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1" max="3" step="0.05" type="number"
              title="Scales yaw movement outside the deadzone."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Yaw Smoothing</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.smoothing"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="1" step="0.01" type="number"
              title="Smooths yaw response over time. Higher values feel slower."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Yaw Deadzone</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.deadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="45" step="0.5" type="number"
              title="Yaw movement inside this angle remains unchanged."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Yaw Max Extra</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraYawDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="180" step="0.5" type="number"
              title="Maximum extra yaw Pivot can add."
            />
          </label>
        </div>
      </div>

      <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch Assist</p>
        <p class="mt-2 text-sm leading-6 text-muted">
          Pitch has its own multiplier, deadzone, smoothing, and clamp so you can look higher over the nose without over-driving the yaw feel you already like.
        </p>
        <div class="mt-4 grid gap-3 lg:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Multiplier</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchRotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1" max="3" step="0.05" type="number"
              title="Scales pitch movement outside the deadzone."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Smoothing</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchSmoothing"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="1" step="0.01" type="number"
              title="Smooths pitch response over time. Higher values feel slower."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Deadzone</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchDeadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="45" step="0.5" type="number"
              title="Pitch movement inside this angle remains unchanged."
            />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Max Extra</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraPitchDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0" max="180" step="0.5" type="number"
              title="Maximum extra pitch Pivot can add."
            />
          </label>
        </div>
      </div>
    </article>

    <!-- Profile list -->
    <section class="space-y-4">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profiles</p>
          <h2 class="text-2xl font-semibold tracking-tight">Pivot per-game overrides</h2>
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
        :key="`${profile.name}-${index}`"
        class="rounded-[1rem] border p-5 shadow-panel transition"
        :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft'"
      >
        <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
          <div>
            <div class="flex flex-wrap items-center gap-2">
              <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profile {{ index + 1 }}</p>
              <span
                class="rounded-full px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em]"
                :class="profile.enabled ? 'chip-success' : 'chip-idle'"
              >
                {{ profile.enabled ? 'Active' : 'Disabled' }}
              </span>
            </div>
            <h3 class="mt-2 text-xl font-semibold tracking-tight">{{ profile.name }}</h3>
          </div>
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="$emit('removePivotProfile', index)"
          >
            Remove
          </button>
        </div>

        <!-- Conflict warning -->
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

        <!-- Name + applications -->
        <div class="grid gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Profile Name</span>
            <input
              v-model="profile.name"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              placeholder="DCS"
              type="text"
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Applications</span>
            <div class="rounded-[0.75rem] border p-3 surface-panel-strong">
              <label
                v-for="application in applications"
                :key="application.id"
                class="flex items-start gap-3 rounded-[0.65rem] px-2 py-2 text-sm"
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
              <p v-if="applications.length === 0" class="text-sm text-muted">
                Add an application on the Home tab before assigning this profile.
              </p>
            </div>
          </label>
        </div>

        <label class="pill-toggle mt-4 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Profile Enabled
        </label>

        <!-- Activation -->
        <div class="mt-4 rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Activation</p>
          <div class="mt-4 grid gap-3 lg:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Activation Mode</span>
              <select
                v-model="profile.activationMode"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                title="Toggle flips Pivot on each binding press. Hold only applies Pivot while the binding is down."
              >
                <option value="toggle">toggle</option>
                <option value="hold">hold</option>
              </select>
            </label>

            <BindingEditor
              v-model="profile.activationBinding"
              label="Activation Binding"
              description="Keyboard chords work today. Device bindings are saved and available when HOTAS runtime support lands."
            />
          </div>
        </div>

        <!-- Yaw settings -->
        <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
          <div class="mt-4 grid gap-3 lg:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Yaw Multiplier</span>
              <input
                v-model.number="profile.settings.rotationMultiplier"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="1" max="3" step="0.05" type="number"
                title="Scales yaw movement outside the deadzone."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Yaw Smoothing</span>
              <input
                v-model.number="profile.settings.smoothing"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="1" step="0.01" type="number"
                title="Smooths yaw response over time. Higher values feel slower."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Yaw Deadzone</span>
              <input
                v-model.number="profile.settings.deadzoneDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="45" step="0.5" type="number"
                title="Yaw movement inside this angle remains unchanged."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Yaw Max Extra</span>
              <input
                v-model.number="profile.settings.maxExtraYawDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="180" step="0.5" type="number"
                title="Maximum extra yaw Pivot can add."
              />
            </label>
          </div>
        </div>

        <!-- Pitch settings -->
        <div class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch Assist</p>
          <p class="mt-2 text-sm leading-6 text-muted">
            Pitch has its own multiplier, deadzone, smoothing, and clamp so you can look higher over the nose without over-driving the yaw feel.
          </p>
          <div class="mt-4 grid gap-3 lg:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Pitch Multiplier</span>
              <input
                v-model.number="profile.settings.pitchRotationMultiplier"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="1" max="3" step="0.05" type="number"
                title="Scales pitch movement outside the deadzone."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Pitch Smoothing</span>
              <input
                v-model.number="profile.settings.pitchSmoothing"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="1" step="0.01" type="number"
                title="Smooths pitch response over time. Higher values feel slower."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Pitch Deadzone</span>
              <input
                v-model.number="profile.settings.pitchDeadzoneDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="45" step="0.5" type="number"
                title="Pitch movement inside this angle remains unchanged."
              />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Pitch Max Extra</span>
              <input
                v-model.number="profile.settings.maxExtraPitchDegrees"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="0" max="180" step="0.5" type="number"
                title="Maximum extra pitch Pivot can add."
              />
            </label>
          </div>
        </div>
      </article>

      <div
        v-if="config.modules.pivotxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No per-game profiles yet. Add a profile to bind Pivot rotation to a specific application.
      </div>
    </section>
  </div>
</template>
