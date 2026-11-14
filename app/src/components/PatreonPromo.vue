<script setup lang="ts">
// The Patreon promotion beside the Ko-fi buttons. It renders nothing unless
// the community config (community.json on master, or a local override) turns
// it on with a valid Patreon URL, so a failed fetch or a disabled config hides it.
// "column" is a titled column like Home's Discord and Ko-fi ones; "stacked"
// sits under About's Ko-fi button.
import { computed, onMounted, ref } from 'vue'
import { openExternalUrl } from '../lib/commands'
import { patreonMessageFor, useCommunityStore } from '../stores/communityStore'
import { activeLocale, t } from '../i18n/index.ts'

withDefaults(defineProps<{ variant?: 'column' | 'stacked'; buttonClass?: string; messageClass?: string }>(), {
  variant: 'stacked',
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
  <div v-if="promo" data-patreon-promo :class="variant === 'column' ? 'flex flex-col' : ''">
    <h3 v-if="variant === 'column'" class="font-semibold">{{ t('app.patreon.title') }}</h3>
    <p v-if="message" :class="[messageClass, variant === 'column' ? 'mb-2 mt-1' : '']">{{ message }}</p>
    <button class="button-accent" :class="[buttonClass, variant === 'column' ? 'mt-auto self-start' : 'mt-2']" type="button" :disabled="opening" @click="openPatreon">{{ t('app.patreon.join') }}</button>
    <p v-if="openError" role="status" class="mt-2 text-xs text-muted">{{ openError }}</p>
  </div>
</template>
