<script setup lang="ts">
// "Read the guide" on a feature's page: opens that feature's guide on the
// Guides page.
import { t } from '../i18n/index.ts'
import { guideEntry, showGuide, type GuideId } from '../lib/guides'
import { useConfigStore } from '../stores/configStore'

const props = defineProps<{ guide: GuideId }>()
const store = useConfigStore()

function open() {
  showGuide(props.guide, guideEntry(props.guide)?.tab ?? null)
  store.setActiveTab('guides')
}
</script>

<template>
  <button class="guide-link inline-flex items-center gap-1 text-xs font-semibold" type="button" :data-guide-link="guide" @click="open">
    <svg aria-hidden="true" viewBox="0 0 20 20" fill="currentColor" class="h-3.5 w-3.5">
      <path d="M4 3.5A1.5 1.5 0 0 1 5.5 2H15a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H6a1 1 0 0 0 0 2h9a1 1 0 1 1 0 2H6a3 3 0 0 1-3-3V3.5Zm3 1.5a1 1 0 0 0 0 2h6a1 1 0 1 0 0-2H7Z" />
    </svg>
    {{ t('guides.readGuide') }}
  </button>
</template>

<style scoped>
.guide-link {
  color: var(--app-accent-soft-text);
}
.guide-link:hover {
  text-decoration: underline;
}
</style>
