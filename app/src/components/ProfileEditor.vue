<script setup lang="ts">
import EffectField from './EffectField.vue'
import type { DepthXRProfileConfig } from '../lib/model'

defineProps<{
  index: number
  profile: DepthXRProfileConfig
}>()

defineEmits<{
  remove: []
  syncName: []
}>()
</script>

<template>
  <article class="rounded-[2rem] border border-black/10 bg-[#f7f2e8]/90 p-5 shadow-panel">
    <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
      <div>
        <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Profile {{ index + 1 }}</p>
        <h3 class="text-xl font-semibold tracking-tight text-depthxr-pine">Per-Game Override</h3>
      </div>
      <button
        class="rounded-full border border-black/10 px-4 py-2 text-sm font-medium text-depthxr-ink transition hover:border-depthxr-copper hover:text-depthxr-copper"
        type="button"
        @click="$emit('remove')"
      >
        Remove
      </button>
    </div>

    <div class="grid gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Profile Name</span>
        <input
          v-model="profile.name"
          class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
          placeholder="DCS"
          type="text"
        />
      </label>

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Executable</span>
        <input
          v-model="profile.match.exe"
          class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
          placeholder="Game.exe"
          type="text"
          @blur="$emit('syncName')"
        />
      </label>
    </div>

    <label class="mt-4 inline-flex items-center gap-3 rounded-full bg-depthxr-pine px-4 py-2 text-sm font-medium text-white">
      <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
      Profile Enabled
    </label>

    <div class="mt-4 grid gap-3 lg:grid-cols-2">
      <EffectField
        v-model:enabled="profile.settings.stereoBoostEnabled"
        v-model:value="profile.settings.stereoBoost"
        title="Stereo Boost"
        subtitle="Scales horizontal eye separation around the midpoint."
        :min="0.5"
        :max="2"
        :step="0.01"
      />
      <EffectField
        v-model:enabled="profile.settings.convergenceEnabled"
        v-model:value="profile.settings.convergence"
        title="Convergence"
        subtitle="Shifts projection centers to move the zero-parallax plane."
        :min="-0.5"
        :max="0.5"
        :step="0.001"
      />
    </div>
  </article>
</template>
