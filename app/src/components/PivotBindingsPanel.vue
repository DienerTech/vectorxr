<script setup lang="ts">
import { ref } from 'vue'

import BindingEditor from './BindingEditor.vue'
import PivotActivationEditor from './PivotActivationEditor.vue'
import type { ActivationMode, InputBinding } from '../lib/model'
import { bindingLabel } from '../lib/model'

defineProps<{
  activationMode: ActivationMode
  activationBinding: InputBinding
  setOriginBinding: InputBinding
  releaseOriginBinding: InputBinding
  // Names the profile in the dialog header, e.g. "Default Profile" or "DCS".
  contextLabel: string
}>()

defineEmits<{
  'update:activationMode': [value: ActivationMode]
  'update:activationBinding': [value: InputBinding]
  'update:setOriginBinding': [value: InputBinding]
  'update:releaseOriginBinding': [value: InputBinding]
}>()

const dialogOpen = ref(false)
</script>

<template>
  <div class="rounded-[1rem] border p-4 surface-panel-soft">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div class="flex min-w-0 flex-wrap items-center gap-2">
        <p class="shrink-0 text-sm font-semibold tracking-tight">Bindings</p>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs font-medium"
          style="border-color: var(--app-border)"
          :title="activationMode === 'alwaysOn'
            ? 'Pivot engages automatically; this binding (optional) suspends and resumes it.'
            : `Pivot ${activationMode === 'toggle' ? 'toggles on each press of' : 'applies while holding'} this binding.`"
        >
          <span class="text-muted">{{ activationMode === 'toggle' ? 'Toggle' : activationMode === 'hold' ? 'Hold' : 'Always On' }}</span>
          {{ bindingLabel(activationBinding) }}
        </span>
        <span
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs font-medium"
          :class="setOriginBinding.type === 'none' ? 'text-muted' : ''"
          style="border-color: var(--app-border)"
          title="Captures the current head yaw/pitch as Pivot's neutral forward. Bind it to the same button as the game's recenter so both origins stay aligned."
        >
          <span class="text-muted">Set Origin</span>
          {{ bindingLabel(setOriginBinding) }}
        </span>
        <span
          v-if="releaseOriginBinding.type !== 'none'"
          class="inline-flex items-center gap-1.5 rounded-full border px-3 py-1 text-xs font-medium"
          style="border-color: var(--app-border)"
          title="Releases a captured origin and returns Pivot to the default HMD origin."
        >
          <span class="text-muted">Release Origin</span>
          {{ bindingLabel(releaseOriginBinding) }}
        </span>
      </div>

      <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="dialogOpen = true">
        Edit Bindings…
      </button>
    </div>

    <div v-if="dialogOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="max-h-[calc(100vh-3rem)] w-full max-w-[860px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Pivot Bindings</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ contextLabel }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="dialogOpen = false">
            Done
          </button>
        </div>

        <div class="mt-5 space-y-4">
          <PivotActivationEditor
            :activation-mode="activationMode"
            :activation-binding="activationBinding"
            description="Choose a keyboard shortcut or capture a joystick button that engages Pivot."
            @update:activation-mode="$emit('update:activationMode', $event)"
            @update:activation-binding="$emit('update:activationBinding', $event)"
          />

          <BindingEditor
            :model-value="setOriginBinding"
            label="Set Origin (optional)"
            description="Captures the current head yaw and pitch as Pivot's neutral forward. Bind this to the same button you use to recenter the view in-game so both origins update together. Until it is pressed, Pivot uses the HMD origin."
            none-text="No binding assigned. Pivot rotates around the HMD origin (the default)."
            sound-mode="single"
            default-sound-name="origin-set.wav"
            @update:model-value="$emit('update:setOriginBinding', $event)"
          />

          <BindingEditor
            :model-value="releaseOriginBinding"
            label="Release Origin (optional)"
            description="Clears a captured origin and returns Pivot to the HMD origin. Useful if an origin was captured while looking off-center."
            none-text="No binding assigned. A captured origin stays active until the session ends or it is recaptured."
            sound-mode="single"
            default-sound-name="origin-release.wav"
            @update:model-value="$emit('update:releaseOriginBinding', $event)"
          />
        </div>
      </div>
    </div>
  </div>
</template>
