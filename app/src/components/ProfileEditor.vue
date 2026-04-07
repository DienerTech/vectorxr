<script setup lang="ts">
import EffectField from './EffectField.vue'
import type { LogLevel, ProfileConfig } from '../lib/model'

defineProps<{
  index: number
  profile: ProfileConfig
}>()

defineEmits<{
  remove: []
}>()

const logLevels: LogLevel[] = ['off', 'error', 'info', 'debug']
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

    <div class="grid gap-3 lg:grid-cols-[minmax(0,1.3fr)_minmax(0,0.8fr)_minmax(0,0.8fr)]">
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Executable</span>
        <input
          v-model="profile.match.exe"
          class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
          placeholder="Game.exe"
          type="text"
        />
      </label>

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Profile Enabled</span>
        <select v-model="profile.enabled" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5">
          <option :value="true">Enabled</option>
          <option :value="false">Disabled</option>
        </select>
      </label>

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Log Level</span>
        <select v-model="profile.logLevel" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5">
          <option v-for="level in logLevels" :key="level" :value="level">
            {{ level }}
          </option>
        </select>
      </label>
    </div>

    <div class="mt-4 grid gap-3 lg:grid-cols-2 2xl:grid-cols-4">
      <EffectField
        v-model:enabled="profile.stereoBoostEnabled"
        v-model:value="profile.stereoBoost"
        title="Stereo Boost"
        subtitle="Scales horizontal eye separation around the midpoint."
        :min="0.5"
        :max="2"
        :step="0.01"
      />
      <EffectField
        v-model:enabled="profile.convergenceEnabled"
        v-model:value="profile.convergence"
        title="Convergence"
        subtitle="Shifts projection centers to move the zero-parallax plane."
        :min="-0.5"
        :max="0.5"
        :step="0.01"
      />
      <EffectField
        v-model:enabled="profile.worldScaleEnabled"
        v-model:value="profile.worldScale"
        title="World Scale"
        subtitle="First-pass view-space translation scale for experimentation."
        :min="0.5"
        :max="2"
        :step="0.01"
      />
      <EffectField
        v-model:enabled="profile.fovScaleEnabled"
        v-model:value="profile.fovScale"
        title="FoV Scale"
        subtitle="Scales tangent-space FoV angles while preserving sign."
        :min="0.5"
        :max="1.5"
        :step="0.01"
      />
    </div>
  </article>
</template>
