<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'

import { t } from '../i18n/index.ts'
import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingListEditor from './BindingListEditor.vue'
import { savedBindingConflictWarnings, type VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
}>()

const emit = defineEmits<{
  close: []
}>()

const warnings = computed(() => savedBindingConflictWarnings(props.config, [
  ...props.config.modules.depthxr.bindings.toggleEnabledBindings,
  ...props.config.modules.depthxr.bindings.toggleAnchorBindings,
]))

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close')
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" :aria-label="t('depth.bindingsPage.breadcrumb')">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">{{ t('depth.title') }}</button>
            <span aria-hidden="true">&rsaquo;</span>
            <span class="font-medium" style="color: var(--app-text)">{{ t('depth.bindingsPage.current') }}</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('depth.bindingsPage.title') }}</h2>
          <p class="mt-1 text-sm text-muted">{{ t('depth.bindingsPage.escHint') }}</p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          :aria-label="t('depth.bindingsPage.closeLabel')"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <div class="mt-5 space-y-4">
        <div class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)" role="note">
          <p class="font-medium">{{ t('depth.bindingsPage.compareGently.heading') }}</p>
          <p class="mt-1">
            {{ t('depth.bindingsPage.compareGently.body') }}
          </p>
        </div>

        <BindingConflictWarnings :warnings="warnings" />

        <BindingListEditor
          v-model="config.modules.depthxr.bindings.toggleEnabledBindings"
          :label="t('depth.bindingsPage.depthToggle.label')"
          :description="t('depth.bindingsPage.depthToggle.description')"
          :none-text="t('depth.bindingsPage.depthToggle.noneText')"
        />

        <BindingListEditor
          v-model="config.modules.depthxr.bindings.toggleAnchorBindings"
          :label="t('depth.bindingsPage.lockToggle.label')"
          :description="t('depth.bindingsPage.lockToggle.description')"
          :none-text="t('depth.bindingsPage.lockToggle.noneText')"
          default-activate-sound="depth-lock-on.wav"
          default-deactivate-sound="depth-lock-off.wav"
        />
      </div>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">
          {{ t('common.done') }}
        </button>
      </div>
    </article>
  </div>
</template>
