<script setup lang="ts">
// Stabilized mirror window: the headset software's mirror shows the headset's
// own image, shake and all; VectorXR's window smooths a copy of it.
import { computed } from 'vue'

import { t } from '../../i18n/index.ts'
import WalkthroughShot from './WalkthroughShot.vue'
import { fit, lerp, progress, reached, type SceneProps } from './kit'

const props = defineProps<SceneProps>()

// The head's shake, from the walkthrough's clock: a roll in degrees and a
// shift in pixels. The headset and the headset software's mirror both show it.
const shake = computed(() => {
  const s = props.time
  return {
    roll: 4.5 * Math.sin(7.1 * s) + 2.5 * Math.sin(11.3 * s + 1.3),
    x: 5 * Math.sin(5.3 * s + 0.4),
    y: 4 * Math.sin(8.7 * s + 2.1),
  }
})
// Step 2: VectorXR's window lands on beat 1, and its copy steadies on beats
// 3-7, zoomed in a little (the margin) so its edges stay filled.
const landed = computed(() => reached(props, 1, 1))
const steady = computed(() => progress(props, 1, 3, 7))
function picture(amount: number, zoom: number) {
  return `rotate(${shake.value.roll * amount}) translate(${shake.value.x * amount}, ${shake.value.y * amount}) scale(${zoom})` // i18n-ignore
}
const shared = computed(() => picture(1, 1.06))
const smoothed = computed(() => picture(1 - 0.92 * steady.value, lerp(1.06, 1.2, steady.value)))

// Step 3: the capture runs from the window to OBS on beats 1-3, the window is
// picked on beat 3, and it records from beat 6.
const capture = computed(() => progress(props, 2, 1, 3))
const picked = computed(() => reached(props, 2, 3))
const recording = computed(() => reached(props, 2, 6))

// Steps 4-6 are the Utilities page: the Default Profile switch, then the
// Stabilization and Window settings of the same picture.
const shot = computed(() => (props.step === 3 ? 'mirror' : 'mirror-settings'))
const box = computed(() => (props.step === 3 ? 'mirror' : props.step === 4 ? 'mirror-stabilization' : 'mirror-window'))
</script>

