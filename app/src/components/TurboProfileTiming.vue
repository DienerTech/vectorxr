<script setup lang="ts">
import { computed, ref } from 'vue'
import { defaultTurboExperimental, toolkitInspiredTurboExperimental, type TurboExperimentalSettings } from '../lib/model'
const props = defineProps<{ modelValue: TurboExperimentalSettings; pacingLabel?: string }>()
const emit = defineEmits<{ 'update:modelValue': [value: TurboExperimentalSettings] }>()
type Strategy = 'normal' | 'toolkit' | 'custom'
const customSelected = ref(false)
const mode = computed<Strategy>(() => !props.modelValue.enabled ? 'normal' : !customSelected.value && props.modelValue.waitForSubmit && props.modelValue.sampleAtEntry && props.modelValue.predictionPercent === 100 && props.modelValue.frameLimit === 0 ? 'toolkit' : 'custom')
const strategies: { value: Strategy; label: string; relaunch: boolean }[] = [
  { value: 'normal', label: 'VectorXR', relaunch: false },
  { value: 'toolkit', label: 'Toolkit-style', relaunch: true },
  { value: 'custom', label: 'Custom', relaunch: true },
]
const description = computed(() => mode.value === 'normal'
  ? `VectorXR's own Turbo. Follows your Runtime Behavior setting (${props.pacingLabel ?? 'Auto · stability-first'}) and can be switched on and off in-game.`
  : mode.value === 'toolkit'
    ? 'Recreates the timing of OpenXR Toolkit’s Turbo Mode. Experimental; restart the game after changing.'
    : 'Toolkit-style with your own adjustments. Experimental; restart the game after changing.')
const advancedSummary = computed(() => [
  props.modelValue.waitForSubmit ? 'Wait for submission' : 'Overlap submission',
  props.modelValue.sampleAtEntry ? 'Sample at entry' : 'Sample at return',
  `${props.modelValue.predictionPercent}% prediction`,
  props.modelValue.frameLimit ? `${props.modelValue.frameLimit} fps cap` : 'No cap',
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
      <p class="text-sm font-semibold tracking-tight">Strategy</p>
      <div class="segmented" role="radiogroup" aria-label="Turbo strategy for this profile">
        <button v-for="item in strategies" :key="item.value" type="button" role="radio" class="segmented-option inline-flex items-center gap-1.5" :class="{ 'segmented-option-active': mode === item.value }" :aria-checked="mode === item.value" @click="select(item.value)">
          {{ item.label }}<span v-if="item.relaunch" class="restart-required-mark" title="Switching to or from this strategy takes effect after Save and a game restart.">&#8635;</span>
        </button>
      </div>
    </div>
    <p class="mt-2 text-sm leading-6 text-muted">{{ description }}</p>
    <p class="mt-1 flex items-center gap-1.5 text-xs text-muted"><span class="restart-required-mark" aria-hidden="true">&#8635;</span>Switching to or from a marked strategy takes effect after Save and a game restart.</p>

    <details v-if="modelValue.enabled" class="section-disclosure mt-3 border-t pt-3" style="border-color: var(--app-border)" :open="mode === 'custom'">
      <summary class="flex flex-wrap items-center gap-2">
        <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor"><path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" /></svg>
        <span class="text-sm font-medium">Advanced timing</span>
        <span class="text-xs text-muted">{{ advancedSummary }}</span>
      </summary>
      <div class="mt-3 space-y-3 text-sm">
        <p class="text-xs text-muted">For experimentation. Most players should leave these alone.</p>
        <div class="grid gap-2 sm:grid-cols-2">
          <label class="flex items-center gap-2.5"><input :checked="modelValue.waitForSubmit" type="checkbox" class="accent-depthxr-copper" @change="patch({ waitForSubmit: ($event.target as HTMLInputElement).checked })" />Wait for an overlapping submission</label>
          <label class="flex items-center gap-2.5"><input :checked="modelValue.sampleAtEntry" type="checkbox" class="accent-depthxr-copper" @change="patch({ sampleAtEntry: ($event.target as HTMLInputElement).checked })" />Sample prediction timing at wait entry</label>
        </div>
        <div class="grid gap-3 sm:grid-cols-2">
          <label class="text-xs text-muted">Prediction (%)<input :value="modelValue.predictionPercent" type="number" min="50" max="100" step="1" class="app-input mt-1 block w-full rounded-[0.75rem] px-3 py-1.5 text-sm" @input="patch({ predictionPercent: Number(($event.target as HTMLInputElement).value) })" /></label>
          <label class="text-xs text-muted">Frame cap (fps; 0 = off)<input :value="modelValue.frameLimit" type="number" min="0" max="240" step="1" class="app-input mt-1 block w-full rounded-[0.75rem] px-3 py-1.5 text-sm" @input="patch({ frameLimit: Number(($event.target as HTMLInputElement).value) })" /></label>
        </div>
        <p class="text-xs text-muted">Changing any value switches the profile to Custom. Prediction adjustment requires runtime clock support.</p>
      </div>
    </details>
  </section>
</template>
