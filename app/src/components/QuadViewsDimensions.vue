<script setup lang="ts">
import { computed } from 'vue'
import type { RuntimeStatusSession } from '../lib/commands'
import { t } from '../i18n/index.ts'
const props = defineProps<{ sessions: RuntimeStatusSession[] }>()
const nameKeys = [
  'quadviews.dimensions.peripheralLeft',
  'quadviews.dimensions.peripheralRight',
  'quadviews.dimensions.focusLeft',
  'quadviews.dimensions.focusRight',
] as const
const live = computed(() => props.sessions.some(session =>
  session.updatedAtUnixMilliseconds - (session.quadviewsDimensionsAt ?? 0) <= 5000,
))
const tooltip = computed(() => {
  if (!props.sessions.length) return t('quadviews.dimensions.waiting')
  return props.sessions.map(session => [
    session.application,
    ...(session.quadviewsDimensions ?? []).map((size, index) =>
      t('quadviews.dimensions.view', {
        name: t(nameKeys[index]),
        width: size.width,
        height: size.height,
        allocatedWidth: size.allocatedWidth,
        allocatedHeight: size.allocatedHeight,
      }),
    ),
  ].join('\n')).join('\n\n') + '\n\n' + t('quadviews.dimensions.footer')
})
</script>

<template>
  <span class="inline-flex shrink-0 cursor-help items-center whitespace-nowrap rounded-full border px-2.5 py-1 text-xs text-muted"
    :title="tooltip" tabindex="0" :aria-label="tooltip">
    {{ !sessions.length ? t('quadviews.dimensions.labelNone') : live ? t('quadviews.dimensions.labelLive') : t('quadviews.dimensions.labelLast') }}
  </span>
</template>
