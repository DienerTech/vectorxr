<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { useOsdPreviewWindow } from '../../lib/osdPreviewWindow'
import { osdCompactLayout } from '../../lib/osdCompactLayout'
import OsdColorPicker from '../OsdColorPicker.vue'
import OsdPreview from '../OsdPreview.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import BindingConflictWarnings from '../BindingConflictWarnings.vue'
import { bindingLabel, savedBindingConflictWarnings, osdAccentColors, osdBodyRows, osdLayout, type OsdBodyRow, type VectorXRConfig, type OsdSettings } from '../../lib/model'
import { loadRuntimeStatus, type RuntimeStatusSession } from '../../lib/commands'

const props = defineProps<{ config: VectorXRConfig; savedConfig: VectorXRConfig }>()
const osd = computed(() => props.config.core.osd)
const previewExpanded = ref(true)
const colorExpanded = ref(false)
const previewCompact = ref(osd.value.compact)
const previewSettings = computed(() => ({ ...osd.value, compact: previewCompact.value }))
const previewLegible = ref(false)
const previewWindow = useOsdPreviewWindow(osd, previewCompact, previewLegible)
watch(() => osd.value.compact, compact => { previewCompact.value = compact })
function customColor() { osd.value.accent = 'custom'; colorExpanded.value = !colorExpanded.value }
const editing = ref<'toggleBinding' | 'cycleBinding' | null>(null)
const warnings = computed(() => savedBindingConflictWarnings(props.config, [osd.value.toggleBinding, osd.value.cycleBinding]))
const sessions = ref<RuntimeStatusSession[]>([])
const statusError = ref('')
let disposed = false
let poll: ReturnType<typeof setTimeout> | undefined
async function refresh() {
  try {
    const status = await loadRuntimeStatus()
    if (!disposed) { sessions.value = status.sessions; statusError.value = '' }
  } catch { if (!disposed) statusError.value = 'Runtime status is temporarily unavailable.' }
  if (!disposed) poll = setTimeout(refresh, 2000)
}
onMounted(refresh)
onUnmounted(() => { disposed = true; if (poll) clearTimeout(poll) })

let savedScrollTop = 0
const scroller = () => document.querySelector('main section.overflow-y-auto')
function openBinding(binding: 'toggleBinding' | 'cycleBinding') {
  savedScrollTop = scroller()?.scrollTop ?? 0
  editing.value = binding
  void nextTick(() => scroller()?.scrollTo({ top: 0 }))
}
function closeBinding() {
  editing.value = null
  void nextTick(() => scroller()?.scrollTo({ top: savedScrollTop }))
}
const contents: Record<OsdBodyRow, { key: 'showGraph' | 'showTurbo' | 'showPivot' | 'showModules'; label: string; hint: string }> = {
  graph: { key: 'showGraph', label: 'Frame-time graph', hint: 'Rolling history of the last 120 application frames.' },
  turbo: { key: 'showTurbo', label: 'Turbo state', hint: 'Distinguishes disabled, enabled but off, active strategy, and safety states.' },
  pivot: { key: 'showPivot', label: 'Pivot state', hint: 'Engaged profile, applied nudges and quick views, or ready when idle.' },
  modules: { key: 'showModules', label: 'Active enhancements', hint: 'Depth, Pivot, Turbo, and Quadviews enabled for this app.' },
}
const compactContents = [
  { key: 'compactShowTurbo', label: 'Turbo state', hint: 'Current strategy, off, waiting, or safety state.' },
  { key: 'compactShowPivot', label: 'Pivot state', hint: 'Ready, applied, nudged, or quick view in a short label.' },
] as const
const compactHeaders = [
  { key: 'compactShowBrand', label: 'VectorXR name' },
  { key: 'compactShowApp', label: 'Application name' },
  { key: 'compactShowRuntime', label: 'OpenXR runtime' },
  { key: 'compactShowClock', label: 'Local clock' },
] as const
const metricChoices = { all: 'FPS + average frame time', fps: 'FPS only', frameTime: 'Average frame time only', none: 'Hidden' } as const
const compactEmpty = computed(() => osdCompactLayout(osd.value).empty)
const compactSummary = computed(() => [osd.value.compactMetrics !== 'none' ? metricChoices[osd.value.compactMetrics] : '',
  ...compactContents.filter(item => osd.value[item.key]).map(item => item.label.replace(' state', '')),
  compactHeaders.some(item => osd.value[item.key]) ? 'Header' : ''].filter(Boolean).join(' · ') || 'Nothing selected')
