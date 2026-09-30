import { createApp } from 'vue'
import App from './App.vue'
import OsdPreviewWindow from './components/OsdPreviewWindow.vue'
import './style.css'

createApp(new URLSearchParams(location.search).has('osd-preview') ? OsdPreviewWindow : App).mount('#app')
