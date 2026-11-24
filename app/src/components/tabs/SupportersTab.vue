<script setup lang="ts">
import { computed, onMounted } from 'vue'

import { t } from '../../i18n/index.ts'
import { useCommunityStore } from '../../stores/communityStore'
import { useSupportersStore } from '../../stores/supportersStore.ts'
import PatreonPromo from '../PatreonPromo.vue'

const supporters = useSupportersStore()
// The Patreon promotion's switch (community.json): while it is on, no
// published list means no one is credited yet, not that the Patreon is coming.
const community = useCommunityStore()
const patreonLive = computed(() => community.state.patreon !== null)
const tiers = computed(() => supporters.state.tiers.filter((tier) => tier.names.length > 0))
const loading = computed(() => supporters.state.status === 'idle' || supporters.state.status === 'loading')

onMounted(() => {
  void supporters.load()
  void community.load()
})
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel" aria-labelledby="supporters-title">
    <div class="flex flex-wrap items-start justify-between gap-4">
      <div>
        <h2 id="supporters-title" class="text-2xl font-semibold tracking-tight">{{ t('app.tabs.supporters.label') }}</h2>
        <p v-if="supporters.state.status !== 'unpublished'" class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t('app.supporters.thanks') }}</p>
      </div>
      <button
        class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium"
        type="button"
        :disabled="loading"
        @click="supporters.load(true)"
      >{{ t('common.refresh') }}</button>
    </div>

    <div role="status" class="text-sm leading-6 text-muted">
      <p v-if="loading" class="mt-4">{{ t('common.loading') }}</p>
      <p v-else-if="supporters.state.status === 'unavailable'" class="mt-4">{{ t('app.supporters.unavailable') }}</p>
      <p v-else-if="supporters.state.status === 'unpublished'" class="mt-4">
        {{ patreonLive ? t('app.supporters.joinFirst') : t('app.supporters.comingSoon') }}
      </p>
      <p v-else-if="supporters.state.status === 'cached'" class="mt-4">{{ t('app.supporters.cached') }}</p>
      <p v-if="!loading && supporters.state.status !== 'unavailable' && supporters.state.status !== 'unpublished' && !tiers.length" class="mt-4">{{ t('app.supporters.empty') }}</p>
    </div>
    <PatreonPromo v-if="supporters.state.status === 'unpublished'" class="mt-1" />

    <section
      v-for="(tier, index) in tiers"
      :key="index"
      class="mt-5 border-t pt-4"
      style="border-color: var(--app-border)"
      :aria-labelledby="`supporter-tier-${index}`"
    >
      <h3 :id="`supporter-tier-${index}`" class="accent-text text-sm font-semibold [overflow-wrap:anywhere]"><bdi>{{ tier.label }}</bdi></h3>
      <ul class="mt-3 grid gap-x-5 gap-y-2 sm:grid-cols-2 xl:grid-cols-3" role="list">
        <li v-for="(name, position) in tier.names" :key="position" class="min-w-0 text-lg font-semibold leading-7 tracking-tight [overflow-wrap:anywhere]">
          <bdi>{{ name }}</bdi>
        </li>
      </ul>
    </section>
  </article>

  <section class="mt-5 rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel" aria-labelledby="community-thanks-title">
    <h2 id="community-thanks-title" class="text-xl font-semibold tracking-tight">{{ t('app.supporters.communityTitle') }}</h2>
    <div class="mt-2 max-w-3xl space-y-2 text-sm leading-6 text-muted">
      <p>{{ t('app.supporters.communityThanks') }}</p>
      <p>{{ t('app.supporters.forumsThanks') }}</p>
    </div>
  </section>
</template>
