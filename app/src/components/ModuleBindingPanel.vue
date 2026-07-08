<script setup lang="ts">
import type { InputBinding } from '../lib/model'
import { bindingLabel } from '../lib/model'

// Compact one-line summary of a module-level binding (Turbo/Depth toggles).
// Editing happens on the dedicated sub-page (ModuleBindingPage), opened via
// `edit` — mirrors the Pivot bindings panel/page pair.
defineProps<{
  heading: string
  binding: InputBinding
  // Tooltip explaining what the binding does.
  hint?: string
}>()

defineEmits<{
  edit: []
}>()
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">{{ heading }}</p>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          :title="hint"
        >
          <span class="text-muted">{{ bindingLabel(binding) }}</span>
          <span
            v-if="binding.type !== 'none' && binding.sound?.enabled"
            class="text-muted"
            title="Sound feedback is enabled for this binding"
            aria-label="Sound feedback enabled"
          >♪</span>
        </span>
      </div>

      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Binding…
      </button>
    </div>
  </div>
</template>
