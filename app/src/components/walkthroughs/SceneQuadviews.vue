<script setup lang="ts">
// Quadviews: a sharp inset where you look, lower detail around it.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, reached, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// The view: x 60..420, y 20..250. The gaze wanders in step 1, jumps between three
// places on beats 0, 4 and 8 of step 3, and rests in the middle otherwise.
const spots = [
  { x: 240, y: 130 },
  { x: 150, y: 90 },
  { x: 330, y: 180 },
  { x: 250, y: 120 },
]
const gaze = computed(() => {
  if (props.step === 0) {
    return { x: 240 + 90 * Math.sin(props.time * 1.3), y: 130 + 45 * Math.sin(props.time * 2.1) }
  }
  if (props.step === 2) return spots[1 + Math.min(2, Math.floor(props.beat / 4))]
  return spots[0]
})
// The inset trails the gaze a little (CSS transition) and appears on beat 2 of step 2.
const insetOn = computed(() => reached(props, 1, 2))
const insetW = 130
const insetH = 92
// Pixels drawn: from 100 % to about 45 % on beats 2 to 6 of step 4.
const pixels = computed(() => lerp(100, 45, progress(props, 3, 2, 6)))
</script>

<template>
  <WalkthroughShot v-if="step === 4" guide="quadviews" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <defs>
        <pattern id="wt-qv-coarse" width="12" height="12" patternUnits="userSpaceOnUse">
          <rect width="12" height="12" class="wt-soft" />
          <rect width="6" height="6" fill="currentColor" opacity="0.06" />
          <rect x="6" y="6" width="6" height="6" fill="currentColor" opacity="0.06" />
        </pattern>
      </defs>
      <!-- the headset view -->
      <rect x="60" y="20" width="360" height="230" rx="16" class="wt-box" />
      <rect v-if="insetOn" x="60" y="20" width="360" height="230" rx="16" fill="url(#wt-qv-coarse)" class="wt-qv-coarse" />
      <g class="wt-move" :style="{ transform: `translate(${gaze.x - insetW / 2}px, ${gaze.y - insetH / 2}px)` }">
        <rect v-if="insetOn" :width="insetW" :height="insetH" rx="10" class="wt-box" />
        <rect v-if="insetOn" :width="insetW" :height="insetH" rx="10" class="wt-accent" />
      </g>
      <g class="wt-move" :style="{ transform: `translate(${gaze.x}px, ${gaze.y}px)` }">
        <circle r="7" class="wt-dot-accent" />
        <circle r="14" class="wt-line-accent" opacity="0.5" />
      </g>
      <!-- labels on the right -->
      <text x="440" y="60" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.whereYouLook'), 190, 13)" lengthAdjust="spacingAndGlyphs">● {{ t('guides.walkthrough.labels.whereYouLook') }}</text>
      <g class="wt-fade" :class="{ 'wt-hidden': !insetOn }">
        <rect x="440" y="80" width="16" height="12" rx="3" class="wt-accent" />
        <text x="464" y="91" class="wt-small" :textLength="fit(t('guides.diagram.focus'), 166, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.focus') }}</text>
        <rect x="440" y="104" width="16" height="12" rx="3" class="wt-soft" />
        <text x="464" y="115" class="wt-small" :textLength="fit(t('guides.diagram.periphery'), 166, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.periphery') }}</text>
      </g>
      <!-- pixels drawn (step 4) -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 3 }">
        <text x="440" y="170" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.pixelsDrawn'), 190, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.pixelsDrawn') }}</text>
        <rect x="440" y="182" width="180" height="20" rx="6" class="wt-soft" />
        <rect x="440" y="182" :width="1.8 * pixels" height="20" rx="6" class="wt-accent" />
        <text x="440" y="236" class="wt-big">{{ Math.round(pixels) }}%</text>
      </g>
    </svg>
  </div>
</template>

<style scoped>
.wt-qv-coarse {
  color: var(--app-text);
}
</style>
