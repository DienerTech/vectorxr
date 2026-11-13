<script setup lang="ts">
// The Patreon promotion beside the Ko-fi buttons. It renders nothing unless
// the community config (community.json on master, or a local override) turns
// it on with a valid Patreon URL, so a failed fetch or a disabled config hides it.
import { computed, onMounted, ref } from 'vue'
import { openExternalUrl } from '../lib/commands'
import { patreonMessageFor, useCommunityStore } from '../stores/communityStore'
import { activeLocale, t } from '../i18n/index.ts'

withDefaults(defineProps<{ buttonClass?: string; messageClass?: string }>(), {
  buttonClass: 'rounded-[0.65rem] px-3 py-1.5 text-sm font-medium',
  messageClass: 'text-sm leading-5 text-muted',
})

const community = useCommunityStore()
const opening = ref(false)
const openError = ref('')
const promo = computed(() => community.state.patreon)
const message = computed(() => patreonMessageFor(promo.value, activeLocale.value))

async function openPatreon() {
  const url = promo.value?.url
  if (!url || opening.value) return
  opening.value = true
  openError.value = ''
  try {
    await openExternalUrl(url)
  } catch {
    openError.value = t('app.patreon.openFailed')
  } finally {
    opening.value = false
  }
}

onMounted(() => { void community.load() })
</script>

<template>
  <div v-if="promo" data-patreon-promo>
    <p v-if="message" :class="messageClass">{{ message }}</p>
    <button class="button-secondary mt-2" :class="buttonClass" type="button" :disabled="opening" @click="openPatreon">{{ t('app.patreon.join') }}</button>
    <p v-if="openError" role="status" class="mt-2 text-xs text-muted">{{ openError }}</p>
  </div>
</template>
