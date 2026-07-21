<script setup lang="ts">
import { computed, ref } from 'vue'

import { fromConvergenceDisplay, fromStereoBoostDisplay, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'

const props = defineProps<{
  stereoBoost: number
  convergence: number
  depthLock: boolean
  convergenceLimit?: number
  muted?: boolean
}>()

const emit = defineEmits<{
  'update:stereoBoost': [value: number]
  'update:convergence': [value: number]
}>()

type AxisSign = -1 | 0 | 1

function sign(value: number, neutralThreshold: number): AxisSign {
  if (value > neutralThreshold) return 1
  if (value < -neutralThreshold) return -1
  return 0
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, value))
}

const depthValue = computed(() => toStereoBoostDisplay(props.stereoBoost))
const convergenceValue = computed(() => toConvergenceDisplay(props.convergence))
const mapConvergenceLimit = computed(() => props.convergenceLimit ?? 5)
const depthSign = computed(() => sign(depthValue.value, 0.05))
const convergenceSign = computed(() => sign(convergenceValue.value, 0.05))
const dragging = ref(false)

const markerStyle = computed(() => ({
  left: `${50 + clamp(depthValue.value / 25, -1, 1) * 43}%`,
  top: `${50 - clamp(convergenceValue.value / mapConvergenceLimit.value, -1, 1) * 43}%`,
}))

function setFromPointer(event: PointerEvent) {
  if (props.muted) return
  const element = event.currentTarget as HTMLButtonElement
  const bounds = element.getBoundingClientRect()
  const x = clamp(((event.clientX - bounds.left) / bounds.width - 0.5) / 0.43, -1, 1)
  const y = clamp((0.5 - (event.clientY - bounds.top) / bounds.height) / 0.43, -1, 1)
  emit('update:stereoBoost', fromStereoBoostDisplay(x * 25))
  emit('update:convergence', fromConvergenceDisplay(y * mapConvergenceLimit.value))
}

function beginDrag(event: PointerEvent) {
  if (props.muted) return
  dragging.value = true
  ;(event.currentTarget as HTMLButtonElement).setPointerCapture(event.pointerId)
  setFromPointer(event)
}

function continueDrag(event: PointerEvent) {
  if (dragging.value) setFromPointer(event)
}

function endDrag(event: PointerEvent) {
  dragging.value = false
  const element = event.currentTarget as HTMLButtonElement
  if (element.hasPointerCapture(event.pointerId)) element.releasePointerCapture(event.pointerId)
}

function adjustFromKeyboard(event: KeyboardEvent) {
  if (props.muted) return
  const depthStep = event.shiftKey ? 5 : 1
  const convergenceStep = event.shiftKey ? 0.5 : 0.1
  let depth = depthValue.value
  let convergence = convergenceValue.value
  if (event.key === 'ArrowLeft') depth -= depthStep
  else if (event.key === 'ArrowRight') depth += depthStep
  else if (event.key === 'ArrowDown') convergence -= convergenceStep
  else if (event.key === 'ArrowUp') convergence += convergenceStep
  else return
  event.preventDefault()
  emit('update:stereoBoost', fromStereoBoostDisplay(clamp(depth, -25, 25)))
  emit('update:convergence', fromConvergenceDisplay(clamp(convergence, -mapConvergenceLimit.value, mapConvergenceLimit.value)))
}

const activeQuadrant = computed(() => {
  if (depthSign.value === 0 || convergenceSign.value === 0) return ''
  return `${depthSign.value > 0 ? 'positive' : 'negative'}-${convergenceSign.value > 0 ? 'positive' : 'negative'}`
})

