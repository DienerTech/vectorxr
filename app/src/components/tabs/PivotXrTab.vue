<script setup lang="ts">
import BindingEditor from '../BindingEditor.vue'
import type { VectorXRConfig } from '../../lib/model'

defineProps<{
  config: VectorXRConfig
}>()
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Pivot</p>
          <h2 class="text-2xl font-semibold tracking-tight">Rotation module setup</h2>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.pivotxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Pivot Enabled
        </label>
      </div>

      <div class="rounded-[0.9rem] border border-dashed px-4 py-3 text-sm leading-6 surface-panel-soft">
        Tune how much extra yaw and pitch headroom the module can add. The activation binding controls when the extra rotation factor is applied.
      </div>

      <div class="mt-4 grid gap-3 lg:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Activation Mode</span>
          <select
            v-model="config.modules.pivotxr.defaults.activationMode"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            title="Toggle flips Pivot on each binding press. Hold only applies Pivot while the binding is down."
          >
            <option value="toggle">toggle</option>
            <option value="hold">hold</option>
          </select>
        </label>

        <BindingEditor
          v-model="config.modules.pivotxr.defaults.activationBinding"
          label="Activation Binding"
          description="Keyboard chords work in the runtime today. Device GUID bindings are saved for the upcoming HOTAS runtime pass."
        />

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Multiplier</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.rotationMultiplier"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="1"
            max="3"
            step="0.05"
            type="number"
            title="Scales yaw movement outside the deadzone."
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Smoothing</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.smoothing"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="1"
            step="0.01"
            type="number"
            title="Smooths yaw response over time. Higher values feel slower."
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Deadzone</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.deadzoneDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="45"
            step="0.5"
            type="number"
            title="Yaw movement inside this angle remains unchanged."
          />
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Yaw Max Extra</span>
          <input
            v-model.number="config.modules.pivotxr.defaults.maxExtraYawDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="180"
            step="0.5"
            type="number"
            title="Maximum extra yaw Pivot can add."
          />
        </label>
      </div>

      <div class="mt-5 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch Assist</p>
        <p class="mt-2 text-sm leading-6 text-muted">
          Pitch has its own multiplier, deadzone, smoothing, and clamp so you can look higher over the nose without over-driving the yaw feel you already like.
        </p>

        <div class="mt-4 grid gap-3 lg:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Multiplier</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchRotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1"
              max="3"
              step="0.05"
              type="number"
              title="Scales pitch movement outside the deadzone."
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Smoothing</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchSmoothing"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="1"
              step="0.01"
              type="number"
              title="Smooths pitch response over time. Higher values feel slower."
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Deadzone</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.pitchDeadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="45"
              step="0.5"
              type="number"
              title="Pitch movement inside this angle remains unchanged."
            />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Pitch Max Extra</span>
            <input
              v-model.number="config.modules.pivotxr.defaults.maxExtraPitchDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
              title="Maximum extra pitch Pivot can add."
            />
          </label>
        </div>
      </div>
    </article>

    <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel-inverse">
      <p class="text-xs uppercase tracking-[0.24em] text-inverse-muted">Usage Notes</p>
      <ul class="mt-3 space-y-2.5 text-sm leading-6 text-inverse-muted">
        <li>`toggle` starts inactive and flips the extra rotation factor on each press.</li>
        <li>`hold` only applies the extra factor while the activation binding is down.</li>
        <li>Smoothing is time-based, so the feel stays closer to the same across frame rates.</li>
        <li>Small changes usually tune faster than large jumps, especially around deadzone and max extra limits.</li>
      </ul>
    </article>
  </div>
</template>
