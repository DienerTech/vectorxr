<script setup lang="ts">
import { computed, ref } from 'vue'

import { t } from '../i18n/index.ts'
import { fromConvergenceDisplay, fromStereoBoostDisplay, toConvergenceDisplay, toStereoBoostDisplay } from '../lib/display'
import I18nText from './I18nText.vue'

const props = defineProps<{
  stereoBoost: number
  convergence: number
  depthLock: boolean
  stereoDepthLimit?: number
  convergenceLimit?: number
  muted?: boolean
  // Collapse behind a summary row (closed by default) so repeated profiles stay compact.
  collapsible?: boolean
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
const expanded = ref(false)

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

type NetEffectCase =
  | 'native' | 'compactNear' | 'detailedFar' | 'largerNear' | 'lifeSizedFar'
  | 'strongerStereo' | 'largerScale' | 'planeNearer' | 'planeFarther'

const netEffectCase = computed<NetEffectCase>(() => {
  const depth = depthSign.value
  const convergence = convergenceSign.value

  if (depth === 0 && convergence === 0) return 'native'
  if (depth > 0 && convergence > 0) return 'compactNear'
  if (depth > 0 && convergence < 0) return 'detailedFar'
  if (depth < 0 && convergence > 0) return 'largerNear'
  if (depth < 0 && convergence < 0) return 'lifeSizedFar'
  if (depth > 0) return 'strongerStereo'
  if (depth < 0) return 'largerScale'
  if (convergence > 0) return 'planeNearer'
  return 'planeFarther'
})

const netEffect = computed(() => {
  const name = netEffectCase.value
  return {
    title: t(`depth.netEffect.cases.${name}.title`),
    description: t(`depth.netEffect.cases.${name}.description`),
    useCase: t(`depth.netEffect.cases.${name}.useCase`),
    caution: t(`depth.netEffect.cases.${name}.caution`),
  }
})

const valuesText = computed(() => t('depth.pairingMap.values', {
  depth: `${depthValue.value > 0 ? '+' : ''}${depthValue.value.toFixed(1)}`,
  convergence: `${convergenceValue.value > 0 ? '+' : ''}${convergenceValue.value.toFixed(1)}`,
}))
</script>

<template>
  <div :class="collapsible ? ['rounded-[1rem] border px-4 py-3 surface-panel-soft', { 'opacity-70': muted }] : undefined" :style="collapsible ? 'border-color: var(--app-border)' : undefined">
  <button v-if="collapsible" type="button" class="flex w-full flex-wrap items-center gap-2 text-left" :aria-expanded="expanded" @click="expanded = !expanded">
    <svg aria-hidden="true" class="depth-map-chevron h-3.5 w-3.5" :class="{ open: expanded }" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
    <span class="text-sm font-semibold tracking-tight">{{ t('depth.pairingMap.heading') }}</span>
    <span class="text-xs text-muted">{{ netEffect.title }}</span>
    <span class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.12em] surface-panel-muted">
      {{ valuesText }}
    </span>
  </button>
  <section
    v-show="!collapsible || expanded"
    class="grid gap-4 lg:grid-cols-[minmax(17rem,0.9fr)_minmax(19rem,1.1fr)]"
    :class="collapsible ? 'mt-3' : ['rounded-[1rem] border p-4 shadow-panel backdrop-blur', muted ? 'surface-panel-soft opacity-70' : 'surface-panel']"
    :aria-label="t('depth.pairingMap.regionLabel')"
  >
    <div class="min-w-0">
      <div class="mb-2 flex flex-wrap items-start justify-between gap-2">
        <div>
          <p class="text-sm font-semibold tracking-tight">{{ t('depth.pairingMap.heading') }}</p>
          <p class="mt-1 text-xs text-muted">{{ t('depth.pairingMap.hint') }}</p>
        </div>
        <span class="chip-accent rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.12em]">
          {{ depthLock ? t('depth.pairingMap.lockOn') : t('depth.pairingMap.lockOff') }}
        </span>
      </div>
      <p class="mb-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">{{ t('depth.pairingMap.convergenceNearer', { limit: mapConvergenceLimit }) }}</p>
      <div class="grid gap-2 sm:grid-cols-[minmax(6rem,0.32fr)_minmax(15rem,1fr)_minmax(6rem,0.32fr)] sm:items-center">
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-right">{{ t('depth.pairingMap.depthLarger', { limit: mapStereoDepthLimit }) }}</p>
        <button
          class="depth-map relative grid aspect-[1.65/1] min-h-[12rem] w-full touch-none grid-cols-2 grid-rows-2 overflow-hidden rounded-[0.8rem] border text-left"
          :class="{ dragging }"
          :disabled="muted"
          type="button"
          :aria-label="t('depth.pairingMap.mapLabel')"
          @pointerdown="beginDrag"
          @pointermove="continueDrag"
          @pointerup="endDrag"
          @pointercancel="endDrag"
          @keydown="adjustFromKeyboard"
        >
          <span class="depth-quadrant border-b border-r p-3" :class="{ active: activeQuadrant === 'negative-positive' }">
            <strong>{{ t('depth.pairingMap.quadrants.largerNear.label') }}</strong>
            <small>{{ t('depth.pairingMap.quadrants.largerNear.axes') }}</small>
          </span>
          <span class="depth-quadrant border-b p-3" :class="{ active: activeQuadrant === 'positive-positive' }">
            <strong>{{ t('depth.pairingMap.quadrants.strongerNear.label') }}</strong>
            <small>{{ t('depth.pairingMap.quadrants.strongerNear.axes') }}</small>
          </span>
          <span class="depth-quadrant border-r p-3" :class="{ active: activeQuadrant === 'negative-negative' }">
            <strong>{{ t('depth.pairingMap.quadrants.largerFar.label') }}</strong>
            <small>{{ t('depth.pairingMap.quadrants.largerFar.axes') }}</small>
          </span>
          <span class="depth-quadrant p-3" :class="{ active: activeQuadrant === 'positive-negative' }">
            <strong>{{ t('depth.pairingMap.quadrants.strongerFar.label') }}</strong>
            <small>{{ t('depth.pairingMap.quadrants.strongerFar.axes') }}</small>
          </span>
          <span class="depth-marker" :style="markerStyle" aria-hidden="true"></span>
        </button>
        <p class="text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted sm:text-left">{{ t('depth.pairingMap.depthStronger', { limit: mapStereoDepthLimit }) }}</p>
      </div>
      <p class="mt-2 text-center text-[10px] font-medium uppercase tracking-[0.08em] text-muted">{{ t('depth.pairingMap.convergenceFarther', { limit: mapConvergenceLimit }) }}</p>
    </div>

    <div class="min-w-0 rounded-[0.8rem] border p-4 surface-panel-strong" style="border-color: var(--app-border)">
      <div class="flex flex-wrap items-center gap-2">
        <p class="text-sm font-semibold tracking-tight">{{ t('depth.netEffect.heading') }}</p>
        <span class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.12em] surface-panel-muted">
          {{ valuesText }}
        </span>
      </div>
      <p class="mt-3 text-base font-semibold">{{ netEffect.title }}</p>
      <p class="mt-2 text-[13px] leading-5 text-muted">{{ netEffect.description }}</p>
      <I18nText keypath="depth.netEffect.goodFit" :params="{ text: netEffect.useCase }" tag="p" class="mt-3 rounded-[0.7rem] px-3 py-2 text-[12px] leading-5 surface-panel-muted">
        <template #label><strong>{{ t('depth.netEffect.goodFitLabel') }}</strong></template>
      </I18nText>
      <I18nText keypath="depth.netEffect.watchFor" :params="{ text: netEffect.caution }" tag="p" class="mt-3 text-[12px] leading-5 text-muted">
        <template #label><strong>{{ t('depth.netEffect.watchForLabel') }}</strong></template>
      </I18nText>
      <p v-if="!depthLock" class="mt-3 text-[12px] leading-5 text-muted">
        {{ t('depth.netEffect.lockOff') }}
      </p>
      <p v-else class="mt-3 text-[12px] leading-5 text-muted">
        {{ t('depth.netEffect.lockOn') }}
      </p>
    </div>
  </section>
  </div>
</template>

<style scoped>
.depth-map-chevron { color: var(--app-text-soft); transition: transform 160ms ease; }
.depth-map-chevron.open { transform: rotate(90deg); }
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
