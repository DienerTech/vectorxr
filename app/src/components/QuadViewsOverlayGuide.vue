<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

const props = defineProps<{
  open: boolean
}>()

const emit = defineEmits<{
  close: []
}>()

type GuideStep = 'zones' | 'alignment' | 'motion' | 'troubleshoot'
type Symptom = 'boundary' | 'offset' | 'jitter' | 'lag' | 'lost' | 'performance'

const activeStep = ref<GuideStep>('zones')
const focusSizePercent = ref(42)
const transitionPercent = ref(22)
const offsetXDegrees = ref(5)
const offsetYDegrees = ref(-2)
const smoothingPercent = ref(45)
const trackingAvailable = ref(true)
const rawGazeX = ref(474)
const rawGazeY = ref(150)
const activeSymptom = ref<Symptom>('boundary')

const steps: Array<{ id: GuideStep; number: string; title: string; summary: string }> = [
  { id: 'zones', number: '01', title: 'Read the zones', summary: 'See where each image contributes.' },
  { id: 'alignment', number: '02', title: 'Check alignment', summary: 'Compare centers, offset, and gaze.' },
  { id: 'motion', number: '03', title: 'Judge tracking', summary: 'Spot jitter, lag, and tracking loss.' },
  { id: 'troubleshoot', number: '04', title: 'Choose a fix', summary: 'Map a symptom to the right control.' },
]

const symptoms: Record<Symptom, { label: string; title: string; body: string; controls: string[] }> = {
  boundary: {
    label: 'Visible seam',
    title: 'The focus edge is easy to see',
    body: 'Increase Transition Thickness a little. If the amber band becomes broadly soft, reduce it again and narrow the resolution difference between Focus and Peripheral.',
    controls: ['Transition Thickness', 'Focus Resolution', 'Peripheral Resolution'],
  },
  offset: {
    label: 'Wrong center',
    title: 'The sharp region is consistently biased',
    body: 'First confirm the cyan and yellow gaze markers land where you look. If they do, use Horizontal or Vertical Offset to correct the consistent bias. Do not use offset to hide gaze lag.',
    controls: ['Horizontal Offset', 'Vertical Offset'],
  },
  jitter: {
    label: 'Jitter',
    title: 'The focus window trembles near your gaze',
    body: 'Raise Smoothing gradually. A small Deadzone can suppress tiny eye movements near center, but too much creates a sticky jump when the focus finally moves.',
    controls: ['Smoothing', 'Deadzone'],
  },
  lag: {
    label: 'Too much lag',
    title: 'The yellow marker trails too far behind cyan',
    body: 'Reduce Smoothing. If movement starts late and then jumps, reduce Deadzone too. Use the smallest values that still keep the focus window visually calm.',
    controls: ['Smoothing', 'Deadzone'],
  },
  lost: {
    label: 'Red outline',
    title: 'Eye tracking is unavailable',
    body: 'A red focus outline is a status warning, not a color preference. Confirm eye tracking is enabled in the game and headset runtime. VectorXR falls back instead of following eye gaze.',
    controls: ['Tracking Mode', 'Game eye tracking', 'Headset runtime'],
  },
  performance: {
    label: 'Need more FPS',
    title: 'Reduce pixels where they matter least',
    body: 'Lower Peripheral Resolution first. Then reduce Focus Width or Height if needed. Focus Resolution changes density inside the window - it does not change the green outline size.',
    controls: ['Peripheral Resolution', 'Focus Width / Height', 'Focus Resolution'],
  },
}

const headX = 360
const headY = 200
const configuredX = computed(() => headX + offsetXDegrees.value * 5)
const configuredY = computed(() => headY + offsetYDegrees.value * 5)
const smoothingFollow = computed(() => 1 - smoothingPercent.value * 0.0075)
const smoothedX = computed(() => headX + (rawGazeX.value - headX) * smoothingFollow.value)
const smoothedY = computed(() => headY + (rawGazeY.value - headY) * smoothingFollow.value)
const trackedX = computed(() => trackingAvailable.value ? smoothedX.value : headX)
const trackedY = computed(() => trackingAvailable.value ? smoothedY.value : headY)
const focusCenterX = computed(() => trackedX.value + offsetXDegrees.value * 5)
const focusCenterY = computed(() => trackedY.value + offsetYDegrees.value * 5)
const focusWidth = computed(() => 720 * focusSizePercent.value / 100)
const focusHeight = computed(() => 400 * focusSizePercent.value * 0.82 / 100)
const focusLeft = computed(() => focusCenterX.value - focusWidth.value / 2)
const focusTop = computed(() => focusCenterY.value - focusHeight.value / 2)
const transitionX = computed(() => Math.min(focusWidth.value * transitionPercent.value / 100, focusWidth.value / 2 - 2))
const transitionY = computed(() => Math.min(focusHeight.value * transitionPercent.value / 100, focusHeight.value / 2 - 2))
const currentSymptom = computed(() => symptoms[activeSymptom.value])

