<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import type { LogSnapshot } from '../lib/commands'

const props = defineProps<{
  open: boolean
  loading: boolean
  snapshot: LogSnapshot | null
}>()

defineEmits<{
  close: []
}>()

const selectedPath = ref('')

watch(
  () => props.snapshot,
  (snapshot) => {
    selectedPath.value = snapshot?.files[0]?.path ?? ''
  },
  { immediate: true },
)

const selectedFile = computed(() => props.snapshot?.files.find((file) => file.path === selectedPath.value) ?? props.snapshot?.files[0] ?? null)
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex max-h-[85vh] w-full max-w-[1180px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex flex-wrap items-start justify-between gap-4 border-b px-6 py-5" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Logs</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">VectorXR Log Viewer</h2>
          <p class="mt-2 text-sm text-muted">Recent layer logs are shown read-only so the runtime state stays easy to inspect.</p>
        </div>

        <button
          class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="$emit('close')"
        >
          Close
        </button>
      </header>

      <div v-if="loading" class="px-6 py-8 text-sm text-muted">Loading logs...</div>

      <div v-else class="grid min-h-0 flex-1 gap-0 lg:grid-cols-[280px_minmax(0,1fr)]">
        <aside class="border-r p-4" style="border-color: var(--app-border)">
          <div class="rounded-[1rem] border p-4 text-sm surface-panel-soft">
            <p class="font-medium">Log Directory</p>
            <p class="mt-2 break-all font-mono text-xs">{{ snapshot?.directory || 'Unavailable' }}</p>
            <p class="mt-4 font-medium">Active Log Path</p>
            <p class="mt-2 break-all font-mono text-xs">{{ snapshot?.activePath || 'Unavailable' }}</p>
          </div>

          <div class="mt-4 space-y-2">
            <button
              v-for="file in snapshot?.files ?? []"
              :key="file.path"
              class="w-full rounded-[0.9rem] border px-3 py-3 text-left transition"
              :class="selectedFile?.path === file.path ? 'tab-button-active' : 'tab-button-idle'"
              type="button"
              @click="selectedPath = file.path"
            >
              <p class="text-sm font-medium tracking-tight">{{ file.name }}</p>
              <p class="mt-1 text-[11px] font-mono" :class="selectedFile?.path === file.path ? 'text-inverse-muted' : 'text-muted'">
                {{ file.path }}
              </p>
            </button>
          </div>

          <div v-if="(snapshot?.files.length ?? 0) === 0" class="mt-4 rounded-[0.9rem] border border-dashed px-4 py-5 text-sm surface-panel-soft">
            No log files were found yet. Start the layer in a title and reopen this viewer.
          </div>
        </aside>

        <section class="min-h-0 p-4 surface-panel-soft">
          <div class="h-full rounded-[1rem] border p-4 surface-panel-inverse">
            <p class="text-xs uppercase tracking-[0.22em] text-inverse-muted">Selected Log</p>
            <p class="mt-2 break-all font-mono text-xs text-inverse-muted">{{ selectedFile?.path || 'No log selected' }}</p>

            <pre class="mt-4 h-[55vh] overflow-auto rounded-[0.9rem] border border-white/10 bg-black/20 p-4 font-mono text-xs leading-6 text-white/88 whitespace-pre-wrap">{{ selectedFile?.content || 'No log content available.' }}</pre>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>
