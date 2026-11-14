<script setup lang="ts">
// A walkthrough step that shows the real page: a screenshot of the guide's
// page in the app's language, framed on the control to use, which is ringed
// and slowly zoomed toward. The screenshots and the control boxes come from
// generated-art/vxr-guides/screens/capture.mjs.
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

import manifest from '../../assets/guides/screens/manifest.json'
import { activeLocale, t } from '../../i18n/index.ts'
import type { GuideId } from '../../lib/guides'

const props = defineProps<{ guide: GuideId; beat: number; caption: string }>()

const images = import.meta.glob<string>('../../assets/guides/screens/*/*.jpg', { eager: true, query: '?url', import: 'default' })
const boxes = manifest.boxes as Record<string, Record<string, number[]>>

// The page in the app's language, or in English for a language without one.
const lang = computed(() => (boxes[activeLocale.value]?.[props.guide] ? activeLocale.value : 'en'))
const src = computed(() => images[`../../assets/guides/screens/${lang.value}/${props.guide}.jpg`])
const box = computed(() => boxes[lang.value]?.[props.guide] ?? [0.4, 0.4, 0.2, 0.1])

// The screenshot is as wide as the frame and taller, so it is moved to keep the
// control in view, then zoomed toward it. The frame's size is measured, since
// the player may cap its height.
const frame = ref<HTMLElement | null>(null)
const frameSize = ref({ w: 640, h: 270 })
let observer: ResizeObserver | null = null
onMounted(() => {
  if (!frame.value) return
  const measure = () => {
    if (frame.value) frameSize.value = { w: frame.value.clientWidth, h: frame.value.clientHeight }
  }
  measure()
  observer = new ResizeObserver(measure)
  observer.observe(frame.value)
})
onBeforeUnmount(() => observer?.disconnect())
const imageAspect = manifest.aspect
// Zoom in a little toward a small control; not at all toward a wide one.
const zoom = computed(() => 1 + 0.12 * Math.max(0, 1 - box.value[2] * 2) * Math.min(1, Math.max(0, props.beat - 2) / 8))
const centre = computed(() => ({ x: box.value[0] + box.value[2] / 2, y: box.value[1] + box.value[3] / 2 }))
// Pixels to move the image up so the control's centre sits mid-frame, kept within the image.
const shift = computed(() => {
  const imageHeight = frameSize.value.w * imageAspect
  return Math.max(0, Math.min(imageHeight - frameSize.value.h, centre.value.y * imageHeight - frameSize.value.h / 2))
})
const imageStyle = computed(() => ({
  transform: `translateY(${-shift.value}px) scale(${zoom.value})`, // i18n-ignore
  transformOrigin: `${Math.min(0.75, Math.max(0.25, centre.value.x)) * 100}% ${centre.value.y * 100}%`,
}))
const ringStyle = computed(() => ({
  left: `${box.value[0] * 100}%`,
  top: `${box.value[1] * 100}%`,
  width: `${box.value[2] * 100}%`,
  height: `${box.value[3] * 100}%`,
}))
const ringOn = computed(() => props.beat >= 1)
</script>

<template>
  <div class="wt-stage wt-shot" role="img" :aria-label="caption">
    <div ref="frame" class="wt-shot-frame">
      <div class="wt-shot-image" :style="imageStyle">
        <img :src="src" alt="" draggable="false" />
        <div class="wt-shot-ring" :class="{ 'wt-shot-ring-on': ringOn }" :style="ringStyle" />
      </div>
    </div>
    <span class="wt-shot-badge">{{ t('guides.walkthrough.labels.inTheApp') }}</span>
  </div>
</template>

<style scoped>
.wt-shot {
  position: relative;
}
.wt-shot-frame {
  position: absolute;
  inset: 0;
  overflow: hidden;
  border-radius: 0.75rem;
  border: 1px solid var(--app-border-strong);
}
.wt-shot-image {
  position: relative;
  width: 100%;
  transition: transform 0.6s ease-out;
}
.wt-shot-image img {
  display: block;
  width: 100%;
  height: auto;
  user-select: none;
}
.wt-shot-ring {
  position: absolute;
  border-radius: 0.6rem;
  border: 3px solid var(--app-accent);
  box-shadow: 0 0 0 9999px rgba(0, 0, 0, 0);
  opacity: 0;
  transition: opacity 0.4s ease, box-shadow 0.6s ease;
}
.wt-shot-ring-on {
  opacity: 1;
  box-shadow: 0 0 0 9999px rgba(0, 0, 0, 0.35);
  animation: wt-ring-pulse 1.6s ease-in-out infinite;
}
.wt-shot-badge {
  position: absolute;
  left: 0.6rem;
  top: 0.6rem;
  border-radius: 999px;
  padding: 0.15rem 0.6rem;
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: #fff;
  background: var(--app-accent);
}
@keyframes wt-ring-pulse {
  0%,
  100% {
    border-color: var(--app-accent);
  }
  50% {
    border-color: var(--app-accent-strong);
  }
}
@media (prefers-reduced-motion: reduce) {
  .wt-shot-image,
  .wt-shot-ring {
    transition: none;
  }
  .wt-shot-ring-on {
    animation: none;
  }
}
</style>
