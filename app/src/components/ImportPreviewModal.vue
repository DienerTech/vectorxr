<script setup lang="ts">
import I18nText from './I18nText.vue'
import { t } from '../i18n/index.ts'
import type { ModuleId, VectorXRConfig } from '../lib/model'

defineProps<{
  open: boolean
  config: VectorXRConfig | null
  errors: string[]
}>()

defineEmits<{
  apply: []
  cancel: []
}>()

const previewModules: ModuleId[] = ['depthxr', 'pivotxr', 'quadviews', 'turbo', 'framegen']
</script>

<template>
  <div v-if="open && config" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex max-h-[85vh] w-full max-w-[640px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex flex-wrap items-start justify-between gap-4 border-b px-6 py-5" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('app.importPreview.eyebrow') }}</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('app.importPreview.title') }}</h2>
          <p class="mt-2 text-sm text-muted">
            {{ t('app.importPreview.description') }}
          </p>
        </div>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('cancel')">
          {{ t('common.cancel') }}
        </button>
      </header>

      <div class="overflow-auto px-6 py-5 space-y-4">
        <div class="rounded-[1rem] border p-4 text-sm leading-6 surface-panel">
          <p class="font-medium">{{ t('app.importPreview.whatWillLoad') }}</p>
          <ul class="mt-3 space-y-2 text-muted">
            <I18nText tag="li" keypath="app.importPreview.applications">
              <template #number><span class="font-medium" style="color: var(--app-text)">{{ config.applications.length }}</span></template>
            </I18nText>
            <I18nText
              v-for="moduleId in previewModules"
              :key="moduleId"
              tag="li"
              keypath="app.importPreview.moduleDefault"
              :params="{ module: t(`app.tabs.${moduleId}.label`), count: config.modules[moduleId].profiles.length }"
            >
              <template #state><span class="font-medium" style="color: var(--app-text)">{{ config.modules[moduleId].enabled ? t('common.on') : t('common.off') }}</span></template>
            </I18nText>
          </ul>
        </div>

        <div
          v-if="errors.length > 0"
          class="rounded-[1rem] border px-4 py-4 text-sm leading-6 chip-warning"
          style="border-color: var(--app-border)"
        >
          <p class="font-medium">{{ t('app.importPreview.issuesFound', { count: errors.length }) }}</p>
          <p class="mt-1 text-sm">
            {{ t('app.importPreview.issuesHint') }}
          </p>
          <ul class="mt-3 space-y-1.5">
            <li v-for="error in errors" :key="error">{{ error }}</li>
          </ul>
        </div>

        <div
          v-else
          class="rounded-[1rem] border px-4 py-4 text-sm chip-success"
          style="border-color: var(--app-border)"
        >
          {{ t('app.importPreview.valid') }}
        </div>
      </div>

      <footer class="flex justify-end gap-3 border-t px-6 py-4" style="border-color: var(--app-border)">
        <button
          class="button-secondary rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('cancel')"
        >
          {{ t('common.cancel') }}
        </button>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('apply')"
        >
          {{ errors.length > 0 ? t('app.importPreview.applyToEditor') : t('app.importPreview.importAndSave') }}
        </button>
      </footer>
    </div>
  </div>
</template>
