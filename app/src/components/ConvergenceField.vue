<script setup lang="ts">
import { computed, ref } from 'vue'

import EffectField from './EffectField.vue'
import {
  convergenceBadge,
  fromConvergenceDisplay,
  toConvergenceDisplay,
} from '../lib/display'

const props = defineProps<{
  value: number
  muted?: boolean
}>()

const emit = defineEmits<{
  'update:value': [value: number]
}>()

const extendedRange = ref(Math.abs(toConvergenceDisplay(props.value)) > 5)
const displayLimit = computed(() => extendedRange.value ? 25 : 5)

function setExtendedRange(enabled: boolean) {
  extendedRange.value = enabled
  if (!enabled && Math.abs(toConvergenceDisplay(props.value)) > 5) {
    emit('update:value', fromConvergenceDisplay(Math.sign(props.value) * 5))
  }
}
</script>

<template>
  <EffectField
    :value="value"
    :muted="muted"
    title="Convergence / Depth Plane"
    subtitle="Places the zero-parallax plane without changing relative depth. Keep 0 while tuning Stereo Depth, then use small steps: negative moves the plane farther; positive moves it nearer."
    :min="-displayLimit / 100"
    :max="displayLimit / 100"
    :step="0.001"
    :display-min="-displayLimit"
    :display-max="displayLimit"
    :display-step="0.1"
    :display-value="toConvergenceDisplay"
    :parse-display-value="fromConvergenceDisplay"
    :display-badge="convergenceBadge"
    left-label="- Farther plane"
    center-label="0 App default"
    right-label="+ Nearer plane"
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
        Extended range (&plusmn;25)
        <span class="text-muted">Use only when normal &plusmn;5 tuning is not enough.</span>
      </label>
    </template>
  </EffectField>
</template>
