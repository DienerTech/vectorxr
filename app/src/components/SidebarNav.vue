<script setup lang="ts">
import { computed } from 'vue'

import logoUrl from '../assets/logo.png'
import type { AppTab } from '../lib/model'

const props = defineProps<{
  activeTab: AppTab
  version: string
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

const primaryTabs = computed(() => props.tabs.filter((tab) => tab.id === 'home' || tab.id === 'core' || tab.id === 'registry' || tab.id === 'layers'))
const moduleTabs = computed(() => {
  const moduleOrder: AppTab[] = ['quadviews', 'pivotxr', 'depthxr']
  return moduleOrder
    .map((id) => props.tabs.find((tab) => tab.id === id))
    .filter((tab): tab is NonNullable<typeof tab> => tab !== undefined)
})

function moduleDotClass(tab: { status: string }): string {
  return tab.status === 'Enabled' ? 'nav-dot-on' : 'nav-dot-off'
}
</script>

<template>
  <nav class="side-nav flex h-full w-56 shrink-0 flex-col border-r px-3 py-4">
    <div class="mb-5 flex items-center gap-2.5 px-2">
      <img :src="logoUrl" alt="VectorXR logo" class="side-nav-mark h-8 w-8 rounded-[0.6rem] object-contain" />
      <div>
        <p class="text-sm font-semibold tracking-tight">VectorXR</p>
        <p class="text-[0.68rem] text-soft">v{{ version }}</p>
      </div>
    </div>

    <p class="side-nav-group-label px-2">App Settings</p>
    <div class="mt-1.5 grid gap-1">
      <button
        v-for="tab in primaryTabs"
        :key="tab.id"
        class="side-nav-item flex items-center justify-between gap-2 rounded-[0.65rem] px-3 py-2 text-left text-sm font-medium transition"
        :class="activeTab === tab.id ? 'side-nav-item-active' : ''"
        type="button"
        :title="tab.subtitle"
        @click="$emit('select', tab.id)"
      >
        <span>{{ tab.label }}</span>
        <span v-if="tab.id === 'core' && tab.status === 'Suite off'" class="nav-dot nav-dot-warn" title="VectorXR runtime effects are off" />
      </button>
    </div>

    <p class="side-nav-group-label mt-5 px-2">OpenXR Enhancements</p>
    <div class="mt-1.5 grid gap-1">
      <button
        v-for="tab in moduleTabs"
        :key="tab.id"
        class="side-nav-item flex items-center justify-between gap-2 rounded-[0.65rem] px-3 py-2 text-left text-sm font-medium transition"
        :class="activeTab === tab.id ? 'side-nav-item-active' : ''"
        type="button"
        :title="tab.badge ? `${tab.subtitle} (Experimental)` : tab.subtitle"
        @click="$emit('select', tab.id)"
      >
        <span class="flex items-center gap-1.5">
          {{ tab.label }}
          <span v-if="tab.badge" class="side-nav-exp rounded-full px-1.5 py-0.5 text-[0.58rem] font-bold uppercase tracking-[0.1em]">Exp</span>
        </span>
        <span class="nav-dot" :class="moduleDotClass(tab)" :title="tab.status" />
      </button>
    </div>

    <div class="mt-auto px-2 pt-4">
      <p class="text-[0.68rem] leading-4 text-soft">Changes apply to future OpenXR app launches.</p>
    </div>
  </nav>
</template>
