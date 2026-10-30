<script setup lang="ts">
import { bindingListLabel, type InputBinding } from '../lib/model'
import { t } from '../i18n/index.ts'

defineProps<{
  setOriginBindings: InputBinding[]
  releaseOriginBindings: InputBinding[]
}>()

defineEmits<{
  edit: []
}>()
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex items-start justify-between gap-3">
      <div class="flex min-w-0 flex-1 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">{{ t('pivotViews.originPanel.originControls') }}</p>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          :title="t('pivotViews.originPanel.setOriginTitle')"
        >
          <span class="font-semibold">{{ t('pivotViews.originPanel.setOrigin') }}</span>
          <span class="text-muted">{{ bindingListLabel(setOriginBindings) }}</span>
        </span>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs"
          style="border-color: var(--app-border)"
          :title="t('pivotViews.originPanel.releaseOriginTitle')"
        >
          <span class="font-semibold">{{ t('pivotViews.originPanel.releaseOrigin') }}</span>
          <span class="text-muted">{{ bindingListLabel(releaseOriginBindings) }}</span>
        </span>
      </div>

      <button class="button-secondary w-40 shrink-0 rounded-[0.75rem] px-4 py-2 text-center text-sm font-medium" type="button" @click="$emit('edit')">
        {{ t('pivotViews.originPanel.editOrigin') }}
      </button>
    </div>
  </div>
</template>
