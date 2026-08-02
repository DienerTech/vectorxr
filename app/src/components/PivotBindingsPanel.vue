<script setup lang="ts">
import { bindingLabel, type PivotActivationBinding } from '../lib/model'

const props = defineProps<{
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
}>()

defineEmits<{ edit: [] }>()

</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="grid min-w-0 flex-1 grid-cols-[auto_auto_minmax(0,1fr)] items-start gap-2">
        <p class="pt-1 text-sm font-semibold tracking-tight">Activation</p>
        <span class="inline-flex items-center rounded-full border px-3 py-1 text-xs font-semibold" style="border-color: var(--app-border)">
          {{ alwaysActive ? 'Always active' : 'Manual' }}
        </span>
        <div v-if="activationBindings.length" class="flex min-w-0 flex-col items-start gap-2">
          <span
            v-for="(item, index) in activationBindings"
            :key="index"
            class="inline-flex max-w-full items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
            style="border-color: var(--app-border)"
          >
            <span class="shrink-0 font-semibold">{{ item.behavior === 'toggle' ? 'Toggle' : 'Hold' }}</span>
            <span class="min-w-0 break-words text-muted">{{ bindingLabel(item.binding) }}</span>
          </span>
        </div>
        <span v-else class="pt-1 text-xs text-muted">
          {{ alwaysActive ? 'No suspend controls' : 'No activation controls' }}
        </span>
      </div>
      <button class="button-secondary w-40 shrink-0 rounded-[0.75rem] px-4 py-2 text-center text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Activation...
      </button>
    </div>
  </div>
</template>
