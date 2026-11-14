<script setup lang="ts">
// Depth: eye separation changes the world's scale; convergence sets the plane
// where the two images line up.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// Seen from above: the eyes at y 230 around x 220, the world above them.
const half = computed(() => {
  if (props.step !== 1) return 32
  // wider on beats 1-4, narrower on beats 6-9, back by beat 11
  const wide = progress(props, 1, 1, 4) - progress(props, 1, 5, 7)
  const narrow = progress(props, 1, 6, 9) - progress(props, 1, 10, 11.5)
  return 32 + 22 * wide - 16 * narrow
})
const compact = computed(() => props.step === 1 && half.value > 40)
const bigger = computed(() => props.step === 1 && half.value < 26)
// The plane where the images meet: moves nearer on beats 2-5, then farther on 6-10.
const planeY = computed(() => (props.step === 2 ? lerp(lerp(70, 130, progress(props, 2, 2, 5)), 45, progress(props, 2, 6, 10)) : 70))
// The object: drawn smaller when the eyes are wider (the world feels more compact).
const objectR = computed(() => 30 * (32 / half.value))
</script>

<template>
  <WalkthroughShot v-if="step === 3" guide="depth" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <!-- the object you look at -->
      <circle cx="220" :cy="planeY" :r="Math.max(12, Math.min(48, objectR))" class="wt-accent" />
      <!-- the plane where the images line up (step 3) -->
      <g v-if="step === 2">
        <path :d="`M 60 ${planeY} H 380`" class="wt-line-accent wt-dash" />
        <text x="390" :y="planeY + 5" class="wt-small wt-strong" :textLength="fit(t('guides.diagram.depthPlane'), 230, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.depthPlane') }}</text>
      </g>
      <!-- the sight lines -->
      <path :d="`M ${220 - half} 226 L 220 ${planeY} M ${220 + half} 226 L 220 ${planeY}`" class="wt-line" />
      <!-- the eyes -->
      <g>
        <circle :cx="220 - half" cy="232" r="11" class="wt-box" />
        <circle :cx="220 - half" cy="228" r="4" class="wt-dot-accent" />
        <circle :cx="220 + half" cy="232" r="11" class="wt-box" />
        <circle :cx="220 + half" cy="228" r="4" class="wt-dot-accent" />
      </g>
      <text :x="220 - half - 18" y="262" text-anchor="end" class="wt-tiny">{{ t('guides.diagram.leftEye') }}</text>
      <text :x="220 + half + 18" y="262" class="wt-tiny">{{ t('guides.diagram.rightEye') }}</text>

      <!-- labels on the right -->
      <g v-if="step === 1" transform="translate(400, 120)">
        <text x="0" y="0" class="wt-small" :class="{ 'wt-strong': bigger }" :textLength="fit(t('guides.walkthrough.labels.biggerWorld'), 220, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.biggerWorld') }}</text>
        <text x="0" y="28" class="wt-small" :class="{ 'wt-strong': compact }" :textLength="fit(t('guides.walkthrough.labels.compactWorld'), 220, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.compactWorld') }}</text>
        <rect :x="-14" :y="bigger ? -12 : 16" width="6" height="16" rx="2" class="wt-dot-accent" :opacity="bigger || compact ? 1 : 0" />
      </g>
    </svg>
  </div>
</template>
