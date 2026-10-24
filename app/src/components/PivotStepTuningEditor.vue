<script setup lang="ts">
import { computed } from 'vue'

import { t } from '../i18n/index.ts'
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
          {{ t('pivot.stepTuning.deadzone') }}
          <span
            :title="t('pivot.stepTuning.deadzoneHint')"
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.deadzoneDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" :max="deadzoneMax" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          {{ t('pivot.stepTuning.stepTrigger') }}
          <span
            :title="t('pivot.stepTuning.stepTriggerHint')"
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.triggerDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="1" max="45" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          {{ t('pivot.stepTuning.stepAmount') }}
          <span
            :title="t('pivot.stepTuning.stepAmountHint')"
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.amountDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="60" step="0.5" type="number" />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          {{ t('pivot.stepTuning.hysteresis') }}
          <span
            :title="t('pivot.stepTuning.hysteresisHint')"
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
          {{ hysteresisInvalid ? t('pivot.stepTuning.hysteresisInvalid') : t('pivot.stepTuning.hysteresisKeepBelow') }}
        </span>
      </label>
      <label class="block sm:col-span-2 xl:col-span-1">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          {{ t('pivot.stepTuning.maxExtra') }}
          <span
            :title="t('pivot.stepTuning.maxExtraHint')"
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </span>
        <input v-model.number="tuning.maxExtraDegrees" class="app-input w-full rounded-[0.75rem] px-3 py-2" min="0" max="180" step="0.5" type="number" />
      </label>
    </div>
  </div>
</template>
