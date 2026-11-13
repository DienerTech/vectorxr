<script setup lang="ts">
// Frame Generation: generated frames fill the headset refreshes between game frames.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, reached, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// Eight refresh slots (x 130..620). Game frames land on the even slots; the odd
// ones are empty until Frame Generation fills them (step 3, one every 2 beats).
const slots = Array.from({ length: 8 }, (_, k) => ({ x: 140 + k * 60, game: k % 2 === 0 }))
const filled = computed(() => (props.step < 2 ? 0 : props.step === 2 ? Math.min(4, Math.floor(props.beat / 2) ) : 4))
// The ball inside each frame moves left to right across the slots; its position
// in a slot is where the picture is at that refresh.
const ballX = (k: number) => 14 + k * 3.4
// Step 1: the refresh ticks pulse, one per beat.
const tickOn = (k: number) => props.step === 0 && Math.floor(props.beat) % 8 === k
// Step 2: the motion arrow between two game frames draws on beats 2 to 6.
const arrow = computed(() => progress(props, 1, 2, 6))
// Step 4: Interpolate and Extrapolate, highlighted in turn (6 beats each).
const showModes = computed(() => props.step === 3)
const extrapolateLit = computed(() => props.step === 3 && props.beat >= 6)
</script>

<template>
  <WalkthroughShot v-if="step === 4" guide="framegen" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <text x="20" y="38" class="wt-small" :textLength="fit(t('guides.diagram.refresh'), 108, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.refresh') }}</text>
      <g v-for="(s, k) in slots" :key="`t${k}`">
        <path :d="`M ${s.x + 24} 26 v 14`" :class="tickOn(k) ? 'wt-line-accent' : 'wt-line'" />
      </g>
      <text x="20" y="96" class="wt-small" :textLength="fit(t('guides.diagram.realFrame'), 108, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.realFrame') }}</text>
      <g v-for="(s, k) in slots" :key="`s${k}`">
        <template v-if="s.game">
          <rect :x="s.x" y="58" width="48" height="56" rx="8" class="wt-box" />
          <circle :cx="s.x + ballX(k)" cy="86" r="7" class="wt-frame" />
        </template>
        <template v-else>
          <rect :x="s.x" y="58" width="48" height="56" rx="8" class="wt-soft wt-dash" fill="none" />
          <g class="wt-fade" :class="{ 'wt-hidden': (k - 1) / 2 >= filled }">
            <rect :x="s.x" y="58" width="48" height="56" rx="8" class="wt-accent wt-dash" />
            <circle :cx="s.x + ballX(k)" cy="86" r="7" class="wt-dot-accent" />
          </g>
        </template>
      </g>
      <!-- the motion between two game frames (step 2) -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 1 }">
        <path :d="`M ${140 + ballX(0)} 132 H ${lerp(140 + ballX(0), 260 + ballX(2), arrow)}`" class="wt-line-accent" />
        <path v-if="arrow > 0.95" :d="`M ${260 + ballX(2) - 8} 126 L ${260 + ballX(2)} 132 L ${260 + ballX(2) - 8} 138`" class="wt-line-accent" />
        <text x="140" y="156" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.motion'), 300, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.motion') }}</text>
      </g>
      <g class="wt-fade" :class="{ 'wt-hidden': !reached(props, 2, 2) || step > 2 }">
        <rect x="140" y="132" width="14" height="14" rx="3" class="wt-accent wt-dash" />
        <text x="160" y="144" class="wt-small" :textLength="fit(t('guides.diagram.generatedFrame'), 300, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.generatedFrame') }}</text>
      </g>
      <!-- the two modes (step 4) -->
      <g class="wt-fade" :class="{ 'wt-hidden': !showModes }">
        <rect x="20" y="168" width="290" height="88" rx="12" :class="extrapolateLit ? 'wt-box' : 'wt-accent'" />
        <text x="36" y="196" class="wt-label wt-strong">{{ t('framegen.settings.mode.interpolate.label') }}</text>
        <text x="36" y="222" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.interpolate'), 260, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.interpolate') }}</text>
        <rect x="330" y="168" width="290" height="88" rx="12" :class="extrapolateLit ? 'wt-accent' : 'wt-box'" />
        <text x="346" y="196" class="wt-label wt-strong">{{ t('framegen.settings.mode.extrapolate.label') }}</text>
        <text x="346" y="222" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.extrapolate'), 260, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.extrapolate') }}</text>
      </g>
      <!-- 45 -> 90 (step 3 onwards) -->
      <!-- i18n-ignore: frame rates -->
      <text v-if="step === 2 && filled === 4" x="620" y="160" text-anchor="end" class="wt-big">45 → 90 fps</text>
    </svg>
  </div>
</template>
