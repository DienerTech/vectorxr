<script setup lang="ts" generic="T extends string">
import { useId } from 'vue'

// A small group of mutually exclusive choices shown side by side as cards, so
// each option's trade-off reads at a glance. Extra controls for the chosen
// option go in the default slot, under the cards.
defineProps<{
  legend: string
  options: { value: T; label: string; badge?: string; description: string }[]
}>()

const model = defineModel<T>({ required: true })
const name = useId()
</script>

<template>
  <fieldset>
    <legend class="sr-only">{{ legend }}</legend>
    <p class="eyebrow text-xs uppercase tracking-[0.18em]" aria-hidden="true">{{ legend }}</p>
    <div class="mt-3 grid gap-3 md:grid-cols-2">
      <label
        v-for="option in options"
        :key="option.value"
        class="flex cursor-pointer items-start gap-3 rounded-[0.9rem] border p-3 transition"
        :class="model === option.value ? 'surface-panel' : 'surface-panel-soft'"
        :style="{ borderColor: model === option.value ? 'var(--app-accent)' : 'var(--app-border)' }"
      >
        <input
          v-model="model"
          :name="name"
          :value="option.value"
          class="mt-1 h-4 w-4 shrink-0 accent-depthxr-copper"
          type="radio"
        />
        <span class="min-w-0">
          <span class="flex flex-wrap items-center gap-2">
            <span class="text-sm font-semibold tracking-tight">{{ option.label }}</span>
            <span v-if="option.badge" class="chip-idle rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.14em]">{{ option.badge }}</span>
          </span>
          <span class="mt-1 block text-xs leading-5 text-muted">{{ option.description }}</span>
        </span>
      </label>
    </div>
    <slot />
  </fieldset>
</template>
