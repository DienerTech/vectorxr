<script setup lang="ts">
import { computed } from 'vue'

import type { PivotViewControls } from '../lib/model'

const props = defineProps<{ viewControls: PivotViewControls }>()
defineEmits<{ edit: [] }>()

const bindingCount = computed(() => props.viewControls.quickViews.reduce((total, view) => total + view.activationBindings.length, 0))
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0 flex-1">
        <p class="text-sm font-semibold tracking-tight">Snap Views</p>
        <div class="mt-2 flex flex-wrap gap-2">
          <span class="inline-flex items-center rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
            {{ viewControls.quickViews.length }} view{{ viewControls.quickViews.length === 1 ? '' : 's' }}
          </span>
          <span v-if="bindingCount > 0" class="inline-flex items-center rounded-full border px-3 py-1 text-xs text-muted" style="border-color: var(--app-border)">
            {{ bindingCount }} binding{{ bindingCount === 1 ? '' : 's' }}
          </span>
        </div>
      </div>
      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Snap Views…
      </button>
    </div>
  </div>
</template>