const netEffect = computed(() => {
  const depth = depthSign.value
  const convergence = convergenceSign.value

  if (depth === 0 && convergence === 0) {
    return {
      title: 'Native stereo geometry',
      description: "Neither control is shifting the application's stereo scale or depth plane.",
      caution: 'Use this as the comfort and comparison baseline.',
    }
  }
  if (depth > 0 && convergence > 0) {
    return {
      title: 'Compounded near-field intensity',
      description: 'Stronger stereo separation and a nearer, more crossed depth plane both emphasize close geometry.',
      caution: 'Highest fusion and eye-strain risk. Increase only in very small steps.',
    }
  }
  if (depth > 0 && convergence < 0) {
    return {
      title: 'Strong depth with a farther plane',
      description: 'Stereo separation strengthens relative depth while negative convergence gives the scene more distance and breathing room.',
      caution: 'Often a useful pairing: move Convergence only far enough to settle the stronger stereo image.',
    }
  }
  if (depth < 0 && convergence > 0) {
    return {
      title: 'Nearer but flatter',
      description: 'Reduced stereo separation makes the world larger and flatter while positive convergence pulls the overall depth field nearer.',
      caution: 'Best treated as a corrective pairing; watch for crossed-eye discomfort.',
    }
  }
  if (depth < 0 && convergence < 0) {
    return {
      title: 'Larger, calmer, more distant',
      description: 'Reduced stereo separation and a farther depth plane both deemphasize close-range presence.',
      caution: 'May improve comfort, but can remove much of the intended 3D effect.',
    }
  }
  if (depth > 0) {
    return {
      title: 'Stronger relative stereo depth',
      description: 'Nearby geometry separates more strongly than distant geometry, increasing presence while making the world feel smaller.',
      caution: 'Set this first, then use small negative convergence adjustments if the scene feels too close.',
    }
  }
  if (depth < 0) {
    return {
      title: 'Larger, flatter world',
      description: 'Reduced stereo separation softens near-field depth and increases apparent world scale.',
      caution: 'Useful when native geometry feels miniaturized or overly intense.',
    }
  }
  if (convergence > 0) {
    return {
      title: 'Depth plane moved nearer',
      description: "The scene's disparity field shifts in the crossed direction without changing its relative depth gradient.",
      caution: 'Positive values can feel cross-eyed, especially with positive Stereo Depth.',
    }
  }
  return {
    title: 'Depth plane moved farther',
    description: "The scene's disparity field shifts farther away without changing its relative depth gradient.",
    caution: 'Often pairs naturally with positive Stereo Depth, but can strengthen vection.',
  }
})
</script>

