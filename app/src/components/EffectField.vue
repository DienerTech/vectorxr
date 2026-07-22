<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  title: string
  subtitle: string
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
  leftLabel?: string
  centerLabel?: string
  rightLabel?: string
}>()

defineEmits<{
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
    class="flex h-full min-w-0 flex-col rounded-[1rem] border p-4 shadow-panel backdrop-blur transition"
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
    </div>

    <div class="flex items-end gap-3">
      <div class="min-w-0 flex-1">
        <input
          :value="toDisplay(value)"
          :min="numberMin"
          :max="numberMax"
          :step="numberStep"
          class="h-2 w-full cursor-pointer accent-depthxr-copper"
          type="range"
          @input="$emit('update:value', fromDisplay(Number(($event.target as HTMLInputElement).value)))"
        />
        <div
          v-if="leftLabel || centerLabel || rightLabel"
          class="mt-2 grid grid-cols-3 gap-2 text-[10px] font-medium uppercase leading-4 tracking-[0.08em] text-muted"
        >
          <span class="text-left">{{ leftLabel }}</span>
          <span class="text-center">{{ centerLabel }}</span>
          <span class="text-right">{{ rightLabel }}</span>
        </div>
      </div>
      <div class="w-[5.5rem] shrink-0">
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
    <div v-if="$slots.footer" class="mt-auto pt-4">
      <div class="border-t pt-3" style="border-color: var(--app-border)">
        <slot name="footer" />
      </div>
    </div>
  </div>
</template>
