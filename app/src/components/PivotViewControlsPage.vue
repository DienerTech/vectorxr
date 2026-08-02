<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'

import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingEditor from './BindingEditor.vue'
import {
  createPivotQuickView,
  defaultKeyboardBinding,
  newPivotQuickViewId,
  savedBindingConflictWarnings,
  type InputBinding,
  type PivotActivationBehavior,
  type PivotQuickView,
  type PivotViewControls,
  type VectorXRConfig,
} from '../lib/model'

interface ViewControlsSubject {
  viewControls: PivotViewControls
}

const props = defineProps<{
  subject: ViewControlsSubject
  config: VectorXRConfig
  contextLabel: string
}>()
const emit = defineEmits<{ close: [] }>()
const editingIndex = ref<number | null>(null)
const editingView = computed(() => editingIndex.value === null
  ? null
  : props.subject.viewControls.quickViews[editingIndex.value] ?? null)
const globalWarnings = computed(() => savedBindingConflictWarnings(
  props.config,
  props.subject.viewControls.quickViews.flatMap((view) => view.activationBindings.map((item) => item.binding)),
  { suppressFocusOnlyConflicts: true },
))

function onKeydown(event: KeyboardEvent) {
  if (event.key !== 'Escape') return
  if (editingIndex.value !== null) editingIndex.value = null
  else emit('close')
}
onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))

function addQuickView() {
  props.subject.viewControls.quickViews.push(createPivotQuickView())
  editingIndex.value = props.subject.viewControls.quickViews.length - 1
}

function addPreset() {
  const presets: Array<Pick<PivotQuickView, 'name' | 'yawDegrees' | 'pitchDegrees' | 'turnDirection'>> = [
    { name: 'Look Left', yawDegrees: -90, pitchDegrees: 0, turnDirection: 'left' },
    { name: 'Look Right', yawDegrees: 90, pitchDegrees: 0, turnDirection: 'right' },
    { name: 'High 12', yawDegrees: 0, pitchDegrees: 35, turnDirection: 'right' },
    { name: 'Check Six', yawDegrees: 180, pitchDegrees: 0, turnDirection: 'right' },
  ]
  props.subject.viewControls.quickViews.push(...presets.map((preset) => ({
    ...createPivotQuickView(preset.name),
    ...preset,
  })))
}

function duplicateQuickView(index: number) {
  const source = props.subject.viewControls.quickViews[index]
  if (!source) return
  const copy = JSON.parse(JSON.stringify(source)) as PivotQuickView
  copy.id = newPivotQuickViewId()
  copy.name = `${source.name} Copy`
  props.subject.viewControls.quickViews.splice(index + 1, 0, copy)
}

function removeQuickView(index: number) {
  props.subject.viewControls.quickViews.splice(index, 1)
  if (editingIndex.value === index) editingIndex.value = null
}

function moveQuickView(index: number, direction: -1 | 1) {
  const target = index + direction
  const views = props.subject.viewControls.quickViews
  if (target < 0 || target >= views.length) return
  const [view] = views.splice(index, 1)
  views.splice(target, 0, view)
}

function addQuickViewBinding(view: PivotQuickView) {
  const used = new Set(view.activationBindings.flatMap((item) => item.binding.type === 'keyboard' ? item.binding.chord : []))
  const key = Array.from({ length: 12 }, (_, index) => `F${index + 1}`).find((candidate) => !used.has(candidate)) ?? 'F8'
  view.activationBindings.push({ behavior: 'hold', binding: defaultKeyboardBinding(key) })
}

function updateQuickViewBinding(view: PivotQuickView, index: number, binding: InputBinding) {
  if (binding.type === 'none') {
    view.activationBindings.splice(index, 1)
    return
  }
  view.activationBindings[index] = { ...view.activationBindings[index], binding }
}

