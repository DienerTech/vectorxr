<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, useId, watch } from 'vue'
import { osdCompactLayout } from '../lib/osdCompactLayout'
import { osdDiagnosticsTagWidth } from '../lib/osdDiagnostics'
import { osdAccentColors, type OsdSettings, type OsdBodyRow } from '../lib/model'
import { activeLocale, t, tIn, type AppLocale, type MessageKey, type MessageParams } from '../i18n/index.ts'
import { loadLanguage } from '../i18n/load.ts'
// pivotSnapLine mirrors the layer: the Pivot row gains a Snap View line only
// when Snap Views are configured. diagnostics: the tag after the brand
// (osdDiagnostics), empty for none. locale: the language the OSD draws in,
// the app's when not given.
const props = withDefaults(defineProps<{ settings: OsdSettings; pivotSnapLine?: boolean; diagnostics?: string; locale?: AppLocale }>(), { pivotSnapLine: false, diagnostics: '', locale: undefined })
// The panel's text is the OSD's (osd.preview.*), in its language; the stage
// around it is the app's.
const osdLocale = computed(() => props.locale ?? activeLocale.value)
watch(osdLocale, locale => { void loadLanguage(locale) }, { immediate: true })
const ot = (key: MessageKey, params?: MessageParams) => tIn(osdLocale.value, key, params)
const tagWidth = computed(() => osdDiagnosticsTagWidth(props.diagnostics))
const legible = defineModel<boolean>('legible', { default: false })
const edgeId = useId()
const emit = defineEmits<{ position: [horizontal: number, vertical: number] }>()
const accent = computed(() => props.settings.accent === 'custom' ? props.settings.customColor : osdAccentColors[props.settings.accent])
const flags = { graph: 'showGraph', performance: 'showPerformance', turbo: 'showTurbo', framegen: 'showFramegen', pivot: 'showPivot', modules: 'showModules' } as const
const expandedHeader = computed(() => {
  const s = props.settings, clockX = s.showClock ? 790 : 932, tagX = s.showBrand ? 200 : 28
  const appX = tagWidth.value ? tagX + tagWidth.value + 18 : tagX
  return { shift: s.showBrand || s.showApp || s.showRuntime || s.showClock || tagWidth.value ? 0 : 40,
    clockX, tagX, appX, runtimeX: s.showRuntime ? (s.showApp ? Math.floor((appX + clockX) / 2) : appX) : clockX }
})
// Row heights as the layer draws them; Frame Generation's row has a second
// line while it generates, Performance its details, and Pivot one for Snap Views.
const rowHeight = (row: OsdBodyRow) => row === 'graph' ? 156 : row === 'framegen' || row === 'performance' ? 80 : row === 'pivot' && props.pivotSnapLine ? 84 : 52
const rows = computed(() => {
  let y = 190 - expandedHeader.value.shift
  return props.settings.bodyOrder.filter(row => props.settings[flags[row]]).map(row => {
    const result = { key: row, y }; y += rowHeight(row); return result
  })
})
// The preview shows a session with Frame Generation on.
const compactLayout = computed(() => osdCompactLayout(props.settings, { pivotSnapLine: props.pivotSnapLine, framegenSession: true, diagnosticsWidth: tagWidth.value }))
const panelWidth = computed(() => props.settings.compact ? compactLayout.value.width : 960)
const height = computed(() => props.settings.compact ? compactLayout.value.height : 210 - expandedHeader.value.shift + rows.value.reduce((sum, row) => sum + rowHeight(row.key), 0))
const width = computed(() => 2 * Math.atan(.275 * props.settings.scale / 100 * (panelWidth.value - 2) / 958) * 180 / Math.PI)
const angularHeight = computed(() => width.value * (height.value - 2) / (panelWidth.value - 2))
// Legible mode draws both layouts at one shared scale so text size matches between Compact and Expanded.
// The scale only shrinks when the stage is too narrow for the widest (expanded) panel.
const legibleMaxScale = 0.56, legibleWidestPanel = 960, legiblePadX = 48, legiblePadY = 52
const stage = ref<HTMLElement | null>(null)
const stageSize = ref({ width: 0, height: 0 })
const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(([entry]) => { if (entry) stageSize.value = { width: entry.contentRect.width, height: entry.contentRect.height } })
onMounted(() => { if (stage.value) observer?.observe(stage.value) })
onUnmounted(() => observer?.disconnect())
const legibleScale = computed(() => Math.min(legibleMaxScale, stageSize.value.width > 0 ? (stageSize.value.width - legiblePadX) / legibleWidestPanel : legibleMaxScale))
const legibleStageHeight = computed(() => Math.max(300, Math.ceil(height.value * legibleScale.value) + legiblePadY))
const legibleView = computed(() => ({ width: stageSize.value.width || legibleWidestPanel, height: stageSize.value.height || legibleStageHeight.value }))
const headerValues = computed(() => ({ brand: 'VECTORXR', app: ot('osd.preview.appName'), runtime: ot('osd.preview.runtimeName'), clock: props.settings.clockFormat === '12' ? ot('osd.preview.clock12') : ot('osd.preview.clock24') }))
// Whether the sample shows a game Frame Generation runs in: Turbo then yields
// to it, as in the headset.
const framegenShown = computed(() => {
  const s = props.settings
  return s.compact ? s.compactShowFramegen || compactLayout.value.framegenBadge : s.showFramegen || s.showFramegenFps
})
// The sample text matches the layer's English OSD wording (osd.preview.*).
const labels = computed<Record<OsdBodyRow, [string, string]>>(() => ({
  graph: ['', ''],
  performance: [ot('osd.preview.labels.performance'), ''],
  turbo: [ot('osd.preview.labels.turbo'), framegenShown.value ? ot('osd.preview.turboPausedForFramegen') : ot('osd.preview.turboAsyncExperimental')],
  framegen: [ot('osd.preview.labels.framegen'), ot('osd.preview.framegenState')],
  pivot: [ot('osd.preview.labels.pivot'), ot('osd.preview.pivotState')],
  modules: [ot('osd.preview.labels.modules'), framegenShown.value ? ot('osd.preview.modulesWithFramegen') : ot('osd.preview.modules')],
}))
// Sample readings: a game Frame Generation doubles while the preview shows
// Frame Generation, a game at full rate otherwise.
const sample = computed(() => {
  return framegenShown.value ? { fps: '45', average: '22.2', p95: '24.8' } : { fps: '90', average: '11.1', p95: '12.4' }
})
// The PERFORMANCE row's sample, as the layer words it (DescribePerformance):
// headroom against the game's target, video memory with Frame Generation's share.
const performance = computed(() => framegenShown.value
  ? { target: ot('osd.preview.performanceTarget', { fps: 45 }), cells: [['CPU +56%', ot('osd.preview.cpuDetail', { ms: '9.8', thread: 61 })], ['GPU +9%', ot('osd.preview.gpuDetail', { busy: 91, game: 74 })], ['VRAM 11.4 GB', ot('osd.preview.vramDetailWithFramegen', { budget: '14.2', framegen: 612 })]] }
  : { target: ot('osd.preview.performanceTarget', { fps: 90 }), cells: [['CPU +32%', ot('osd.preview.cpuDetail', { ms: '7.6', thread: 84 })], ['GPU +18%', ot('osd.preview.gpuDetail', { busy: 82, game: 70 })], ['VRAM 9.8 GB', ot('osd.preview.vramDetail', { budget: '14.2' })]] })
