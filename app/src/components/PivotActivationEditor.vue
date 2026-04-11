<script setup lang="ts">
import { computed } from 'vue'

import {
  defaultDeviceBinding,
  defaultKeyboardBinding,
  defaultNoneBinding,
  keyboardBindingKeyGroups,
  keyboardModifierKeys,
  type ActivationMode,
  type InputBinding,
} from '../lib/model'

const props = defineProps<{
  activationMode: ActivationMode
  activationBinding: InputBinding
  description: string
}>()

const emit = defineEmits<{
  'update:activationMode': [value: ActivationMode]
  'update:activationBinding': [value: InputBinding]
}>()

const bindingType = computed({
  get: () => props.activationBinding.type,
  set: (type: InputBinding['type']) => {
    if (type === 'none') {
      emit('update:activationBinding', defaultNoneBinding())
      return
    }

    emit('update:activationBinding', type === 'keyboard' ? defaultKeyboardBinding() : defaultDeviceBinding())
  },
})

const selectedPrimaryKey = computed({
  get: () => (props.activationBinding.type === 'keyboard' ? props.activationBinding.chord.find((key) => !keyboardModifierKeys.includes(key as never)) ?? 'F8' : 'F8'),
  set: (key: string) => {
    if (props.activationBinding.type !== 'keyboard') {
      return
    }

    const modifiers = props.activationBinding.chord.filter((item) => keyboardModifierKeys.includes(item as never))
    emit('update:activationBinding', { type: 'keyboard', chord: [...modifiers, key] })
  },
})

function updateMode(value: string) {
  emit('update:activationMode', value === 'hold' ? 'hold' : 'toggle')
}

function toggleModifier(modifier: string, enabled: boolean) {
  if (props.activationBinding.type !== 'keyboard') {
    return
  }

  const primaryKey = selectedPrimaryKey.value
  const modifiers = props.activationBinding.chord.filter((item) => keyboardModifierKeys.includes(item as never) && item !== modifier)
  if (enabled) {
    modifiers.push(modifier)
  }

  emit('update:activationBinding', { type: 'keyboard', chord: [...modifiers, primaryKey] })
}

function updateDeviceGuid(deviceGuid: string) {
  if (props.activationBinding.type !== 'device') {
    return
  }

  emit('update:activationBinding', { ...props.activationBinding, deviceGuid })
}

function updateInputPath(inputPath: string) {
  if (props.activationBinding.type !== 'device') {
    return
  }

  emit('update:activationBinding', { ...props.activationBinding, inputPath })
}
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <p class="eyebrow text-xs uppercase tracking-[0.18em]">Activation</p>
    <p class="mt-2 text-sm leading-6 text-muted">{{ description }}</p>

    <div class="mt-3 grid gap-3 sm:grid-cols-2 lg:grid-cols-[minmax(140px,180px)_minmax(140px,180px)]">
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

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Binding Type</span>
        <select v-model="bindingType" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
          <option value="none">none</option>
          <option value="keyboard">keyboard</option>
          <option value="device">device</option>
        </select>
      </label>
    </div>

    <div v-if="activationBinding.type === 'keyboard'" class="mt-3 grid gap-3 lg:grid-cols-[minmax(180px,240px)_minmax(0,1fr)]">
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Primary Key</span>
        <select v-model="selectedPrimaryKey" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
          <optgroup v-for="group in keyboardBindingKeyGroups" :key="group.label" :label="group.label">
            <option v-for="option in group.options" :key="option" :value="option">{{ option }}</option>
          </optgroup>
        </select>
      </label>

      <div>
        <span class="mb-1.5 block text-sm font-medium">Modifiers</span>
        <div class="flex flex-wrap gap-2">
          <label v-for="modifier in keyboardModifierKeys" :key="modifier" class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-3 py-2 text-sm">
            <input
              class="h-4 w-4 accent-depthxr-copper"
              type="checkbox"
              :checked="activationBinding.chord.includes(modifier)"
              @change="toggleModifier(modifier, ($event.target as HTMLInputElement).checked)"
            />
            {{ modifier }}
          </label>
        </div>
      </div>
    </div>

    <div v-else-if="activationBinding.type === 'device'" class="mt-3 grid gap-3 lg:grid-cols-2">
      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Windows Device GUID</span>
        <input
          :value="activationBinding.deviceGuid"
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          placeholder="{00000000-0000-0000-0000-000000000000}"
          type="text"
          @input="updateDeviceGuid(($event.target as HTMLInputElement).value)"
        />
      </label>

      <label class="block">
        <span class="mb-1.5 block text-sm font-medium">Input Path</span>
        <input
          :value="activationBinding.inputPath"
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          placeholder="button-1"
          type="text"
          @input="updateInputPath(($event.target as HTMLInputElement).value)"
        />
      </label>
    </div>

    <div v-else class="mt-3 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">
      No binding assigned. Pivot will not activate until a binding is selected.
    </div>
  </div>
</template>