function updateQuickViewBehavior(view: PivotQuickView, index: number, behavior: string) {
  const normalized: PivotActivationBehavior = behavior === 'toggle' ? 'toggle' : 'hold'
  view.activationBindings[index] = { ...view.activationBindings[index], behavior: normalized }
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <nav class="flex items-center gap-1.5 text-xs text-muted" aria-label="Breadcrumb">
            <button class="underline-offset-2 hover:underline" type="button" @click="$emit('close')">Pivot</button>
            <span aria-hidden="true">›</span>
            <span>{{ contextLabel }}</span>
            <template v-if="editingView">
              <span aria-hidden="true">›</span>
              <button class="underline-offset-2 hover:underline" type="button" @click="editingIndex = null">Snap Views</button>
              <span aria-hidden="true">›</span>
              <span class="font-medium" style="color: var(--app-text)">{{ editingView.name }}</span>
            </template>
            <template v-else>
              <span aria-hidden="true">›</span>
              <span class="font-medium" style="color: var(--app-text)">Snap Views</span>
            </template>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">
            {{ editingView ? `Edit ${editingView.name}` : `${contextLabel} — Snap Views` }}
          </h2>
          <p class="mt-1 text-sm text-muted">Press Esc or Done to return.</p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          :aria-label="editingView ? 'Return to Snap Views' : 'Return to Pivot'"
          @click="editingView ? editingIndex = null : $emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <template v-if="editingView">
        <div class="mt-5 space-y-4">
          <section class="rounded-[1rem] border p-4 surface-panel-soft">
            <div class="grid gap-4 md:grid-cols-2">
              <label class="block md:col-span-2">
                <span class="mb-1.5 block text-sm font-medium">Name</span>
                <input v-model="editingView.name" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="text" />
              </label>
              <label class="block">
                <span class="mb-1.5 block text-sm font-medium">Yaw</span>
                <div class="flex items-center gap-2"><input v-model.number="editingView.yawDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="-180" max="180" step="1" /><span class="text-sm text-muted">°</span></div>
                <span class="mt-1 block text-xs text-muted">Negative turns left; positive turns right.</span>
              </label>
              <label class="block">
                <span class="mb-1.5 block text-sm font-medium">Pitch</span>
                <div class="flex items-center gap-2"><input v-model.number="editingView.pitchDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="-85" max="85" step="1" /><span class="text-sm text-muted">°</span></div>
              </label>
              <label class="block">
                <span class="mb-1.5 block text-sm font-medium">Transition</span>
                <div class="flex items-center gap-2"><input v-model.number="editingView.transitionSeconds" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="0" max="2" step="0.01" /><span class="text-sm text-muted">s</span></div>
              </label>
              <label v-if="Math.abs(editingView.yawDegrees) === 180" class="block">
                <span class="mb-1.5 block text-sm font-medium">Travel to 180°</span>
                <select v-model="editingView.turnDirection" class="app-input w-full rounded-[0.75rem] px-4 py-2.5"><option value="right">Turn right</option><option value="left">Turn left</option></select>
              </label>
            </div>
            <div class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
              <p class="text-sm font-semibold">Optional position offset</p>
              <p class="mt-1 text-sm text-muted">Position is relative to the captured Pivot origin. Natural head translation remains available.</p>
              <div class="mt-3 grid gap-4 md:grid-cols-3">
                <label class="block"><span class="mb-1.5 block text-sm font-medium">Right / left</span><div class="flex items-center gap-2"><input v-model.number="editingView.positionRightCm" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="-100" max="100" step="1" /><span class="text-sm text-muted">cm</span></div></label>
                <label class="block"><span class="mb-1.5 block text-sm font-medium">Up / down</span><div class="flex items-center gap-2"><input v-model.number="editingView.positionUpCm" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="-100" max="100" step="1" /><span class="text-sm text-muted">cm</span></div></label>
                <label class="block"><span class="mb-1.5 block text-sm font-medium">Forward / back</span><div class="flex items-center gap-2"><input v-model.number="editingView.positionForwardCm" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" type="number" min="-100" max="100" step="1" /><span class="text-sm text-muted">cm</span></div></label>
              </div>
            </div>
          </section>

          <section class="rounded-[1rem] border p-4 surface-panel-soft">
            <div class="sticky top-0 z-10 flex flex-wrap items-start justify-between gap-3 rounded-[0.75rem] border p-3 shadow-panel backdrop-blur surface-panel-strong">
              <div><p class="text-sm font-semibold">Bindings</p><p class="mt-1 text-sm text-muted">Hold returns when released. Toggle stays until pressed again or another Quick View takes over.</p></div>
              <button class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="addQuickViewBinding(editingView)">Add Binding</button>
            </div>
            <div v-if="editingView.activationBindings.length === 0" class="mt-4 rounded-[0.75rem] border border-dashed px-4 py-3 text-sm surface-panel-strong">No bindings assigned. This Quick View is inert until a binding is added.</div>
            <div v-else class="mt-4 space-y-3">
              <BindingEditor
                v-for="(item, index) in editingView.activationBindings"
                :key="index"
                :model-value="item.binding"
                :label="`Binding ${index + 1}`"
                :description="item.behavior === 'hold' ? 'The view is active only while held.' : 'Press once to enter; press again to leave.'"
                removable
                @update:model-value="updateQuickViewBinding(editingView, index, $event)"
                @remove="editingView.activationBindings.splice(index, 1)"
              >
                <template #controls>
                  <label class="block"><span class="mb-1.5 block text-sm font-medium">Behavior</span><select :value="item.behavior" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" @change="updateQuickViewBehavior(editingView, index, ($event.target as HTMLSelectElement).value)"><option value="hold">Hold</option><option value="toggle">Toggle</option></select></label>
                </template>
              </BindingEditor>
            </div>
          </section>
        </div>
      </template>

      <template v-else>
        <div class="mt-5 space-y-5">
          <section style="border-color: var(--app-border)">
            <div class="sticky top-0 z-20 mb-3 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
              <div><h3 class="text-lg font-semibold tracking-tight">Quick Views</h3><p class="mt-1 text-sm text-muted">Named poses relative to the Pivot origin, with 1:1 natural head tracking around the target.</p></div>
              <div class="flex flex-wrap gap-2"><button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="addPreset">Create 4-view preset</button><button class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="addQuickView">Add Quick View</button></div>
            </div>
            <div v-if="subject.viewControls.quickViews.length === 0" class="rounded-[0.9rem] border border-dashed px-5 py-5 text-center text-sm surface-panel-soft">No Quick Views yet. Add one manually or start with the four-view HAT preset.</div>
            <div v-else class="space-y-3">
              <div v-for="(quickView, index) in subject.viewControls.quickViews" :key="quickView.id" class="rounded-[1rem] border p-4 surface-panel-soft">
                <div class="flex flex-wrap items-start justify-between gap-3">
                  <div><p class="font-semibold">{{ quickView.name }}</p><p class="mt-1 text-sm text-muted">Yaw {{ quickView.yawDegrees }}° · Pitch {{ quickView.pitchDegrees }}° · {{ quickView.activationBindings.length }} binding{{ quickView.activationBindings.length === 1 ? '' : 's' }}</p><p v-if="quickView.positionRightCm || quickView.positionUpCm || quickView.positionForwardCm" class="mt-1 text-xs text-muted">Position R {{ quickView.positionRightCm }} · U {{ quickView.positionUpCm }} · F {{ quickView.positionForwardCm }} cm</p></div>
                  <div class="flex flex-wrap gap-2"><button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-sm" type="button" :disabled="index === 0" @click="moveQuickView(index, -1)">Up</button><button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-sm" type="button" :disabled="index === subject.viewControls.quickViews.length - 1" @click="moveQuickView(index, 1)">Down</button><button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-sm" type="button" @click="duplicateQuickView(index)">Duplicate</button><button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-sm" type="button" @click="editingIndex = index">Edit</button><button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-sm" type="button" @click="removeQuickView(index)">Remove</button></div>
                </div>
              </div>
            </div>
          </section>
          <BindingConflictWarnings :warnings="globalWarnings" />
        </div>
      </template>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="editingView ? editingIndex = null : $emit('close')">Done</button>
      </div>
    </article>
  </div>
</template>
