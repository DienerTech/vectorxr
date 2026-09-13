<script setup lang="ts">
import { computed } from 'vue'
import type { RuntimeStatusSession } from '../lib/commands'
const props = defineProps<{ sessions: RuntimeStatusSession[] }>()
const names = ['Peripheral L', 'Peripheral R', 'Focus L', 'Focus R']
const live = computed(() => props.sessions.some(session =>
  session.updatedAtUnixMilliseconds - (session.quadviewsDimensionsAt ?? 0) <= 5000,
))
const tooltip = computed(() => {
  if (!props.sessions.length) return 'View dimensions appear here while a Quadviews game is running.'
  return props.sessions.map(session => [
    session.application,
    ...(session.quadviewsDimensions ?? []).map((size, index) =>
      `${names[index]}: ${size.width} × ${size.height} (texture ${size.allocatedWidth} × ${size.allocatedHeight})`,
    ),
  ].join('\n')).join('\n\n') + '\n\nSubmitted view sizes, refreshed every second. Saved resolution changes may require a game restart.'
})
</script>

<template>
  <span class="inline-flex shrink-0 cursor-help items-center whitespace-nowrap rounded-full border px-2.5 py-1 text-xs text-muted"
    :title="tooltip" tabindex="0" :aria-label="tooltip">
    Dimensions {{ !sessions.length ? '—' : live ? '· Live' : '· Last' }}
  </span>
</template>
