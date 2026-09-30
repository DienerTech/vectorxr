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
  <div v-else class="space-y-4">
    <article class="surface-panel rounded-[1.25rem] border p-5 shadow-panel backdrop-blur">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div class="min-w-0">
          <div class="flex flex-wrap items-center gap-2">
            <h2 class="text-2xl font-semibold tracking-tight">On-Screen Display</h2>
            <span class="chip-success rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]">Live after Save</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">A head-following performance panel in VR. Direct3D 11, Direct3D 12, and Vulkan, in stereo or Quadviews.</p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="osd.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" />
          {{ osd.enabled ? 'OSD On' : 'OSD Off' }}
        </label>
      </div>

      <section class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-center gap-2">
          <p class="eyebrow mr-1 text-xs font-semibold uppercase tracking-[0.24em]">In-headset controls</p>
          <span v-if="statusError" class="text-xs text-muted">{{ statusError }}</span>
          <span v-else-if="!sessions.length" class="chip-idle rounded-full px-2.5 py-1 text-xs">No VR application running</span>
          <template v-else>
            <span v-for="session in sessions" :key="session.sessionId" class="chip-idle rounded-full px-2.5 py-1 text-xs">
              <strong class="font-semibold">{{ session.application }}</strong> · {{ session.state.osdMessage || 'Needs the updated VectorXR layer' }}<template v-if="session.state.osdVisible"> · {{ session.state.osdCompact ? 'Compact' : 'Expanded' }}</template>
            </span>
          </template>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <ModuleBindingPanel heading="Show / hide" :binding="osd.toggleBinding" @edit="openBinding('toggleBinding')" />
          <ModuleBindingPanel heading="Compact / expanded" :binding="osd.cycleBinding" @edit="openBinding('cycleBinding')" />
        </div>
        <BindingConflictWarnings class="mt-3" :warnings="warnings" />
      </section>

      <details class="section-disclosure mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <summary class="flex items-center gap-2 text-sm font-medium text-muted">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
          What applies live, and what am I measuring?
        </summary>
        <div class="mt-3 max-w-4xl space-y-2 text-xs leading-5 text-muted">
          <p>Every setting on this page applies after Save, including enabling the OSD in a running game. Bindings change visibility and layout immediately for the current session. The next session uses your saved starting layout and visibility.</p>
          <p>The overlay measures intervals between the application's xrEndFrame calls over the last 120 frames. P95 is the interval at the 95th percentile; lower, steadier frame times are better. These values do not measure GPU work, reprojection, or compositor frame rate. Turbo can make application cadence differ from the headset's refresh rate.</p>
          <p>Turbo can be enabled after Save in a running game, and the in-game binding switches it off/on; live enable and Async toggles have been observed in DCS. Experimental timing controls and Quadviews enable/disable changes still require an application restart. The expanded OSD shows these pending changes. Other VectorXR controls retain the behavior described on their own settings pages.</p>
          <p>This is an information panel with in-game visibility and layout controls. Edit settings in the desktop app. OpenGL is not supported. A runtime with no spare composition layers cannot show the panel.</p>
        </div>
      </details>
    </article>

    <article class="surface-panel overflow-hidden rounded-[1.25rem] border shadow-panel backdrop-blur">
      <div class="flex flex-wrap items-center gap-2 px-5 py-3">
        <div class="segmented" role="group" aria-label="Layout to preview and edit">
          <button v-for="compact in [true, false]" :key="String(compact)" type="button" class="segmented-option" :class="{ 'segmented-option-active': previewCompact === compact }" :aria-pressed="previewCompact === compact" @click="previewCompact = compact">{{ compact ? 'Compact' : 'Expanded' }}</button>
        </div>
        <span v-if="osd.compact === previewCompact" class="chip-accent rounded-full px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em]" title="The OSD opens in this layout each session. The in-headset binding switches layouts during play.">Starting layout</span>
        <button v-else type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" title="The OSD opens in this layout each session. The in-headset binding switches layouts during play." @click="osd.compact = previewCompact">Start in {{ previewCompact ? 'Compact' : 'Expanded' }}</button>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <div class="segmented" role="group" aria-label="Preview scale">
            <button v-for="mode in [false, true]" :key="String(mode)" type="button" class="segmented-option" :class="{ 'segmented-option-active': previewLegible === mode }" :aria-pressed="previewLegible === mode" @click="previewLegible = mode">{{ mode ? 'Legible' : 'Placement' }}</button>
          </div>
          <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="previewWindow.open">{{ previewWindow.opened.value ? 'Focus window' : 'Pop out' }}</button>
        </div>
      </div>
      <p v-if="previewWindow.error.value" role="alert" class="chip-danger px-5 py-2 text-xs">{{ previewWindow.error.value }}</p>
      <OsdPreview v-if="!previewWindow.opened.value" v-model:legible="previewLegible" :settings="previewSettings" @position="(horizontal, vertical) => { osd.horizontalDegrees = horizontal; osd.verticalDegrees = vertical }" />
      <p v-else class="surface-panel-soft border-y px-5 py-4 text-xs text-muted" style="border-color: var(--app-border)">Preview is open in a separate window.</p>

      <div class="grid gap-x-8 gap-y-6 border-t p-5 lg:grid-cols-2" style="border-color: var(--app-border)">
        <section class="min-w-0 space-y-3">
          <div class="flex flex-wrap items-center justify-between gap-2">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ previewCompact ? 'Compact' : 'Expanded' }} contents</p>
            <button v-if="previewCompact" type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" title="Clears the header and status rows for a tiny display. Size still controls text size." @click="fpsOnly">Just FPS</button>
            <span v-else class="text-xs text-muted">FPS, average, and P95 always shown</span>
          </div>

          <template v-if="previewCompact">
            <label class="block"><span class="mb-1.5 block text-xs text-muted">Performance</span><select v-model="osd.compactMetrics" class="app-input manual-input-select w-full rounded-[0.75rem] px-3 py-2 text-sm"><option v-for="(label, value) in metricChoices" :key="value" :value="value">{{ label }}</option></select></label>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">Header · turn all off to remove it</legend>
              <div class="grid grid-cols-2 gap-2"><label v-for="item in compactHeaders" :key="item.key" class="flex items-center gap-2.5 text-sm"><input v-model="osd[item.key]" type="checkbox" class="accent-depthxr-copper" />{{ item.label }}</label></div>
            </fieldset>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">Status</legend>
              <div class="grid grid-cols-2 gap-2"><label v-for="item in compactContents" :key="item.key" class="flex items-center gap-2.5 text-sm" :title="item.hint"><input v-model="osd[item.key]" type="checkbox" class="accent-depthxr-copper" />{{ item.label }}</label></div>
            </fieldset>
            <p v-if="compactEmpty" role="status" class="chip-warning rounded-[0.75rem] px-3 py-2 text-xs">Nothing selected. Compact stays hidden until you choose a value.</p>
          </template>

          <template v-else>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">Header · turn all off to remove it</legend>
              <div class="grid grid-cols-2 gap-2">
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showBrand" type="checkbox" class="accent-depthxr-copper" />VectorXR name</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showApp" type="checkbox" class="accent-depthxr-copper" />Application name</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showRuntime" type="checkbox" class="accent-depthxr-copper" />OpenXR runtime</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showClock" type="checkbox" class="accent-depthxr-copper" />Local clock</label>
              </div>
            </fieldset>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">Rows · drag or use the arrows to reorder</legend>
              <div ref="bodyList" class="space-y-1" @pointermove="dragRow" @pointerup="endRowDrag" @pointercancel="endRowDrag" @lostpointercapture="endRowDrag">
                <div v-for="(row, index) in osd.bodyOrder" :key="row" :data-osd-row="row" :class="{ 'osd-row-dragging': draggedRow === row }" class="osd-row flex items-center gap-2 rounded-[0.75rem] border py-0.5 pl-1 pr-1.5">
                  <button type="button" class="osd-drag-handle rounded px-1.5 py-1 text-muted" :aria-label="`Drag ${contents[row].label} to reorder`" title="Drag to reorder; use arrow keys to move" @pointerdown.prevent="startRowDrag($event, row)" @keydown.up.prevent="moveRow(row, -1)" @keydown.down.prevent="moveRow(row, 1)">⠿</button>
                  <label class="flex min-w-0 flex-1 items-center gap-2.5 text-sm" :title="contents[row].hint"><input v-model="osd[contents[row].key]" type="checkbox" class="accent-depthxr-copper" /><span class="truncate">{{ contents[row].label }}</span></label>
                  <button type="button" :disabled="index === 0" :aria-label="`Move ${contents[row].label} up`" class="button-secondary rounded px-1.5 py-0.5 text-xs disabled:opacity-30" @click="moveRow(row, -1)">↑</button>
                  <button type="button" :disabled="index === osd.bodyOrder.length - 1" :aria-label="`Move ${contents[row].label} down`" class="button-secondary rounded px-1.5 py-0.5 text-xs disabled:opacity-30" @click="moveRow(row, 1)">↓</button>
                </div>
              </div>
            </fieldset>
          </template>

          <label v-if="osd.showClock || osd.compactShowClock" class="flex items-center justify-between gap-3 text-xs text-muted">Clock format (both layouts)<select v-model="osd.clockFormat" aria-label="Clock format" class="app-input manual-input-select rounded-[0.75rem] px-2 py-1 text-xs"><option value="12">12 hour · 8:42 PM</option><option value="24">24 hour · 20:42</option></select></label>
        </section>

        <section class="min-w-0 space-y-6">
          <div class="space-y-3">
            <div class="flex items-center justify-between gap-2">
              <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Position &amp; size</p>
              <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" :disabled="osd.horizontalDegrees === 0 && osd.verticalDegrees === 0" @click="osd.horizontalDegrees = 0; osd.verticalDegrees = 0">Center in view</button>
            </div>
            <div class="grid gap-x-6 gap-y-3 sm:grid-cols-2">
              <label class="osd-control"><span>Horizontal <output>{{ osd.horizontalDegrees }}°</output></span><input v-model.number="osd.horizontalDegrees" class="accent-depthxr-copper" aria-label="Horizontal position" type="range" min="-40" max="40" step="1" /></label>
              <label class="osd-control"><span>Vertical <output>{{ osd.verticalDegrees }}°</output></span><input v-model.number="osd.verticalDegrees" class="accent-depthxr-copper" aria-label="Vertical position" type="range" min="-35" max="35" step="1" /></label>
              <label class="osd-control"><span>Size <output>{{ osd.scale }}%</output></span><input v-model.number="osd.scale" class="accent-depthxr-copper" aria-label="Display size" type="range" min="25" max="150" step="5" /></label>
              <label class="osd-control"><span>Viewing distance <output>{{ osd.distanceMeters.toFixed(1) }} m</output></span><input v-model.number="osd.distanceMeters" class="accent-depthxr-copper" aria-label="Viewing distance" type="range" min="0.5" max="3" step="0.1" /></label>
            </div>
          </div>

          <div class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Appearance</p>
            <div class="grid items-start gap-x-6 gap-y-3 sm:grid-cols-2">
              <label class="osd-control"><span>Opacity <output>{{ osd.opacity }}%</output></span><input v-model.number="osd.opacity" class="accent-depthxr-copper" aria-label="Display opacity" type="range" min="30" max="100" step="1" /></label>
              <label class="osd-control" title="Frame history samples every frame. Higher rates redraw the panel more often."><span>Refresh rate</span><select v-model.number="osd.updateHz" class="app-input manual-input-select mt-1.5 w-full rounded-[0.75rem] px-3 py-1.5 text-sm"><option v-for="hz in [1, 2, 5, 10, 20]" :key="hz" :value="hz">{{ hz }} Hz{{ hz === 5 ? ' · Recommended' : '' }}</option><option v-if="![1, 2, 5, 10, 20].includes(osd.updateHz)" :value="osd.updateHz">{{ osd.updateHz }} Hz</option></select></label>
            </div>
            <div class="flex flex-wrap gap-1.5" role="group" aria-label="Accent color">
              <button v-for="(hex, color) in osdAccentColors" :key="color" type="button" class="color-choice rounded-full border px-2.5 py-1 text-xs capitalize" :aria-pressed="osd.accent === color" :style="{ '--swatch': hex }" @click="osd.accent = color; colorExpanded = false"><i />{{ color }}</button>
              <button type="button" class="color-choice rounded-full border px-2.5 py-1 text-xs" :aria-pressed="osd.accent === 'custom'" :aria-expanded="colorExpanded" aria-controls="osd-custom-color" :style="{ '--swatch': osd.customColor }" @click="customColor"><i />Custom</button>
            </div>
            <OsdColorPicker v-if="colorExpanded" id="osd-custom-color" v-model="osd.customColor" class="surface-panel-soft rounded-[1rem] border p-4" style="border-color: var(--app-border)" />
            <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.visibleOnStart" type="checkbox" class="accent-depthxr-copper" />Show automatically when enabled or a session starts</label>
          </div>
        </section>
      </div>
      <div class="flex flex-wrap items-center gap-2 border-t px-5 py-3" style="border-color: var(--app-border)" aria-label="Display presets">
        <p class="eyebrow mr-1 text-xs font-semibold uppercase tracking-[0.24em]" title="Presets apply position, appearance, and contents together.">Presets</p>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('balanced')">Expanded</button>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('performance')">Performance</button>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('minimal')">Minimal</button>
        <span class="mx-1 hidden h-5 border-l sm:block" style="border-color: var(--app-border)" aria-hidden="true" />
        <select v-model="selectedPreset" aria-label="Saved custom presets" class="app-input manual-input-select w-44 rounded-[0.75rem] px-3 py-1.5 text-xs" @change="loadPreset">
          <option value="">{{ osd.customPresets.length ? 'Saved presets…' : 'No saved presets' }}</option>
          <option v-for="item in osd.customPresets" :key="item.name" :value="item.name">{{ item.name }}</option>
        </select>
        <button v-if="selectedPreset" type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" title="Reload the selected preset into the controls" @click="loadPreset">Reload</button>
        <button v-if="selectedPreset" type="button" class="button-secondary rounded-[0.75rem] p-1.5" title="Delete selected preset" aria-label="Delete selected preset" @click="deletePreset"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M3 6h18 M9 6V3h6v3 M5 6l1 15h12l1-15 M10 10v7 M14 10v7" /></svg></button>
        <div class="flex items-center gap-1">
          <input v-model="presetName" maxlength="60" aria-label="Preset name" class="app-input w-44 rounded-[0.75rem] px-3 py-1.5 text-xs" placeholder="Save current as…" @keydown.enter.prevent="savePreset" />
          <button type="button" class="button-secondary rounded-[0.75rem] p-1.5 disabled:opacity-40" :disabled="!presetName.trim()" :title="existingPreset ? 'Update saved preset' : 'Save new preset'" :aria-label="existingPreset ? 'Update saved preset' : 'Save new preset'" @click="savePreset"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 3h13l4 4v14H3V3z M7 3v6h10V3 M7 21v-8h10v8" /></svg></button>
        </div>
        <p v-if="presetMessage" class="w-full text-xs text-muted" role="status">{{ presetMessage }}</p>
      </div>

    </article>
  </div>
</template>

<style scoped>
.osd-row { background: var(--app-surface-strong); border-color: var(--app-border); }
.osd-drag-handle { touch-action: none; cursor: grab; user-select: none; }
.osd-drag-handle:hover { color: var(--app-text); }
.osd-row-dragging { outline: 2px solid var(--app-accent); }
.osd-row-dragging .osd-drag-handle { cursor: grabbing; }
.osd-control { display: block; font-size: 13px; }
.osd-control > span { display: flex; justify-content: space-between; gap: 12px; }
.osd-control output { font-variant-numeric: tabular-nums; color: var(--app-text-muted); }
.osd-control input[type=range] { width: 100%; height: 0.5rem; margin-top: 8px; cursor: pointer; }
.color-choice { display: flex; align-items: center; gap: 6px; border-color: var(--app-border); color: var(--app-text-muted); }
.color-choice:hover { color: var(--app-text); }
.color-choice[aria-pressed=true] { border-color: var(--swatch); color: var(--app-text); background: color-mix(in srgb, var(--swatch) 14%, transparent); }
.color-choice i { width: 9px; height: 9px; border-radius: 50%; background: var(--swatch); }
</style>
