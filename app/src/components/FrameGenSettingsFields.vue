<script setup lang="ts">
import { computed, useId } from 'vue'

import { frameGenLimits, type FrameGenFlowQuality, type FrameGenFrameMultiplier, type FrameGenMotionEstimation, type FrameGenSettings, type FrameGenSynthesisMode } from '../lib/model'

// Shared Frame Generation settings editor used by both the Default Profile and
// each custom profile, so the two stay visually and structurally identical.
// Fields mutate the passed-in reactive `settings` object directly. Every field
// here applies live; only turning Frame Generation on needs a restart.
const props = defineProps<{
  settings: FrameGenSettings
}>()

const id = useId()

const synthesisModes: { value: FrameGenSynthesisMode; label: string; badge?: string; description: string }[] = [
  {
    value: 'interpolate',
    label: 'Interpolate',
    badge: 'Default',
    description: 'Blends between the previous and newest real frame. Fewer artifacts; each real frame is shown about one game frame later, after the frames leading up to it. On Direct3D 12, such as MSFS, a game frame that arrives late is covered by frames moved on from the newest one instead of a stutter.',
  },
  {
    value: 'extrapolate',
    label: 'Extrapolate',
    description: 'Pushes the newest real frame forward along its motion. Gives the game more time per frame, with more edge artifacts on fast motion and newly revealed areas. Best for heavy games: on Direct3D 12, such as MSFS, generated frames are shown from their own thread while the game keeps rendering. Use it with NVIDIA hardware motion estimation; with Shader, Interpolate usually looks cleaner.',
  },
]

const frameMultipliers: { value: FrameGenFrameMultiplier; label: string; description: string }[] = [
  {
    value: 'auto',
    label: 'Auto',
    description: 'Starts at full rate, passing frames straight through while the game holds the headset refresh. When it cannot, steps up to 2x, 3x or 4x, and steps back down when it can, checking now and then.',
  },
  {
    value: 'dynamic',
    label: 'Dynamic',
    description: 'The game runs as fast as it can, never held to a fraction of the headset refresh, and generated frames fill the refreshes between its frames: 40 fps becomes 90 with about 1.25 generated frames per real one. Direct3D 12 games, such as MSFS; elsewhere it behaves as Auto.',
  },
  { value: '2x', label: '2x', description: 'The game runs at 1/2 the headset refresh, for example 45 fps at 90 Hz, with one generated frame between real frames.' },
  { value: '3x', label: '3x', description: 'The game runs at 1/3 the headset refresh, for example 30 fps at 90 Hz, with two generated frames between real frames.' },
  { value: '4x', label: '4x', description: 'The game runs at 1/4 the headset refresh, for example 22.5 fps at 90 Hz, with three generated frames between real frames. The most artifacts; for the heaviest games.' },
]

const multiplierDescription = computed(() => frameMultipliers.find((item) => item.value === props.settings.frameMultiplier)?.description ?? '')
const multiplierModeNote = computed(() => props.settings.synthesisMode === 'extrapolate'
  ? 'On Direct3D 12 each generated frame is written right before its refresh into one set of headset-resolution images, whatever the multiplier. Elsewhere Extrapolate keeps a set per generated frame, plus a spare for a late game frame.'
  : 'Interpolate shows real frames about one game frame later than Extrapolate. On Direct3D 12 each generated frame is written right before its refresh into one set of headset-resolution images; elsewhere it needs each real frame ready before the frames leading up to it, so it needs GPU headroom.')

const motionEstimations: { value: FrameGenMotionEstimation; label: string; description: string }[] = [
  {
    value: 'shader',
    label: 'Shader',
    description: 'Block matching on the GPU\'s shader cores. Works on any GPU and graphics API; repeating patterns such as building windows can lock onto the wrong repeat. Pairs best with Interpolate.',
  },
  {
    value: 'hardware',
    label: 'NVIDIA hardware',
    description: 'Recommended on NVIDIA RTX 20-50 GPUs: their optical flow accelerator measures motion in Direct3D 12 games, such as MSFS, at least as well as Shader, and repeating patterns stay put. Extrapolate needs it to look its best. It runs on Frame Generation\'s own GPU queue, after head movement is taken out, so the game does not wait for it. In Direct3D 11 games such as DCS, Interpolate measures motion with Shader instead: beside the game\'s rendering the hardware answers too late for the frames between real frames. Elsewhere Shader is used and the log says why.',
  },
]

const motionEstimationDescription = computed(() =>
  motionEstimations.find((item) => item.value === props.settings.motionEstimation)?.description ?? '')

