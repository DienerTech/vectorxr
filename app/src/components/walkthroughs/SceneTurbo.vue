<script setup lang="ts">
// Turbo: less time waiting on the runtime, more frames in the same time.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, reached, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// Two timelines over the same span (x 150..620). Without Turbo each frame is a
// long wait then the render; with Turbo the wait is short, so five frames fit
// where four did. Blocks appear one per beat pair as each row is drawn.
const span = { x: 150, w: 470 }
const without = Array.from({ length: 4 }, (_, k) => ({ x: span.x + k * 117.5, wait: 52, work: 60 }))
const withTurbo = Array.from({ length: 5 }, (_, k) => ({ x: span.x + k * 94, wait: 18, work: 70 }))

const shownWithout = computed(() => (props.step === 0 ? Math.min(4, Math.floor(props.beat / 2) + 1) : 4))
const shownWith = computed(() => (props.step < 1 ? 0 : props.step === 1 ? Math.min(5, Math.floor(props.beat / 2) + 1) : 5))
// Step 3: a fault on beat 3, and Safety holds Turbo off from beat 4.
const fault = computed(() => props.step === 2 && props.beat >= 3)
const heldOff = computed(() => props.step === 2 && props.beat >= 4)
// Step 4: the in-game toggle flips Turbo on and off on beats 0, 4 and 8.
const toggledOn = computed(() => (props.step === 3 ? Math.floor(props.beat / 4) % 2 === 0 : true))
const withActive = computed(() => !heldOff.value && toggledOn.value)
</script>

<template>
  <WalkthroughShot v-if="step === 4" guide="turbo" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <!-- legend -->
      <rect x="150" y="16" width="14" height="12" rx="3" class="wt-wait" />
      <text x="170" y="27" class="wt-tiny" :textLength="fit(t('guides.diagram.wait'), 190, 11)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.wait') }}</text>
      <rect x="390" y="16" width="14" height="12" rx="3" class="wt-accent" />
      <text x="410" y="27" class="wt-tiny" :textLength="fit(t('guides.diagram.render'), 200, 11)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.render') }}</text>

      <!-- without Turbo -->
      <text x="20" y="82" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.turboOff'), 120, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.turboOff') }}</text>
      <g v-for="(f, k) in without" :key="`o${k}`" class="wt-fade" :class="{ 'wt-hidden': k >= shownWithout }">
        <rect :x="f.x" y="62" :width="f.wait" height="30" rx="5" class="wt-wait" />
        <rect :x="f.x + f.wait + 2" y="62" :width="f.work" height="30" rx="5" class="wt-accent" />
      </g>
      <text x="620" y="112" text-anchor="end" class="wt-tiny">{{ t('guides.walkthrough.labels.frames', { count: 4 }) }}</text>

      <!-- with Turbo -->
      <g class="wt-fade" :class="{ 'wt-hidden': step < 1 }">
        <text x="20" y="162" class="wt-small" :class="{ 'wt-strong': withActive }" :textLength="fit(t('guides.walkthrough.labels.turboOn'), 120, 13)" lengthAdjust="spacingAndGlyphs">{{ withActive ? t('guides.walkthrough.labels.turboOn') : t('guides.walkthrough.labels.turboOff') }}</text>
        <template v-if="withActive">
          <g v-for="(f, k) in withTurbo" :key="`w${k}`" class="wt-fade" :class="{ 'wt-hidden': k >= shownWith }">
            <rect :x="f.x" y="142" :width="f.wait" height="30" rx="5" class="wt-wait" />
            <rect :x="f.x + f.wait + 2" y="142" :width="f.work" height="30" rx="5" class="wt-accent" />
          </g>
          <text x="620" y="192" text-anchor="end" class="wt-tiny wt-strong">{{ t('guides.walkthrough.labels.frames', { count: 5 }) }}</text>
        </template>
        <template v-else>
          <g v-for="(f, k) in without" :key="`h${k}`">
            <rect :x="f.x" y="142" :width="f.wait" height="30" rx="5" class="wt-wait" />
            <rect :x="f.x + f.wait + 2" y="142" :width="f.work" height="30" rx="5" class="wt-accent" />
          </g>
          <text x="620" y="192" text-anchor="end" class="wt-tiny">{{ t('guides.walkthrough.labels.frames', { count: 4 }) }}</text>
        </template>
      </g>

      <!-- Turbo Safety (step 3) -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 2 }">
        <rect v-if="fault && !heldOff" x="338" y="138" width="80" height="38" rx="7" class="wt-danger" />
        <g transform="translate(150, 214)">
          <path d="M 12 0 L 24 5 V 14 C 24 22 18 27 12 30 C 6 27 0 22 0 14 V 5 Z" :class="heldOff ? 'wt-accent' : 'wt-soft'" />
          <text x="34" y="20" class="wt-small" :class="{ 'wt-strong': heldOff }" :textLength="fit(heldOff ? t('guides.walkthrough.labels.heldOff') : t('turbo.safety.title'), 420, 13)" lengthAdjust="spacingAndGlyphs">{{ heldOff ? t('guides.walkthrough.labels.heldOff') : t('turbo.safety.title') }}</text>
        </g>
      </g>

      <!-- the in-game toggle (step 4) -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 3 }">
        <rect x="150" y="212" width="190" height="34" rx="8" class="wt-box" :class="{ 'wt-accent': toggledOn }" />
        <text x="245" y="234" text-anchor="middle" class="wt-small wt-strong" :textLength="fit(t('turbo.tab.essentials.toggleHeading'), 176, 13)" lengthAdjust="spacingAndGlyphs">{{ t('turbo.tab.essentials.toggleHeading') }}</text>
      </g>
    </svg>
  </div>
</template>
