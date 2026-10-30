import { createApp } from 'vue'
import App from './App.vue'
import OsdPreviewWindow from './components/OsdPreviewWindow.vue'
import { firstRunLanguagePreference } from './i18n/firstRun.ts'
import { applyLanguagePreference } from './i18n/load.ts'
import { loadLanguagePreference } from './i18n/preference.ts'
import './style.css'

// The language loads before the first render, so the window never flashes
// English first.
async function start() {
  const preference = loadLanguagePreference() ?? await firstRunLanguagePreference()
  await applyLanguagePreference(preference)
  createApp(new URLSearchParams(location.search).has('osd-preview') ? OsdPreviewWindow : App).mount('#app')
}

void start()
