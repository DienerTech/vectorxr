<script setup lang="ts">
import { t } from '../i18n/index.ts'

defineProps<{
  value: boolean
  muted?: boolean
}>()

defineEmits<{
  'update:value': [value: boolean]
}>()
</script>

<template>
  <label
    class="flex cursor-pointer items-start gap-3 rounded-[1rem] border p-4 shadow-panel backdrop-blur transition"
    :class="muted ? 'surface-panel-soft opacity-70' : 'surface-panel'"
  >
    <input
      :checked="value"
      class="mt-1 h-4 w-4 shrink-0 accent-depthxr-copper"
      type="checkbox"
      @change="$emit('update:value', ($event.target as HTMLInputElement).checked)"
    />
    <span class="min-w-0">
      <span class="flex flex-wrap items-center gap-2">
        <span class="text-base font-semibold tracking-tight">{{ t('depth.depthLock.name') }}</span>
        <span class="chip-warning rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em]">
          {{ t('depth.experimental') }}
        </span>
        <span
          class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em]"
          :class="value ? 'chip-accent' : 'surface-panel-strong text-muted'"
        >
          {{ value ? t('common.on') : t('common.off') }}
        </span>
      </span>
      <span class="mt-1 block text-[13px] leading-5 text-muted">
        {{ t('depth.depthLock.description') }}
      </span>
    </span>
  </label>
</template>
