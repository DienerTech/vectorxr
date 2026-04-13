<script setup lang="ts">
import { ref } from 'vue'

import EffectField from './EffectField.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'
import type { DepthXRProfileConfig, RegisteredApplication } from '../lib/model'

defineProps<{
  index: number
  profile: DepthXRProfileConfig
  applications: RegisteredApplication[]
  warnings?: string[]
}>()

defineEmits<{
  remove: []
  syncName: []
}>()

const editingName = ref(false)

function finishNameEdit() {
  editingName.value = false
}
</script>

<template>
  <article
    class="rounded-[1rem] border p-5 shadow-panel transition"
    :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft'"
  >
    <div class="mb-4 space-y-2">
      <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profile {{ index + 1 }}</p>
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div class="flex flex-wrap items-center gap-2">
          <h3 v-if="!editingName" class="text-xl font-semibold tracking-tight">{{ profile.name }}</h3>
          <input
            v-else
            v-model="profile.name"
            class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-xl font-semibold tracking-tight"
            placeholder="DCS"
            type="text"
            @blur="finishNameEdit"
            @keydown.enter="finishNameEdit"
          />
          <button
            v-if="!editingName"
            class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]"
            type="button"
            aria-label="Edit profile name"
            @click="editingName = true"
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
          @click="$emit('remove')"
        >
          Remove
        </button>
      </div>
    </div>

    <div
      v-if="warnings && warnings.length > 0"
      class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
    >
      <p class="font-medium">Profile conflict</p>
      <ul class="mt-2 space-y-1">
        <li v-for="warning in warnings" :key="warning">{{ warning }}</li>
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
            <input v-model="profile.applicationIds" class="mt-0.5 h-4 w-4 accent-depthxr-copper" type="checkbox" :value="application.id" @change="$emit('syncName')" />
            <span>
              <span class="block font-medium">{{ application.name }}</span>
              <span class="block font-mono text-xs text-muted">{{ application.match.exe }}</span>
            </span>
          </label>
          </div>

          <p v-if="applications.length === 0" class="text-sm text-muted">Add an application on the Application Registry tab before assigning this profile.</p>
        </div>
      </label>
    </div>

    <div class="mt-4 grid gap-3 lg:grid-cols-2">
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
  </article>
</template>
