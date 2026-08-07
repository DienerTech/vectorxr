<script setup lang="ts">
import { computed } from 'vue'

import type { PivotViewControls } from '../lib/model'

const props = defineProps<{ viewControls: PivotViewControls }>()
defineEmits<{ edit: [] }>()

const bindingCount = computed(() => props.viewControls.quickViews.reduce((total, view) => total + view.activationBindings.length, 0))
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Snap View Bindings</p>
        <span class="inline-flex items-center rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
          {{ bindingCount }} binding{{ bindingCount === 1 ? '' : 's' }}
        </span>
        <span v-if="bindingCount === 0" class="text-xs text-muted">Snap Views remain inactive until a binding is assigned.</span>
      </div>
      <button class="button-secondary w-40 shrink-0 rounded-[0.75rem] px-4 py-2 text-center text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Bindings…
      </button>
    </div>
  </div>
</template>