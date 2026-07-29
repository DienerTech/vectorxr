<script setup lang="ts">
import { computed } from 'vue'

import type { PivotStepTuning } from '../lib/model'

const props = defineProps<{
  tuning: PivotStepTuning
  label: string
  hint: string
  deadzoneMax: number
}>()

const hysteresisInvalid = computed(() => (
  Number.isFinite(props.tuning.hysteresisDegrees)
  && Number.isFinite(props.tuning.triggerDegrees)
  && props.tuning.hysteresisDegrees >= props.tuning.triggerDegrees
))
</script>

<template>
  <div class="rounded-[0.9rem] border p-3 surface-panel">
    <p class="eyebrow text-xs uppercase tracking-[0.18em]" :title="hint">{{ label }}</p>
    <div class="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Deadzone
          <span
            title="The base head angle before step thresholds begin. Motion inside this angle never creates a step."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.deadzoneDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" :max="deadzoneMax" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Step Trigger
          <span
            title="How many more degrees you turn after the deadzone to add each step."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.triggerDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="1" max="45" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Step Amount
          <span
            title="How much extra view rotation is added each time you cross a step trigger."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.amountDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="60" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Hysteresis
          <span
            title="How far you must turn back below a trigger before its step is removed. This prevents flicker near a threshold and must be smaller than Step Trigger."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input
          v-model.number="tuning.hysteresisDegrees"
          class="app-input w-full rounded-[0.75rem] px-3 py-2"
          :class="{ 'app-input-error': hysteresisInvalid }"
          :aria-invalid="hysteresisInvalid"
          min="0"
          max="20"
          step="0.5"
          type="number"
        />
        <span class="mt-1 block text-xs" :class="hysteresisInvalid ? 'text-input-error' : 'text-muted'">
          {{ hysteresisInvalid ? 'must be smaller than step trigger' : 'keep below trigger' }}
        </span>
      </label>
      <label class="block sm:col-span-2 xl:col-span-1">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Max Extra
          <span
            title="The most extra view rotation these steps can add. Set to 0 for no cap."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.maxExtraDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="180" step="0.5" type="number" />
      </label>
    </div>
  </div>
</template>
