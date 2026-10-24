<script setup lang="ts">
import type { PivotSnapStackingStatus, PivotXRSettings } from '../lib/model'
import { t } from '../i18n/index.ts'

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
      <span class="block font-semibold">{{ t('pivotViews.snapStacking.title') }}</span>
      <span class="mt-1 block text-muted" :class="compact ? 'text-xs' : 'text-sm'">
        {{ t('pivotViews.snapStacking.description') }}
      </span>
      <span v-if="status.message" class="mt-1 block text-xs text-muted">{{ status.message }}</span>
    </span>
  </label>
  <div v-else>
    <p class="font-semibold">{{ t('pivotViews.snapStacking.title') }}</p>
    <p class="mt-1 text-muted" :class="compact ? 'text-xs' : 'text-sm'">{{ status.message }}</p>
  </div>
</template>
