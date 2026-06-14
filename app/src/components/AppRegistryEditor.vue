<script setup lang="ts">
import { computed, ref } from 'vue'

import type { SeenApplication } from '../lib/commands'
import { moduleLabels, moduleStateForApplication, type ModuleId, type RegisteredApplication, type VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
  seenApps: SeenApplication[]
  seenAppsLoading: boolean
}>()

defineEmits<{
  add: []
  addSeen: [exe: string]
  clearSeen: []
  refreshSeen: []
  remove: [index: number]
  openModule: [moduleId: ModuleId, applicationId: string]
}>()

const moduleIds: ModuleId[] = ['quadviews', 'pivotxr', 'depthxr']

function moduleChip(applicationId: string, moduleId: ModuleId) {
  const moduleState = moduleStateForApplication(props.config, moduleId, applicationId)
  const label = moduleLabels[moduleId]

  switch (moduleState.kind) {
    case 'default-off':
      return {
        text: `${label} · Default off`,
        chipClass: 'chip-idle',
        title: `The default ${label} profile is off. Click to create a custom profile for this application.`,
      }
    case 'custom':
      return {
        text: `${label} · ${moduleState.profileName}`,
        chipClass: 'chip-success',
        title: `Custom ${label} profile "${moduleState.profileName}" applies. Click to open it.`,
      }
    default:
      return {
        text: `${label} · Default`,
        chipClass: 'chip-idle',
        title: `The default ${label} profile applies. Click to create a custom profile for this application.`,
      }
  }
}

function normalizeExe(value: string): string {
  return value
    .trim()
    .split(/[/\\]/)
    .pop()
    ?.toLowerCase() ?? ''
}

function formatUnixSeconds(value: number): string {
  if (!value) {
    return 'Unknown'
  }

  return new Date(value * 1000).toLocaleString()
}

const seenAppViews = computed(() =>
  props.seenApps.map((seenApp) => {
    const registeredApplication = props.applications.find((application) => normalizeExe(application.match.exe) === normalizeExe(seenApp.exe))
    return {
      ...seenApp,
      registeredApplication,
    }
  }),
)

const editingApplicationId = ref<string | null>(null)

function finishNameEdit() {
  editingApplicationId.value = null
}
</script>

<template>
  <div class="space-y-5">
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div>
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Registry</p>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">Application Registry</h2>
        <p class="mt-2 text-sm leading-6 text-muted">
          Register the applications that module profiles can target by executable name. The chips on each card show how every module currently treats that application — click one to jump to its profile, or to create one.
        </p>
      </div>

      <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('add')">
        Add Application
      </button>
    </div>

    <div v-if="applications.length > 0" class="mt-4 grid gap-3 xl:grid-cols-2">
      <div v-for="(application, index) in applications" :key="application.id" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div class="flex flex-wrap items-center gap-2">
            <p v-if="editingApplicationId !== application.id" class="text-base font-semibold tracking-tight">{{ application.name || 'Unnamed application' }}</p>
            <input
              v-else
              v-model="application.name"
              class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-base font-semibold tracking-tight"
              placeholder="DCS"
              type="text"
              @blur="finishNameEdit"
              @keydown.enter="finishNameEdit"
            />
            <button
              v-if="editingApplicationId !== application.id"
              class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]"
              type="button"
              aria-label="Edit application name"
              @click="editingApplicationId = application.id"
            >
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
              </svg>
            </button>
          </div>

          <div class="flex flex-wrap items-center gap-3">
            <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('remove', index)">
              Remove
            </button>
          </div>
        </div>

        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Executable
            <span
              title="Executable matching is case-insensitive, but the name must include the application extension. For example: DCS.exe"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input v-model="application.match.exe" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" placeholder="DCS.exe" type="text" />
        </label>

        <div class="mt-3 flex flex-wrap items-center gap-2">
          <button
            v-for="moduleId in moduleIds"
            :key="moduleId"
            class="rounded-full px-3 py-1 text-xs font-medium transition hover:brightness-110"
            :class="moduleChip(application.id, moduleId).chipClass"
            type="button"
            :title="moduleChip(application.id, moduleId).title"
            @click="$emit('openModule', moduleId, application.id)"
          >
            {{ moduleChip(application.id, moduleId).text }}
          </button>
        </div>
      </div>
    </div>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      No applications registered yet. Add one before creating module profiles.
    </div>
  </article>

  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div>
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Discovery</p>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">OpenXR Application Discovery</h2>
        <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
          Automatically detects OpenXR applications you launch and stores their details locally for easy registration. Discovered apps won’t affect XR settings until added to your registry.
        </p>
      </div>

      <div class="flex flex-wrap gap-3">
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('refreshSeen')">
          Refresh
        </button>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('clearSeen')">
          Clear Seen Apps
        </button>
      </div>
    </div>

    <div class="mt-4 flex items-center justify-between gap-4 rounded-[1rem] border px-4 py-3 surface-panel-soft">
      <div>
        <p class="text-sm font-semibold">Track Discovered XR apps</p>
        <p class="mt-1 text-xs leading-5 text-muted">
          Stores executable names and first/last seen timestamps. All data is kept locally on this PC.
        </p>
      </div>
      <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
        <input v-model="config.core.trackSeenApps" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
        {{ config.core.trackSeenApps ? 'On' : 'Off' }}
      </label>
    </div>

    <div v-if="seenAppsLoading" class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      Loading seen apps...
    </div>

    <div v-else-if="seenAppViews.length > 0" class="mt-4 space-y-3">
      <div v-for="seenApp in seenAppViews" :key="seenApp.exe" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="font-mono text-base font-semibold tracking-tight">{{ seenApp.exe }}</p>
            <p class="mt-1 text-sm text-muted">
              {{ seenApp.launchCount }} {{ seenApp.launchCount === 1 ? 'launch' : 'launches' }}
            </p>
          </div>

          <div class="flex flex-wrap items-center gap-3">
            <span v-if="seenApp.registeredApplication" class="chip-success rounded-full px-3 py-1 text-xs font-medium">
              Registered as {{ seenApp.registeredApplication.name }}
            </span>
            <button
              v-else
              class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium"
              type="button"
              @click="$emit('addSeen', seenApp.exe)"
            >
              Add to Registry
            </button>
          </div>
        </div>

        <dl class="mt-3 grid gap-3 text-sm md:grid-cols-2">
          <div>
            <dt class="text-xs uppercase tracking-[0.18em] text-muted">First seen</dt>
            <dd class="mt-1">{{ formatUnixSeconds(seenApp.firstSeenUnixSeconds) }}</dd>
          </div>
          <div>
            <dt class="text-xs uppercase tracking-[0.18em] text-muted">Last seen</dt>
            <dd class="mt-1">{{ formatUnixSeconds(seenApp.lastSeenUnixSeconds) }}</dd>
          </div>
        </dl>
      </div>
    </div>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      No XR apps seen yet. Launch an OpenXR app while VectorXR is active, then refresh this list.
    </div>
  </article>
  </div>
</template>
