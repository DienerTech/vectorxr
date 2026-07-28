<script setup lang="ts">
import type { ActivationMode, InputBinding } from '../lib/model'
import { bindingListLabel } from '../lib/model'

// Compact read-only summary of a profile's bindings. Editing happens on the
// dedicated bindings sub-page (see PivotBindingsPage), opened via `edit`.
defineProps<{
  activationMode: ActivationMode
  activationBindings: InputBinding[]
}>()

defineEmits<{
  edit: []
}>()
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <!-- items-start + shrink-0 keep the edit button pinned top-right so it
         stays vertically aligned with the other summary rows' buttons even
         when chips wrap. -->
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Bindings</p>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          :title="activationMode === 'alwaysOn'
            ? 'Pivot engages automatically; any assigned binding suspends and resumes it.'
            : `Pivot ${activationMode === 'toggle' ? 'toggles when pressing' : 'applies while holding'} any assigned binding.`"
        >
          <span class="font-semibold">{{ activationMode === 'toggle' ? 'Toggle' : activationMode === 'hold' ? 'Hold' : 'Always On' }}</span>
          <span class="text-muted">{{ bindingListLabel(activationBindings) }}</span>
        </span>
      </div>

      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Bindings…
      </button>
    </div>
  </div>
</template>
