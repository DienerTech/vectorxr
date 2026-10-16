<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import OptionCards from './OptionCards.vue'
import { useGpuCapabilities } from '../lib/gpuCapabilities'
import { motionSourceState } from '../lib/motionSource'
import {
  defaultFrameGenSettings,
  frameGenFixedMultipliers,
  frameGenLimits,
  type FrameGenFixedMultiplier,
  type FrameGenFlowQuality,
  type FrameGenMotionEstimation,
  type FrameGenSettings,
  type FrameGenSynthesisMode,
} from '../lib/model'

// Shared Frame Generation settings editor used by both the Default Profile and
// each custom profile, so the two stay visually and structurally identical.
// Fields mutate the passed-in reactive `settings` object directly. Every field
// here applies live; only turning Frame Generation on needs a restart.
const props = defineProps<{
  settings: FrameGenSettings
}>()

const modes: { value: FrameGenSynthesisMode; label: string; badge?: string; description: string }[] = [
  {
    value: 'interpolate',
    label: 'Interpolate',
    badge: 'Default',
    description: 'Blends between the last two game frames. The smoothest image with the fewest artifacts; adds a little latency.',
  },
  {
    value: 'extrapolate',
    label: 'Extrapolate',
    description: 'Projects the newest game frame forward. The lowest latency, with more artifacts at moving edges; works best with NVIDIA hardware.',
  },
]

// Dynamic and the fixed rates share one setting: the fixed rates are every
// value but 'dynamic'. The last fixed rate is remembered while the page is open,
// so switching to Dynamic and back keeps it.
type FrameRate = 'dynamic' | 'fixed'
const frameRates: { value: FrameRate; label: string; badge?: string; description: string }[] = [
  {
    value: 'dynamic',
    label: 'Dynamic',
    badge: 'Default',
    description: 'The game runs as fast as it can, and generated frames fill in up to your headset\'s refresh rate.',
  },
  {
    value: 'fixed',
    label: 'Fixed',
    description: 'Holds the game at a set fraction of the refresh rate for even pacing, and fills the rest with generated frames.',
  },
]

const fixedRates: { value: FrameGenFixedMultiplier; label: string; description: string }[] = [
  { value: 'auto', label: 'Auto', description: 'Picks 2x, 3x or 4x as the game needs, and generates nothing while the game keeps up on its own.' },
  { value: '2x', label: '2x', description: 'The game runs at 1/2 the refresh rate, such as 45 fps at 90 Hz.' },
  { value: '3x', label: '3x', description: 'The game runs at 1/3 the refresh rate, such as 30 fps at 90 Hz.' },
  { value: '4x', label: '4x', description: 'The game runs at 1/4 the refresh rate, such as 22.5 fps at 90 Hz. The most artifacts; for the heaviest games.' },
]

function isFixed(value: string): value is FrameGenFixedMultiplier {
  return (frameGenFixedMultipliers as readonly string[]).includes(value)
}

const lastFixedRate = ref<FrameGenFixedMultiplier>(isFixed(props.settings.frameMultiplier) ? props.settings.frameMultiplier : 'auto')
watch(() => props.settings.frameMultiplier, (value) => {
  if (isFixed(value)) lastFixedRate.value = value
})

const frameRate = computed<FrameRate>({
  get: () => props.settings.frameMultiplier === 'dynamic' ? 'dynamic' : 'fixed',
  set: (value) => {
    props.settings.frameMultiplier = value === 'dynamic' ? 'dynamic' : lastFixedRate.value
  },
})
const fixedRateDescription = computed(() => fixedRates.find((rate) => rate.value === props.settings.frameMultiplier)?.description ?? '')

// Interpolate in Dynamic always measures with Shader, and NVIDIA hardware needs a card
// that has it (any card in the PC, as the driver reports): the cards show
// the source in use, greyed where the choice does nothing; a click still
// saves the choice.
const gpuCapabilities = useGpuCapabilities()
const motionSource = computed(() => motionSourceState(props.settings, gpuCapabilities.value))
const motionSourceModel = computed<FrameGenMotionEstimation>({
  get: () => motionSource.value.effective,
  set: (value) => { props.settings.motionEstimation = value },
})
const motionSources = computed(() => [
  {
    value: 'shader' as const,
    label: 'Shader',
    badge: 'Default',
    description: 'Measures motion on the GPU\'s shader cores. Works on any GPU, and pairs best with Interpolate.',
  },
  {
    value: 'hardware' as const,
    label: 'NVIDIA hardware',
    description: 'Uses the optical flow unit of NVIDIA RTX GPUs: more accurate, especially on repeating patterns. Used by Extrapolate, and by Interpolate at a fixed or Auto frame rate (experimental).',
    disabled: motionSource.value.hardwareUnavailable !== null,
    disabledReason: motionSource.value.hardwareUnavailable,
  },
])

const flowQualities: { value: FrameGenFlowQuality; label: string; resolution: string }[] = [
  { value: 'performance', label: 'Performance', resolution: '1/16' },
  { value: 'balanced', label: 'Balanced', resolution: '1/8' },
  { value: 'quality', label: 'Quality', resolution: '1/4' },
]
const flowQuality = computed(() => flowQualities.find((item) => item.value === props.settings.flowQuality) ?? flowQualities[1])

const advancedDefaults = defaultFrameGenSettings()
const advancedAtDefaults = computed(() =>
  props.settings.flowQuality === advancedDefaults.flowQuality &&
  props.settings.artifactSuppression === advancedDefaults.artifactSuppression &&
  props.settings.motionLimitPercent === advancedDefaults.motionLimitPercent)
const advancedSummary = computed(() =>
  `${flowQuality.value.label} flow · Suppression ${props.settings.artifactSuppression} · Motion limit ${props.settings.motionLimitPercent}%`)

