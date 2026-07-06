<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'

import PivotSettingsFields from './PivotSettingsFields.vue'
import type { PivotXRSettings } from '../lib/model'

// Full-page editor for one profile's rotation settings; fields mutate the
// passed-in reactive settings object directly.
defineProps<{
  settings: PivotXRSettings
  // Names the profile in the breadcrumb/title, e.g. "Default Profile" or "DCS".
  contextLabel: string
}>()

const emit = defineEmits<{
  close: []
}>()

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    emit('close')
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
})

onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
})
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">Pivot</button>
            <span aria-hidden="true">›</span>
            <span>{{ contextLabel }}</span>
            <span aria-hidden="true">›</span>
            <span class="font-medium" style="color: var(--app-text)">Rotation Settings</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ contextLabel }} — Rotation Settings</h2>
          <p class="mt-1 text-sm text-muted">Press Esc or Done to return to Pivot.</p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          aria-label="Close settings and return to Pivot"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <div class="mt-5">
        <PivotSettingsFields :settings="settings" />
      </div>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">
          Done
        </button>
      </div>
    </article>
  </div>
</template>
