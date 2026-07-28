<script setup lang="ts">
import BindingEditor from './BindingEditor.vue'
import { bindingsShareInput, defaultKeyboardBinding, type InputBinding } from '../lib/model'

const props = defineProps<{
  modelValue: InputBinding[]
  label: string
  description: string
  noneText?: string
  soundMode?: 'transition' | 'single'
  defaultActivateSound?: string
  defaultDeactivateSound?: string
}>()

const emit = defineEmits<{
  'update:modelValue': [value: InputBinding[]]
}>()

function removeBinding(index: number) {
  emit('update:modelValue', props.modelValue.filter((_, candidate) => candidate !== index))
}

function updateBinding(index: number, binding: InputBinding) {
  if (binding.type === 'none') {
    removeBinding(index)
    return
  }

  const bindings = [...props.modelValue]
  bindings[index] = binding
  emit('update:modelValue', bindings)
}

function addBinding() {
  const candidates = Array.from({ length: 12 }, (_, index) => `F${index + 1}`)
  const key = candidates.find((candidate) => {
    const binding = defaultKeyboardBinding(candidate)
    return !props.modelValue.some((existing) => bindingsShareInput(existing, binding))
  }) ?? 'F8'

  emit('update:modelValue', [...props.modelValue, defaultKeyboardBinding(key)])
}
</script>

<template>
  <section class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div class="min-w-0 flex-1">
        <p class="text-sm font-semibold tracking-tight">{{ label }}</p>
        <p class="mt-1 text-sm leading-6 text-muted">{{ description }}</p>
        <div v-if="$slots.controls" class="mt-3">
          <slot name="controls" />
        </div>
      </div>
      <button class="button-accent shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="addBinding">
        Add Binding
      </button>
    </div>

    <div v-if="modelValue.length === 0" class="mt-4 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">
      {{ noneText || 'No bindings assigned. This action will not run until a binding is added.' }}
    </div>

    <div v-else class="mt-4 space-y-3">
      <BindingEditor
        v-for="(binding, index) in modelValue"
        :key="index"
        :model-value="binding"
        :label="`Binding ${index + 1}`"
        description="Choose a keyboard shortcut or detected device input."
        removable
        :sound-mode="soundMode"
        :default-activate-sound="defaultActivateSound"
        :default-deactivate-sound="defaultDeactivateSound"
        @remove="removeBinding(index)"
        @update:model-value="updateBinding(index, $event)"
      />
    </div>
  </section>
</template>
