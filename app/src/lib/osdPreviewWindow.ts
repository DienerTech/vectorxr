import { onUnmounted, ref, watch, type Ref } from 'vue'
import { isTauri } from '@tauri-apps/api/core'
import { WebviewWindow } from '@tauri-apps/api/webviewWindow'
import { osdLayout, type OsdSettings } from './model'

export function useOsdPreviewWindow(settings: Ref<OsdSettings>) {
  const opened = ref(false)
  const error = ref('')
  const channelId = `osd-preview-${crypto.randomUUID()}`
  const channel = new BroadcastChannel(channelId)
  let nativeWindow: WebviewWindow | null = null
  let browserWindow: Window | null = null
  let disposed = false
  const publish = () => channel.postMessage({ type: 'settings', settings: osdLayout(settings.value), theme: document.documentElement.dataset.theme })
  channel.onmessage = ({ data }) => {
    if (data?.type === 'ready') publish()
    if (data?.type === 'closed') { opened.value = false; nativeWindow = null; browserWindow = null }
    if (data?.type === 'position' && Number.isFinite(data.horizontal) && Number.isFinite(data.vertical)) {
      settings.value.horizontalDegrees = Math.round(Math.max(-40, Math.min(40, data.horizontal)))
      settings.value.verticalDegrees = Math.round(Math.max(-35, Math.min(35, data.vertical)))
    }
  }
  watch(settings, publish, { deep: true })
  const themeObserver = new MutationObserver(publish)
  themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] })
  function close() {
    void nativeWindow?.close().catch(() => {})
    browserWindow?.close()
  }
  async function open() {
    error.value = ''
    if (opened.value) {
      try { await nativeWindow?.setFocus(); browserWindow?.focus() } catch { opened.value = false }
      if (opened.value) return
    }
    const url = `/?osd-preview=${channelId}`
    opened.value = true
    if (isTauri()) {
      const preview = new WebviewWindow(channelId, { url, title: 'VectorXR — OSD Preview', width: 760, height: 640, minWidth: 480, minHeight: 400, alwaysOnTop: true, dragDropEnabled: false })
      nativeWindow = preview
      await preview.once('tauri://created', () => { if (disposed) void preview.close() })
      await preview.once('tauri://error', () => { opened.value = false; nativeWindow = null; error.value = 'Could not open the preview window. Please try again.' })
    } else {
      browserWindow = window.open(url, channelId, 'popup,width=760,height=640')
      if (!browserWindow) { opened.value = false; error.value = 'Allow pop-ups to open the preview window.' }
    }
  }
  window.addEventListener('beforeunload', close)
  onUnmounted(() => { disposed = true; close(); channel.close(); themeObserver.disconnect(); window.removeEventListener('beforeunload', close) })
  return { opened, error, open }
}
