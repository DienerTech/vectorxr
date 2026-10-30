<script setup lang="ts">
import { t } from '../i18n/index.ts'

defineProps<{ modelValue: boolean }>()
defineEmits<{ 'update:modelValue': [value: boolean] }>()
</script>

<template>
  <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
    <input :checked="modelValue" type="checkbox" class="h-4 w-4 accent-depthxr-copper" :aria-label="t('turbo.safety.title')"
      @change="$emit('update:modelValue', ($event.target as HTMLInputElement).checked)" />
    {{ modelValue ? t('common.enabled') : t('common.disabled') }}
  </label>
</template>
