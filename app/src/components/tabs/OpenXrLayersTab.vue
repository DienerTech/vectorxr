<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

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
const loading = ref(false)
const busyKey = ref<string | null>(null)
const status = ref('')

const allLayers = computed(() => snapshot.value?.slices.flatMap((slice) => slice.layers) ?? [])
const totalLayerCount = computed(() => allLayers.value.length)
const uncommonLayerCount = computed(
  () => snapshot.value?.slices.filter((slice) => slice.uncommon).reduce((count, slice) => count + slice.layers.length, 0) ?? 0,
)
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

function actionKey(layer: OpenXrLayerEntry, action: string): string {
  return `${layer.slice}:${layer.manifestPath}:${action}`
}

function isQuadViewsLayer(layer: OpenXrLayerEntry): boolean {
  const value = `${layer.layerName} ${layer.description} ${layer.manifestPath}`.toLowerCase()
  return value.includes('quad-views') || value.includes('quad_views') || value.includes('quadviews')
}

function sliceDescription(slice: OpenXrLayerRegistrySlice): string {
  if (slice.recommended) {
    return 'The common Windows PCVR path. VectorXR recommends keeping normal 64-bit OpenXR layers here.'
  }

  if (slice.id.includes('32')) {
    return 'Usually only relevant for legacy 32-bit OpenXR applications. Its order is separate from the 64-bit lists.'
  }

  return 'A per-user registration bucket. It can be valid, but mixing it with machine-wide layers can be harder to reason about.'
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
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">System OpenXR</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">OpenXR Layer Manager</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Inspect implicit OpenXR API layers, toggle whether they load, and adjust order within each registry slice. Changes apply to future XR app launches.
          </p>
        </div>
        <button
          class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          :disabled="loading"
          @click="refresh"
        >
          {{ loading ? 'Refreshing...' : 'Refresh' }}
        </button>
      </div>

      <div class="mt-5 grid gap-3 md:grid-cols-3">
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Layers Found</p>
          <p class="mt-2 text-2xl font-semibold">{{ totalLayerCount }}</p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Uncommon Slices</p>
          <p class="mt-2 text-2xl font-semibold">{{ uncommonLayerCount }}</p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-soft">
          <p class="eyebrow text-xs uppercase tracking-[0.18em]">Primary Slice</p>
          <p class="mt-2 text-sm font-semibold">Machine-wide 64-bit recommended</p>
        </div>
      </div>

      <div class="mt-4 rounded-[1rem] border px-4 py-4 text-sm leading-6 surface-panel-strong">
        Ordering is meaningful only inside one registry slice. Windows does not merge HKLM, HKCU, 64-bit, and 32-bit layer order into one clean list, so VectorXR keeps them separated on purpose.
      </div>

      <div
        v-if="vectorQuadWarning.length > 0"
        class="mt-4 rounded-[1rem] border px-4 py-4 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        <p class="font-medium">Pivot compatibility note</p>
        <p v-for="warning in vectorQuadWarning" :key="warning" class="mt-1">{{ warning }}</p>
      </div>

      <div v-if="status" class="mt-4 rounded-[1rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
        {{ status }}
      </div>
    </article>

    <section v-if="snapshot" class="space-y-4">
      <article
        v-for="slice in snapshot.slices"
        :key="slice.id"
        class="rounded-[1.25rem] border p-5 shadow-panel surface-panel"
      >
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <div class="flex flex-wrap items-center gap-2">
              <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ slice.registryPath }}</p>
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="slice.recommended ? 'chip-success' : 'chip-warning'">
                {{ slice.recommended ? 'Recommended' : 'Uncommon' }}
              </span>
              <span v-if="slice.requiresElevationForWrites" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-idle">
                Admin to write
              </span>
            </div>
            <h3 class="mt-2 text-xl font-semibold tracking-tight">{{ slice.label }}</h3>
            <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ sliceDescription(slice) }}</p>
          </div>
          <p class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">
            {{ slice.layers.length }} layer{{ slice.layers.length === 1 ? '' : 's' }}
          </p>
        </div>

        <div v-if="slice.layers.length === 0" class="mt-5 rounded-[1rem] border border-dashed px-5 py-6 text-center text-sm surface-panel-soft">
          No implicit OpenXR layers are registered in this slice.
        </div>

        <div v-else class="mt-5 space-y-3">
          <article
            v-for="(layer, index) in slice.layers"
            :key="`${slice.id}-${layer.manifestPath}`"
            class="rounded-[1rem] border p-4 surface-panel-strong"
          >
            <div class="flex flex-wrap items-start justify-between gap-4">
              <div class="min-w-0 flex-1">
                <div class="flex flex-wrap items-center gap-2">
                  <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">#{{ layer.order }}</span>
                  <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="layer.enabled ? 'chip-success' : 'chip-idle'">
                    {{ layer.enabled ? 'Enabled' : 'Disabled' }}
                  </span>
                  <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="signatureChipClass(layer.signatureStatus)">
                    {{ signatureLabel(layer.signatureStatus) }}
                  </span>
                  <span v-if="layer.isVectorXr" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-success">VectorXR</span>
                  <span v-if="isQuadViewsLayer(layer)" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-warning">Quadviews</span>
                </div>
                <h4 class="mt-3 text-lg font-semibold tracking-tight">{{ layer.layerName }}</h4>
                <p class="mt-1 text-sm leading-6 text-muted">{{ layer.description }}</p>
              </div>

              <div class="flex flex-wrap gap-2">
                <button
                  class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium"
                  type="button"
                  :disabled="busyKey !== null || index === 0"
                  @click="moveLayer(slice.id, layer, 'up')"
                >
                  Move Up
                </button>
                <button
                  class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium"
                  type="button"
                  :disabled="busyKey !== null || index === slice.layers.length - 1"
                  @click="moveLayer(slice.id, layer, 'down')"
                >
                  Move Down
                </button>
                <button
                  class="button-secondary rounded-[0.65rem] px-3 py-2 text-xs font-medium"
                  type="button"
                  :disabled="busyKey !== null"
                  @click="toggleLayer(slice.id, layer)"
                >
                  {{ layer.enabled ? 'Disable' : 'Enable' }}
                </button>
              </div>
            </div>

            <div class="mt-4 grid gap-3 lg:grid-cols-2">
              <div class="rounded-[0.85rem] border p-3 surface-panel-soft">
                <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">Manifest</p>
                <p class="mt-2 break-all font-mono text-xs">{{ layer.manifestPath }}</p>
                <div class="mt-3 flex flex-wrap items-center gap-2">
                  <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="layer.manifestExists ? 'chip-success' : 'chip-warning'">
                    {{ layer.manifestExists ? 'Found' : 'Missing' }}
                  </span>
                  <button class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium" type="button" @click="openFolder(layer.manifestPath)">
                    Open Manifest Folder
                  </button>
                </div>
              </div>

              <div class="rounded-[0.85rem] border p-3 surface-panel-soft">
                <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">DLL</p>
                <p class="mt-2 break-all font-mono text-xs">{{ layer.libraryPath || 'Not resolved from manifest' }}</p>
                <div class="mt-3 flex flex-wrap items-center gap-2">
                  <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="layer.libraryExists ? 'chip-success' : 'chip-warning'">
                    {{ layer.libraryExists ? 'Found' : 'Missing or Unknown' }}
                  </span>
                  <button
                    v-if="layer.libraryPath"
                    class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium"
                    type="button"
                    @click="openFolder(layer.libraryPath)"
                  >
                    Open DLL Folder
                  </button>
                </div>
              </div>
            </div>

            <div v-if="layer.error" class="mt-3 rounded-[0.85rem] border px-3 py-2 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
              {{ layer.error }}
            </div>
          </article>
        </div>
      </article>
    </section>

    <div v-else class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ loading ? 'Reading OpenXR layer registrations...' : 'OpenXR layer state has not loaded yet.' }}
    </div>
  </div>
</template>
