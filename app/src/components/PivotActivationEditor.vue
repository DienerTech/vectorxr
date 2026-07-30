<script setup lang="ts">
import BindingEditor from './BindingEditor.vue'
import { defaultKeyboardBinding, type InputBinding, type PivotActivationBehavior, type PivotActivationBinding } from '../lib/model'

const props = defineProps<{
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
  description: string
}>()

const emit = defineEmits<{
  'update:alwaysActive': [value: boolean]
  'update:activationBindings': [value: PivotActivationBinding[]]
}>()

function addBinding() {
  const usedKeys = new Set(props.activationBindings.flatMap((item) => item.binding.type === 'keyboard' ? item.binding.chord : []))
  const key = Array.from({ length: 12 }, (_, index) => `F${index + 1}`).find((candidate) => !usedKeys.has(candidate)) ?? 'F8'
  emit('update:activationBindings', [...props.activationBindings, { behavior: 'toggle', binding: defaultKeyboardBinding(key) }])
}

function updateBinding(index: number, binding: InputBinding) {
  emit('update:activationBindings', props.activationBindings.map((item, itemIndex) => itemIndex === index ? { ...item, binding } : item))
}

function updateBehavior(index: number, behavior: string) {
  const normalized: PivotActivationBehavior = behavior === 'hold' ? 'hold' : 'toggle'
  emit('update:activationBindings', props.activationBindings.map((item, itemIndex) => itemIndex === index ? { ...item, behavior: normalized } : item))
}

function removeBinding(index: number) {
  emit('update:activationBindings', props.activationBindings.filter((_, itemIndex) => itemIndex !== index))
}
</script>

<template>
  <div class="space-y-4">
    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-center justify-between gap-4">
        <div class="min-w-0">
          <p class="text-sm font-semibold tracking-tight">Profile baseline</p>
          <p class="mt-1 text-sm leading-6 text-muted">
            {{ alwaysActive
              ? 'Pivot starts active. Bindings below suspend it instead of activating it.'
              : 'Pivot starts inactive and is controlled by the activation bindings below.' }}
          </p>
        </div>
        <label class="button-secondary inline-flex shrink-0 cursor-pointer items-center gap-3 rounded-full px-4 py-2.5">
          <input
            class="h-4 w-4 accent-depthxr-copper"
            type="checkbox"
            :checked="alwaysActive"
            @change="$emit('update:alwaysActive', ($event.target as HTMLInputElement).checked)"
          />
          <span class="text-sm font-semibold">Always active</span>
        </label>
      </div>
    </section>

    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="text-sm font-semibold tracking-tight">{{ alwaysActive ? 'Suspend bindings' : 'Activation bindings' }}</p>
          <p class="mt-1 text-sm leading-6 text-muted">{{ description }}</p>
        </div>
        <button class="button-accent shrink-0 rounded-[0.75rem] px-4 py-2.5 text-sm font-medium" type="button" @click="addBinding">
          Add Binding
        </button>
      </div>

      <div v-if="activationBindings.length === 0" class="mt-4 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">
        {{ alwaysActive
          ? 'No suspend bindings assigned. Pivot remains active for the whole session.'
          : 'No activation binding assigned. Pivot cannot be activated until one is added.' }}
      </div>

      <div v-else class="mt-4 space-y-4">
        <BindingEditor
          v-for="(item, index) in activationBindings"
          :key="index"
          :model-value="item.binding"
          :label="`Binding ${index + 1}`"
          :description="alwaysActive
            ? (item.behavior === 'toggle' ? 'Press once to suspend Pivot; press again to resume.' : 'Hold to suspend Pivot; release to resume.')
            : (item.behavior === 'toggle' ? 'Press once to activate Pivot; press again to deactivate.' : 'Pivot stays active only while this control is held.')"
          :none-text="alwaysActive ? 'No suspend control assigned for this slot.' : 'No activation control assigned for this slot.'"
          removable
          @update:model-value="updateBinding(index, $event)"
          @remove="removeBinding(index)"
        >
          <template #controls>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Behavior</span>
              <select
                :value="item.behavior"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                @change="updateBehavior(index, ($event.target as HTMLSelectElement).value)"
              >
                <option value="toggle">Toggle</option>
                <option value="hold">Hold</option>
              </select>
            </label>
          </template>
        </BindingEditor>
      </div>
    </section>
  </div>
</template>
