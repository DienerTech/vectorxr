<script setup lang="ts">
import { t } from '../i18n/index.ts'

defineProps<{
  open: boolean
  exporting: boolean
}>()

defineEmits<{
  cancel: []
  confirm: []
}>()
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="w-full max-w-[640px] rounded-[1.5rem] border p-5 surface-panel-strong">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('app.debugExport.eyebrow') }}</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('app.debugExport.title') }}</h2>
          <p class="mt-2 max-w-2xl text-sm leading-6 text-muted">
            {{ t('app.debugExport.description') }}
          </p>
        </div>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('cancel')">
          {{ t('common.close') }}
        </button>
      </div>

      <section class="mt-5 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="text-sm font-semibold tracking-tight">{{ t('app.debugExport.included') }}</p>
        <div class="mt-3 grid gap-2 text-sm leading-6 md:grid-cols-2">
          <span>{{ t('app.debugExport.items.settings') }}</span>
          <span>{{ t('app.debugExport.items.seenApps') }}</span>
          <span>{{ t('app.debugExport.items.logs') }}</span>
          <span>{{ t('app.debugExport.items.layers') }}</span>
          <span>{{ t('app.debugExport.items.health') }}</span>
          <span>{{ t('app.debugExport.items.version') }}</span>
          <span>{{ t('app.debugExport.items.turboMetrics') }}</span>
          <span>{{ t('app.debugExport.items.turboSafety') }}</span>
          <span>{{ t('app.debugExport.items.runtime') }}</span>
          <span>{{ t('app.debugExport.items.inventory') }}</span>
        </div>
      </section>

      <p class="mt-4 text-xs leading-5 text-muted">
        {{ t('app.debugExport.limits') }}
      </p>
      <p class="mt-2 text-xs leading-5 text-muted">
        {{ t('app.debugExport.privacy') }}
      </p>

      <div class="mt-5 flex flex-wrap justify-end gap-3">
        <button class="button-secondary rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('cancel')">
          {{ t('common.cancel') }}
        </button>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('confirm')">
          {{ exporting ? t('app.debugExport.exporting') : t('app.debugExport.save') }}
        </button>
      </div>
    </div>
  </div>
</template>
