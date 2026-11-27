<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import GuideLink from '../GuideLink.vue'

import BindingConflictWarnings from '../BindingConflictWarnings.vue'
import BindingListEditor from '../BindingListEditor.vue'
import OptionCards from '../OptionCards.vue'
import SetupGuide from '../SetupGuide.vue'
import { t } from '../../i18n/index.ts'
import { defaultScreenshotFolder, openScreenshotFolder, pickFolder } from '../../lib/commands'
import { errorText } from '../../lib/errors'
import { savedBindingConflictWarnings, type ScreenshotEyes, type VectorXRConfig } from '../../lib/model'

const props = defineProps<{ config: VectorXRConfig }>()

const screenshot = computed(() => props.config.core.screenshot)
const warnings = computed(() => savedBindingConflictWarnings(props.config, screenshot.value.bindings))

// Translated at render so a language change applies.
const eyeOptions = computed<{ value: ScreenshotEyes; label: string; description: string }[]>(() => [
  { value: 'both', label: t('utilities.screenshots.eyes.both.label'), description: t('utilities.screenshots.eyes.both.description') },
  { value: 'left', label: t('utilities.screenshots.eyes.left.label'), description: t('utilities.screenshots.eyes.left.description') },
  { value: 'right', label: t('utilities.screenshots.eyes.right.label'), description: t('utilities.screenshots.eyes.right.description') },
])

// The default folder as this PC resolves it; until it is known (and in a
// browser preview) the page describes it in words.
const defaultFolder = ref('')
const folderError = ref('')
const customFolder = computed(() => screenshot.value.folder?.trim() ?? '')
const shownFolder = computed(() => customFolder.value || defaultFolder.value || t('utilities.screenshots.folder.defaultLocation'))

onMounted(async () => {
  try {
    defaultFolder.value = await defaultScreenshotFolder()
  } catch {
    defaultFolder.value = ''
  }
})

async function chooseFolder() {
  folderError.value = ''
  try {
    const picked = await pickFolder(customFolder.value || defaultFolder.value)
    if (picked) screenshot.value.folder = picked
  } catch (error) {
    folderError.value = errorText(error, t('utilities.screenshots.folder.pickerFailed'))
  }
}

function useDefaultFolder() {
  folderError.value = ''
  delete screenshot.value.folder
}

async function openFolder() {
  folderError.value = ''
  try {
    await openScreenshotFolder(customFolder.value)
  } catch (error) {
    folderError.value = errorText(error, t('utilities.screenshots.folder.openFailed'))
  }
}
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel" data-utility="screenshots">
    <h2 class="text-2xl font-semibold tracking-tight">{{ t('utilities.screenshots.heading') }}</h2>
    <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t('utilities.screenshots.intro') }}</p>
    <GuideLink guide="screenshots" class="mt-2" />
    <SetupGuide class="mt-3" :title="t('utilities.screenshots.notes.title')">
      <ul class="list-disc space-y-1 pl-5 text-sm leading-6 text-muted">
        <li>{{ t('utilities.screenshots.notes.names') }}</li>
        <li>{{ t('utilities.screenshots.notes.api') }}</li>
        <li>{{ t('utilities.screenshots.notes.quadviews') }}</li>
        <li>{{ t('utilities.screenshots.notes.framegen') }}</li>
        <li>{{ t('utilities.screenshots.notes.feedback') }}</li>
      </ul>
    </SetupGuide>

    <div class="mt-4 space-y-4">
      <BindingConflictWarnings :warnings="warnings" />

      <BindingListEditor
        v-model="screenshot.bindings"
        :label="t('utilities.screenshots.binding.label')"
        :description="t('utilities.screenshots.binding.description')"
        :none-text="t('utilities.screenshots.binding.none')"
        sound-mode="single"
      />

      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <OptionCards v-model="screenshot.eyes" :legend="t('utilities.screenshots.eyes.legend')" :options="eyeOptions" />
      </div>

      <div class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div class="min-w-0 flex-1">
            <p class="flex flex-wrap items-center gap-2 text-sm font-semibold tracking-tight" style="color: var(--app-text)">
              {{ t('utilities.screenshots.folder.title') }}
              <span class="chip-idle rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.14em]">
                {{ customFolder ? t('common.custom') : t('common.default') }}
              </span>
            </p>
            <p class="mt-1 break-all font-mono text-xs leading-5" style="color: var(--app-text)" data-screenshot-folder>{{ shownFolder }}</p>
            <p class="mt-1 text-xs leading-5 text-muted">{{ t('utilities.screenshots.folder.perGame') }}</p>
            <p v-if="folderError" class="mt-1 text-xs leading-5" style="color: var(--app-danger-text)" role="alert">{{ folderError }}</p>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium" type="button" @click="chooseFolder">
              {{ t('utilities.screenshots.folder.choose') }}
            </button>
            <button
              class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
              type="button"
              :disabled="!customFolder"
              @click="useDefaultFolder"
            >
              {{ t('utilities.screenshots.folder.useDefault') }}
            </button>
            <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium" type="button" @click="openFolder">
              {{ t('utilities.screenshots.folder.open') }}
            </button>
          </div>
        </div>
      </div>
    </div>
  </article>
</template>
