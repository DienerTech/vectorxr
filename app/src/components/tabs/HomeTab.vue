<script setup lang="ts">
import { computed, ref } from 'vue'
import HealthLoading from '../HealthLoading.vue'
import DiscordJoinButton from '../DiscordJoinButton.vue'
import { t } from '../../i18n/index.ts'

import type { OpenXrLayerEntry, OpenXrLayerSnapshot } from '../../lib/commands'
import { openExternalUrl } from '../../lib/commands'
import { findVectorXrLayer } from '../../lib/health'
import type { AppTab, ModuleId, VectorXRConfig } from '../../lib/model'

const supportLinkError = ref('')

async function openSupport() {
  try {
    await openExternalUrl('https://ko-fi.com/dienertech')
    supportLinkError.value = ''
  } catch {
    supportLinkError.value = t('home.supportLinkError')
  }
}

const props = defineProps<{
  config: VectorXRConfig
  openXrLayerSnapshot: OpenXrLayerSnapshot | null
  openXrLayersLoading: boolean
}>()

defineEmits<{
  viewHealth: []
  exportDebug: []
  navigate: [tab: AppTab]
}>()

const vectorXrLayer = computed<OpenXrLayerEntry | null>(() => findVectorXrLayer(props.openXrLayerSnapshot))

const vectorXrLayerStatusLabel = computed(() => {
  if (props.openXrLayersLoading) return t('home.layerStatus.checking')
  if (!vectorXrLayer.value) return t('home.layerStatus.notFound')
  return vectorXrLayer.value.enabled ? t('home.layerStatus.enabled') : t('home.layerStatus.disabled')
})

const vectorXrLayerStatusClass = computed(() => {
  if (props.openXrLayersLoading || !vectorXrLayer.value) return 'chip-idle'
  return vectorXrLayer.value.enabled ? 'chip-success' : 'chip-warning'
})

const vectorXrLayerStatusDescription = computed(() => {
  if (props.openXrLayersLoading) {
    return t('home.layerDescription.checking')
  }

  if (!vectorXrLayer.value) {
    return t('home.layerDescription.notFound')
  }

  if (vectorXrLayer.value.enabled) {
    return t('home.layerDescription.enabled')
  }

  return t('home.layerDescription.disabled')
})

const runtimeStatusClass = computed(() => props.config.core.enabled ? 'chip-success' : 'chip-warning')
const runtimeStatusLabel = computed(() => props.config.core.enabled ? t('home.runtime.enabled') : t('home.runtime.disabled'))
const systemHealthDescription = computed(() => {
  if (!props.config.core.enabled) {
    return t('home.systemHealth.suiteOff')
  }

  if (props.openXrLayersLoading) {
    return t('home.systemHealth.checking')
  }

  if (!vectorXrLayer.value) {
    return t('home.systemHealth.layerNotFound')
  }

  if (!vectorXrLayer.value.enabled) {
    return t('home.systemHealth.layerDisabled')
  }

  return t('home.systemHealth.ready')
})

const moduleOrder: ModuleId[] = ['quadviews', 'turbo', 'framegen', 'pivotxr', 'depthxr']

const enhancementRows = computed(() =>
  moduleOrder.map((id) => {
    const module = props.config.modules[id]
    const totalCustom = module.profiles.length
    const enabledCustom = module.profiles.filter((profile) => profile.enabled).length
    return {
      id,
      label: t(`app.tabs.${id}.label`),
      defaultEnabled: module.enabled,
      enabledCustom,
      totalCustom,
      active: module.enabled || enabledCustom > 0,
    }
  }),
)

const activeEnhancementCount = computed(() => enhancementRows.value.filter((row) => row.active).length)
</script>

