<script setup lang="ts">
import type { PivotXRSettings } from '../lib/model'

// Shared Pivot settings editor used by both the Default Profile and each custom
// profile, so the two stay visually and structurally identical. Fields mutate
// the passed-in reactive `settings` object directly.
defineProps<{
  settings: PivotXRSettings
}>()
</script>

<template>
  <div class="grid gap-3 lg:grid-cols-3">
    <!-- General: applies to both axes -->
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">General</p>
      <div class="mt-3 grid gap-3">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Smoothing
            <span
              title="Low-pass filter on pivot response for both yaw and pitch. 0 = instant, higher = smoother but laggier. Good range: 0.05–0.2."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.smoothing"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="1"
            step="0.01"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Activation Ramp
            <span
              title="Seconds to ease the pivot effect in and out when you toggle it on or off, so activating it never snaps the view. 0 = instant. Default 0.35."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.activationRampSeconds"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="2"
            step="0.05"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">seconds</span>
        </label>
      </div>
    </div>

    <!-- Yaw -->
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
      <div class="mt-3 grid gap-3">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Multiplier
            <span
              title="Amplifies head yaw (left/right) beyond the deadzone. 1.0 = no change. 1.3–2.0 is typical; up to 3 helps look further or behind you depending on your headset and head-tracking range."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.rotationMultiplier"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="1"
            max="3"
            step="0.05"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Deadzone
            <span
              title="Degrees of head yaw ignored before amplification kicks in. Prevents jitter when looking straight ahead. 2–8° is typical."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.deadzoneDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="45"
            step="0.5"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">degrees</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Max Extra
            <span
              title="Cap on the extra yaw degrees pivot can add on top of natural head movement. 180° lets you look directly behind you."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.maxExtraYawDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="180"
            step="0.5"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">degrees</span>
        </label>
      </div>
    </div>

    <!-- Pitch -->
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch</p>
      <div class="mt-3 grid gap-3">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Multiplier
            <span
              title="Amplifies head pitch (up/down) beyond the deadzone. 1.0 = no change. 1.2–2.0 is typical; up to 3 helps look over the nose or up at the canopy."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.pitchRotationMultiplier"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="1"
            max="3"
            step="0.05"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Deadzone
            <span
              title="Degrees of head pitch ignored before amplification kicks in. 2–12° is typical."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.pitchDeadzoneDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="45"
            step="0.5"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">degrees</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Max Extra
            <span
              title="Cap on the extra pitch degrees pivot can add. Useful for looking over the nose or up at the canopy without craning your neck."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.maxExtraPitchDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="180"
            step="0.5"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">degrees</span>
        </label>
      </div>
    </div>
  </div>
</template>
