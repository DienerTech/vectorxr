<script setup lang="ts">
import { computed, ref } from 'vue'

import SeenAppDetailsModal from './SeenAppDetailsModal.vue'
import { hasMessage, t } from '../i18n/index.ts'
import type { SeenApplication } from '../lib/commands'
import { moduleLabels, moduleStateForApplication, type ModuleId, type RegisteredApplication, type VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
  seenApps: SeenApplication[]
  seenAppsLoading: boolean
}>()

defineEmits<{
  add: []
  addSeen: [exe: string]
  clearSeen: []
  refreshSeen: []
  remove: [index: number]
  openModule: [moduleId: ModuleId, applicationId: string]
}>()

const moduleIds: ModuleId[] = ['quadviews', 'turbo', 'framegen', 'pivotxr', 'depthxr']

function moduleChip(applicationId: string, moduleId: ModuleId) {
  const moduleState = moduleStateForApplication(props.config, moduleId, applicationId)
  const labelKey = `model.modules.${moduleId}`
  const module = hasMessage(labelKey) ? t(labelKey) : moduleLabels[moduleId]

  switch (moduleState.kind) {
    case 'default-off':
      return {
        text: t('registry.applications.chip.defaultOff', { module }),
        chipClass: 'chip-idle',
        title: t('registry.applications.chip.defaultOffTitle', { module }),
      }
    case 'custom': {
      const profile = moduleState.profileName ?? ''
      return {
        text: t('registry.applications.chip.custom', { module, profile }),
        chipClass: 'chip-success',
        title: t('registry.applications.chip.customTitle', { module, profile }),
      }
    }
    default:
      return {
        text: t('registry.applications.chip.default', { module }),
        chipClass: 'chip-idle',
        title: t('registry.applications.chip.defaultTitle', { module }),
      }
  }
}

function normalizeExe(value: string): string {
  return value
    .trim()
    .split(/[/\\]/)
    .pop()
    ?.toLowerCase() ?? ''
}

const seenAppViews = computed(() =>
  props.seenApps.map((seenApp) => {
    const registeredApplication = props.applications.find((application) => normalizeExe(application.match.exe) === normalizeExe(seenApp.exe))
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
const editingApplicationId = ref<string | null>(null)

function finishNameEdit() {
  editingApplicationId.value = null
}
</script>

<template>
  <div class="space-y-5">
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div>
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('registry.applications.eyebrow') }}</p>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('registry.applications.title') }}</h2>
        <p class="mt-2 text-sm leading-6 text-muted">
          {{ t('registry.applications.intro') }}
        </p>
      </div>

      <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('add')">
        {{ t('registry.applications.add') }}
      </button>
    </div>

    <div v-if="applications.length > 0" class="mt-4 grid gap-3 xl:grid-cols-2">
      <div v-for="(application, index) in applications" :key="application.id" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div class="flex flex-wrap items-center gap-2">
            <p v-if="editingApplicationId !== application.id" class="text-base font-semibold tracking-tight">{{ application.name || t('registry.applications.unnamed') }}</p>
            <input
              v-else
              v-model="application.name"
              class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-base font-semibold tracking-tight"
              placeholder="DCS"
              type="text"
              @blur="finishNameEdit"
              @keydown.enter="finishNameEdit"
            />
            <button
              v-if="editingApplicationId !== application.id"
              class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]"
              type="button"
              :aria-label="t('registry.applications.editName')"
              @click="editingApplicationId = application.id"
            >
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
              </svg>
            </button>
          </div>

          <div class="flex flex-wrap items-center gap-3">
            <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('remove', index)">
              {{ t('common.remove') }}
            </button>
          </div>
        </div>

        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('registry.applications.executable') }}
            <span
              :title="t('registry.applications.executableHelp')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <!-- i18n-ignore: an example file name, not text -->
          <input v-model="application.match.exe" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" placeholder="DCS.exe" type="text" />
        </label>

        <div class="mt-3 flex flex-wrap items-center gap-2">
          <button
            v-for="moduleId in moduleIds"
            :key="moduleId"
            class="rounded-full px-3 py-1 text-xs font-medium transition hover:brightness-110"
            :class="moduleChip(application.id, moduleId).chipClass"
            type="button"
            :title="moduleChip(application.id, moduleId).title"
            @click="$emit('openModule', moduleId, application.id)"
          >
            {{ moduleChip(application.id, moduleId).text }}
          </button>
        </div>
      </div>
    </div>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ t('registry.applications.empty') }}
    </div>
  </article>

  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-start justify-between gap-3">
      <div>
        <div class="flex flex-wrap items-center gap-2">
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('registry.discovery.eyebrow') }}</p>
          <span
            class="rounded-full px-2.5 py-0.5 text-xs font-semibold uppercase tracking-[0.14em]"
            :class="config.core.trackSeenApps ? 'chip-success' : 'chip-warning'"
            :title="config.core.trackSeenApps
              ? t('registry.discovery.trackingOnTitle')
              : t('registry.discovery.trackingOffTitle')"
          >
            {{ config.core.trackSeenApps ? t('registry.discovery.trackingOn') : t('registry.discovery.trackingOff') }}
          </span>
        </div>
        <h3 class="mt-1 text-lg font-semibold tracking-tight">{{ t('registry.discovery.title') }}</h3>
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
        <template v-if="config.core.trackSeenApps">
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
      <template v-if="config.core.trackSeenApps">
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
  </div>
</template>