function setGazeFromPointer(event: PointerEvent) {
  const target = event.currentTarget as SVGSVGElement
  const bounds = target.getBoundingClientRect()
  rawGazeX.value = Math.max(24, Math.min(696, (event.clientX - bounds.left) / bounds.width * 720))
  rawGazeY.value = Math.max(24, Math.min(376, (event.clientY - bounds.top) / bounds.height * 400))
}

function resetDemo() {
  focusSizePercent.value = 42
  transitionPercent.value = 22
  offsetXDegrees.value = 5
  offsetYDegrees.value = -2
  smoothingPercent.value = 45
  trackingAvailable.value = true
  rawGazeX.value = 474
  rawGazeY.value = 150
}

function handleKeydown(event: KeyboardEvent) {
  if (props.open && event.key === 'Escape') emit('close')
}

watch(() => props.open, (open) => {
  if (open) activeStep.value = 'zones'
})

onMounted(() => window.addEventListener('keydown', handleKeydown))
onUnmounted(() => window.removeEventListener('keydown', handleKeydown))
</script>

<template>
  <div
    v-if="open"
    class="fixed inset-0 z-[70] flex items-center justify-center bg-black/55 px-3 py-4 backdrop-blur-sm md:px-6"
    role="dialog"
    aria-modal="true"
    aria-labelledby="quadviews-guide-title"
    @click.self="emit('close')"
  >
    <div class="flex h-[92vh] max-h-[760px] w-full max-w-[1180px] flex-col overflow-hidden rounded-[1.5rem] border shadow-panel surface-panel-strong">
      <header class="flex shrink-0 items-start justify-between gap-4 border-b px-5 py-4 md:px-6" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-[10px] uppercase tracking-[0.22em]">Interactive field guide</p>
          <h2 id="quadviews-guide-title" class="mt-1 text-xl font-semibold tracking-tight md:text-2xl">Reading the Quadviews overlay</h2>
          <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">Learn the picture first, then change the setting that matches what you actually see.</p>
        </div>
        <button class="button-secondary rounded-[0.7rem] px-3 py-2 text-sm font-medium" type="button" aria-label="Close overlay guide" @click="emit('close')">Close</button>
      </header>

      <div class="grid min-h-0 flex-1 overflow-hidden lg:grid-cols-[15rem_minmax(0,1fr)]">
        <nav class="flex gap-2 overflow-x-auto border-b p-3 lg:flex-col lg:overflow-y-auto lg:border-b-0 lg:border-r lg:p-4" aria-label="Overlay guide steps" style="border-color: var(--app-border)">
          <button
            v-for="item in steps"
            :key="item.id"
            class="min-w-[12rem] rounded-[0.9rem] border px-3 py-3 text-left transition lg:min-w-0"
            :class="activeStep === item.id ? 'button-accent' : 'surface-panel-soft'"
            type="button"
            @click="activeStep = item.id"
          >
            <span class="text-[10px] font-bold tracking-[0.18em] opacity-70">{{ item.number }}</span>
            <strong class="mt-1 block text-sm">{{ item.title }}</strong>
            <span class="mt-1 block text-xs leading-5 opacity-75">{{ item.summary }}</span>
          </button>
        </nav>

        <main class="min-h-0 overflow-y-auto p-4 md:p-5">
          <div class="grid gap-4 xl:grid-cols-[minmax(0,1.45fr)_minmax(17rem,0.7fr)]">
            <section class="min-w-0">
              <div class="relative overflow-hidden rounded-[1.15rem] border bg-[#07101f] shadow-inner" style="border-color: var(--app-border)">
                <svg
                  class="block aspect-[16/9] w-full touch-none select-none"
                  viewBox="0 0 720 400"
                  role="img"
                  aria-label="Interactive illustration of the Quadviews diagnostic overlay"
                  @pointerdown="setGazeFromPointer"
                  @pointermove="($event.buttons & 1) && setGazeFromPointer($event)"
                >
                  <defs>
                    <linearGradient id="qv-sky" x1="0" y1="0" x2="0" y2="1">
                      <stop offset="0" stop-color="#18345d" />
                      <stop offset="0.58" stop-color="#315d77" />
                      <stop offset="0.59" stop-color="#41543b" />
                      <stop offset="1" stop-color="#101923" />
                    </linearGradient>
                    <pattern id="qv-grid" width="36" height="36" patternUnits="userSpaceOnUse">
                      <path d="M 36 0 L 0 0 0 36" fill="none" stroke="#ffffff" stroke-opacity="0.055" stroke-width="1" />
                    </pattern>
                  </defs>

                  <rect width="720" height="400" fill="url(#qv-sky)" />
                  <path d="M0 252 C90 216 153 239 232 218 C318 196 400 240 474 210 C563 174 629 213 720 181 L720 400 L0 400Z" fill="#17231c" />
                  <path d="M0 282 C132 248 218 283 335 255 C454 227 564 268 720 231" fill="none" stroke="#9ab083" stroke-opacity="0.28" stroke-width="3" />
                  <path d="M70 400 L272 235 L448 235 L650 400" fill="#09101a" fill-opacity="0.72" />
                  <path d="M312 400 L345 235 M408 400 L375 235" stroke="#d4d9df" stroke-opacity="0.28" stroke-width="3" stroke-dasharray="18 18" />
                  <rect width="720" height="400" fill="url(#qv-grid)" />

                  <rect width="720" height="400" fill="#1447f2" fill-opacity="0.14" />
                  <rect :x="focusLeft" :y="focusTop" :width="focusWidth" :height="focusHeight" rx="3" fill="#ff8c0a" fill-opacity="0.24" />
                  <rect
                    :x="focusLeft + transitionX"
                    :y="focusTop + transitionY"
                    :width="Math.max(1, focusWidth - transitionX * 2)"
                    :height="Math.max(1, focusHeight - transitionY * 2)"
                    rx="2"
                    fill="#15d947"
                    fill-opacity="0.12"
                    stroke="#ffad16"
                    stroke-opacity="0.92"
                    stroke-width="2"
                  />
                  <rect
                    :x="focusLeft"
                    :y="focusTop"
                    :width="focusWidth"
                    :height="focusHeight"
                    rx="3"
                    fill="none"
                    :stroke="trackingAvailable ? '#26ff50' : '#ff1710'"
                    stroke-width="4"
                  />

                  <g v-if="activeStep !== 'zones'">
                    <path :d="`M ${headX} ${headY} L ${configuredX} ${headY} L ${configuredX} ${configuredY}`" fill="none" stroke="#ff1ad9" stroke-width="3" stroke-opacity="0.9" />
                    <path :d="`M ${headX - 14} ${headY} H ${headX + 14} M ${headX} ${headY - 14} V ${headY + 14}`" stroke="white" stroke-width="3" />
                    <circle :cx="configuredX" :cy="configuredY" r="12" fill="none" stroke="#ff1ad9" stroke-width="3" />
                    <circle :cx="rawGazeX" :cy="rawGazeY" r="8" fill="none" stroke="#00f2ff" stroke-width="4" :opacity="trackingAvailable ? 1 : 0.22" />
                    <path :d="`M ${smoothedX - 9} ${smoothedY} H ${smoothedX + 9} M ${smoothedX} ${smoothedY - 9} V ${smoothedY + 9}`" stroke="#fff20d" stroke-width="4" :opacity="trackingAvailable ? 1 : 0.22" />
                  </g>

                  <g v-if="activeStep === 'zones'" class="qv-svg-labels">
                    <text x="30" y="42">BLUE - PERIPHERAL CONTEXT</text>
                    <text :x="focusLeft + 12" :y="focusTop + 24">AMBER - BLEND</text>
                    <text :x="focusCenterX" :y="focusCenterY + 5" text-anchor="middle">FOCUS IMAGE</text>
                  </g>
                  <g v-else-if="activeStep === 'alignment'" class="qv-svg-labels">
                    <text :x="headX - 18" :y="headY + 32" text-anchor="end">HEAD CENTER</text>
                    <text :x="configuredX - 14" :y="configuredY - 18" text-anchor="end">CONFIGURED OFFSET</text>
                    <text :x="rawGazeX + 14" :y="rawGazeY - 12">RAW</text>
                    <text :x="smoothedX + 18" :y="smoothedY + 34">SMOOTHED</text>
                  </g>
                  <g v-else-if="activeStep === 'motion'" class="qv-svg-labels">
                    <text x="24" y="36">CLICK OR DRAG TO MOVE RAW GAZE</text>
                    <text v-if="!trackingAvailable" x="360" y="370" text-anchor="middle" fill="#ff4a45">TRACKING UNAVAILABLE - FALLBACK ACTIVE</text>
                  </g>
                </svg>

                <div class="pointer-events-none absolute bottom-2 right-2 rounded-full bg-black/55 px-2.5 py-1 text-[10px] font-semibold tracking-wide text-white/75">Illustrative - not a pixel-perfect headset capture</div>
              </div>

              <div v-if="activeStep === 'zones'" class="mt-3 grid gap-3 sm:grid-cols-2">
                <label class="rounded-[0.85rem] border p-3 surface-panel-soft">
                  <span class="flex justify-between gap-3 text-xs font-semibold"><span>Focus size</span><span>{{ focusSizePercent }}%</span></span>
                  <input v-model.number="focusSizePercent" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" type="range" min="20" max="72" step="1" />
                </label>
                <label class="rounded-[0.85rem] border p-3 surface-panel-soft">
                  <span class="flex justify-between gap-3 text-xs font-semibold"><span>Transition thickness</span><span>{{ transitionPercent }}%</span></span>
                  <input v-model.number="transitionPercent" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" type="range" min="0" max="42" step="1" />
                </label>
              </div>

              <div v-else-if="activeStep === 'alignment'" class="mt-3 grid gap-3 sm:grid-cols-2">
                <label class="rounded-[0.85rem] border p-3 surface-panel-soft">
                  <span class="flex justify-between gap-3 text-xs font-semibold"><span>Horizontal offset</span><span>{{ offsetXDegrees }} deg</span></span>
                  <input v-model.number="offsetXDegrees" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" type="range" min="-18" max="18" step="1" />
                </label>
                <label class="rounded-[0.85rem] border p-3 surface-panel-soft">
                  <span class="flex justify-between gap-3 text-xs font-semibold"><span>Vertical offset</span><span>{{ offsetYDegrees }} deg</span></span>
                  <input v-model.number="offsetYDegrees" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" type="range" min="-12" max="12" step="1" />
                </label>
              </div>

              <div v-else-if="activeStep === 'motion'" class="mt-3 grid gap-3 sm:grid-cols-[minmax(0,1fr)_auto]">
                <label class="rounded-[0.85rem] border p-3 surface-panel-soft">
                  <span class="flex justify-between gap-3 text-xs font-semibold"><span>Smoothing</span><span>{{ smoothingPercent }}%</span></span>
                  <input v-model.number="smoothingPercent" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" type="range" min="0" max="95" step="1" />
                </label>
                <button class="rounded-[0.85rem] border px-4 py-3 text-left text-xs font-semibold" :class="trackingAvailable ? 'chip-success' : 'chip-danger'" type="button" @click="trackingAvailable = !trackingAvailable">
                  Eye tracking: {{ trackingAvailable ? 'Available' : 'Lost' }}
                </button>
              </div>
            </section>

            <aside class="space-y-3">
              <template v-if="activeStep === 'zones'">
                <GuideCard color="#1447f2" title="Peripheral area" setting="Peripheral Resolution">Blue covers the context image. Lowering its resolution usually saves the most pixels without shrinking your sharp region.</GuideCard>
                <GuideCard color="#ff8c0a" title="Transition band" setting="Transition Thickness">Amber is the blend between images. Wider can hide a seam; too wide can make the edge feel soft.</GuideCard>
                <GuideCard color="#26ff50" title="Focus boundary" setting="Focus Width / Height">The green outline is the focus window size. Focus Resolution changes detail density, not this outline.</GuideCard>
              </template>

              <template v-else-if="activeStep === 'alignment'">
                <GuideCard color="#ffffff" title="Head center" setting="Reference only">The white cross is straight ahead from the headset. It gives every other marker a stable reference.</GuideCard>
                <GuideCard color="#ff1ad9" title="Configured offset" setting="Horizontal / Vertical Offset">The magenta L and ring show your intentional correction from head center.</GuideCard>
                <GuideCard color="#00f2ff" title="Raw gaze" setting="Tracking source">Cyan is the immediate eye-tracking sample. Click the illustration to move it.</GuideCard>
                <GuideCard color="#fff20d" title="Smoothed gaze" setting="Smoothing / Deadzone">Yellow is the filtered position that drives the focus window before configured offset is added.</GuideCard>
              </template>

              <template v-else-if="activeStep === 'motion'">
                <div class="rounded-[1rem] border p-4 surface-panel-soft">
                  <p class="text-sm font-semibold">What good tracking looks like</p>
                  <ul class="mt-2 list-disc space-y-1.5 pl-5 text-xs leading-5 text-muted">
                    <li>Cyan responds immediately to your eyes.</li>
                    <li>Yellow follows closely without constant trembling.</li>
                    <li>The focus window stays on the intended detail.</li>
                  </ul>
                </div>
                <GuideCard :color="trackingAvailable ? '#26ff50' : '#ff1710'" :title="trackingAvailable ? 'Green outline' : 'Red outline'" setting="Tracking Mode">{{ trackingAvailable ? 'Eye tracking is available. Compare cyan and yellow to judge filtering.' : 'The eye source is unavailable. VectorXR is using its fallback focus behavior.' }}</GuideCard>
                <button class="button-secondary w-full rounded-[0.75rem] px-3 py-2 text-xs font-semibold" type="button" @click="resetDemo">Reset illustration</button>
              </template>

              <template v-else>
                <div class="flex flex-wrap gap-2">
                  <button v-for="(symptom, id) in symptoms" :key="id" class="rounded-full border px-3 py-1.5 text-xs font-semibold" :class="activeSymptom === id ? 'button-accent' : 'button-secondary'" type="button" @click="activeSymptom = id as Symptom">{{ symptom.label }}</button>
                </div>
                <div class="rounded-[1rem] border p-4 surface-panel-soft">
                  <p class="eyebrow text-[10px] uppercase tracking-[0.18em]">If you see this</p>
                  <h3 class="mt-2 text-base font-semibold">{{ currentSymptom.title }}</h3>
                  <p class="mt-2 text-sm leading-6 text-muted">{{ currentSymptom.body }}</p>
                  <div class="mt-3 flex flex-wrap gap-2">
                    <span v-for="control in currentSymptom.controls" :key="control" class="rounded-full border px-2.5 py-1 text-[11px] font-semibold chip-warning">{{ control }}</span>
                  </div>
                </div>
                <div class="rounded-[1rem] border p-4 text-xs leading-5 surface-panel-soft">
                  <strong>Good tuning order:</strong>
                  <ol class="mt-2 list-decimal space-y-1 pl-5 text-muted">
                    <li>Confirm tracking is available.</li>
                    <li>Correct a consistent alignment bias.</li>
                    <li>Balance smoothing versus lag.</li>
                    <li>Then tune image quality and pixel cost.</li>
                  </ol>
                </div>
              </template>
            </aside>
          </div>
        </main>
      </div>
    </div>
  </div>
