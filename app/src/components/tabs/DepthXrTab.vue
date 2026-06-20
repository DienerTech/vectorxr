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
      <div class="mb-4">
        <h2 class="text-2xl font-semibold tracking-tight">Depth</h2>
        <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
          Tune stereo boost and convergence defaults, then add per-application profiles when a title needs different depth behavior. Start with small value changes and use the runtime toggle binding for quick comparisons in headset.
        </p>
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
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>
        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.depthxr.enabled ? 'On' : 'Off' }}
        </label>
        <div class="mt-3 grid gap-3 lg:grid-cols-2">
          <EffectField
            v-model:value="config.modules.depthxr.defaults.stereoBoost"
            title="Stereo Boost"
            subtitle="Widens or narrows the gap between your two eye viewpoints (0% = your real IPD). Positive adds depth and 3D 'pop' and can make the world feel miniaturized; negative flattens depth and makes it feel larger. Start around +10–25%; high values exaggerate parallax and can strain your eyes."
            :min="0"
            :max="2"
            :step="0.01"
            :display-min="-100"
            :display-max="100"
            :display-step="1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:value="config.modules.depthxr.defaults.convergence"
            title="Convergence"
            subtitle="Shifts where your eyes' sightlines cross — the distance that sits at screen depth with no parallax. Positive pulls that plane toward you (near objects pop forward); negative pushes it away (scene settles back, can ease strain). 0 = app default. Use small amounts (±2–10); large values force your eyes to cross and break fusion."
            :min="-0.25"
            :max="0.25"
            :step="0.01"
            :display-min="-25"
            :display-max="25"
            :display-step="1"
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
          <p class="text-sm text-muted">Override Depth per application. The first enabled matching profile wins.</p>
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
            v-model:value="profile.settings.stereoBoost"
            :muted="!profile.enabled"
            title="Stereo Boost"
            subtitle="Widens or narrows the gap between your two eye viewpoints (0% = your real IPD). Positive adds depth and 3D 'pop' and can make the world feel miniaturized; negative flattens depth and makes it feel larger. Start around +10–25%; high values exaggerate parallax and can strain your eyes."
            :min="0"
            :max="2"
            :step="0.01"
            :display-min="-100"
            :display-max="100"
            :display-step="1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:value="profile.settings.convergence"
            :muted="!profile.enabled"
            title="Convergence"
            subtitle="Shifts where your eyes' sightlines cross — the distance that sits at screen depth with no parallax. Positive pulls that plane toward you (near objects pop forward); negative pushes it away (scene settles back, can ease strain). 0 = app default. Use small amounts (±2–10); large values force your eyes to cross and break fusion."
            :min="-0.25"
            :max="0.25"
            :step="0.01"
            :display-min="-25"
            :display-max="25"
            :display-step="1"
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
        No custom profiles yet. Add a profile to override depth values for a specific application.
      </div>
    </section>
  </div>
</template>
