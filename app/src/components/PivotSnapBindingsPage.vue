<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'

import BindingEditor from './BindingEditor.vue'
import BindingConflictWarnings from './BindingConflictWarnings.vue'
import { t } from '../i18n/index.ts'
import {
  defaultKeyboardBinding,
  savedBindingConflictWarnings,
  type InputBinding,
  type PivotActivationBehavior,
  type PivotQuickView,
  type PivotViewControls,
  type VectorXRConfig,
} from '../lib/model'

const props = defineProps<{
  viewControls: PivotViewControls
  config: VectorXRConfig
  contextLabel: string
}>()
const emit = defineEmits<{ close: [] }>()

const bindings = computed(() => props.viewControls.quickViews.flatMap((view) => view.activationBindings.map((item) => item.binding)))
const warnings = computed(() => savedBindingConflictWarnings(props.config, bindings.value, { suppressFocusOnlyConflicts: true }))

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close')
}
onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))

function addBinding(view: PivotQuickView) {
  const used = new Set(view.activationBindings.flatMap((item) => item.binding.type === 'keyboard' ? item.binding.chord : []))
  const key = Array.from({ length: 12 }, (_, index) => `F${index + 1}`).find((candidate) => !used.has(candidate)) ?? 'F8'
  view.activationBindings.push({ behavior: 'hold', binding: defaultKeyboardBinding(key) })
}

function updateBinding(view: PivotQuickView, index: number, binding: InputBinding) {
  if (binding.type === 'none') view.activationBindings.splice(index, 1)
  else view.activationBindings[index] = { ...view.activationBindings[index], binding }
}

function updateBehavior(view: PivotQuickView, index: number, behavior: string) {
  const normalized: PivotActivationBehavior = behavior === 'toggle' ? 'toggle' : 'hold'
  view.activationBindings[index] = { ...view.activationBindings[index], behavior: normalized }
}
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-start justify-between gap-4">
      <div>
        <nav class="flex items-center gap-1.5 text-xs text-muted" :aria-label="t('pivotViews.shared.breadcrumb')">
          <button class="underline-offset-2 hover:underline" type="button" @click="emit('close')">{{ t('pivotViews.shared.pivot') }}</button>
          <span aria-hidden="true">›</span>
          <span>{{ contextLabel }}</span>
          <span aria-hidden="true">›</span>
          <span class="font-medium" style="color: var(--app-text)">{{ t('pivotViews.snapBindingsPage.snapViewBindings') }}</span>
        </nav>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('pivotViews.snapBindingsPage.title', { context: contextLabel }) }}</h2>
        <p class="mt-1 text-sm text-muted">{{ t('pivotViews.snapBindingsPage.hint') }}</p>
      </div>
      <button class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]" type="button" :aria-label="t('pivotViews.shared.returnToPivot')" @click="emit('close')">×</button>
    </div>

    <div class="mt-5 space-y-4">
      <section v-for="view in viewControls.quickViews" :key="view.id" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <h3 class="font-semibold">{{ view.name }}</h3>
            <p class="mt-1 text-sm text-muted">{{ t('pivotViews.shared.bindingCount', { count: view.activationBindings.length }) }}</p>
          </div>
          <button class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="addBinding(view)">{{ t('pivotViews.shared.addBinding') }}</button>
        </div>
        <div v-if="view.activationBindings.length === 0" class="mt-3 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">{{ t('pivotViews.snapBindingsPage.noBinding') }}</div>
        <div v-else class="mt-3 space-y-3">
          <BindingEditor
            v-for="(item, index) in view.activationBindings"
            :key="index"
            :model-value="item.binding"
            :label="t('pivotViews.shared.bindingLabel', { index: index + 1 })"
            :description="item.behavior === 'hold' ? t('pivotViews.shared.holdDescription') : t('pivotViews.shared.toggleDescription')"
            removable
            @update:model-value="updateBinding(view, index, $event)"
            @remove="view.activationBindings.splice(index, 1)"
          >
            <template #controls>
              <label class="block">
                <span class="mb-1.5 block text-sm font-medium">{{ t('pivotViews.shared.behavior') }}</span>
                <select :value="item.behavior" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" @change="updateBehavior(view, index, ($event.target as HTMLSelectElement).value)">
                  <option value="hold">{{ t('pivotViews.shared.hold') }}</option>
                  <option value="toggle">{{ t('pivotViews.shared.toggle') }}</option>
                </select>
              </label>
            </template>
          </BindingEditor>
        </div>
      </section>
      <div v-if="viewControls.quickViews.length === 0" class="rounded-[1rem] border border-dashed px-5 py-6 text-center text-sm surface-panel-soft">{{ t('pivotViews.snapBindingsPage.empty') }}</div>
      <BindingConflictWarnings :warnings="warnings" />
    </div>

    <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
      <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="emit('close')">{{ t('common.done') }}</button>
    </div>
  </article>
</template>