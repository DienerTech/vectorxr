<script setup lang="ts">
import { computed } from 'vue'

import { t } from '../i18n/index.ts'
import type { PivotNudgeSet, PivotProfileBehavior, PivotSnapStackingStatus, PivotXRSettings } from '../lib/model'
import PivotSnapStackingControl from './PivotSnapStackingControl.vue'

const NO_NUDGE_SET_ID = 'none'

interface PivotBehaviorSubject {
  behavior: PivotProfileBehavior
  nudgeSetId: string
}

const props = defineProps<{
  subject: PivotBehaviorSubject
  nudgeSets: PivotNudgeSet[]
  // Motion settings and the Snap View stacking state, shown under the behavior.
  settings: PivotXRSettings
  snapStacking: PivotSnapStackingStatus
  snapViewMotionNote: string
}>()

const emit = defineEmits<{
  editNudges: [id: string]
  addNudgeSet: []
}>()

const selectedSet = computed(() => props.nudgeSets.find((set) => set.id === props.subject.nudgeSetId) ?? null)
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="grid gap-4 md:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
      <div class="space-y-3">
        <label class="block">
          <span class="mb-1.5 block text-sm font-semibold">{{ t('pivot.behavior.profileBehavior') }}</span>
          <select v-model="subject.behavior" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">

            <option value="enhancedMotion">{{ t('pivot.behavior.enhancedMotion') }}</option>
            <option value="snapViews">{{ t('pivot.behavior.snapViews') }}</option>
          </select>
          <span class="mt-1 block text-xs text-muted">
            {{ subject.behavior === 'enhancedMotion' ? t('pivot.behavior.enhancedMotionDescription') : t('pivot.behavior.snapViewsDescription') }}
          </span>
        </label>

        <div class="rounded-[0.75rem] border px-3 py-2.5 text-sm surface-panel-strong">
          <PivotSnapStackingControl v-if="subject.behavior === 'enhancedMotion'" :settings="settings" :status="snapStacking" compact />
          <template v-else>
            <span class="block font-semibold">{{ t('pivot.behavior.enhancedMotionInSnapViews') }}</span>
            <span class="mt-1 block text-xs text-muted">{{ snapViewMotionNote }}</span>
          </template>
        </div>
      </div>

      <div class="space-y-3">
        <div>
          <span class="mb-1.5 block text-sm font-semibold">{{ t('pivot.behavior.nudgeSet') }}</span>
          <div class="flex gap-2">
            <select v-model="subject.nudgeSetId" class="app-input min-w-0 flex-1 rounded-[0.75rem] px-4 py-2.5">
              <option :value="NO_NUDGE_SET_ID">{{ t('common.none') }}</option>
              <option v-for="set in nudgeSets" :key="set.id" :value="set.id">{{ set.name }}</option>
            </select>
            <button v-if="selectedSet" class="button-secondary shrink-0 rounded-[0.75rem] px-3 py-2 text-sm" type="button" @click="emit('editNudges', selectedSet.id)">{{ t('common.edit') }}</button>
            <button class="button-secondary shrink-0 rounded-[0.75rem] px-3 py-2 text-sm" type="button" @click="emit('addNudgeSet')">{{ selectedSet ? t('common.copy') : t('pivot.behavior.newNudgeSet') }}</button>
          </div>
          <span class="mt-1 block text-xs text-muted">
            {{ selectedSet ? t('pivot.behavior.nudgeSetApplied') : t('pivot.behavior.nudgesDisabled') }}
          </span>
        </div>

        <div v-if="selectedSet" class="rounded-[0.75rem] border px-3 py-2.5 text-sm surface-panel-strong">
          <span class="block font-semibold">{{ selectedSet.allowWhileInactive ? t('pivot.behavior.nudgesWhileInactive') : t('pivot.behavior.nudgesFollowProfile') }}</span>
          <span class="mt-1 block text-xs text-muted">
            {{ selectedSet.allowWhileInactive
              ? t('pivot.behavior.nudgesWhileInactiveDetail', { name: selectedSet.name })
              : t('pivot.behavior.nudgesFollowProfileDetail', { name: selectedSet.name }) }}
          </span>
        </div>
      </div>
    </div>
  </div>
</template>
