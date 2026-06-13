<script setup lang="ts">
import { computed } from 'vue'

import DeviceBindingEditor from './DeviceBindingEditor.vue'
import { defaultDeviceBinding, defaultKeyboardBinding, defaultNoneBinding, keyboardBindingKeyGroups, keyboardModifierKeys, type InputBinding } from '../lib/model'

const props = defineProps<{
  modelValue: InputBinding
  label: string
  description: string
  noneText?: string
}>()

const emit = defineEmits<{
  'update:modelValue': [value: InputBinding]
}>()

const bindingType = computed({
  get: () => props.modelValue.type,
  set: (type: InputBinding['type']) => {
    if (type === 'none') {
      emit('update:modelValue', defaultNoneBinding())
      return
    }

    emit('update:modelValue', type === 'keyboard' ? defaultKeyboardBinding() : defaultDeviceBinding())
  },
})

const selectedPrimaryKey = computed({
  get: () => (props.modelValue.type === 'keyboard' ? props.modelValue.chord.find((key) => !keyboardModifierKeys.includes(key as never)) ?? 'F8' : 'F8'),
  set: (key: string) => {
    if (props.modelValue.type !== 'keyboard') {
      return
    }

    const modifiers = props.modelValue.chord.filter((item) => keyboardModifierKeys.includes(item as never))
    emit('update:modelValue', { type: 'keyboard', chord: [...modifiers, key] })
  },
})

function toggleModifier(modifier: string, enabled: boolean) {
  if (props.modelValue.type !== 'keyboard') {
    return
  }

  const primaryKey = selectedPrimaryKey.value
  const modifiers = props.modelValue.chord.filter((item) => keyboardModifierKeys.includes(item as never) && item !== modifier)
  if (enabled) {
    modifiers.push(modifier)
  }

  emit('update:modelValue', { type: 'keyboard', chord: [...modifiers, primaryKey] })
}

</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div>
      <p class="text-sm font-semibold tracking-tight">{{ label }}</p>
      <p class="mt-1 text-sm leading-6 text-muted">{{ description }}</p>
    </div>

    <div class="mt-4 grid gap-3 sm:grid-cols-[repeat(auto-fit,minmax(150px,180px))]">
      <slot name="controls" />

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Binding Type</span>
        <select v-model="bindingType" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
          <option value="none">None</option>
          <option value="keyboard">Keyboard</option>
          <option value="device">Device</option>
        </select>
      </label>
    </div>

    <div v-if="modelValue.type === 'none'" class="mt-4 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">
      {{ noneText || 'No binding assigned. This action will not run until a binding is selected.' }}
    </div>

    <div v-else-if="modelValue.type === 'keyboard'" class="mt-4 flex flex-wrap items-start gap-3">
      <div class="min-w-[240px]">
        <span class="mb-1.5 block text-sm font-medium">Modifiers</span>
        <div class="flex flex-wrap gap-2">
          <label v-for="modifier in keyboardModifierKeys" :key="modifier" class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-3 py-2 text-sm">
            <input
              class="h-4 w-4 accent-depthxr-copper"
              type="checkbox"
              :checked="modelValue.chord.includes(modifier)"
              @change="toggleModifier(modifier, ($event.target as HTMLInputElement).checked)"
            />
            {{ modifier }}
          </label>
        </div>
      </div>

      <label class="block w-full sm:w-[220px]">
        <span class="mb-1.5 block text-sm font-medium">Primary Key</span>
        <select v-model="selectedPrimaryKey" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
          <optgroup v-for="group in keyboardBindingKeyGroups" :key="group.label" :label="group.label">
            <option v-for="option in group.options" :key="option" :value="option">{{ option }}</option>
          </optgroup>
        </select>
      </label>
    </div>

    <DeviceBindingEditor v-else :model-value="modelValue" @update:model-value="$emit('update:modelValue', $event)" />
  </div>
</template>
