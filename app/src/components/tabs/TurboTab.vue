<script setup lang="ts">
import { ref } from 'vue'

import BindingEditor from '../BindingEditor.vue'
import ProfileShell from '../ProfileShell.vue'
import type { RegisteredApplication, VectorXRConfig } from '../../lib/model'

defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addTurboProfile: []
  removeTurboProfile: [index: number]
  syncTurboProfileName: [index: number]
}>()

const howItWorksOpen = ref(false)
</script>

<template>
  <div class="space-y-4">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Turbo</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Frees your game from the headset runtime's frame pacing so the GPU sets the pace. On some systems the runtime throttles frames at an unfortunate moment, capping fps well below what the hardware can deliver — Turbo removes that wait. If your fps is already stable, Turbo changes nothing worth having; it is a targeted fix, not a general boost. Note: switching Turbo on mid-session can cause a brief (~1 second) hitch while the frame pipeline re-synchronizes.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          How Turbo Works
        </button>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>

        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.turbo.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.turbo.enabled ? 'On' : 'Off' }}
        </label>

        <BindingEditor
          :model-value="config.modules.turbo.toggleBinding"
          class="mt-3"
          label="Turbo Toggle (optional)"
          description="Flip Turbo on and off while in-game to compare fps and frame feel directly. Turbo starts enabled whenever it applies to the running application."
          none-text="No binding assigned. Turbo stays on for applications it applies to."
          @update:model-value="config.modules.turbo.toggleBinding = $event"
        />
      </details>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Enable Turbo for specific applications while leaving the default off.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addTurboProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.turbo.profiles"
        :key="profile.id"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Turbo"
        @remove="$emit('removeTurboProfile', index)"
        @sync-name="$emit('syncTurboProfileName', index)"
      >
        <div class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          Turbo is on for this profile's applications. There is nothing further to tune — pacing is either overridden or it isn't.
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.turbo.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to enable Turbo for a specific application.
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Turbo Mode</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">How Turbo Works</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Normally, an OpenXR game must wait for the headset runtime to say "start the next frame now." Some runtime and GPU combinations time that signal badly, forcing the game to idle and capping fps below what the hardware can do.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            With Turbo, VectorXR performs that wait in the background and lets the game begin its next frame immediately after submitting the previous one — allowing one frame of pipelining. The headset runtime still receives a fully correct frame sequence; only the game's pacing is decoupled.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Trade-offs: frame timing prediction becomes slightly less accurate, which can show up as marginally increased latency or judder in some titles. Turning Turbo on mid-session can occasionally cause a one-second hitch while the frame pipeline re-synchronizes.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Turbo works alongside all of VectorXR's other enhancements, including the Varjo quadviews compatibility path — VectorXR sequences its frame handling so the two never conflict.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Tip: assign the optional Turbo Toggle binding and flip it in-game while watching an fps counter. If Turbo does not clearly help a title, leave it off there.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
