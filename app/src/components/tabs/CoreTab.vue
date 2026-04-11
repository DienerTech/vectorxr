<script setup lang="ts">
import { computed } from 'vue'

import AppRegistryEditor from '../AppRegistryEditor.vue'
import ThemeToggle from '../ThemeToggle.vue'
import type { VectorXRConfig } from '../../lib/model'
import type { ThemePreference } from '../../lib/theme'

const props = defineProps<{
  config: VectorXRConfig
  path: string
  logPath?: string
  themePreference: ThemePreference
}>()

defineEmits<{
  viewLogs: []
  addApplication: []
  removeApplication: [index: number]
  'update:themePreference': [value: ThemePreference]
}>()

const moduleCards = computed(() => [
  {
    name: 'Depth',
    description: 'Stereo depth tuning with per-title profiles.',
    status: props.config.modules.depthxr.enabled ? 'Enabled' : 'Disabled',
    detail: `${props.config.modules.depthxr.profiles.length} profile${props.config.modules.depthxr.profiles.length === 1 ? '' : 's'}`,
    tone: props.config.modules.depthxr.enabled ? 'active' : 'idle',
  },
  {
    name: 'Pivot',
    description: 'Rotation amplification controls and activation tuning.',
    status: props.config.modules.pivotxr.enabled ? 'Enabled' : 'Disabled',
    detail: `${props.config.modules.pivotxr.defaults.activationMode} activation`,
    tone: props.config.modules.pivotxr.enabled ? 'active' : 'idle',
  },
])
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Home</p>
          <h2 class="text-2xl font-semibold tracking-tight">VectorXR runtime settings</h2>
        </div>
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.core.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Suite Enabled
        </label>
      </div>

      <div class="grid gap-3 lg:grid-cols-[minmax(0,240px)_minmax(0,240px)_minmax(0,1fr)]">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Log Level</span>
          <select v-model="config.core.logLevel" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
            <option value="off">off</option>
            <option value="error">error</option>
            <option value="info">info</option>
            <option value="debug">debug</option>
          </select>
        </label>

        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Log Retention</span>
          <input
            v-model.number="config.core.logRetentionFiles"
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
            min="1"
            max="50"
            step="1"
            type="number"
          />
        </label>

        <div class="block">
          <span class="mb-1.5 block text-sm font-medium">Theme</span>
          <ThemeToggle :model-value="themePreference" @update:model-value="$emit('update:themePreference', $event)" />
        </div>

        <div class="rounded-[0.9rem] border border-dashed px-4 py-3 text-sm leading-6 surface-panel-soft">
          Shared suite settings stay here so logging, runtime intent, and module visibility remain consistent as the rest of the app grows.
        </div>
      </div>

      <div class="mt-4 flex flex-wrap items-center gap-3">
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('viewLogs')"
        >
          View Logs
        </button>
        <div class="rounded-full border px-4 py-2 text-sm surface-panel-strong">
          Latest Log: <span class="font-mono text-xs">{{ logPath || 'Resolve after first runtime attach' }}</span>
        </div>
      </div>
    </article>

    <AppRegistryEditor
      :applications="config.applications"
      @add="$emit('addApplication')"
      @remove="$emit('removeApplication', $event)"
    />

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <p class="eyebrow text-xs uppercase tracking-[0.24em]">Modules</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight">Module overview</h2>

      <div class="mt-4 grid gap-3 md:grid-cols-2">
        <div
          v-for="moduleCard in moduleCards"
          :key="moduleCard.name"
          class="rounded-[1rem] border p-4"
          :class="moduleCard.tone === 'active' ? 'surface-panel-strong' : 'surface-panel-soft'"
        >
          <div class="flex items-center justify-between gap-3">
            <div>
              <p class="text-base font-semibold tracking-tight">{{ moduleCard.name }}</p>
              <p class="mt-1 text-sm text-muted">{{ moduleCard.description }}</p>
            </div>
            <span
              class="rounded-full px-3 py-1 text-xs font-medium"
              :class="moduleCard.tone === 'active' ? 'chip-success' : 'chip-idle'"
            >
              {{ moduleCard.status }}
            </span>
          </div>
          <p class="eyebrow mt-4 text-xs uppercase tracking-[0.18em]">{{ moduleCard.detail }}</p>
        </div>
      </div>
    </article>

    <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel-inverse">
      <p class="text-xs uppercase tracking-[0.24em] text-inverse-muted">About</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight">VectorXR</h2>
      <p class="mt-3 max-w-3xl text-sm leading-6 text-inverse-muted">
        VectorXR keeps shared XR tuning in one place, with focused editors for depth and rotation behavior and a save flow that stays predictable.
      </p>

      <div class="mt-5 grid gap-3 lg:grid-cols-[minmax(0,1.3fr)_minmax(0,0.9fr)]">
        <div class="rounded-[1rem] border border-white/10 bg-white/5 p-4 text-sm text-inverse-muted">
          <p class="font-medium text-white">Built by Michael Diener - DienerTech</p>
          <ul class="mt-3 space-y-2.5 leading-6">
            <li>Copyright Michael Diener.</li>
            <li>One shared OpenXR layer, one shared config, and module-specific editing where it helps.</li>
            <li>Designed for tuning without losing track of what changed.</li>
          </ul>
        </div>

        <div class="rounded-[1rem] border border-white/10 bg-white/5 p-4 text-sm text-inverse-muted">
          <p class="font-medium text-white">Runtime Notes</p>
          <ul class="mt-3 space-y-2.5 leading-6">
            <li>Disabling the suite bypasses effects, but it does not unregister the OpenXR layer from the loader.</li>
            <li>Layer logs rotate inside the shared VectorXR log directory and respect the configured retention count.</li>
            <li>Shared Config Path: <span class="break-all font-mono text-xs md:text-sm">{{ path || 'Resolving...' }}</span></li>
          </ul>
        </div>
      </div>
    </article>
  </div>
</template>
