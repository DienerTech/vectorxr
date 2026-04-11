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
    class="min-w-0 rounded-[1rem] border p-4 shadow-panel backdrop-blur transition"
    :class="muted ? 'surface-panel-soft opacity-70' : 'surface-panel'"
  >
    <div class="mb-3 flex items-start justify-between gap-3">
      <div>
        <div class="flex flex-wrap items-center gap-2">
          <p class="text-base font-semibold tracking-tight">{{ title }}</p>
          <span v-if="displayBadge" class="chip-accent rounded-full px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em]">
            {{ displayBadge(value) }}
          </span>
        </div>
        <p class="mt-1 text-[13px] leading-5 text-muted">{{ subtitle }}</p>
      </div>
      <label class="pill-toggle inline-flex shrink-0 items-center gap-2 rounded-full px-3 py-1.5 text-xs font-medium">
        <input
          :checked="enabled"
          class="h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="$emit('update:enabled', ($event.target as HTMLInputElement).checked)"
        />
        Enabled
      </label>
    </div>

    <div class="max-w-[9rem]">
      <input
        :value="toDisplay(value)"
        :min="numberMin"
        :max="numberMax"
        :step="numberStep"
        class="app-input w-full rounded-[0.75rem] px-3 py-2 text-right text-sm"
        type="number"
        @input="$emit('update:value', fromDisplay(Number(($event.target as HTMLInputElement).value)))"
      />
    </div>
  </div>
</template>
