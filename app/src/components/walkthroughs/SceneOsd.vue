<script setup lang="ts">
// On-Screen Display: a panel in the headset view that follows your head.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// The view sways a little (your head moving); the panel moves with it.
const sway = computed(() => (props.step === 0 ? 10 * Math.sin(props.time * 1.4) : 0))
// Step 3: the panel is dragged from the top left to the right on beats 2 to 6.
const drag = computed(() => progress(props, 2, 2, 6))
const panel = computed(() => ({ x: lerp(90, 330, props.step >= 2 ? drag.value : 0) + sway.value, y: 50 }))
const rows = computed(() => [
  { k: 'fps', text: '90 fps · 11.1 ms' },
  { k: 'turbo', text: `${t('app.tabs.turbo.label')}: Async` }, // i18n-ignore: the pacing mode's name, as the OSD shows it
  { k: 'fg', text: `${t('app.tabs.framegen.label')}: 45 → 90` },
  { k: 'pivot', text: `${t('app.tabs.pivotxr.label')}: ${t('common.active')}` },
])
// Rows appear one per beat pair in step 2.
const rowsShown = computed(() => (props.step === 0 ? 1 : props.step === 1 ? Math.min(4, 1 + Math.floor(props.beat / 2)) : 4))
// Step 4: Ctrl+Alt+F10 hides the panel on beat 2 and shows it on beat 6; F11 switches to compact on beat 9.
const hidden = computed(() => props.step === 3 && props.beat >= 2 && props.beat < 6)
const compact = computed(() => props.step === 3 && props.beat >= 9)
const keyHot = (from: number, to: number) => props.step === 3 && props.beat >= from && props.beat < to
const cursorOn = computed(() => props.step === 2 && props.beat >= 1 && props.beat < 7)
</script>

<template>
  <WalkthroughShot v-if="step === 4" guide="osd" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <!-- the headset view -->
      <g :transform="`translate(${sway}, 0)`">
        <rect x="60" y="20" width="520" height="230" rx="18" class="wt-box" />
        <path d="M 60 190 L 220 120 L 330 170 L 450 100 L 580 180 V 232 Q 580 250 562 250 H 78 Q 60 250 60 232 Z" class="wt-soft" />
      </g>
      <!-- the panel -->
      <g class="wt-fade" :class="{ 'wt-hidden': hidden }" :transform="`translate(${panel.x}, ${panel.y})`">
        <rect x="0" y="0" width="210" :height="compact ? 34 : 20 + 26 * rowsShown" rx="10" class="wt-osd-panel" />
        <template v-if="!compact">
          <g v-for="(row, k) in rows.slice(0, rowsShown)" :key="row.k">
            <text x="14" :y="28 + 26 * k" :class="k === 0 ? 'wt-osd-big' : 'wt-osd-row'" :textLength="fit(row.text, 184, k === 0 ? 15 : 12)" lengthAdjust="spacingAndGlyphs">{{ row.text }}</text>
          </g>
        </template>
        <text v-else x="14" y="23" class="wt-osd-big">90 fps</text>
      </g>
      <!-- the mouse dragging it (step 3) -->
      <g v-if="cursorOn" :transform="`translate(${panel.x + 180}, ${panel.y + 18})`">
        <path d="M 0 0 L 0 18 L 5 13 L 9 22 L 12 21 L 8 12 L 15 12 Z" class="wt-osd-cursor" />
      </g>
      <!-- the keys (step 4) -->
      <g v-if="step === 3" transform="translate(150, 205)">
        <rect x="0" y="0" width="150" height="34" rx="8" :class="keyHot(2, 3) || keyHot(6, 7) ? 'wt-accent' : 'wt-box'" />
        <!-- i18n-ignore: key names -->
        <text x="75" y="22" text-anchor="middle" class="wt-small wt-strong">Ctrl+Alt+F10</text>
        <rect x="190" y="0" width="150" height="34" rx="8" :class="keyHot(9, 10) ? 'wt-accent' : 'wt-box'" />
        <!-- i18n-ignore: key names -->
        <text x="265" y="22" text-anchor="middle" class="wt-small wt-strong">Ctrl+Alt+F11</text>
      </g>
    </svg>
  </div>
</template>

<style scoped>
.wt-osd-panel {
  fill: rgba(10, 14, 12, 0.82);
  stroke: var(--app-accent);
  stroke-width: 1.5;
}
.wt-osd-big {
  fill: #fff;
  font-size: 15px;
  font-weight: 700;
}
.wt-osd-row {
  fill: #d6e2dc;
  font-size: 12px;
}
.wt-osd-cursor {
  fill: #fff;
  stroke: #000;
  stroke-width: 1;
}
</style>
