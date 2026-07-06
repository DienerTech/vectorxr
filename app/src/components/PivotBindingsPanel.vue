<script setup lang="ts">
import type { ActivationMode, InputBinding } from '../lib/model'
import { bindingLabel } from '../lib/model'

// Compact read-only summary of a profile's bindings. Editing happens on the
// dedicated bindings sub-page (see PivotBindingsPage), opened via `edit`.
defineProps<{
  activationMode: ActivationMode
  activationBinding: InputBinding
  setOriginBinding: InputBinding
  releaseOriginBinding: InputBinding
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
            ? 'Pivot engages automatically; this binding (optional) suspends and resumes it.'
            : `Pivot ${activationMode === 'toggle' ? 'toggles on each press of' : 'applies while holding'} this binding.`"
        >
          <span class="font-semibold">{{ activationMode === 'toggle' ? 'Toggle' : activationMode === 'hold' ? 'Hold' : 'Always On' }}</span>
          <span class="text-muted">{{ bindingLabel(activationBinding) }}</span>
        </span>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          title="Captures the current head yaw/pitch as Pivot's neutral forward. Bind it to the same button as the game's recenter so both origins stay aligned."
        >
          <span class="font-semibold">Set Origin</span>
          <span class="text-muted">{{ bindingLabel(setOriginBinding) }}</span>
        </span>
        <span
          v-if="releaseOriginBinding.type !== 'none'"
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          title="Releases a captured origin and returns Pivot to the default HMD origin."
        >
          <span class="font-semibold">Release Origin</span>
          <span class="text-muted">{{ bindingLabel(releaseOriginBinding) }}</span>
        </span>
      </div>

      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Bindings…
      </button>
    </div>
  </div>
</template>
