<script setup lang="ts">
import { computed } from 'vue'
import type { PivotActivationBinding } from '../lib/model'
import { bindingListLabel } from '../lib/model'

const props = defineProps<{
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
}>()

defineEmits<{ edit: [] }>()

const toggleBindings = computed(() => props.activationBindings.filter((item) => item.behavior === 'toggle').map((item) => item.binding))
const holdBindings = computed(() => props.activationBindings.filter((item) => item.behavior === 'hold').map((item) => item.binding))
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Bindings</p>
        <span class="inline-flex items-center rounded-full border px-3 py-1 text-xs font-semibold" style="border-color: var(--app-border)">
          {{ alwaysActive ? 'Always active' : 'Manual' }}
        </span>
        <span v-if="toggleBindings.length" class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
          <span class="font-semibold">Toggle</span><span class="text-muted">{{ bindingListLabel(toggleBindings) }}</span>
        </span>
        <span v-if="holdBindings.length" class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs" style="border-color: var(--app-border)">
          <span class="font-semibold">Hold</span><span class="text-muted">{{ bindingListLabel(holdBindings) }}</span>
        </span>
        <span v-if="activationBindings.length === 0" class="text-xs text-muted">
          {{ alwaysActive ? 'No suspend controls' : 'No activation controls' }}
        </span>
      </div>
      <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('edit')">
        Edit Bindings?
      </button>
    </div>
  </div>
</template>
