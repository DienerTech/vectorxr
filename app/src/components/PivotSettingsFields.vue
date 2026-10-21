<script setup lang="ts">
import { t } from '../i18n/index.ts'
import type { PivotAxisTuning, PivotSnapStackingStatus, PivotXRSettings } from '../lib/model'
import PivotSteppedSettings from './PivotSteppedSettings.vue'
import PivotPositionAssistEditor from './PivotPositionAssistEditor.vue'
import PivotSnapStackingControl from './PivotSnapStackingControl.vue'

// Shared Pivot settings editor used by both the Default Profile and each custom
// profile, so the two stay visually and structurally identical. Fields mutate
// the passed-in reactive `settings` object directly.
//
// Layout: the General card always spans the full width with its fields side by
// side; the rows below adapt to the response mode so no grid cell is ever left
// empty.
const props = defineProps<{
  settings: PivotXRSettings
  snapStacking: PivotSnapStackingStatus
}>()

const advancedDirections = [
  { key: 'yawLeft', labelKey: 'pivot.directions.yawLeft', hintKey: 'pivot.fields.yawLeftHint', deadzoneMax: 180 },
  { key: 'yawRight', labelKey: 'pivot.directions.yawRight', hintKey: 'pivot.fields.yawRightHint', deadzoneMax: 180 },
  { key: 'pitchUp', labelKey: 'pivot.directions.pitchUp', hintKey: 'pivot.fields.pitchUpHint', deadzoneMax: 90 },
  { key: 'pitchDown', labelKey: 'pivot.directions.pitchDown', hintKey: 'pivot.fields.pitchDownHint', deadzoneMax: 90 },
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
    deadzoneTransitionDegrees: props.settings.deadzoneTransitionDegrees,
  }
  const pitch: PivotAxisTuning = {
    rotationMultiplier: props.settings.pitchRotationMultiplier,
    deadzoneDegrees: props.settings.pitchDeadzoneDegrees,
    maxExtraDegrees: props.settings.maxExtraPitchDegrees,
    deadzoneTransitionDegrees: props.settings.pitchDeadzoneTransitionDegrees,
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
    <section class="rounded-[1rem] border p-4 surface-panel-soft">
      <PivotSnapStackingControl :settings="settings" :status="snapStacking" />
    </section>
    <!-- General: applies to both axes -->
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('pivot.fields.general') }}</p>
      <div class="mt-3 grid gap-3" :class="settings.responseMode === 'continuous' ? 'sm:grid-cols-3' : 'sm:grid-cols-2'">
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('pivot.fields.responseMode') }}
            <span
              :title="t('pivot.fields.responseModeHint')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select v-model="settings.responseMode" class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5">
            <option value="continuous">{{ t('pivot.fields.responseModeContinuous') }}</option>
            <option value="stepped">{{ t('pivot.fields.responseModeStepped') }}</option>
          </select>
        </label>
        <label v-if="settings.responseMode === 'continuous'" class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('pivot.fields.smoothing') }}
            <span
              :title="t('pivot.fields.smoothingHint')"
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
            {{ t('pivot.fields.activationRamp') }}
            <span
              :title="t('pivot.fields.activationRampHint')"
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
          <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.seconds') }}</span>
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
          <span class="block text-sm font-medium">{{ t('pivot.advancedAxes.label') }}</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            {{ t('pivot.advancedAxes.continuousDescription') }}
          </span>
        </span>
      </label>
    </div>

    <!-- Yaw + Pitch (symmetric continuous, or stepped limits) -->
    <div v-if="settings.responseMode === 'continuous' && !settings.advancedAxes" class="grid gap-3 md:grid-cols-2">
      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('pivot.fields.yaw') }}</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.multiplier') }}
              <span
                :title="t('pivot.fields.yawMultiplierHint')"
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
              {{ t('pivot.fields.maxExtra') }}
              <span
                :title="t('pivot.fields.yawMaxExtraHint')"
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
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.deadzone') }}
              <span
                :title="t('pivot.fields.yawDeadzoneHint')"
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
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.deadzoneTransition') }}
              <span
                :title="t('pivot.fields.deadzoneTransitionHint')"
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.deadzoneTransitionDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degreesHardDeadzone') }}</span>
          </label>
        </div>
      </div>

      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('pivot.fields.pitch') }}</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.multiplier') }}
              <span
                :title="t('pivot.fields.pitchMultiplierHint')"
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
              {{ t('pivot.fields.maxExtra') }}
              <span
                :title="t('pivot.fields.pitchMaxExtraHint')"
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
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.deadzone') }}
              <span
                :title="t('pivot.fields.pitchDeadzoneHint')"
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
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t('pivot.fields.deadzoneTransition') }}
              <span
                :title="t('pivot.fields.deadzoneTransitionHint')"
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <input
              v-model.number="settings.pitchDeadzoneTransitionDegrees"
              class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
              min="0"
              max="180"
              step="0.5"
              type="number"
            />
            <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degreesHardDeadzone') }}</span>
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
          <span class="block text-sm font-medium">{{ t('pivot.advancedAxes.label') }}</span>
          <span class="mt-0.5 block text-sm leading-6 text-muted">
            {{ t('pivot.advancedAxes.continuousDescription') }}
          </span>
        </span>
      </label>

      <div class="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
        <div v-for="direction in advancedDirections" :key="direction.key" class="rounded-[0.9rem] border p-3 surface-panel">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]" :title="t(direction.hintKey)">{{ t(direction.labelKey) }}</p>
          <div class="mt-3 grid gap-3">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                {{ t('pivot.fields.multiplier') }}
                <span
                  :title="t('pivot.fields.directionMultiplierHint')"
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
                {{ t('pivot.fields.deadzone') }}
                <span
                  :title="t('pivot.fields.directionDeadzoneHint')"
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
              <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                {{ t('pivot.fields.deadzoneTransition') }}
                <span
                  :title="t('pivot.fields.deadzoneTransitionHint')"
                  class="cursor-help select-none text-xs text-muted"
                  >ⓘ</span
                >
              </span>
              <input
                v-model.number="axisTuning(direction.key).deadzoneTransitionDegrees"
                class="app-input w-full rounded-[0.75rem] px-3 py-2"
                min="0"
                max="180"
                step="0.5"
                type="number"
              />
              <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                {{ t('pivot.fields.maxExtra') }}
                <span
                  :title="t('pivot.fields.directionMaxExtraHint')"
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
              <span class="mt-1 block text-xs text-muted">{{ t('pivot.units.degrees') }}</span>
            </label>
          </div>
        </div>
      </div>
    </div>
    <PivotPositionAssistEditor :settings="settings.positionAssist" />
  </div>
</template>
