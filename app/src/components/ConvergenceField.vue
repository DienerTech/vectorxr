<script setup lang="ts">
import { computed } from 'vue'

import { t } from '../i18n/index.ts'
import EffectField from './EffectField.vue'
import {
  convergenceBadge,
  fromConvergenceDisplay,
  toConvergenceDisplay,
} from '../lib/display'

const props = defineProps<{
  value: number
  displayLimit: number
  muted?: boolean
}>()

const emit = defineEmits<{
  'update:value': [value: number]
  'update:displayLimit': [value: number]
}>()

const extendedRange = computed(() => props.displayLimit > 5)

function setExtendedRange(enabled: boolean) {
  emit('update:displayLimit', enabled ? 25 : 5)
  if (!enabled && Math.abs(toConvergenceDisplay(props.value)) > 5) {
    emit('update:value', fromConvergenceDisplay(Math.sign(props.value) * 5))
  }
}
</script>

<template>
  <EffectField
    :value="value"
    :muted="muted"
    :title="t('depth.convergence.title')"
    :subtitle="t('depth.convergence.subtitle')"
    :min="-props.displayLimit / 100"
    :max="props.displayLimit / 100"
    :step="0.001"
    :display-min="-props.displayLimit"
    :display-max="props.displayLimit"
    :display-step="0.1"
    :display-value="toConvergenceDisplay"
    :parse-display-value="fromConvergenceDisplay"
    :display-badge="convergenceBadge"
    :left-label="t('depth.convergence.left')"
    :center-label="t('depth.convergence.center')"
    :right-label="t('depth.convergence.right')"
    @update:value="$emit('update:value', $event)"
  >
    <template #footer>
      <label class="inline-flex cursor-pointer items-center gap-2 text-xs font-medium">
        <input
          :checked="extendedRange"
          class="h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="setExtendedRange(($event.target as HTMLInputElement).checked)"
        />
        {{ t('depth.convergence.extendedRange') }}
        <span class="text-muted">{{ t('depth.convergence.extendedRangeHint') }}</span>
      </label>
    </template>
  </EffectField>
</template>
