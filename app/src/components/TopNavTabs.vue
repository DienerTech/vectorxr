<script setup lang="ts">
import { computed } from 'vue'

import type { AppTab } from '../lib/model'

const props = defineProps<{
  activeTab: AppTab
  tabs: Array<{
    id: AppTab
    label: string
    subtitle: string
    status: string
  }>
}>()

defineEmits<{
  select: [tab: AppTab]
}>()

const primaryTabs = computed(() => props.tabs.filter((tab) => tab.id === 'core' || tab.id === 'registry' || tab.id === 'about'))
const moduleTabs = computed(() => props.tabs.filter((tab) => tab.id === 'depthxr' || tab.id === 'pivotxr'))
</script>

<template>
  <nav class="rounded-[1.25rem] border p-3 backdrop-blur tab-shell">
    <div class="grid gap-2 md:grid-cols-3">
      <button
        v-for="tab in primaryTabs"
        :key="tab.id"
        class="rounded-[1rem] border px-4 py-3 text-left transition"
        :class="activeTab === tab.id ? 'tab-button-active' : 'tab-button-idle'"
        type="button"
        @click="$emit('select', tab.id)"
      >
        <p class="text-sm font-semibold tracking-tight">{{ tab.label }}</p>
        <p class="mt-1 text-xs" :class="activeTab === tab.id ? 'text-inverse-muted' : 'text-muted'">{{ tab.subtitle }}</p>
        <p class="mt-3 text-[11px] font-medium uppercase tracking-[0.16em]" :class="activeTab === tab.id ? 'text-inverse-muted' : 'eyebrow'">
          {{ tab.status }}
        </p>
      </button>
    </div>

    <div class="mt-2 grid gap-2 md:grid-cols-2">
      <button
        v-for="tab in moduleTabs"
        :key="tab.id"
        class="rounded-[1rem] border px-4 py-3 text-left transition"
        :class="activeTab === tab.id ? 'tab-button-active' : 'tab-button-idle'"
        type="button"
        @click="$emit('select', tab.id)"
      >
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="text-sm font-semibold tracking-tight">{{ tab.label }}</p>
            <p class="mt-1 text-xs" :class="activeTab === tab.id ? 'text-inverse-muted' : 'text-muted'">{{ tab.subtitle }}</p>
          </div>
          <span
            class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="tab.status === 'Enabled' ? 'chip-success' : 'chip-idle'"
          >
            {{ tab.status }}
          </span>
        </div>
      </button>
    </div>
  </nav>
</template>
