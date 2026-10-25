<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import { useOsdPreviewWindow } from '../../lib/osdPreviewWindow'
import { osdDiagnostics } from '../../lib/osdDiagnostics'
import { osdCompactLayout } from '../../lib/osdCompactLayout'
import OsdColorPicker from '../OsdColorPicker.vue'
import OsdPreview from '../OsdPreview.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import BindingConflictWarnings from '../BindingConflictWarnings.vue'
import { bindingLabel, savedBindingConflictWarnings, osdAccentColors, osdBodyRows, osdLayout, pivotSnapViewsConfigured, type OsdBodyRow, type VectorXRConfig, type OsdSettings } from '../../lib/model'
import { loadRuntimeStatus, type RuntimeStatusSession } from '../../lib/commands'
import { activeLocale, layerLanguage, t, type MessageKey } from '../../i18n/index.ts'
import { supportedLocales } from '../../i18n/locales.ts'

const props = defineProps<{ config: VectorXRConfig; savedConfig: VectorXRConfig }>()
const osd = computed(() => props.config.core.osd)
// "Same as app" names the language that means now.
const appLanguageName = computed(() => supportedLocales.find((locale) => locale.code === layerLanguage())?.nativeName ?? layerLanguage())
const colorExpanded = ref(false)
const previewCompact = ref(osd.value.compact)
const previewSettings = computed(() => ({ ...osd.value, compact: previewCompact.value }))
const previewLegible = ref(false)
const previewPivotSnapLine = computed(() => pivotSnapViewsConfigured(props.config))
// Debug logging or a Frame Generation diagnostic view left on: named after
// the brand, in the headset as here.
const previewDiagnostics = computed(() => osdDiagnostics(props.config))
// The preview draws in the OSD's language as it is picked.
const previewLocale = computed(() => osd.value.language === 'app' ? activeLocale.value : osd.value.language)
const previewWindow = useOsdPreviewWindow(osd, previewCompact, previewLegible, previewPivotSnapLine, previewDiagnostics, previewLocale)
watch(() => osd.value.compact, compact => { previewCompact.value = compact })
function customColor() { osd.value.accent = 'custom'; colorExpanded.value = !colorExpanded.value }
const editing = ref<'toggleBindings' | 'cycleBindings' | null>(null)
const warnings = computed(() => savedBindingConflictWarnings(props.config, [...osd.value.toggleBindings, ...osd.value.cycleBindings]))
const sessions = ref<RuntimeStatusSession[]>([])
const statusError = ref('')
let disposed = false
let poll: ReturnType<typeof setTimeout> | undefined
async function refresh() {
  try {
    const status = await loadRuntimeStatus()
    if (!disposed) { sessions.value = status.sessions; statusError.value = '' }
  } catch { if (!disposed) statusError.value = t('osd.runtimeStatusUnavailable') }
  if (!disposed) poll = setTimeout(refresh, 2000)
}
onMounted(refresh)
onUnmounted(() => { disposed = true; if (poll) clearTimeout(poll) })

