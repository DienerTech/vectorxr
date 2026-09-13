<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import type { RuntimeStatusEnvelope, TurboRecoveryRecord } from '../lib/commands'
import type { VectorXRConfig } from '../lib/model'
import { turboFaultReason, turboSafetyApplies, turboSafetyBlocks } from '../lib/turboSafety'
import TurboSafetyToggle from './TurboSafetyToggle.vue'

const props = defineProps<{ config: VectorXRConfig; savedConfig: VectorXRConfig; status: RuntimeStatusEnvelope; error: string; clearing: string | null; clearingLogs: boolean }>()
const emit = defineEmits<{ close: []; clear: [fingerprint: string]; clearLogs: [] }>()
const blocks = computed(() => turboSafetyBlocks(props.status.recovery))
const selected = ref<TurboRecoveryRecord | null>(null)
function date(value: number) { return value ? new Date(value).toLocaleString() : 'Unknown' }
function sessionState(state?: string) {
  return ({ 'recovery-disabled': 'Turbo enabled · off after safety block', suspended: 'Turbo enabled · suspended after a fault',
    async: 'Turbo active · Async', sequenced: 'Turbo active · Sequenced', waiting: 'Turbo waiting to engage', off: 'Turbo off' } as Record<string, string>)[state ?? ''] ?? 'Status unavailable'
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
        <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
          <button class="underline-offset-2 hover:underline" type="button" @click="emit('close')">Turbo</button><span aria-hidden="true">›</span><span>Turbo Safety</span>
        </nav>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">Turbo Safety</h2>
        <p class="mt-1 max-w-2xl text-sm leading-6 text-muted">Review blocked application and runtime setups, retry Turbo, and inspect recorded faults.</p>
      </div>
      <button type="button" class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" @click="emit('close')">Back to Turbo</button>
    </header>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <h3 class="text-base font-semibold">Automatic protection</h3>
        <TurboSafetyToggle v-model="config.modules.turbo.interruptedSessionRecovery" />
      </div>
      <p class="mt-2 text-sm leading-6 text-muted">An interrupted session or repeated runtime fault can hold Turbo off for that application and runtime setup. Turbo stays enabled in its profile, so its in-game binding can retry even with this UI closed.</p>
      <p class="mt-2 text-sm leading-6 text-muted">Save setting changes to apply them. Custom profiles can bypass safety for selected applications. Live pacing fallback still suspends a failing session; the binding can re-arm it.</p>
      <div v-for="session in status.sessions" :key="session.sessionId" class="mt-3 rounded-lg border p-3 text-sm" role="status">
        <p class="font-medium">{{ session.application }} · {{ sessionState(session.state.turboState) }}</p>
        <p v-if="session.state.turboReason" class="mt-1 text-xs text-muted">{{ session.state.turboReason }}</p>
      </div>
    </section>
    <p v-if="error" class="rounded-lg p-3 text-sm chip-danger" role="alert">{{ error }}</p>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <h3 class="text-base font-semibold">Blocked setups <span class="text-sm text-muted">({{ blocks.length }})</span></h3>
      <p class="mt-1 text-sm leading-6 text-muted">Clear &amp; retest preserves faults and clears the runtime's learned pacing decision. Relaunch in Auto without a manual override for a fresh Async test, followed by Sequenced if needed. A running session can retry its current mode; a full retest requires a relaunch.</p>
      <div class="mt-3 max-h-72 overflow-auto rounded-lg border" tabindex="0" aria-label="Blocked Turbo setups">
        <table v-if="blocks.length" class="w-full text-left text-sm">
          <thead class="sticky top-0 surface-panel-strong"><tr><th class="p-3">Application / runtime</th><th class="p-3">Recorded</th><th class="p-3">Protection</th><th class="p-3"><span class="sr-only">Actions</span></th></tr></thead>
          <tbody><tr v-for="record in blocks" :key="record.fingerprint" class="border-t">
            <td class="p-3"><p class="font-medium">{{ record.application }}</p><p class="text-xs text-muted">{{ record.runtime }}<template v-if="record.systemName"> · {{ record.systemName }}</template></p></td>
            <td class="p-3 text-xs">{{ date(record.updatedAtUnixMilliseconds) }}</td>
            <td class="p-3"><span class="rounded-full px-2 py-1 text-xs" :class="turboSafetyApplies(savedConfig, record.application) ? 'chip-warning' : 'chip-idle'">{{ turboSafetyApplies(savedConfig, record.application) ? 'Blocked' : 'Bypassed' }}</span></td>
            <td class="p-3"><div class="flex flex-wrap gap-2">
              <button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" @click="selected = record">Details</button>
              <button type="button" class="button-accent rounded-lg px-3 py-1.5 text-xs" :disabled="clearing !== null || clearingLogs" @click="emit('clear', record.fingerprint)">{{ clearing === record.fingerprint ? 'Clearing…' : 'Clear & retest' }}</button>
            </div></td>
          </tr></tbody>
        </table>
        <p v-else class="p-4 text-sm text-muted">No recorded safety blocks.</p>
      </div>
    </section>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <h3 class="text-base font-semibold">Fault log <span class="text-sm text-muted">({{ status.faults.length }})</span></h3>
        <button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" :disabled="clearingLogs || clearing !== null || !status.faults.length" @click="selected = null; emit('clearLogs')">{{ clearingLogs ? 'Clearing…' : 'Clear Logs' }}</button>
      </div>
      <p class="mt-1 text-sm text-muted">Recorded faults remain here after a retry. For an unclean exit, the recorded time is the last safety marker, not a confirmed crash time.</p>
      <p class="mt-1 text-xs text-muted">Clear Logs removes fault history. Safety blocks and learned pacing decisions stay in place.</p>
      <div class="mt-3 max-h-80 overflow-auto rounded-lg border" tabindex="0" aria-label="Turbo fault log">
        <table v-if="status.faults.length" class="w-full table-fixed text-left text-sm">
          <thead class="sticky top-0 surface-panel-strong"><tr><th class="w-44 p-3">Recorded</th><th class="w-48 p-3">Application / runtime</th><th class="p-3">Fault</th><th class="w-24 p-3"><span class="sr-only">Details</span></th></tr></thead>
          <tbody><tr v-for="(record, index) in status.faults" :key="record.fingerprint + record.updatedAtUnixMilliseconds + ':' + index" class="border-t">
            <td class="p-3 text-xs">{{ date(record.updatedAtUnixMilliseconds) }}</td>
            <td class="p-3"><p class="truncate font-medium" :title="record.application">{{ record.application }}</p><p class="truncate text-xs text-muted" :title="record.runtime">{{ record.runtime }}</p></td>
            <td class="truncate p-3" :title="turboFaultReason(record)">{{ turboFaultReason(record) }}</td>
            <td class="p-3"><button type="button" class="button-secondary rounded-lg px-3 py-1.5 text-xs" @click="selected = record">Details</button></td>
          </tr></tbody>
        </table>
        <p v-else class="p-4 text-sm text-muted">No Turbo faults recorded.</p>
      </div>
    </section>

    <div v-if="selected" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 p-6 backdrop-blur-sm" @click.self="selected = null">
      <section role="dialog" aria-modal="true" aria-labelledby="turbo-fault-title" class="max-h-[80vh] w-full max-w-2xl overflow-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex items-start justify-between gap-4"><h3 id="turbo-fault-title" class="text-xl font-semibold">{{ selected.application }} · Turbo fault</h3><button type="button" class="button-secondary rounded-lg px-3 py-2 text-sm" @click="selected = null">Close</button></div>
        <p class="mt-4 rounded-lg p-3 text-sm leading-6 chip-warning">{{ turboFaultReason(selected) }}</p>
        <dl class="mt-4 grid grid-cols-2 gap-4 text-sm">
          <div><dt class="text-muted">Recorded</dt><dd>{{ date(selected.updatedAtUnixMilliseconds) }}</dd></div>
          <div><dt class="text-muted">Pacing mode</dt><dd>{{ selected.mode || 'Unknown' }}</dd></div>
          <div><dt class="text-muted">Runtime</dt><dd>{{ selected.runtime }} {{ selected.runtimeVersion }}</dd></div>
          <div><dt class="text-muted">Headset</dt><dd>{{ selected.systemName || 'Not recorded' }}</dd></div>
          <div><dt class="text-muted">Graphics API</dt><dd>{{ selected.graphicsApi || 'Not recorded' }}</dd></div>
          <div><dt class="text-muted">Process ID</dt><dd>{{ selected.processId ?? 'Not recorded' }}</dd></div>
        </dl>
        <p v-if="selected.state === 'armed'" class="mt-4 text-xs text-muted">An unclean exit was inferred from a stopped process. This does not establish that Turbo caused a crash.</p>
      </section>
    </div>
  </article>
</template>
