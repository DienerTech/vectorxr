<script setup lang="ts">
import EffectField from '../EffectField.vue'
import ProfileEditor from '../ProfileEditor.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../../lib/display'
import type { VectorXRConfig } from '../../lib/model'

defineProps<{
  config: VectorXRConfig
}>()

defineEmits<{
  addProfile: []
  removeProfile: [index: number]
  syncProfileName: [index: number]
}>()
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[2rem] border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">DepthXR</p>
          <h2 class="text-2xl font-semibold tracking-tight text-depthxr-pine">Default Module Settings</h2>
        </div>
        <label class="inline-flex items-center gap-3 rounded-full bg-depthxr-pine px-4 py-2 text-sm font-medium text-white">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          DepthXR Enabled
        </label>
      </div>

      <div class="mb-4 rounded-2xl border border-dashed border-black/10 bg-[#f7f2e8] px-4 py-3 text-sm leading-6 text-depthxr-steel">
        Milestone 2 keeps DepthXR focused on stereo boost and convergence while the app shell evolves around it. Display values are now tuned for
        easier sharing and discussion, while the config still stores canonical runtime numbers.
      </div>

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
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Profiles</p>
          <h2 class="text-2xl font-semibold tracking-tight text-depthxr-pine">DepthXR Per-Game Overrides</h2>
        </div>
        <button
          class="rounded-full bg-depthxr-copper px-5 py-2.5 text-sm font-medium text-white transition hover:brightness-110"
          type="button"
          @click="$emit('addProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileEditor
        v-for="(profile, index) in config.modules.depthxr.profiles"
        :key="`${profile.match.exe}-${index}`"
        :index="index"
        :profile="profile"
        @remove="$emit('removeProfile', index)"
        @sync-name="$emit('syncProfileName', index)"
      />

      <div
        v-if="config.modules.depthxr.profiles.length === 0"
        class="rounded-[2rem] border border-dashed border-black/15 bg-white/50 px-6 py-7 text-center text-sm text-depthxr-steel"
      >
        No per-game overrides yet. Add a profile to bind custom DepthXR values to a specific executable.
      </div>
    </section>
  </div>
</template>
