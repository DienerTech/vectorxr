<script setup lang="ts">
import { computed } from 'vue'

import type { InputBinding } from '../lib/model'
import { bindingListLabel } from '../lib/model'

// Compact one-line summary of a module-level toggle's bindings (Turbo, Depth,
// the OSD): any one of them acts, one per device for instance.
// Editing happens on the dedicated sub-page (ModuleBindingPage), opened via
// `edit` — mirrors the Pivot bindings panel/page pair.
const props = defineProps<{
  heading: string
  // A toggle's list; or `binding`, a single one (Quadviews' diagnostic view).
  bindings?: InputBinding[]
  binding?: InputBinding
  // Tooltip explaining what the binding does.
  hint?: string
}>()

defineEmits<{
  edit: []
}>()

const list = computed(() => props.bindings ?? (props.binding && props.binding.type !== 'none' ? [props.binding] : []))
const withSound = computed(() => list.value.some((binding) => binding.type !== 'none' && binding.sound?.enabled))
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
          <span class="text-muted">{{ bindingListLabel(list) }}</span>
          <span
            v-if="withSound"
            class="text-muted"
            title="Sound feedback is enabled for a binding"
            aria-label="Sound feedback enabled"
          >♪</span>
        </span>
      </div>

      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        {{ list.length > 1 ? 'Edit Bindings…' : 'Edit Binding…' }}
      </button>
    </div>
  </div>
</template>
