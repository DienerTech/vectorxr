<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import HealthLoading from '../HealthLoading.vue'

import { formatList, hasMessage, t } from '../../i18n/index.ts'
import {
  deleteOpenXrLayer,
  ensureOpenXrLayerElevation,
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
import { errorText } from '../../lib/errors'

const props = defineProps<{
  snapshot: OpenXrLayerSnapshot | null
  loading: boolean
  machineWritesUnlocked: boolean
  turboInUse: boolean
  quadviewsInUse: boolean
}>()

const emit = defineEmits<{
  refresh: []
  machineWritesUnlocked: [unlocked: boolean]
  snapshotUpdated: [snapshot: OpenXrLayerSnapshot]
  status: [message: string]
}>()

const activeSliceId = ref<OpenXrLayerRegistrySliceId>('hklm64')
const selectedLayer = ref<OpenXrLayerEntry | null>(null)
const pendingDeletion = ref<{ slice: OpenXrLayerRegistrySliceId; layer: OpenXrLayerEntry } | null>(null)
const busyKey = ref<string | null>(null)
const unlockingMachineWrites = ref(false)

const activeSlice = computed(() => props.snapshot?.slices.find((slice) => slice.id === activeSliceId.value) ?? null)
const adminWritesAvailable = computed(() => props.snapshot?.processElevated === true || props.machineWritesUnlocked)
const activeSliceReadOnly = computed(() => activeSlice.value?.requiresElevationForWrites === true && !adminWritesAvailable.value)
const uncommonLayerCount = computed(
  () => props.snapshot?.slices.filter((slice) => slice.uncommon).reduce((count, slice) => count + slice.layers.length, 0) ?? 0,
)
const enabledLayerCount = computed(() => activeSlice.value?.layers.filter((layer) => layer.enabled).length ?? 0)
const vectorQuadWarning = computed(() => {
  const warnings: string[] = []

  for (const slice of props.snapshot?.slices ?? []) {
    const vector = slice.layers.find((layer) => layer.isVectorXr && layer.enabled)
      ?? slice.layers.find((layer) => layer.isVectorXr)
    const quad = slice.layers.find(isQuadViewsLayer)

    if (vector && quad && vector.order < quad.order) {
      warnings.push(t('layers.warnings.quadOrder', { slice: sliceLabel(slice) }))
    }
  }

  return warnings
})

// Two frame-pacing layers fight each other: advise when OpenXR Toolkit is
// enabled while VectorXR Turbo applies anywhere.
const toolkitTurboWarning = computed(() => {
  if (!props.turboInUse) {
    return false
  }
  return (props.snapshot?.slices ?? []).some((slice) =>
    slice.layers.some((layer) => layer.enabled && layer.layerName.toLowerCase().includes('mbucchia_toolkit')),
  )
})

// OpenXR Toolkit supports two views only: above VectorXR it would get the
// game's quad views, so the layer keeps Quadviews off in that order.
const toolkitQuadSlices = computed(() => {
  if (!props.quadviewsInUse) {
    return []
  }
  return (props.snapshot?.slices ?? []).filter((slice) => {
    const vector = slice.layers.find((layer) => layer.isVectorXr && layer.enabled)
    const toolkit = slice.layers.find((layer) => layer.enabled && layer.layerName.toLowerCase().includes('mbucchia_toolkit'))
    return vector && toolkit && toolkit.order < vector.order
  }).map((slice) => sliceLabel(slice))
})

// The backend names each slice in English; show the name for its id when the
// catalog has one.
function sliceLabel(slice: OpenXrLayerRegistrySlice): string {
  const key = `layers.slices.${slice.id}`
  return hasMessage(key) ? t(key) : slice.label
}

watch(() => props.snapshot, (value) => {
  if (selectedLayer.value && value) {
    const selected = selectedLayer.value
    selectedLayer.value = value.slices.flatMap(slice => slice.layers).find(layer =>
      layer.slice === selected.slice && layer.manifestPath === selected.manifestPath) ?? null
  }
  if (value && !value.slices.some((slice) => slice.id === activeSliceId.value)) {
    activeSliceId.value = 'hklm64'
  }
})

async function unlockMachineWrites() {
  unlockingMachineWrites.value = true

  try {
    await ensureOpenXrLayerElevation()
    emit('machineWritesUnlocked', true)
    emit('status', t('layers.status.unlocked'))
    await nextTick()
  } catch (error) {
    emit('status', errorText(error, t('layers.status.unlockDenied')))
  } finally {
    unlockingMachineWrites.value = false
  }
}

async function toggleLayer(slice: OpenXrLayerRegistrySliceId, layer: OpenXrLayerEntry) {
  if (activeSliceReadOnly.value) {
    emit('status', t('layers.status.unlockBeforeToggle'))
    return
  }

  const key = actionKey(layer, 'toggle')
  busyKey.value = key
  const optimisticSnapshot = props.snapshot ? applyLayerEnabled(props.snapshot, slice, layer.manifestPath, !layer.enabled) : null
  if (optimisticSnapshot) {
    emit('snapshotUpdated', optimisticSnapshot)
    updateSelectedLayer(optimisticSnapshot, slice, layer.manifestPath)
  }

  try {
    // The backend reads the registry after the change is written, so show
    // what it found: a write that did not take shows as such.
    const nextSnapshot = await setOpenXrLayerEnabled(slice, layer.manifestPath, !layer.enabled)
    emit('snapshotUpdated', nextSnapshot)
    updateSelectedLayer(nextSnapshot, slice, layer.manifestPath)
  } catch (error) {
    emit('status', errorText(error, t('layers.status.toggleFailed')))
    queueRefresh()
  } finally {
    busyKey.value = null
  }
}

async function moveLayer(slice: OpenXrLayerRegistrySliceId, layer: OpenXrLayerEntry, direction: OpenXrLayerMoveDirection) {
  if (activeSliceReadOnly.value) {
    emit('status', t('layers.status.unlockBeforeReorder'))
    return
  }

  const key = actionKey(layer, direction)
  busyKey.value = key
  const optimisticSnapshot = props.snapshot ? applyLayerMove(props.snapshot, slice, layer.manifestPath, direction) : null
  if (optimisticSnapshot) {
    emit('snapshotUpdated', optimisticSnapshot)
    updateSelectedLayer(optimisticSnapshot, slice, layer.manifestPath)
  }

  try {
    const nextSnapshot = await moveOpenXrLayer(slice, layer.manifestPath, direction)
    emit('snapshotUpdated', nextSnapshot)
    updateSelectedLayer(nextSnapshot, slice, layer.manifestPath)
  } catch (error) {
    emit('status', errorText(error, t('layers.status.reorderFailed')))
    queueRefresh()
  } finally {
    busyKey.value = null
  }
}

function requestLayerDeletion(slice: OpenXrLayerRegistrySliceId, layer: OpenXrLayerEntry) {
  if (activeSliceReadOnly.value) {
    emit('status', t('layers.status.unlockBeforeRemove'))
    return
  }

  pendingDeletion.value = { slice, layer }
}

function cancelLayerDeletion() {
  if (busyKey.value === null) {
    pendingDeletion.value = null
  }
}

async function confirmLayerDeletion() {
  const pending = pendingDeletion.value
  if (!pending) {
    return
  }

  const { slice, layer } = pending
  const key = actionKey(layer, 'delete')
  busyKey.value = key
  const optimisticSnapshot = props.snapshot ? applyLayerDelete(props.snapshot, slice, layer.manifestPath) : null
  if (optimisticSnapshot) {
    emit('snapshotUpdated', optimisticSnapshot)
    selectedLayer.value = null
  }

  try {
    const nextSnapshot = await deleteOpenXrLayer(slice, layer.manifestPath)
    emit('snapshotUpdated', nextSnapshot)
    selectedLayer.value = null
    pendingDeletion.value = null
    emit('status', activeSlice.value
      ? t('layers.status.removed', { name: layer.layerName, slice: sliceLabel(activeSlice.value) })
      : t('layers.status.removedFromRegistry', { name: layer.layerName }))
    emit('refresh')
  } catch (error) {
    emit('status', errorText(error, t('layers.status.removeFailed')))
    queueRefresh()
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
    emit('status', errorText(error, t('layers.status.openFolderFailed')))
  }
}

function findLayer(snapshot: OpenXrLayerSnapshot, slice: OpenXrLayerRegistrySliceId, manifestPath: string): OpenXrLayerEntry | null {
  return snapshot.slices.find((item) => item.id === slice)?.layers.find((layer) => layer.manifestPath === manifestPath) ?? null
}

function updateSelectedLayer(snapshot: OpenXrLayerSnapshot, slice: OpenXrLayerRegistrySliceId, manifestPath: string) {
  if (selectedLayer.value) {
    selectedLayer.value = findLayer(snapshot, slice, manifestPath)
  }
}

function applyLayerEnabled(
  snapshot: OpenXrLayerSnapshot,
  sliceId: OpenXrLayerRegistrySliceId,
  manifestPath: string,
  enabled: boolean,
): OpenXrLayerSnapshot {
  return {
    ...snapshot,
    slices: snapshot.slices.map((slice) => ({
      ...slice,
      layers: slice.id === sliceId
        ? slice.layers.map((layer) => layer.manifestPath === manifestPath ? { ...layer, enabled } : layer)
        : slice.layers,
    })),
  }
}

function applyLayerMove(
  snapshot: OpenXrLayerSnapshot,
  sliceId: OpenXrLayerRegistrySliceId,
  manifestPath: string,
  direction: OpenXrLayerMoveDirection,
): OpenXrLayerSnapshot {
  return {
    ...snapshot,
    slices: snapshot.slices.map((slice) => {
      if (slice.id !== sliceId) {
        return slice
      }

      const layers = [...slice.layers]
      const index = layers.findIndex((layer) => layer.manifestPath === manifestPath)
      const targetIndex = direction === 'up' ? index - 1 : index + 1

      if (index < 0 || targetIndex < 0 || targetIndex >= layers.length) {
        return slice
      }

      const [layer] = layers.splice(index, 1)
      layers.splice(targetIndex, 0, layer)

      return {
        ...slice,
        layers: layers.map((layer, layerIndex) => ({ ...layer, order: layerIndex + 1 })),
      }
    }),
  }
}

function applyLayerDelete(
  snapshot: OpenXrLayerSnapshot,
  sliceId: OpenXrLayerRegistrySliceId,
  manifestPath: string,
): OpenXrLayerSnapshot {
  return {
    ...snapshot,
    slices: snapshot.slices.map((slice) => ({
      ...slice,
      layers: slice.id === sliceId
        ? slice.layers
            .filter((layer) => layer.manifestPath !== manifestPath)
            .map((layer, layerIndex) => ({ ...layer, order: layerIndex + 1 }))
        : slice.layers,
    })),
  }
}

function queueRefresh() {
  window.setTimeout(() => {
    emit('refresh')
  }, 250)
}

function actionKey(layer: OpenXrLayerEntry, action: string): string {
  return `${layer.slice}:${layer.manifestPath}:${action}`
}

function isActionBusy(layer: OpenXrLayerEntry, action: string): boolean {
  return busyKey.value === actionKey(layer, action)
}

function isQuadViewsLayer(layer: OpenXrLayerEntry): boolean {
  const value = `${layer.layerName} ${layer.description} ${layer.manifestPath}`.toLowerCase()
  return value.includes('quad-views') || value.includes('quad_views') || value.includes('quadviews')
}

function sliceDescription(slice: OpenXrLayerRegistrySlice): string {
  if (slice.recommended) {
    return t('layers.slice.descriptionRecommended')
  }

  if (slice.id.includes('32')) {
    return t('layers.slice.description32')
  }

  return t('layers.slice.descriptionPerUser')
}

function signatureLabel(status: OpenXrLayerSignatureStatus): string {
  switch (status) {
    case 'signed':
      return t('layers.signature.signed')
    case 'unsigned':
      return t('layers.signature.unsigned')
    case 'invalid':
      return t('layers.signature.invalid')
    default:
      return t('common.unknown')
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

function signatureTooltip(layer: OpenXrLayerEntry): string {
  switch (layer.signatureStatus) {
    case 'signed':
      return t('layers.signature.signedTooltip')
    case 'unsigned':
      return t('layers.signature.unsignedTooltip')
    case 'invalid':
      return layer.signatureSignerNotAfter
        ? t('layers.signature.invalidTooltipWithDate', { date: layer.signatureSignerNotAfter })
        : t('layers.signature.invalidTooltip')
    default:
      return t('layers.signature.unknownTooltip')
  }
}

function vectorXrTooltip(): string {
  return t('layers.layer.vectorXrTooltip')
}

function quadViewsTooltip(): string {
  return t('layers.layer.quadViewsTooltip')
}

// `action` is a whole sentence naming the action; the message adds why it
// does or does not need administrator approval.
function elevatedWriteTooltip(action: string, slice: OpenXrLayerRegistrySlice): string {
  if (!slice.requiresElevationForWrites) {
    return t('layers.layer.perUserWrite', { action })
  }

  if (!adminWritesAvailable.value) {
    return t('layers.layer.lockedWrite', { action })
  }

  return t('layers.layer.unlockedWrite', { action })
}

function attentionTooltip(layer: OpenXrLayerEntry): string {
  if (layer.error) {
    return layer.error
  }

  if (!layer.manifestExists) {
    return t('layers.layer.manifestMissing')
  }

  if (!layer.libraryExists) {
    return t('layers.layer.libraryMissing')
  }

  return t('layers.layer.needsReview')
}

function signatureGuidance(layer: OpenXrLayerEntry): string {
  switch (layer.signatureStatus) {
    case 'signed':
      return t('layers.signature.signedGuidance')
    case 'unsigned':
      return t('layers.signature.unsignedGuidance')
    case 'invalid':
      return layer.signatureSignerNotAfter
        ? t('layers.signature.invalidGuidanceWithDate', { date: layer.signatureSignerNotAfter })
        : t('layers.signature.invalidGuidance')
    default:
      return t('layers.signature.unknownGuidance')
  }
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-4 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('layers.manager.eyebrow') }}</p>
          <h2 class="mt-1 text-2xl font-semibold tracking-tight">{{ t('layers.manager.title') }}</h2>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <span v-if="activeSlice" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">
            {{ t('layers.manager.enabledCount', { enabled: enabledLayerCount, total: activeSlice.layers.length }) }}
          </span>
          <span v-if="uncommonLayerCount > 0" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-warning">
            {{ t('layers.manager.uncommonCount', { count: uncommonLayerCount }) }}
          </span>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" :disabled="loading" @click="$emit('refresh')">
            {{ loading ? t('layers.manager.refreshing') : t('common.refresh') }}
          </button>
        </div>
      </div>

      <p class="mt-2 max-w-4xl text-sm leading-6 text-muted">
        {{ t('layers.manager.intro') }}
      </p>

      <div
        v-if="vectorQuadWarning.length > 0"
        class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        <span class="font-medium">{{ t('layers.warnings.pivotLabel') }}</span>
        {{ vectorQuadWarning.join(' ') }}
      </div>

      <div
        v-if="toolkitTurboWarning"
        class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        <span class="font-medium">{{ t('layers.warnings.turboLabel') }}</span>
        {{ t('layers.warnings.toolkitTurbo') }}
      </div>

      <div
        v-if="toolkitQuadSlices.length > 0"
        class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        <span class="font-medium">{{ t('layers.warnings.quadviewsOffLabel') }}</span>
        {{ t('layers.warnings.toolkitQuad', { slices: formatList(toolkitQuadSlices) }) }}
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
              <p class="text-sm font-semibold tracking-tight">{{ sliceLabel(slice) }}</p>
              <p class="mt-0.5 text-xs" :class="activeSliceId === slice.id ? 'text-inverse-muted' : 'text-muted'">
                {{ slice.recommended ? t('layers.slice.recommended') : t('layers.slice.uncommon') }}
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
              <h3 class="text-lg font-semibold tracking-tight">{{ sliceLabel(activeSlice) }}</h3>
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="activeSlice.recommended ? 'chip-success' : 'chip-warning'">
                {{ activeSlice.recommended ? t('layers.slice.recommended') : t('layers.slice.uncommon') }}
              </span>
              <span v-if="activeSlice.requiresElevationForWrites" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-idle">
                {{ t('layers.slice.adminWrites') }}
              </span>
              <span
                v-if="activeSlice.requiresElevationForWrites"
                class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
                :class="adminWritesAvailable ? 'chip-success' : 'chip-warning'"
              >
                {{ adminWritesAvailable ? t('layers.slice.unlocked') : t('layers.slice.readOnly') }}
              </span>
            </div>
            <p class="mt-1 text-sm leading-6 text-muted">{{ sliceDescription(activeSlice) }}</p>
          </div>
          <div class="flex max-w-full flex-wrap items-center justify-end gap-2">
            <p class="max-w-full truncate font-mono text-xs text-soft">{{ activeSlice.registryPath }}</p>
          </div>
        </div>

        <div
          v-if="activeSliceReadOnly"
          class="mt-3 flex flex-wrap items-center justify-between gap-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning"
          style="border-color: var(--app-border)"
        >
          <span>{{ t('layers.slice.readOnlyNotice') }}</span>
          <button
            class="button-accent rounded-[0.7rem] px-3 py-1.5 text-sm font-medium"
            type="button"
            :disabled="unlockingMachineWrites || busyKey !== null"
            :title="t('layers.slice.unlockHelp')"
            @click.stop="unlockMachineWrites"
          >
            {{ unlockingMachineWrites ? t('layers.slice.requesting') : t('layers.slice.unlockButton') }}
          </button>
        </div>

        <div v-if="activeSlice.layers.length === 0" class="mt-3 rounded-[0.9rem] border border-dashed px-5 py-5 text-center text-sm surface-panel-soft">
          {{ t('layers.slice.empty') }}
        </div>

        <TransitionGroup v-else name="openxr-layer-list" tag="div" class="mt-3 space-y-2">
          <article
            v-for="(layer, index) in activeSlice.layers"
            :key="`${activeSlice.id}-${layer.manifestPath}`"
            class="rounded-[0.9rem] border px-3 py-2.5 surface-panel"
            style="border-color: var(--app-border)"
          >
            <div class="flex items-center gap-3">
              <div class="flex shrink-0 items-center gap-2 rounded-[0.75rem] border px-2 py-1.5 surface-panel-soft" style="border-color: var(--app-border)">
                <span class="min-w-[3.25rem] rounded-full px-2.5 py-1 text-center text-xs font-semibold uppercase tracking-[0.14em] chip-accent">#{{ layer.order }}</span>
                <div class="flex flex-col gap-1">
                <button
                  class="button-secondary inline-flex h-7 w-8 items-center justify-center rounded-[0.5rem] text-sm font-semibold disabled:cursor-not-allowed disabled:opacity-60"
                  type="button"
                  :aria-label="t('layers.layer.moveUp')"
                  :title="elevatedWriteTooltip(t('layers.layer.moveUpAction'), activeSlice)"
                  :disabled="busyKey !== null || activeSliceReadOnly || index === 0"
                  @click.stop="moveLayer(activeSlice.id, layer, 'up')"
                >
                  &uarr;
                </button>
                <button
                  class="button-secondary inline-flex h-7 w-8 items-center justify-center rounded-[0.5rem] text-sm font-semibold disabled:cursor-not-allowed disabled:opacity-60"
                  type="button"
                  :aria-label="t('layers.layer.moveDown')"
                  :title="elevatedWriteTooltip(t('layers.layer.moveDownAction'), activeSlice)"
                  :disabled="busyKey !== null || activeSliceReadOnly || index === activeSlice.layers.length - 1"
                  @click.stop="moveLayer(activeSlice.id, layer, 'down')"
                >
                  &darr;
                </button>
                </div>
              </div>

              <button
                class="inline-flex w-[6.25rem] shrink-0 items-center justify-center gap-2 rounded-[0.7rem] border px-2.5 py-2 text-center transition disabled:cursor-not-allowed disabled:opacity-60"
                :class="layer.enabled ? 'layer-state-on' : 'layer-state-off'"
                type="button"
                :title="elevatedWriteTooltip(layer.enabled ? t('layers.layer.disableAction') : t('layers.layer.enableAction'), activeSlice)"
                :disabled="busyKey !== null || activeSliceReadOnly"
                @click.stop="toggleLayer(activeSlice.id, layer)"
              >
                <span v-if="isActionBusy(layer, 'toggle')" class="vectorxr-spinner" aria-hidden="true">
                  <span></span>
                  <span></span>
                </span>
                <span class="block text-[0.68rem] font-semibold uppercase tracking-[0.16em]">{{ layer.enabled ? t('common.enabled') : t('common.disabled') }}</span>
              </button>

              <div class="min-w-0 flex-1">
                <div class="flex min-w-0 items-start justify-between gap-3">
                  <div class="min-w-0">
                    <div class="flex min-w-0 flex-wrap items-center gap-2">
                      <h4 class="truncate text-base font-semibold leading-5 tracking-tight">{{ layer.layerName }}</h4>
                      <span
                        class="mr-2 cursor-default rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
                        :class="signatureChipClass(layer.signatureStatus)"
                        :title="signatureTooltip(layer)"
                      >
                        <HealthLoading v-if="loading" />
                        <template v-else>{{ signatureLabel(layer.signatureStatus) }}</template>
                      </span>
                      <span
                        v-if="layer.isVectorXr"
                        class="mr-2 cursor-default rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-success"
                        :title="vectorXrTooltip()"
                      >
                        VectorXR
                      </span>
                      <span
                        v-if="layer.isVectorXr && !layer.enabled"
                        class="mr-2 cursor-default rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning"
                        :title="t('layers.layer.vectorXrDisabledTooltip')"
                      >
                        {{ t('layers.layer.enhancementsInactive') }}
                      </span>
                      <span
                        v-if="isQuadViewsLayer(layer)"
                        class="mr-2 cursor-default rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning"
                        :title="quadViewsTooltip()"
                      >
                        Quadviews
                      </span>
                      <span
                        v-if="!layer.manifestExists || !layer.libraryExists || layer.error"
                        class="mr-2 cursor-default rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning"
                        :title="attentionTooltip(layer)"
                      >
                        {{ t('layers.layer.needsAttention') }}
                      </span>
                    </div>
                    <p class="mt-0.5 truncate text-sm leading-5 text-muted">{{ layer.description }}</p>
                  </div>

                  <div class="flex shrink-0 flex-wrap justify-end gap-2">
                    <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click.stop="selectedLayer = layer">
                      {{ t('layers.layer.details') }}
                    </button>
                    <button
                      class="button-danger rounded-[0.65rem] px-3 py-1.5 text-xs font-medium disabled:cursor-not-allowed disabled:opacity-50"
                      type="button"
                      :disabled="busyKey !== null || activeSliceReadOnly"
                      :title="elevatedWriteTooltip(t('layers.layer.removeAction'), activeSlice)"
                      @click.stop="requestLayerDeletion(activeSlice.id, layer)"
                    >
                      {{ t('common.remove') }}
                    </button>
                  </div>
                </div>

              </div>
            </div>
          </article>
        </TransitionGroup>
      </article>
    </section>

    <div v-else class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ loading ? t('layers.manager.loadingLayers') : t('layers.manager.notLoaded') }}
    </div>

    <div v-if="selectedLayer" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm" role="dialog" aria-modal="true" @click.self="selectedLayer = null">
      <div class="max-h-[calc(100vh-3rem)] w-full max-w-[820px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="sticky top-0 z-10 -mx-5 -mt-5 flex flex-wrap items-start justify-between gap-4 border-b px-5 py-4 surface-panel-strong" style="border-color: var(--app-border)">
          <div class="min-w-0 flex-1">
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('layers.details.eyebrow') }}</p>
            <h2 class="mt-2 truncate text-xl font-semibold tracking-tight">{{ selectedLayer.layerName }}</h2>
            <p class="mt-1 text-sm leading-6 text-muted">{{ selectedLayer.description }}</p>
          </div>
          <button class="button-secondary shrink-0 rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click.stop="selectedLayer = null">
            {{ t('common.close') }}
          </button>
        </div>

        <div class="mt-5 grid gap-3 md:grid-cols-2">
          <div class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">{{ t('layers.details.manifest') }}</p>
            <p class="mt-2 break-all font-mono text-xs">{{ selectedLayer.manifestPath }}</p>
            <div class="mt-3 flex flex-wrap items-center gap-2">
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="selectedLayer.manifestExists ? 'chip-success' : 'chip-warning'">
                {{ selectedLayer.manifestExists ? t('layers.details.found') : t('layers.details.missing') }}
              </span>
              <button class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium" type="button" @click.stop="openFolder(selectedLayer.manifestPath)">
                {{ t('layers.details.openManifestFolder') }}
              </button>
            </div>
          </div>

          <div class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">{{ t('layers.details.dll') }}</p>
            <p class="mt-2 break-all font-mono text-xs">{{ selectedLayer.libraryPath || t('layers.details.libraryNotResolved') }}</p>
            <div class="mt-3 flex flex-wrap items-center gap-2">
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="selectedLayer.libraryExists ? 'chip-success' : 'chip-warning'">
                {{ selectedLayer.libraryExists ? t('layers.details.found') : t('layers.details.libraryMissingOrUnknown') }}
              </span>
              <span
                class="cursor-default rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
                :class="signatureChipClass(selectedLayer.signatureStatus)"
                :title="signatureTooltip(selectedLayer)"
              >
                <HealthLoading v-if="loading" />
                <template v-else>{{ signatureLabel(selectedLayer.signatureStatus) }}</template>
              </span>
              <button
                v-if="selectedLayer.libraryPath"
                class="button-secondary rounded-[0.6rem] px-3 py-1.5 text-xs font-medium"
                type="button"
                @click.stop="openFolder(selectedLayer.libraryPath)"
              >
                {{ t('layers.details.openDllFolder') }}
              </button>
            </div>
          </div>
        </div>

        <div class="mt-3 rounded-[0.9rem] border p-3 surface-panel-soft">
          <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">{{ t('layers.details.binarySignature') }}</p>
          <div class="mt-2 flex flex-wrap items-center gap-2">
            <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="signatureChipClass(selectedLayer.signatureStatus)">
              <HealthLoading v-if="loading" />
              <template v-else>{{ signatureLabel(selectedLayer.signatureStatus) }}</template>
            </span>
            <span v-if="!loading" class="text-sm leading-6 text-muted">{{ signatureGuidance(selectedLayer) }}</span>
          </div>
          <p v-if="!loading" class="mt-2 text-xs leading-5 text-muted">{{ t('layers.details.windowsStatus', { status: selectedLayer.signatureStatusDescription }) }}</p>
          <dl class="mt-3 grid gap-2 text-xs md:grid-cols-2">
            <div v-if="selectedLayer.signatureSignerNotBefore || selectedLayer.signatureSignerNotAfter" class="min-w-0 md:col-span-2">
              <dt class="font-semibold uppercase tracking-[0.14em] text-soft">{{ t('layers.details.certificateValidity') }}</dt>
              <dd class="mt-1 font-mono">
                {{ t('layers.details.validityRange', {
                  from: selectedLayer.signatureSignerNotBefore || t('layers.details.unknownStart'),
                  to: selectedLayer.signatureSignerNotAfter || t('layers.details.unknownEnd'),
                }) }}
              </dd>
            </div>
            <div v-if="selectedLayer.signatureSignerSubject" class="min-w-0">
              <dt class="font-semibold uppercase tracking-[0.14em] text-soft">{{ t('layers.details.signer') }}</dt>
              <dd class="mt-1 break-all font-mono">{{ selectedLayer.signatureSignerSubject }}</dd>
            </div>
            <div v-if="selectedLayer.signatureSignerIssuer" class="min-w-0">
              <dt class="font-semibold uppercase tracking-[0.14em] text-soft">{{ t('layers.details.issuer') }}</dt>
              <dd class="mt-1 break-all font-mono">{{ selectedLayer.signatureSignerIssuer }}</dd>
            </div>
            <div v-if="selectedLayer.signatureSignerThumbprint" class="min-w-0 md:col-span-2">
              <dt class="font-semibold uppercase tracking-[0.14em] text-soft">{{ t('layers.details.thumbprint') }}</dt>
              <dd class="mt-1 break-all font-mono">{{ selectedLayer.signatureSignerThumbprint }}</dd>
            </div>
          </dl>
        </div>

        <div v-if="selectedLayer.error" class="mt-3 rounded-[0.9rem] border px-3 py-2 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
          {{ selectedLayer.error }}
        </div>
      </div>
    </div>

    <div
      v-if="pendingDeletion"
      class="fixed inset-0 z-[60] flex items-center justify-center bg-black/55 px-4 py-6 backdrop-blur-sm"
      role="alertdialog"
      aria-modal="true"
      aria-labelledby="remove-layer-title"
      aria-describedby="remove-layer-description"
      @click.self="cancelLayerDeletion"
    >
      <div class="w-full max-w-[520px] rounded-[1.15rem] border p-5 shadow-panel surface-panel-strong">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('layers.remove.eyebrow') }}</p>
        <h2 id="remove-layer-title" class="mt-2 text-xl font-semibold tracking-tight">
          {{ t('layers.remove.title', { name: pendingDeletion.layer.layerName }) }}
        </h2>
        <p id="remove-layer-description" class="mt-3 text-sm leading-6 text-muted">
          {{ activeSlice
            ? t('layers.remove.description', { slice: sliceLabel(activeSlice) })
            : t('layers.remove.descriptionSelectedHive') }}
        </p>
        <p class="mt-3 break-all rounded-[0.75rem] border px-3 py-2 font-mono text-xs text-soft surface-panel-soft" style="border-color: var(--app-border)">
          {{ pendingDeletion.layer.manifestPath }}
        </p>
        <div class="mt-5 flex flex-wrap justify-end gap-2">
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            :disabled="busyKey !== null"
            @click="cancelLayerDeletion"
          >
            {{ t('common.cancel') }}
          </button>
          <button
            class="button-danger rounded-[0.75rem] px-4 py-2 text-sm font-medium disabled:cursor-not-allowed disabled:opacity-50"
            type="button"
            :disabled="busyKey !== null"
            @click="confirmLayerDeletion"
          >
            {{ isActionBusy(pendingDeletion.layer, 'delete') ? t('layers.remove.removing') : t('layers.remove.confirm') }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
