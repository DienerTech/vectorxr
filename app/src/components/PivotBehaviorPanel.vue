<script setup lang="ts">
import { computed } from 'vue'

import type { PivotNudgeSet, PivotProfileBehavior, PivotSnapTurnPreference } from '../lib/model'

interface PivotBehaviorSubject {
  behavior: PivotProfileBehavior
  snapTurnPreference: PivotSnapTurnPreference
  nudgeSetId: string
}

const props = defineProps<{
  subject: PivotBehaviorSubject
  nudgeSets: PivotNudgeSet[]
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
      <label class="block">
        <span class="mb-1.5 block text-sm font-semibold">Profile behavior</span>
        <select v-model="subject.behavior" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">

          <option value="enhancedMotion">Enhanced Motion</option>
          <option value="snapViews">Snap Views</option>
        </select>
        <span class="mt-1 block text-xs text-muted">
          {{ subject.behavior === 'enhancedMotion' ? 'Amplifies natural head rotation.' : 'Bindings select fixed poses relative to the Pivot origin.' }}
        </span>
      </label>

      <div>
        <span class="mb-1.5 block text-sm font-semibold">Nudge Set</span>
        <div class="flex gap-2">
          <select v-model="subject.nudgeSetId" class="app-input min-w-0 flex-1 rounded-[0.75rem] px-4 py-2.5">
            <option v-for="set in nudgeSets" :key="set.id" :value="set.id">{{ set.name }}</option>
          </select>
          <button v-if="selectedSet" class="button-secondary shrink-0 rounded-[0.75rem] px-3 py-2 text-sm" type="button" @click="emit('editNudges', selectedSet.id)">Edit</button>
          <button class="button-secondary shrink-0 rounded-[0.75rem] px-3 py-2 text-sm" type="button" @click="emit('addNudgeSet')">Copy</button>
        </div>
        <span class="mt-1 block text-xs text-muted">Applied after the active behavior; linked edits update every profile using this set.</span>
      </div>

      <label v-if="subject.behavior === 'snapViews'" class="block">
        <span class="mb-1.5 block text-sm font-semibold">Snap travel</span>
        <select v-model="subject.snapTurnPreference" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
          <option value="shortest">Shortest path</option>
          <option value="left">Prefer left</option>
          <option value="right">Prefer right</option>
        </select>
        <span class="mt-1 block text-xs text-muted">All paths are shortest; the preference only resolves an exact 180-degree tie.</span>
      </label>

      <div v-if="selectedSet" class="rounded-[0.75rem] border px-3 py-2.5 text-sm surface-panel-strong">
        <span class="block font-semibold">{{ selectedSet.allowWhileInactive ? 'Nudges remain available while Pivot is inactive' : 'Nudges follow the active profile' }}</span>
        <span class="mt-1 block text-xs text-muted">
          {{ selectedSet.allowWhileInactive
            ? 'The ' + selectedSet.name + ' Nudge Set can adjust the view even when this profile is disengaged.'
            : "The " + selectedSet.name + " Nudge Set is available only while this profile's behavior is active." }}
        </span>
      </div>
    </div>
  </div>
</template>