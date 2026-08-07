<script setup lang="ts">
import { computed } from 'vue'

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
</script>

<template>
  <span
    v-if="excludedApplications.length"
    class="inline-flex max-w-full flex-wrap items-center gap-x-1 rounded-full border px-3 py-2 text-xs text-muted"
    style="border-color: var(--app-border)"
    :title="`Enabled custom profiles take priority for: ${excludedApplications.map((application) => application.name).join(', ')}`"
  >
    <span class="font-semibold" style="color: var(--app-text)">Does not apply to:</span>
    <span>{{ excludedApplications.map((application) => application.name).join(', ') }}</span>
  </span>
</template>
