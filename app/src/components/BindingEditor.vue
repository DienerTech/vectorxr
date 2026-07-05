<script setup lang="ts">
import { computed, ref } from 'vue'

import DeviceBindingEditor from './DeviceBindingEditor.vue'
import { pickSoundFile, playTestSound } from '../lib/commands'
import { defaultDeviceBinding, defaultKeyboardBinding, defaultNoneBinding, defaultSoundFeedback, keyboardBindingKeyGroups, keyboardModifierKeys, type InputBinding, type SoundFeedback } from '../lib/model'

const props = defineProps<{
  modelValue: InputBinding
  label: string
  description: string
  noneText?: string
  // 'single' hides the deactivate sound row for momentary actions (e.g. set
  // origin) that only ever fire an activate cue.
  soundMode?: 'transition' | 'single'
  // Bundled default WAV used when the sound path is empty; lets actions with
  // their own cue (origin set/release) test the right default tone.
  defaultSoundName?: string
}>()

const emit = defineEmits<{
  'update:modelValue': [value: InputBinding]
}>()

type SoundPath = 'activateSound' | 'deactivateSound'

const soundError = ref('')

const sound = computed<SoundFeedback | null>(() => (props.modelValue.type === 'none' ? null : props.modelValue.sound ?? null))

const soundEnabled = computed({
  get: () => sound.value?.enabled ?? false,
  set: (enabled: boolean) => updateSound({ enabled }),
})

function updateSound(patch: Partial<SoundFeedback>) {
  if (props.modelValue.type === 'none') {
    return
  }

  const next: SoundFeedback = { ...(props.modelValue.sound ?? defaultSoundFeedback()), ...patch }
  emit('update:modelValue', { ...props.modelValue, sound: next })
}

function soundFileName(path: string): string {
  const trimmed = path.trim()
  if (!trimmed) {
    return 'Default tone'
  }

  return trimmed.split(/[/\\]/).pop() || trimmed
}

async function browseSound(which: SoundPath) {
  soundError.value = ''
  try {
    const picked = await pickSoundFile()
    if (picked) {
      updateSound({ [which]: picked })
    }
  } catch (error) {
    soundError.value = error instanceof Error ? error.message : 'Failed to open file picker.'
  }
}

async function testSound(which: SoundPath) {
  soundError.value = ''
  try {
    await playTestSound(sound.value?.[which] ?? '', which === 'activateSound', 100, props.defaultSoundName)
  } catch (error) {
    soundError.value = error instanceof Error ? error.message : 'Failed to play sound.'
  }
}

const soundRows = computed(() =>
  props.soundMode === 'single'
    ? [{ key: 'activateSound' as const, label: 'Sound' }]
    : [
        { key: 'activateSound' as const, label: 'Activate' },
        { key: 'deactivateSound' as const, label: 'Deactivate' },
      ],
)

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

    <div v-if="modelValue.type !== 'none'" class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
      <label class="flex items-start gap-2.5">
        <input v-model="soundEnabled" class="mt-0.5 h-4 w-4 accent-depthxr-copper" type="checkbox" />
        <span>
          <span class="block text-sm font-medium">{{ soundMode === 'single' ? 'Play a sound when this action fires' : 'Play a sound on activate / deactivate' }}</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">An extra audible cue for this binding. Leave a slot on the default tone, or choose your own short .wav.</span>
        </span>
      </label>

      <div v-if="soundEnabled" class="mt-3 space-y-2">
        <div
          v-for="row in soundRows"
          :key="row.key"
          class="flex flex-wrap items-center gap-2"
        >
          <span class="w-[88px] shrink-0 text-sm font-medium">{{ row.label }}</span>
          <span class="app-readonly-field min-w-0 flex-1 truncate rounded-[0.75rem] px-3 py-2 text-sm" :title="sound?.[row.key] || 'Default tone'">
            {{ soundFileName(sound?.[row.key] ?? '') }}
          </span>
          <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium" type="button" @click="browseSound(row.key)">
            Browse…
          </button>
          <button
            v-if="sound?.[row.key]"
            class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
            type="button"
            @click="updateSound({ [row.key]: '' })"
          >
            Reset
          </button>
          <button class="button-accent rounded-[0.75rem] px-3 py-2 text-sm font-medium" type="button" @click="testSound(row.key)">
            ▶ Test
          </button>
        </div>

        <p v-if="soundError" class="text-sm chip-warning">{{ soundError }}</p>
      </div>
    </div>
  </div>
</template>
