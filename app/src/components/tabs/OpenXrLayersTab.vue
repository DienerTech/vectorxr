<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'

import {
  loadOpenXrLayers,
  moveOpenXrLayer,
  openFileDirectory,
  setOpenXrLayerEnabled,
  type OpenXrLayerEntry,
  type OpenXrLayerMoveDirection,
  type OpenXrLayerRegistrySlice,
  type OpenXrLayerRegistrySliceId,
  type OpenXrLayerSignatureStatus,
  type OpenXrLayerSnapshot,
} from '../../lib/commands'

const snapshot = ref<OpenXrLayerSnapshot | null>(null)
const activeSliceId = ref<OpenXrLayerRegistrySliceId>('hklm64')
const selectedLayer = ref<OpenXrLayerEntry | null>(null)
const loading = ref(false)
const busyKey = ref<string | null>(null)
const status = ref('')

const activeSlice = computed(() => snapshot.value?.slices.find((slice) => slice.id === activeSliceId.value) ?? null)
const uncommonLayerCount = computed(
  () => snapshot.value?.slices.filter((slice) => slice.uncommon).reduce((count, slice) => count + slice.layers.length, 0) ?? 0,
)
const enabledLayerCount = computed(() => activeSlice.value?.layers.filter((layer) => layer.enabled).length ?? 0)
const vectorQuadWarning = computed(() => {
  const warnings: string[] = []

  for (const slice of snapshot.value?.slices ?? []) {
    const vector = slice.layers.find((layer) => layer.isVectorXr)
    const quad = slice.layers.find(isQuadViewsLayer)

    if (vector && quad && vector.order < quad.order) {
      warnings.push(`${slice.label}: Quad-Views-Foveated should be above VectorXR for Pivot compatibility.`)
    }
  }

  return warnings
})

watch(snapshot, (value) => {
  if (value && !value.slices.some((slice) => slice.id === activeSliceId.value)) {
    activeSliceId.value = 'hklm64'
  }
})

onMounted(() => {
  void refresh()
})

async function refresh() {
  loading.value = true
  status.value = ''

  try {
    snapshot.value = await loadOpenXrLayers()
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Failed to load OpenXR layers'
  } finally {
    loading.value = false
  }
}

async function toggleLayer(slice: OpenXrLayerRegistrySliceId, layer: OpenXrLayerEntry) {
  const key = actionKey(layer, 'toggle')
  busyKey.value = key
  status.value = ''

  try {
    snapshot.value = await setOpenXrLayerEnabled(slice, layer.manifestPath, !layer.enabled)
    selectedLayer.value = findLayer(slice, layer.manifestPath)
    status.value = `${layer.enabled ? 'Disabled' : 'Enabled'} ${layer.layerName}`
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Failed to update OpenXR layer'
  } finally {
    busyKey.value = null
  }
}

async function moveLayer(slice: OpenXrLayerRegistrySliceId, layer: OpenXrLayerEntry, direction: OpenXrLayerMoveDirection) {
  const key = actionKey(layer, direction)
  busyKey.value = key
  status.value = ''

  try {
    snapshot.value = await moveOpenXrLayer(slice, layer.manifestPath, direction)
    selectedLayer.value = findLayer(slice, layer.manifestPath)
    status.value = `Moved ${layer.layerName} ${direction}`
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Failed to reorder OpenXR layer'
  } finally {
    busyKey.value = null
  }
}

async function openFolder(path: string | undefined) {
  if (!path) {
    return
  }

  try {
    await openFileDirectory(path)
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Unable to open folder'
  }
}

function findLayer(slice: OpenXrLayerRegistrySliceId, manifestPath: string): OpenXrLayerEntry | null {
  return snapshot.value?.slices.find((item) => item.id === slice)?.layers.find((layer) => layer.manifestPath === manifestPath) ?? null
}

function actionKey(layer: OpenXrLayerEntry, action: string): string {
  return `${layer.slice}:${layer.manifestPath}:${action}`
}

function isQuadViewsLayer(layer: OpenXrLayerEntry): boolean {
  const value = `${layer.layerName} ${layer.description} ${layer.manifestPath}`.toLowerCase()
  return value.includes('quad-views') || value.includes('quad_views') || value.includes('quadviews')
}

