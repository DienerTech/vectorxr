<script setup lang="ts">
import type { PivotSnapStackingStatus, PivotXRSettings } from '../lib/model'

// Apply Enhanced Motion to Snap Views. The profile card and the motion settings
// page both show this control, bound to the same setting.
defineProps<{
  settings: PivotXRSettings
  status: PivotSnapStackingStatus
  compact?: boolean
}>()
</script>

<template>
  <label v-if="status.available" class="flex items-start gap-3">
    <input v-model="settings.applyMotionToSnapViews" type="checkbox" class="mt-1 h-4 w-4 accent-depthxr-copper" />
    <span>
      <span class="block font-semibold">Apply Enhanced Motion to Snap Views</span>
      <span class="mt-1 block text-muted" :class="compact ? 'text-xs' : 'text-sm'">
        Keep assistance when entering a Snap View, or activate it around a view already selected. Uses this profile’s rotation and position tuning.
      </span>
      <span v-if="status.message" class="mt-1 block text-xs text-muted">{{ status.message }}</span>
    </span>
  </label>
  <div v-else>
    <p class="font-semibold">Apply Enhanced Motion to Snap Views</p>
    <p class="mt-1 text-muted" :class="compact ? 'text-xs' : 'text-sm'">{{ status.message }}</p>
  </div>
</template>