let drag: { x: number; y: number; h: number; v: number; unit: number } | null = null
function start(event: PointerEvent) {
  if (event.button !== 0 || legible.value) return
  const target = event.currentTarget as SVGElement, rect = target.ownerSVGElement!.getBoundingClientRect()
  drag = { x: event.clientX, y: event.clientY, h: props.settings.horizontalDegrees, v: props.settings.verticalDegrees, unit: Math.min(rect.width / 100, rect.height / 80) }
  target.setPointerCapture(event.pointerId)
}
function move(event: PointerEvent) {
  if (!drag) return
  emit('position', Math.round(Math.max(-40, Math.min(40, drag.h + (event.clientX - drag.x) / drag.unit))), Math.round(Math.max(-35, Math.min(35, drag.v - (event.clientY - drag.y) / drag.unit))))
}
</script>

<template>
  <div class="osd-preview">
  <div class="osd-stage" :class="{ legible }" ref="stage" :style="legible ? { height: `${legibleStageHeight}px` } : undefined">
    <span class="stage-note">{{ legible ? t('osd.previewStage.legibleNote') : t('osd.previewStage.placementNote') }}</span>
    <svg :viewBox="legible ? `0 0 ${legibleView.width} ${legibleView.height}` : '-50 -40 100 80'" class="stage-svg" :aria-label="legible ? t('osd.previewStage.legibleAria') : t('osd.previewStage.placementAria')">
      <path v-if="!legible" d="M -50 0 H 50 M 0 -40 V 40" stroke="#a4c1d630" stroke-width=".15" />
      <circle v-if="!legible" r="1" fill="none" stroke="#a4c1d685" stroke-width=".15" />
      <svg v-if="!settings.compact || !compactLayout.empty" :x="legible ? (legibleView.width - panelWidth * legibleScale) / 2 : settings.horizontalDegrees - width / 2" :y="legible ? (legibleView.height - height * legibleScale) / 2 + 8 : -settings.verticalDegrees - angularHeight / 2" :width="legible ? panelWidth * legibleScale : width" :height="legible ? height * legibleScale : angularHeight" :viewBox="`0 0 ${panelWidth} ${height}`":lang="osdLocale" class="preview" :style="{ '--accent': accent, opacity: settings.opacity / 100 }"
        @pointerdown="start" @pointermove="move" @pointerup="drag = null" @pointercancel="drag = null" @lostpointercapture="drag = null">
        <defs>
          <filter :id="`${edgeId}-blur`"><feGaussianBlur stdDeviation="2" /></filter>
          <mask :id="`${edgeId}-mask`"><rect x="6" y="6" :width="panelWidth - 12" :height="height - 12" rx="18" fill="white" :filter="`url(#${edgeId}-blur)`" /></mask>
        </defs>
        <g :mask="`url(#${edgeId}-mask)`">
          <rect :width="panelWidth" :height="height" fill="#101010" />
          <rect x="12" y="12" :width="panelWidth - 24" :height="height - 24" rx="12" fill="#0d141f" :filter="`url(#${edgeId}-blur)`" />
        </g>
        <template v-if="settings.compact">
          <foreignObject v-for="field in compactLayout.fields" :key="field.key" :x="field.x" :y="compactLayout.headerY" :width="field.width" height="34"><div class="header-field" :style="field.key === 'brand' ? { color: accent, fontWeight: 600, fontSize: '22px' } : undefined">{{ headerValues[field.key as keyof typeof headerValues] }}</div></foreignObject>
          <g v-if="compactLayout.diagnostics" :transform="`translate(${compactLayout.diagnostics.x} ${compactLayout.headerY - 1})`"><rect :width="compactLayout.diagnostics.width" height="30" fill="#f5b96c" /><foreignObject x="10" :width="compactLayout.diagnostics.width - 16" height="30"><div class="diagnostics-tag">{{ diagnostics }}</div></foreignObject></g>
          <text v-if="compactLayout.fps" x="28" :y="compactLayout.metricsY + 40" font-size="40" fill="#e9f0f8" font-weight="600">{{ sample.fps }} fps</text>
          <g v-if="compactLayout.framegenBadge" font-size="20" font-weight="600">
            <text x="150" :y="compactLayout.metricsY + 23" :fill="accent">{{ ot('osd.preview.fgOn') }}</text>
            <text x="150" :y="compactLayout.metricsY + 47" fill="#e9f0f8">90 fps</text>
          </g>
          <text v-if="compactLayout.frameTime" :x="compactLayout.frameTimeX" :y="compactLayout.metricsY + 40" font-size="40" fill="#e9f0f8" font-weight="600">{{ ot('osd.preview.averageCompact', { value: sample.average }) }}</text>
          <g v-if="compactLayout.statusY >= 0">
            <path v-if="compactLayout.statusY > 18" :d="`M28 ${compactLayout.statusY - 8} H${panelWidth - 28}`" stroke="#141f2d" />
            <g v-if="settings.compactShowPerformance" :transform="`translate(0 ${compactLayout.performanceY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">{{ ot('osd.preview.labels.performanceCompact') }}</text>
              <foreignObject :x="compactLayout.statusValueX" :width="panelWidth - compactLayout.statusValueX - 28" height="36"><div class="body-value" style="color: #e9f0f8">{{ performance.cells[0][0] }}<span class="separator">  /  </span>{{ performance.cells[1][0] }}<span class="separator">  /  </span>{{ performance.cells[2][0] }}</div></foreignObject>
            </g>
            <g v-if="settings.compactShowTurbo" :transform="`translate(0 ${compactLayout.turboY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">{{ ot('osd.preview.labels.turbo') }}</text>
              <foreignObject :x="compactLayout.statusValueX" :width="panelWidth - compactLayout.statusValueX - 28" height="36"><div class="body-value" :style="{ color: accent }">{{ framegenShown ? ot('osd.preview.turboPausedForFramegen') : ot('osd.preview.turboAsyncCompact') }}</div></foreignObject>
            </g>
            <g v-if="settings.compactShowFramegen" :transform="`translate(0 ${compactLayout.framegenY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">{{ ot('osd.preview.labels.framegen') }}</text>
              <foreignObject :x="compactLayout.statusValueX" :width="panelWidth - compactLayout.statusValueX - 28" height="36"><div class="body-value" style="color: #e9f0f8">{{ ot('osd.preview.framegenCompact') }}</div></foreignObject>
            </g>
            <g v-if="settings.compactShowPivot" :transform="`translate(0 ${compactLayout.pivotY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">{{ ot('osd.preview.labels.pivot') }}</text>
              <foreignObject :x="compactLayout.statusValueX" :width="panelWidth - compactLayout.statusValueX - 28" height="36"><div class="body-value" style="color: #e9f0f8">{{ ot('osd.preview.pivotCompact') }}</div></foreignObject>
              <foreignObject v-if="pivotSnapLine" :x="compactLayout.statusValueX" y="32" :width="panelWidth - compactLayout.statusValueX - 28" height="36"><div class="body-value" style="color: #e9f0f8">{{ ot('osd.preview.snapCompact') }}</div></foreignObject>
            </g>
          </g>
        </template>
        <template v-else>
          <text v-if="settings.showBrand" x="28" y="40" font-size="22" :fill="accent" font-weight="600">VECTORXR</text>
          <g v-if="tagWidth" :transform="`translate(${expandedHeader.tagX} 17)`"><rect :width="tagWidth" height="30" fill="#f5b96c" /><foreignObject x="10" :width="tagWidth - 16" height="30"><div class="diagnostics-tag">{{ diagnostics }}</div></foreignObject></g>
          <foreignObject v-if="settings.showApp" :x="expandedHeader.appX" y="18" :width="expandedHeader.runtimeX - expandedHeader.appX - 16" height="34"><div class="header-field">{{ headerValues.app }}</div></foreignObject>
          <foreignObject v-if="settings.showRuntime" :x="expandedHeader.runtimeX" y="18" :width="expandedHeader.clockX - expandedHeader.runtimeX - 18" height="34"><div class="header-field">{{ headerValues.runtime }}</div></foreignObject>
          <text v-if="settings.showClock" x="790" y="39" font-size="21" fill="#94a5b8">{{ headerValues.clock }}</text>
          <g :transform="`translate(0 ${-expandedHeader.shift})`">
            <g font-size="68" fill="#e9f0f8" font-weight="600"><text x="28" y="125">{{ sample.fps }}</text><text x="325" y="125">{{ sample.average }} ms</text><text x="655" y="125" :fill="accent">{{ sample.p95 }} ms</text></g>
            <g v-if="settings.showFramegenFps" font-size="24" font-weight="600"><text x="120" y="99" :fill="accent">{{ ot('osd.preview.fgOn') }}</text><text x="120" y="128" fill="#e9f0f8">90 fps</text></g>
            <g font-size="20" fill="#94a5b8"><text x="30" y="161">{{ ot('osd.preview.appFps') }}</text><text x="327" y="161">{{ ot('osd.preview.appFrameAverage') }}</text><text x="657" y="161">{{ ot('osd.preview.appFrameP95') }}</text></g>
          </g>
        </template>
        <template v-if="!settings.compact"><g v-for="row in rows" :key="row.key" :transform="`translate(0 ${row.y})`">
          <template v-if="row.key === 'graph'"><rect x="28" width="904" height="130" fill="#141f2d" /><text x="40" y="22" font-size="18" fill="#94a5b8">{{ ot('osd.preview.frameTimeGraph') }}</text><path d="M40 94 L100 90 160 98 220 94 280 92 340 35 400 87 460 91 520 96 580 92 640 90 700 99 760 88 820 94 920 93" fill="none" :stroke="accent" stroke-width="3" /></template>
          <template v-else-if="row.key === 'performance'"><text x="30" y="20" font-size="20" fill="#94a5b8">{{ ot('osd.preview.labels.performance') }}</text><text x="30" y="50" font-size="20" fill="#94a5b8">{{ performance.target }}</text>
            <g v-for="(cell, index) in performance.cells" :key="index"><foreignObject :x="242 + index * 230" width="220" height="36"><div class="body-value" style="color: #e9f0f8">{{ cell[0] }}</div></foreignObject><foreignObject :x="242 + index * 230" y="30" width="220" height="30"><div class="body-detail">{{ cell[1] }}</div></foreignObject></g></template>
          <template v-else><text x="30" y="20" font-size="20" fill="#94a5b8">{{ labels[row.key][0] }}</text><foreignObject x="242" width="680" height="36"><div class="body-value" :style="{ color: row.key === 'turbo' ? accent : '#e9f0f8' }">{{ labels[row.key][1] }}</div></foreignObject><foreignObject v-if="row.key === 'pivot' && pivotSnapLine" x="242" y="32" width="680" height="36"><div class="body-value">{{ ot('osd.preview.snapState') }}</div></foreignObject>
            <foreignObject v-if="row.key === 'framegen'" x="242" y="30" width="680" height="30"><div class="body-detail">{{ ot('osd.preview.framegenDetail') }}</div></foreignObject></template>
        </g></template>
      </svg>
    </svg>
    <span v-if="settings.compact && compactLayout.empty" class="empty-preview">{{ t('osd.previewStage.emptyCompact') }}</span>
    <span v-if="!legible" class="stage-bottom">{{ t('osd.previewStage.footer', { width: width.toFixed(1) }) }}</span>
  </div>
  </div>
