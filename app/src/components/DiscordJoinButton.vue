<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { openExternalUrl } from '../lib/commands'
import { useCommunityStore } from '../stores/communityStore'

withDefaults(defineProps<{ buttonClass?: string }>(), {
  buttonClass: 'rounded-[0.75rem] px-4 py-2 text-sm font-medium',
})

const community = useCommunityStore()
const opening = ref(false)
const openError = ref('')
const loading = computed(() => community.state.status === 'idle' || community.state.status === 'loading')
const available = computed(() => community.state.status === 'ready' && !!community.state.discordInvite)
const label = computed(() => loading.value ? 'Loading Discord…' : opening.value ? 'Opening Discord…' : available.value ? 'Join Discord ↗' : 'Discord unavailable')
const message = computed(() => {
  if (community.state.status === 'error') return 'Could not load the Discord invite. Try again.'
  if (community.state.status === 'unavailable') return 'Discord invitations are currently unavailable.'
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
    openError.value = 'Could not open the browser. Please try again.'
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
      <button v-if="!loading && !available" class="button-secondary rounded-lg px-2 py-1 text-xs" type="button" aria-label="Retry loading Discord invite" @click="retry">Retry</button>
    </div>
    <p role="status" aria-live="polite" :class="message ? 'mt-2 text-xs text-muted' : 'sr-only'">{{ message || (loading ? 'Loading Discord invite.' : '') }}</p>
  </div>
</template>