</template>

<script lang="ts">
import { defineComponent, h } from 'vue'

export const GuideCard = defineComponent({
  name: 'GuideCard',
  props: {
    color: { type: String, required: true },
    title: { type: String, required: true },
    setting: { type: String, required: true },
  },
  setup(props, { slots }) {
    return () => h('div', { class: 'rounded-[1rem] border p-4 surface-panel-soft' }, [
      h('div', { class: 'flex items-start gap-3' }, [
        h('span', { class: 'mt-1 h-3 w-3 shrink-0 rounded-full shadow-sm', style: { background: props.color } }),
        h('div', { class: 'min-w-0' }, [
          h('p', { class: 'text-sm font-semibold' }, props.title),
          h('p', { class: 'mt-1 text-xs leading-5 text-muted' }, slots.default?.()),
          h('span', { class: 'mt-2 inline-flex rounded-full border px-2.5 py-1 text-[10px] font-bold tracking-wide chip-warning' }, props.setting),
        ]),
      ]),
    ])
  },
})
</script>

<style scoped>
.qv-svg-labels text {
  fill: rgba(255, 255, 255, 0.92);
  font-family: ui-sans-serif, system-ui, sans-serif;
  font-size: 12px;
  font-weight: 800;
  letter-spacing: 0.08em;
  paint-order: stroke;
  stroke: rgba(2, 7, 16, 0.78);
  stroke-width: 4px;
  stroke-linejoin: round;
}
</style>