function sliceDescription(slice: OpenXrLayerRegistrySlice): string {
  if (slice.recommended) {
    return 'Recommended for most Windows PCVR layers. Order below applies only inside this registry slice.'
  }

  if (slice.id.includes('32')) {
    return 'Uncommon: usually only relevant for legacy 32-bit OpenXR applications.'
  }

  return 'Uncommon: valid for per-user registrations, but separate from machine-wide ordering.'
}

function signatureLabel(status: OpenXrLayerSignatureStatus): string {
  switch (status) {
    case 'signed':
      return 'Signed'
    case 'unsigned':
      return 'Unsigned'
    case 'invalid':
      return 'Signature invalid'
    default:
      return 'Unknown'
  }
}

function signatureChipClass(status: OpenXrLayerSignatureStatus): string {
  switch (status) {
    case 'signed':
      return 'chip-success'
    case 'unsigned':
    case 'invalid':
      return 'chip-warning'
    default:
      return 'chip-idle'
  }
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-4 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">System OpenXR</p>
          <h2 class="mt-1 text-2xl font-semibold tracking-tight">OpenXR Layer Manager</h2>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <span v-if="activeSlice" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">
            {{ enabledLayerCount }}/{{ activeSlice.layers.length }} enabled
          </span>
          <span v-if="uncommonLayerCount > 0" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-warning">
            {{ uncommonLayerCount }} uncommon
          </span>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" :disabled="loading" @click="refresh">
            {{ loading ? 'Refreshing...' : 'Refresh' }}
          </button>
        </div>
      </div>

      <p class="mt-2 max-w-4xl text-sm leading-6 text-muted">
        Toggle implicit API layers and adjust order within one registry hive at a time. Changes apply to future XR app launches.
      </p>

      <div
        v-if="vectorQuadWarning.length > 0"
        class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        <span class="font-medium">Pivot compatibility:</span>
        {{ vectorQuadWarning.join(' ') }}
      </div>

      <div v-if="status" class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 surface-panel-strong">
        {{ status }}
      </div>
    </article>

    <section v-if="snapshot" class="rounded-[1.25rem] border p-3 shadow-panel surface-panel">
      <div class="grid gap-2 md:grid-cols-4">
        <button
          v-for="slice in snapshot.slices"
          :key="slice.id"
          class="rounded-[0.9rem] border px-3 py-2 text-left transition"
          :class="activeSliceId === slice.id ? 'tab-button-active' : 'tab-button-idle'"
          type="button"
          @click="activeSliceId = slice.id"
        >
          <div class="flex items-start justify-between gap-2">
            <div>
              <p class="text-sm font-semibold tracking-tight">{{ slice.label }}</p>
              <p class="mt-0.5 text-xs" :class="activeSliceId === slice.id ? 'text-inverse-muted' : 'text-muted'">
                {{ slice.recommended ? 'Recommended' : 'Uncommon' }}
              </p>
            </div>
            <span
              class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="slice.layers.length > 0 ? 'chip-accent' : 'chip-idle'"
            >
              {{ slice.layers.length }}
            </span>
          </div>
        </button>
      </div>

      <article v-if="activeSlice" class="mt-3 rounded-[1rem] border p-3 surface-panel-strong">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <div>
            <div class="flex flex-wrap items-center gap-2">
              <h3 class="text-lg font-semibold tracking-tight">{{ activeSlice.label }}</h3>
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="activeSlice.recommended ? 'chip-success' : 'chip-warning'">
                {{ activeSlice.recommended ? 'Recommended' : 'Uncommon' }}
              </span>
              <span v-if="activeSlice.requiresElevationForWrites" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-idle">
                Admin writes
              </span>
            </div>
            <p class="mt-1 text-sm leading-6 text-muted">{{ sliceDescription(activeSlice) }}</p>
          </div>
          <p class="max-w-full truncate font-mono text-xs text-soft">{{ activeSlice.registryPath }}</p>
        </div>

        <div v-if="activeSlice.layers.length === 0" class="mt-3 rounded-[0.9rem] border border-dashed px-5 py-5 text-center text-sm surface-panel-soft">
          No implicit OpenXR layers are registered in this slice.
        </div>

        <div v-else class="mt-3 overflow-hidden rounded-[0.9rem] border" style="border-color: var(--app-border)">
          <article
            v-for="(layer, index) in activeSlice.layers"
            :key="`${activeSlice.id}-${layer.manifestPath}`"
            class="grid gap-3 border-b px-3 py-2 last:border-b-0 surface-panel"
            style="border-color: var(--app-border)"
          >
            <div class="grid gap-3 lg:grid-cols-[8rem_minmax(0,1fr)_auto] lg:items-center">
              <button
                class="rounded-[0.75rem] border px-3 py-2 text-left transition"
                :class="layer.enabled ? 'chip-success' : 'chip-idle'"
                type="button"
                :disabled="busyKey !== null"
                @click="toggleLayer(activeSlice.id, layer)"
              >
                <span class="block text-xs font-semibold uppercase tracking-[0.16em]">{{ layer.enabled ? 'Enabled' : 'Disabled' }}</span>
                <span class="mt-0.5 block text-xs">{{ layer.enabled ? 'Click to disable' : 'Click to enable' }}</span>
              </button>

              <div class="min-w-0">
                <div class="flex flex-wrap items-center gap-2">
                  <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-accent">#{{ layer.order }}</span>
                  <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]" :class="signatureChipClass(layer.signatureStatus)">
                    {{ signatureLabel(layer.signatureStatus) }}
                  </span>
                  <span v-if="layer.isVectorXr" class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-success">VectorXR</span>
                  <span v-if="isQuadViewsLayer(layer)" class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">Quadviews</span>
                  <span v-if="!layer.manifestExists || !layer.libraryExists || layer.error" class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">
                    Needs attention
                  </span>
                </div>
                <div class="mt-1 flex min-w-0 flex-wrap items-baseline gap-x-3 gap-y-1">
                  <h4 class="truncate text-base font-semibold tracking-tight">{{ layer.layerName }}</h4>
                  <p class="min-w-0 flex-1 truncate text-sm text-muted">{{ layer.description }}</p>
                </div>
              </div>

              <div class="flex flex-wrap gap-2 lg:justify-end">
                <button class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium" type="button" :disabled="busyKey !== null || index === 0" @click="moveLayer(activeSlice.id, layer, 'up')">
                  Up
                </button>
                <button class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium" type="button" :disabled="busyKey !== null || index === activeSlice.layers.length - 1" @click="moveLayer(activeSlice.id, layer, 'down')">
                  Down
                </button>
                <button class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium" type="button" @click="selectedLayer = layer">
                  Details
                </button>
              </div>
            </div>
          </article>
        </div>
      </article>
    </section>

    <div v-else class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ loading ? 'Reading OpenXR layer registrations...' : 'OpenXR layer state has not loaded yet.' }}
    </div>

    <div v-if="selectedLayer" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[820px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div class="min-w-0">
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Layer Details</p>
            <h2 class="mt-2 truncate text-xl font-semibold tracking-tight">{{ selectedLayer.layerName }}</h2>
            <p class="mt-1 text-sm leading-6 text-muted">{{ selectedLayer.description }}</p>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="selectedLayer = null">
            Close
          </button>
        </div>

        <div class="mt-5 grid gap-3 md:grid-cols-2">
          <div class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">Manifest</p>
            <p class="mt-2 break-all font-mono text-xs">{{ selectedLayer.manifestPath }}</p>
            <div class="mt-3 flex flex-wrap items-center gap-2">
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="selectedLayer.manifestExists ? 'chip-success' : 'chip-warning'">
                {{ selectedLayer.manifestExists ? 'Found' : 'Missing' }}
              </span>
              <button class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium" type="button" @click="openFolder(selectedLayer.manifestPath)">
                Open Manifest Folder
              </button>
            </div>
          </div>

          <div class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">DLL</p>
            <p class="mt-2 break-all font-mono text-xs">{{ selectedLayer.libraryPath || 'Not resolved from manifest' }}</p>
            <div class="mt-3 flex flex-wrap items-center gap-2">
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="selectedLayer.libraryExists ? 'chip-success' : 'chip-warning'">
                {{ selectedLayer.libraryExists ? 'Found' : 'Missing or Unknown' }}
              </span>
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="signatureChipClass(selectedLayer.signatureStatus)">
                {{ signatureLabel(selectedLayer.signatureStatus) }}
              </span>
              <button
                v-if="selectedLayer.libraryPath"
                class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium"
                type="button"
                @click="openFolder(selectedLayer.libraryPath)"
              >
                Open DLL Folder
              </button>
            </div>
          </div>
        </div>

        <div v-if="selectedLayer.error" class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
          {{ selectedLayer.error }}
        </div>
      </div>
    </div>
  </div>
</template>
