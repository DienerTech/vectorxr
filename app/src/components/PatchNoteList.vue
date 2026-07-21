<script setup lang="ts">
import { formatPatchNoteInlineHtml, type PatchNoteItem } from '../lib/patchNotes'

withDefaults(
  defineProps<{
    items: PatchNoteItem[]
    depth?: number
  }>(),
  {
    depth: 0,
  },
)

function itemHtml(item: PatchNoteItem): string {
  return formatPatchNoteInlineHtml(typeof item === 'string' ? item : item.html)
}

function childItems(item: PatchNoteItem): PatchNoteItem[] {
  return typeof item === 'string' ? [] : (item.items ?? [])
}
</script>

<template>
  <ul :class="['patch-note-list space-y-2 text-sm leading-6 text-muted', { 'patch-note-list-nested': depth > 0 }]">
    <li v-for="(item, index) in items" :key="`${depth}-${index}-${itemHtml(item)}`">
      <span class="patch-note-inline" v-html="itemHtml(item)"></span>
      <PatchNoteList v-if="childItems(item).length" :items="childItems(item)" :depth="depth + 1" />
    </li>
  </ul>
</template>

<style scoped>
.patch-note-list {
  list-style-type: disc;
  padding-inline-start: 1.25rem;
}

.patch-note-list-nested {
  margin-top: 0.4rem;
  list-style-type: circle;
}

.patch-note-list :deep(.patch-note-list-nested .patch-note-list-nested) {
  list-style-type: square;
}

.patch-note-list li::marker {
  color: var(--app-accent);
}

.patch-note-inline :deep(strong) {
  color: var(--app-text);
  font-weight: 650;
}

.patch-note-inline :deep(em) {
  color: var(--app-text);
}

.patch-note-inline :deep(code) {
  border: 1px solid var(--app-border);
  border-radius: 0.3rem;
  background: var(--app-surface-strong);
  color: var(--app-text);
  padding: 0.08rem 0.3rem;
  font-size: 0.82rem;
}
</style>
