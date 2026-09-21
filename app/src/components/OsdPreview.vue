<script setup lang="ts">
import { computed, useId } from 'vue'
import { osdAccentColors, type OsdSettings, type OsdBodyRow } from '../lib/model'
const props = defineProps<{ settings: OsdSettings }>()
const edgeId = useId()
const emit = defineEmits<{ position: [horizontal: number, vertical: number] }>()
const accent = computed(() => props.settings.accent === 'custom' ? props.settings.customColor : osdAccentColors[props.settings.accent])
const flags = { graph: 'showGraph', turbo: 'showTurbo', pivot: 'showPivot', modules: 'showModules' } as const
const rows = computed(() => {
  let y = 190
  return props.settings.bodyOrder.filter(row => props.settings[flags[row]]).map(row => {
    const result = { key: row, y }; y += row === 'graph' ? 156 : 52; return result
  })
})
const height = computed(() => props.settings.compact ? 180 : 210 + rows.value.reduce((sum, row) => sum + (row.key === 'graph' ? 156 : 52), 0))
const width = computed(() => 2 * Math.atan(.275 * props.settings.scale / 100) * 180 / Math.PI)
const labels: Record<OsdBodyRow, [string, string]> = { graph: ['', ''], turbo: ['TURBO', 'Async / Experimental'], pivot: ['PIVOT', 'DCS Stepped Applied, Nudges Applied'], modules: ['ENHANCEMENTS', 'Depth / Pivot / Turbo / Quadviews'] }
let drag: { x: number; y: number; h: number; v: number; unit: number } | null = null
function start(event: PointerEvent) {
  if (event.button !== 0) return
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
  <div class="osd-stage">
    <span class="stage-note">PLACEMENT PREVIEW · 100° × 80° reference view</span>
    <svg viewBox="-50 -40 100 80" class="stage-svg" aria-label="OSD placement at its angular size within a 100 by 80 degree reference view">
      <path d="M -50 0 H 50 M 0 -40 V 40" stroke="#a4c1d630" stroke-width=".15" />
      <circle r="1" fill="none" stroke="#a4c1d685" stroke-width=".15" />
      <svg :x="settings.horizontalDegrees - width / 2" :y="-settings.verticalDegrees - width * height / 960 / 2" :width="width" :height="width * height / 960" :viewBox="`0 0 960 ${height}`" class="preview" :style="{ '--accent': accent, opacity: settings.opacity / 100 }"
        @pointerdown="start" @pointermove="move" @pointerup="drag = null" @pointercancel="drag = null" @lostpointercapture="drag = null">
        <defs>
          <filter :id="`${edgeId}-blur`"><feGaussianBlur stdDeviation="2" /></filter>
          <mask :id="`${edgeId}-mask`"><rect x="6" y="6" width="948" :height="height - 12" rx="18" fill="white" :filter="`url(#${edgeId}-blur)`" /></mask>
        </defs>
        <g :mask="`url(#${edgeId}-mask)`">
          <rect width="960" :height="height" fill="#101010" />
          <rect x="12" y="12" width="936" :height="height - 24" rx="12" fill="#0d141f" :filter="`url(#${edgeId}-blur)`" />
        </g>
        <text x="28" y="40" font-size="22" :fill="accent" font-weight="600">VECTORXR</text>
        <foreignObject x="200" y="18" :width="settings.showRuntime ? 260 : settings.showClock ? 575 : 717" height="34"><div class="header-field">your-game.exe</div></foreignObject>
        <foreignObject v-if="settings.showRuntime" x="475" y="18" :width="settings.showClock ? 297 : 439" height="34"><div class="header-field">Your OpenXR runtime</div></foreignObject>
        <text v-if="settings.showClock" x="790" y="39" font-size="21" fill="#94a5b8">{{ settings.clockFormat === '12' ? '8:42 PM' : '20:42' }}</text>
        <g :font-size="settings.compact ? 48 : 68" fill="#e9f0f8" font-weight="600"><text x="28" :y="settings.compact ? 105 : 125">90</text><text x="325" :y="settings.compact ? 105 : 125">11.1 ms</text><text x="655" :y="settings.compact ? 105 : 125" :fill="accent">12.4 ms</text></g>
        <g font-size="20" fill="#94a5b8"><text x="30" :y="settings.compact ? 141 : 161">APP FPS</text><text x="327" :y="settings.compact ? 141 : 161">APP FRAME / AVG</text><text x="657" :y="settings.compact ? 141 : 161">APP FRAME / P95</text></g>
        <template v-if="!settings.compact"><g v-for="row in rows" :key="row.key" :transform="`translate(0 ${row.y})`">
          <template v-if="row.key === 'graph'"><rect x="28" width="904" height="130" fill="#141f2d" /><text x="40" y="22" font-size="18" fill="#94a5b8">FRAME TIME / LAST 120 FRAMES</text><path d="M40 94 L100 90 160 98 220 94 280 92 340 35 400 87 460 91 520 96 580 92 640 90 700 99 760 88 820 94 920 93" fill="none" :stroke="accent" stroke-width="3" /></template>
          <template v-else><text x="30" y="20" font-size="20" fill="#94a5b8">{{ labels[row.key][0] }}</text><foreignObject x="242" width="680" height="36"><div class="body-value" :style="{ color: row.key === 'turbo' ? accent : '#e9f0f8' }">{{ labels[row.key][1] }}</div></foreignObject></template>
        </g></template>
      </svg>
    </svg>
    <span class="stage-bottom">{{ width.toFixed(1) }}° panel width · Sample values · Headset field of view varies</span>
  </div>
</template>

<style scoped>
.osd-stage { height: 340px; position: relative; overflow: hidden; background: radial-gradient(ellipse at 50% 42%, #273d54, #132232 48%, #0b121c); color: #9eb1c7; }
.stage-svg { width: 100%; height: 100%; }
.stage-note, .stage-bottom { position: absolute; left: 18px; font-size: 10px; pointer-events: none; }
.stage-note { top: 15px; } .stage-bottom { bottom: 14px; }
.preview { cursor: grab; touch-action: none; user-select: none; font-family: 'Segoe UI', sans-serif; }
.preview:active { cursor: grabbing; }
.header-field { font: 21px 'Segoe UI', sans-serif; color: #94a5b8; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.body-value { font: 600 24px 'Segoe UI', sans-serif; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
</style>