function fpsOnly() {
  Object.assign(osd.value, { compactMetrics: 'fps', compactShowBrand: false, compactShowApp: false,
    compactShowRuntime: false, compactShowClock: false, compactShowTurbo: false, compactShowPivot: false })
  previewCompact.value = true
}
function moveRow(row: OsdBodyRow, direction: number) {
  const order = [...osd.value.bodyOrder], index = order.indexOf(row), target = index + direction
  if (target < 0 || target >= order.length) return
  order.splice(index, 1); order.splice(target, 0, row); osd.value.bodyOrder = order
}
const draggedRow = ref<OsdBodyRow | null>(null)
const bodyList = ref<HTMLElement | null>(null)
let dragPointer: number | null = null
let dragStartY = 0
function startRowDrag(event: PointerEvent, row: OsdBodyRow) {
  if (event.button !== 0) return
  draggedRow.value = row; dragPointer = event.pointerId; dragStartY = event.clientY
  bodyList.value?.setPointerCapture(event.pointerId)
}
function dragRow(event: PointerEvent) {
  if (!draggedRow.value || event.pointerId !== dragPointer || Math.abs(event.clientY - dragStartY) < 4) return
  const rows = [...(bodyList.value?.querySelectorAll<HTMLElement>('[data-osd-row]') ?? [])]
  const to = rows.filter(element => {
    if (element.dataset.osdRow === draggedRow.value) return false
    const rect = element.getBoundingClientRect()
    return event.clientY > rect.top + rect.height / 2
  }).length
  const order = [...osd.value.bodyOrder], from = order.indexOf(draggedRow.value)
  if (from === to) return
  order.splice(from, 1); order.splice(to, 0, draggedRow.value)
  osd.value.bodyOrder = order
}
function endRowDrag() {
  draggedRow.value = null; dragPointer = null
}
const presetName = ref('')
const selectedPreset = ref('')
const presetMessage = ref('')
watch([() => props.savedConfig, () => props.config], () => { presetMessage.value = '' })
const existingPreset = computed(() => osd.value.customPresets.some(p => p.name.toLowerCase() === presetName.value.trim().toLowerCase()))
function savePreset() {
  const name = presetName.value.trim().slice(0, 60)
  if (!name) return
  const index = osd.value.customPresets.findIndex(p => p.name.toLowerCase() === name.toLowerCase())
  if (index < 0 && osd.value.customPresets.length >= 50) { presetMessage.value = 'You can save up to 50 presets.'; return }
  const preset = { name, settings: osdLayout(osd.value) }
  if (index >= 0) osd.value.customPresets.splice(index, 1, preset)
  else osd.value.customPresets.push(preset)
  selectedPreset.value = name; presetName.value = name
  presetMessage.value = `${name} ${index >= 0 ? 'updated' : 'added'}. Save Changes to keep it.`
}
function loadPreset() {
  const preset = osd.value.customPresets.find(p => p.name === selectedPreset.value)
  if (!preset) return
  Object.assign(osd.value, { ...preset.settings, bodyOrder: [...preset.settings.bodyOrder] })
  previewCompact.value = osd.value.compact
  presetName.value = preset.name; presetMessage.value = `${preset.name} loaded. Save Changes to apply in VR.`
}
function deletePreset() {
  const name = selectedPreset.value
  osd.value.customPresets = osd.value.customPresets.filter(p => p.name !== name)
  selectedPreset.value = ''; presetName.value = ''; presetMessage.value = `${name} removed. Save Changes to keep this change.`
}
function preset(name: 'balanced' | 'minimal' | 'performance') {
  const shared: Partial<OsdSettings> = { opacity: 90, accent: 'teal', distanceMeters: 1.2, updateHz: 5,
    compactShowRuntime: true, compactShowClock: true, compactShowTurbo: false, compactShowPivot: false,
    compactShowBrand: true, compactShowApp: true, compactMetrics: 'all',
    showGraph: true, showRuntime: true, showBrand: true, showApp: true, showTurbo: true, showModules: true, showClock: true, showPivot: true, clockFormat: '24', bodyOrder: [...osdBodyRows] }
  const layout = name === 'minimal'
    ? { compact: true, horizontalDegrees: 0, verticalDegrees: -23, scale: 80 }
    : name === 'performance'
      ? { compact: false, horizontalDegrees: -20, verticalDegrees: -10, scale: 100, showRuntime: false, showModules: false, showPivot: false, accent: 'blue' as const, updateHz: 10 }
      : { compact: false, horizontalDegrees: 20, verticalDegrees: -12, scale: 100 }
  Object.assign(osd.value, shared, layout)
  previewCompact.value = osd.value.compact
  selectedPreset.value = ''
  presetMessage.value = `${name === 'balanced' ? 'Expanded' : name === 'performance' ? 'Performance' : 'Minimal'} applied. Save Changes to apply in VR.`
}
</script>

