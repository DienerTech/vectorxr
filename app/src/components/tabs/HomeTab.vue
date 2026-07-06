<script setup lang="ts">
import { computed } from 'vue'

import type { OpenXrLayerEntry, OpenXrLayerSnapshot } from '../../lib/commands'
import type { AppTab, ModuleId, VectorXRConfig } from '../../lib/model'

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

const vectorXrLayer = computed<OpenXrLayerEntry | null>(() => {
  return props.openXrLayerSnapshot?.slices
    .flatMap((slice) => slice.layers)
    .find((layer) => layer.isVectorXr) ?? null
})

const vectorXrLayerStatusLabel = computed(() => {
  if (props.openXrLayersLoading) return 'Checking'
  if (!vectorXrLayer.value) return 'Not found'
  return vectorXrLayer.value.enabled ? 'Layer enabled' : 'Layer disabled'
})

const vectorXrLayerStatusClass = computed(() => {
  if (props.openXrLayersLoading || !vectorXrLayer.value) return 'chip-idle'
  return vectorXrLayer.value.enabled ? 'chip-success' : 'chip-warning'
})

const vectorXrLayerStatusDescription = computed(() => {
  if (props.openXrLayersLoading) {
    return 'Checking the registered VectorXR API layer.'
  }

  if (!vectorXrLayer.value) {
    return "VectorXR's API layer registration was not found. Enhancements will not apply until the layer is registered and enabled."
  }

  if (vectorXrLayer.value.enabled) {
    return 'The VectorXR API layer is enabled, so Enhancements can apply at runtime.'
  }

  return 'The VectorXR API layer is disabled. None of the Enhancements will apply, regardless of suite or Enhancement enabled state.'
})

const runtimeStatusClass = computed(() => props.config.core.enabled ? 'chip-success' : 'chip-warning')
const runtimeStatusLabel = computed(() => props.config.core.enabled ? 'Runtime enabled' : 'Runtime disabled')
const systemHealthDescription = computed(() => {
  if (!props.config.core.enabled) {
    return 'Runtime effects are disabled from the suite switch. Layer status is still available for troubleshooting.'
  }

  if (props.openXrLayersLoading) {
    return 'Checking VectorXR runtime and OpenXR layer readiness.'
  }

  if (!vectorXrLayer.value) {
    return 'VectorXR is enabled, but the OpenXR API layer registration was not found.'
  }

  if (!vectorXrLayer.value.enabled) {
    return 'VectorXR is enabled, but the OpenXR API layer is disabled.'
  }

  return 'VectorXR runtime and OpenXR layer status look ready for future OpenXR app launches.'
})

const moduleMeta: { id: ModuleId; label: string }[] = [
  { id: 'quadviews', label: 'Quadviews' },
  { id: 'turbo', label: 'Turbo' },
  { id: 'pivotxr', label: 'Pivot' },
  { id: 'depthxr', label: 'Depth' },
]

const enhancementRows = computed(() =>
  moduleMeta.map(({ id, label }) => {
    const module = props.config.modules[id]
    const totalCustom = module.profiles.length
    const enabledCustom = module.profiles.filter((profile) => profile.enabled).length
    return {
      id,
      label,
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
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">System Health</p>
        <div class="flex flex-wrap gap-2">
          <span
            class="inline-flex cursor-default items-center justify-center whitespace-nowrap rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="runtimeStatusClass"
            title="Suite-level runtime switch state."
          >
            {{ runtimeStatusLabel }}
          </span>
          <span
            class="inline-flex cursor-default items-center justify-center whitespace-nowrap rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="vectorXrLayerStatusClass"
            :title="vectorXrLayerStatusDescription"
          >
            {{ vectorXrLayerStatusLabel }}
          </span>
        </div>
      </div>
      <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
        {{ systemHealthDescription }}
      </p>
      <div class="mt-4 flex flex-wrap gap-2 border-t pt-4" style="border-color: var(--app-border)">
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('viewHealth')">
          View Health
        </button>
        <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('exportDebug')">
          Export Debug
        </button>
      </div>
    </article>

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-end justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Enhancements</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Active Overview</h2>
        </div>
        <span class="text-sm text-muted">{{ activeEnhancementCount }} of {{ enhancementRows.length }} active</span>
      </div>

      <div
        class="mt-4 grid grid-cols-[minmax(0,1.6fr)_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1fr)] gap-3 border border-transparent px-3 text-[0.68rem] font-semibold uppercase tracking-[0.16em] text-soft"
      >
        <span>Enhancement</span>
        <span class="text-center">Default profile</span>
        <span class="text-center">Custom profiles</span>
        <span class="text-center">Status</span>
      </div>

      <div class="mt-2 grid gap-2">
        <button
          v-for="row in enhancementRows"
          :key="row.id"
          type="button"
          class="enhancement-row grid grid-cols-[minmax(0,1.6fr)_minmax(0,1fr)_minmax(0,1fr)_minmax(0,1fr)] items-center gap-3 rounded-[0.9rem] border px-3 py-3 text-left transition"
          :title="`Open ${row.label} settings`"
          @click="$emit('navigate', row.id)"
        >
          <span class="text-sm font-semibold">{{ row.label }}</span>
          <span class="text-center">
            <span
              class="inline-flex rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="row.defaultEnabled ? 'chip-success' : 'chip-idle'"
            >
              {{ row.defaultEnabled ? 'Enabled' : 'Disabled' }}
            </span>
          </span>
          <span class="text-center text-sm text-muted">{{ row.enabledCustom }} / {{ row.totalCustom }} enabled</span>
          <span class="text-center">
            <span
              class="inline-flex rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="row.active ? 'chip-success' : 'chip-idle'"
            >
              {{ row.active ? 'Active' : 'Inactive' }}
            </span>
          </span>
        </button>
      </div>

      <p class="mt-3 text-xs text-soft">Select an Enhancement to open its settings.</p>
    </article>
  </div>
</template>
