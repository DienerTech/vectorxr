<script setup lang="ts">
import { computed } from 'vue'

import { t } from '../i18n/index.ts'

const props = defineProps<{
  dirty: boolean
  saving: boolean
  loading: boolean
  status: string
  disabled: boolean
  // Validation errors currently blocking save; shown as the save button's
  // tooltip so the user can find the offending control.
  errors?: string[]
}>()

defineEmits<{
  save: []
  discard: []
}>()

const saveTooltip = computed(() => {
  if (!props.errors || props.errors.length === 0) {
    return ''
  }
  const shown = props.errors.slice(0, 6)
  const remainder = props.errors.length - shown.length
  return [
    t('app.saveBar.fixBeforeSaving'),
    ...shown.map((error) => `• ${error}`),
    ...(remainder > 0 ? [t('app.saveBar.moreErrors', { count: remainder })] : []),
  ].join('\n')
})
</script>

<template>
  <div>
    <div class="mx-auto max-w-[1500px] rounded-[1.25rem] border p-4 backdrop-blur surface-panel-strong">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <div class="flex items-center gap-2.5">
            <p class="text-sm font-semibold tracking-tight">
              {{ dirty ? t('app.saveBar.unsaved') : t('app.saveBar.allSaved') }}
            </p>
            <span v-if="dirty" class="chip-warning rounded-full px-2.5 py-1 text-xs font-medium">{{ t('app.saveBar.pending') }}</span>
          </div>
          <p class="mt-1 text-sm text-muted">{{ status || (dirty ? t('app.saveBar.dirtyHint') : t('app.saveBar.cleanHint')) }}</p>
        </div>

        <div class="flex flex-wrap gap-3">
          <span :title="saveTooltip" class="inline-flex">
            <button
              class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-accent disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="disabled || !dirty"
              type="button"
              @click="$emit('save')"
            >
              {{ saving ? t('app.saveBar.saving') : t('app.saveBar.save') }}
            </button>
          </span>
          <button
            class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-secondary disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving || !dirty"
            type="button"
            @click="$emit('discard')"
          >
            {{ t('app.saveBar.discard') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
