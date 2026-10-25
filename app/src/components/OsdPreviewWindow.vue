<script setup lang="ts">
import { onUnmounted, ref } from 'vue'
import { isTauri } from '@tauri-apps/api/core'
import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow'
import { defaultOsdSettings, normalizeOsdSettings } from '../lib/model'
import OsdPreview from './OsdPreview.vue'
import { isAppLocale } from '../i18n/locales.ts'
import { t, type AppLocale } from '../i18n/index.ts'

const settings = ref(defaultOsdSettings())
const ready = ref(false)
const legible = ref(false)
const pivotSnapLine = ref(false)
const diagnostics = ref('')
const locale = ref<AppLocale | undefined>()
const channel = new BroadcastChannel(new URLSearchParams(location.search).get('osd-preview')!)
channel.onmessage = ({ data }) => {
  if (data?.type !== 'settings') return
  settings.value = normalizeOsdSettings(data.settings)
  legible.value = data.legible === true
  pivotSnapLine.value = data.pivotSnapLine === true
  diagnostics.value = typeof data.diagnostics === 'string' ? data.diagnostics : ''
  locale.value = isAppLocale(data.locale) ? data.locale : undefined
  document.documentElement.dataset.theme = data.theme === 'dark' ? 'dark' : 'light'
  ready.value = true
}
channel.postMessage({ type: 'ready' })
function position(horizontal: number, vertical: number) { channel.postMessage({ type: 'position', horizontal, vertical }) }
function layout(compact: boolean) {
  settings.value.compact = compact
  channel.postMessage({ type: 'view', compact })
}
function setLegible(mode: boolean) {
  legible.value = mode
  channel.postMessage({ type: 'view', legible: mode })
}
function closed() { channel.postMessage({ type: 'closed' }) }
function close() { if (isTauri()) void getCurrentWebviewWindow().close(); else window.close() }
window.addEventListener('beforeunload', closed)
onUnmounted(() => { closed(); channel.close(); window.removeEventListener('beforeunload', closed) })
</script>

<template>
  <main class="preview-window surface-panel">
    <header class="flex flex-wrap items-center gap-2 px-5 py-3">
      <h1 class="mr-auto text-sm font-semibold">{{ t('osd.previewWindow.title') }}</h1>
      <template v-if="ready">
        <div class="segmented" role="group" :aria-label="t('osd.layout.previewAria')">
          <button v-for="compact in [true, false]" :key="String(compact)" type="button" class="segmented-option" :class="{ 'segmented-option-active': settings.compact === compact }" :aria-pressed="settings.compact === compact" @click="layout(compact)">{{ compact ? t('osd.layout.compact') : t('osd.layout.expanded') }}</button>
        </div>
        <div class="segmented" role="group" :aria-label="t('osd.previewScale.aria')">
          <button v-for="mode in [false, true]" :key="String(mode)" type="button" class="segmented-option" :class="{ 'segmented-option-active': legible === mode }" :aria-pressed="legible === mode" @click="setLegible(mode)">{{ mode ? t('osd.previewScale.legible') : t('osd.previewScale.placement') }}</button>
        </div>
      </template>
      <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="close">{{ t('common.close') }}</button>
    </header>
    <OsdPreview v-if="ready" v-model:legible="legible" :settings="settings" :pivot-snap-line="pivotSnapLine" :diagnostics="diagnostics" :locale="locale" @position="position" />
    <p v-else class="p-5 text-sm text-muted">{{ t('osd.previewWindow.connecting') }}</p>
  </main>
</template>

<style scoped>
.preview-window { height: 100vh; display: flex; flex-direction: column; }
.preview-window :deep(.osd-preview) { flex: 1; min-height: 0; }
.preview-window :deep(.osd-stage) { flex: 1; min-height: 0; }
</style>
