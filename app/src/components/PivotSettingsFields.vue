<script setup lang="ts">
import type { PivotAxisTuning, PivotXRSettings } from '../lib/model'
import PivotSteppedSettings from './PivotSteppedSettings.vue'

// Shared Pivot settings editor used by both the Default Profile and each custom
// profile, so the two stay visually and structurally identical. Fields mutate
// the passed-in reactive `settings` object directly.
//
// Layout: the General card always spans the full width with its fields side by
// side; the rows below adapt to the response mode so no grid cell is ever left
// empty.
const props = defineProps<{
  settings: PivotXRSettings
}>()

const advancedDirections = [
  { key: 'yawLeft', label: 'Yaw Left', hint: 'Applied when turning your head to the left.', deadzoneMax: 180 },
  { key: 'yawRight', label: 'Yaw Right', hint: 'Applied when turning your head to the right.', deadzoneMax: 180 },
  { key: 'pitchUp', label: 'Pitch Up', hint: 'Applied when tilting your head up.', deadzoneMax: 90 },
  { key: 'pitchDown', label: 'Pitch Down', hint: 'Applied when tilting your head down.', deadzoneMax: 90 },
] as const

function axisTuning(key: (typeof advancedDirections)[number]['key']): PivotAxisTuning {
  return props.settings[key]
}

// Switching advanced axes on seeds each direction from the current symmetric
// values, so tuning starts from what already feels right.
function onAdvancedAxesChange(enabled: boolean) {
  if (!enabled) {
    return
  }

  const yaw: PivotAxisTuning = {
    rotationMultiplier: props.settings.rotationMultiplier,
    deadzoneDegrees: props.settings.deadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraYawDegrees,
  }
  const pitch: PivotAxisTuning = {
    rotationMultiplier: props.settings.pitchRotationMultiplier,
    deadzoneDegrees: props.settings.pitchDeadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraPitchDegrees,
  }
  props.settings.yawLeft = { ...yaw }
  props.settings.yawRight = { ...yaw }
  props.settings.pitchUp = { ...pitch }
  props.settings.pitchDown = { ...pitch }
  props.settings.yawLeftStep = { ...props.settings.yawStep }
  props.settings.yawRightStep = { ...props.settings.yawStep }
  props.settings.pitchUpStep = { ...props.settings.pitchStep }
  props.settings.pitchDownStep = { ...props.settings.pitchStep }
}
</script>

