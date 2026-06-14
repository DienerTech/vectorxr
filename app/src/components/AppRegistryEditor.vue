<script setup lang="ts">
import { computed, ref } from 'vue'

import SeenAppDetailsModal from './SeenAppDetailsModal.vue'
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

const seenAppViews = computed(() =>
  props.seenApps.map((seenApp) => {
    const registeredApplication = props.applications.find((application) => normalizeExe(application.match.exe) === normalizeExe(seenApp.exe))
    return {
      ...seenApp,
      registeredApplication,
    }
  }),
)

// Unregistered apps are the actionable ones (promotion is the point); already
// registered apps are "filed" and only kept around for reference.
const unregisteredSeenApps = computed(() => seenAppViews.value.filter((seenApp) => !seenApp.registeredApplication))
const registeredSeenApps = computed(() => seenAppViews.value.filter((seenApp) => seenApp.registeredApplication))

type SeenAppView = SeenApplication & { registeredApplication: RegisteredApplication | undefined }

const detailsApp = ref<SeenAppView | null>(null)
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
        <div class="flex flex-wrap items-center gap-2">
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Discovery</p>
          <span
            class="rounded-full px-2.5 py-0.5 text-xs font-semibold uppercase tracking-[0.14em]"
            :class="config.core.trackSeenApps ? 'chip-success' : 'chip-warning'"
            :title="config.core.trackSeenApps
              ? 'Discovery is tracking the OpenXR apps you launch.'
              : 'Discovery is off. New OpenXR app launches will not be recorded until you enable tracking in Settings.'"
          >
            {{ config.core.trackSeenApps ? 'Tracking on' : 'Tracking off' }}
          </span>
        </div>
        <h3 class="mt-1 text-lg font-semibold tracking-tight">Where new apps come from</h3>
        <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
          VectorXR notes the OpenXR apps you launch so you can easily register them in one click. Nothing here changes your settings until you register it. Tracking is controlled in Settings.
        </p>
      </div>

      <div class="flex flex-wrap gap-2">
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('refreshSeen')">
          Refresh
        </button>
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('clearSeen')">
          Clear Seen Apps
        </button>
      </div>
    </div>

    <div v-if="seenAppsLoading" class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      Loading seen apps...
    </div>

    <template v-else-if="seenAppViews.length > 0">
      <!-- Actionable: unregistered discovered apps lead with a clear Register action -->
      <div v-if="unregisteredSeenApps.length > 0" class="mt-4 space-y-3">
        <div
          v-for="seenApp in unregisteredSeenApps"
          :key="seenApp.exe"
          class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft"
        >
          <div class="min-w-0">
            <p class="font-mono text-base font-semibold tracking-tight">{{ seenApp.exe }}</p>
            <p class="mt-1 text-sm text-muted">
              {{ seenApp.launchCount }} {{ seenApp.launchCount === 1 ? 'launch' : 'launches' }}
            </p>
          </div>

          <div class="flex flex-wrap items-center gap-2">
            <button
              class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
              type="button"
              @click="detailsApp = seenApp"
            >
              Details
            </button>
            <button
              class="button-accent rounded-[0.75rem] px-5 py-2 text-sm font-semibold"
              type="button"
              @click="$emit('addSeen', seenApp.exe)"
            >
              Register
            </button>
          </div>
        </div>
      </div>

      <p v-else class="mt-4 rounded-[1rem] border border-dashed px-4 py-4 text-center text-sm text-muted surface-panel-soft">
        <template v-if="config.core.trackSeenApps">
          Every discovered app is registered. New apps you launch will appear here.
        </template>
        <template v-else>
          Every discovered app is registered. Tracking is off, so no new apps will appear here.
        </template>
      </p>

      <!-- Filed: already-registered apps recede; they are done -->
      <div v-if="registeredSeenApps.length > 0" class="mt-5">
        <p class="text-xs uppercase tracking-[0.18em] text-muted">Already registered</p>
        <div class="mt-2 space-y-1.5">
          <div
            v-for="seenApp in registeredSeenApps"
            :key="seenApp.exe"
            class="flex flex-wrap items-center justify-between gap-3 rounded-[0.85rem] border px-4 py-2.5 surface-panel-soft opacity-60 transition hover:opacity-100"
          >
            <div class="flex min-w-0 flex-wrap items-center gap-2">
              <span class="font-mono text-sm font-medium">{{ seenApp.exe }}</span>
              <span class="chip-success rounded-full px-2.5 py-0.5 text-xs font-medium">
                Registered as {{ seenApp.registeredApplication?.name }}
              </span>
            </div>
            <button
              class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium"
              type="button"
              @click="detailsApp = seenApp"
            >
              Details
            </button>
          </div>
        </div>
      </div>
    </template>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      <template v-if="config.core.trackSeenApps">
        No XR apps seen yet. Launch an OpenXR app while VectorXR is active, then refresh this list.
      </template>
      <template v-else>
        Application tracking is disabled, so no new apps will appear here. Enable tracking in Settings to record OpenXR app launches.
      </template>
    </div>
  </article>

  <SeenAppDetailsModal
    :open="detailsApp !== null"
    :app="detailsApp"
    :registered-name="detailsApp?.registeredApplication?.name ?? null"
    @close="detailsApp = null"
  />
  </div>
</template>
