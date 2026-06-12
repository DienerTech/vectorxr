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
    badge?: string
  }>
}>()

defineEmits<{
  select: [tab: AppTab]
}>()

const primaryTabs = computed(() => props.tabs.filter((tab) => tab.id === 'core' || tab.id === 'registry' || tab.id === 'layers' || tab.id === 'about'))
const moduleTabs = computed(() => {
  const moduleOrder: AppTab[] = ['quadviews', 'pivotxr', 'depthxr']
  return moduleOrder
    .map((id) => props.tabs.find((tab) => tab.id === id))
    .filter((tab): tab is NonNullable<typeof tab> => tab !== undefined)
})

function statusChipClass(tab: { id: AppTab; status: string }): string {
  if (tab.id === 'core') {
    return tab.status === 'Suite on' ? 'chip-success' : 'chip-idle'
  }

  if (tab.id === 'depthxr' || tab.id === 'pivotxr' || tab.id === 'quadviews') {
    return tab.status === 'Enabled' ? 'chip-success' : 'chip-idle'
  }

  return 'chip-accent'
}
</script>

<template>
  <nav class="rounded-[1.25rem] border p-3 backdrop-blur tab-shell">
    <div class="tab-group">
      <p class="tab-group-label">App Settings</p>
      <div class="grid gap-2 md:grid-cols-4">
        <button
          v-for="tab in primaryTabs"
          :key="tab.id"
          class="rounded-[1rem] border px-4 py-3 text-left transition"
          :class="activeTab === tab.id ? 'tab-button-active' : 'tab-button-idle'"
          type="button"
          @click="$emit('select', tab.id)"
        >
          <div class="flex flex-wrap items-start justify-between gap-3">
            <div>
              <p class="flex flex-wrap items-center gap-2 text-sm font-semibold tracking-tight">
                <span>{{ tab.label }}</span>
                <span
                  v-if="tab.badge"
                  class="rounded-full px-2 py-0.5 text-[0.65rem] font-semibold uppercase tracking-[0.12em]"
                  :class="activeTab === tab.id ? 'chip-warning' : 'chip-accent'"
                >
                  {{ tab.badge }}
                </span>
              </p>
              <p class="mt-1 text-xs" :class="activeTab === tab.id ? 'text-inverse-muted' : 'text-muted'">{{ tab.subtitle }}</p>
            </div>
          </div>
        </button>
      </div>
    </div>

    <div class="tab-group mt-3">
      <p class="tab-group-label">OpenXR Tweaks</p>
      <div class="grid gap-2 md:grid-cols-3">
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
              :class="statusChipClass(tab)"
            >
              {{ tab.status }}
            </span>
          </div>
        </button>
      </div>
    </div>
  </nav>
</template>
