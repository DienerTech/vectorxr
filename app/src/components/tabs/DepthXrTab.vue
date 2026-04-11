<script setup lang="ts">
import { computed } from 'vue'

import BindingEditor from '../BindingEditor.vue'
import EffectField from '../EffectField.vue'
import ProfileEditor from '../ProfileEditor.vue'
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
      const message = `${appName} is already targeted by Profile ${firstIndex + 1}. The first enabled profile wins.`
      warnings.set(index, [...(warnings.get(index) ?? []), message])
    }
  })

  return warnings
})
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Depth</p>
          <h2 class="text-2xl font-semibold tracking-tight">Default depth settings</h2>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Depth Enabled
        </label>
      </div>

      <div class="mb-4 rounded-[0.9rem] border border-dashed px-4 py-3 text-sm leading-6 surface-panel-soft">
        Adjust the shared depth defaults here. Profiles below can still override the same values per title while the config keeps the canonical
        runtime numbers.
      </div>

      <BindingEditor
        v-model="config.modules.depthxr.bindings.toggleEnabled"
        class="mb-4"
        label="Depth Toggle Binding"
        description="Toggle Depth on or off at runtime for quick A/B testing."
      />

      <div class="grid gap-3 lg:grid-cols-2">
        <EffectField
          v-model:enabled="config.modules.depthxr.defaults.stereoBoostEnabled"
          v-model:value="config.modules.depthxr.defaults.stereoBoost"
          title="Stereo Boost"
          subtitle="Scales horizontal eye separation around the midpoint."
          :min="0.5"
          :max="2"
          :step="0.01"
          :display-min="-50"
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
          subtitle="Moves the zero-parallax plane by shifting per-eye projection centers."
          :min="-0.5"
          :max="0.5"
          :step="0.001"
          :display-min="-500"
          :display-max="500"
          :display-step="0.1"
          :display-value="toConvergenceDisplay"
          :parse-display-value="fromConvergenceDisplay"
          :display-badge="convergenceBadge"
        />
      </div>
    </article>

    <section class="space-y-4">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profiles</p>
          <h2 class="text-2xl font-semibold tracking-tight">Depth per-game overrides</h2>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileEditor
        v-for="(profile, index) in config.modules.depthxr.profiles"
        :key="`${profile.name}-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeProfile', index)"
        @sync-name="$emit('syncProfileName', index)"
      />

      <div
        v-if="config.modules.depthxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No per-game overrides yet. Add a profile to bind custom depth values to a specific executable.
      </div>
    </section>
  </div>
</template>
