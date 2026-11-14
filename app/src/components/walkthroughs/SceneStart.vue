<script setup lang="ts">
// The start walkthrough: what VectorXR is, and how to turn it off.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import { fitText as fit } from '../../lib/walkthroughs'

const props = defineProps<{ step: number; beat: number; time: number; spb: number; caption: string }>()

// Layout in a 640 x 270 view: the frame path runs along y = 165.
const nodes = { game: 70, layer: 245, runtime: 410, headset: 570 }
const showRuntime = computed(() => props.step >= 1)
const showLayer = computed(() => props.step >= 2)
const showApp = computed(() => props.step === 3 || (props.step === 4 && props.beat < 3))
const appClosing = computed(() => props.step === 4 && props.beat >= 2)
const savePressed = computed(() => props.step === 3 && props.beat >= 2 && props.beat < 3)
// The settings file flies from the app to the layer on beats 4 to 7 of step 4.
const fileProgress = computed(() => (props.step === 3 ? Math.max(0, Math.min(1, (props.beat - 4) / 3)) : props.step > 3 ? 1 : 0))
const layerRead = computed(() => props.step === 3 && props.beat >= 7 && props.beat < 9)
const stillRunning = computed(() => props.step === 4 && props.beat >= 6)
const switchOff = computed(() => props.step === 5 && props.beat >= 2)
const layerDim = computed(() => switchOff.value)
const chips = computed(() => [t('app.tabs.quadviews.label'), t('app.tabs.turbo.label'), t('app.tabs.framegen.label'), t('app.tabs.pivotxr.label')])
// Chips light one by one on beats 4, 6, 8 and 10 of step 3, and go out with the switch.
function chipLit(index: number) {
  if (switchOff.value) return false
  if (props.step > 2) return true
  return props.step === 2 && props.beat >= 4 + 2 * index
}

// Frames travel the path through every node that is showing. Each dot is a
// frame; while the layer works on them they take its accent color.
const pathNodes = computed(() => [nodes.game, ...(showLayer.value ? [nodes.layer] : []), ...(showRuntime.value ? [nodes.runtime] : []), nodes.headset])
const frames = computed(() => {
  const xs = pathNodes.value
  const from = xs[0] + 50
  const to = xs[xs.length - 1] - 45
  const count = 5
  return Array.from({ length: count }, (_, k) => {
    const phase = (props.time / (props.spb * 4) + k / count) % 1
    const x = from + (to - from) * phase
    const touched = showLayer.value && !layerDim.value && x > nodes.layer
    // A frame inside a box is being worked on there: it disappears behind it.
    const hidden = boxes.value.some(([center, half]) => Math.abs(x - center) < half + 4)
    return { x, touched, hidden }
  }).filter((f) => !f.hidden)
})
const boxes = computed<Array<[number, number]>>(() => [
  [nodes.game, 55],
  ...(showLayer.value ? [[nodes.layer, 62] as [number, number]] : []),
  ...(showRuntime.value ? [[nodes.runtime, 60] as [number, number]] : []),
  [nodes.headset, 45],
])
</script>

