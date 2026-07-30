<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'

import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingListEditor from './BindingListEditor.vue'
import {
  pivotBindingConflictWarnings,
  savedBindingConflictWarnings,
  type InputBinding,
  type PivotActivationBinding,
  type VectorXRConfig,
} from '../lib/model'

interface PivotOriginSubject {
  alwaysActive: boolean
  activationBindings: PivotActivationBinding[]
  setOriginBindings: InputBinding[]
  releaseOriginBindings: InputBinding[]
}

const props = defineProps<{
  subject: PivotOriginSubject
  config: VectorXRConfig
  contextLabel: string
}>()

const bindingWarnings = computed(() => pivotBindingConflictWarnings(
  props.subject.alwaysActive,
  props.subject.activationBindings,
  props.subject.setOriginBindings,
  props.subject.releaseOriginBindings,
))
const globalBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  ...props.subject.setOriginBindings,
  ...props.subject.releaseOriginBindings,
], {
  suppressFocusOnlyConflicts: true,
}))

const emit = defineEmits<{
  close: []
}>()

function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape') emit('close')
}

onMounted(() => window.addEventListener('keydown', onKeydown))
onUnmounted(() => window.removeEventListener('keydown', onKeydown))
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
            <span class="font-medium" style="color: var(--app-text)">Origin Controls</span>
          </nav>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ contextLabel }} — Origin Controls</h2>
          <p class="mt-1 text-sm text-muted">Keep Pivot's neutral pose aligned with the simulator's recenter position.</p>
        </div>
        <button
          class="button-secondary inline-flex h-9 w-9 items-center justify-center rounded-[0.75rem]"
          type="button"
          aria-label="Close origin controls and return to Pivot"
          @click="$emit('close')"
        >
          <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
            <path d="M5.3 5.3a1 1 0 0 1 1.4 0L10 8.6l3.3-3.3a1 1 0 1 1 1.4 1.4L11.4 10l3.3 3.3a1 1 0 0 1-1.4 1.4L10 11.4l-3.3 3.3a1 1 0 0 1-1.4-1.4L8.6 10 5.3 6.7a1 1 0 0 1 0-1.4Z" />
          </svg>
        </button>
      </div>

      <div class="mt-5 space-y-4">
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

        <BindingListEditor
          v-model="subject.setOriginBindings"
          label="Set Origin (optional)"
          description="Captures the current head pose as Pivot's neutral seated origin. Bind this to the same control you use to recenter the view in-game so both origins update together."
          none-text="No binding assigned. Pivot rotates around the HMD origin."
          sound-mode="single"
          default-activate-sound="origin-set.wav"
        />

        <BindingListEditor
          v-model="subject.releaseOriginBindings"
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