</template>

<style scoped>
.osd-preview { display: flex; flex-direction: column; min-height: 0; }
.empty-preview { position: absolute; inset: 45% 18px auto; text-align: center; font-size: 13px; }
.legible .preview { cursor: default; }
.osd-stage { height: 300px; position: relative; overflow: hidden; background: radial-gradient(ellipse at 50% 42%, #273d54, #132232 48%, #0b121c); color: #9eb1c7; }
.stage-svg { width: 100%; height: 100%; }
.stage-note, .stage-bottom { position: absolute; left: 18px; font-size: 10px; pointer-events: none; }
.stage-note { top: 15px; } .stage-bottom { bottom: 14px; }
.preview { cursor: grab; touch-action: none; user-select: none; font-family: 'Segoe UI', sans-serif; }
.preview:active { cursor: grabbing; }
.header-field { font: 21px 'Segoe UI', sans-serif; color: #94a5b8; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.diagnostics-tag { font: 600 17px/30px 'Segoe UI', sans-serif; color: #0d141f; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.body-value { font: 600 24px 'Segoe UI', sans-serif; white-space: pre; overflow: hidden; text-overflow: ellipsis; }
.body-detail { font: 20px 'Segoe UI', sans-serif; color: #94a5b8; white-space: pre; overflow: hidden; text-overflow: ellipsis; }
.separator { color: #94a5b8; }
</style>
