<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import { openFileDirectory, type LogSnapshot } from '../lib/commands'

const props = defineProps<{
  open: boolean
  loading: boolean
  snapshot: LogSnapshot | null
}>()

defineEmits<{
  close: []
}>()

const selectedPath = ref('')
const copyStatus = ref<'idle' | 'copied' | 'failed'>('idle')
const openStatus = ref<'idle' | 'opened' | 'failed'>('idle')
let copyStatusTimeout: number | undefined
let openStatusTimeout: number | undefined

watch(
  () => props.snapshot,
  (snapshot) => {
    selectedPath.value = snapshot?.files[0]?.path ?? ''
    resetActionStatus()
  },
  { immediate: true },
)

const sortedFiles = computed(() =>
  [...(props.snapshot?.files ?? [])].sort((lhs, rhs) => rhs.modifiedUnixSeconds - lhs.modifiedUnixSeconds),
)
const selectedFile = computed(() => sortedFiles.value.find((file) => file.path === selectedPath.value) ?? sortedFiles.value[0] ?? null)

function selectLog(path: string) {
  selectedPath.value = path
  resetActionStatus()
}

function resetActionStatus() {
  copyStatus.value = 'idle'
  openStatus.value = 'idle'
  if (copyStatusTimeout !== undefined) {
    window.clearTimeout(copyStatusTimeout)
    copyStatusTimeout = undefined
  }
  if (openStatusTimeout !== undefined) {
    window.clearTimeout(openStatusTimeout)
    openStatusTimeout = undefined
  }
}

function flashCopyStatus(status: 'copied' | 'failed') {
  copyStatus.value = status
  if (copyStatusTimeout !== undefined) {
    window.clearTimeout(copyStatusTimeout)
  }
  copyStatusTimeout = window.setTimeout(() => {
    copyStatus.value = 'idle'
    copyStatusTimeout = undefined
  }, 1600)
}

function flashOpenStatus(status: 'opened' | 'failed') {
  openStatus.value = status
  if (openStatusTimeout !== undefined) {
    window.clearTimeout(openStatusTimeout)
  }
  openStatusTimeout = window.setTimeout(() => {
    openStatus.value = 'idle'
    openStatusTimeout = undefined
  }, 1600)
}

async function copySelectedLog() {
  if (!selectedFile.value?.content) {
    return
  }

  try {
    await navigator.clipboard.writeText(selectedFile.value.content)
    flashCopyStatus('copied')
  } catch {
    flashCopyStatus('failed')
  }
}

async function openSelectedLogDirectory() {
  if (!selectedFile.value?.path) {
    return
  }

  try {
    await openFileDirectory(selectedFile.value.path)
    flashOpenStatus('opened')
  } catch {
    flashOpenStatus('failed')
  }
}
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex h-[85vh] w-full max-w-[1180px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex shrink-0 flex-wrap items-start justify-between gap-4 border-b px-6 py-5" style="border-color: var(--app-border)">
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

      <div v-else class="flex min-h-0 flex-1 flex-col">
        <div class="shrink-0 border-b px-6 py-4" style="border-color: var(--app-border)">
          <div class="rounded-[1rem] border px-4 py-3 text-sm surface-panel-soft">
            <p class="font-medium">Log Directory</p>
            <p class="mt-2 break-all font-mono text-xs">{{ snapshot?.directory || 'Unavailable' }}</p>
          </div>
        </div>

        <div class="grid min-h-0 flex-1 gap-0 lg:grid-cols-[280px_minmax(0,1fr)]">
          <aside class="flex min-h-0 flex-col border-r p-4" style="border-color: var(--app-border)">
            <p class="mb-3 text-xs font-semibold uppercase tracking-[0.18em] text-muted">VectorXR Logs</p>

            <div class="min-h-0 flex-1 space-y-2 overflow-y-auto pr-1">
              <button
                v-for="file in sortedFiles"
                :key="file.path"
                class="w-full rounded-[0.9rem] border px-3 py-2.5 text-left transition"
                :class="selectedFile?.path === file.path ? 'tab-button-active' : 'tab-button-idle'"
                type="button"
                @click="selectLog(file.path)"
              >
                <p class="text-sm font-medium tracking-tight">{{ file.name }}</p>
              </button>
            </div>

            <div v-if="sortedFiles.length === 0" class="mt-4 rounded-[0.9rem] border border-dashed px-4 py-5 text-sm surface-panel-soft">
              No log files were found yet. Start the layer in a title and reopen this viewer.
            </div>
          </aside>

          <section class="min-h-0 p-4 surface-panel-soft">
            <div class="flex h-full min-h-0 flex-col rounded-[1rem] border p-4 surface-panel-inverse">
              <div class="flex flex-wrap items-start justify-between gap-3">
                <div class="min-w-0">
                  <p class="text-xs uppercase tracking-[0.22em] text-inverse-muted">Selected Log</p>
                  <p class="mt-2 break-all font-mono text-xs text-inverse-muted">{{ selectedFile?.path || 'No log selected' }}</p>
                </div>

                <div class="flex shrink-0 flex-wrap gap-2">
                  <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-xs font-medium disabled:cursor-not-allowed disabled:opacity-50" :disabled="!selectedFile" type="button" @click="copySelectedLog">
                    {{ copyStatus === 'copied' ? 'Copied!' : copyStatus === 'failed' ? 'Copy Failed' : 'Copy Text' }}
                  </button>
                  <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-xs font-medium disabled:cursor-not-allowed disabled:opacity-50" :disabled="!selectedFile" type="button" @click="openSelectedLogDirectory">
                    {{ openStatus === 'opened' ? 'Opened!' : openStatus === 'failed' ? 'Open Failed' : 'Open Directory' }}
                  </button>
                </div>
              </div>

              <pre class="mt-4 min-h-0 flex-1 overflow-auto rounded-[0.9rem] border border-white/10 bg-black/20 p-4 font-mono text-xs leading-6 text-white/88 whitespace-pre">{{ selectedFile?.content || 'No log content available.' }}</pre>
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
