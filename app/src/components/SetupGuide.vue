<script setup lang="ts">
// Collapsible first-run guidance for a module page. Pages open it while the
// module is unused and close it once a profile is enabled, keeping the
// controls near the top for returning users.
defineProps<{ title?: string; summary?: string; open?: boolean }>()
</script>

<template>
  <details class="section-disclosure rounded-[1rem] border px-4 py-3 surface-panel-soft" style="border-color: var(--app-border)" :open="open">
    <summary class="flex flex-wrap items-center gap-2">
      <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
      <span class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ title ?? 'Setup guide' }}</span>
      <span v-if="summary" class="text-xs text-muted">{{ summary }}</span>
    </summary>
    <div class="mt-3" style="color: var(--app-text)">
      <slot />
    </div>
  </details>
</template>
