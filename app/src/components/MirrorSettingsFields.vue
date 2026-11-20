<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

import OptionCards from './OptionCards.vue'
import { t } from '../i18n/index.ts'
import { listMonitors, type MonitorInfo } from '../lib/commands'
import { mirrorAspects, mirrorFillsMonitor, mirrorLimits, mirrorWindowSize, monitorDisplayName, type MirrorEye, type MirrorSettings } from '../lib/model'

// Shared stabilized mirror window editor for the Default Profile and each
// custom profile. Fields mutate the passed-in reactive `settings` object
// directly.
const props = defineProps<{
  settings: MirrorSettings
}>()

// Translated at render so a language change applies.
const eyeOptions = computed<{ value: MirrorEye; label: string; description: string }[]>(() => [
  { value: 'left', label: t('utilities.mirror.fields.eye.left.label'), description: t('utilities.mirror.fields.eye.left.description') },
  { value: 'right', label: t('utilities.mirror.fields.eye.right.label'), description: t('utilities.mirror.fields.eye.right.description') },
])

// Smoothing reads Off at 0: the window is then a plain mirror.
const smoothingText = computed(() => props.settings.smoothing === 0 ? t('utilities.mirror.fields.smoothing.off') : String(props.settings.smoothing))

// The picture size the window opens with; nothing while the width is not a number in range.
const openingSize = computed(() => {
  const { width } = props.settings
  if (!Number.isInteger(width) || width < mirrorLimits.width.min || width > mirrorLimits.width.max) return null
  return mirrorWindowSize(props.settings)
})

// The PC's monitors for the Fill a monitor picker. Outside the desktop app,
// or when they cannot be read, the list is empty and the page says where they
// are listed; that is not an error.
const monitors = ref<MonitorInfo[]>([])
const monitorsLoading = ref(false)
async function refreshMonitors() {
  monitorsLoading.value = true
  try {
    monitors.value = await listMonitors()
  } catch {
    monitors.value = []
  } finally {
    monitorsLoading.value = false
  }
}
onMounted(refreshMonitors)

// With a monitor filled, the picture takes that monitor's shape.
const fillsMonitor = computed(() => mirrorFillsMonitor(props.settings))
const monitorOptions = computed(() => monitors.value.map((monitor) => ({
  value: monitor.name,
  label: t(monitor.primary ? 'utilities.mirror.fields.monitor.optionPrimary' : 'utilities.mirror.fields.monitor.option', {
    name: monitorDisplayName(monitor.name),
    width: monitor.width,
    height: monitor.height,
  }),
})))
// A saved monitor that is not in the list stays selected. It reads "not
// connected" once the PC's monitors are known.
const missingMonitor = computed(() => {
  const saved = props.settings.monitor
  if (typeof saved !== 'string' || saved === '' || monitors.value.some((monitor) => monitor.name === saved)) return null
  const name = monitorDisplayName(saved)
  return { value: saved, label: monitors.value.length > 0 ? t('utilities.mirror.fields.monitor.notConnected', { name }) : name }
})
</script>