<template>
  <WalkthroughShot v-if="step >= 3" guide="mirror" :shot="shot" :box="box" :beat="beat" :caption="caption" />
  <div v-else class="wt-stage">
    <svg viewBox="0 0 640 270" role="img" :aria-label="caption">
      <defs>
        <clipPath id="wt-mirror-visor"><rect x="-90" y="-60" width="180" height="120" rx="34" /></clipPath>
        <clipPath id="wt-mirror-pane"><path d="M -90 -42 H 90 V 50 a 10 10 0 0 1 -10 10 H -80 a 10 10 0 0 1 -10 -10 Z" /></clipPath>
        <clipPath id="wt-mirror-preview"><rect x="-62" y="-35" width="124" height="70" rx="6" /></clipPath>
      </defs>

      <!-- steps 1 and 2: the headset and the headset software's mirror, one image -->
      <g class="wt-fade" :class="{ 'wt-hidden': step === 2 }">
        <g transform="translate(110, 100)">
          <g clip-path="url(#wt-mirror-visor)">
            <g :transform="shared">
              <rect x="-130" y="-100" width="260" height="200" class="wt-mirror-sky" />
              <path d="M -130 20 L -60 -22 L -10 8 L 50 -30 L 130 14 V 100 H -130 Z" class="wt-mirror-hill" />
            </g>
          </g>
          <rect x="-90" y="-60" width="180" height="120" rx="34" fill="none" class="wt-line" />
        </g>
        <text x="110" y="182" text-anchor="middle" class="wt-small" :textLength="fit(t('guides.diagram.headset'), 180, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.headset') }}</text>

        <g transform="translate(320, 100)">
          <rect x="-90" y="-60" width="180" height="120" rx="10" class="wt-box" />
          <g clip-path="url(#wt-mirror-pane)">
            <g :transform="shared">
              <rect x="-130" y="-100" width="260" height="200" class="wt-mirror-sky" />
              <path d="M -130 20 L -60 -22 L -10 8 L 50 -30 L 130 14 V 100 H -130 Z" class="wt-mirror-hill" />
            </g>
          </g>
          <rect x="-90" y="-60" width="180" height="18" rx="9" class="wt-titlebar" />
          <rect x="-90" y="-60" width="180" height="120" rx="10" fill="none" class="wt-line" />
        </g>
        <text x="320" y="182" text-anchor="middle" class="wt-small" :textLength="fit(t('guides.diagram.runtimeMirror'), 196, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.runtimeMirror') }}</text>

        <path d="M 110 194 v 8 H 320 v -8" class="wt-line" />
        <text x="215" y="224" text-anchor="middle" class="wt-label wt-strong" :textLength="fit(t('guides.diagram.sameImage'), 300, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.sameImage') }}</text>
        <path v-if="landed" d="M 414 100 H 436" class="wt-line-accent wt-dash" />
      </g>

      <!-- steps 2 and 3: VectorXR's own window, with its smoothed copy -->
      <g class="wt-drop" :class="{ 'wt-hidden-up': !landed }">
        <g class="wt-move" :style="{ transform: step === 2 ? 'translate(-420px, 0)' : 'none' }">
          <g transform="translate(530, 100)">
            <rect x="-90" y="-60" width="180" height="120" rx="10" class="wt-box" />
            <g clip-path="url(#wt-mirror-pane)">
              <g :transform="smoothed">
                <rect x="-130" y="-100" width="260" height="200" class="wt-mirror-sky" />
                <path d="M -130 20 L -60 -22 L -10 8 L 50 -30 L 130 14 V 100 H -130 Z" class="wt-mirror-hill" />
              </g>
            </g>
            <rect x="-90" y="-60" width="180" height="18" rx="9" class="wt-titlebar" />
            <!-- i18n-ignore -->
            <text x="-80" y="-47" class="wt-tiny">VectorXR Mirror – DCS</text>
            <rect x="-90" y="-60" width="180" height="120" rx="10" class="wt-accent" fill-opacity="0" />
          </g>
          <text x="530" y="182" text-anchor="middle" class="wt-small" :textLength="fit(t('guides.diagram.mirrorWindow'), 196, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.mirrorWindow') }}</text>
          <text x="530" y="224" text-anchor="middle" class="wt-label wt-good wt-fade" :class="{ 'wt-hidden': steady < 0.5 }" :textLength="fit(t('guides.diagram.smoothedCopy'), 196, 14)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.smoothedCopy') }}</text>
        </g>
      </g>

      <!-- step 3: OBS captures that window -->
      <g class="wt-fade" :class="{ 'wt-hidden': step !== 2 }">
        <path :d="`M 206 100 H ${lerp(206, 292, capture)}`" class="wt-line-accent" :opacity="capture > 0 ? 1 : 0" />
        <rect x="300" y="24" width="320" height="200" rx="12" class="wt-box" />
        <rect x="300" y="24" width="320" height="22" rx="11" class="wt-titlebar" />
        <!-- i18n-ignore -->
        <text x="314" y="39" class="wt-tiny">OBS</text>
        <text x="316" y="68" class="wt-small" :textLength="fit(t('guides.walkthrough.labels.windowCapture'), 288, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.walkthrough.labels.windowCapture') }}</text>
        <rect x="316" y="78" width="288" height="30" rx="8" :class="picked ? 'wt-accent' : 'wt-soft'" />
        <!-- i18n-ignore -->
        <text x="328" y="98" class="wt-small wt-fade" :class="{ 'wt-hidden': !picked, 'wt-strong': picked }">VectorXR Mirror – DCS</text>
        <path d="M 584 90 l 6 6 l 6 -6" class="wt-line" />
        <g transform="translate(378, 166)" class="wt-fade" :class="{ 'wt-hidden': !picked }">
          <g clip-path="url(#wt-mirror-preview)">
            <g :transform="picture(0.08, 0.82)">
              <rect x="-130" y="-100" width="260" height="200" class="wt-mirror-sky" />
              <path d="M -130 20 L -60 -22 L -10 8 L 50 -30 L 130 14 V 100 H -130 Z" class="wt-mirror-hill" />
            </g>
          </g>
          <rect x="-62" y="-35" width="124" height="70" rx="6" fill="none" class="wt-line" />
        </g>
        <g class="wt-fade" :class="{ 'wt-hidden': !recording }">
          <circle cx="474" cy="166" r="7" class="wt-mirror-rec" />
          <!-- i18n-ignore -->
          <text x="488" y="171" class="wt-small wt-strong">REC</text>
        </g>
      </g>
    </svg>
  </div>
</template>

<style scoped>
.wt-mirror-sky {
  fill: #7fa7c9;
}
.wt-mirror-hill {
  fill: #4f7a52;
}
.wt-mirror-rec {
  fill: var(--app-danger);
}
</style>