<template>
  <div class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <!-- the app window (steps 4 and 5) -->
      <g class="wt-fade" :class="{ 'wt-hidden': !showApp, 'wt-closing': appClosing }">
        <rect x="120" y="14" width="190" height="72" rx="10" class="wt-box" />
        <rect x="120" y="14" width="190" height="18" rx="10" class="wt-titlebar" />
        <text x="132" y="27" class="wt-tiny">{{ t('guides.diagram.app') }}</text>
        <text x="298" y="27" text-anchor="end" class="wt-tiny" :class="{ 'wt-x-hot': appClosing }">✕</text>
        <rect x="190" y="46" width="108" height="26" rx="7" class="wt-button" :class="{ 'wt-pressed': savePressed }" />
        <text x="244" y="63" text-anchor="middle" class="wt-small wt-on-accent" :textLength="fit(t('app.saveBar.save'), 98, 12)" lengthAdjust="spacingAndGlyphs">{{ t('app.saveBar.save') }}</text>
      </g>
      <!-- where the app was, once it is closed (step 5) -->
      <g class="wt-fade" :class="{ 'wt-hidden': !(step === 4 && beat >= 3) }">
        <rect x="120" y="14" width="190" height="72" rx="10" class="wt-ghost" />
        <text x="215" y="55" text-anchor="middle" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.appClosed'), 176, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.appClosed') }}</text>
      </g>
      <!-- the settings file travelling to the layer -->
      <g v-if="fileProgress > 0 && fileProgress < 1 && step === 3" :transform="`translate(${244 + (245 - 244) * fileProgress}, ${86 + (120 - 86) * fileProgress})`">
        <path d="M -9 -11 h 12 l 6 6 v 16 h -18 Z" class="wt-file" />
      </g>
      <text v-if="step === 3 && fileProgress > 0.05" x="330" y="108" class="wt-small" :textLength="fit(t('guides.diagram.settingsFile'), 150, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.settingsFile') }}</text>

      <!-- the VectorXR Enabled switch (step 6) -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 5 }">
        <text x="400" y="56" text-anchor="end" class="wt-small" :textLength="fit(t('settings.core.general.enabled.title'), 160, 13)" lengthAdjust="spacingAndGlyphs">{{ t('settings.core.general.enabled.title') }}</text>
        <rect x="412" y="40" width="44" height="22" rx="11" class="wt-switch" :class="{ 'wt-switch-off': switchOff }" />
        <circle :cx="switchOff ? 423 : 445" cy="51" r="8" class="wt-knob" />
      </g>

      <!-- the frame path -->
      <path :d="`M ${nodes.game + 50} 165 H ${nodes.headset - 45}`" class="wt-track" />
      <circle v-for="(f, k) in frames" :key="k" :cx="f.x" cy="165" r="5" :class="f.touched ? 'wt-frame-touched' : 'wt-frame'" />

      <!-- the boxes -->
      <g>
        <rect :x="nodes.game - 55" y="140" width="110" height="50" rx="12" class="wt-box" />
        <text :x="nodes.game" y="170" text-anchor="middle" class="wt-label" :textLength="fit(t('guides.diagram.game'), 96, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.game') }}</text>
      </g>
      <g class="wt-drop" :class="{ 'wt-hidden-up': !showLayer, 'wt-dim': layerDim, 'wt-flash': layerRead }">
        <rect :x="nodes.layer - 62" y="136" width="124" height="58" rx="12" class="wt-accent" />
        <text :x="nodes.layer" y="170" text-anchor="middle" class="wt-label wt-strong" :textLength="fit(t('guides.diagram.layer'), 110, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.layer') }}</text>
      </g>
      <g class="wt-drop" :class="{ 'wt-hidden-up': !showRuntime }">
        <rect :x="nodes.runtime - 60" y="140" width="120" height="50" rx="12" class="wt-box" />
        <text :x="nodes.runtime" y="170" text-anchor="middle" class="wt-label" :textLength="fit(t('guides.diagram.runtime'), 106, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.runtime') }}</text>
      </g>
      <g>
        <rect :x="nodes.headset - 45" y="140" width="90" height="50" rx="12" class="wt-box" />
        <text :x="nodes.headset" y="170" text-anchor="middle" class="wt-label" :textLength="fit(t('guides.diagram.headset'), 78, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.headset') }}</text>
      </g>

      <!-- the enhancements the layer adds -->
      <g v-if="showLayer">
        <g v-for="(chip, index) in chips" :key="chip">
          <rect :x="62 + index * 130" y="222" width="120" height="26" rx="13" class="wt-chip" :class="{ 'wt-chip-on': chipLit(index) }" />
          <text :x="122 + index * 130" y="239" text-anchor="middle" class="wt-tiny" :class="{ 'wt-strong': chipLit(index) }" :textLength="fit(chip, 108, 11)" lengthAdjust="spacingAndGlyphs">{{ chip }}</text>
        </g>
      </g>

      <!-- notes under the layer -->
      <text v-if="stillRunning" :x="nodes.layer" y="125" text-anchor="middle" class="wt-small wt-good">{{ t('guides.walkthrough.labels.stillRunning') }}</text>
      <text v-if="switchOff" :x="nodes.layer" y="125" text-anchor="middle" class="wt-small">{{ t('guides.walkthrough.labels.passThrough') }}</text>
    </svg>
  </div>
</template>
