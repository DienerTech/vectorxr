<script setup lang="ts">
import EffectField from './EffectField.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'
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
  <article
    class="rounded-[2rem] border p-5 shadow-panel transition"
    :class="profile.enabled ? 'border-black/10 bg-[#f7f2e8]/90' : 'border-stone-300 bg-stone-100/95'"
  >
    <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
      <div>
        <div class="flex flex-wrap items-center gap-2">
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Profile {{ index + 1 }}</p>
          <span
            class="rounded-full px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em]"
            :class="profile.enabled ? 'bg-emerald-100 text-emerald-700' : 'bg-stone-300 text-stone-700'"
          >
            {{ profile.enabled ? 'Active' : 'Fallback' }}
          </span>
        </div>
        <h3 class="mt-2 text-xl font-semibold tracking-tight text-depthxr-pine">Per-Game Override</h3>
      </div>
      <button
        class="rounded-full border border-black/10 px-4 py-2 text-sm font-medium text-depthxr-ink transition hover:border-depthxr-copper hover:text-depthxr-copper"
        type="button"
        @click="$emit('remove')"
      >
        Remove
      </button>
    </div>

    <div
      v-if="!profile.enabled"
      class="mb-4 rounded-2xl border border-stone-300 bg-white/70 px-4 py-3 text-sm leading-6 text-depthxr-steel"
    >
      This profile is disabled, so VectorXR will fall back to the module defaults at runtime. The settings below remain editable for later reuse.
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
