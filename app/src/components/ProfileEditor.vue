<script setup lang="ts">
import EffectField from './EffectField.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'
import type { DepthXRProfileConfig, RegisteredApplication } from '../lib/model'

defineProps<{
  index: number
  profile: DepthXRProfileConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  remove: []
  syncName: []
}>()
</script>

<template>
  <article
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
            {{ profile.enabled ? 'Active' : 'Fallback' }}
          </span>
        </div>
        <h3 class="mt-2 text-xl font-semibold tracking-tight">Per-game override</h3>
      </div>
      <button
        class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
        type="button"
        @click="$emit('remove')"
      >
        Remove
      </button>
    </div>

    <div
      v-if="!profile.enabled"
      class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong"
    >
      This profile is disabled, so VectorXR will fall back to the module defaults at runtime. The settings below remain editable for later reuse.
    </div>

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
            <input v-model="profile.applicationIds" class="mt-0.5 h-4 w-4 accent-depthxr-copper" type="checkbox" :value="application.id" @change="$emit('syncName')" />
            <span>
              <span class="block font-medium">{{ application.name }}</span>
              <span class="block font-mono text-xs text-muted">{{ application.match.exe }}</span>
            </span>
          </label>

          <p v-if="applications.length === 0" class="text-sm text-muted">Add an application on the Home tab before assigning this profile.</p>
        </div>
      </label>
    </div>

    <label class="pill-toggle mt-4 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
      <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
      Profile Enabled
    </label>

    <div class="mt-4 grid gap-3 lg:grid-cols-2">
      <EffectField
        v-model:enabled="profile.settings.stereoBoostEnabled"
        v-model:value="profile.settings.stereoBoost"
        :muted="!profile.enabled"
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
        v-model:enabled="profile.settings.convergenceEnabled"
        v-model:value="profile.settings.convergence"
        :muted="!profile.enabled"
        title="Convergence"
        subtitle="Shifts projection centers to move the zero-parallax plane."
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
</template>
