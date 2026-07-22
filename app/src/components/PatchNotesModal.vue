<script setup lang="ts">
import PatchNoteList from './PatchNoteList.vue'
import { formatPatchNoteInlineHtml, type PatchNoteEntry } from '../lib/patchNotes'

defineProps<{
  open: boolean
  entries: PatchNoteEntry[]
}>()

defineEmits<{
  close: []
}>()
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex max-h-[85vh] w-full max-w-[1040px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex flex-wrap items-start justify-between gap-4 border-b px-6 py-5" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Patch Notes</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Recent updates</h2>
          <p class="mt-2 text-sm text-muted">Everything new in the app, with the latest entry pinned at the top.</p>
        </div>

        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('close')">
          Close
        </button>
      </header>

      <div class="overflow-auto px-6 py-5">
        <article v-for="entry in entries" :key="`${entry.version}-${entry.date}`" class="rounded-[1rem] border p-5 surface-panel mb-4 last:mb-0">
          <div class="flex flex-wrap items-center gap-3">
            <span class="chip-accent rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">{{ entry.version }}</span>
            <span class="text-sm text-muted">{{ entry.date }}</span>
          </div>

          <h3 class="mt-3 text-xl font-semibold tracking-tight">{{ entry.title }}</h3>
          <p class="mt-2 text-sm leading-6 text-muted" v-html="formatPatchNoteInlineHtml(entry.summary)"></p>

          <PatchNoteList class="mt-4" :items="entry.items" />
        </article>
      </div>
    </div>
  </div>
</template>
