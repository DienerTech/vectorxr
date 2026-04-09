<script setup lang="ts">
import type { AppTab } from '../lib/model'

defineProps<{
  activeTab: AppTab
}>()

defineEmits<{
  select: [tab: AppTab]
}>()

const tabs: Array<{ id: AppTab; label: string; subtitle: string }> = [
  { id: 'core', label: 'VectorXR', subtitle: 'Core suite controls' },
  { id: 'depthxr', label: 'DepthXR', subtitle: 'Stereo depth tuning' },
  { id: 'pivotxr', label: 'PivotXR', subtitle: 'Rotation amplification' },
]
</script>

<template>
  <nav class="rounded-[2rem] border border-black/10 bg-white/75 p-3 shadow-panel backdrop-blur">
    <div class="grid gap-2 md:grid-cols-3">
      <button
        v-for="tab in tabs"
        :key="tab.id"
        class="rounded-[1.4rem] border px-4 py-3 text-left transition"
        :class="
          activeTab === tab.id
            ? 'border-depthxr-pine bg-depthxr-pine text-white'
            : 'border-black/10 bg-white/70 text-depthxr-ink hover:border-depthxr-copper hover:bg-[#fbf7ef]'
        "
        type="button"
        @click="$emit('select', tab.id)"
      >
        <p class="text-sm font-semibold tracking-tight">{{ tab.label }}</p>
        <p class="mt-1 text-xs" :class="activeTab === tab.id ? 'text-white/72' : 'text-depthxr-steel'">{{ tab.subtitle }}</p>
      </button>
    </div>
  </nav>
</template>
