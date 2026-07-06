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
    result.push({
      label: 'Stepped',
      value: `${settings.stepTriggerDegrees}° → +${settings.stepAmountDegrees}°`,
      title: `Every ${settings.stepTriggerDegrees}° of head rotation past the deadzone adds ${settings.stepAmountDegrees}° of view rotation (hysteresis ${settings.stepHysteresisDegrees}°).`,
    })
    result.push({
      label: 'Deadzone',
      value: `${settings.deadzoneDegrees}° / ${settings.pitchDeadzoneDegrees}°`,
      title: `Yaw ${settings.deadzoneDegrees}°, pitch ${settings.pitchDeadzoneDegrees}° before the first step threshold.`,
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
    label: 'Smoothing',
    value: `${settings.smoothing}`,
    title: `Tracking smoothing ${settings.smoothing}; activation ramp ${settings.activationRampSeconds}s.`,
  })

  return result
})
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="flex min-w-0 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Rotation</p>
        <span
          v-for="chip in chips"
          :key="chip.label"
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs font-medium"
          style="border-color: var(--app-border)"
          :title="chip.title"
        >
          <span class="text-muted">{{ chip.label }}</span>
          {{ chip.value }}
        </span>
      </div>

      <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Settings…
      </button>
    </div>
  </div>
</template>
