<script setup lang="ts">
import type { VectorXRConfig } from '../lib/model'

defineProps<{
  open: boolean
  config: VectorXRConfig | null
  errors: string[]
}>()

defineEmits<{
  apply: []
  cancel: []
}>()
</script>

<template>
  <div v-if="open && config" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex max-h-[85vh] w-full max-w-[640px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex flex-wrap items-start justify-between gap-4 border-b px-6 py-5" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Import</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Review before applying</h2>
          <p class="mt-2 text-sm text-muted">
            A valid import will be saved immediately. Imports with validation issues can still be loaded into the editor for repair.
          </p>
        </div>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('cancel')">
          Cancel
        </button>
      </header>

      <div class="overflow-auto px-6 py-5 space-y-4">
        <div class="rounded-[1rem] border p-4 text-sm leading-6 surface-panel">
          <p class="font-medium">What will load</p>
          <ul class="mt-3 space-y-2 text-muted">
            <li>
              Applications:
              <span class="font-medium" style="color: var(--app-text)">{{ config.applications.length }}</span>
            </li>
            <li>
              Depth default: <span class="font-medium" style="color: var(--app-text)">{{ config.modules.depthxr.enabled ? 'On' : 'Off' }}</span>
              — {{ config.modules.depthxr.profiles.length }} {{ config.modules.depthxr.profiles.length === 1 ? 'profile' : 'profiles' }}
            </li>
            <li>
              Pivot default: <span class="font-medium" style="color: var(--app-text)">{{ config.modules.pivotxr.enabled ? 'On' : 'Off' }}</span>
              — {{ config.modules.pivotxr.profiles.length }} {{ config.modules.pivotxr.profiles.length === 1 ? 'profile' : 'profiles' }}
            </li>
            <li>
              Quadviews default: <span class="font-medium" style="color: var(--app-text)">{{ config.modules.quadviews.enabled ? 'On' : 'Off' }}</span>
              - {{ config.modules.quadviews.profiles.length }} {{ config.modules.quadviews.profiles.length === 1 ? 'profile' : 'profiles' }}
            </li>
          </ul>
        </div>

        <div
          v-if="errors.length > 0"
          class="rounded-[1rem] border px-4 py-4 text-sm leading-6 chip-warning"
          style="border-color: var(--app-border)"
        >
          <p class="font-medium">{{ errors.length }} validation {{ errors.length === 1 ? 'issue' : 'issues' }} found</p>
          <p class="mt-1 text-sm">
            You can still apply — errors will appear in the Validation panel and save will be blocked until resolved.
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
          Config structure and value ranges look valid.
        </div>
      </div>

      <footer class="flex justify-end gap-3 border-t px-6 py-4" style="border-color: var(--app-border)">
        <button
          class="button-secondary rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('cancel')"
        >
          Cancel
        </button>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('apply')"
        >
          {{ errors.length > 0 ? 'Apply to Editor' : 'Import and Save' }}
        </button>
      </footer>
    </div>
  </div>
</template>
