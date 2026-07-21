<script setup lang="ts">
import { computed, onMounted } from 'vue'

import { openExternalUrl } from '../../lib/commands'
import { formatPatchNoteInlineHtml, type PatchNoteEntry } from '../../lib/patchNotes'
import { useUpdateStore } from '../../stores/updateStore'

const props = defineProps<{
  latestPatch: PatchNoteEntry
}>()

defineEmits<{
  openPatchNotes: []
}>()

const dienerTechUrl = 'https://diener.tech'
const githubUrl = 'https://github.com/DienerTech/vectorxr'
const koFiUrl = 'https://ko-fi.com/dienertech'
const releasesUrl = 'https://github.com/DienerTech/vectorxr/releases/latest'
const currentVersion = props.latestPatch.version

// Shared with the sidebar indicator so both reflect the same check result.
const updateStore = useUpdateStore()
const updateStatus = computed(() => updateStore.state.status)
const updateError = computed(() => updateStore.state.error)
const latestRelease = computed(() => updateStore.state.latestRelease)

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

// Manual re-check from the "Check" button forces a fresh request.
function refreshUpdates() {
  void updateStore.check(currentVersion, true)
}

onMounted(() => {
  // No-op if the startup check has already run; keeps the two entry points in sync
  // without firing a duplicate request.
  void updateStore.check(currentVersion)
})
</script>

<template>
  <div class="space-y-5">
    <section class="grid gap-5 lg:grid-cols-[minmax(0,1fr)_minmax(20rem,0.42fr)]">
      <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">About</p>

        <h2 class="mt-3 text-3xl font-semibold tracking-tight">VectorXR</h2>
        <p class="mt-1.5 text-sm font-medium accent-text">
          OpenXR Enhancements, tuned per game.
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          VectorXR is an OpenXR Enhancement platform that reshapes how your headset renders and tracks — title by title. Reclaim frame rate with dynamic foveated rendering, amplify head rotation so you can check your six without overturning, and deepen stereo separation for a stronger sense of scale and presence.
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          OpenXR layer management and per-application profiles keep every title configured exactly how you like it, all from one place.
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          <span class="font-semibold accent-text">The first and only</span> OpenXR solution where enhanced head rotation and quad-views foveated rendering work together — the foveated region follows your pivoted view instead of being left behind.
        </p>
        <p class="mt-4 text-sm leading-6 text-muted">Developed independently by DienerTech LLC.</p>
        <p class="mt-2 text-sm leading-6 text-muted">(c) 2026 DienerTech LLC. Released under the Mozilla Public License 2.0.</p>

        <div class="mt-5 flex flex-wrap gap-3">
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="openLink(dienerTechUrl)"
          >
            DienerTech
          </button>
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
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Latest Patch Notes</p>
            <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ props.latestPatch.title }}</h2>
          </div>

          <span class="chip-accent rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">
            {{ props.latestPatch.version }}
          </span>
        </div>

        <p
          class="mt-3 max-w-3xl text-sm leading-6 text-muted"
          v-html="formatPatchNoteInlineHtml(props.latestPatch.summary)"
        ></p>

        <div class="mt-5 flex flex-wrap items-center gap-3 text-sm">
          <span class="text-muted">{{ props.latestPatch.date }}</span>
          <button
            class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            @click="$emit('openPatchNotes')"
          >
            Read full patch notes
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
  </div>
</template>
