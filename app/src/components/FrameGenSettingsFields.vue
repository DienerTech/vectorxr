<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import { t } from '../i18n/index.ts'
import OptionCards from './OptionCards.vue'
import { useGpuCapabilities } from '../lib/gpuCapabilities'
import { motionSourceState } from '../lib/motionSource'
import {
  defaultFrameGenSettings,
  frameGenFixedMultipliers,
  frameGenLimits,
  type FrameGenFixedMultiplier,
  type FrameGenFlowQuality,
  type FrameGenInterpolateFlow,
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

const modes = computed<{ value: FrameGenSynthesisMode; label: string; badge?: string; description: string }[]>(() => [
  {
    value: 'interpolate',
    label: t('framegen.settings.mode.interpolate.label'),
    badge: t('common.default'),
    description: t('framegen.settings.mode.interpolate.description'),
  },
  {
    value: 'extrapolate',
    label: t('framegen.settings.mode.extrapolate.label'),
    description: t('framegen.settings.mode.extrapolate.description'),
  },
])

// Dynamic and the fixed rates share one setting: the fixed rates are every
// value but 'dynamic'. The last fixed rate is remembered while the page is open,
// so switching to Dynamic and back keeps it.
type FrameRate = 'dynamic' | 'fixed'
const frameRates = computed<{ value: FrameRate; label: string; badge?: string; description: string }[]>(() => [
  {
    value: 'dynamic',
    label: t('framegen.settings.frameRate.dynamic.label'),
    badge: t('common.default'),
    description: t('framegen.settings.frameRate.dynamic.description'),
  },
  {
    value: 'fixed',
    label: t('framegen.settings.frameRate.fixed.label'),
    description: t('framegen.settings.frameRate.fixed.description'),
  },
])

const fixedRates = computed<{ value: FrameGenFixedMultiplier; label: string; description: string }[]>(() => [
  { value: 'auto', label: t('common.auto'), description: t('framegen.settings.frameRate.rates.auto') },
  { value: '2x', label: '2x', description: t('framegen.settings.frameRate.rates.x2') },
  { value: '3x', label: '3x', description: t('framegen.settings.frameRate.rates.x3') },
  { value: '4x', label: '4x', description: t('framegen.settings.frameRate.rates.x4') },
])

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
const fixedRateDescription = computed(() => fixedRates.value.find((rate) => rate.value === props.settings.frameMultiplier)?.description ?? '')

// NVIDIA hardware needs a card that has it. Interpolate can fall back to
// shader flow while running; this editor preserves the saved hardware choice.
const gpuCapabilities = useGpuCapabilities()
const motionSource = computed(() => motionSourceState(props.settings, gpuCapabilities.value))
const motionSourceModel = computed<FrameGenMotionEstimation>({
  // This card edits the saved source; the separate policy may temporarily use Shader.
  get: () => motionSource.value.hardwareUnavailable ? 'shader' : props.settings.motionEstimation,
  set: (value) => { props.settings.motionEstimation = value },
})
const motionSources = computed(() => [
  {
    value: 'shader' as const,
    label: t('framegen.settings.motionSource.shader.label'),
    badge: t('common.default'),
    description: t('framegen.settings.motionSource.shader.description'),
  },
  {
    value: 'hardware' as const,
    label: t('framegen.settings.motionSource.hardware.label'),
    description: t('framegen.settings.motionSource.hardware.description'),
    disabled: motionSource.value.hardwareUnavailable !== null,
    disabledReason: motionSource.value.hardwareUnavailable,
  },
])

const interpolateFlows = computed<{ value: FrameGenInterpolateFlow; label: string; description: string }[]>(() => [
  { value: 'auto', label: t('common.auto'), description: t('framegen.settings.interpolateFlow.description') },
  { value: 'hardware', label: t('framegen.settings.motionSource.hardware.label'), description: t('framegen.settings.interpolateFlow.hardware') },
  { value: 'shader', label: t('framegen.settings.motionSource.shader.label'), description: t('framegen.settings.interpolateFlow.shader') },
])

const flowQualities = computed<{ value: FrameGenFlowQuality; label: string; summary: string; resolution: string }[]>(() => [
  { value: 'performance', label: t('framegen.settings.flowQuality.levels.performance'), summary: t('framegen.settings.flowQuality.summary.performance'), resolution: '1/16' },
  { value: 'balanced', label: t('framegen.settings.flowQuality.levels.balanced'), summary: t('framegen.settings.flowQuality.summary.balanced'), resolution: '1/8' },
  { value: 'quality', label: t('framegen.settings.flowQuality.levels.quality'), summary: t('framegen.settings.flowQuality.summary.quality'), resolution: '1/4' },
])
const flowQuality = computed(() => flowQualities.value.find((item) => item.value === props.settings.flowQuality) ?? flowQualities.value[1])

// Extend head prediction stays out of the app until it has flown: on a Pimax
// PC with Virtual Desktop installed it finds no runtime clock. The layer still
// reads extendHeadPrediction from settings.json, and a profile that has it on
// still shows it in the summary and clears it with Reset.
const showHeadPrediction = false

const advancedDefaults = defaultFrameGenSettings()
const advancedAtDefaults = computed(() =>
  props.settings.flowQuality === advancedDefaults.flowQuality &&
  props.settings.artifactSuppression === advancedDefaults.artifactSuppression &&
  props.settings.motionLimitPercent === advancedDefaults.motionLimitPercent &&
  !props.settings.extendHeadPrediction)
const advancedSummary = computed(() =>
  t(props.settings.extendHeadPrediction ? 'framegen.settings.advanced.summaryHeadPrediction' : 'framegen.settings.advanced.summary', {
    flow: flowQuality.value.summary,
    suppression: props.settings.artifactSuppression,
    motionLimit: props.settings.motionLimitPercent,
  }))

function resetAdvanced() {
  props.settings.flowQuality = advancedDefaults.flowQuality
  props.settings.artifactSuppression = advancedDefaults.artifactSuppression
  props.settings.motionLimitPercent = advancedDefaults.motionLimitPercent
  delete props.settings.extendHeadPrediction
}
</script>

<template>
  <div class="space-y-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="settings.synthesisMode" :legend="t('framegen.settings.mode.legend')" :options="modes" />
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="frameRate" :legend="t('framegen.settings.frameRate.legend')" :options="frameRates">
        <div v-if="frameRate === 'fixed'" class="mt-3 flex flex-wrap items-center justify-between gap-3">
          <span class="text-sm font-medium">{{ t('framegen.settings.frameRate.fixedRate') }}</span>
          <div class="segmented flex w-full sm:w-auto" role="radiogroup" :aria-label="t('framegen.settings.frameRate.fixedRate')">
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

    <div v-if="settings.synthesisMode === 'interpolate' && settings.motionEstimation === 'hardware'" class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="settings.interpolateFlow" :legend="t('framegen.settings.interpolateFlow.legend')" :options="interpolateFlows" />
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <OptionCards v-model="motionSourceModel" :legend="t('framegen.settings.motionSource.legend')" :options="motionSources" :disabled="motionSource.locked">
        <p v-if="motionSource.note" class="mt-2 text-xs leading-5 text-muted">{{ motionSource.note }}</p>
      </OptionCards>
    </div>

    <details class="section-disclosure rounded-[1rem] border p-4 surface-panel-soft" style="border-color: var(--app-border)">
      <summary class="flex flex-wrap items-center gap-2">
        <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
          <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
        </svg>
        <span class="text-sm font-semibold tracking-tight">{{ t('common.advanced') }}</span>
        <span class="text-xs text-muted">{{ advancedSummary }}</span>
      </summary>

      <div class="mt-3 grid gap-3 lg:grid-cols-3">
        <div class="rounded-[1rem] border p-4 surface-panel">
          <div class="flex flex-wrap items-center justify-between gap-2">
            <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('framegen.settings.flowQuality.heading') }}</p>
            <span class="text-xs text-muted">{{ t('framegen.settings.flowQuality.resolution', { resolution: flowQuality.resolution }) }}</span>
          </div>
          <div class="segmented mt-3 flex w-full" role="radiogroup" :aria-label="t('framegen.settings.flowQuality.heading')">
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
            {{ t('framegen.settings.flowQuality.description') }}
          </p>
        </div>

        <label class="block rounded-[1rem] border p-4 surface-panel">
          <span class="flex flex-wrap items-center justify-between gap-2">
            <span class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('framegen.settings.artifactSuppression.heading') }}</span>
            <output class="text-sm font-semibold tabular-nums">{{ settings.artifactSuppression }}</output>
          </span>
          <span class="mt-3 flex items-center gap-3">
            <input
              v-model.number="settings.artifactSuppression"
              class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper"
              :aria-label="t('framegen.settings.artifactSuppression.heading')"
              type="range"
              :min="frameGenLimits.artifactSuppression.min"
              :max="frameGenLimits.artifactSuppression.max"
              step="1"
            />
            <input
              v-model.number="settings.artifactSuppression"
              class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm"
              :aria-label="t('framegen.settings.artifactSuppression.valueLabel')"
              type="number"
              :min="frameGenLimits.artifactSuppression.min"
              :max="frameGenLimits.artifactSuppression.max"
              step="1"
            />
          </span>
          <span class="mt-2 grid grid-cols-2 text-[10px] font-medium uppercase tracking-[0.08em] text-muted">
            <span>{{ t('framegen.settings.artifactSuppression.smoother') }}</span>
            <span class="text-right">{{ t('framegen.settings.artifactSuppression.fewer') }}</span>
          </span>
          <span class="mt-2 block text-xs leading-5 text-muted">
            {{ t('framegen.settings.artifactSuppression.description') }}
          </span>
        </label>

        <label class="block rounded-[1rem] border p-4 surface-panel">
          <span class="flex flex-wrap items-center justify-between gap-2">
            <span class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('framegen.settings.motionLimit.heading') }}</span>
            <output class="text-sm font-semibold tabular-nums">{{ settings.motionLimitPercent }}%</output>
          </span>
          <span class="mt-3 flex items-center gap-3">
            <input
              v-model.number="settings.motionLimitPercent"
              class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper"
              :aria-label="t('framegen.settings.motionLimit.heading')"
              type="range"
              :min="frameGenLimits.motionLimitPercent.min"
              :max="frameGenLimits.motionLimitPercent.max"
              step="1"
            />
            <input
              v-model.number="settings.motionLimitPercent"
              class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm"
              :aria-label="t('framegen.settings.motionLimit.valueLabel')"
              type="number"
              :min="frameGenLimits.motionLimitPercent.min"
              :max="frameGenLimits.motionLimitPercent.max"
              step="1"
            />
          </span>
          <span class="mt-2 block text-xs leading-5 text-muted">
            {{ t('framegen.settings.motionLimit.description') }}
          </span>
        </label>

        <label v-if="showHeadPrediction" class="flex cursor-pointer items-start gap-3 rounded-[1rem] border p-4 surface-panel lg:col-span-3">
          <input
            v-model="settings.extendHeadPrediction"
            class="mt-1 h-4 w-4 shrink-0 accent-depthxr-copper"
            type="checkbox"
          />
          <span class="min-w-0">
            <span class="flex flex-wrap items-center gap-2">
              <span class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('framegen.settings.headPrediction.heading') }}</span>
              <span class="chip-warning rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em]">
                {{ t('framegen.experimental') }}
              </span>
            </span>
            <span class="mt-2 block text-xs leading-5 text-muted">
              {{ t('framegen.settings.headPrediction.description') }}
            </span>
          </span>
        </label>
      </div>

      <div v-if="!advancedAtDefaults" class="mt-3 flex justify-end">
        <button type="button" class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" @click="resetAdvanced">
          {{ t('framegen.settings.advanced.reset') }}
        </button>
      </div>
    </details>
  </div>
</template>
