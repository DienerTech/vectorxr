<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  title: string
  subtitle: string
  enabled: boolean
  value: number
  min: number
  max: number
  step: number
  muted?: boolean
  displayMin?: number
  displayMax?: number
  displayStep?: number
  displayValue?: (value: number) => number
  parseDisplayValue?: (value: number) => number
  displayBadge?: (value: number) => string
}>()

defineEmits<{
  'update:enabled': [value: boolean]
  'update:value': [value: number]
}>()

const numberMin = computed(() => props.displayMin ?? props.min)
const numberMax = computed(() => props.displayMax ?? props.max)
const numberStep = computed(() => props.displayStep ?? props.step)

function toDisplay(value: number): number {
  return props.displayValue ? props.displayValue(value) : value
}

function fromDisplay(value: number): number {
  return props.parseDisplayValue ? props.parseDisplayValue(value) : value
}
</script>

<template>
  <div
    class="min-w-0 rounded-[1.6rem] border border-black/10 p-4 shadow-panel backdrop-blur transition"
    :class="muted ? 'bg-stone-50/85 opacity-70' : 'bg-white/80'"
  >
    <div class="mb-3 flex items-start justify-between gap-3">
      <div>
        <div class="flex flex-wrap items-center gap-2">
          <p class="text-base font-semibold tracking-tight">{{ title }}</p>
          <span v-if="displayBadge" class="rounded-full bg-[#f0e1d2] px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em] text-depthxr-copper">
            {{ displayBadge(value) }}
          </span>
        </div>
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

    <div class="grid gap-3 md:grid-cols-[minmax(0,1fr)_7rem] md:items-center">
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
        :value="toDisplay(value)"
        :min="numberMin"
        :max="numberMax"
        :step="numberStep"
        class="w-full rounded-2xl border border-black/10 bg-white px-3 py-2 text-right text-sm"
        type="number"
        @input="$emit('update:value', fromDisplay(Number(($event.target as HTMLInputElement).value)))"
      />
    </div>
  </div>
</template>