<template>
  <section
    class="grid gap-4 rounded-[1rem] border p-4 shadow-panel backdrop-blur lg:grid-cols-[minmax(17rem,0.9fr)_minmax(19rem,1.1fr)]"
    :class="muted ? 'surface-panel-soft opacity-70' : 'surface-panel'"
    aria-label="Combined Depth effect"
  >
    <div class="min-w-0">
      <div class="mb-2 flex flex-wrap items-start justify-between gap-2">
        <div>
          <p class="text-sm font-semibold tracking-tight">Depth pairing map</p>
          <p class="mt-1 text-xs text-muted">Drag the puck to explore both controls together. Use the sliders for fine tuning.</p>
        </div>
        <span class="chip-accent rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.12em]">
          {{ depthLock ? 'Depth Lock on' : 'Depth Lock off' }}
        </span>
      </div>
      <p class="mb-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">Convergence: Nearer</p>
      <div class="grid gap-2 sm:grid-cols-[minmax(6rem,0.32fr)_minmax(15rem,1fr)_minmax(6rem,0.32fr)] sm:items-center">
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-right">Depth: Larger &amp; Flatter</p>
        <button
          class="depth-map relative grid aspect-[1.65/1] min-h-[12rem] w-full touch-none grid-cols-2 grid-rows-2 overflow-hidden rounded-[0.8rem] border text-left"
          :class="{ dragging }"
          :disabled="muted"
          type="button"
          aria-label="Depth pairing map. Drag to set Stereo Depth horizontally and Convergence vertically. Arrow keys make fine adjustments; hold Shift for larger steps."
          @pointerdown="beginDrag"
          @pointermove="continueDrag"
          @pointerup="endDrag"
          @pointercancel="endDrag"
          @keydown="adjustFromKeyboard"
        >
          <span class="depth-quadrant border-b border-r p-3" :class="{ active: activeQuadrant === 'negative-positive' }">
            <strong>Nearer &amp; Flatter</strong>
            <small>Depth &minus; &middot; Convergence &plus;</small>
          </span>
          <span class="depth-quadrant border-b p-3" :class="{ active: activeQuadrant === 'positive-positive' }">
            <strong>Nearer &amp; Stronger</strong>
            <small>Depth &plus; &middot; Convergence &plus;</small>
          </span>
          <span class="depth-quadrant border-r p-3" :class="{ active: activeQuadrant === 'negative-negative' }">
            <strong>Farther &amp; Flatter</strong>
            <small>Depth &minus; &middot; Convergence &minus;</small>
          </span>
          <span class="depth-quadrant p-3" :class="{ active: activeQuadrant === 'positive-negative' }">
            <strong>Farther &amp; Stronger</strong>
            <small>Depth &plus; &middot; Convergence &minus;</small>
          </span>
          <span class="depth-marker" :style="markerStyle" aria-hidden="true"></span>
        </button>
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-left">Depth: Stronger Stereo</p>
      </div>
      <p class="mt-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">Convergence: Farther</p>
    </div>

    <div class="min-w-0 rounded-[0.8rem] border p-4 surface-panel-strong" style="border-color: var(--app-border)">
      <div class="flex flex-wrap items-center gap-2">
        <p class="text-sm font-semibold tracking-tight">Net effect</p>
        <span class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.12em] surface-panel-muted">
          Depth {{ depthValue > 0 ? '+' : '' }}{{ depthValue.toFixed(1) }} &middot; Conv {{ convergenceValue > 0 ? '+' : '' }}{{ convergenceValue.toFixed(1) }}
        </span>
      </div>
      <p class="mt-3 text-base font-semibold">{{ netEffect.title }}</p>
      <p class="mt-2 text-[13px] leading-5 text-muted">{{ netEffect.description }}</p>
      <p class="mt-3 rounded-[0.7rem] px-3 py-2 text-[12px] leading-5 chip-warning">{{ netEffect.caution }}</p>
      <p v-if="!depthLock" class="mt-3 text-[12px] leading-5 text-muted">
        Depth Lock is off, so the runtime may reinterpret and partially normalize this combination at submission.
      </p>
      <p v-else class="mt-3 text-[12px] leading-5 text-muted">
        Depth Lock preserves the rendered pairing when native geometry is restored for submission.
      </p>
    </div>
  </section>
</template>

<style scoped>
.depth-map { border-color: var(--app-border); background: var(--app-surface-subtle); cursor: crosshair; }
.depth-map:disabled { cursor: default; }
.depth-quadrant { display: block; color: var(--app-text-muted); border-color: var(--app-border); background: var(--app-surface-subtle); }
.depth-quadrant strong, .depth-quadrant small { display: block; pointer-events: none; }
.depth-quadrant strong { color: var(--app-text); font-size: 0.75rem; font-weight: 600; }
.depth-quadrant small { margin-top: 0.25rem; font-size: 0.65rem; }
.depth-quadrant.active { background: var(--app-accent-soft); color: var(--app-accent-soft-text); }
.depth-marker { position: absolute; width: 0.95rem; height: 0.95rem; border: 2px solid var(--app-surface-strong); border-radius: 999px; background: var(--app-accent); box-shadow: 0 0 0 2px var(--app-accent-strong); transform: translate(-50%, -50%); transition: left 160ms ease, top 160ms ease; pointer-events: none; }
.depth-map.dragging .depth-marker { transition: none; }
@media (prefers-reduced-motion: reduce) { .depth-marker { transition: none; } }
</style>
