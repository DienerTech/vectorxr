<script setup lang="ts">
import { computed, ref } from 'vue'
import { defaultTurboExperimental, toolkitInspiredTurboExperimental, type TurboExperimentalSettings } from '../lib/model'
const props = defineProps<{ modelValue: TurboExperimentalSettings }>()
const emit = defineEmits<{ 'update:modelValue': [value: TurboExperimentalSettings] }>()
const customSelected = ref(false)
const mode = computed(() => !props.modelValue.enabled ? 'normal' : !customSelected.value && props.modelValue.waitForSubmit && props.modelValue.sampleAtEntry && props.modelValue.predictionPercent === 100 && props.modelValue.frameLimit === 0 ? 'toolkit' : 'custom')
function select(event: Event) {
  const value = (event.target as HTMLSelectElement).value
  customSelected.value = value === 'custom'
  emit('update:modelValue', value === 'normal' ? defaultTurboExperimental() : value === 'toolkit' ? toolkitInspiredTurboExperimental() : { ...props.modelValue, applicationIds: [], enabled: true })
}
function patch(value: Partial<TurboExperimentalSettings>) { emit('update:modelValue', { ...props.modelValue, ...value, applicationIds: [] }) }
</script>

<template>
  <section class="mt-3 rounded-xl border p-4 surface-panel-soft">
    <label class="block text-sm font-semibold">Profile timing
      <select :value="mode" class="app-input manual-input-select mt-2 w-full rounded-lg px-3 py-2 font-normal" @change="select">
        <option value="normal">Normal Turbo timing</option><option value="toolkit">Toolkit-inspired · Experimental</option><option value="custom">Custom timing · Experimental</option>
      </select>
    </label>
    <p class="mt-2 text-xs text-muted">Applies to this profile’s applications. Save and relaunch the VR application after changing timing.</p>
    <div v-if="modelValue.enabled" class="mt-4 space-y-3 text-sm">
      <label class="flex items-center gap-3"><input :checked="modelValue.waitForSubmit" type="checkbox" @change="patch({ waitForSubmit: ($event.target as HTMLInputElement).checked })" />Wait for an overlapping submission</label>
      <label class="flex items-center gap-3"><input :checked="modelValue.sampleAtEntry" type="checkbox" @change="patch({ sampleAtEntry: ($event.target as HTMLInputElement).checked })" />Sample prediction timing at wait entry</label>
      <div class="grid gap-3 sm:grid-cols-2">
        <label>Prediction (%)<input :value="modelValue.predictionPercent" type="number" min="50" max="100" step="1" class="app-input mt-1 block w-full rounded-lg px-3 py-2" @input="patch({ predictionPercent: Number(($event.target as HTMLInputElement).value) })" /></label>
        <label>Frame cap (FPS; 0 = off)<input :value="modelValue.frameLimit" type="number" min="0" max="240" step="1" class="app-input mt-1 block w-full rounded-lg px-3 py-2" @input="patch({ frameLimit: Number(($event.target as HTMLInputElement).value) })" /></label>
      </div>
      <p class="text-xs text-muted">Experimental profiles use Async pacing. Prediction adjustment requires runtime clock support.</p>
    </div>
  </section>
</template>
