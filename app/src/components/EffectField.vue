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
  <div class="min-w-0 rounded-[1.6rem] border border-black/10 bg-white/80 p-4 shadow-panel backdrop-blur">
    <div class="mb-3 flex items-start justify-between gap-3">
      <div>
        <p class="text-base font-semibold tracking-tight">{{ title }}</p>
        <p class="mt-1 text-[13px] leading-5 text-depthxr-steel">{{ subtitle }}</p>
      </div>
      <label class="inline-flex shrink-0 items-center gap-2 rounded-full bg-depthxr-ink px-3 py-1.5 text-xs font-medium text-white">
        <input
          :checked="enabled"
          class="h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="$emit('update:enabled', ($event.target as HTMLInputElement).checked)"
        />
        Enabled
      </label>
    </div>

    <div class="grid gap-3 md:grid-cols-[minmax(0,1fr)_5.5rem] md:items-center">
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
        class="w-full rounded-2xl border border-black/10 bg-white px-3 py-2 text-right text-sm"
        type="number"
        @input="$emit('update:value', Number(($event.target as HTMLInputElement).value))"
      />
    </div>
  </div>
</template>
