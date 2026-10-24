<script setup lang="ts">
import { t } from '../i18n/index.ts'
import type { SeenApplication } from '../lib/commands'

defineProps<{
  open: boolean
  app: SeenApplication | null
  registeredName: string | null
}>()

defineEmits<{
  close: []
}>()

function formatUnixSeconds(value: number): string {
  if (!value) {
    return t('common.unknown')
  }

  return new Date(value * 1000).toLocaleString()
}
</script>

<template>
  <div
    v-if="open && app"
    class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm"
  >
    <div class="w-full max-w-[480px] rounded-[1.5rem] border p-5 surface-panel-strong">
      <div class="flex items-start justify-between gap-4">
        <div class="min-w-0">
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('registry.seenDetails.eyebrow') }}</p>
          <h2 class="mt-2 break-all font-mono text-xl font-semibold tracking-tight">{{ app.exe }}</h2>
        </div>
        <button
          class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="$emit('close')"
        >
          {{ t('common.close') }}
        </button>
      </div>

      <dl class="mt-5 grid grid-cols-2 gap-3 text-sm">
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <dt class="text-xs uppercase tracking-[0.18em] text-muted">{{ t('registry.seenDetails.registeredAs') }}</dt>
          <dd class="mt-1 truncate font-semibold" :class="registeredName ? '' : 'text-muted'">
            {{ registeredName ?? t('registry.seenDetails.unregistered') }}
          </dd>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <dt class="text-xs uppercase tracking-[0.18em] text-muted">{{ t('registry.seenDetails.launches') }}</dt>
          <dd class="mt-1 font-semibold">{{ app.launchCount }}</dd>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <dt class="text-xs uppercase tracking-[0.18em] text-muted">{{ t('registry.seenDetails.firstSeen') }}</dt>
          <dd class="mt-1">{{ formatUnixSeconds(app.firstSeenUnixSeconds) }}</dd>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <dt class="text-xs uppercase tracking-[0.18em] text-muted">{{ t('registry.seenDetails.lastSeen') }}</dt>
          <dd class="mt-1">{{ formatUnixSeconds(app.lastSeenUnixSeconds) }}</dd>
        </div>
      </dl>
    </div>
  </div>
</template>
