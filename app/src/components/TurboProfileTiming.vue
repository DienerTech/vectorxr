<script setup lang="ts">
import { computed, ref } from 'vue'
import { t, type MessageKey } from '../i18n/index.ts'
import { defaultTurboExperimental, toolkitInspiredTurboExperimental, type TurboExperimentalSettings } from '../lib/model'
const props = defineProps<{ modelValue: TurboExperimentalSettings; pacingLabel?: string }>()
const emit = defineEmits<{ 'update:modelValue': [value: TurboExperimentalSettings] }>()
type Strategy = 'normal' | 'toolkit' | 'custom'
const customSelected = ref(false)
const mode = computed<Strategy>(() => !props.modelValue.enabled ? 'normal' : !customSelected.value && props.modelValue.waitForSubmit && props.modelValue.sampleAtEntry && props.modelValue.predictionPercent === 100 && props.modelValue.frameLimit === 0 ? 'toolkit' : 'custom')
const strategies: { value: Strategy; labelKey: MessageKey; relaunch: boolean }[] = [
  { value: 'normal', labelKey: 'turbo.strategy.vectorxr', relaunch: false },
  { value: 'toolkit', labelKey: 'turbo.strategy.toolkit', relaunch: true },
  { value: 'custom', labelKey: 'turbo.strategy.custom', relaunch: true },
]
const description = computed(() => mode.value === 'normal'
  ? t('turbo.profileTiming.descriptionVectorxr', { pacing: props.pacingLabel ?? t('turbo.pacingMode.auto') })
  : mode.value === 'toolkit'
    ? t('turbo.profileTiming.descriptionToolkit')
    : t('turbo.profileTiming.descriptionCustom'))
const advancedSummary = computed(() => [
  props.modelValue.waitForSubmit ? t('turbo.profileTiming.summary.waitForSubmission') : t('turbo.profileTiming.summary.overlapSubmission'),
  props.modelValue.sampleAtEntry ? t('turbo.profileTiming.summary.sampleAtEntry') : t('turbo.profileTiming.summary.sampleAtReturn'),
  t('turbo.profileTiming.summary.prediction', { percent: props.modelValue.predictionPercent }),
  props.modelValue.frameLimit ? t('turbo.profileTiming.summary.fpsCap', { fps: props.modelValue.frameLimit }) : t('turbo.profileTiming.summary.noCap'),
].join(' · '))
function select(value: Strategy) {
  customSelected.value = value === 'custom'
  emit('update:modelValue', value === 'normal' ? defaultTurboExperimental() : value === 'toolkit' ? toolkitInspiredTurboExperimental() : { ...props.modelValue, applicationIds: [], enabled: true })
}
function patch(value: Partial<TurboExperimentalSettings>) { emit('update:modelValue', { ...props.modelValue, ...value, applicationIds: [] }) }
</script>

<template>
  <section class="mt-3 rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <p class="text-sm font-semibold tracking-tight">{{ t('turbo.profileTiming.strategy') }}</p>
      <div class="segmented" role="radiogroup" :aria-label="t('turbo.profileTiming.strategyAria')">
        <button v-for="item in strategies" :key="item.value" type="button" role="radio" class="segmented-option inline-flex items-center gap-1.5" :class="{ 'segmented-option-active': mode === item.value }" :aria-checked="mode === item.value" @click="select(item.value)">
          {{ t(item.labelKey) }}<span v-if="item.relaunch" class="restart-required-mark" :title="t('turbo.profileTiming.restartTitle')">&#8635;</span>
        </button>
      </div>
    </div>
    <p class="mt-2 text-sm leading-6 text-muted">{{ description }}</p>
    <p class="mt-1 flex items-center gap-1.5 text-xs text-muted"><span class="restart-required-mark" aria-hidden="true">&#8635;</span>{{ t('turbo.profileTiming.restartNote') }}</p>

    <details v-if="modelValue.enabled" class="section-disclosure mt-3 border-t pt-3" style="border-color: var(--app-border)" :open="mode === 'custom'">
      <summary class="flex flex-wrap items-center gap-2">
        <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
        <span class="text-sm font-medium">{{ t('turbo.profileTiming.advancedTitle') }}</span>
        <span class="text-xs text-muted">{{ advancedSummary }}</span>
      </summary>
      <div class="mt-3 space-y-3 text-sm">
        <p class="text-xs text-muted">{{ t('turbo.profileTiming.advancedIntro') }}</p>
        <div class="grid gap-2 sm:grid-cols-2">
          <label class="flex items-center gap-2.5"><input :checked="modelValue.waitForSubmit" type="checkbox" class="accent-depthxr-copper" @change="patch({ waitForSubmit: ($event.target as HTMLInputElement).checked })" />{{ t('turbo.profileTiming.waitForOverlap') }}</label>
          <label class="flex items-center gap-2.5"><input :checked="modelValue.sampleAtEntry" type="checkbox" class="accent-depthxr-copper" @change="patch({ sampleAtEntry: ($event.target as HTMLInputElement).checked })" />{{ t('turbo.profileTiming.sampleAtWaitEntry') }}</label>
        </div>
        <div class="grid gap-3 sm:grid-cols-2">
          <label class="text-xs text-muted">{{ t('turbo.profileTiming.predictionLabel') }}<input :value="modelValue.predictionPercent" type="number" min="50" max="100" step="1" class="app-input mt-1 block w-full rounded-[0.75rem] px-3 py-1.5 text-sm" @input="patch({ predictionPercent: Number(($event.target as HTMLInputElement).value) })" /></label>
          <label class="text-xs text-muted">{{ t('turbo.profileTiming.frameCapLabel') }}<input :value="modelValue.frameLimit" type="number" min="0" max="240" step="1" class="app-input mt-1 block w-full rounded-[0.75rem] px-3 py-1.5 text-sm" @input="patch({ frameLimit: Number(($event.target as HTMLInputElement).value) })" /></label>
        </div>
        <p class="text-xs text-muted">{{ t('turbo.profileTiming.changeNote') }}</p>
      </div>
    </details>
  </section>
</template>
