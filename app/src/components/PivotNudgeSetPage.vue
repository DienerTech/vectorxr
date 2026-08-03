<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'

import BindingListEditor from './BindingListEditor.vue'
import { defaultPivotNudgeSettings, type PivotNudgeSet } from '../lib/model'

const props = defineProps<{
  nudgeSet: PivotNudgeSet
  usageCount: number
  canDelete: boolean
}>()
const emit = defineEmits<{ close: []; delete: [] }>()

function resetToDefaults() {
  if (!window.confirm('Reset movement values and remove every binding from this nudge set?')) return
  props.nudgeSet.settings = defaultPivotNudgeSettings()
}

function deleteSet() {
  if (!props.canDelete) return
  if (!window.confirm(`Delete "${props.nudgeSet.name}"? Profiles using it will switch to another nudge set.`)) return
  emit('delete')
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close')
}
onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-start justify-between gap-4">
      <div>
        <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
          <button class="underline-offset-2 hover:underline" type="button" @click="emit('close')">Pivot</button>
          <span aria-hidden="true">›</span>
          <span class="font-medium" style="color: var(--app-text)">Nudge Sets</span>
        </nav>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">Edit {{ nudgeSet.name }}</h2>
        <p class="mt-1 text-sm text-muted">Linked to {{ usageCount }} profile{{ usageCount === 1 ? '' : 's' }}. Changes apply everywhere this set is selected.</p>
      </div>
      <button class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]" type="button" aria-label="Return to Pivot" @click="emit('close')">×</button>
    </div>

    <div class="mt-5 space-y-4">
      <section class="rounded-[1rem] border p-4 surface-panel-soft">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Set name</span>
          <input v-model="nudgeSet.name" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="text" />
        </label>
        <label class="mt-4 flex items-start gap-3 rounded-[0.75rem] border p-3 surface-panel-strong">
          <input v-model="nudgeSet.allowWhileInactive" class="mt-1 h-4 w-4" type="checkbox" />
          <span>
            <span class="block text-sm font-semibold">Allow while Pivot is inactive</span>
            <span class="mt-1 block text-xs text-muted">These bindings can adjust and retain the global view offset even when no linked profile is engaged.</span>
          </span>
        </label>
        <div class="mt-4 grid gap-4 md:grid-cols-3">
          <label class="block"><span class="mb-1.5 block text-sm font-medium">Yaw step</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.yawStepDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="1" max="90" step="1" /><span class="text-sm text-muted">°</span></div></label>
          <label class="block"><span class="mb-1.5 block text-sm font-medium">Pitch step</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.pitchStepDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="1" max="60" step="1" /><span class="text-sm text-muted">°</span></div></label>
          <label class="block"><span class="mb-1.5 block text-sm font-medium">Transition</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.transitionSeconds" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="0" max="2" step="0.01" /><span class="text-sm text-muted">s</span></div></label>
        </div>
      </section>

      <section class="space-y-3">
        <BindingListEditor v-model="nudgeSet.settings.yawLeftBindings" label="Yaw Left" description="Add one yaw step to the current view." sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.yawRightBindings" label="Yaw Right" description="Add one yaw step to the current view." sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.pitchUpBindings" label="Pitch Up" description="Add one pitch step to the current view." sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.pitchDownBindings" label="Pitch Down" description="Add one pitch step to the current view." sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.centerBindings" label="Center Nudge Offset" description="Clear the accumulated nudge offset." sound-mode="single" />
      </section>
    </div>

    <div class="mt-5 flex flex-wrap items-center justify-between gap-3 border-t pt-4" style="border-color: var(--app-border)">
      <div class="flex flex-wrap gap-2">
        <button class="button-secondary rounded-[0.75rem] px-4 py-2.5 text-sm font-medium" type="button" @click="resetToDefaults">Reset to Defaults</button>
        <button
          class="button-secondary rounded-[0.75rem] px-4 py-2.5 text-sm font-medium"
          type="button"
          :disabled="!canDelete"
          :title="canDelete ? 'Delete this nudge set' : 'At least one nudge set is required'"
          @click="deleteSet"
        >
          Delete Nudge Set
        </button>
      </div>
      <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="emit('close')">Done</button>
    </div>
  </article>
</template>