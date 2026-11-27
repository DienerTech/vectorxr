<script setup lang="ts">
import { computed, ref } from 'vue'

import SeenAppDetailsModal from './SeenAppDetailsModal.vue'
import { t } from '../i18n/index.ts'
import type { SeenApplication } from '../lib/commands'
import { normalizeExeName, type RegisteredApplication } from '../lib/model'

const props = defineProps<{
  applications: RegisteredApplication[]
  trackingEnabled: boolean
  seenApps: SeenApplication[]
  seenAppsLoading: boolean
}>()

defineEmits<{
  addSeen: [exe: string]
  clearSeen: []
  refreshSeen: []
}>()

const seenAppViews = computed(() =>
  props.seenApps.map((seenApp) => {
    const registeredApplication = props.applications.find((application) => normalizeExeName(application.match.exe) === normalizeExeName(seenApp.exe))
    return {
      ...seenApp,
      registeredApplication,
    }
  }),
)

// Unregistered apps are the actionable ones (promotion is the point); already
// registered apps are "filed" and only kept around for reference.
const unregisteredSeenApps = computed(() => seenAppViews.value.filter((seenApp) => !seenApp.registeredApplication))
const registeredSeenApps = computed(() => seenAppViews.value.filter((seenApp) => seenApp.registeredApplication))

type SeenAppView = SeenApplication & { registeredApplication: RegisteredApplication | undefined }

const detailsApp = ref<SeenAppView | null>(null)
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div>
        <div class="flex flex-wrap items-center gap-2">
          <h2 class="text-2xl font-semibold tracking-tight">{{ t('registry.discovery.eyebrow') }}</h2>
          <span
            class="rounded-full px-2.5 py-0.5 text-xs font-semibold uppercase tracking-[0.14em]"
            :class="trackingEnabled ? 'chip-success' : 'chip-warning'"
            :title="trackingEnabled
              ? t('registry.discovery.trackingOnTitle')
              : t('registry.discovery.trackingOffTitle')"
          >
            {{ trackingEnabled ? t('registry.discovery.trackingOn') : t('registry.discovery.trackingOff') }}
          </span>
        </div>
        <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
          {{ t('registry.discovery.intro') }}
        </p>
      </div>

      <div class="flex flex-wrap gap-2">
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('refreshSeen')">
          {{ t('common.refresh') }}
        </button>
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('clearSeen')">
          {{ t('registry.discovery.clear') }}
        </button>
      </div>
    </div>

    <div v-if="seenAppsLoading" class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ t('registry.discovery.loading') }}
    </div>

    <template v-else-if="seenAppViews.length > 0">
      <!-- Actionable: unregistered discovered apps lead with a clear Register action -->
      <div v-if="unregisteredSeenApps.length > 0" class="mt-4 space-y-3">
        <div
          v-for="seenApp in unregisteredSeenApps"
          :key="seenApp.exe"
          class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft"
        >
          <div class="min-w-0">
            <p class="font-mono text-base font-semibold tracking-tight">{{ seenApp.exe }}</p>
            <p class="mt-1 text-sm text-muted">
              {{ t('registry.discovery.launches', { count: seenApp.launchCount }) }}
            </p>
          </div>

          <div class="flex flex-wrap items-center gap-2">
            <button
              class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium"
              type="button"
              @click="detailsApp = seenApp"
            >
              {{ t('registry.discovery.details') }}
            </button>
            <button
              class="button-accent rounded-[0.75rem] px-5 py-2 text-sm font-semibold"
              type="button"
              @click="$emit('addSeen', seenApp.exe)"
            >
              {{ t('registry.discovery.register') }}
            </button>
          </div>
        </div>
      </div>

      <p v-else class="mt-4 rounded-[1rem] border border-dashed px-4 py-4 text-center text-sm text-muted surface-panel-soft">
        <template v-if="trackingEnabled">
          {{ t('registry.discovery.allRegistered') }}
        </template>
        <template v-else>
          {{ t('registry.discovery.allRegisteredTrackingOff') }}
        </template>
      </p>

      <!-- Filed: already-registered apps recede; they are done -->
      <div v-if="registeredSeenApps.length > 0" class="mt-5">
        <p class="text-xs uppercase tracking-[0.18em] text-muted">{{ t('registry.discovery.alreadyRegistered') }}</p>
        <div class="mt-2 space-y-1.5">
          <div
            v-for="seenApp in registeredSeenApps"
            :key="seenApp.exe"
            class="flex flex-wrap items-center justify-between gap-3 rounded-[0.85rem] border px-4 py-2.5 surface-panel-soft opacity-60 transition hover:opacity-100"
          >
            <div class="flex min-w-0 flex-wrap items-center gap-2">
              <span class="font-mono text-sm font-medium">{{ seenApp.exe }}</span>
              <span class="chip-success rounded-full px-2.5 py-0.5 text-xs font-medium">
                {{ t('registry.discovery.registeredAs', { name: seenApp.registeredApplication?.name ?? '' }) }}
              </span>
            </div>
            <button
              class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium"
              type="button"
              @click="detailsApp = seenApp"
            >
              {{ t('registry.discovery.details') }}
            </button>
          </div>
        </div>
      </div>
    </template>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      <template v-if="trackingEnabled">
        {{ t('registry.discovery.empty') }}
      </template>
      <template v-else>
        {{ t('registry.discovery.emptyTrackingOff') }}
      </template>
    </div>
  </article>

  <SeenAppDetailsModal
    :open="detailsApp !== null"
    :app="detailsApp"
    :registered-name="detailsApp?.registeredApplication?.name ?? null"
    @close="detailsApp = null"
  />
</template>
