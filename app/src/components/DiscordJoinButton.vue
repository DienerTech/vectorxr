<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { openExternalUrl } from '../lib/commands'
import { useCommunityStore } from '../stores/communityStore'
import { t } from '../i18n/index.ts'

withDefaults(defineProps<{ buttonClass?: string }>(), {
  buttonClass: 'rounded-[0.75rem] px-4 py-2 text-sm font-medium',
})

const community = useCommunityStore()
const opening = ref(false)
const openError = ref('')
const loading = computed(() => community.state.status === 'idle' || community.state.status === 'loading')
const available = computed(() => community.state.status === 'ready' && !!community.state.discordInvite)
const label = computed(() => loading.value
  ? t('app.discord.loading')
  : opening.value
    ? t('app.discord.opening')
    : available.value ? t('app.discord.join') : t('app.discord.unavailable'))
const message = computed(() => {
  if (community.state.status === 'error') return t('app.discord.loadFailed')
  if (community.state.status === 'unavailable') return t('app.discord.invitesUnavailable')
  return openError.value
})

async function openDiscord() {
  const invite = community.state.discordInvite
  if (!available.value || !invite || opening.value) return
  opening.value = true
  openError.value = ''
  try {
    await openExternalUrl(invite)
  } catch {
    openError.value = t('app.discord.openFailed')
  } finally {
    opening.value = false
  }
}

function retry() {
  openError.value = ''
  void community.load(true)
}

onMounted(() => { void community.load() })
</script>

<template>
  <div>
    <div class="flex flex-wrap items-center gap-2">
      <button
        class="button-secondary"
        :class="buttonClass"
        type="button"
        :disabled="!available || opening"
        :aria-busy="loading || opening"
        @click="openDiscord"
      >{{ label }}</button>
      <button v-if="!loading && !available" class="button-secondary rounded-lg px-2 py-1 text-xs" type="button" :aria-label="t('app.discord.retryAria')" @click="retry">{{ t('common.retry') }}</button>
    </div>
    <p role="status" aria-live="polite" :class="message ? 'mt-2 text-xs text-muted' : 'sr-only'">{{ message || (loading ? t('app.discord.loadingStatus') : '') }}</p>
  </div>
</template>
