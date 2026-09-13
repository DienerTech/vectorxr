<script setup lang="ts">
defineProps<{ modelValue: boolean }>()
defineEmits<{ 'update:modelValue': [value: boolean] }>()
</script>

<template>
  <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
    <input :checked="modelValue" type="checkbox" class="h-4 w-4 accent-depthxr-copper" aria-label="Turbo Safety"
      @change="$emit('update:modelValue', ($event.target as HTMLInputElement).checked)" />
    {{ modelValue ? 'Enabled' : 'Disabled' }}
  </label>
</template>