<template>
  <ModuleBindingPage v-if="editing" module-label="On-Screen Display" :binding="osd[editing]"
    :label="editing === 'toggleBinding' ? 'Show / hide display' : 'Switch compact / expanded'"
    description="Works in the VR application. Assign a keyboard chord, joystick button, or hat direction. Save to apply it live."
    :warnings="warnings" @update:binding="osd[editing!] = $event" @close="closeBinding" />
  <div v-else class="space-y-5">
    <article class="surface-panel rounded-[1.25rem] border p-5 shadow-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">In-headset</p>
          <h2 class="mt-1 text-2xl font-semibold tracking-tight">On-Screen Display</h2>
          <p class="mt-2 max-w-xl text-sm leading-6 text-muted">Keep an eye on performance without leaving VR. A head-following panel with the information you choose.</p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="osd.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" />
          {{ osd.enabled ? 'Enabled' : 'Enable OSD' }}
        </label>
      </div>
      <div class="mt-4 flex flex-wrap items-center gap-x-4 gap-y-2 text-xs text-muted">
        <span class="live-badge">Live after Save</span>
        <span>Direct3D 11 / 12 / Vulkan · Stereo &amp; Quadviews</span>
        <span>Show / hide: <strong class="font-medium">{{ bindingLabel(osd.toggleBinding) }}</strong></span>
      </div>
    </article>

    <article class="surface-panel overflow-hidden rounded-[1.25rem] border shadow-panel">
      <div class="flex flex-wrap items-center justify-between gap-3 px-5 py-4">
        <button class="flex items-center gap-2 font-semibold" :aria-expanded="previewExpanded" aria-controls="osd-preview-controls" @click="previewExpanded = !previewExpanded"><span aria-hidden="true">{{ previewExpanded ? '▾' : '▸' }}</span>On Screen Display Preview</button>
        <button class="button-secondary rounded-lg px-3 py-2 text-xs" @click="previewWindow.open">{{ previewWindow.opened.value ? 'Focus preview window' : 'Pop out preview' }}</button>
      </div>
      <p v-if="previewWindow.error.value" role="alert" class="px-5 pb-3 text-xs text-muted">{{ previewWindow.error.value }}</p>
      <div v-show="previewExpanded" id="osd-preview-controls">
        <div class="flex items-center gap-2 px-5 pb-3"><span class="mr-auto text-xs text-muted">Preview layout</span><button v-for="compact in [true, false]" :key="String(compact)" :aria-pressed="previewCompact === compact" :class="previewCompact === compact ? 'button-accent' : 'button-secondary'" class="rounded-lg px-3 py-2 text-xs" @click="previewCompact = compact">{{ compact ? 'Compact' : 'Expanded' }}</button></div>
        <OsdPreview v-if="!previewWindow.opened.value" v-model:legible="previewLegible" :settings="previewSettings" @position="(horizontal, vertical) => { osd.horizontalDegrees = horizontal; osd.verticalDegrees = vertical }" />
        <p v-else class="px-5 text-xs text-muted">Preview open in a separate window.</p>
        <div class="border-b p-5 preset-controls">
          <h3 class="mb-3 text-sm font-semibold">Position &amp; size</h3>
          <div class="grid gap-x-6 gap-y-3 sm:grid-cols-2">
          <label class="osd-control"><span>Horizontal <output>{{ osd.horizontalDegrees }}°</output></span><input v-model.number="osd.horizontalDegrees" aria-label="Horizontal position" type="range" min="-40" max="40" step="1" /></label>
          <label class="osd-control"><span>Vertical <output>{{ osd.verticalDegrees }}°</output></span><input v-model.number="osd.verticalDegrees" aria-label="Vertical position" type="range" min="-35" max="35" step="1" /></label>
          <label class="osd-control"><span>Size <output>{{ osd.scale }}%</output></span><input v-model.number="osd.scale" aria-label="Display size" type="range" min="25" max="150" step="5" /></label>
          <label class="osd-control"><span>Viewing distance <output>{{ osd.distanceMeters.toFixed(1) }} m</output></span><input v-model.number="osd.distanceMeters" aria-label="Viewing distance" type="range" min="0.5" max="3" step="0.1" /></label>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs" @click="osd.horizontalDegrees = 0; osd.verticalDegrees = 0">Center in view</button>
          </div>
        </div>
      </div>
      <div class="border-b p-5 preset-controls">
        <h3 class="text-sm font-semibold">Display presets</h3>
        <p class="mt-1 text-xs text-muted">Apply position, appearance, and display contents together.</p>
        <div class="mt-3 flex flex-wrap gap-2" aria-label="Display presets">
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('balanced')">Expanded</button>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('performance')">Performance</button>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('minimal')">Minimal</button>
        </div>
        <div class="mt-3 flex flex-wrap items-end gap-2">
          <label class="min-w-40 flex-1 text-xs">Saved custom presets<select v-model="selectedPreset" class="app-input manual-input-select mt-1 w-full rounded-lg px-3 py-2" @change="loadPreset"><option value="">Choose a preset…</option><option v-for="item in osd.customPresets" :key="item.name" :value="item.name">{{ item.name }}</option></select></label>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs disabled:opacity-40" :disabled="!selectedPreset" title="Reload selected preset into the controls" @click="loadPreset">Load</button>
          <label class="min-w-40 flex-1 text-xs">Preset name<input v-model="presetName" maxlength="60" class="app-input mt-1 w-full rounded-lg px-3 py-2" placeholder="My display" @keydown.enter.prevent="savePreset" /></label>
          <button class="button-secondary rounded-lg p-2.5 disabled:opacity-40" :disabled="!presetName.trim()" :title="existingPreset ? 'Update saved preset' : 'Save new preset'" :aria-label="existingPreset ? 'Update saved preset' : 'Save new preset'" @click="savePreset"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 3h13l4 4v14H3V3z M7 3v6h10V3 M7 21v-8h10v8" /></svg></button>
          <button class="button-secondary rounded-lg p-2.5 disabled:opacity-40" :disabled="!selectedPreset" title="Delete selected preset" aria-label="Delete selected preset" @click="deletePreset"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M3 6h18 M9 6V3h6v3 M5 6l1 15h12l1-15 M10 10v7 M14 10v7" /></svg></button>
        </div>
        <p v-if="presetMessage" class="mt-2 text-xs text-muted" role="status">{{ presetMessage }}</p>
      </div>
      <div class="p-5">
        <div class="space-y-4">
          <h3 class="text-sm font-semibold">Appearance</h3>
          <div><span class="mb-2 block text-xs text-muted">Starting layout</span><div class="flex gap-2"><button v-for="compact in [false, true]" :key="String(compact)" :aria-pressed="osd.compact === compact" :class="osd.compact === compact ? 'button-accent' : 'button-secondary'" class="flex-1 rounded-lg px-3 py-2 text-sm" @click="osd.compact = compact">{{ compact ? 'Compact' : 'Expanded' }}</button></div></div>
          <label class="osd-control"><span>Opacity <output>{{ osd.opacity }}%</output></span><input v-model.number="osd.opacity" aria-label="Display opacity" type="range" min="30" max="100" step="1" /></label>
          <div><span class="mb-2 block text-xs text-muted">Accent</span><div class="flex flex-wrap gap-2"><button v-for="(hex, color) in osdAccentColors" :key="color" class="color-choice rounded-lg border px-3 py-2 text-xs capitalize" :aria-pressed="osd.accent === color" :style="{ '--swatch': hex }" @click="osd.accent = color; colorExpanded = false"><i />{{ color }}</button><button class="color-choice rounded-lg border px-3 py-2 text-xs" :aria-pressed="osd.accent === 'custom'" :aria-expanded="colorExpanded" aria-controls="osd-custom-color" :style="{ '--swatch': osd.customColor }" @click="customColor"><i />Custom</button></div><OsdColorPicker v-if="colorExpanded" id="osd-custom-color" v-model="osd.customColor" class="mt-4" /></div>
          <label class="block text-sm"><span class="mb-2 block text-xs text-muted">Panel refresh rate</span><select v-model.number="osd.updateHz" class="app-input manual-input-select w-full rounded-lg px-3 py-2"><option v-for="hz in [1, 2, 5, 10, 20]" :key="hz" :value="hz">{{ hz }} Hz{{ hz === 5 ? ' · Recommended' : '' }}</option><option v-if="![1, 2, 5, 10, 20].includes(osd.updateHz)" :value="osd.updateHz">{{ osd.updateHz }} Hz</option></select><small class="mt-1 block text-muted">The frame history samples every frame. Higher refresh rates redraw the panel more often.</small></label>
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.visibleOnStart" type="checkbox" />Show automatically when enabled or a session starts</label>
        </div>
      </div>
    </article>

    <div class="grid gap-5 lg:grid-cols-2">
      <article class="surface-panel rounded-[1.25rem] border p-5">
        <h3 class="font-semibold">On your display</h3>
        <p class="mt-1 text-xs leading-5 text-muted">Keep Compact as small as you like. Expanded keeps FPS, average frame time, and P95 together.</p>
        <details class="osd-content-drawer mt-4 rounded-xl border" :open="osd.compact">
          <summary class="cursor-pointer p-4"><span class="font-medium">Compact</span><span class="mt-1 block text-xs text-muted">{{ compactSummary }}</span></summary>
          <div class="space-y-3 border-t p-4">
            <div class="flex items-center justify-between gap-3"><h4 class="text-sm font-semibold">Performance</h4><button class="button-secondary rounded-lg px-3 py-1.5 text-xs" title="Just FPS clears the header and status rows for a tiny display. Size still controls text size." @click="fpsOnly">Just FPS</button></div>
            <label class="block text-sm"><span class="sr-only">Compact performance values</span><select v-model="osd.compactMetrics" class="app-input manual-input-select w-full rounded-lg px-3 py-2"><option v-for="(label, value) in metricChoices" :key="value" :value="value">{{ label }}</option></select></label>
            <h4 class="pt-2 text-sm font-semibold">Header</h4>
            <div class="grid gap-3 sm:grid-cols-2"><label v-for="item in compactHeaders" :key="item.key" class="flex items-center gap-3 text-sm"><input v-model="osd[item.key]" type="checkbox" />{{ item.label }}</label></div>
            <p class="text-xs text-muted">Turn all four off to remove the header.</p>
            <h4 class="pt-2 text-sm font-semibold">Status</h4>
            <label v-for="item in compactContents" :key="item.key" class="flex items-start gap-3"><input v-model="osd[item.key]" type="checkbox" class="mt-1" /><span><span class="block text-sm font-medium">{{ item.label }}</span><small class="block text-muted">{{ item.hint }}</small></span></label>
            <p v-if="compactEmpty" role="status" class="text-xs text-muted">Nothing selected. Compact will be hidden until you choose a value.</p>
          </div>
        </details>
        <details class="osd-content-drawer mt-3 rounded-xl border" :open="!osd.compact">
          <summary class="cursor-pointer p-4"><span class="font-medium">Expanded</span><span class="mt-1 block text-xs text-muted">Full status details and frame history</span></summary>
          <div class="border-t px-4 pb-4">
        <h4 class="mt-5 text-sm font-semibold">Header</h4>
        <div class="mt-3 grid gap-3 sm:grid-cols-2">
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.showBrand" type="checkbox" />VectorXR name</label>
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.showApp" type="checkbox" />Application name</label>
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.showRuntime" type="checkbox" />OpenXR runtime</label>
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.showClock" type="checkbox" />Local clock</label>
        </div>
        <p class="mt-3 text-xs text-muted">Turn all four off to remove the header.</p>
        <h4 class="mt-5 text-sm font-semibold">Body</h4>
        <p class="mt-1 text-xs text-muted">Drag rows or use the arrows to arrange them.</p>
        <div ref="bodyList" class="mt-3 space-y-2" @pointermove="dragRow" @pointerup="endRowDrag" @pointercancel="endRowDrag" @lostpointercapture="endRowDrag"><div v-for="(row, index) in osd.bodyOrder" :key="row" :data-osd-row="row" :class="{ 'osd-row-dragging': draggedRow === row }" class="surface-panel-muted flex items-center gap-3 rounded-lg border p-3">
          <label class="flex min-w-0 flex-1 items-start gap-3"><input v-model="osd[contents[row].key]" type="checkbox" class="mt-1" /><span><span class="block text-sm font-medium">{{ contents[row].label }}</span><small class="mt-0.5 block text-muted">{{ contents[row].hint }}</small></span></label>
          <div class="flex gap-1"><button class="button-secondary osd-drag-handle rounded px-2 py-1" :aria-label="`Drag ${contents[row].label} to reorder`" title="Drag to reorder; use arrow keys to move" @pointerdown.prevent="startRowDrag($event, row)" @keydown.up.prevent="moveRow(row, -1)" @keydown.down.prevent="moveRow(row, 1)">⠿</button><button :disabled="index === 0" :aria-label="`Move ${contents[row].label} up`" class="button-secondary rounded px-2 py-1 disabled:opacity-30" @click="moveRow(row, -1)">↑</button><button :disabled="index === osd.bodyOrder.length - 1" :aria-label="`Move ${contents[row].label} down`" class="button-secondary rounded px-2 py-1 disabled:opacity-30" @click="moveRow(row, 1)">↓</button></div>
        </div></div>
          </div>
        </details>
        <label v-if="osd.showClock || osd.compactShowClock" class="mt-4 flex items-center justify-between gap-3 text-xs text-muted">Clock format · both layouts<select v-model="osd.clockFormat" aria-label="Clock format" class="app-input manual-input-select rounded-lg px-2 py-1 text-xs"><option value="12">12 hour · 8:42 PM</option><option value="24">24 hour · 20:42</option></select></label>
      </article>
      <article class="surface-panel rounded-[1.25rem] border p-5">
        <h3 class="font-semibold">In-headset controls</h3><p class="mt-1 text-xs leading-5 text-muted">Use a keyboard or your flight controls without leaving the game.</p>
        <div class="mt-4 space-y-3"><ModuleBindingPanel heading="Show / hide" :binding="osd.toggleBinding" @edit="openBinding('toggleBinding')" /><ModuleBindingPanel heading="Compact / expanded" :binding="osd.cycleBinding" @edit="openBinding('cycleBinding')" /><BindingConflictWarnings :warnings="warnings" /></div>
        <h4 class="mt-5 text-sm font-semibold">VR session</h4>
        <p v-if="statusError" class="mt-2 text-xs text-muted">{{ statusError }}</p>
        <p v-else-if="!sessions.length" class="mt-2 text-xs leading-5 text-muted">No active VR application detected. Save your layout, then launch a supported OpenXR game with VectorXR registered.</p>
        <div v-for="session in sessions" :key="session.sessionId" class="mt-3 rounded-lg border p-3 text-xs"><strong>{{ session.application }}</strong><p class="mt-1 text-muted">{{ session.state.osdMessage || 'This session needs the updated VectorXR layer.' }}<span v-if="session.state.osdVisible"> · {{ session.state.osdCompact ? 'Compact' : 'Expanded' }}</span></p></div>
      </article>
    </div>
    <details class="surface-panel rounded-[1.25rem] border p-5 text-sm">
      <summary class="cursor-pointer font-semibold">What applies live, and what am I measuring?</summary>
      <div class="mt-4 space-y-3 text-muted leading-6"><p>Every setting on this page applies after Save, including enabling the OSD in a running game. Bindings change visibility and layout immediately for the current session. The next session uses your saved starting layout and visibility.</p><p>The overlay measures intervals between the application's xrEndFrame calls over the last 120 frames. P95 is the interval at the 95th percentile; lower, steadier frame times are better. These values do not measure GPU work, reprojection, or compositor frame rate. Turbo can make application cadence differ from the headset's refresh rate.</p><p>Turbo can be enabled after Save in a running game, and the in-game binding switches it off/on; live enable and Async toggles have been observed in DCS. Experimental timing controls and Quadviews enable/disable changes still require an application restart. The expanded OSD shows these pending changes. Other VectorXR controls retain the behavior described on their own settings pages.</p><p>This is an information panel with in-game visibility and layout controls. Edit settings in the desktop app. Direct3D 11, Direct3D 12, and Vulkan are supported. OpenGL is not supported. A runtime with no spare composition layers cannot show the panel.</p></div>
    </details>
  </div>
