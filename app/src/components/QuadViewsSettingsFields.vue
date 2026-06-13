<script setup lang="ts">
import type { QuadViewsSettings } from '../lib/model'

// Shared Quadviews settings editor used by both the Default Profile and each
// custom profile, so the two stay visually and structurally identical.
// Fields mutate the passed-in reactive `settings` object directly.
defineProps<{
  settings: QuadViewsSettings
}>()
</script>

<template>
  <div class="grid gap-3 lg:grid-cols-3">
    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Focus Window</p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Focus Width
            <span
              title="Width of the high-detail focus area as a percent of the projected eye view. Larger values sharpen more of the screen but cost more GPU time."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.focusHorizontalSizePercent"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="5"
            max="100"
            step="1"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">% of FOV</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Focus Height
            <span
              title="Height of the high-detail focus area as a percent of the projected eye view. Larger values are more forgiving but reduce the performance gain."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.focusVerticalSizePercent"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="5"
            max="100"
            step="1"
            type="number"
          />
          <span class="mt-1 block text-xs text-muted">% of FOV</span>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Horizontal Offset
            <span
              title="Moves the focus area left or right in degrees. Use this only if the sharp region feels off-center."
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
            Vertical Offset
            <span
              title="Moves the focus area up or down in degrees. Helpful if a headset's gaze center feels vertically biased."
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
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Resolution</p>
      <div class="mt-3 grid gap-3 sm:grid-cols-2">
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Foveate Resolution
            <span
              title="Resolution scale for the high-detail focus views. Higher values sharpen the center but increase GPU load."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.focusScale"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0.5"
            max="2"
            step="0.05"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Peripheral Scale
            <span
              title="Resolution scale for the outer peripheral views. Lower values improve frames but make the edges softer."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.peripheralScale"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0.1"
            max="1.5"
            step="0.05"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Foveate Sharpness
            <span
              title="Applies extra sharpening to the high-detail focus image. 0 is off; higher values add crispness but can create halos."
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
            Transition Thickness
            <span
              title="Controls how wide the blended edge is between the focus and peripheral images. Higher values hide the boundary better but soften more of the focus window."
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
          <span class="mt-1 block text-xs text-muted">% of focus window</span>
        </label>
      </div>
    </div>

    <div class="rounded-[1rem] border p-4 surface-panel-soft">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">Tracking</p>
      <div class="mt-3 grid gap-3">
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Mode
            <span
              title="Chooses whether the focus area follows head direction or eye gaze when eye tracking is available."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select
            v-model="settings.trackingMode"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          >
            <option value="head">Head tracked</option>
            <option value="eye">Eye tracked</option>
          </select>
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Smoothing
            <span
              title="Softens focus-area movement. 0 is instant; higher values reduce jitter but add lag."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.gazeSmoothing"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="1"
            step="0.01"
            type="number"
          />
        </label>
        <label class="block">
          <span class="mb-1.5 flex min-h-[2.5rem] items-start gap-1.5 text-sm font-medium">
            Deadzone
            <span
              title="Small gaze movements ignored before the focus area moves. Useful for reducing shimmer near center."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="settings.gazeDeadzoneDegrees"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="0"
            max="10"
            step="0.1"
            type="number"
          />
        </label>
      </div>
    </div>
  </div>
</template>
