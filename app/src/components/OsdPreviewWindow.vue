<script setup lang="ts">
import { onUnmounted, ref } from 'vue'
import { isTauri } from '@tauri-apps/api/core'
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow'
import { defaultOsdSettings, normalizeOsdSettings } from '../lib/model'
import OsdPreview from './OsdPreview.vue'

const settings = ref(defaultOsdSettings())
const ready = ref(false)
const legible = ref(false)
const channel = new BroadcastChannel(new URLSearchParams(location.search).get('osd-preview')!)
channel.onmessage = ({ data }) => {
  if (data?.type !== 'settings') return
  settings.value = normalizeOsdSettings(data.settings)
  legible.value = data.legible === true
  document.documentElement.dataset.theme = data.theme === 'dark' ? 'dark' : 'light'
  ready.value = true
}
channel.postMessage({ type: 'ready' })
function position(horizontal: number, vertical: number) { channel.postMessage({ type: 'position', horizontal, vertical }) }
function layout(compact: boolean) {
  settings.value.compact = compact
  channel.postMessage({ type: 'view', compact })
}
function closed() { channel.postMessage({ type: 'closed' }) }
function close() { if (isTauri()) void getCurrentWebviewWindow().close(); else window.close() }
window.addEventListener('beforeunload', closed)
onUnmounted(() => { closed(); channel.close(); window.removeEventListener('beforeunload', closed) })
</script>

<template>
  <main class="preview-window surface-panel">
    <header class="flex items-center justify-between gap-3 p-4">
      <h1 class="text-sm font-semibold">On Screen Display Preview</h1>
      <button class="button-secondary rounded-lg px-3 py-2 text-xs" @click="close">Close preview</button>
    </header>
    <div v-if="ready" class="flex items-center gap-2 px-5 pb-3">
      <span class="mr-auto text-xs text-muted">Preview layout</span>
      <button v-for="compact in [true, false]" :key="String(compact)" :aria-pressed="settings.compact === compact" :class="settings.compact === compact ? 'button-accent' : 'button-secondary'" class="rounded-lg px-3 py-2 text-xs" @click="layout(compact)">{{ compact ? 'Compact' : 'Expanded' }}</button>
    </div>
    <OsdPreview v-if="ready" v-model:legible="legible" :settings="settings" @update:legible="channel.postMessage({ type: 'view', legible: $event })" @position="position" />
    <p v-else class="p-5 text-sm text-muted">Connecting to OSD controls…</p>
  </main>
</template>

<style scoped>
.preview-window { height: 100vh; display: flex; flex-direction: column; }
.preview-window :deep(.osd-preview) { flex: 1; min-height: 0; }
.preview-window :deep(.osd-stage) { flex: 1; min-height: 0; }
</style>
