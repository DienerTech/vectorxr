<script setup lang="ts">
import { computed } from 'vue'
import { t } from '../i18n/index.ts'
import type { PivotPositionAssist, PivotPositionAssistPreset, PivotPositionDirection } from '../lib/model'
import { applyPivotPositionAssistPreset, matchingPivotPositionAssistPreset } from '../lib/model'

const props = defineProps<{ settings: PivotPositionAssist }>()

const presets = computed<Array<{ id: PivotPositionAssistPreset; label: string; title: string }>>(() => [
  { id: 'none', label: t('pivot.positionAssist.presetNone'), title: t('pivot.positionAssist.presetNoneTitle') },
  { id: 'gentle', label: t('pivot.positionAssist.presetGentle'), title: t('pivot.positionAssist.presetGentleTitle') },
  { id: 'strong', label: t('pivot.positionAssist.presetStrong'), title: t('pivot.positionAssist.presetStrongTitle') },
])
const activePreset = computed(() => matchingPivotPositionAssistPreset(props.settings))

const directions = computed(() => props.settings.advancedDirections
  ? [
      { label: t('pivot.positionAssist.turningLeft'), value: props.settings.yawLeft },
      { label: t('pivot.positionAssist.turningRight'), value: props.settings.yawRight },
      { label: t('pivot.positionAssist.lookingUp'), value: props.settings.pitchUp },
      { label: t('pivot.positionAssist.lookingDown'), value: props.settings.pitchDown },
    ]
  : [{ label: t('pivot.positionAssist.turningLeftRight'), value: props.settings.yaw }])

// Switching to advanced directions seeds left and right from the basic lean so
// tuning starts from what already feels right. Existing directional values win.
function seedDirections() {
  if (!props.settings.advancedDirections) return
  const directional = [props.settings.yawLeft, props.settings.yawRight, props.settings.pitchUp, props.settings.pitchDown]
  if (directional.some((direction) => direction.rightCm || direction.upCm || direction.forwardCm)) return
  props.settings.yawRight = { ...props.settings.yaw }
  props.settings.yawLeft = { ...props.settings.yaw, rightCm: -props.settings.yaw.rightCm }
}

function anglesOutOfOrder(direction: PivotPositionDirection) {
  return direction.fullAngleDegrees <= direction.startAngleDegrees
}
</script>

<template>
  <section class="rounded-[1rem] border p-4 surface-panel-soft">
    <label class="flex items-start gap-3">
      <input v-model="settings.enabled" type="checkbox" class="mt-1 h-4 w-4 accent-depthxr-copper" />
      <span>
        <span class="block font-semibold">{{ t('pivot.positionAssist.title') }}</span>
        <span class="mt-1 block text-sm text-muted">
          {{ t('pivot.positionAssist.description') }}
        </span>
      </span>
    </label>
    <template v-if="settings.enabled">
      <div class="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-[0.75rem] border px-3 py-2.5 surface-panel-strong">
        <div class="min-w-0 flex-1 basis-72">
          <p class="text-sm font-semibold">{{ t('pivot.positionAssist.startingPoints') }}</p>
          <p class="mt-1 text-xs text-muted">
            {{ t('pivot.positionAssist.startingPointsDetail') }}
          </p>
        </div>
        <div class="flex flex-wrap gap-2">
          <button
            v-for="preset in presets"
            :key="preset.id"
            class="rounded-[0.65rem] border px-3 py-1.5 text-xs font-semibold"
            :class="activePreset === preset.id ? 'button-accent' : 'button-secondary'"
            type="button"
            :title="preset.title"
            @click="applyPivotPositionAssistPreset(settings, preset.id)"
          >
            {{ preset.label }}
          </button>
        </div>
      </div>
      <label class="mt-4 flex gap-3 text-sm">
        <input v-model="settings.advancedDirections" type="checkbox" @change="seedDirections" />
        {{ t('pivot.positionAssist.advancedDirections') }}
      </label>
      <div v-for="direction in directions" :key="direction.label" class="mt-4 rounded-[0.75rem] border p-3 surface-panel-strong">
        <p class="font-semibold">{{ direction.label }}</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-3">
          <label class="text-sm">
            {{ settings.advancedDirections ? t('pivot.positionAssist.rightLeft') : t('pivot.positionAssist.sidewaysLean') }}
            <input v-model.number="direction.value.rightCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" />
          </label>
          <label class="text-sm">
            {{ t('pivot.positionAssist.upDown') }}
            <input v-model.number="direction.value.upCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" />
          </label>
          <label class="text-sm">
            {{ t('pivot.positionAssist.forwardBack') }}
            <input v-model.number="direction.value.forwardCm" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" type="number" min="-100" max="100" step="1" />
          </label>
          <!-- Fixed bounds keep step="1" counting from whole degrees; a min/max
               tied to the other angle would shift the step base (15.1, 16.1, ...)
               and flag whole-degree values. Ordering is flagged separately. -->
          <label class="text-sm">
            {{ t('pivot.positionAssist.startAngle') }}
            <input v-model.number="direction.value.startAngleDegrees" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" :class="{ 'app-input-error': anglesOutOfOrder(direction.value) }" type="number" min="0" max="179" step="1" />
          </label>
          <label class="text-sm">
            {{ t('pivot.positionAssist.fullOffsetAngle') }}
            <input v-model.number="direction.value.fullAngleDegrees" class="app-input mt-1 w-full rounded-[0.75rem] px-3 py-2" :class="{ 'app-input-error': anglesOutOfOrder(direction.value) }" type="number" min="0" max="180" step="1" />
          </label>
        </div>
        <p v-if="anglesOutOfOrder(direction.value)" class="mt-2 text-sm text-warning">
          {{ t('pivot.positionAssist.anglesOutOfOrder') }}
        </p>
      </div>
      <p class="mt-3 text-xs text-muted">
        {{ t('pivot.positionAssist.footnote') }}
      </p>
    </template>
  </section>
</template>
