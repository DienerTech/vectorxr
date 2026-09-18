<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import BindingConflictWarnings from '../BindingConflictWarnings.vue'
import { bindingLabel, savedBindingConflictWarnings, type VectorXRConfig, type OsdSettings } from '../../lib/model'
import { loadRuntimeStatus, type RuntimeStatusSession } from '../../lib/commands'

const props = defineProps<{ config: VectorXRConfig }>()
const osd = computed(() => props.config.core.osd)
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

const accent = computed(() => ({ teal: '#51ddbd', copper: '#f5ae78', blue: '#6facff' })[osd.value.accent])
const panelStyle = computed(() => ({
  left: `${50 + osd.value.horizontalDegrees * .75}%`,
  top: `${50 - osd.value.verticalDegrees * .75}%`,
  width: `${Math.min(60, 38 * osd.value.scale / 100)}%`,
  opacity: osd.value.opacity / 100,
  '--osd-accent': accent.value,
}))
const contents: { key: 'showGraph' | 'showRuntime' | 'showTurbo' | 'showModules' | 'showClock'; label: string; hint: string }[] = [
  { key: 'showGraph', label: 'Frame-time graph', hint: 'Rolling history of the last 120 application frames.' },
  { key: 'showRuntime', label: 'OpenXR runtime', hint: 'Runtime reported by the current VR session.' },
  { key: 'showTurbo', label: 'Turbo state', hint: 'Off, async, sequenced, suspended, or recovery disabled.' },
  { key: 'showModules', label: 'Active enhancements', hint: 'Depth, Pivot, and Quadviews configuration for this app.' },
  { key: 'showClock', label: 'Local clock', hint: 'A little help keeping track of the real world.' },
]
function preset(name: 'balanced' | 'minimal' | 'performance') {
  const shared: Partial<OsdSettings> = { opacity: 90, accent: 'teal', distanceMeters: 1.2, updateHz: 5,
    showGraph: true, showRuntime: true, showTurbo: true, showModules: true, showClock: true }
  const layout = name === 'minimal'
    ? { compact: true, horizontalDegrees: 0, verticalDegrees: -23, scale: 80 }
    : name === 'performance'
      ? { compact: false, horizontalDegrees: -20, verticalDegrees: -10, scale: 100, showRuntime: false, showModules: false, accent: 'blue' as const, updateHz: 10 }
      : { compact: false, horizontalDegrees: 20, verticalDegrees: -12, scale: 100 }
  Object.assign(osd.value, shared, layout)
}
let drag: { x: number; y: number; horizontal: number; vertical: number; width: number; height: number } | null = null
function startDrag(event: PointerEvent) {
  if (event.button !== 0) return
  const target = event.currentTarget as HTMLElement
  const rect = target.parentElement!.getBoundingClientRect()
  drag = { x: event.clientX, y: event.clientY, horizontal: osd.value.horizontalDegrees, vertical: osd.value.verticalDegrees, width: rect.width, height: rect.height }
  target.setPointerCapture(event.pointerId)
}
function moveDrag(event: PointerEvent) {
  if (!drag) return
  osd.value.horizontalDegrees = Math.round(Math.max(-40, Math.min(40, drag.horizontal + (event.clientX - drag.x) / drag.width * 100 / .75)))
  osd.value.verticalDegrees = Math.round(Math.max(-35, Math.min(35, drag.vertical - (event.clientY - drag.y) / drag.height * 100 / .75)))
}
const graph = '0,35 10,34 20,38 30,36 40,35 50,38 60,29 70,36 80,37 90,35 100,34 110,8 120,30 130,35 140,37 150,34 160,35 170,32 180,37 190,35 200,36 210,33 220,36 230,35 240,37 250,32 260,35 270,36 280,33 290,36 300,35'
</script>

