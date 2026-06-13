<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

import { openExternalUrl, type OpenXrLayerEntry, type OpenXrLayerSnapshot } from '../../lib/commands'
import type { VectorXRConfig } from '../../lib/model'
import type { PatchNoteEntry } from '../../lib/patchNotes'
import { checkForUpdates, type GitHubReleaseInfo, type UpdateCheckStatus } from '../../lib/updates'

const props = defineProps<{
  config: VectorXRConfig
  latestPatch: PatchNoteEntry
  openXrLayerSnapshot: OpenXrLayerSnapshot | null
  openXrLayersLoading: boolean
}>()

defineEmits<{
  viewHealth: []
  exportDebug: []
  openPatchNotes: []
}>()

const koFiUrl = 'https://ko-fi.com/dienertech'
const githubUrl = 'https://github.com/DienerTech/vectorxr'
const releasesUrl = 'https://github.com/DienerTech/vectorxr/releases/latest'
const currentVersion = props.latestPatch.version

const updateStatus = ref<UpdateCheckStatus>('idle')
const updateError = ref('')
const latestRelease = ref<GitHubReleaseInfo | null>(null)

const vectorXrLayer = computed<OpenXrLayerEntry | null>(() => {
  return props.openXrLayerSnapshot?.slices
    .flatMap((slice) => slice.layers)
    .find((layer) => layer.isVectorXr) ?? null
})

const vectorXrLayerStatusLabel = computed(() => {
  if (props.openXrLayersLoading) return 'Checking'
  if (!vectorXrLayer.value) return 'Not found'
  return vectorXrLayer.value.enabled ? 'Layer enabled' : 'Layer disabled'
})

const vectorXrLayerStatusClass = computed(() => {
  if (props.openXrLayersLoading || !vectorXrLayer.value) return 'chip-idle'
  return vectorXrLayer.value.enabled ? 'chip-success' : 'chip-warning'
})

const vectorXrLayerStatusDescription = computed(() => {
  if (props.openXrLayersLoading) {
    return 'Checking the registered VectorXR API layer.'
  }

  if (!vectorXrLayer.value) {
    return "VectorXR's API layer registration was not found. Enhancements will not apply until the layer is registered and enabled."
  }

  if (vectorXrLayer.value.enabled) {
    return 'The VectorXR API layer is enabled, so Enhancements can apply at runtime.'
  }

  return 'The VectorXR API layer is disabled. None of the Enhancements will apply, regardless of suite or tweak enabled state.'
})

const runtimeStatusClass = computed(() => props.config.core.enabled ? 'chip-success' : 'chip-warning')
const runtimeStatusLabel = computed(() => props.config.core.enabled ? 'Runtime enabled' : 'Runtime disabled')
const systemHealthDescription = computed(() => {
  if (!props.config.core.enabled) {
    return 'Runtime effects are disabled from the suite switch. Layer status is still available for troubleshooting.'
  }

  if (props.openXrLayersLoading) {
    return 'Checking VectorXR runtime and OpenXR layer readiness.'
  }

  if (!vectorXrLayer.value) {
    return 'VectorXR is enabled, but the OpenXR API layer registration was not found.'
  }

  if (!vectorXrLayer.value.enabled) {
    return 'VectorXR is enabled, but the OpenXR API layer is disabled.'
  }

  return 'VectorXR runtime and OpenXR layer status look ready for future OpenXR app launches.'
})

const releaseDate = computed(() => {
  if (!latestRelease.value?.publishedAt) {
    return ''
  }

  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
  }).format(new Date(latestRelease.value.publishedAt))
})

const updateSummary = computed(() => {
  if (updateStatus.value === 'checking') {
    return 'Checking GitHub releases...'
  }

  if (updateStatus.value === 'available' && latestRelease.value) {
    return `VectorXR ${latestRelease.value.tagName} is available.`
  }

  if (updateStatus.value === 'upToDate' && latestRelease.value) {
    return `You are on the latest published release, ${latestRelease.value.tagName}.`
  }

  if (updateStatus.value === 'unavailable') {
    return 'No published GitHub releases are available yet.'
  }

  if (updateStatus.value === 'error') {
    return updateError.value || 'Unable to check for updates right now.'
  }

  return 'Check GitHub for the latest published VectorXR build.'
})

const updateChipClass = computed(() => {
  if (updateStatus.value === 'available') {
    return 'chip-accent'
  }

  if (updateStatus.value === 'upToDate') {
    return 'chip-success'
  }

  if (updateStatus.value === 'error') {
    return 'chip-warning'
  }

  return 'chip-idle'
})

const updateChipLabel = computed(() => {
  if (updateStatus.value === 'checking') {
    return 'Checking'
  }

  if (updateStatus.value === 'available') {
    return 'Update'
  }

  if (updateStatus.value === 'upToDate') {
    return 'Current'
  }

  if (updateStatus.value === 'error') {
    return 'Retry'
  }

  return 'GitHub'
})

