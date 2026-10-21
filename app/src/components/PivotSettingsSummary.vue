<script setup lang="ts">
import { computed } from 'vue'

import { t } from '../i18n/index.ts'
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
        label: t('pivot.summary.steppedAdvanced'),
        value: t('pivot.summary.fourDirections'),
        title: t('pivot.summary.steppedAdvancedTitle'),
      })
    } else {
      result.push({
        label: t('pivot.summary.stepped'),
        value: t('pivot.summary.steppedValue', {
          yawTrigger: settings.yawStep.triggerDegrees,
          yawAmount: settings.yawStep.amountDegrees,
          pitchTrigger: settings.pitchStep.triggerDegrees,
          pitchAmount: settings.pitchStep.amountDegrees,
        }),
        title: t('pivot.summary.steppedTitle'),
      })
    }
    result.push({
      label: t('pivot.summary.stepGlide'),
      value: settings.stepGlideMode === 'instant' ? t('pivot.summary.instant') : `${settings.stepGlideSeconds}s`,
      title: settings.stepGlideMode === 'instant'
        ? t('pivot.summary.stepInstantTitle')
        : t('pivot.summary.stepGlideTitle', { seconds: settings.stepGlideSeconds }),
    })
  } else if (settings.advancedAxes) {
    result.push({
      label: t('pivot.summary.advancedAxes'),
      value: `×${settings.yawLeft.rotationMultiplier}/${settings.yawRight.rotationMultiplier} · ×${settings.pitchUp.rotationMultiplier}/${settings.pitchDown.rotationMultiplier}`,
      title: t('pivot.summary.advancedAxesTitle', {
        yawLeft: settings.yawLeft.rotationMultiplier,
        yawRight: settings.yawRight.rotationMultiplier,
        pitchUp: settings.pitchUp.rotationMultiplier,
        pitchDown: settings.pitchDown.rotationMultiplier,
      }),
    })
  } else {
    result.push({
      label: t('pivot.summary.yaw'),
      value: `×${settings.rotationMultiplier}`,
      title: t('pivot.summary.yawTitle', {
        multiplier: settings.rotationMultiplier,
        deadzone: settings.deadzoneDegrees,
        maxExtra: settings.maxExtraYawDegrees,
      }),
    })
    result.push({
      label: t('pivot.summary.pitch'),
      value: `×${settings.pitchRotationMultiplier}`,
      title: t('pivot.summary.pitchTitle', {
        multiplier: settings.pitchRotationMultiplier,
        deadzone: settings.pitchDeadzoneDegrees,
        maxExtra: settings.maxExtraPitchDegrees,
      }),
    })
  }

  result.push({
    label: settings.responseMode === 'stepped' ? t('pivot.summary.activationRamp') : t('pivot.summary.smoothing'),
    value: settings.responseMode === 'stepped' ? `${settings.activationRampSeconds}s` : `${settings.smoothing}`,
    title: settings.responseMode === 'stepped'
      ? t('pivot.summary.activationRampTitle')
      : t('pivot.summary.smoothingTitle', { smoothing: settings.smoothing, ramp: settings.activationRampSeconds }),
  })

  if (settings.positionAssist.enabled) {
    result.push({ label: t('pivot.summary.positionAssist'), value: t('common.on'), title: t('pivot.summary.positionAssistTitle') })
  }
  if (settings.responseMode === 'continuous') {
    result.push({
      label: t('pivot.summary.deadzoneTransition'),
      value: settings.advancedAxes
        ? t('pivot.summary.perDirection')
        : t('pivot.summary.deadzoneTransitionValue', { yaw: settings.deadzoneTransitionDegrees, pitch: settings.pitchDeadzoneTransitionDegrees }),
      title: t('pivot.summary.deadzoneTransitionTitle'),
    })
  }
  return result
})
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <!-- items-start + shrink-0 keep the edit button pinned top-right, aligned
         with the bindings row's button regardless of chip wrapping. -->
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">{{ t('pivot.summary.motion') }}</p>
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
        {{ t('pivot.summary.editMotion') }}
      </button>
    </div>
  </div>
</template>
