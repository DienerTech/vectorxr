<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { t, type MessageKey } from '../i18n/index.ts'
import type { RuntimeStatusEnvelope, TurboRecoveryRecord } from '../lib/commands'
import type { VectorXRConfig } from '../lib/model'
import { turboFaultReason, turboSafetyApplies, turboSafetyBlocks } from '../lib/turboSafety'
import TurboSafetyToggle from './TurboSafetyToggle.vue'

const props = defineProps<{ config: VectorXRConfig; savedConfig: VectorXRConfig; status: RuntimeStatusEnvelope; error: string; clearing: string | null; clearingLogs: boolean }>()
const emit = defineEmits<{ close: []; clear: [fingerprint: string]; clearLogs: [] }>()
const blocks = computed(() => turboSafetyBlocks(props.status.recovery))
const selected = ref<TurboRecoveryRecord | null>(null)
function date(value: number) { return value ? new Date(value).toLocaleString() : t('common.unknown') }
// Turbo states the layer reports, shown by their label.
const sessionStateKeys: Record<string, MessageKey> = {
  'recovery-disabled': 'turbo.safety.state.recoveryDisabled', suspended: 'turbo.safety.state.suspended',
  async: 'turbo.safety.state.async', sequenced: 'turbo.safety.state.sequenced', waiting: 'turbo.safety.state.waiting', off: 'turbo.safety.state.off',
  framegen: 'turbo.safety.state.framegen',
}
function sessionState(state?: string) {
  return t(sessionStateKeys[state ?? ''] ?? 'turbo.safety.state.unavailable')
}
function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') { if (selected.value) selected.value = null; else emit('close') }
}
onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
</script>