async function openLink(url: string) {
  await openExternalUrl(url)
}

async function refreshUpdates() {
  updateStatus.value = 'checking'
  updateError.value = ''

  try {
    const result = await checkForUpdates(currentVersion)
    latestRelease.value = result.latestRelease

    if (!result.latestRelease) {
      updateStatus.value = 'unavailable'
    } else if (result.updateAvailable) {
      updateStatus.value = 'available'
    } else {
      updateStatus.value = 'upToDate'
    }
  } catch (error) {
    updateStatus.value = 'error'
    updateError.value = error instanceof Error ? error.message : 'Unable to check for updates right now.'
  }
}

onMounted(() => {
  void refreshUpdates()
})
</script>

<template>
  <div class="space-y-5">
    <section class="grid gap-5 lg:grid-cols-[minmax(0,1fr)_minmax(20rem,0.42fr)]">
      <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Welcome</p>

        <h2 class="mt-3 text-3xl font-semibold tracking-tight">VectorXR</h2>
        <p class="mt-1.5 text-sm font-medium accent-text">
          OpenXR Enhancements, tuned per game.
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          VectorXR is an OpenXR Enhancement platform that reshapes how your headset renders and tracks — title by title. Reclaim frame rate with dynamic foveated rendering, amplify head rotation so you can check your six without overturning, and deepen stereo separation for a stronger sense of scale and presence.
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          OpenXR layer management and per-application profiles keep every title configured exactly how you like it, all from one place. Pick an Enhancement from the sidebar to get started, or review your system status below.
        </p>
        <p class="mt-3 text-xs text-soft">Developed independently by DienerTech LLC.</p>

        <div class="mt-5 flex flex-wrap gap-3">
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="openLink(githubUrl)"
          >
            GitHub
          </button>
        </div>
      </article>

      <aside class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Support Development</p>
        <h2 class="mt-3 text-2xl font-semibold tracking-tight">Support VectorXR</h2>
        <p class="mt-3 text-sm leading-6 text-muted">
          Tips through Ko-fi help fund testing hardware, release work, and continued polish for OpenXR tools.
        </p>
        <button
          class="button-accent mt-5 w-full rounded-[0.75rem] px-4 py-2.5 text-sm font-semibold"
          type="button"
          @click="openLink(koFiUrl)"
        >
          Support on Ko-fi
        </button>
      </aside>
    </section>

    <section class="grid gap-5 lg:grid-cols-[minmax(0,1fr)_minmax(20rem,0.42fr)]">
      <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">System Health</p>
          <div class="flex flex-wrap gap-2">
            <span
              class="inline-flex min-w-[9.75rem] cursor-default items-center justify-center rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
              :class="runtimeStatusClass"
              title="Suite-level runtime switch state."
            >
              {{ runtimeStatusLabel }}
            </span>
            <span
              class="inline-flex min-w-[9.75rem] cursor-default items-center justify-center rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
              :class="vectorXrLayerStatusClass"
              :title="vectorXrLayerStatusDescription"
            >
              {{ vectorXrLayerStatusLabel }}
            </span>
          </div>
        </div>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          {{ systemHealthDescription }}
        </p>
        <div class="mt-5 flex flex-wrap gap-2 border-t pt-4" style="border-color: var(--app-border)">
          <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('viewHealth')">
            View Health
          </button>
          <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('exportDebug')">
            Export Debug
          </button>
        </div>
      </article>

      <aside class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <div class="flex items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Updates</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">Release status</h2>
          </div>

          <span :class="['rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]', updateChipClass]">
            {{ updateChipLabel }}
          </span>
        </div>

        <p class="mt-3 text-sm leading-6 text-muted">{{ updateSummary }}</p>
        <p v-if="releaseDate" class="mt-2 text-xs font-medium uppercase tracking-[0.16em] text-soft">
          Published {{ releaseDate }}
        </p>

        <div class="mt-4 grid grid-cols-2 gap-3">
          <button
            class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
            type="button"
            :disabled="updateStatus === 'checking'"
            @click="refreshUpdates"
          >
            Check
          </button>
          <button
            class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
            type="button"
            @click="openLink(latestRelease?.htmlUrl || releasesUrl)"
          >
            Release
          </button>
        </div>
      </aside>
    </section>

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Latest Patch Notes</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ props.latestPatch.title }}</h2>
        </div>

        <div class="flex flex-wrap items-center gap-3">
          <span class="text-sm text-muted">{{ props.latestPatch.date }}</span>
          <span class="chip-accent rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">
            {{ props.latestPatch.version }}
          </span>
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="$emit('openPatchNotes')"
          >
            Read full patch notes
          </button>
        </div>
      </div>

      <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">{{ props.latestPatch.summary }}</p>
    </article>
  </div>
</template>