const flowQualities: { value: FrameGenFlowQuality; label: string; resolution: string }[] = [
  { value: 'performance', label: 'Performance', resolution: '1/16' },
  { value: 'balanced', label: 'Balanced', resolution: '1/8' },
  { value: 'quality', label: 'Quality', resolution: '1/4' },
]

function flowResolution(quality: FrameGenFlowQuality) {
  return flowQualities.find((item) => item.value === quality)?.resolution ?? '1/8'
}
</script>

<template>
  <div class="space-y-3">
    <fieldset class="rounded-[1rem] border p-4 surface-panel-soft">
      <legend class="sr-only">Synthesis mode</legend>
      <p class="eyebrow text-xs uppercase tracking-[0.18em]" aria-hidden="true">Synthesis mode</p>
      <div class="mt-3 grid gap-3 md:grid-cols-2">
        <label
          v-for="mode in synthesisModes"
          :key="mode.value"
          class="flex cursor-pointer items-start gap-3 rounded-[0.9rem] border p-3 transition"
          :class="settings.synthesisMode === mode.value ? 'surface-panel' : 'surface-panel-soft'"
          :style="settings.synthesisMode === mode.value ? { borderColor: 'var(--app-accent)' } : { borderColor: 'var(--app-border)' }"
        >
          <input
            v-model="settings.synthesisMode"
            :name="`${id}-synthesis-mode`"
            :value="mode.value"
            class="mt-1 h-4 w-4 shrink-0 accent-depthxr-copper"
            type="radio"
          />
          <span class="min-w-0">
            <span class="flex flex-wrap items-center gap-2">
              <span class="text-sm font-semibold tracking-tight">{{ mode.label }}</span>
              <span v-if="mode.badge" class="chip-idle rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.14em]">{{ mode.badge }}</span>
            </span>
            <span class="mt-1 block text-xs leading-5 text-muted">{{ mode.description }}</span>
          </span>
        </label>
      </div>
    </fieldset>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Frame multiplier</p>
        <div class="segmented flex w-full sm:w-auto" role="radiogroup" aria-label="Frame multiplier">
          <button
            v-for="multiplier in frameMultipliers"
            :key="multiplier.value"
            type="button"
            role="radio"
            class="segmented-option flex-1 sm:min-w-[4.5rem]"
            :class="{ 'segmented-option-active': settings.frameMultiplier === multiplier.value }"
            :aria-checked="settings.frameMultiplier === multiplier.value"
            :title="multiplier.description"
            @click="settings.frameMultiplier = multiplier.value"
          >
            {{ multiplier.label }}
          </button>
        </div>
      </div>
      <p class="mt-2 text-sm leading-6">{{ multiplierDescription }}</p>
      <p class="mt-1 text-xs leading-5 text-muted">
        Headset frames per real game frame. Higher multipliers rescue very heavy games, but each generated frame moves content further, so artifacts grow. {{ multiplierModeNote }}
      </p>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Motion estimation</p>
        <div class="segmented flex w-full sm:w-auto" role="radiogroup" aria-label="Motion estimation">
          <button
            v-for="estimation in motionEstimations"
            :key="estimation.value"
            type="button"
            role="radio"
            class="segmented-option flex-1 sm:min-w-[6rem]"
            :class="{ 'segmented-option-active': settings.motionEstimation === estimation.value }"
            :aria-checked="settings.motionEstimation === estimation.value"
            :title="estimation.description"
            @click="settings.motionEstimation = estimation.value"
          >
            {{ estimation.label }}
          </button>
        </div>
      </div>
      <p class="mt-2 text-sm leading-6">{{ motionEstimationDescription }}</p>
    </div>

    <div class="grid gap-3 lg:grid-cols-3">
      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-center justify-between gap-2">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Flow quality</p>
          <span class="text-xs text-muted">{{ flowResolution(settings.flowQuality) }} of the eye image</span>
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
          The resolution motion is measured at. Higher quality tracks small and thin objects better and costs more GPU time.
        </p>
      </div>

      <label class="block rounded-[1rem] border p-4 surface-panel-soft">
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
          Higher values fall back to the real image sooner where motion is uncertain: fewer warping artifacts, more local judder.
        </span>
      </label>

      <label class="block rounded-[1rem] border p-4 surface-panel-soft">
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
          The farthest a generated frame may move image content, as a percentage of view width. Motion beyond it falls back to the real image.
        </span>
      </label>
    </div>
  </div>
</template>
