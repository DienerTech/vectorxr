<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'

import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingEditor from './BindingEditor.vue'
import PivotActivationEditor from './PivotActivationEditor.vue'
import { pivotBindingConflictWarnings, savedBindingConflictWarnings, type ActivationMode, type InputBinding, type VectorXRConfig } from '../lib/model'

// The default module config and each pivot profile share this binding shape;
// fields are mutated directly on the passed-in reactive object.
interface PivotBindingsSubject {
  activationMode: ActivationMode
  activationBinding: InputBinding
  setOriginBinding: InputBinding
  releaseOriginBinding: InputBinding
}

const props = defineProps<{
  subject: PivotBindingsSubject
  config: VectorXRConfig
  // Names the profile in the breadcrumb/title, e.g. "Default Profile" or "DCS".
  contextLabel: string
}>()
const bindingWarnings = computed(() => pivotBindingConflictWarnings(
  props.subject.activationMode,
  props.subject.activationBinding,
  props.subject.setOriginBinding,
  props.subject.releaseOriginBinding,
))
const globalBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  props.subject.activationBinding,
  props.subject.setOriginBinding,
  props.subject.releaseOriginBinding,
]))

const emit = defineEmits<{
  close: []
}>()

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') {
    emit('close')
  }
}

onMounted(() => {
  window.addEventListener('keydown', onKeydown)
})

onUnmounted(() => {
  window.removeEventListener('keydown', onKeydown)
})
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
            <span aria-hidden="true">›</span>
            <span class="font-medium" style="color: var(--app-text)">Bindings</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ contextLabel }} — Bindings</h2>
          <p class="mt-1 text-sm text-muted">Press Esc or Done to return to Pivot.</p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          aria-label="Close bindings and return to Pivot"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <div class="mt-5 space-y-4">
        <PivotActivationEditor
          v-model:activation-mode="subject.activationMode"
          v-model:activation-binding="subject.activationBinding"
          description="Choose a keyboard shortcut, joystick button, or HAT direction that engages Pivot."
        />

        <div
          v-for="warning in bindingWarnings"
          :key="warning.title"
          class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
          style="border-color: var(--app-border)"
        >
          <p class="font-medium">{{ warning.title }}</p>
          <p class="mt-1">{{ warning.message }}</p>
        </div>
        <BindingConflictWarnings :warnings="globalBindingWarnings" />
        <BindingEditor
          v-model="subject.setOriginBinding"
          label="Set Origin (optional)"
          description="Captures the current head yaw and pitch as Pivot's neutral forward. Bind this to the same button you use to recenter the view in-game so both origins update together. Until it is pressed, Pivot uses the HMD origin."
          none-text="No binding assigned. Pivot rotates around the HMD origin (the default)."
          sound-mode="single"
          default-activate-sound="origin-set.wav"
        />

        <BindingEditor
          v-model="subject.releaseOriginBinding"
          label="Release Origin (optional)"
          description="Clears a captured origin and returns Pivot to the HMD origin. Useful if an origin was captured while looking off-center."
          none-text="No binding assigned. A captured origin stays active until the session ends or it is recaptured."
          sound-mode="single"
          default-activate-sound="origin-release.wav"
        />
      </div>

      <div class="mt-5 flex justify-end border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-accent rounded-[0.75rem] px-6 py-2.5 text-sm font-medium" type="button" @click="$emit('close')">
          Done
        </button>
      </div>
    </article>
  </div>
</template>
