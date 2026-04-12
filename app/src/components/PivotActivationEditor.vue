<script setup lang="ts">
import BindingEditor from './BindingEditor.vue'
import type { ActivationMode, InputBinding } from '../lib/model'

defineProps<{
  activationMode: ActivationMode
  activationBinding: InputBinding
  description: string
}>()

const emit = defineEmits<{
  'update:activationMode': [value: ActivationMode]
  'update:activationBinding': [value: InputBinding]
}>()

function updateMode(value: string) {
  emit('update:activationMode', value === 'hold' ? 'hold' : 'toggle')
}

</script>

<template>
  <BindingEditor
    :model-value="activationBinding"
    label="Activation"
    :description="description"
    none-text="No binding assigned. Pivot will not activate until a binding is selected."
    @update:model-value="$emit('update:activationBinding', $event)"
  >
    <template #controls>
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Activation Mode</span>
        <select
          :value="activationMode"
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          title="Toggle flips Pivot on each binding press. Hold only applies Pivot while the binding is held down."
          @change="updateMode(($event.target as HTMLSelectElement).value)"
        >
          <option value="toggle">toggle</option>
          <option value="hold">hold</option>
        </select>
      </label>
    </template>
  </BindingEditor>
</template>