<template>
  <ModuleBindingPage v-if="editing" module-label="On-screen display" :binding="osd[editing]"
    :label="editing === 'toggleBinding' ? 'Show / hide display' : 'Switch compact / detailed'"
    description="Works in the VR application. Assign a keyboard chord, joystick button, or hat direction. Save to apply it live."
    :warnings="warnings" @update:binding="osd[editing!] = $event" @close="editing = null" />
  <div v-else class="space-y-5">
    <article class="surface-panel rounded-[1.25rem] border p-5 shadow-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">In-headset / Experimental</p>
          <h2 class="mt-1 text-2xl font-semibold tracking-tight">On-screen display</h2>
          <p class="mt-2 max-w-xl text-sm leading-6 text-muted">Keep an eye on performance without leaving VR. A head-following panel with the information you choose.</p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="osd.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" />
          {{ osd.enabled ? 'Enabled' : 'Enable OSD' }}
        </label>
      </div>
      <div class="mt-4 flex flex-wrap items-center gap-x-4 gap-y-2 text-xs text-muted">
        <span class="live-badge">Live after Save</span>
        <span>Direct3D 11 · Stereo &amp; Quadviews</span>
        <span>Show / hide: <strong class="font-medium">{{ bindingLabel(osd.toggleBinding) }}</strong></span>
      </div>
    </article>

    <article class="surface-panel overflow-hidden rounded-[1.25rem] border shadow-panel">
      <div class="flex flex-wrap items-center justify-between gap-3 px-5 py-4">
        <div><h3 class="font-semibold">Make it yours</h3><p class="mt-1 text-xs text-muted">Drag the panel to position it, or use the controls below.</p></div>
        <div class="flex flex-wrap gap-2" aria-label="Display presets">
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('balanced')">Flight deck</button>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('minimal')">Minimal</button>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs font-medium" @click="preset('performance')">Performance</button>
        </div>
      </div>
      <div class="osd-stage">
        <div class="osd-horizon" aria-hidden="true" />
        <div class="osd-reticle" aria-hidden="true">+</div>
        <span class="osd-stage-note">PLACEMENT PREVIEW <span>· Sample values</span></span>
        <div class="osd-preview" :style="panelStyle" role="img" aria-label="Illustrative OSD layout. Adjust position using the horizontal and vertical controls below."
          @pointerdown="startDrag" @pointermove="moveDrag" @pointerup="drag = null" @pointercancel="drag = null" @lostpointercapture="drag = null">
          <div class="preview-heading"><b>VECTORXR</b><span>your-game.exe</span><span v-if="osd.showClock">20:42</span></div>
          <div class="preview-metrics">
            <div><strong>90</strong><small>APP FPS</small></div>
            <div><strong>11.1<em> ms</em></strong><small>APP FRAME / AVG</small></div>
            <div><strong>12.4<em> ms</em></strong><small>APP FRAME / P95</small></div>
          </div>
          <template v-if="!osd.compact">
            <div v-if="osd.showGraph" class="preview-graph"><span>FRAME TIME / LAST 120 FRAMES</span><svg viewBox="0 0 300 52" preserveAspectRatio="none" aria-hidden="true"><polyline :points="graph" /></svg></div>
            <div v-if="osd.showRuntime" class="preview-row"><span>RUNTIME</span><b>Your OpenXR runtime</b></div>
            <div v-if="osd.showTurbo" class="preview-row"><span>TURBO</span><b class="preview-accent">Async / Experimental</b></div>
            <div v-if="osd.showModules" class="preview-row"><span>ENHANCEMENTS</span><b>Depth / Pivot / Quadviews</b></div>
            <p class="preview-footer">Application cadence. Not GPU or compositor FPS.</p>
          </template>
        </div>
        <span class="osd-stage-bottom">HEAD-FOLLOWING <span>· Approximate placement; headset field of view varies</span></span>
      </div>
      <div class="grid gap-6 p-5 md:grid-cols-2">
        <div class="space-y-4">
          <h3 class="text-sm font-semibold">Position &amp; size</h3>
          <label class="osd-control"><span>Horizontal <output>{{ osd.horizontalDegrees }}°</output></span><input v-model.number="osd.horizontalDegrees" aria-label="Horizontal position" type="range" min="-40" max="40" step="1" /><small>Left ← → Right</small></label>
          <label class="osd-control"><span>Vertical <output>{{ osd.verticalDegrees }}°</output></span><input v-model.number="osd.verticalDegrees" aria-label="Vertical position" type="range" min="-35" max="35" step="1" /><small>Down ← → Up</small></label>
          <label class="osd-control"><span>Size <output>{{ osd.scale }}%</output></span><input v-model.number="osd.scale" aria-label="Display size" type="range" min="50" max="150" step="5" /></label>
          <label class="osd-control"><span>Viewing distance <output>{{ osd.distanceMeters.toFixed(1) }} m</output></span><input v-model.number="osd.distanceMeters" aria-label="Viewing distance" type="range" min="0.5" max="3" step="0.1" /><small>Changes focus depth while keeping the same apparent size.</small></label>
          <button class="button-secondary rounded-lg px-3 py-2 text-xs" @click="osd.horizontalDegrees = 0; osd.verticalDegrees = 0">Center in view</button>
        </div>
        <div class="space-y-4">
          <h3 class="text-sm font-semibold">Appearance</h3>
          <div><span class="mb-2 block text-xs text-muted">Starting layout</span><div class="flex gap-2"><button v-for="compact in [false, true]" :key="String(compact)" :aria-pressed="osd.compact === compact" :class="osd.compact === compact ? 'button-accent' : 'button-secondary'" class="flex-1 rounded-lg px-3 py-2 text-sm" @click="osd.compact = compact">{{ compact ? 'Compact' : 'Detailed' }}</button></div></div>
          <label class="osd-control"><span>Opacity <output>{{ osd.opacity }}%</output></span><input v-model.number="osd.opacity" aria-label="Display opacity" type="range" min="30" max="100" step="1" /></label>
          <div><span class="mb-2 block text-xs text-muted">Accent</span><div class="flex gap-2"><button v-for="color in (['teal', 'copper', 'blue'] as const)" :key="color" class="color-choice rounded-lg border px-3 py-2 text-xs capitalize" :aria-pressed="osd.accent === color" :style="{ '--swatch': ({ teal: '#51ddbd', copper: '#f5ae78', blue: '#6facff' })[color] }" @click="osd.accent = color"><i />{{ color }}</button></div></div>
          <label class="block text-sm"><span class="mb-2 block text-xs text-muted">Panel refresh rate</span><select v-model.number="osd.updateHz" class="w-full rounded-lg border px-3 py-2"><option v-for="hz in [1, 2, 5, 10, 20]" :key="hz" :value="hz">{{ hz }} Hz{{ hz === 5 ? ' · Recommended' : '' }}</option><option v-if="![1, 2, 5, 10, 20].includes(osd.updateHz)" :value="osd.updateHz">{{ osd.updateHz }} Hz</option></select><small class="mt-1 block text-muted">The frame history samples every frame. Higher refresh rates redraw the panel more often.</small></label>
          <label class="flex items-center gap-3 text-sm"><input v-model="osd.visibleOnStart" type="checkbox" />Show automatically when enabled or a session starts</label>
        </div>
      </div>
    </article>

    <div class="grid gap-5 lg:grid-cols-2">
      <article class="surface-panel rounded-[1.25rem] border p-5">
        <h3 class="font-semibold">On your display</h3>
        <p class="mt-1 text-xs leading-5 text-muted">FPS, average frame time, and P95 are always visible. Detailed layout adds the items below; compact keeps only the metrics and clock.</p>
        <div class="mt-4 space-y-4"><label v-for="item in contents" :key="item.key" class="flex items-start gap-3"><input v-model="osd[item.key]" type="checkbox" class="mt-1" /><span><span class="block text-sm font-medium">{{ item.label }}</span><small class="mt-0.5 block text-muted">{{ item.hint }}</small></span></label></div>
      </article>
      <article class="surface-panel rounded-[1.25rem] border p-5">
        <h3 class="font-semibold">In-headset controls</h3><p class="mt-1 text-xs leading-5 text-muted">Use a keyboard or your flight controls without leaving the game.</p>
        <div class="mt-4 space-y-3"><ModuleBindingPanel heading="Show / hide" :binding="osd.toggleBinding" @edit="editing = 'toggleBinding'" /><ModuleBindingPanel heading="Compact / detailed" :binding="osd.cycleBinding" @edit="editing = 'cycleBinding'" /><BindingConflictWarnings :warnings="warnings" /></div>
        <h4 class="mt-5 text-sm font-semibold">VR session</h4>
        <p v-if="statusError" class="mt-2 text-xs text-muted">{{ statusError }}</p>
        <p v-else-if="!sessions.length" class="mt-2 text-xs leading-5 text-muted">No active VR application detected. Save your layout, then launch a Direct3D 11 OpenXR game with VectorXR registered.</p>
        <div v-for="session in sessions" :key="session.sessionId" class="mt-3 rounded-lg border p-3 text-xs"><strong>{{ session.application }}</strong><p class="mt-1 text-muted">{{ session.state.osdMessage || 'This session needs the updated VectorXR layer.' }}<span v-if="session.state.osdVisible"> · {{ session.state.osdCompact ? 'Compact' : 'Detailed' }}</span></p></div>
      </article>
    </div>
    <details class="surface-panel rounded-[1.25rem] border p-5 text-sm">
      <summary class="cursor-pointer font-semibold">What applies live, and what am I measuring?</summary>
      <div class="mt-4 space-y-3 text-muted leading-6"><p>Every setting on this page applies after Save, including enabling the OSD in a running game. Bindings change visibility and layout immediately for the current session. The next session uses your saved starting layout and visibility.</p><p>The overlay measures intervals between the application's xrEndFrame calls over the last 120 frames. P95 is the interval at the 95th percentile; lower, steadier frame times are better. These values do not measure GPU work, reprojection, or compositor frame rate. Turbo can make application cadence differ from the headset's refresh rate.</p><p>Turbo timing experiments and Quadviews enable/disable changes require an application restart. The detailed OSD shows these pending changes. Other VectorXR controls retain the behavior described on their own settings pages.</p><p>This first version is an information panel with in-game visibility and layout controls. Edit settings in the desktop app. Direct3D 12, Vulkan, and OpenGL are not supported yet. A runtime with no spare composition layers cannot show the panel.</p></div>
    </details>
  </div>
