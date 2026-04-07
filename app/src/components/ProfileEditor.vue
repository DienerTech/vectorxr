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
  <article class="rounded-4xl border border-black/10 bg-[#f7f2e8]/90 p-6 shadow-panel">
    <div class="mb-5 flex flex-wrap items-center justify-between gap-3">
      <div>
        <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Profile {{ index + 1 }}</p>
        <h3 class="text-2xl font-semibold tracking-tight text-depthxr-pine">Per-Game Override</h3>
      </div>
      <button
        class="rounded-full border border-black/10 px-4 py-2 text-sm font-medium text-depthxr-ink transition hover:border-depthxr-copper hover:text-depthxr-copper"
        type="button"
        @click="$emit('remove')"
      >
        Remove
      </button>
    </div>

    <div class="grid gap-4 md:grid-cols-3">
      <label class="block">
        <span class="mb-2 block text-sm font-medium">Executable</span>
        <input
          v-model="profile.match.exe"
          class="w-full rounded-2xl border border-black/10 bg-white px-4 py-3"
          placeholder="Game.exe"
          type="text"
        />
      </label>

      <label class="block">
        <span class="mb-2 block text-sm font-medium">Profile Enabled</span>
        <select v-model="profile.enabled" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-3">
          <option :value="true">Enabled</option>
          <option :value="false">Disabled</option>
        </select>
      </label>

      <label class="block">
        <span class="mb-2 block text-sm font-medium">Log Level</span>
        <select v-model="profile.logLevel" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-3">
          <option v-for="level in logLevels" :key="level" :value="level">
            {{ level }}
          </option>
        </select>
      </label>
    </div>

    <div class="mt-5 grid gap-4 xl:grid-cols-3">
      <EffectField
        v-model:enabled="profile.stereoBoostEnabled"
        v-model:value="profile.stereoBoost"
        title="Stereo Boost"
        subtitle="Scales eye separation around the midpoint returned by xrLocateViews."
        :min="0.5"
        :max="2"
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
