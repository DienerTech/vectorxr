<script setup lang="ts">
import { computed } from 'vue'

import logoUrl from '../assets/logo.png'
import type { AppTab } from '../lib/model'

const props = defineProps<{
  activeTab: AppTab
  version: string
  updateAvailable?: boolean
  updateTooltip?: string
  tabs: Array<{
    id: AppTab
    label: string
    subtitle: string
    status: string
    badge?: string
    enhancementActive?: boolean
  }>
}>()

defineEmits<{
  select: [tab: AppTab]
  showUpdates: []
}>()

const primaryTabs = computed(() => props.tabs.filter((tab) => tab.id === 'home' || tab.id === 'core' || tab.id === 'registry' || tab.id === 'layers' || tab.id === 'about'))
const moduleTabs = computed(() => {
  const moduleOrder: AppTab[] = ['quadviews', 'pivotxr', 'depthxr']
  return moduleOrder
    .map((id) => props.tabs.find((tab) => tab.id === id))
    .filter((tab): tab is NonNullable<typeof tab> => tab !== undefined)
})

function moduleDotClass(tab: { enhancementActive?: boolean }): string {
  return tab.enhancementActive ? 'nav-dot-on' : 'nav-dot-off'
}
</script>

<template>
  <nav class="side-nav flex h-full w-56 shrink-0 flex-col border-r px-3 py-4">
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
        :title="`${tab.subtitle}. ${tab.status}`"
        @click="$emit('select', tab.id)"
      >
        <span class="flex items-center gap-1.5">
          {{ tab.label }}
        </span>
        <span class="nav-dot" :class="moduleDotClass(tab)" :title="tab.status" />
      </button>
    </div>

    <div class="mt-auto px-2 pt-4">
      <p class="text-[0.68rem] leading-4 text-soft">Enabling or disabling an enhancement will apply to future OpenXR app launches.</p>

      <div class="mt-4 flex items-center gap-2.5 border-t pt-3" style="border-color: var(--app-border)">
        <img :src="logoUrl" alt="VectorXR logo" class="side-nav-mark h-10 w-10 rounded-[0.6rem] object-contain" />
        <div>
          <p class="text-sm font-semibold tracking-tight">VectorXR</p>
          <p class="text-[0.68rem] text-soft">v{{ version }}</p>
        </div>
        <button
          v-if="updateAvailable"
          class="update-arrow ml-auto inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-full"
          type="button"
          :title="updateTooltip || 'A new version is available. Click for details.'"
          aria-label="A new version is available"
          @click="$emit('showUpdates')"
        >
          <svg aria-hidden="true" viewBox="0 0 20 20" fill="currentColor" class="h-4 w-4">
            <path fill-rule="evenodd" d="M10 3a1 1 0 0 1 .707.293l5 5a1 1 0 0 1-1.414 1.414L11 6.414V16a1 1 0 1 1-2 0V6.414L5.707 9.707a1 1 0 0 1-1.414-1.414l5-5A1 1 0 0 1 10 3Z" clip-rule="evenodd" />
          </svg>
        </button>
      </div>
    </div>
  </nav>
</template>

<style scoped>
.update-arrow {
  color: #22c55e;
  border: 1px solid rgba(34, 197, 94, 0.45);
  background: rgba(34, 197, 94, 0.12);
  /* Slow, gentle blink a few times when it first appears, then settle fully visible. */
  animation: update-arrow-blink 1s ease-in-out 3;
  transition: transform 0.15s ease, background 0.15s ease;
}

.update-arrow:hover {
  transform: translateY(-1px);
  background: rgba(34, 197, 94, 0.22);
}

@keyframes update-arrow-blink {
  0%,
  100% {
    opacity: 1;
  }
  50% {
    opacity: 0.2;
  }
}

@media (prefers-reduced-motion: reduce) {
  .update-arrow {
    animation: none;
  }
}
</style>
