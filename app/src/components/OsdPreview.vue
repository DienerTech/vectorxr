<script setup lang="ts">
import { computed, useId } from 'vue'
import { osdCompactLayout } from '../lib/osdCompactLayout'
import { osdAccentColors, type OsdSettings, type OsdBodyRow } from '../lib/model'
const props = defineProps<{ settings: OsdSettings }>()
const legible = defineModel<boolean>('legible', { default: false })
const edgeId = useId()
const emit = defineEmits<{ position: [horizontal: number, vertical: number] }>()
const accent = computed(() => props.settings.accent === 'custom' ? props.settings.customColor : osdAccentColors[props.settings.accent])
const flags = { graph: 'showGraph', turbo: 'showTurbo', pivot: 'showPivot', modules: 'showModules' } as const
const expandedHeader = computed(() => {
  const s = props.settings, clockX = s.showClock ? 790 : 932, appX = s.showBrand ? 200 : 28
  return { shift: s.showBrand || s.showApp || s.showRuntime || s.showClock ? 0 : 40,
    clockX, appX, runtimeX: s.showRuntime ? (s.showApp ? Math.floor((appX + clockX) / 2) : appX) : clockX }
})
const rows = computed(() => {
  let y = 190 - expandedHeader.value.shift
  return props.settings.bodyOrder.filter(row => props.settings[flags[row]]).map(row => {
    const result = { key: row, y }; y += row === 'graph' ? 156 : 52; return result
  })
})
const compactLayout = computed(() => osdCompactLayout(props.settings))
const panelWidth = computed(() => props.settings.compact ? compactLayout.value.width : 960)
const height = computed(() => props.settings.compact ? compactLayout.value.height : 210 - expandedHeader.value.shift + rows.value.reduce((sum, row) => sum + (row.key === 'graph' ? 156 : 52), 0))
const width = computed(() => 2 * Math.atan(.275 * props.settings.scale / 100 * (panelWidth.value - 2) / 958) * 180 / Math.PI)
const angularHeight = computed(() => width.value * (height.value - 2) / (panelWidth.value - 2))
const readableWidth = computed(() => Math.max(560, panelWidth.value) + 80)
const readableHeight = computed(() => Math.max(220, height.value + 60))
const headerValues = computed(() => ({ brand: 'VECTORXR', app: 'your-game.exe', runtime: 'Your OpenXR runtime', clock: props.settings.clockFormat === '12' ? '8:42 PM' : '20:42' }))
const labels: Record<OsdBodyRow, [string, string]> = { graph: ['', ''], turbo: ['TURBO', 'Async / Experimental'], pivot: ['PIVOT', 'DCS Stepped Applied, Nudges Applied'], modules: ['ENHANCEMENTS', 'Depth / Pivot / Turbo / Quadviews'] }
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
    <div class="preview-view-controls flex flex-wrap items-center justify-end gap-2 px-5 pb-3">
      <span class="mr-auto text-xs text-muted">Preview size</span>
      <button v-for="mode in [false, true]" :key="String(mode)" :aria-pressed="legible === mode" :class="legible === mode ? 'button-accent' : 'button-secondary'" class="rounded-lg px-3 py-2 text-xs" @click="legible = mode">{{ mode ? 'Legible' : 'Estimated size' }}</button>
    </div>
  <div class="osd-stage" :class="{ legible }" :style="legible ? { height: `${Math.min(620, readableHeight)}px` } : undefined">
    <span class="stage-note">{{ legible ? 'CONTENT PREVIEW · Enlarged for reading' : 'PLACEMENT PREVIEW · 100° × 80° reference view' }}</span>
    <svg :viewBox="legible ? `0 0 ${readableWidth} ${readableHeight}` : '-50 -40 100 80'" class="stage-svg" :aria-label="legible ? 'Legible OSD content preview, enlarged and centered' : 'Estimated OSD size within a 100 by 80 degree reference view'">
      <path v-if="!legible" d="M -50 0 H 50 M 0 -40 V 40" stroke="#a4c1d630" stroke-width=".15" />
      <circle v-if="!legible" r="1" fill="none" stroke="#a4c1d685" stroke-width=".15" />
      <svg v-if="!settings.compact || !compactLayout.empty" :x="legible ? (readableWidth - panelWidth) / 2 : settings.horizontalDegrees - width / 2" :y="legible ? (readableHeight - height) / 2 : -settings.verticalDegrees - angularHeight / 2" :width="legible ? panelWidth : width" :height="legible ? height : angularHeight" :viewBox="`0 0 ${panelWidth} ${height}`" class="preview" :style="{ '--accent': accent, opacity: settings.opacity / 100 }"
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
          <text v-if="compactLayout.fps" x="28" :y="compactLayout.metricsY + 40" font-size="40" fill="#e9f0f8" font-weight="600">90 fps</text>
          <text v-if="compactLayout.frameTime" :x="compactLayout.fps ? 262 : 28" :y="compactLayout.metricsY + 40" font-size="40" fill="#e9f0f8" font-weight="600">11.1 ms avg</text>
          <g v-if="compactLayout.statusY >= 0">
            <path v-if="compactLayout.statusY > 18" :d="`M28 ${compactLayout.statusY - 8} H${panelWidth - 28}`" stroke="#141f2d" />
            <g v-if="settings.compactShowTurbo" :transform="`translate(0 ${compactLayout.turboY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">TURBO</text>
              <foreignObject x="128" :width="panelWidth - 156" height="36"><div class="body-value" :style="{ color: accent }">Async / EXP</div></foreignObject>
            </g>
            <g v-if="settings.compactShowPivot" :transform="`translate(0 ${compactLayout.pivotY})`">
              <text x="30" y="22" font-size="20" fill="#94a5b8">PIVOT</text>
              <foreignObject x="128" :width="panelWidth - 156" height="36"><div class="body-value" style="color: #e9f0f8">Applied + Nudge</div></foreignObject>
            </g>
          </g>
        </template>
        <template v-else>
          <text v-if="settings.showBrand" x="28" y="40" font-size="22" :fill="accent" font-weight="600">VECTORXR</text>
          <foreignObject v-if="settings.showApp" :x="expandedHeader.appX" y="18" :width="expandedHeader.runtimeX - expandedHeader.appX - 16" height="34"><div class="header-field">your-game.exe</div></foreignObject>
          <foreignObject v-if="settings.showRuntime" :x="expandedHeader.runtimeX" y="18" :width="expandedHeader.clockX - expandedHeader.runtimeX - 18" height="34"><div class="header-field">Your OpenXR runtime</div></foreignObject>
          <text v-if="settings.showClock" x="790" y="39" font-size="21" fill="#94a5b8">{{ headerValues.clock }}</text>
          <g :transform="`translate(0 ${-expandedHeader.shift})`">
            <g font-size="68" fill="#e9f0f8" font-weight="600"><text x="28" y="125">90</text><text x="325" y="125">11.1 ms</text><text x="655" y="125" :fill="accent">12.4 ms</text></g>
            <g font-size="20" fill="#94a5b8"><text x="30" y="161">APP FPS</text><text x="327" y="161">APP FRAME / AVG</text><text x="657" y="161">APP FRAME / P95</text></g>
          </g>
        </template>
        <template v-if="!settings.compact"><g v-for="row in rows" :key="row.key" :transform="`translate(0 ${row.y})`">
          <template v-if="row.key === 'graph'"><rect x="28" width="904" height="130" fill="#141f2d" /><text x="40" y="22" font-size="18" fill="#94a5b8">FRAME TIME / LAST 120 FRAMES</text><path d="M40 94 L100 90 160 98 220 94 280 92 340 35 400 87 460 91 520 96 580 92 640 90 700 99 760 88 820 94 920 93" fill="none" :stroke="accent" stroke-width="3" /></template>
          <template v-else><text x="30" y="20" font-size="20" fill="#94a5b8">{{ labels[row.key][0] }}</text><foreignObject x="242" width="680" height="36"><div class="body-value" :style="{ color: row.key === 'turbo' ? accent : '#e9f0f8' }">{{ labels[row.key][1] }}</div></foreignObject></template>
        </g></template>
      </svg>
    </svg>
    <span v-if="settings.compact && compactLayout.empty" class="empty-preview">Choose a compact value to show the display.</span>
    <span class="stage-bottom">{{ legible ? 'Sample values · Use Estimated size to judge placement and size' : `${width.toFixed(1)}° panel width · Drag to position · Headset field of view varies` }}</span>
  </div>
  </div>
</template>

<style scoped>
.osd-preview { display: flex; flex-direction: column; min-height: 0; }
.empty-preview { position: absolute; inset: 45% 18px auto; text-align: center; font-size: 13px; }
.legible .preview { cursor: default; }
.osd-stage { height: 340px; position: relative; overflow: hidden; background: radial-gradient(ellipse at 50% 42%, #273d54, #132232 48%, #0b121c); color: #9eb1c7; }
.stage-svg { width: 100%; height: 100%; }
.stage-note, .stage-bottom { position: absolute; left: 18px; font-size: 10px; pointer-events: none; }
.stage-note { top: 15px; } .stage-bottom { bottom: 14px; }
.preview { cursor: grab; touch-action: none; user-select: none; font-family: 'Segoe UI', sans-serif; }
.preview:active { cursor: grabbing; }
.header-field { font: 21px 'Segoe UI', sans-serif; color: #94a5b8; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.body-value { font: 600 24px 'Segoe UI', sans-serif; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
