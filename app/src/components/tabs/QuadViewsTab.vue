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
        </label>
      </div>

      <div class="mb-5 grid gap-3 md:grid-cols-2">
        <label class="rounded-[1rem] border p-4 surface-panel-soft">
          <span class="mb-1.5 block text-sm font-medium">Prefer Eye Tracking</span>
          <span class="mb-3 block text-sm leading-6 text-muted">Use OpenXR eye gaze when available, then fall back to head-tracked focus.</span>
          <span class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
            <input v-model="config.modules.quadviews.preferEyeTracking" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            {{ config.modules.quadviews.preferEyeTracking ? 'Enabled' : 'Disabled' }}
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
              <span class="mb-1.5 block text-sm font-medium">Horizontal FOV</span>
              <input v-model.number="config.modules.quadviews.defaults.focusHorizontalFovDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="10" max="90" step="1" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Vertical FOV</span>
              <input v-model.number="config.modules.quadviews.defaults.focusVerticalFovDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="10" max="90" step="1" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Horizontal Offset</span>
              <input v-model.number="config.modules.quadviews.defaults.horizontalOffsetDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="-45" max="45" step="0.5" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Vertical Offset</span>
              <input v-model.number="config.modules.quadviews.defaults.verticalOffsetDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="-45" max="45" step="0.5" type="number" />
            </label>
          </div>
        </div>

        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Resolution</p>
          <div class="mt-3 grid gap-3 sm:grid-cols-2">
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Focus Scale</span>
              <input v-model.number="config.modules.quadviews.defaults.focusScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.5" max="2" step="0.05" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Peripheral Scale</span>
              <input v-model.number="config.modules.quadviews.defaults.peripheralScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.1" max="1.5" step="0.05" type="number" />
            </label>
          </div>
        </div>

        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Tracking</p>
          <div class="mt-3 grid gap-3">
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Mode</span>
              <select v-model="config.modules.quadviews.defaults.trackingMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
                <option value="head">Head tracked</option>
                <option value="eye">Eye tracked</option>
              </select>
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Smoothing</span>
              <input v-model.number="config.modules.quadviews.defaults.gazeSmoothing" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0" max="1" step="0.01" type="number" />
            </label>
            <label class="block">
              <span class="mb-1.5 block text-sm font-medium">Deadzone</span>
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
            <input v-else v-model="profile.name" class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-xl font-semibold tracking-tight" type="text" @blur="finishProfileNameEdit" @keydown.enter="finishProfileNameEdit" />
            <button v-if="editingProfileName !== index" class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]" type="button" aria-label="Edit profile name" @click="editingProfileName = index">
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
              </svg>
            </button>
            <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">{{ trackingModeLabel(profile.settings) }}</span>
            <label class="pill-toggle inline-flex items-center gap-2 rounded-full px-3 py-1.5 text-xs font-medium">
              <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ profile.enabled ? 'Profile Enabled' : 'Profile Disabled' }}
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
          <span class="mb-1.5 block text-sm font-medium">Applications</span>
          <div class="overflow-x-auto rounded-[0.75rem] border p-3 surface-panel-strong">
            <div class="flex min-w-max gap-3">
              <label v-for="application in applications" :key="application.id" class="flex min-w-[13rem] max-w-[16rem] items-start gap-3 rounded-[0.65rem] border px-3 py-3 text-sm surface-panel-soft">
                <input v-model="profile.applicationIds" class="mt-0.5 h-4 w-4 accent-depthxr-copper" type="checkbox" :value="application.id" @change="$emit('syncQuadViewsProfileName', index)" />
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
            <span class="mb-1.5 block text-sm font-medium">Mode</span>
            <select v-model="profile.settings.trackingMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
              <option value="head">Head tracked</option>
              <option value="eye">Eye tracked</option>
            </select>
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Focus FOV H</span>
            <input v-model.number="profile.settings.focusHorizontalFovDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="10" max="90" step="1" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Focus FOV V</span>
            <input v-model.number="profile.settings.focusVerticalFovDegrees" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="10" max="90" step="1" type="number" />
          </label>
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Peripheral Scale</span>
            <input v-model.number="profile.settings.peripheralScale" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" min="0.1" max="1.5" step="0.05" type="number" />
          </label>
        </div>
      </article>

      <div v-if="config.modules.quadviews.profiles.length === 0" class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
        No custom profiles yet. Add a profile to override quadview values for a specific application.
      </div>
    </section>
  </div>
</template>