<template>
  <div class="grid gap-3 xl:grid-cols-2">
    <div class="space-y-4 rounded-[1rem] border p-4 surface-panel-soft" data-mirror-group="stabilization">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('utilities.mirror.fields.stabilization.heading') }}</p>

      <div>
        <div class="flex flex-wrap items-center justify-between gap-2 text-sm font-medium">
          <span>{{ t('utilities.mirror.fields.smoothing.label') }}</span>
          <output class="text-sm font-semibold tabular-nums">{{ smoothingText }}</output>
        </div>
        <div class="mt-2 flex items-center gap-3">
          <input v-model.number="settings.smoothing" class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper" :aria-label="t('utilities.mirror.fields.smoothing.label')" type="range" :min="mirrorLimits.smoothing.min" :max="mirrorLimits.smoothing.max" step="1" />
          <input v-model.number="settings.smoothing" class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm" :aria-label="t('utilities.mirror.fields.valueLabel', { setting: t('utilities.mirror.fields.smoothing.label') })" type="number" :min="mirrorLimits.smoothing.min" :max="mirrorLimits.smoothing.max" step="1" />
        </div>
        <p class="mt-1 text-xs leading-5 text-muted">{{ t('utilities.mirror.fields.smoothing.help') }}</p>
      </div>

      <div>
        <div class="flex flex-wrap items-center justify-between gap-2 text-sm font-medium">
          <span>{{ t('utilities.mirror.fields.margin.label') }}</span>
          <output class="text-sm font-semibold tabular-nums">{{ t('utilities.mirror.fields.percent', { value: settings.margin }) }}</output>
        </div>
        <div class="mt-2 flex items-center gap-3">
          <input v-model.number="settings.margin" class="h-2 min-w-0 flex-1 cursor-pointer accent-depthxr-copper" :aria-label="t('utilities.mirror.fields.margin.label')" type="range" :min="mirrorLimits.margin.min" :max="mirrorLimits.margin.max" step="1" />
          <input v-model.number="settings.margin" class="app-input w-[4.5rem] shrink-0 rounded-[0.75rem] px-2 py-1.5 text-right text-sm" :aria-label="t('utilities.mirror.fields.valueLabel', { setting: t('utilities.mirror.fields.margin.label') })" type="number" :min="mirrorLimits.margin.min" :max="mirrorLimits.margin.max" step="1" />
        </div>
        <p class="mt-1 text-xs leading-5 text-muted">{{ t('utilities.mirror.fields.margin.help') }}</p>
      </div>

      <div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="settings.levelHorizon" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ t('utilities.mirror.fields.levelHorizon.label') }}
        </label>
        <p class="mt-1 text-xs leading-5 text-muted">{{ t('utilities.mirror.fields.levelHorizon.help') }}</p>
      </div>
    </div>

    <div class="space-y-4 rounded-[1rem] border p-4 surface-panel-soft" data-mirror-group="window">
      <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('utilities.mirror.fields.window.heading') }}</p>

      <OptionCards v-model="settings.eye" :legend="t('utilities.mirror.fields.eye.legend')" :options="eyeOptions" />

      <div data-mirror-fill>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="settings.fillMonitor" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ t('utilities.mirror.fields.fillMonitor.label') }}
        </label>
        <p class="mt-1 text-xs leading-5 text-muted">{{ t('utilities.mirror.fields.fillMonitor.help') }}</p>

        <div v-if="settings.fillMonitor" class="mt-3">
          <div class="flex flex-wrap items-end gap-2">
            <label class="block min-w-0 flex-1 text-sm font-medium">
              {{ t('utilities.mirror.fields.monitor.label') }}
              <select v-model="settings.monitor" class="app-input mt-2 w-full rounded-[0.75rem] px-3 py-2 text-sm" data-mirror-monitor>
                <option value="">{{ t('utilities.mirror.fields.monitor.choose') }}</option>
                <option v-for="option in monitorOptions" :key="option.value" :value="option.value">{{ option.label }}</option>
                <option v-if="missingMonitor" :value="missingMonitor.value">{{ missingMonitor.label }}</option>
              </select>
            </label>
            <button class="button-secondary rounded-[0.75rem] px-3 py-2 text-sm font-medium" type="button" :disabled="monitorsLoading" @click="refreshMonitors">
              {{ t('common.refresh') }}
            </button>
          </div>
          <p v-if="!monitorsLoading && monitors.length === 0" class="mt-1 text-xs leading-5 text-muted" data-mirror-monitors-unavailable>{{ t('utilities.mirror.fields.monitor.unavailable') }}</p>
          <p class="mt-1 text-xs leading-5 text-muted" role="note">
            {{ fillsMonitor ? t('utilities.mirror.fields.monitor.covers') : t('utilities.mirror.fields.monitor.none') }}
          </p>
        </div>
      </div>

      <div class="grid gap-3 sm:grid-cols-3">
        <label class="block text-sm font-medium" :class="fillsMonitor ? 'opacity-55' : ''">
          {{ t('utilities.mirror.fields.aspect.label') }}
          <select v-model="settings.aspect" class="app-input mt-2 w-full rounded-[0.75rem] px-3 py-2 text-sm" :disabled="fillsMonitor">
            <option v-for="aspect in mirrorAspects" :key="aspect" :value="aspect">{{ aspect }}</option>
          </select>
        </label>
        <label class="block text-sm font-medium" :class="fillsMonitor ? 'opacity-55' : ''">
          {{ t('utilities.mirror.fields.width.label') }}
          <input v-model.number="settings.width" class="app-input mt-2 w-full rounded-[0.75rem] px-3 py-2 text-sm" type="number" :disabled="fillsMonitor" :min="mirrorLimits.width.min" :max="mirrorLimits.width.max" step="1" />
        </label>
        <label class="block text-sm font-medium">
          {{ t('utilities.mirror.fields.frameRate.label') }}
          <input v-model.number="settings.frameRate" class="app-input mt-2 w-full rounded-[0.75rem] px-3 py-2 text-sm" type="number" :min="mirrorLimits.frameRate.min" :max="mirrorLimits.frameRate.max" step="1" />
        </label>
      </div>
      <ul class="list-disc space-y-1 pl-5 text-xs leading-5 text-muted">
        <li v-if="fillsMonitor" data-mirror-size-unused>{{ t('utilities.mirror.fields.fillMonitor.sizeUnused') }}</li>
        <li v-else>{{ t('utilities.mirror.fields.width.help', { min: mirrorLimits.width.min, max: mirrorLimits.width.max }) }}</li>
        <li v-if="openingSize && !fillsMonitor" data-mirror-opening-size>{{ t('utilities.mirror.fields.width.opensAt', { width: openingSize.width, height: openingSize.height }) }}</li>
        <li>{{ t('utilities.mirror.fields.frameRate.help', { min: mirrorLimits.frameRate.min, max: mirrorLimits.frameRate.max }) }}</li>
      </ul>
    </div>
  </div>
</template>
