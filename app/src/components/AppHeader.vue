<script setup lang="ts">
import type { PatchNoteEntry } from '../lib/patchNotes'

defineProps<{
  eyebrow: string
  title: string
  body: string
  status: string
  dirty: boolean
  latestPatch: PatchNoteEntry
}>()

defineEmits<{
  openPatchNotes: []
}>()
</script>

<template>
  <header class="rounded-[1.5rem] border p-4 surface-panel-strong md:p-5">
    <div class="grid gap-4 xl:grid-cols-[minmax(0,1.45fr)_minmax(300px,0.85fr)]">
      <div class="min-w-0">
        <div class="flex flex-wrap items-center gap-3">
          <span class="chip-accent rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">VectorXR</span>
          <span class="rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]" :class="dirty ? 'chip-warning' : 'chip-success'">
            {{ dirty ? 'Unsaved changes' : 'Saved' }}
          </span>
        </div>

        <p class="eyebrow mt-4 text-xs uppercase tracking-[0.24em]">{{ eyebrow }}</p>
        <h1 class="mt-2 text-3xl font-semibold tracking-tight md:text-[2.4rem]">{{ title }}</h1>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted md:text-[15px]">{{ body }}</p>
      </div>

      <div class="space-y-3 rounded-[1rem] border p-4 surface-panel">
        <div class="flex items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.22em]">Latest Patch Notes</p>
            <h2 class="mt-2 text-lg font-semibold tracking-tight">{{ latestPatch.title }}</h2>
            <p class="mt-2 text-sm text-muted">{{ latestPatch.summary }}</p>
          </div>
          <span class="chip-accent shrink-0 rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">
            {{ latestPatch.version }}
          </span>
        </div>

        <div class="flex flex-wrap items-center justify-between gap-3 text-sm">
          <span class="text-muted">{{ latestPatch.date }}</span>
          <button class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('openPatchNotes')">
            Open log
          </button>
        </div>
      </div>
    </div>
  </header>
</template>
