<script setup lang="ts">
import { computed } from 'vue'
import type { PivotPositionAssist } from '../lib/model'
const props=defineProps<{settings: PivotPositionAssist}>()
const directions=computed(() => props.settings.advancedDirections ? [
  {label:'Turning left',value:props.settings.yawLeft}, {label:'Turning right',value:props.settings.yawRight},
  {label:'Looking up',value:props.settings.pitchUp}, {label:'Looking down',value:props.settings.pitchDown},
] : [{label:'Turning left / right',value:props.settings.yaw}])
function seedDirections() {
  if (!props.settings.advancedDirections) return
  if ([props.settings.yawLeft, props.settings.yawRight, props.settings.pitchUp, props.settings.pitchDown].some(d => d.rightCm || d.upCm || d.forwardCm)) return
  props.settings.yawRight={...props.settings.yaw}
  props.settings.yawLeft={...props.settings.yaw,rightCm:-props.settings.yaw.rightCm}
}
</script>
<template>
  <section class="rounded-[1rem] border p-4 surface-panel-soft">
    <label class="flex items-start gap-3"><input v-model="settings.enabled" type="checkbox" class="mt-1 h-4 w-4 accent-depthxr-copper" />
      <span><span class="block font-semibold">Position Assist</span><span class="mt-1 block text-sm text-muted">Add lean or height as you turn. Physical leaning remains 1:1. Offsets ease in and out relative to your seated Pivot origin.</span></span>
    </label>
    <template v-if="settings.enabled">
      <label class="mt-4 flex gap-3 text-sm"><input v-model="settings.advancedDirections" type="checkbox" @change="seedDirections" />Advanced directions — independent left, right, up, and down</label>
      <div v-for="direction in directions" :key="direction.label" class="mt-4 rounded-[0.75rem] border p-3 surface-panel-strong">
        <p class="font-semibold">{{ direction.label }}</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-3">
          <label class="text-sm">{{settings.advancedDirections ? 'Right / left (cm)' : 'Sideways lean toward turn (cm)'}}
            <input v-model.number="direction.value.rightCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" /></label>
          <label class="text-sm">Up / down (cm)<input v-model.number="direction.value.upCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" /></label>
          <label class="text-sm">Forward / back (cm)<input v-model.number="direction.value.forwardCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" /></label>
          <label class="text-sm">Start angle (°)<input v-model.number="direction.value.startAngleDegrees" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="0" :max="direction.value.fullAngleDegrees - 0.1" step="1" /></label>
          <label class="text-sm">Full-offset angle (°)<input v-model.number="direction.value.fullAngleDegrees" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" :min="direction.value.startAngleDegrees + 0.1" max="180" step="1" /></label>
        </div>
        <p v-if="direction.value.fullAngleDegrees <= direction.value.startAngleDegrees" class="mt-2 text-sm text-warning">Full-offset angle must be greater than Start angle.</p>
      </div>
      <p class="mt-3 text-xs text-muted">Uses physical head angle. Inside a Snap View, movement is measured around its captured head reference. Negative offsets reverse the movement.</p>
    </template>
  </section>
</template>
