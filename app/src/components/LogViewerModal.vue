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
    <div class="flex max-h-[85vh] w-full max-w-[1180px] flex-col overflow-hidden rounded-[2rem] border border-black/10 bg-[#f7f3ea] shadow-[0_30px_120px_rgba(17,19,22,0.38)]">
      <header class="flex flex-wrap items-start justify-between gap-4 border-b border-black/10 bg-white/75 px-6 py-5">
        <div>
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Logs</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight text-depthxr-pine">VectorXR Log Viewer</h2>
          <p class="mt-2 text-sm text-depthxr-steel">Recent layer logs are shown read-only so we can inspect runtime state without leaving the app.</p>
        </div>

        <button
          class="rounded-full border border-black/10 px-4 py-2 text-sm font-medium text-depthxr-ink transition hover:border-depthxr-copper hover:text-depthxr-copper"
          type="button"
          @click="$emit('close')"
        >
          Close
        </button>
      </header>

      <div v-if="loading" class="px-6 py-8 text-sm text-depthxr-steel">Loading logs...</div>

      <div v-else class="grid min-h-0 flex-1 gap-0 lg:grid-cols-[280px_minmax(0,1fr)]">
        <aside class="border-r border-black/10 bg-white/60 p-4">
          <div class="rounded-[1.4rem] border border-black/10 bg-[#fbf7ef] p-4 text-sm text-depthxr-steel">
            <p class="font-medium text-depthxr-pine">Log Directory</p>
            <p class="mt-2 break-all font-mono text-xs">{{ snapshot?.directory || 'Unavailable' }}</p>
            <p class="mt-4 font-medium text-depthxr-pine">Active Log Path</p>
            <p class="mt-2 break-all font-mono text-xs">{{ snapshot?.activePath || 'Unavailable' }}</p>
          </div>

          <div class="mt-4 space-y-2">
            <button
              v-for="file in snapshot?.files ?? []"
              :key="file.path"
              class="w-full rounded-[1.2rem] border px-3 py-3 text-left transition"
              :class="
                selectedFile?.path === file.path
                  ? 'border-depthxr-pine bg-depthxr-pine text-white'
                  : 'border-black/10 bg-white text-depthxr-ink hover:border-depthxr-copper hover:bg-[#f8efe4]'
              "
              type="button"
              @click="selectedPath = file.path"
            >
              <p class="text-sm font-medium tracking-tight">{{ file.name }}</p>
              <p class="mt-1 text-[11px] font-mono" :class="selectedFile?.path === file.path ? 'text-white/70' : 'text-depthxr-steel'">
                {{ file.path }}
              </p>
            </button>
          </div>

          <div v-if="(snapshot?.files.length ?? 0) === 0" class="mt-4 rounded-[1.2rem] border border-dashed border-black/15 bg-white/60 px-4 py-5 text-sm text-depthxr-steel">
            No log files were found yet. Start the layer in a title and reopen this viewer.
          </div>
        </aside>

        <section class="min-h-0 bg-[#f3eee2] p-4">
          <div class="h-full rounded-[1.6rem] border border-black/10 bg-[#1e2724] p-4 text-white">
            <p class="text-xs uppercase tracking-[0.22em] text-depthxr-sand">Selected Log</p>
            <p class="mt-2 break-all font-mono text-xs text-white/72">{{ selectedFile?.path || 'No log selected' }}</p>

            <pre class="mt-4 h-[55vh] overflow-auto rounded-[1.2rem] border border-white/10 bg-black/20 p-4 font-mono text-xs leading-6 text-white/88 whitespace-pre-wrap">{{ selectedFile?.content || 'No log content available.' }}</pre>
          </div>
        </section>
      </div>
    </div>
  </div>
</template>
