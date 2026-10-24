<script setup lang="ts">
import { t } from '../i18n/index.ts'
import PivotStepTuningEditor from './PivotStepTuningEditor.vue'
import type { PivotStepTuning, PivotXRSettings } from '../lib/model'

const props = defineProps<{ settings: PivotXRSettings }>()

const directions = [
  { key: 'yawLeftStep', labelKey: 'pivot.directions.yawLeft', hintKey: 'pivot.stepped.yawLeftHint', deadzoneMax: 180 },
  { key: 'yawRightStep', labelKey: 'pivot.directions.yawRight', hintKey: 'pivot.stepped.yawRightHint', deadzoneMax: 180 },
  { key: 'pitchUpStep', labelKey: 'pivot.directions.pitchUp', hintKey: 'pivot.stepped.pitchUpHint', deadzoneMax: 90 },
  { key: 'pitchDownStep', labelKey: 'pivot.directions.pitchDown', hintKey: 'pivot.stepped.pitchDownHint', deadzoneMax: 90 },
] as const

function copyTuning(tuning: PivotStepTuning): PivotStepTuning {
  return { ...tuning }
}

function onAdvancedAxesChange(enabled: boolean) {
  if (!enabled) return
  props.settings.yawLeftStep = copyTuning(props.settings.yawStep)
  props.settings.yawRightStep = copyTuning(props.settings.yawStep)
  props.settings.pitchUpStep = copyTuning(props.settings.pitchStep)
  props.settings.pitchDownStep = copyTuning(props.settings.pitchStep)
  props.settings.yawLeft = {
    rotationMultiplier: props.settings.rotationMultiplier,
    deadzoneDegrees: props.settings.deadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraYawDegrees,
    deadzoneTransitionDegrees: props.settings.deadzoneTransitionDegrees,
  }
  props.settings.yawRight = { ...props.settings.yawLeft }
  props.settings.pitchUp = {
    rotationMultiplier: props.settings.pitchRotationMultiplier,
    deadzoneDegrees: props.settings.pitchDeadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraPitchDegrees,
    deadzoneTransitionDegrees: props.settings.pitchDeadzoneTransitionDegrees,
  }
  props.settings.pitchDown = { ...props.settings.pitchUp }
}
</script>

<template>
  <div class="space-y-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('pivot.stepped.stepGlide') }}</p>
      <p class="mt-2 text-sm leading-6 text-muted">
        {{ t('pivot.stepped.stepGlideDescription') }}
      </p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('pivot.stepped.transition') }}
            <span
              :title="t('pivot.stepped.transitionHint')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select v-model="settings.stepGlideMode" class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5">
            <option value="instant">{{ t('pivot.stepped.instant') }}</option>
            <option value="glide">{{ t('pivot.stepped.glide') }}</option>
          </select>
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('pivot.stepped.duration') }}
            <span
              :title="t('pivot.stepped.durationHint')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-if="settings.stepGlideMode === 'glide'"
            v-model.number="settings.stepGlideSeconds"
            class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5"
            min="0.01"
            max="2"
            step="0.01"
            type="number"
          />
          <input v-else class="app-input h-11 w-full cursor-not-allowed rounded-[0.75rem] px-4 py-2.5 opacity-70" value="0.0" type="number" readonly />
          <span class="mt-1 block text-xs text-muted">{{ settings.stepGlideMode === 'instant' ? t('pivot.stepped.durationInstant') : t('pivot.stepped.durationGlide') }}</span>
        </label>
      </div>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <label class="flex items-start gap-2.5">
        <input
          v-model="settings.advancedAxes"
          class="mt-0.5 h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="onAdvancedAxesChange(($event.target as HTMLInputElement).checked)"
        />
        <span>
          <span class="block text-sm font-medium">{{ t('pivot.advancedAxes.label') }}</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            {{ t('pivot.advancedAxes.steppedDescription') }}
          </span>
        </span>
      </label>
    </div>

    <div v-if="!settings.advancedAxes" class="grid gap-3 md:grid-cols-2">
      <PivotStepTuningEditor :tuning="settings.yawStep" :label="t('pivot.stepped.yaw')" :hint="t('pivot.stepped.yawHint')" :deadzone-max="180" />
      <PivotStepTuningEditor :tuning="settings.pitchStep" :label="t('pivot.stepped.pitch')" :hint="t('pivot.stepped.pitchHint')" :deadzone-max="90" />
    </div>

    <div v-else class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
      <PivotStepTuningEditor
        v-for="direction in directions"
        :key="direction.key"
        :tuning="settings[direction.key]"
        :label="t(direction.labelKey)"
        :hint="t(direction.hintKey)"
        :deadzone-max="direction.deadzoneMax"
      />
    </div>
  </div>
</template>