<template>
  <div class="space-y-3">
    <!-- General: applies to both axes -->
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">General</p>
      <div class="mt-3 grid gap-3" :class="settings.responseMode === 'continuous' ? 'sm:grid-cols-3' : 'sm:grid-cols-2'">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Response Mode
            <span
              title="Continuous scales your head rotation smoothly with a multiplier. Stepped adds fixed chunks of rotation as your head crosses angle thresholds, like XRNeckSafer's stepped mode."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select v-model="settings.responseMode" class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5">
            <option value="continuous">continuous</option>
            <option value="stepped">stepped</option>
          </select>
        </label>
        <label v-if="settings.responseMode === 'continuous'" class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Smoothing
            <span
              title="Low-pass filter on continuous pivot response for both yaw and pitch. 0 = instant, higher = smoother but laggier. Good range: 0.05–0.2."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.smoothing"
            class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="1"
            step="0.01"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Activation Ramp
            <span
              title="Seconds to ease the pivot effect in and out when you toggle it on or off, so activating it never snaps the view. 0 = instant. Default 0.35."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.activationRampSeconds"
            class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="2"
            step="0.05"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">seconds</span>
        </label>
      </div>
    </div>

    <!-- Stepped response: independent yaw/pitch or per-direction tuning -->
    <PivotSteppedSettings v-if="settings.responseMode === 'stepped'" :settings="settings" />

    <div v-if="settings.responseMode === 'continuous'" class="rounded-[1rem] border p-4 surface-panel-soft">
      <label class="flex items-start gap-2.5">
        <input
          v-model="settings.advancedAxes"
          class="mt-0.5 h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="onAdvancedAxesChange(($event.target as HTMLInputElement).checked)"
        />
        <span>
          <span class="block text-sm font-medium">Advanced axes &mdash; tune each direction independently</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            Separate multiplier, deadzone, and cap for looking left, right, up, and down. Enabling this starts each direction from your current yaw/pitch values.
          </span>
        </span>
      </label>
    </div>

    <!-- Yaw + Pitch (symmetric continuous, or stepped limits) -->
    <div v-if="settings.responseMode === 'continuous' && !settings.advancedAxes" class="grid gap-3 md:grid-cols-2">
      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Yaw</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-3">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Multiplier
              <span
                title="Amplifies head yaw (left/right) beyond the deadzone. 1.0 = no change. 1.3–2.0 is typical; up to 3 helps look further or behind you depending on your headset and head-tracking range."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.rotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1"
              max="3"
              step="0.05"
              type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Deadzone
              <span
                title="Degrees of head yaw ignored before amplification kicks in. Prevents jitter when looking straight ahead. 2–8° is typical."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.deadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">degrees</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Max Extra
              <span
                title="Cap on the extra yaw degrees pivot can add on top of natural head movement. 180° lets you look directly behind you."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.maxExtraYawDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">degrees</span>
          </label>
        </div>
      </div>

      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">Pitch</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-3">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Multiplier
              <span
                title="Amplifies head pitch (up/down) beyond the deadzone. 1.0 = no change. 1.2–2.0 is typical; up to 3 helps look over the nose or up at the canopy."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.pitchRotationMultiplier"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="1"
              max="3"
              step="0.05"
              type="number"
            />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Deadzone
              <span
                title="Degrees of head pitch ignored before amplification kicks in. 2–12° is typical."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.pitchDeadzoneDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="90"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">degrees</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Max Extra
              <span
                title="Cap on the extra pitch degrees pivot can add. Useful for looking over the nose or up at the canopy without craning your neck."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.maxExtraPitchDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">degrees</span>
          </label>
        </div>
      </div>
    </div>

    <!-- Advanced per-direction axes (continuous mode only) -->
    <div v-if="settings.responseMode === 'continuous' && settings.advancedAxes" class="rounded-[1rem] border p-4 surface-panel-soft">
      <label class="hidden">
        <input
          v-model="settings.advancedAxes"
          class="mt-0.5 h-4 w-4 accent-depthxr-copper"
          type="checkbox"
          @change="onAdvancedAxesChange(($event.target as HTMLInputElement).checked)"
        />
        <span>
          <span class="block text-sm font-medium">Advanced axes — tune each direction independently</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            Separate multiplier, deadzone, and cap for looking left, right, up, and down. Enabling this starts each direction from your current yaw/pitch values.
          </span>
        </span>
      </label>

      <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
        <div v-for="direction in advancedDirections" :key="direction.key" class="rounded-[0.9rem] border p-3 surface-panel">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]" :title="direction.hint">{{ direction.label }}</p>
          <div class="mt-3 grid gap-3">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Multiplier
                <span
                  title="How much head rotation is amplified in this direction after the deadzone. 1.0 adds no extra rotation."
                  class="cursor-help select-none text-xs text-muted"
                  >ⓘ</span
                >
              </span>
              <input
                v-model.number="axisTuning(direction.key).rotationMultiplier"
                class="app-input w-full rounded-[0.75rem] px-3 py-2"
                min="1"
                max="3"
                step="0.05"
                type="number"
              />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Deadzone
                <span
                  title="Head rotation in this direction that is ignored before Motion starts adding rotation."
                  class="cursor-help select-none text-xs text-muted"
                  >ⓘ</span
                >
              </span>
              <input
                v-model.number="axisTuning(direction.key).deadzoneDegrees"
                class="app-input w-full rounded-[0.75rem] px-3 py-2"
                min="0"
                :max="direction.deadzoneMax"
                step="0.5"
                type="number"
              />
              <span class="mt-1 block text-xs text-muted">degrees</span>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Max Extra
                <span
                  title="The most extra rotation Motion can add in this direction."
                  class="cursor-help select-none text-xs text-muted"
                  >ⓘ</span
                >
              </span>
              <input
                v-model.number="axisTuning(direction.key).maxExtraDegrees"
                class="app-input w-full rounded-[0.75rem] px-3 py-2"
                min="0"
                max="180"
                step="0.5"
                type="number"
              />
              <span class="mt-1 block text-xs text-muted">degrees</span>
            </label>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
