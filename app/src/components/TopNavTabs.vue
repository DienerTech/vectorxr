<script setup lang="ts">
import type { AppTab } from '../lib/model'

defineProps<{
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
</script>

<template>
  <nav class="rounded-[1.25rem] border p-3 backdrop-blur tab-shell">
    <div class="grid gap-2 md:grid-cols-3">
      <button
        v-for="tab in tabs"
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
  </nav>
</template>
