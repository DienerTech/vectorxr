<script setup lang="ts">
import { t, type MessageKey } from '../i18n/index.ts'
import type { QuadViewsSettings } from '../lib/model'

// Shared Quadviews settings editor used by both the Default Profile and each
// custom profile, so the two stay visually and structurally identical.
// Fields mutate the passed-in reactive `settings` object directly.
const props = defineProps<{
  settings: QuadViewsSettings
}>()

type QuadViewsPreset = 'performance' | 'balanced' | 'quality'

const presets: Record<QuadViewsPreset, Pick<QuadViewsSettings,
  'focusHorizontalSizePercent' | 'focusVerticalSizePercent' | 'focusScale' |
  'peripheralScale' | 'foveateSharpness' | 'transitionThicknessPercent'>> = {
  performance: {
    focusHorizontalSizePercent: 35,
    focusVerticalSizePercent: 35,
    focusScale: 1,
    peripheralScale: 0.3,
    foveateSharpness: 55,
    transitionThicknessPercent: 20,
  },
  balanced: {
    focusHorizontalSizePercent: 40,
    focusVerticalSizePercent: 40,
    focusScale: 1.1,
    peripheralScale: 0.35,
    foveateSharpness: 50,
    transitionThicknessPercent: 25,
  },
  quality: {
    focusHorizontalSizePercent: 45,
    focusVerticalSizePercent: 45,
    focusScale: 1.15,
    peripheralScale: 0.5,
    foveateSharpness: 45,
    transitionThicknessPercent: 25,
  },
}

const presetLabels: Record<QuadViewsPreset, MessageKey> = {
  performance: 'quadviews.fields.preset.performance',
  balanced: 'quadviews.fields.preset.balanced',
  quality: 'quadviews.fields.preset.quality',
}

function applyPreset(preset: QuadViewsPreset) {
  Object.assign(props.settings, presets[preset])
}

function presetActive(preset: QuadViewsPreset) {
  return Object.entries(presets[preset]).every(
    ([key, value]) => props.settings[key as keyof QuadViewsSettings] === value,
  )
}
</script>

