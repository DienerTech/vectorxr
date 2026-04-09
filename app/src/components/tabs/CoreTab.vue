<script setup lang="ts">
import type { VectorXRConfig } from '../../lib/model'

defineProps<{
  config: VectorXRConfig
  path: string
}>()
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
          Suite-level settings now live at the top of the config so logging and global enablement stay shared across current and future features.
        </div>
      </div>
    </article>

    <article class="rounded-[2rem] border border-black/10 bg-white/80 p-5 shadow-panel backdrop-blur">
      <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Modules</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight text-depthxr-pine">Module Overview</h2>

      <div class="mt-4 grid gap-3 md:grid-cols-2">
        <div class="rounded-[1.6rem] border border-black/10 bg-[#fbf7ef] p-4">
          <div class="flex items-center justify-between gap-3">
            <div>
              <p class="text-base font-semibold tracking-tight">DepthXR</p>
              <p class="mt-1 text-sm text-depthxr-steel">Active runtime feature path for stereo depth tuning.</p>
            </div>
            <span class="rounded-full px-3 py-1 text-xs font-medium" :class="config.modules.depthxr.enabled ? 'bg-emerald-100 text-emerald-700' : 'bg-stone-200 text-stone-700'">
              {{ config.modules.depthxr.enabled ? 'Enabled' : 'Disabled' }}
            </span>
          </div>
        </div>

        <div class="rounded-[1.6rem] border border-black/10 bg-[#fbf7ef] p-4">
          <div class="flex items-center justify-between gap-3">
            <div>
              <p class="text-base font-semibold tracking-tight">PivotXR</p>
              <p class="mt-1 text-sm text-depthxr-steel">Reserved module settings for the upcoming rotation pass.</p>
            </div>
            <span class="rounded-full px-3 py-1 text-xs font-medium" :class="config.modules.pivotxr.enabled ? 'bg-emerald-100 text-emerald-700' : 'bg-stone-200 text-stone-700'">
              {{ config.modules.pivotxr.enabled ? 'Enabled' : 'Disabled' }}
            </span>
          </div>
        </div>
      </div>
    </article>

    <article class="rounded-[2rem] border border-black/10 bg-[#24322d] p-5 text-white shadow-panel">
      <p class="text-xs uppercase tracking-[0.24em] text-depthxr-sand">About</p>
      <h2 class="mt-2 text-2xl font-semibold tracking-tight">VectorXR Suite Shell</h2>
      <p class="mt-3 text-sm leading-6 text-white/76">
        This phase separates suite controls from feature controls so the app can scale cleanly as more XR utilities come online.
      </p>
      <div class="mt-4 rounded-[1.4rem] border border-white/10 bg-white/5 p-4 text-sm text-white/72">
        <p class="font-medium text-white">Shared Config Path</p>
        <p class="mt-2 break-all font-mono text-xs md:text-sm">{{ path || 'Resolving...' }}</p>
      </div>
    </article>
  </div>
</template>