</template>

<style scoped>
.live-badge { color: #238973; background: #51ddbd18; border: 1px solid #51ddbd40; padding: 4px 9px; border-radius: 999px; font-weight: 600; }
.osd-content-drawer, .osd-content-drawer > div { border-color: var(--app-border); }
.osd-content-drawer summary::marker { color: var(--app-text-muted); }
.osd-drag-handle { touch-action: none; cursor: grab; user-select: none; }
.osd-row-dragging { outline: 2px solid #39b69b; }
.osd-row-dragging .osd-drag-handle { cursor: grabbing; }
.preset-controls { border-color: var(--app-border); }
.osd-control { display: block; font-size: 13px; } .osd-control > span { display: flex; justify-content: space-between; gap: 12px; } .osd-control output { font-variant-numeric: tabular-nums; color: var(--app-text-muted); } .osd-control input { width: 100%; margin-top: 8px; accent-color: #39b69b; } .osd-control small { display: block; font-size: 11px; color: var(--app-text-muted); }
.color-choice { display: flex; align-items: center; gap: 6px; border-color: var(--app-border); } .color-choice[aria-pressed=true] { border-color: var(--swatch); background: color-mix(in srgb, var(--swatch) 12%, transparent); } .color-choice i { width: 9px; height: 9px; border-radius: 50%; background: var(--swatch); }
</style>
