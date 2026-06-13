<script setup lang="ts">
import { computed } from 'vue'

import BindingEditor from '../BindingEditor.vue'
import EffectField from '../EffectField.vue'
import ProfileShell from '../ProfileShell.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../../lib/display'
import type { RegisteredApplication, VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addProfile: []
  removeProfile: [index: number]
  syncProfileName: [index: number]
}>()

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
      const message = `${appName} is already targeted by Profile ${firstIndex + 1}. The first active profile wins.`
      warnings.set(index, [...(warnings.get(index) ?? []), message])
    }
  })

  return warnings
})
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <!-- Module header -->
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Depth</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Tune stereo boost and convergence defaults, then add per-application profiles when a title needs different depth behavior. Start with small value changes and use the runtime toggle binding for quick comparisons in headset.
          </p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Depth Enabled
        </label>
      </div>

      <!-- Module-level binding — applies regardless of which profile is active -->
      <BindingEditor
        v-model="config.modules.depthxr.bindings.toggleEnabled"
        class="mb-5"
        label="Depth Toggle Binding"
        description="Toggle Depth on or off at runtime for quick A/B testing."
      />

      <!-- Default Profile -->
      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to every application without a custom profile</span>
        </summary>
        <div class="mt-3 grid gap-3 lg:grid-cols-2">
          <EffectField
            v-model:enabled="config.modules.depthxr.defaults.stereoBoostEnabled"
            v-model:value="config.modules.depthxr.defaults.stereoBoost"
            title="Stereo Boost"
            subtitle="Scales horizontal eye separation around the midpoint. Start around +5.0 to +10.0 and adjust slowly."
            :min="1"
            :max="2"
            :step="0.01"
            :display-min="0"
            :display-max="100"
            :display-step="0.1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:enabled="config.modules.depthxr.defaults.convergenceEnabled"
            v-model:value="config.modules.depthxr.defaults.convergence"
            title="Convergence"
            subtitle="Moves the zero-parallax plane by shifting per-eye projection centers. Start at 0.0; it can be particularly strong and may not apply in all titles."
            :min="0"
            :max="0.5"
            :step="0.001"
            :display-min="0"
            :display-max="500"
            :display-step="0.1"
            :display-value="toConvergenceDisplay"
            :parse-display-value="fromConvergenceDisplay"
            :display-badge="convergenceBadge"
          />
        </div>
      </details>
    </article>

    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Override Depth per application, or disable it for specific titles.</p>
        </div>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('addProfile')">
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.depthxr.profiles"
        :key="`depth-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Depth"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeProfile', index)"
        @sync-name="$emit('syncProfileName', index)"
      >
        <div class="grid gap-3 lg:grid-cols-2">
          <EffectField
            v-model:enabled="profile.settings.stereoBoostEnabled"
            v-model:value="profile.settings.stereoBoost"
            :muted="!profile.enabled"
            title="Stereo Boost"
            subtitle="Scales horizontal eye separation around the midpoint. Start around +5.0 to +10.0 and adjust slowly."
            :min="1"
            :max="2"
            :step="0.01"
            :display-min="0"
            :display-max="100"
            :display-step="0.1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:enabled="profile.settings.convergenceEnabled"
            v-model:value="profile.settings.convergence"
            :muted="!profile.enabled"
            title="Convergence"
            subtitle="Shifts projection centers to move the zero-parallax plane. Start at 0.0; it can be particularly strong and may not apply in all titles."
            :min="0"
            :max="0.5"
            :step="0.001"
            :display-min="0"
            :display-max="500"
            :display-step="0.1"
            :display-value="toConvergenceDisplay"
            :parse-display-value="fromConvergenceDisplay"
            :display-badge="convergenceBadge"
          />
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.depthxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to override depth values for a specific application, or to turn Depth off for one.
      </div>
    </section>
  </div>
</template>
