<script setup lang="ts">
import { useId } from 'vue'

import { frameGenLimits, type FrameGenFlowQuality, type FrameGenSettings, type FrameGenSynthesisMode } from '../lib/model'

// Shared Frame Generation settings editor used by both the Default Profile and
// each custom profile, so the two stay visually and structurally identical.
// Fields mutate the passed-in reactive `settings` object directly. Every field
// here applies live; only turning Frame Generation on or off needs a restart.
defineProps<{
  settings: FrameGenSettings
}>()

const id = useId()

const synthesisModes: { value: FrameGenSynthesisMode; label: string; badge?: string; description: string }[] = [
  {
    value: 'interpolate',
    label: 'Interpolate',
    badge: 'Default',
    description: 'Blends between the previous and newest real frame. Fewer artifacts, but each real frame must finish one refresh earlier, so it needs more GPU headroom.',
  },
  {
    value: 'extrapolate',
    label: 'Extrapolate',
    description: 'Pushes the newest real frame forward along its motion. Gives the game more GPU time per frame, with more edge artifacts on fast motion and newly revealed areas.',
  },
]

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
