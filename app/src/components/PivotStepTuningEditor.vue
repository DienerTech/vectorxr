<script setup lang="ts">
import type { PivotStepTuning } from '../lib/model'

defineProps<{
  tuning: PivotStepTuning
  label: string
  hint: string
  deadzoneMax: number
}>()
</script>

<template>
  <div class="rounded-[0.9rem] border p-3 surface-panel">
    <p class="eyebrow text-xs uppercase tracking-[0.18em]" :title="hint">{{ label }}</p>
    <div class="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-1">
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Deadzone</span>
        <input v-model.number="tuning.deadzoneDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" :max="deadzoneMax" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Step Trigger</span>
        <input v-model.number="tuning.triggerDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="1" max="45" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Step Amount</span>
        <input v-model.number="tuning.amountDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="60" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Hysteresis</span>
        <input v-model.number="tuning.hysteresisDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="20" step="0.5" type="number" />
        <span class="mt-1 block text-xs text-muted">keep below trigger</span>
      </label>
      <label class="block sm:col-span-2 xl:col-span-1">
        <span class="mb-1.5 block text-sm font-medium">Max Extra</span>
        <input v-model.number="tuning.maxExtraDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="180" step="0.5" type="number" />
      </label>
    </div>
  </div>
</template>
