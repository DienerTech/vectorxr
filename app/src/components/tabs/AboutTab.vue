<script setup lang="ts">
import { computed, onMounted } from 'vue'

import { openExternalUrl } from '../../lib/commands'
import { formatPatchNoteInlineHtml, type PatchNoteEntry } from '../../lib/patchNotes'
import { useUpdateStore } from '../../stores/updateStore'
import DiscordJoinButton from '../DiscordJoinButton.vue'
import I18nText from '../I18nText.vue'
import { t } from '../../i18n/index.ts'

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
    return t('about.updates.summary.checking')
  }

  if (updateStatus.value === 'available' && latestRelease.value) {
    return t('about.updates.summary.available', { tag: latestRelease.value.tagName })
  }

  if (updateStatus.value === 'upToDate' && latestRelease.value) {
    return t('about.updates.summary.upToDate', { tag: latestRelease.value.tagName })
  }

  if (updateStatus.value === 'unavailable') {
    return t('about.updates.summary.unavailable')
  }

  if (updateStatus.value === 'error') {
    return updateError.value || t('app.updates.checkFailed')
  }

  return t('about.updates.summary.idle')
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
    return t('about.updates.chip.checking')
  }

  if (updateStatus.value === 'available') {
    return t('about.updates.chip.available')
  }

  if (updateStatus.value === 'upToDate') {
    return t('about.updates.chip.upToDate')
  }

  if (updateStatus.value === 'error') {
    return t('about.updates.chip.error')
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
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('about.eyebrow') }}</p>

        <h2 class="mt-3 text-3xl font-semibold tracking-tight">VectorXR</h2>
        <p class="mt-1.5 text-sm font-medium accent-text">
          {{ t('about.tagline') }}
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          {{ t('about.intro') }}
        </p>
        <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          {{ t('about.profiles') }}
        </p>
        <I18nText keypath="about.firstAndOnly" tag="p" class="mt-3 max-w-3xl text-sm leading-6 text-muted">
          <template #highlight><span class="font-semibold accent-text">{{ t('about.firstAndOnlyHighlight') }}</span></template>
        </I18nText>
        <p class="mt-4 text-sm leading-6 text-muted">{{ t('about.developedBy') }}</p>
        <p class="mt-2 text-sm leading-6 text-muted">{{ t('about.copyright') }}</p>

        <div class="mt-5 flex flex-wrap gap-3">
          <!-- i18n-ignore: company name -->
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
          <DiscordJoinButton />
        </div>
      </article>

      <aside class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('about.support.eyebrow') }}</p>
        <h2 class="mt-3 text-2xl font-semibold tracking-tight">{{ t('about.support.title') }}</h2>
        <p class="mt-3 text-sm leading-6 text-muted">
          {{ t('about.support.body') }}
        </p>
        <button
          class="button-accent mt-5 w-full rounded-[0.75rem] px-4 py-2.5 text-sm font-semibold"
          type="button"
          @click="openLink(koFiUrl)"
        >
          {{ t('about.support.button') }}
        </button>
      </aside>
    </section>

    <section class="grid gap-5 lg:grid-cols-[minmax(0,1fr)_minmax(20rem,0.42fr)]">
      <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('about.patchNotes.eyebrow') }}</p>
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
            {{ t('about.patchNotes.readFull') }}
          </button>
        </div>
      </article>

      <aside class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
        <div class="flex items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('about.updates.eyebrow') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('about.updates.title') }}</h2>
          </div>

          <span :class="['rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]', updateChipClass]">
            {{ updateChipLabel }}
          </span>
        </div>

        <p class="mt-3 text-sm leading-6 text-muted">{{ updateSummary }}</p>
        <p v-if="releaseDate" class="mt-2 text-xs font-medium uppercase tracking-[0.16em] text-soft">
          {{ t('about.updates.published', { date: releaseDate }) }}
        </p>

        <div class="mt-4 grid grid-cols-2 gap-3">
          <button
            class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
            type="button"
            :disabled="updateStatus === 'checking'"
            @click="refreshUpdates"
          >
            {{ t('about.updates.check') }}
          </button>
          <button
            class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
            type="button"
            @click="openLink(latestRelease?.htmlUrl || releasesUrl)"
          >
            {{ t('about.updates.release') }}
          </button>
        </div>
      </aside>
    </section>
  </div>
</template>
