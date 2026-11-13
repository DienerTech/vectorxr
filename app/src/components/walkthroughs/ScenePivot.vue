<script setup lang="ts">
// Pivot: a small head turn turns the view further; a deadzone keeps small
// movements 1:1; Snap Views jump to fixed views.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// Seen from above: the seat at (220, 190), forward is up. Angles in degrees, + is right.
const C = { x: 220, y: 190 }
const deadzone = 12
const multiplier = 2.5
function amplified(head: number) {
  const sign = Math.sign(head)
  const a = Math.abs(head)
  return a <= deadzone ? head : sign * (deadzone + (a - deadzone) * multiplier)
}
const head = computed(() => {
  switch (props.step) {
    case 0:
      return lerp(0, 62, progress(props, 0, 1, 9)) // straining to look back
    case 1:
      return lerp(0, 50, progress(props, 1, 1, 7))
    case 2:
      return props.beat < 6 ? 10 * Math.sin(((props.beat - 0) / 3) * Math.PI) : lerp(0, 34, progress(props, 2, 6, 10))
    default:
      return 0
  }
})
const snaps = [-120, 75, 160]
const view = computed(() => {
  if (props.step === 0) return head.value
  if (props.step === 3) return props.beat < 1 ? 0 : snaps[Math.min(2, Math.floor((props.beat - 1) / 4))]
  return amplified(head.value)
})
const strain = computed(() => props.step === 0 && head.value > 45)
function cone(angle: number, r: number, spread: number) {
  const rad = (d: number) => ((d - 90) * Math.PI) / 180
  const a = rad(angle - spread)
  const b = rad(angle + spread)
  return `M ${C.x} ${C.y} L ${C.x + r * Math.cos(a)} ${C.y + r * Math.sin(a)} A ${r} ${r} 0 0 1 ${C.x + r * Math.cos(b)} ${C.y + r * Math.sin(b)} Z`
}
function ray(angle: number, r: number) {
  const a = ((angle - 90) * Math.PI) / 180
  return `M ${C.x} ${C.y} L ${C.x + r * Math.cos(a)} ${C.y + r * Math.sin(a)}`
}
</script>

<template>
  <WalkthroughShot v-if="step === 4" guide="pivot" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <!-- the deadzone (step 3) -->
      <path v-if="step === 2" :d="cone(0, 150, deadzone)" class="wt-soft" />
      <!-- where you look: the view -->
      <path :d="cone(view, 150, 22)" class="wt-accent wt-pivot-view" :class="{ 'wt-pivot-snap': step === 3 }" />
      <!-- where your head points -->
      <path :d="ray(head, 95)" :class="strain ? 'wt-pivot-strain' : 'wt-line'" stroke-width="3" />
      <!-- the seat and you -->
      <rect :x="C.x - 26" :y="C.y + 6" width="52" height="40" rx="10" class="wt-box" />
      <circle :cx="C.x" :cy="C.y" r="17" class="wt-box" />
      <circle :cx="C.x" :cy="C.y" r="5" class="wt-dot-accent" />

      <!-- labels on the right -->
      <g transform="translate(410, 60)">
        <text x="0" y="0" class="wt-small" :textLength="fit(t('guides.diagram.headTurn'), 210, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.headTurn') }}</text>
        <text x="0" y="28" class="wt-big">{{ Math.round(Math.abs(head)) }}°</text>
        <g v-if="step === 1 || step === 2">
          <text x="0" y="70" class="wt-small wt-strong" :textLength="fit(t('guides.diagram.viewTurns'), 210, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.viewTurns') }}</text>
          <text x="0" y="98" class="wt-big wt-pivot-accent">{{ Math.round(Math.abs(view)) }}°</text>
        </g>
        <text v-if="step === 0 && strain" x="0" y="70" class="wt-small wt-pivot-warn" :textLength="fit(t('guides.walkthrough.labels.neck'), 210, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.neck') }}</text>
        <text v-if="step === 2" x="0" y="140" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.deadzone'), 210, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.deadzone') }}</text>
        <g v-if="step === 3">
          <text x="0" y="70" class="wt-small wt-strong" :textLength="fit(t('pivot.behavior.snapViews'), 210, 13)" lengthAdjust="spacingAndGlyphs">{{ t('pivot.behavior.snapViews') }}</text>
          <g v-for="(s, k) in snaps" :key="s">
            <rect :x="k * 62" y="86" width="52" height="30" rx="8" :class="view === s ? 'wt-accent' : 'wt-box'" />
            <text :x="k * 62 + 26" y="106" text-anchor="middle" class="wt-small wt-strong">{{ k + 1 }}</text>
          </g>
        </g>
      </g>
    </svg>
  </div>
</template>

<style scoped>
.wt-pivot-view {
  opacity: 0.85;
}
.wt-pivot-snap {
  transition: d 0.35s ease;
}
.wt-pivot-strain {
  stroke: var(--app-danger);
  stroke-linecap: round;
}
.wt-pivot-warn {
  fill: var(--app-danger);
  font-weight: 600;
}
.wt-pivot-accent {
  fill: var(--app-accent);
}
</style>
