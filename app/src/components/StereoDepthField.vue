<script setup lang="ts">
import { computed } from 'vue'

import EffectField from './EffectField.vue'
import {
  fromStereoBoostDisplay,
  stereoBoostBadge,
  toStereoBoostDisplay,
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

const extendedRange = computed(() => props.displayLimit > 25)

function setExtendedRange(enabled: boolean) {
  emit('update:displayLimit', enabled ? 100 : 25)
  const displayValue = toStereoBoostDisplay(props.value)
  if (!enabled && Math.abs(displayValue) > 25) {
    emit('update:value', fromStereoBoostDisplay(Math.sign(displayValue) * 25))
  }
}
</script>

<template>
  <EffectField
    :value="value"
    :muted="muted"
    title="Stereo Depth / World Scale"
    subtitle="Changes virtual eye separation while keeping your physical headset IPD untouched. Move right for stronger near-field stereo and a smaller-feeling world; move left for a larger-feeling, flatter world. Start around ±5–15%."
    :min="1 - props.displayLimit / 100"
    :max="1 + props.displayLimit / 100"
    :step="0.001"
    :display-min="-props.displayLimit"
    :display-max="props.displayLimit"
    :display-step="0.1"
    :display-value="toStereoBoostDisplay"
    :parse-display-value="fromStereoBoostDisplay"
    :display-badge="stereoBoostBadge"
    left-label="- Larger world"
    center-label="0 Native scale"
    right-label="+ 3D stereo effect"
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
        Extended range (&plusmn;100%)
        <span class="text-muted">Use only when normal &plusmn;25% tuning is not enough.</span>
      </label>
    </template>
  </EffectField>
</template>