<template>
  <div class="space-y-3">
    <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel-soft">
      <div>
        <p class="eyebrow flex items-center gap-1.5 text-xs uppercase tracking-[0.18em]">
          {{ t('quadviews.fields.preset.heading') }}
          <span class="restart-required-mark" :title="t('quadviews.fields.preset.restartTitle')">&#8635;</span>
        </p>
        <p class="mt-1 text-xs text-muted">{{ t('quadviews.fields.preset.intro') }}</p>
      </div>
      <div class="flex flex-wrap gap-2">
        <button
          v-for="preset in (['performance', 'balanced', 'quality'] as QuadViewsPreset[])"
          :key="preset"
          class="rounded-[0.65rem] border px-3 py-1.5 text-xs font-semibold capitalize"
          :class="presetActive(preset) ? 'button-accent' : 'button-secondary'"
          type="button"
          :title="t('quadviews.fields.preset.buttonTitle')"
          @click="applyPreset(preset)"
        >
          {{ t(presetLabels[preset]) }}
        </button>
      </div>
    </div>

    <div class="grid gap-3 lg:grid-cols-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('quadviews.fields.focusWindow') }}</p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.focusWidth.label') }}
            <span class="restart-required-mark" :title="t('quadviews.fields.focusWidth.restart')">&#8635;</span>
          </span>
          <input
            v-model.number="settings.focusHorizontalSizePercent"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="5"
            max="100"
            step="1"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">{{ t('quadviews.fields.percentOfFov') }}</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.focusHeight.label') }}
            <span class="restart-required-mark" :title="t('quadviews.fields.focusHeight.restart')">&#8635;</span>
          </span>
          <input
            v-model.number="settings.focusVerticalSizePercent"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="5"
            max="100"
            step="1"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">{{ t('quadviews.fields.percentOfFov') }}</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.horizontalOffset.label') }}
            <span
              :title="t('quadviews.fields.horizontalOffset.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.horizontalOffsetDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="-45"
            max="45"
            step="0.5"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.verticalOffset.label') }}
            <span
              :title="t('quadviews.fields.verticalOffset.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.verticalOffsetDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="-45"
            max="45"
            step="0.5"
            type="number"
          />
        </label>
      </div>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('quadviews.fields.resolution') }}</p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.focusResolution.label') }}
            <span class="restart-required-mark" :title="t('quadviews.fields.focusResolution.restart')">&#8635;</span>
          </span>
          <input
            :value="Math.round(settings.focusScale * 100)"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="50"
            max="300"
            step="1"
            type="number"
            @input="settings.focusScale = Number(($event.target as HTMLInputElement).value) / 100"
          />
          <span class="mt-1 block text-xs text-muted">{{ t('quadviews.fields.percentOfHmd') }}</span>
          <span
            v-if="settings.focusScale > 2"
            class="mt-2 block rounded-[0.75rem] border px-3 py-2 text-xs leading-5 chip-warning"
          >
            {{ t('quadviews.fields.focusResolution.warning', {
              percent: Math.round(settings.focusScale * 100),
              factor: (settings.focusScale * settings.focusScale).toFixed(1),
            }) }}
          </span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.peripheralResolution.label') }}
            <span class="restart-required-mark" :title="t('quadviews.fields.peripheralResolution.restart')">&#8635;</span>
          </span>
          <input
            :value="Math.round(settings.peripheralScale * 100)"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="10"
            max="150"
            step="1"
            type="number"
            @input="settings.peripheralScale = Number(($event.target as HTMLInputElement).value) / 100"
          />
          <span class="mt-1 block text-xs text-muted">{{ t('quadviews.fields.percentOfHmd') }}</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.foveateSharpness.label') }}
            <span
              :title="t('quadviews.fields.foveateSharpness.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.foveateSharpness"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="100"
            step="1"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.transitionThickness.label') }}
            <span
              :title="t('quadviews.fields.transitionThickness.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.transitionThicknessPercent"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="50"
            step="1"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">{{ t('quadviews.fields.percentOfFocusWindow') }}</span>
        </label>
      </div>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('quadviews.fields.tracking') }}</p>
      <div class="mt-2 grid gap-2">
        <label class="block">
          <span class="mb-1 flex items-center gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.mode.label') }}
            <span class="restart-required-mark" :title="t('quadviews.fields.mode.restart')">&#8635;</span>
            <span
              :title="t('quadviews.fields.mode.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select
            v-model="settings.trackingMode"
            class="app-input w-full rounded-[0.75rem] px-3 py-2"
          >
            <option value="head">{{ t('quadviews.fields.mode.head') }}</option>
            <option value="eye">{{ t('quadviews.fields.mode.eye') }}</option>
          </select>
        </label>
        <label class="block">
          <span class="mb-1 flex items-center gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.eyeTrackingCorrection.label') }}
            <span :title="t('quadviews.fields.eyeTrackingCorrection.help')" class="cursor-help select-none text-xs text-muted">ⓘ</span>
          </span>
          <select v-model="settings.eyeTrackingCorrection" class="app-input w-full rounded-[0.75rem] px-3 py-2">
            <option value="default">{{ t('quadviews.fields.eyeTrackingCorrection.default') }}</option>
            <option value="flip-z">{{ t('quadviews.fields.eyeTrackingCorrection.flipZ') }}</option>
          </select>
        </label>
        <label class="block">
          <span class="mb-1 flex items-center gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.smoothing.label') }}
            <span
              :title="t('quadviews.fields.smoothing.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.gazeSmoothing"
            class="app-input w-full rounded-[0.75rem] px-3 py-2"
            min="0"
            max="1"
            step="0.01"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1 flex items-center gap-1.5 text-sm font-medium">
            {{ t('quadviews.fields.deadzone.label') }}
            <span
              :title="t('quadviews.fields.deadzone.help')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.gazeDeadzoneDegrees"
            class="app-input w-full rounded-[0.75rem] px-3 py-2"
            min="0"
            max="10"
            step="0.1"
            type="number"
          />
        </label>
      </div>
    </div>
    </div>
  </div>
</template>
