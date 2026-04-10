<script setup lang="ts">
import { pivotActivationKeyGroups, type VectorXRConfig } from '../../lib/model'

defineProps<{
  config: VectorXRConfig
}>()
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[2rem] border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">PivotXR</p>
          <h2 class="text-2xl font-semibold tracking-tight text-depthxr-pine">Rotation Module Setup</h2>
        </div>
        <label class="inline-flex items-center gap-3 rounded-full bg-depthxr-pine px-4 py-2 text-sm font-medium text-white">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          PivotXR Enabled
        </label>
      </div>

      <div class="rounded-2xl border border-dashed border-black/10 bg-[#f7f2e8] px-4 py-3 text-sm leading-6 text-depthxr-steel">
        PivotXR now runs as an experimental runtime spike. When the module is enabled, the activation key controls when the extra pivot factor is actually applied.
      </div>

      <div class="mt-4 grid gap-3 lg:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Activation Mode</span>
          <select v-model="config.modules.pivotxr.defaults.activationMode" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5">
            <option value="toggle">toggle</option>
            <option value="hold">hold</option>
          </select>
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Activation Key</span>
          <select v-model="config.modules.pivotxr.defaults.activationKey" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5">
            <optgroup v-for="group in pivotActivationKeyGroups" :key="group.label" :label="group.label">
              <option v-for="option in group.options" :key="option" :value="option">{{ option }}</option>
            </optgroup>
          </select>
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Multiplier</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.rotationMultiplier"
            class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
            min="1"
            max="3"
            step="0.05"
            type="number"
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Smoothing</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.smoothing"
            class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
            min="0"
            max="1"
            step="0.01"
            type="number"
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Deadzone</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.deadzoneDegrees"
            class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
            min="0"
            max="45"
            step="0.5"
            type="number"
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Max Extra</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.maxExtraYawDegrees"
            class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
            min="0"
            max="45"
            step="0.5"
            type="number"
          />
        </label>
      </div>

      <div class="mt-5 rounded-2xl border border-black/10 bg-[#fbf7ef] p-4">
        <p class="text-xs uppercase tracking-[0.18em] text-depthxr-copper">Pitch Assist</p>
        <p class="mt-2 text-sm leading-6 text-depthxr-steel">
          Pitch has its own multiplier, deadzone, smoothing, and clamp so you can look higher over the nose without over-driving the yaw feel you already like.
        </p>

        <div class="mt-4 grid gap-3 lg:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Multiplier</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchRotationMultiplier"
              class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
              min="1"
              max="3"
              step="0.05"
              type="number"
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Smoothing</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchSmoothing"
              class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
              min="0"
              max="1"
              step="0.01"
              type="number"
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Deadzone</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchDeadzoneDegrees"
              class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
              min="0"
              max="45"
              step="0.5"
              type="number"
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Max Extra</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraPitchDegrees"
              class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
              min="0"
              max="45"
              step="0.5"
              type="number"
            />
          </label>
        </div>
      </div>
    </article>

    <article class="rounded-[2rem] border border-black/10 bg-[#24322d] p-5 text-white shadow-panel">
      <p class="text-xs uppercase tracking-[0.24em] text-depthxr-sand">Compatibility Notes</p>
      <ul class="mt-3 space-y-2.5 text-sm leading-6 text-white/78">
        <li>The current runtime path uses `xrLocateSpace`, `xrLocateViews`, and `xrEndFrame` together so DCS can tolerate the extra pivot without the old black-mask failure.</li>
        <li>`toggle` starts inactive and flips the extra yaw factor on each press. `hold` only applies the extra factor while the activation key is down.</li>
        <li>Smoothing is time-based now, so the response should stay closer to the same feel even when frame rate changes.</li>
        <li>The first activation pass still uses desktop keyboard polling with a focused key list, which is good enough for tuning but not yet the final smart-activation story.</li>
      </ul>
    </article>
  </div>
</template>