let savedScrollTop = 0
const scroller = () => document.querySelector('main section.overflow-y-auto')
function openBinding(binding: 'toggleBindings' | 'cycleBindings') {
  savedScrollTop = scroller()?.scrollTop ?? 0
  editing.value = binding
  void nextTick(() => scroller()?.scrollTo({ top: 0 }))
}
function closeBinding() {
  editing.value = null
  void nextTick(() => scroller()?.scrollTo({ top: savedScrollTop }))
}
// Message keys, translated at render so a language change applies.
const contents: Record<OsdBodyRow, { key: 'showGraph' | 'showPerformance' | 'showTurbo' | 'showFramegen' | 'showPivot' | 'showModules'; labelKey: MessageKey; hintKey: MessageKey }> = {
  graph: { key: 'showGraph', labelKey: 'osd.rows.graph.label', hintKey: 'osd.rows.graph.hint' },
  performance: { key: 'showPerformance', labelKey: 'osd.rows.performance.label', hintKey: 'osd.rows.performance.hint' },
  turbo: { key: 'showTurbo', labelKey: 'osd.rows.turbo.label', hintKey: 'osd.rows.turbo.hint' },
  framegen: { key: 'showFramegen', labelKey: 'osd.rows.framegen.label', hintKey: 'osd.rows.framegen.hint' },
  pivot: { key: 'showPivot', labelKey: 'osd.rows.pivot.label', hintKey: 'osd.rows.pivot.hint' },
  modules: { key: 'showModules', labelKey: 'osd.rows.modules.label', hintKey: 'osd.rows.modules.hint' },
}
const compactContents = [
  { key: 'compactShowPerformance', labelKey: 'osd.rows.performance.label', hintKey: 'osd.compactRows.performanceHint' },
  { key: 'compactShowTurbo', labelKey: 'osd.rows.turbo.label', hintKey: 'osd.compactRows.turboHint' },
  { key: 'compactShowFramegen', labelKey: 'osd.rows.framegen.label', hintKey: 'osd.compactRows.framegenHint' },
  { key: 'compactShowPivot', labelKey: 'osd.rows.pivot.label', hintKey: 'osd.compactRows.pivotHint' },
] as const satisfies readonly { key: string; labelKey: MessageKey; hintKey: MessageKey }[]
const compactHeaders = [
  { key: 'compactShowBrand', labelKey: 'osd.header.brand' },
  { key: 'compactShowApp', labelKey: 'osd.header.app' },
  { key: 'compactShowRuntime', labelKey: 'osd.header.runtime' },
  { key: 'compactShowClock', labelKey: 'osd.header.clock' },
] as const satisfies readonly { key: string; labelKey: MessageKey }[]
const metricChoices = { all: 'osd.contents.metrics.all', fps: 'osd.contents.metrics.fps', frameTime: 'osd.contents.metrics.frameTime', none: 'osd.contents.metrics.none' } as const satisfies Record<string, MessageKey>
const accentNames: Record<keyof typeof osdAccentColors, MessageKey> = {
  teal: 'osd.appearance.colors.teal', copper: 'osd.appearance.colors.copper', blue: 'osd.appearance.colors.blue',
  violet: 'osd.appearance.colors.violet', rose: 'osd.appearance.colors.rose',
}
const compactEmpty = computed(() => osdCompactLayout(osd.value).empty)
function fpsOnly() {
  Object.assign(osd.value, { compactMetrics: 'fps', compactShowBrand: false, compactShowApp: false,
    compactShowRuntime: false, compactShowClock: false, compactShowTurbo: false, compactShowFramegen: false, compactShowPivot: false,
    compactShowPerformance: false })
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
  if (index < 0 && osd.value.customPresets.length >= 50) { presetMessage.value = t('osd.presets.limit'); return }
  const preset = { name, settings: osdLayout(osd.value) }
  if (index >= 0) osd.value.customPresets.splice(index, 1, preset)
  else osd.value.customPresets.push(preset)
  selectedPreset.value = name; presetName.value = name
  presetMessage.value = t(index >= 0 ? 'osd.presets.updated' : 'osd.presets.added', { name })
}
function loadPreset() {
  const preset = osd.value.customPresets.find(p => p.name === selectedPreset.value)
  if (!preset) return
  Object.assign(osd.value, { ...preset.settings, bodyOrder: [...preset.settings.bodyOrder] })
  previewCompact.value = osd.value.compact
  presetName.value = preset.name; presetMessage.value = t('osd.presets.loaded', { name: preset.name })
}
function deletePreset() {
  const name = selectedPreset.value
  osd.value.customPresets = osd.value.customPresets.filter(p => p.name !== name)
  selectedPreset.value = ''; presetName.value = ''; presetMessage.value = t('osd.presets.removed', { name })
}
function preset(name: 'balanced' | 'minimal' | 'performance') {
  const shared: Partial<OsdSettings> = { opacity: 90, accent: 'teal', distanceMeters: 1.2, updateHz: 5,
    compactShowRuntime: true, compactShowClock: true, compactShowTurbo: false, compactShowFramegen: false, compactShowPivot: false,
    compactShowPerformance: false, compactShowBrand: true, compactShowApp: true, compactMetrics: 'all',
    showGraph: true, showPerformance: true, showRuntime: true, showBrand: true, showApp: true, showTurbo: true, showFramegen: true, showFramegenFps: true, showModules: true, showClock: true, showPivot: true, clockFormat: '24', bodyOrder: [...osdBodyRows] }
  const layout = name === 'minimal'
    ? { compact: true, horizontalDegrees: 0, verticalDegrees: -23, scale: 80 }
    : name === 'performance'
      ? { compact: false, horizontalDegrees: -20, verticalDegrees: -10, scale: 100, showRuntime: false, showModules: false, showPivot: false, accent: 'blue' as const, updateHz: 10 }
      : { compact: false, horizontalDegrees: 20, verticalDegrees: -12, scale: 100 }
  Object.assign(osd.value, shared, layout)
  previewCompact.value = osd.value.compact
  selectedPreset.value = ''
  presetMessage.value = t(name === 'balanced' ? 'osd.presets.appliedExpanded' : name === 'performance' ? 'osd.presets.appliedPerformance' : 'osd.presets.appliedMinimal')
}
</script>

