<script setup lang="ts">
import PivotStepTuningEditor from './PivotStepTuningEditor.vue'
import type { PivotStepTuning, PivotXRSettings } from '../lib/model'

const props = defineProps<{ settings: PivotXRSettings }>()

const directions = [
  { key: 'yawLeftStep', label: 'Yaw Left', hint: 'Step behavior while turning left.', deadzoneMax: 180 },
  { key: 'yawRightStep', label: 'Yaw Right', hint: 'Step behavior while turning right.', deadzoneMax: 180 },
  { key: 'pitchUpStep', label: 'Pitch Up', hint: 'Step behavior while looking up.', deadzoneMax: 90 },
  { key: 'pitchDownStep', label: 'Pitch Down', hint: 'Step behavior while looking down.', deadzoneMax: 90 },
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
  }
  props.settings.yawRight = { ...props.settings.yawLeft }
  props.settings.pitchUp = {
    rotationMultiplier: props.settings.pitchRotationMultiplier,
    deadzoneDegrees: props.settings.pitchDeadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraPitchDegrees,
  }
  props.settings.pitchDown = { ...props.settings.pitchUp }
}
</script>

<template>
  <div class="space-y-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Step Glide</p>
      <p class="mt-2 text-sm leading-6 text-muted">
        Instant jumps directly to each step. Glide moves monotonically to the new target without overshoot or coasting.
      </p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Transition</span>
          <select v-model="settings.stepGlideMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
            <option value="instant">Instant</option>
            <option value="glide">Glide</option>
          </select>
        </label>
        <label v-if="settings.stepGlideMode === 'glide'" class="block">
          <span class="mb-1.5 block text-sm font-medium">Duration</span>
          <input v-model.number="settings.stepGlideSeconds" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.01" max="2" step="0.01" type="number" />
          <span class="mt-1 block text-xs text-muted">seconds, shared by every direction</span>
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
          <span class="block text-sm font-medium">Advanced axes — tune each direction independently</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            Give left, right, up, and down separate deadzones, triggers, amounts, hysteresis, and limits. Enabling this copies the current basic yaw and pitch values.
          </span>
        </span>
      </label>
    </div>

    <div v-if="!settings.advancedAxes" class="grid gap-3 md:grid-cols-2">
      <PivotStepTuningEditor :tuning="settings.yawStep" label="Yaw" hint="Shared by left and right." :deadzone-max="180" />
      <PivotStepTuningEditor :tuning="settings.pitchStep" label="Pitch" hint="Shared by up and down." :deadzone-max="90" />
    </div>

    <div v-else class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
      <PivotStepTuningEditor
        v-for="direction in directions"
        :key="direction.key"
        :tuning="settings[direction.key]"
        :label="direction.label"
        :hint="direction.hint"
        :deadzone-max="direction.deadzoneMax"
      />
    </div>
  </div>
</template>
