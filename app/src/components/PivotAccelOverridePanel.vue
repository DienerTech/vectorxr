<script setup lang="ts">
import { computed } from 'vue'

import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingListEditor from './BindingListEditor.vue'
import { savedBindingConflictWarnings, type PivotAccelOverrideSettings, type VectorXRConfig } from '../lib/model'

// Static, global section (not per profile): one hold binding and one pair of
// multipliers that temporarily replace the Enhanced Motion multipliers while
// the control is held. Fields mutate the passed-in reactive `settings` object.
const props = defineProps<{
  settings: PivotAccelOverrideSettings
  config: VectorXRConfig
}>()

const conflictWarnings = computed(() => savedBindingConflictWarnings(props.config, props.settings.bindings, {
  suppressFocusOnlyConflicts: true,
}))

const multiplierHint = [
  'Total view rotation per degree of head rotation while the control is held.',
  '1.0 = no acceleration (the view follows your head 1:1). Values below 1.0 make the view move less than your head, which steadies it further.',
  'The view does not jump when you press: movement is measured from where your head was at that moment.',
].join('\n\n')
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <p class="text-sm font-semibold tracking-tight">Acceleration Override (while held)</p>
    <p class="mt-1 text-sm leading-6 text-muted">
      Temporarily replaces the Enhanced Motion multipliers while a control is held, for example bind it to your zoom button so head jitter is not amplified while zoomed in. On release the normal multipliers apply again.
    </p>

    <div v-if="conflictWarnings.length" class="mt-3 space-y-3">
      <BindingConflictWarnings :warnings="conflictWarnings" />
    </div>

    <div class="mt-3 grid gap-3 sm:grid-cols-2">
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Yaw Multiplier
          <span :title="multiplierHint" class="cursor-help select-none text-xs text-muted">ⓘ</span>
        </span>
        <input
          v-model.number="settings.yawMultiplier"
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          min="0"
          max="3"
          step="0.05"
          type="number"
        />
      </label>
      <label class="block">
        <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
          Pitch Multiplier
          <span :title="multiplierHint" class="cursor-help select-none text-xs text-muted">ⓘ</span>
        </span>
        <input
          v-model.number="settings.pitchMultiplier"
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          min="0"
          max="3"
          step="0.05"
          type="number"
        />
      </label>
    </div>

    <BindingListEditor
      v-model="settings.bindings"
      class="mt-3"
      label="Hold Bindings"
      description="The override applies only while one of these controls is held down."
      none-text="No binding assigned. The override never engages."
    />
  </div>
</template>
