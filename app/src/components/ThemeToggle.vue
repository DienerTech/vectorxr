<script setup lang="ts">
import type { ThemePreference } from '../lib/theme'

defineProps<{
  modelValue: ThemePreference
}>()

defineEmits<{
  'update:modelValue': [value: ThemePreference]
}>()

const options: Array<{ value: ThemePreference; label: string }> = [
  { value: 'system', label: 'System' },
  { value: 'light', label: 'Light' },
  { value: 'dark', label: 'Dark' },
]
</script>

<template>
  <div class="inline-flex flex-wrap gap-2 rounded-[1rem] border p-1 surface-panel-strong">
    <button
      v-for="option in options"
      :key="option.value"
      class="rounded-[0.75rem] px-3 py-2 text-sm font-medium transition"
      :class="modelValue === option.value ? 'tab-button-active' : 'tab-button-idle'"
      type="button"
      @click="$emit('update:modelValue', option.value)"
    >
      {{ option.label }}
    </button>
  </div>
</template>
