<script setup lang="ts">
import { t } from '../i18n/index.ts'
</script>

<template>
  <span class="inline-flex items-center gap-1.5 text-xs text-muted" role="status" :title="t('app.healthLoading.label')" :aria-label="t('app.healthLoading.label')">
    <svg class="h-3.5 w-3.5 animate-spin" viewBox="0 0 24 24" fill="none" aria-hidden="true"><circle cx="12" cy="12" r="9" stroke="currentColor" stroke-width="3" opacity="0.25" /><path d="M12 3a9 9 0 0 1 9 9" stroke="currentColor" stroke-width="3" stroke-linecap="round" /></svg>
    <span>{{ t('app.healthLoading.checking') }}</span>
  </span>
</template>
