<script setup lang="ts">
import { computed } from 'vue'

import type { PivotXRSettings } from '../lib/model'

// Compact read-only summary of a profile's rotation settings. Editing happens
// on the dedicated settings sub-page (see PivotSettingsPage), opened via `edit`.
const props = defineProps<{
  settings: PivotXRSettings
}>()

defineEmits<{
  edit: []
}>()

const chips = computed(() => {
  const settings = props.settings
  const result: Array<{ label: string; value: string; title: string }> = []

  if (settings.responseMode === 'stepped') {
    if (settings.advancedAxes) {
      result.push({
        label: 'Stepped · Advanced',
        value: '4 directions',
        title: `Independent yaw-left, yaw-right, pitch-up, and pitch-down step tuning.`,
      })
    } else {
      result.push({
        label: 'Stepped',
        value: `Y ${settings.yawStep.triggerDegrees}° → +${settings.yawStep.amountDegrees}° · P ${settings.pitchStep.triggerDegrees}° → +${settings.pitchStep.amountDegrees}°`,
        title: `Yaw and pitch use independent trigger, amount, hysteresis, deadzone, and maximum values.`,
      })
    }
    result.push({
      label: 'Step Glide',
      value: settings.stepGlideMode === 'instant' ? 'Instant' : `${settings.stepGlideSeconds}s`,
      title: settings.stepGlideMode === 'instant'
        ? 'Each step is applied immediately.'
        : `Each step reaches its target in ${settings.stepGlideSeconds}s without overshoot.`,
    })
  } else if (settings.advancedAxes) {
    result.push({
      label: 'Advanced axes',
      value: `×${settings.yawLeft.rotationMultiplier}/${settings.yawRight.rotationMultiplier} · ×${settings.pitchUp.rotationMultiplier}/${settings.pitchDown.rotationMultiplier}`,
      title: `Per-direction multipliers — yaw left ×${settings.yawLeft.rotationMultiplier}, yaw right ×${settings.yawRight.rotationMultiplier}, pitch up ×${settings.pitchUp.rotationMultiplier}, pitch down ×${settings.pitchDown.rotationMultiplier}.`,
    })
  } else {
    result.push({
      label: 'Yaw',
      value: `×${settings.rotationMultiplier}`,
      title: `Yaw multiplier ×${settings.rotationMultiplier}, deadzone ${settings.deadzoneDegrees}°, max extra ${settings.maxExtraYawDegrees}°.`,
    })
    result.push({
      label: 'Pitch',
      value: `×${settings.pitchRotationMultiplier}`,
      title: `Pitch multiplier ×${settings.pitchRotationMultiplier}, deadzone ${settings.pitchDeadzoneDegrees}°, max extra ${settings.maxExtraPitchDegrees}°.`,
    })
  }

  result.push({
    label: settings.responseMode === 'stepped' ? 'Activation Ramp' : 'Smoothing',
    value: settings.responseMode === 'stepped' ? `${settings.activationRampSeconds}s` : `${settings.smoothing}`,
    title: settings.responseMode === 'stepped' ? 'Pivot activation and release ramp.' : `Tracking smoothing ${settings.smoothing}; activation ramp ${settings.activationRampSeconds}s.`,
  })

  return result
})
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <!-- items-start + shrink-0 keep the edit button pinned top-right, aligned
         with the bindings row's button regardless of chip wrapping. -->
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Motion</p>
        <span
          v-for="chip in chips"
          :key="chip.label"
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          :title="chip.title"
        >
          <span class="font-semibold">{{ chip.label }}</span>
          <span class="text-muted">{{ chip.value }}</span>
        </span>
      </div>

      <button class="button-secondary w-40 shrink-0 rounded-[0.75rem] px-4 py-2 text-center text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Motion…
      </button>
    </div>
  </div>
</template>
