<script setup lang="ts">
// Screenshots: the game's full-resolution image for each eye, saved as PNG.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// Step 3: the binding is pressed on beat 2; the two files fly to the folder on
// beats 3-6 (left) and 4-7 (right).
const pressed = computed(() => props.step === 2 && props.beat >= 2 && props.beat < 3)
const flyL = computed(() => progress(props, 2, 3, 6))
const flyR = computed(() => progress(props, 2, 4, 7))
const saved = computed(() => props.step === 2 && props.beat >= 7)
</script>

<template>
  <WalkthroughShot v-if="step === 3" guide="screenshots" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <defs>
        <filter id="wt-shot-blur"><feGaussianBlur stdDeviation="2.4" /></filter>
        <clipPath id="wt-shot-lens"><ellipse cx="0" cy="0" rx="58" ry="48" /></clipPath>
      </defs>

      <!-- step 1: the desktop mirror, small and lens-shaped -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 0 }">
        <rect x="150" y="40" width="340" height="190" rx="12" class="wt-box" />
        <rect x="150" y="40" width="340" height="22" rx="12" class="wt-titlebar" />
        <text x="164" y="55" class="wt-tiny" :textLength="fit(t('guides.walkthrough.labels.mirror'), 300, 11)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.mirror') }}</text>
        <g v-for="cx in [255, 385]" :key="cx" :transform="`translate(${cx}, 145)`">
          <g clip-path="url(#wt-shot-lens)" filter="url(#wt-shot-blur)">
            <rect x="-60" y="-50" width="120" height="100" class="wt-shot-sky" />
            <path d="M -60 30 L -20 -10 L 10 15 L 60 -20 V 50 H -60 Z" class="wt-shot-hill" />
          </g>
        </g>
      </g>

      <!-- steps 2 and 3: the full-resolution eye images -->
      <g v-if="step >= 1">
        <g v-for="(x, k) in [40, 245]" :key="x">
          <rect :x="x" y="30" width="190" height="160" rx="10" class="wt-shot-sky" />
          <path :d="`M ${x} 150 L ${x + 60} 90 L ${x + 100} 125 L ${x + 190} 70 V 190 H ${x} Z`" class="wt-shot-hill" />
          <rect :x="x" y="30" width="190" height="160" rx="10" fill="none" class="wt-line" />
          <text :x="x + 95" y="212" text-anchor="middle" class="wt-tiny">{{ k === 0 ? t('guides.diagram.leftEye') : t('guides.diagram.rightEye') }}</text>
        </g>
        <text v-if="step === 1" x="460" y="100" class="wt-label wt-strong" :textLength="fit(t('guides.walkthrough.labels.fullRes'), 170, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.fullRes') }}</text>
      </g>

      <!-- step 3: the binding, the files and the folder -->
      <g v-if="step === 2">
        <rect x="40" y="226" width="190" height="34" rx="8" class="wt-button" :class="{ 'wt-pressed': pressed }" />
        <text x="135" y="248" text-anchor="middle" class="wt-small wt-on-accent" :textLength="fit(t('utilities.screenshots.binding.label'), 176, 13)" lengthAdjust="spacingAndGlyphs">{{ t('utilities.screenshots.binding.label') }}</text>
        <g v-for="(f, k) in [flyL, flyR]" :key="k" :transform="`translate(${lerp(k === 0 ? 120 : 330, 540, f)}, ${lerp(110, 150, f)}) scale(${lerp(1, 0.6, f)})`" :opacity="f > 0 && f < 1 ? 1 : 0">
          <path d="M -16 -20 h 22 l 10 10 v 30 h -32 Z" class="wt-file" />
          <text x="0" y="12" text-anchor="middle" class="wt-tiny wt-strong">{{ k === 0 ? 'L' : 'R' }}</text>
        </g>
        <path d="M 490 130 h 34 l 8 8 h 70 v 60 h -112 Z" class="wt-box" :class="{ 'wt-accent': saved }" />
        <text x="546" y="222" text-anchor="middle" class="wt-tiny">{{ t('guides.walkthrough.labels.folder') }}</text>
        <text v-if="saved" x="546" y="174" text-anchor="middle" class="wt-small wt-strong">{{ t('guides.diagram.pngFiles') }}</text>
      </g>
    </svg>
  </div>
</template>

<style scoped>
.wt-shot-sky {
  fill: #7fa7c9;
}
.wt-shot-hill {
  fill: #4f7a52;
}
</style>
