<script setup lang="ts">
import { computed } from 'vue'

import { formatList, t } from '../i18n/index.ts'
import type { RegisteredApplication } from '../lib/model'

interface ProfileAssignment {
  enabled: boolean
  applicationIds: string[]
}

const props = defineProps<{
  applications: RegisteredApplication[]
  profiles: ProfileAssignment[]
}>()

const excludedApplications = computed(() => {
  const excludedIds = new Set(
    props.profiles
      .filter((profile) => profile.enabled)
      .flatMap((profile) => profile.applicationIds),
  )

  return props.applications.filter((application) => excludedIds.has(application.id))
})

const excludedNames = computed(() => formatList(excludedApplications.value.map((application) => application.name)))
</script>

<template>
  <span
    v-if="excludedApplications.length"
    class="inline-flex max-w-full flex-wrap items-center gap-x-1 rounded-full border px-3 py-2 text-xs text-muted"
    style="border-color: var(--app-border)"
    :title="t('registry.exclusions.title', { apps: excludedNames })"
  >
    <span class="font-semibold" style="color: var(--app-text)">{{ t('registry.exclusions.label') }}</span>
    <span>{{ excludedNames }}</span>
  </span>
</template>
