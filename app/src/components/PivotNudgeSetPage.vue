<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'

import BindingListEditor from './BindingListEditor.vue'
import { t } from '../i18n/index.ts'
import { defaultPivotNudgeSettings, type PivotNudgeSet } from '../lib/model'

const props = defineProps<{
  nudgeSet: PivotNudgeSet
  usageCount: number
  canDelete: boolean
}>()
const emit = defineEmits<{ close: []; delete: [] }>()

function resetToDefaults() {
  if (!window.confirm(t('pivotViews.nudgeSetPage.confirmReset'))) return
  props.nudgeSet.settings = defaultPivotNudgeSettings()
}

function deleteSet() {
  if (!props.canDelete) return
  if (!window.confirm(t('pivotViews.nudgeSetPage.confirmDelete', { name: props.nudgeSet.name }))) return
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
        <nav class="flex items-center gap-1.5 text-xs text-muted" :aria-label="t('pivotViews.shared.breadcrumb')">
          <button class="underline-offset-2 hover:underline" type="button" @click="emit('close')">{{ t('pivotViews.shared.pivot') }}</button>
          <span aria-hidden="true">›</span>
          <span class="font-medium" style="color: var(--app-text)">{{ t('pivotViews.nudgeSetPage.nudgeSets') }}</span>
        </nav>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('pivotViews.shared.editTitle', { name: nudgeSet.name }) }}</h2>
        <p class="mt-1 text-sm text-muted">{{ t('pivotViews.nudgeSetPage.linked', { count: usageCount }) }}</p>
      </div>
      <button class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]" type="button" :aria-label="t('pivotViews.shared.returnToPivot')" @click="emit('close')">×</button>
    </div>

    <div class="mt-5 space-y-4">
      <section class="rounded-[1rem] border p-4 surface-panel-soft">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">{{ t('pivotViews.nudgeSetPage.setName') }}</span>
          <input v-model="nudgeSet.name" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="text" />
        </label>
        <label class="mt-4 flex items-start gap-3 rounded-[0.75rem] border p-3 surface-panel-strong">
          <input v-model="nudgeSet.allowWhileInactive" class="mt-1 h-4 w-4" type="checkbox" />
          <span>
            <span class="block text-sm font-semibold">{{ t('pivotViews.nudgeSetPage.allowWhileInactive') }}</span>
            <span class="mt-1 block text-xs text-muted">{{ t('pivotViews.nudgeSetPage.allowWhileInactiveHint') }}</span>
          </span>
        </label>
        <div class="mt-4 grid gap-4 md:grid-cols-3">
          <label class="block"><span class="mb-1.5 block text-sm font-medium">{{ t('pivotViews.nudgeSetPage.yawStep') }}</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.yawStepDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="1" max="90" step="1" /><span class="text-sm text-muted">°</span></div></label>
          <label class="block"><span class="mb-1.5 block text-sm font-medium">{{ t('pivotViews.nudgeSetPage.pitchStep') }}</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.pitchStepDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="1" max="60" step="1" /><span class="text-sm text-muted">°</span></div></label>
          <label class="block"><span class="mb-1.5 block text-sm font-medium">{{ t('pivotViews.shared.transition') }}</span><div class="flex items-center gap-2"><input v-model.number="nudgeSet.settings.transitionSeconds" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="0" max="2" step="0.01" /><span class="text-sm text-muted">{{ t('pivotViews.shared.unitSeconds') }}</span></div></label>
        </div>
      </section>

      <section class="space-y-3">
        <BindingListEditor v-model="nudgeSet.settings.yawLeftBindings" :label="t('pivotViews.nudgeSetPage.yawLeft')" :description="t('pivotViews.nudgeSetPage.yawStepDescription')" sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.yawRightBindings" :label="t('pivotViews.nudgeSetPage.yawRight')" :description="t('pivotViews.nudgeSetPage.yawStepDescription')" sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.pitchUpBindings" :label="t('pivotViews.nudgeSetPage.pitchUp')" :description="t('pivotViews.nudgeSetPage.pitchStepDescription')" sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.pitchDownBindings" :label="t('pivotViews.nudgeSetPage.pitchDown')" :description="t('pivotViews.nudgeSetPage.pitchStepDescription')" sound-mode="single" />
        <BindingListEditor v-model="nudgeSet.settings.centerBindings" :label="t('pivotViews.nudgeSetPage.center')" :description="t('pivotViews.nudgeSetPage.centerDescription')" sound-mode="single" />
      </section>
    </div>

    <div class="mt-5 flex flex-wrap items-center justify-between gap-3 border-t pt-4" style="border-color: var(--app-border)">
      <div class="flex flex-wrap gap-2">
        <button class="button-secondary rounded-[0.75rem] px-4 py-2.5 text-sm font-medium" type="button" @click="resetToDefaults">{{ t('pivotViews.nudgeSetPage.resetToDefaults') }}</button>
        <button
          class="button-danger rounded-[0.75rem] px-4 py-2.5 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
          type="button"
          :disabled="!canDelete"
          :title="canDelete ? t('pivotViews.nudgeSetPage.deleteTitle') : t('pivotViews.nudgeSetPage.deleteRequired')"
          @click="deleteSet"
        >
          {{ t('pivotViews.nudgeSetPage.deleteSet') }}
        </button>
      </div>
      <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="emit('close')">{{ t('common.done') }}</button>
    </div>
  </article>
</template>