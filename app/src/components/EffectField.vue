<script setup lang="ts">
defineProps<{
  title: string
  subtitle: string
  enabled: boolean
  value: number
  min: number
  max: number
  step: number
}>()

defineEmits<{
  'update:enabled': [value: boolean]
  'update:value': [value: number]
}>()
</script>

<template>
  <div class="rounded-3xl border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
    <div class="mb-4 flex items-start justify-between gap-4">
      <div>
        <p class="text-lg font-semibold tracking-tight">{{ title }}</p>
        <p class="mt-1 text-sm text-depthxr-steel">{{ subtitle }}</p>
      </div>
      <label class="inline-flex items-center gap-2 rounded-full bg-depthxr-ink px-3 py-1.5 text-sm text-white">
        <input
          :checked="enabled"
          class="h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="$emit('update:enabled', ($event.target as HTMLInputElement).checked)"
        />
        Enabled
      </label>
    </div>

    <div class="flex items-center gap-4">
      <input
        :value="value"
        :min="min"
        :max="max"
        :step="step"
        class="w-full accent-depthxr-copper"
        type="range"
        @input="$emit('update:value', Number(($event.target as HTMLInputElement).value))"
      />
      <input
        :value="value"
        :min="min"
        :max="max"
        :step="step"
        class="w-24 rounded-2xl border border-black/10 bg-white px-3 py-2 text-right"
        type="number"
        @input="$emit('update:value', Number(($event.target as HTMLInputElement).value))"
      />
    </div>
  </div>
</template>