</template>

<style scoped>
.live-badge { color: #238973; background: #51ddbd18; border: 1px solid #51ddbd40; padding: 4px 9px; border-radius: 999px; font-weight: 600; }
.osd-stage { height: 410px; position: relative; overflow: hidden; background: radial-gradient(ellipse at 50% 42%, #273d54 0%, #132232 48%, #0b121c 100%); color: #9eb1c7; }
.osd-stage::before { content: ''; position: absolute; inset: 0; background-image: linear-gradient(#a9d6ff07 1px, transparent 1px), linear-gradient(90deg, #a9d6ff07 1px, transparent 1px); background-size: 40px 40px; }
.osd-horizon { position: absolute; top: 50%; left: 0; right: 0; height: 1px; background: #9cbdd329; }
.osd-reticle { position: absolute; top: 50%; left: 50%; transform: translate(-50%, -50%); font-size: 22px; color: #a4c1d685; }
.osd-stage-note, .osd-stage-bottom { position: absolute; left: 18px; font-size: 9px; letter-spacing: .13em; pointer-events: none; }
.osd-stage-note { top: 15px; } .osd-stage-bottom { bottom: 14px; } .osd-stage-note span, .osd-stage-bottom span { letter-spacing: .02em; opacity: .7; }
.osd-preview { position: absolute; transform: translate(-50%,-50%); background: #0d141f; color: #e9f0f8; border-radius: 10px; border-left: 3px solid var(--osd-accent); padding: 12px; box-shadow: 0 16px 48px #0006; cursor: grab; user-select: none; touch-action: none; container-type: inline-size; }
.osd-preview:active { cursor: grabbing; }
.preview-heading { display: flex; align-items: center; gap: 10px; font-size: clamp(7px,2.3cqw,11px); color: #94a5b8; white-space: nowrap; }
.preview-heading b { color: var(--osd-accent); letter-spacing: .12em; } .preview-heading span:nth-child(2) { flex: 1; overflow: hidden; }
.preview-metrics { display: grid; grid-template-columns: 1fr 1.2fr 1.2fr; gap: 8px; margin: 12px 0; }
.preview-metrics strong { display: block; font-size: clamp(14px,7.1cqw,32px); font-weight: 600; line-height: 1.2; white-space: nowrap; }
.preview-metrics em { font-size: .55em; font-style: normal; } .preview-metrics small { display: block; font-size: clamp(5px,1.9cqw,9px); color: #94a5b8; margin-top: 5px; white-space: nowrap; }
.preview-metrics div:last-child strong, .preview-accent { color: var(--osd-accent); }
.preview-graph { background: #141f2d; padding: 6px 8px; margin: 12px 0; } .preview-graph span { font-size: clamp(5px,1.8cqw,8px); color: #94a5b8; display: block; margin-bottom: 3px; }
.preview-graph svg { width: 100%; height: 48px; } .preview-graph polyline { fill: none; stroke: var(--osd-accent); stroke-width: 1.6; }
.preview-row { display: flex; gap: 10px; padding: 5px 0; font-size: clamp(6px,2.3cqw,10px); white-space: nowrap; } .preview-row span { width: 24%; color: #94a5b8; font-size: .8em; } .preview-row b { font-weight: 500; overflow: hidden; text-overflow: ellipsis; }
.preview-footer { color: #94a5b8; font-size: clamp(5px,1.8cqw,8px); margin-top: 10px; }
.osd-control { display: block; font-size: 13px; } .osd-control > span { display: flex; justify-content: space-between; gap: 12px; } .osd-control output { font-variant-numeric: tabular-nums; color: var(--app-text-muted); } .osd-control input { width: 100%; margin-top: 8px; accent-color: #39b69b; } .osd-control small { display: block; font-size: 11px; color: var(--app-text-muted); }
.color-choice { display: flex; align-items: center; gap: 6px; border-color: var(--app-border); } .color-choice[aria-pressed=true] { border-color: var(--swatch); background: color-mix(in srgb, var(--swatch) 12%, transparent); } .color-choice i { width: 9px; height: 9px; border-radius: 50%; background: var(--swatch); }
@media (max-width: 800px) { .osd-stage { height: 340px; } .osd-preview { padding: 8px; } .preview-graph svg { height: 30px; } }
</style>