<template>
  <article class="space-y-5 rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <header class="flex flex-wrap items-start justify-between gap-4">
      <div>
        <nav class="flex items-center gap-1.5 text-xs text-muted" :aria-label="t('turbo.breadcrumb')">
          <button class="underline-offset-2 hover:underline" type="button" @click="emit('close')">{{ t('turbo.title') }}</button><span aria-hidden="true">›</span><span>{{ t('turbo.safety.title') }}</span>
        </nav>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('turbo.safety.title') }}</h2>
        <p class="mt-1 max-w-2xl text-sm leading-6 text-muted">{{ t('turbo.safety.intro') }}</p>
      </div>
      <button type="button" class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" @click="emit('close')">{{ t('turbo.safety.back') }}</button>
    </header>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <h3 class="text-base font-semibold">{{ t('turbo.safety.protection.title') }}</h3>
        <TurboSafetyToggle v-model="config.modules.turbo.interruptedSessionRecovery" />
      </div>
      <p class="mt-2 text-sm leading-6 text-muted">{{ t('turbo.safety.protection.body') }}</p>
      <p class="mt-2 text-sm leading-6 text-muted">{{ t('turbo.safety.protection.saveNote') }}</p>
      <div v-for="session in status.sessions" :key="session.sessionId" class="mt-3 rounded-lg border p-3 text-sm" role="status">
        <p class="font-medium">{{ session.application }} · {{ sessionState(session.state.turboState) }}</p>
        <p v-if="session.state.turboReason" class="mt-1 text-xs text-muted">{{ session.state.turboReason }}</p>
      </div>
    </section>
    <p v-if="error" class="rounded-lg p-3 text-sm chip-danger" role="alert">{{ error }}</p>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <h3 class="text-base font-semibold">{{ t('turbo.safety.blocked.title') }} <span class="text-sm text-muted">({{ blocks.length }})</span></h3>
      <p class="mt-1 text-sm leading-6 text-muted">{{ t('turbo.safety.blocked.intro') }}</p>
      <div class="mt-3 max-h-72 overflow-auto rounded-lg border" tabindex="0" :aria-label="t('turbo.safety.blocked.ariaLabel')">
        <table v-if="blocks.length" class="w-full text-left text-sm">
          <thead class="sticky top-0 surface-panel-strong"><tr><th class="p-3">{{ t('turbo.safety.columns.application') }}</th><th class="p-3">{{ t('turbo.safety.columns.recorded') }}</th><th class="p-3">{{ t('turbo.safety.columns.protection') }}</th><th class="p-3"><span class="sr-only">{{ t('turbo.safety.columns.actions') }}</span></th></tr></thead>
          <tbody><tr v-for="record in blocks" :key="record.fingerprint" class="border-t">
            <td class="p-3"><p class="font-medium">{{ record.application }}</p><p class="text-xs text-muted">{{ record.runtime }}<template v-if="record.systemName"> · {{ record.systemName }}</template></p></td>
            <td class="p-3 text-xs">{{ date(record.updatedAtUnixMilliseconds) }}</td>
            <td class="p-3"><span class="rounded-full px-2 py-1 text-xs" :class="turboSafetyApplies(savedConfig, record.application) ? 'chip-warning' : 'chip-idle'">{{ turboSafetyApplies(savedConfig, record.application) ? t('turbo.safety.blocked.blocked') : t('turbo.safety.blocked.bypassed') }}</span></td>
            <td class="p-3"><div class="flex flex-wrap gap-2">
              <button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" @click="selected = record">{{ t('turbo.safety.blocked.details') }}</button>
              <button type="button" class="button-accent rounded-lg px-3 py-1.5 text-xs" :disabled="clearing !== null || clearingLogs" @click="emit('clear', record.fingerprint)">{{ clearing === record.fingerprint ? t('turbo.safety.blocked.clearing') : t('turbo.safety.blocked.clearRetest') }}</button>
            </div></td>
          </tr></tbody>
        </table>
        <p v-else class="p-4 text-sm text-muted">{{ t('turbo.safety.blocked.empty') }}</p>
      </div>
    </section>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <h3 class="text-base font-semibold">{{ t('turbo.safety.faults.title') }} <span class="text-sm text-muted">({{ status.faults.length }})</span></h3>
        <button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" :disabled="clearingLogs || clearing !== null || !status.faults.length" @click="selected = null; emit('clearLogs')">{{ clearingLogs ? t('turbo.safety.faults.clearing') : t('turbo.safety.faults.clearLogs') }}</button>
      </div>
      <p class="mt-1 text-sm text-muted">{{ t('turbo.safety.faults.intro') }}</p>
      <p class="mt-1 text-xs text-muted">{{ t('turbo.safety.faults.clearHint') }}</p>
      <div class="mt-3 max-h-80 overflow-auto rounded-lg border" tabindex="0" :aria-label="t('turbo.safety.faults.ariaLabel')">
        <table v-if="status.faults.length" class="w-full table-fixed text-left text-sm">
          <thead class="sticky top-0 surface-panel-strong"><tr><th class="w-44 p-3">{{ t('turbo.safety.columns.recorded') }}</th><th class="w-48 p-3">{{ t('turbo.safety.columns.application') }}</th><th class="p-3">{{ t('turbo.safety.columns.fault') }}</th><th class="w-24 p-3"><span class="sr-only">{{ t('turbo.safety.columns.details') }}</span></th></tr></thead>
          <tbody><tr v-for="(record, index) in status.faults" :key="record.fingerprint + record.updatedAtUnixMilliseconds + ':' + index" class="border-t">
            <td class="p-3 text-xs">{{ date(record.updatedAtUnixMilliseconds) }}</td>
            <td class="p-3"><p class="truncate font-medium" :title="record.application">{{ record.application }}</p><p class="truncate text-xs text-muted" :title="record.runtime">{{ record.runtime }}</p></td>
            <td class="truncate p-3" :title="turboFaultReason(record)">{{ turboFaultReason(record) }}</td>
            <td class="p-3"><button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" @click="selected = record">{{ t('turbo.safety.faults.details') }}</button></td>
          </tr></tbody>
        </table>
        <p v-else class="p-4 text-sm text-muted">{{ t('turbo.safety.faults.empty') }}</p>
      </div>
    </section>

    <div v-if="selected" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 p-6 backdrop-blur-sm" @click.self="selected = null">
      <section role="dialog" aria-modal="true" aria-labelledby="turbo-fault-title" class="max-h-[80vh] w-full max-w-2xl overflow-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex items-start justify-between gap-4"><h3 id="turbo-fault-title" class="text-xl font-semibold">{{ t('turbo.safety.dialog.title', { application: selected.application }) }}</h3><button type="button" class="button-secondary rounded-lg px-3 py-2 text-sm" @click="selected = null">{{ t('common.close') }}</button></div>
        <p class="mt-4 rounded-lg p-3 text-sm leading-6 chip-warning">{{ turboFaultReason(selected) }}</p>
        <dl class="mt-4 grid grid-cols-2 gap-4 text-sm">
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.recorded') }}</dt><dd>{{ date(selected.updatedAtUnixMilliseconds) }}</dd></div>
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.pacingMode') }}</dt><dd>{{ selected.mode || t('common.unknown') }}</dd></div>
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.runtime') }}</dt><dd>{{ selected.runtime }} {{ selected.runtimeVersion }}</dd></div>
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.headset') }}</dt><dd>{{ selected.systemName || t('turbo.safety.dialog.notRecorded') }}</dd></div>
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.graphicsApi') }}</dt><dd>{{ selected.graphicsApi || t('turbo.safety.dialog.notRecorded') }}</dd></div>
          <div><dt class="text-muted">{{ t('turbo.safety.dialog.processId') }}</dt><dd>{{ selected.processId ?? t('turbo.safety.dialog.notRecorded') }}</dd></div>
        </dl>
        <p v-if="selected.state === 'armed'" class="mt-4 text-xs text-muted">{{ t('turbo.safety.dialog.armedNote') }}</p>
        <p v-else-if="selected.state === 'recovered'" class="mt-4 text-xs text-muted">{{ t('turbo.safety.dialog.recoveredNote') }}</p>
      </section>
    </div>
  </article>
</template>