<template>
  <ModuleBindingPage v-if="editing" :module-label="t('osd.title')" :bindings="osd[editing]"
    :label="editing === 'toggleBindings' ? t('osd.controls.toggleBindingLabel') : t('osd.controls.cycleBindingLabel')"
    :description="t('osd.controls.bindingDescription')"
    :warnings="warnings" @update:bindings="osd[editing!] = $event" @close="closeBinding" />
  <div v-else class="space-y-4">
    <article class="surface-panel rounded-[1.25rem] border p-5 shadow-panel backdrop-blur">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div class="min-w-0">
          <div class="flex flex-wrap items-center gap-2">
            <h2 class="text-2xl font-semibold tracking-tight">{{ t('osd.title') }}</h2>
            <span class="chip-success rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]">{{ t('osd.liveAfterSave') }}</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t('osd.intro') }}</p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="osd.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" />
          {{ osd.enabled ? t('osd.toggleOn') : t('osd.toggleOff') }}
        </label>
      </div>

      <section class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-center gap-2">
          <p class="eyebrow mr-1 text-xs font-semibold uppercase tracking-[0.24em]">{{ t('osd.controls.heading') }}</p>
          <span v-if="statusError" class="text-xs text-muted">{{ statusError }}</span>
          <span v-else-if="!sessions.length" class="chip-idle rounded-full px-2.5 py-1 text-xs">{{ t('osd.controls.noSession') }}</span>
          <template v-else>
            <span v-for="session in sessions" :key="session.sessionId" class="chip-idle rounded-full px-2.5 py-1 text-xs">
              <strong class="font-semibold">{{ session.application }}</strong> · {{ session.state.osdMessage || t('osd.controls.needsUpdatedLayer') }}<template v-if="session.state.osdVisible"> · {{ session.state.osdCompact ? t('osd.layout.compact') : t('osd.layout.expanded') }}</template>
            </span>
          </template>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <ModuleBindingPanel :heading="t('osd.controls.toggleHeading')" :bindings="osd.toggleBindings" @edit="openBinding('toggleBindings')" />
          <ModuleBindingPanel :heading="t('osd.controls.cycleHeading')" :bindings="osd.cycleBindings" @edit="openBinding('cycleBindings')" />
        </div>
        <BindingConflictWarnings class="mt-3" :warnings="warnings" />
      </section>

      <details class="section-disclosure mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <summary class="flex items-center gap-2 text-sm font-medium text-muted">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
          {{ t('osd.explainer.summary') }}
        </summary>
        <div class="mt-3 max-w-4xl space-y-2 text-xs leading-5 text-muted">
          <p>{{ t('osd.explainer.live') }}</p>
          <p>{{ t('osd.explainer.measuring') }}</p>
          <p>{{ t('osd.explainer.headroom') }}</p>
          <p>{{ t('osd.explainer.turbo') }}</p>
          <p>{{ t('osd.explainer.panel') }}</p>
        </div>
      </details>
    </article>

    <article class="surface-panel overflow-hidden rounded-[1.25rem] border shadow-panel backdrop-blur">
      <div class="flex flex-wrap items-center gap-2 px-5 py-3">
        <div class="segmented" role="group" :aria-label="t('osd.layout.editAria')">
          <button v-for="compact in [true, false]" :key="String(compact)" type="button" class="segmented-option" :class="{ 'segmented-option-active': previewCompact === compact }" :aria-pressed="previewCompact === compact" @click="previewCompact = compact">{{ compact ? t('osd.layout.compact') : t('osd.layout.expanded') }}</button>
        </div>
        <span v-if="osd.compact === previewCompact" class="chip-accent rounded-full px-2.5 py-1 text-[11px] font-medium uppercase tracking-[0.15em]" :title="t('osd.layout.startingHint')">{{ t('osd.layout.starting') }}</span>
        <button v-else type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" :title="t('osd.layout.startingHint')" @click="osd.compact = previewCompact">{{ previewCompact ? t('osd.layout.startInCompact') : t('osd.layout.startInExpanded') }}</button>
        <div class="ml-auto flex flex-wrap items-center gap-2">
          <div class="segmented" role="group" :aria-label="t('osd.previewScale.aria')">
            <button v-for="mode in [false, true]" :key="String(mode)" type="button" class="segmented-option" :class="{ 'segmented-option-active': previewLegible === mode }" :aria-pressed="previewLegible === mode" @click="previewLegible = mode">{{ mode ? t('osd.previewScale.legible') : t('osd.previewScale.placement') }}</button>
          </div>
          <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="previewWindow.open">{{ previewWindow.opened.value ? t('osd.focusWindow') : t('osd.popOut') }}</button>
        </div>
      </div>
      <p v-if="previewWindow.error.value" role="alert" class="chip-danger px-5 py-2 text-xs">{{ previewWindow.error.value }}</p>
      <OsdPreview v-if="!previewWindow.opened.value" v-model:legible="previewLegible" :settings="previewSettings" :locale="previewLocale" :pivot-snap-line="previewPivotSnapLine" :diagnostics="previewDiagnostics" @position="(horizontal, vertical) => { osd.horizontalDegrees = horizontal; osd.verticalDegrees = vertical }" />
      <p v-else class="surface-panel-soft border-y px-5 py-4 text-xs text-muted" style="border-color: var(--app-border)">{{ t('osd.previewOpenElsewhere') }}</p>

      <div class="grid gap-x-8 gap-y-6 border-t p-5 lg:grid-cols-2" style="border-color: var(--app-border)">
        <section class="min-w-0 space-y-3">
          <div class="flex flex-wrap items-center justify-between gap-2">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ previewCompact ? t('osd.contents.compactHeading') : t('osd.contents.expandedHeading') }}</p>
            <button v-if="previewCompact" type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" :title="t('osd.contents.justFpsHint')" @click="fpsOnly">{{ t('osd.contents.justFps') }}</button>
            <span v-else class="text-xs text-muted">{{ t('osd.contents.alwaysShown') }}</span>
          </div>

          <template v-if="previewCompact">
            <label class="block"><span class="mb-1.5 block text-xs text-muted">{{ t('osd.contents.performance') }}</span><select v-model="osd.compactMetrics" class="app-input manual-input-select w-full rounded-[0.75rem] px-3 py-2 text-sm"><option v-for="(labelKey, value) in metricChoices" :key="value" :value="value">{{ t(labelKey) }}</option></select></label>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">{{ t('osd.contents.headerLegend') }}</legend>
              <div class="grid grid-cols-2 gap-2"><label v-for="item in compactHeaders" :key="item.key" class="flex items-center gap-2.5 text-sm"><input v-model="osd[item.key]" type="checkbox" class="accent-depthxr-copper" />{{ t(item.labelKey) }}</label></div>
            </fieldset>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">{{ t('osd.contents.statusLegend') }}</legend>
              <div class="grid grid-cols-2 gap-2"><label v-for="item in compactContents" :key="item.key" class="flex items-center gap-2.5 text-sm" :title="t(item.hintKey)"><input v-model="osd[item.key]" type="checkbox" class="accent-depthxr-copper" />{{ t(item.labelKey) }}</label></div>
            </fieldset>
            <p v-if="compactEmpty" role="status" class="chip-warning rounded-[0.75rem] px-3 py-2 text-xs">{{ t('osd.contents.nothingSelected') }}</p>
          </template>

          <template v-else>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">{{ t('osd.contents.headerLegend') }}</legend>
              <div class="grid grid-cols-2 gap-2">
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showBrand" type="checkbox" class="accent-depthxr-copper" />{{ t('osd.header.brand') }}</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showApp" type="checkbox" class="accent-depthxr-copper" />{{ t('osd.header.app') }}</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showRuntime" type="checkbox" class="accent-depthxr-copper" />{{ t('osd.header.runtime') }}</label>
                <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.showClock" type="checkbox" class="accent-depthxr-copper" />{{ t('osd.header.clock') }}</label>
              </div>
            </fieldset>
            <fieldset>
              <legend class="mb-2 text-xs text-muted">{{ t('osd.contents.rowsLegend') }}</legend>
              <div ref="bodyList" class="space-y-1" @pointermove="dragRow" @pointerup="endRowDrag" @pointercancel="endRowDrag" @lostpointercapture="endRowDrag">
                <div v-for="(row, index) in osd.bodyOrder" :key="row" :data-osd-row="row" :class="{ 'osd-row-dragging': draggedRow === row }" class="osd-row flex items-center gap-2 rounded-[0.75rem] border py-0.5 pl-1 pr-1.5">
                  <button type="button" class="osd-drag-handle rounded px-1.5 py-1 text-muted" :aria-label="t('osd.contents.dragRow', { row: t(contents[row].labelKey) })" :title="t('osd.contents.dragHint')" @pointerdown.prevent="startRowDrag($event, row)" @keydown.up.prevent="moveRow(row, -1)" @keydown.down.prevent="moveRow(row, 1)">⠿</button>
                  <label class="flex min-w-0 flex-1 items-center gap-2.5 text-sm" :title="t(contents[row].hintKey)"><input v-model="osd[contents[row].key]" type="checkbox" class="accent-depthxr-copper" /><span class="truncate">{{ t(contents[row].labelKey) }}</span></label>
                  <button type="button" :disabled="index === 0" :aria-label="t('osd.contents.moveUp', { row: t(contents[row].labelKey) })" class="button-secondary rounded px-1.5 py-0.5 text-xs disabled:opacity-30" @click="moveRow(row, -1)">↑</button>
                  <button type="button" :disabled="index === osd.bodyOrder.length - 1" :aria-label="t('osd.contents.moveDown', { row: t(contents[row].labelKey) })" class="button-secondary rounded px-1.5 py-0.5 text-xs disabled:opacity-30" @click="moveRow(row, 1)">↓</button>
                </div>
              </div>
            </fieldset>
          </template>

          <label class="flex items-start gap-2.5 text-sm" :title="t('osd.contents.framegenFpsHint')">
            <input v-model="osd.showFramegenFps" type="checkbox" class="mt-0.5 accent-depthxr-copper" />
            <span>{{ t('osd.contents.framegenFps') }} <span class="text-xs text-muted">{{ t('osd.contents.bothLayouts') }}</span>
              <span class="mt-0.5 block text-xs leading-5 text-muted">{{ t('osd.contents.framegenFpsHint') }}</span>
            </span>
          </label>
          <label v-if="osd.showClock || osd.compactShowClock" class="flex items-center justify-between gap-3 text-xs text-muted">{{ t('osd.contents.clockFormat') }}<select v-model="osd.clockFormat" :aria-label="t('osd.contents.clockFormatAria')" class="app-input manual-input-select rounded-[0.75rem] px-2 py-1 text-xs"><option value="12">{{ t('osd.contents.clock12') }}</option><option value="24">{{ t('osd.contents.clock24') }}</option></select></label>
        </section>

        <section class="min-w-0 space-y-6">
          <div class="space-y-3">
            <div class="flex items-center justify-between gap-2">
              <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('osd.position.heading') }}</p>
              <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" :disabled="osd.horizontalDegrees === 0 && osd.verticalDegrees === 0" @click="osd.horizontalDegrees = 0; osd.verticalDegrees = 0">{{ t('osd.position.center') }}</button>
            </div>
            <div class="grid gap-x-6 gap-y-3 sm:grid-cols-2">
              <label class="osd-control"><span>{{ t('osd.position.horizontal') }} <output>{{ osd.horizontalDegrees }}°</output></span><input v-model.number="osd.horizontalDegrees" class="accent-depthxr-copper" :aria-label="t('osd.position.horizontalAria')" type="range" min="-40" max="40" step="1" /></label>
              <label class="osd-control"><span>{{ t('osd.position.vertical') }} <output>{{ osd.verticalDegrees }}°</output></span><input v-model.number="osd.verticalDegrees" class="accent-depthxr-copper" :aria-label="t('osd.position.verticalAria')" type="range" min="-35" max="35" step="1" /></label>
              <label class="osd-control"><span>{{ t('osd.position.size') }} <output>{{ osd.scale }}%</output></span><input v-model.number="osd.scale" class="accent-depthxr-copper" :aria-label="t('osd.position.sizeAria')" type="range" min="25" max="150" step="5" /></label>
              <label class="osd-control"><span>{{ t('osd.position.distance') }} <output>{{ osd.distanceMeters.toFixed(1) }} m</output></span><input v-model.number="osd.distanceMeters" class="accent-depthxr-copper" :aria-label="t('osd.position.distanceAria')" type="range" min="0.5" max="3" step="0.1" /></label>
            </div>
          </div>

          <div class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('osd.appearance.heading') }}</p>
            <div class="grid items-start gap-x-6 gap-y-3 sm:grid-cols-2">
              <label class="osd-control"><span>{{ t('osd.appearance.opacity') }} <output>{{ osd.opacity }}%</output></span><input v-model.number="osd.opacity" class="accent-depthxr-copper" :aria-label="t('osd.appearance.opacityAria')" type="range" min="30" max="100" step="1" /></label>
              <label class="osd-control" :title="t('osd.appearance.refreshRateHint')"><span>{{ t('osd.appearance.refreshRate') }}</span><select v-model.number="osd.updateHz" class="app-input manual-input-select mt-1.5 w-full rounded-[0.75rem] px-3 py-1.5 text-sm"><option v-for="hz in [1, 2, 5, 10, 20]" :key="hz" :value="hz">{{ hz === 5 ? t('osd.appearance.rateRecommended', { hz }) : `${hz} Hz` }}</option><option v-if="![1, 2, 5, 10, 20].includes(osd.updateHz)" :value="osd.updateHz">{{ osd.updateHz }} Hz</option></select></label>
              <label class="osd-control" :title="`${t('osd.language.description')} ${t('osd.language.restartNote')}`"><span>{{ t('osd.language.title') }}</span><select v-model="osd.language" class="app-input manual-input-select mt-1.5 w-full rounded-[0.75rem] px-3 py-1.5 text-sm"><option value="app">{{ t('osd.language.sameAsAppResolved', { language: appLanguageName }) }}</option><option v-for="locale in supportedLocales" :key="locale.code" :value="locale.code" :lang="locale.code">{{ locale.nativeName }}</option></select></label>
            </div>
            <div class="flex flex-wrap gap-1.5" role="group" :aria-label="t('osd.appearance.accentAria')">
              <button v-for="(hex, color) in osdAccentColors" :key="color" type="button" class="color-choice rounded-full border px-2.5 py-1 text-xs capitalize" :aria-pressed="osd.accent === color" :style="{ '--swatch': hex }" @click="osd.accent = color; colorExpanded = false"><i />{{ t(accentNames[color]) }}</button>
              <button type="button" class="color-choice rounded-full border px-2.5 py-1 text-xs" :aria-pressed="osd.accent === 'custom'" :aria-expanded="colorExpanded" aria-controls="osd-custom-color" :style="{ '--swatch': osd.customColor }" @click="customColor"><i />{{ t('common.custom') }}</button>
            </div>
            <OsdColorPicker v-if="colorExpanded" id="osd-custom-color" v-model="osd.customColor" class="surface-panel-soft rounded-[1rem] border p-4" style="border-color: var(--app-border)" />
            <label class="flex items-center gap-2.5 text-sm"><input v-model="osd.visibleOnStart" type="checkbox" class="accent-depthxr-copper" />{{ t('osd.appearance.visibleOnStart') }}</label>
          </div>
        </section>
      </div>
      <div class="flex flex-wrap items-center gap-2 border-t px-5 py-3" style="border-color: var(--app-border)" :aria-label="t('osd.presets.aria')">
        <p class="eyebrow mr-1 text-xs font-semibold uppercase tracking-[0.24em]" :title="t('osd.presets.hint')">{{ t('osd.presets.heading') }}</p>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('balanced')">{{ t('osd.presets.expanded') }}</button>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('performance')">{{ t('osd.presets.performance') }}</button>
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="preset('minimal')">{{ t('osd.presets.minimal') }}</button>
        <span class="mx-1 hidden h-5 border-l sm:block" style="border-color: var(--app-border)" aria-hidden="true" />
        <select v-model="selectedPreset" :aria-label="t('osd.presets.savedAria')" class="app-input manual-input-select w-44 rounded-[0.75rem] px-3 py-1.5 text-xs" @change="loadPreset">
          <option value="">{{ osd.customPresets.length ? t('osd.presets.savedPlaceholder') : t('osd.presets.noneSaved') }}</option>
          <option v-for="item in osd.customPresets" :key="item.name" :value="item.name">{{ item.name }}</option>
        </select>
        <button v-if="selectedPreset" type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" :title="t('osd.presets.reloadHint')" @click="loadPreset">{{ t('osd.presets.reload') }}</button>
        <button v-if="selectedPreset" type="button" class="button-secondary rounded-[0.75rem] p-1.5" :title="t('osd.presets.delete')" :aria-label="t('osd.presets.delete')" @click="deletePreset"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M3 6h18 M9 6V3h6v3 M5 6l1 15h12l1-15 M10 10v7 M14 10v7" /></svg></button>
        <div class="flex items-center gap-1">
          <input v-model="presetName" maxlength="60" :aria-label="t('osd.presets.nameAria')" class="app-input w-44 rounded-[0.75rem] px-3 py-1.5 text-xs" :placeholder="t('osd.presets.namePlaceholder')" @keydown.enter.prevent="savePreset" />
          <button type="button" class="button-secondary rounded-[0.75rem] p-1.5 disabled:opacity-40" :disabled="!presetName.trim()" :title="existingPreset ? t('osd.presets.update') : t('osd.presets.saveNew')" :aria-label="existingPreset ? t('osd.presets.update') : t('osd.presets.saveNew')" @click="savePreset"><svg aria-hidden="true" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 3h13l4 4v14H3V3z M7 3v6h10V3 M7 21v-8h10v8" /></svg></button>
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