<template>
  <div class="space-y-5">

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('home.systemHealth.eyebrow') }}</p>
        <div class="flex flex-wrap gap-2">
          <span
            class="inline-flex cursor-default items-center justify-center whitespace-nowrap rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="runtimeStatusClass"
            :title="t('home.runtime.title')"
          >
            {{ runtimeStatusLabel }}
          </span>
          <span
            class="inline-flex cursor-default items-center justify-center whitespace-nowrap rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="vectorXrLayerStatusClass"
            :title="vectorXrLayerStatusDescription"
          >
            <HealthLoading v-if="openXrLayersLoading" />
            <template v-else>{{ vectorXrLayerStatusLabel }}</template>
          </span>
        </div>
      </div>
      <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
        {{ systemHealthDescription }}
      </p>
      <div class="mt-4 flex flex-wrap gap-2 border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('viewHealth')">
          {{ t('home.systemHealth.viewHealth') }}
        </button>
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('exportDebug')">
          {{ t('home.systemHealth.exportDebug') }}
        </button>
      </div>
    </article>

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-end justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('home.enhancements.eyebrow') }}</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ t('home.enhancements.title') }}</h2>
        </div>
        <span class="text-sm text-muted">{{ t('home.enhancements.activeCount', { active: activeEnhancementCount, total: enhancementRows.length }) }}</span>
      </div>

      <div
        class="mt-4 grid grid-cols-[minmax(0,1.6fr)_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1fr)] gap-3 border border-transparent px-3 text-[0.68rem] font-semibold uppercase tracking-[0.16em] text-soft"
      >
        <span>{{ t('home.enhancements.columns.enhancement') }}</span>
        <span class="text-center">{{ t('home.enhancements.columns.defaultProfile') }}</span>
        <span class="text-center">{{ t('home.enhancements.columns.customProfiles') }}</span>
        <span class="text-center">{{ t('home.enhancements.columns.status') }}</span>
      </div>

      <div class="mt-2 grid gap-2">
        <button
          v-for="row in enhancementRows"
          :key="row.id"
          type="button"
          class="enhancement-row grid grid-cols-[minmax(0,1.6fr)_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1fr)] items-center gap-3 rounded-[0.9rem] border px-3 py-3 text-left transition"
          :title="t('home.enhancements.open', { module: row.label })"
          @click="$emit('navigate', row.id)"
        >
          <span class="text-sm font-semibold">{{ row.label }}</span>
          <span class="text-center">
            <span
              class="inline-flex rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="row.defaultEnabled ? 'chip-success' : 'chip-idle'"
            >
              {{ row.defaultEnabled ? t('common.enabled') : t('common.disabled') }}
            </span>
          </span>
          <span class="text-center text-sm text-muted">{{ t('home.enhancements.customEnabled', { enabled: row.enabledCustom, total: row.totalCustom }) }}</span>
          <span class="text-center">
            <span
              class="inline-flex rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="row.active ? 'chip-success' : 'chip-idle'"
            >
              {{ row.active ? t('common.active') : t('common.inactive') }}
            </span>
          </span>
        </button>
      </div>

      <p class="mt-3 text-xs text-soft">{{ t('home.enhancements.hint') }}</p>
    </article>
    <article class="rounded-[1.25rem] border px-5 py-4 shadow-panel surface-panel">
      <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('home.community.eyebrow') }}</p>
      <h2 class="mt-1 text-lg font-semibold">{{ t('home.community.title') }}</h2>
      <div class="mt-2 grid gap-4 sm:grid-cols-2">
        <div>
          <h3 class="font-semibold">{{ t('home.community.discordTitle') }}</h3>
          <p class="mt-1 text-sm leading-5 text-muted">{{ t('home.community.discordBody') }}</p>
          <DiscordJoinButton class="mt-2" button-class="rounded-[0.65rem] px-3 py-1.5 text-sm font-medium" />
        </div>
        <div>
          <h3 class="font-semibold">{{ t('home.community.supportTitle') }}</h3>
          <p class="mt-1 text-sm leading-5 text-muted">{{ t('home.community.supportBody') }}</p>
          <button class="button-accent mt-2 rounded-[0.65rem] px-3 py-1.5 text-sm font-medium" type="button" @click="openSupport">{{ t('home.community.supportButton') }}</button>
          <p v-if="supportLinkError" role="status" class="mt-2 text-xs text-muted">{{ supportLinkError }}</p>
        </div>
      </div>
    </article>
  </div>
</template>
