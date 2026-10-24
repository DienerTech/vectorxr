<script setup lang="ts">
import { t, type MessageKey } from '../i18n/index.ts'
import type { ThemePreference } from '../lib/theme'

defineProps<{
  modelValue: ThemePreference
}>()

defineEmits<{
  'update:modelValue': [value: ThemePreference]
}>()

// Labels are keys, translated at render so a language change applies.
const options: Array<{ value: ThemePreference; labelKey: MessageKey }> = [
  { value: 'system', labelKey: 'settings.theme.system' },
  { value: 'light', labelKey: 'settings.theme.light' },
  { value: 'dark', labelKey: 'settings.theme.dark' },
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
      {{ t(option.labelKey) }}
    </button>
  </div>
</template>
