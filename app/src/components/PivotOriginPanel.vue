<script setup lang="ts">
import { bindingListLabel, type InputBinding } from '../lib/model'

defineProps<{
  setOriginBindings: InputBinding[]
  releaseOriginBindings: InputBinding[]
}>()

defineEmits<{
  edit: []
}>()
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Origin Controls</p>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          title="Captures the current head pose as Pivot's neutral seated origin."
        >
          <span class="font-semibold">Set Origin</span>
          <span class="text-muted">{{ bindingListLabel(setOriginBindings) }}</span>
        </span>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          title="Releases a captured origin and returns Pivot to the default HMD origin."
        >
          <span class="font-semibold">Release Origin</span>
          <span class="text-muted">{{ bindingListLabel(releaseOriginBindings) }}</span>
        </span>
      </div>

      <button class="button-secondary w-40 shrink-0 rounded-[0.75rem] px-4 py-2 text-center text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Origin…
      </button>
    </div>
  </div>
</template>
