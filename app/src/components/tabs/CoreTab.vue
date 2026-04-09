<script setup lang="ts">
import { computed } from 'vue'

import type { VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  path: string
}>()

const moduleCards = computed(() => [
  {
    name: 'DepthXR',
    description: 'Stereo depth tuning with active runtime support.',
    status: props.config.modules.depthxr.enabled ? 'Enabled' : 'Disabled',
    detail: `${props.config.modules.depthxr.profiles.length} profile${props.config.modules.depthxr.profiles.length === 1 ? '' : 's'}`,
    tone: props.config.modules.depthxr.enabled ? 'active' : 'idle',
  },
  {
    name: 'PivotXR',
    description: 'Rotation amplification shell, ready for the runtime spike.',
    status: props.config.modules.pivotxr.enabled ? 'Enabled' : 'Disabled',
    detail: `${props.config.modules.pivotxr.defaults.activationMode} activation`,
    tone: props.config.modules.pivotxr.enabled ? 'active' : 'idle',
  },
])
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[2rem] border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Core</p>
          <h2 class="text-2xl font-semibold tracking-tight text-depthxr-pine">VectorXR Runtime Settings</h2>
        </div>
        <label class="inline-flex items-center gap-3 rounded-full bg-depthxr-pine px-4 py-2 text-sm font-medium text-white">
          <input v-model="config.core.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Suite Enabled
        </label>
      </div>

      <div class="grid gap-3 lg:grid-cols-[minmax(0,240px)_minmax(0,240px)_minmax(0,1fr)]">
        <label class="block">
          <span class="mb-1.5 block text-sm font-medium">Log Level</span>
          <select v-model="config.core.logLevel" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5">
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
            class="w-full rounded-2xl border border-black/10 bg-white px-4 py-2.5"
            min="1"
            max="50"
            step="1"
            type="number"
          />
        </label>

        <div class="rounded-2xl border border-dashed border-black/10 bg-[#f7f2e8] px-4 py-3 text-sm leading-6 text-depthxr-steel">
          Suite-level settings stay centralized here so logging, lifecycle intent, and module visibility remain consistent as more features arrive.
        </div>
      </div>
    </article>

    <article class="rounded-[2rem] border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
      <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Modules</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight text-depthxr-pine">Module Overview</h2>

      <div class="mt-4 grid gap-3 md:grid-cols-2">
        <div
          v-for="moduleCard in moduleCards"
          :key="moduleCard.name"
          class="rounded-[1.6rem] border p-4"
          :class="moduleCard.tone === 'active' ? 'border-emerald-200 bg-emerald-50/70' : 'border-black/10 bg-[#fbf7ef]'"
        >
          <div class="flex items-center justify-between gap-3">
            <div>
              <p class="text-base font-semibold tracking-tight">{{ moduleCard.name }}</p>
              <p class="mt-1 text-sm text-depthxr-steel">{{ moduleCard.description }}</p>
            </div>
            <span
              class="rounded-full px-3 py-1 text-xs font-medium"
              :class="moduleCard.tone === 'active' ? 'bg-emerald-100 text-emerald-700' : 'bg-stone-200 text-stone-700'"
            >
              {{ moduleCard.status }}
            </span>
          </div>
          <p class="mt-4 text-xs uppercase tracking-[0.18em] text-depthxr-copper">{{ moduleCard.detail }}</p>
        </div>
      </div>
    </article>

    <article class="rounded-[2rem] border border-black/10 bg-[#24322d] p-5 text-white shadow-panel">
      <p class="text-xs uppercase tracking-[0.24em] text-depthxr-sand">About</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight">VectorXR Suite Shell</h2>
      <p class="mt-3 max-w-3xl text-sm leading-6 text-white/76">
        VectorXR is evolving from a single depth tool into a coordinated suite of XR utilities. The shell work in this phase makes room for more
        features without splintering the runtime or the user experience.
      </p>

      <div class="mt-5 grid gap-3 lg:grid-cols-[minmax(0,1.3fr)_minmax(0,0.9fr)]">
        <div class="rounded-[1.4rem] border border-white/10 bg-white/5 p-4 text-sm text-white/72">
          <p class="font-medium text-white">Suite Principles</p>
          <ul class="mt-3 space-y-2.5 leading-6">
            <li>One shared runtime pipeline under the hood.</li>
            <li>Feature-specific tabs and settings ownership in the app.</li>
            <li>Explicit save and validation behavior across the whole suite.</li>
          </ul>
        </div>

        <div class="rounded-[1.4rem] border border-white/10 bg-white/5 p-4 text-sm text-white/72">
          <p class="font-medium text-white">Shared Config Path</p>
          <p class="mt-2 break-all font-mono text-xs md:text-sm">{{ path || 'Resolving...' }}</p>
        </div>
      </div>
    </article>
  </div>
</template>