function resetAdvanced() {
  props.settings.flowQuality = advancedDefaults.flowQuality
  props.settings.artifactSuppression = advancedDefaults.artifactSuppression
  props.settings.motionLimitPercent = advancedDefaults.motionLimitPercent
}
</script>

<template>
  <div class="space-y-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="settings.synthesisMode" legend="Mode" :options="modes" />
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="frameRate" legend="Frame rate" :options="frameRates">
        <div v-if="frameRate === 'fixed'" class="mt-3 flex flex-wrap items-center justify-between gap-3">
          <span class="text-sm font-medium">Fixed rate</span>
          <div class="segmented flex w-full sm:w-auto" role="radiogroup" aria-label="Fixed rate">
            <button
              v-for="rate in fixedRates"
              :key="rate.value"
              type="button"
              role="radio"
              class="segmented-option flex-1 sm:min-w-[4rem]"
              :class="{ 'segmented-option-active': settings.frameMultiplier === rate.value }"
              :aria-checked="settings.frameMultiplier === rate.value"
              :title="rate.description"
              @click="settings.frameMultiplier = rate.value"
            >
              {{ rate.label }}
            </button>
          </div>
        </div>
        <p v-if="frameRate === 'fixed'" class="mt-2 text-xs leading-5 text-muted">{{ fixedRateDescription }}</p>
      </OptionCards>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="motionSourceModel" legend="Motion source" :options="motionSources" :disabled="motionSource.locked">
        <p v-if="motionSource.note" class="mt-2 text-xs leading-5 text-muted">{{ motionSource.note }}</p>
      </OptionCards>
    </div>

    <details class="section-disclosure rounded-[1rem] border p-4 surface-panel-soft" style="border-color: var(--app-border)">
      <summary class="flex flex-wrap items-center gap-2">
        <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
          <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
        </svg>
        <span class="text-sm font-semibold tracking-tight">Advanced</span>
        <span class="text-xs text-muted">{{ advancedSummary }}</span>
      </summary>

      <div class="mt-3 grid gap-3 lg:grid-cols-3">
        <div class="rounded-[1rem] border p-4 surface-panel">
          <div class="flex flex-wrap items-center justify-between gap-2">
            <p class="eyebrow text-xs uppercase tracking-[0.18em]">Flow quality</p>
            <span class="text-xs text-muted">{{ flowQuality.resolution }} of the eye image</span>
          </div>
          <div class="segmented mt-3 flex w-full" role="radiogroup" aria-label="Flow quality">
            <button
              v-for="quality in flowQualities"
              :key="quality.value"
              type="button"
              role="radio"
              class="segmented-option flex-1"
              :class="{ 'segmented-option-active': settings.flowQuality === quality.value }"
              :aria-checked="settings.flowQuality === quality.value"
              @click="settings.flowQuality = quality.value"
            >
              {{ quality.label }}
            </button>
          </div>
          <p class="mt-2 text-xs leading-5 text-muted">
            How finely motion is measured. Higher tracks small, thin objects better and costs more GPU time.
          </p>
        </div>

        <label class="block rounded-[1rem] border p-4 surface-panel">
          <span class="flex flex-wrap items-center justify-between gap-2">
            <span class="eyebrow text-xs uppercase tracking-[0.18em]">Artifact suppression</span>
            <output class="text-sm font-semibold tabular-nums">{{ settings.artifactSuppression }}</output>
          </span>
          <span class="mt-3 flex items-center gap-3">
            <input
              v-model.number="settings.artifactSuppression"
              class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper"
              aria-label="Artifact suppression"
              type="range"
              :min="frameGenLimits.artifactSuppression.min"
              :max="frameGenLimits.artifactSuppression.max"
              step="1"
            />
            <input
              v-model.number="settings.artifactSuppression"
              class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm"
              aria-label="Artifact suppression value"
              type="number"
              :min="frameGenLimits.artifactSuppression.min"
              :max="frameGenLimits.artifactSuppression.max"
              step="1"
            />
          </span>
          <span class="mt-2 grid grid-cols-2 text-[10px] font-medium uppercase tracking-[0.08em] text-muted">
            <span>Smoother motion</span>
            <span class="text-right">Fewer artifacts</span>
          </span>
          <span class="mt-2 block text-xs leading-5 text-muted">
            Where motion is uncertain, higher values show the real image sooner: less warping, a little more judder.
          </span>
        </label>

        <label class="block rounded-[1rem] border p-4 surface-panel">
          <span class="flex flex-wrap items-center justify-between gap-2">
            <span class="eyebrow text-xs uppercase tracking-[0.18em]">Motion limit</span>
            <output class="text-sm font-semibold tabular-nums">{{ settings.motionLimitPercent }}%</output>
          </span>
          <span class="mt-3 flex items-center gap-3">
            <input
              v-model.number="settings.motionLimitPercent"
              class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper"
              aria-label="Motion limit"
              type="range"
              :min="frameGenLimits.motionLimitPercent.min"
              :max="frameGenLimits.motionLimitPercent.max"
              step="1"
            />
            <input
              v-model.number="settings.motionLimitPercent"
              class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm"
              aria-label="Motion limit value in percent of view width"
              type="number"
              :min="frameGenLimits.motionLimitPercent.min"
              :max="frameGenLimits.motionLimitPercent.max"
              step="1"
            />
          </span>
          <span class="mt-2 block text-xs leading-5 text-muted">
            The farthest a generated frame may move the image, as a share of view width. Faster motion shows the real image.
          </span>
        </label>
      </div>

      <div v-if="!advancedAtDefaults" class="mt-3 flex justify-end">
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="resetAdvanced">
          Reset advanced settings
        </button>
      </div>
    </details>
  </div>
</template>
