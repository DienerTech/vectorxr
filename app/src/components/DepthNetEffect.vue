<script setup lang="ts">
import { computed, ref } from 'vue'

import { fromConvergenceDisplay, fromStereoBoostDisplay, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'

const props = defineProps<{
  stereoBoost: number
  convergence: number
  depthLock: boolean
  stereoDepthLimit?: number
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
const mapStereoDepthLimit = computed(() => props.stereoDepthLimit ?? 25)
const mapConvergenceLimit = computed(() => props.convergenceLimit ?? 5)
const depthSign = computed(() => sign(depthValue.value, 0.05))
const convergenceSign = computed(() => sign(convergenceValue.value, 0.05))
const dragging = ref(false)

const markerStyle = computed(() => ({
  left: `${50 + clamp(depthValue.value / mapStereoDepthLimit.value, -1, 1) * 43}%`,
  top: `${50 - clamp(convergenceValue.value / mapConvergenceLimit.value, -1, 1) * 43}%`,
}))

function setFromPointer(event: PointerEvent) {
  if (props.muted) return
  const element = event.currentTarget as HTMLButtonElement
  const bounds = element.getBoundingClientRect()
  const x = clamp(((event.clientX - bounds.left) / bounds.width - 0.5) / 0.43, -1, 1)
  const y = clamp((0.5 - (event.clientY - bounds.top) / bounds.height) / 0.43, -1, 1)
  emit('update:stereoBoost', fromStereoBoostDisplay(x * mapStereoDepthLimit.value))
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
  emit('update:stereoBoost', fromStereoBoostDisplay(clamp(depth, -mapStereoDepthLimit.value, mapStereoDepthLimit.value)))
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
      useCase: "Use this to learn a title's authored scale and as the A/B reference for every profile.",
      caution: 'If the native proportions already feel convincing, no correction is necessary.',
    }
  }
  if (depth > 0 && convergence > 0) {
    return {
      title: 'Compact scale with immediate presence',
      description: 'Stronger stereo separation makes the world feel smaller and more dimensional, while a nearer plane brings the working area toward you.',
      useCase: 'Useful when a cockpit feels oversized or distant and you want panels, controls, and close geometry to feel immediate.',
      caution: 'Both controls increase near-field demand, so approach this quadrant in small steps.',
    }
  }
  if (depth > 0 && convergence < 0) {
    return {
      title: 'Detailed stereo with comfortable placement',
      description: 'Stronger stereo separation adds shape and presence while a farther plane gives that more compact image breathing room.',
      useCase: 'Useful when native proportions feel too large, but positive Stereo Depth by itself makes the cockpit feel crowded.',
      caution: 'Tune Stereo Depth first, then move the plane only far enough to make the stronger image settle naturally.',
    }
  }
  if (depth < 0 && convergence > 0) {
    return {
      title: 'Larger scale with a near working plane',
      description: 'Reduced stereo separation expands apparent world scale, while a nearer plane keeps the panel and controls visually accessible.',
      useCase: 'Useful when a cockpit feels toy-like but its gauges or interaction area should still feel close and readable.',
      caution: 'A strongly positive Convergence value can become tiring even when Stereo Depth is reduced.',
    }
  }
  if (depth < 0 && convergence < 0) {
    return {
      title: 'Life-sized scale with relaxed placement',
      description: 'Reduced stereo separation increases apparent size, while a farther plane lets the cockpit surround you instead of crowding your face.',
      useCase: 'Useful in aircraft and vehicle sims whose native camera feels miniaturized or too close; it can better match remembered real-world proportions.',
      caution: 'The softer stereo is intentional—stop when scale feels natural rather than chasing the strongest possible 3D effect.',
    }
  }
  if (depth > 0) {
    return {
      title: 'Stronger relative stereo depth',
      description: 'Nearby geometry gains separation and shape, increasing presence while making the world feel somewhat smaller.',
      useCase: 'Useful when a title looks flat or oversized but its existing depth-plane placement already feels comfortable.',
      caution: 'If the scene starts to crowd you, pair it with a small negative Convergence adjustment.',
    }
  }
  if (depth < 0) {
    return {
      title: 'Larger, gentler world scale',
      description: 'Reduced stereo separation softens near-field intensity and increases apparent world size.',
      useCase: 'Useful when native geometry feels miniaturized, cramped, or more stereoscopically intense than the real vehicle.',
      caution: 'Reduce only far enough to restore believable proportions and retain useful shape cues.',
    }
  }
  if (convergence > 0) {
    return {
      title: 'Working plane moved nearer',
      description: "The scene's disparity field moves toward you without changing its relative depth gradient or overall scale.",
      useCase: 'Useful when scale looks correct but the panel, sight, or interaction area feels visually too far away.',
      caution: 'Positive values can feel cross-eyed, especially when paired with positive Stereo Depth.',
    }
  }
  return {
    title: 'Working plane moved farther',
    description: "The scene's disparity field moves away without changing its relative depth gradient or overall scale.",
    useCase: 'Useful when scale looks correct but the cockpit or near geometry feels pressed too close to your face.',
    caution: 'Move only far enough to relax placement while keeping the scene easy to fuse.',
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
      <p class="mb-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">Convergence: Nearer (+{{ mapConvergenceLimit }})</p>
      <div class="grid gap-2 sm:grid-cols-[minmax(6rem,0.32fr)_minmax(15rem,1fr)_minmax(6rem,0.32fr)] sm:items-center">
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-right">Depth: Larger Scale (&minus;{{ mapStereoDepthLimit }}%)</p>
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
            <strong>Larger Scale &middot; Near</strong>
            <small>Depth &minus; &middot; Convergence &plus;</small>
          </span>
          <span class="depth-quadrant border-b p-3" :class="{ active: activeQuadrant === 'positive-positive' }">
            <strong>Stronger Stereo &middot; Near</strong>
            <small>Depth &plus; &middot; Convergence &plus;</small>
          </span>
          <span class="depth-quadrant border-r p-3" :class="{ active: activeQuadrant === 'negative-negative' }">
            <strong>Larger Scale &middot; Far</strong>
            <small>Depth &minus; &middot; Convergence &minus;</small>
          </span>
          <span class="depth-quadrant p-3" :class="{ active: activeQuadrant === 'positive-negative' }">
            <strong>Stronger Stereo &middot; Far</strong>
            <small>Depth &plus; &middot; Convergence &minus;</small>
          </span>
          <span class="depth-marker" :style="markerStyle" aria-hidden="true"></span>
        </button>
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-left">Depth: Stronger Stereo (+{{ mapStereoDepthLimit }}%)</p>
      </div>
      <p class="mt-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">Convergence: Farther (&minus;{{ mapConvergenceLimit }})</p>
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
      <p class="mt-3 rounded-[0.7rem] px-3 py-2 text-[12px] leading-5 surface-panel-muted"><strong>Good fit:</strong> {{ netEffect.useCase }}</p>
      <p class="mt-3 text-[12px] leading-5 text-muted"><strong>Watch for:</strong> {{ netEffect.caution }}</p>
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
