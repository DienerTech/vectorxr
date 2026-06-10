<script setup lang="ts">
import { computed, ref } from 'vue'

import type { QuadViewsProfileConfig, QuadViewsSettings, RegisteredApplication, VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addQuadViewsProfile: []
  removeQuadViewsProfile: [index: number]
  syncQuadViewsProfileName: [index: number]
}>()

const editingProfileName = ref<number | null>(null)

function finishProfileNameEdit() {
  editingProfileName.value = null
}

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  props.config.modules.quadviews.profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex === undefined) {
        firstProfileByApplication.set(applicationId, index)
        continue
      }

      const appName = applicationNameById.get(applicationId) ?? applicationId
      warnings.set(index, [...(warnings.get(index) ?? []), `${appName} is already targeted by Profile ${firstIndex + 1}. The first enabled profile wins.`])
    }
  })

  return warnings
})

function trackingModeLabel(settings: QuadViewsSettings) {
  return settings.trackingMode === 'eye' ? 'Eye tracked' : 'Head tracked'
}
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Quadviews</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Manage native quadview defaults and per-application profiles for foveal and peripheral rendering.
          </p>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.quadviews.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Quadviews Enabled
          <span title="Turns VectorXR's native quadviews rendering path on or off for OpenXR apps." class="cursor-help select-none text-xs text-muted">ⓘ</span>
        </label>
      </div>

      <div class="mb-5 grid gap-3 md:grid-cols-2">
        <label class="rounded-[1rem] border p-4 surface-panel-soft">
          <span class="mb-1.5 block text-sm font-medium">Prefer Eye Tracking</span>
          <span class="mb-3 block text-sm leading-6 text-muted">Use OpenXR eye gaze when available, then fall back to head-tracked focus.</span>
          <span class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
            <input v-model="config.modules.quadviews.preferEyeTracking" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            {{ config.modules.quadviews.preferEyeTracking ? 'Enabled' : 'Disabled' }}
            <span title="Uses eye gaze for the sharp focus area when the headset and runtime support it." class="cursor-help select-none text-xs text-muted">ⓘ</span>
          </span>
        </label>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="mb-1.5 text-sm font-medium">Runtime Source</p>
          <p class="text-sm leading-6 text-muted">
            The engine contract resolves one focus direction shared by Quadviews and Pivot so the high-fidelity zone follows the effective look direction.
          </p>
        </div>
      </div>

      <p class="eyebrow mb-3 text-xs uppercase tracking-[0.24em]">Default Profile</p>
      <div class="grid gap-3 lg:grid-cols-3">
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Focus Window</p>
          <div class="mt-3 grid gap-3 sm:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Focus Width
                <span title="Width of the high-detail focus area as a percent of the projected eye view. Larger values sharpen more of the screen but cost more GPU time." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.focusHorizontalSizePercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="5" max="100" step="1" type="number" />
              <span class="mt-1 block text-xs text-muted">% of FOV</span>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Focus Height
                <span title="Height of the high-detail focus area as a percent of the projected eye view. Larger values are more forgiving but reduce the performance gain." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.focusVerticalSizePercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="5" max="100" step="1" type="number" />
              <span class="mt-1 block text-xs text-muted">% of FOV</span>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Horizontal Offset
                <span title="Moves the focus area left or right in degrees. Use this only if the sharp region feels off-center." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.horizontalOffsetDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="-45" max="45" step="0.5" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Vertical Offset
                <span title="Moves the focus area up or down in degrees. Helpful if a headset's gaze center feels vertically biased." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.verticalOffsetDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="-45" max="45" step="0.5" type="number" />
            </label>
          </div>
        </div>

        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Resolution</p>
          <div class="mt-3 grid gap-3 sm:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Foveate Resolution
                <span title="Resolution scale for the high-detail focus views. Higher values sharpen the center but increase GPU load." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.focusScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.5" max="2" step="0.05" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Peripheral Scale
                <span title="Resolution scale for the outer peripheral views. Lower values improve frames but make the edges softer." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.peripheralScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.1" max="1.5" step="0.05" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Foveate Sharpness
                <span title="Applies extra sharpening to the high-detail focus image. 0 is off; higher values add crispness but can create halos." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.foveateSharpness" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="100" step="1" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Transition Thickness
                <span title="Controls how wide the blended edge is between the focus and peripheral images. Higher values hide the boundary better but soften more of the focus window." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.transitionThicknessPercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="50" step="1" type="number" />
              <span class="mt-1 block text-xs text-muted">% of focus window</span>
            </label>
          </div>
        </div>

        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Tracking</p>
          <div class="mt-3 grid gap-3">
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Mode
                <span title="Chooses whether the focus area follows head direction or eye gaze when eye tracking is available." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <select v-model="config.modules.quadviews.defaults.trackingMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
                <option value="head">Head tracked</option>
                <option value="eye">Eye tracked</option>
              </select>
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Smoothing
                <span title="Softens focus-area movement. 0 is instant; higher values reduce jitter but add lag." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.gazeSmoothing" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="1" step="0.01" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
                Deadzone
                <span title="Small gaze movements ignored before the focus area moves. Useful for reducing shimmer near center." class="cursor-help select-none text-xs text-muted">ⓘ</span>
              </span>
              <input v-model.number="config.modules.quadviews.defaults.gazeDeadzoneDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="10" step="0.1" type="number" />
            </label>
          </div>
        </div>
      </div>
    </article>

    <section class="space-y-4">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profiles</p>
          <h2 class="text-2xl font-semibold tracking-tight">Custom Profiles</h2>
        </div>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('addQuadViewsProfile')">
          Add Profile
        </button>
      </div>

      <article
        v-for="(profile, index) in config.modules.quadviews.profiles"
        :key="`quadviews-profile-${index}`"
        class="rounded-[1rem] border p-5 shadow-panel transition"
        :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft'"
      >
        <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
          <div class="flex flex-wrap items-center gap-2">
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profile {{ index + 1 }}</p>
            <h3 v-if="editingProfileName !== index" class="text-xl font-semibold tracking-tight">{{ profile.name }}</h3>
            <input v-else v-model="profile.name" class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-xl font-semibold tracking-tight" title="Friendly name for this Quadviews profile." type="text" @blur="finishProfileNameEdit" @keydown.enter="finishProfileNameEdit" />
            <button v-if="editingProfileName !== index" class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]" type="button" aria-label="Edit profile name" @click="editingProfileName = index">
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
              </svg>
            </button>
            <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">{{ trackingModeLabel(profile.settings) }}</span>
            <label class="pill-toggle inline-flex items-center gap-2 rounded-full px-3 py-1.5 text-xs font-medium">
              <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ profile.enabled ? 'Profile Enabled' : 'Profile Disabled' }}
              <span title="When enabled, this profile overrides the default Quadviews settings for its assigned applications." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </label>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('removeQuadViewsProfile', index)">
            Remove
          </button>
        </div>

        <div v-if="profileWarnings.get(index)?.length" class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
          <p class="font-medium">Profile conflict</p>
          <ul class="mt-2 space-y-1">
            <li v-for="warning in profileWarnings.get(index)" :key="warning">{{ warning }}</li>
          </ul>
        </div>

        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Applications
            <span title="Choose which registered applications should use this Quadviews profile. The first enabled matching profile wins." class="cursor-help select-none text-xs text-muted">ⓘ</span>
          </span>
          <div class="overflow-x-auto rounded-[0.75rem] border p-3 surface-panel-strong">
            <div class="flex min-w-max gap-3">
              <label v-for="application in applications" :key="application.id" class="flex min-w-[13rem] max-w-[16rem] items-start gap-3 rounded-[0.65rem] border px-3 py-3 text-sm surface-panel-soft">
                <input v-model="profile.applicationIds" class="mt-0.5 h-4 w-4 accent-depthxr-copper" type="checkbox" :value="application.id" title="Assign this application to the current Quadviews profile." @change="$emit('syncQuadViewsProfileName', index)" />
                <span>
                  <span class="block font-medium">{{ application.name }}</span>
                  <span class="block font-mono text-xs text-muted">{{ application.match.exe }}</span>
                </span>
              </label>
            </div>
            <p v-if="applications.length === 0" class="text-sm text-muted">Add an application on the Application Registry tab before assigning this profile.</p>
          </div>
        </label>

        <div class="mt-4 grid gap-3 lg:grid-cols-4">
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Mode
              <span title="Chooses whether this application's focus area follows head direction or eye gaze." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <select v-model="profile.settings.trackingMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
              <option value="head">Head tracked</option>
              <option value="eye">Eye tracked</option>
            </select>
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Focus Width
              <span title="Width of this application's high-detail focus area as a percent of the projected eye view." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input v-model.number="profile.settings.focusHorizontalSizePercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="5" max="100" step="1" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Focus Height
              <span title="Height of this application's high-detail focus area as a percent of the projected eye view." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input v-model.number="profile.settings.focusVerticalSizePercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="5" max="100" step="1" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Focus Scale
              <span title="Resolution scale for this application's high-detail focus views." class="cursor-help select-none text-xs text-muted">â“˜</span>
            </span>
            <input v-model.number="profile.settings.focusScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.5" max="2" step="0.05" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Peripheral Scale
              <span title="Resolution scale for this application's outer peripheral views. Lower values can improve frame rate." class="cursor-help select-none text-xs text-muted">ⓘ</span>
            </span>
            <input v-model.number="profile.settings.peripheralScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.1" max="1.5" step="0.05" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Sharpness
              <span title="Applies extra sharpening to this application's focus image." class="cursor-help select-none text-xs text-muted">â“˜</span>
            </span>
            <input v-model.number="profile.settings.foveateSharpness" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="100" step="1" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Transition
              <span title="Blend width between the focus and peripheral images for this application." class="cursor-help select-none text-xs text-muted">â“˜</span>
            </span>
            <input v-model.number="profile.settings.transitionThicknessPercent" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="50" step="1" type="number" />
          </label>
        </div>
      </article>

      <div v-if="config.modules.quadviews.profiles.length === 0" class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
        No custom profiles yet. Add a profile to override quadview values for a specific application.
      </div>
    </section>
  </div>
</template>
