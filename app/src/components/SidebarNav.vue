<script setup lang="ts">
import { computed, nextTick, reactive, ref, watch } from 'vue'

import logoUrl from '../assets/logo.png'
import { t, type MessageKey } from '../i18n/index.ts'
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
    statusWarning?: boolean
    enhancementActive?: boolean
  }>
}>()

defineEmits<{
  select: [tab: AppTab]
  showUpdates: []
}>()

interface SidebarGroup {
  id: string
  label?: MessageKey
  aggregateStatus?: boolean
  tabs: AppTab[]
}

const sections: { label: MessageKey; groups: SidebarGroup[] }[] = [
  {
    label: 'app.sidebar.appSettings',
    groups: [
      { id: 'app-settings', tabs: ['home', 'guides', 'core'] },
      { id: 'applications', label: 'app.sidebar.applications', tabs: ['registry', 'discovery'] },
      { id: 'app-info', tabs: ['supporters', 'about'] },
    ],
  },
  {
    label: 'app.sidebar.openxrUtilities',
    groups: [
      { id: 'openxr-utilities', tabs: ['osd', 'layers'] },
      { id: 'capture', label: 'app.sidebar.capture', tabs: ['screenshots', 'mirror'], aggregateStatus: true },
    ],
  },
  {
    label: 'app.sidebar.enhancements',
    groups: [
      { id: 'performance', label: 'app.sidebar.performance', tabs: ['quadviews', 'turbo', 'framegen'] },
      { id: 'look-and-feel', label: 'app.sidebar.lookAndFeel', tabs: ['depthxr', 'pivotxr'] },
    ],
  },
]

const nav = ref<HTMLElement | null>(null)
const collapsibleGroups = sections.flatMap(section => section.groups).filter(group => group.label)
const storageKey = 'vectorxr-sidebar-collapsed'
function loadCollapsed(): Set<string> {
  try {
    const saved: unknown = JSON.parse(window.localStorage.getItem(storageKey) ?? '[]')
    if (Array.isArray(saved)) return new Set(collapsibleGroups.filter(group => saved.includes(group.id)).map(group => group.id))
  } catch { /* Storage may be unavailable. */ }
  return new Set()
}

const collapsed = reactive(loadCollapsed())
const revealed = ref<string | null>(null)
const tabsById = computed(() => new Map(props.tabs.map(tab => [tab.id, tab])))

function tabsFor(group: SidebarGroup) {
  return group.tabs.map(id => tabsById.value.get(id)).filter((tab): tab is NonNullable<typeof tab> => tab !== undefined)
}

function groupActive(group: SidebarGroup) {
  return tabsFor(group).some(tab => tab.enhancementActive)
}

function isCollapsed(id: string) {
  return collapsed.has(id) && revealed.value !== id
}

function toggleGroup(id: string) {
  if (isCollapsed(id)) collapsed.delete(id)
  else collapsed.add(id)
  if (revealed.value === id) revealed.value = null
  try {
    window.localStorage.setItem(storageKey, JSON.stringify([...collapsed]))
  } catch { /* The disclosure still works for this session. */ }
}

// Reveal a destination selected from another page without changing the user's saved groups.
watch(() => props.activeTab, async id => {
  revealed.value = collapsibleGroups.find(group => group.tabs.includes(id))?.id ?? null
  await nextTick()
  nav.value?.querySelector<HTMLElement>('[data-tab="' + id + '"]')?.scrollIntoView({ block: 'nearest', inline: 'nearest' })
}, { immediate: true })
</script>

<template>
  <nav ref="nav" class="side-nav flex h-full min-h-0 w-56 shrink-0 flex-col overflow-y-auto border-r px-3 py-4">
    <section v-for="section in sections" :key="section.label" class="mb-4 shrink-0">
      <h2 class="side-nav-group-label px-2">{{ t(section.label) }}</h2>
      <div v-for="group in section.groups" :key="group.id" class="mt-1">
        <h3 v-if="group.label">
          <button
            class="side-nav-disclosure flex w-full items-center gap-2 rounded-lg px-3 py-2 text-left text-sm font-semibold"
            type="button"
            :aria-controls="'side-nav-' + group.id"
            :aria-expanded="!isCollapsed(group.id)"
            @click="toggleGroup(group.id)"
          >
            <span class="min-w-0 flex-1">{{ t(group.label) }}</span>
            <span v-if="group.aggregateStatus" class="nav-dot" :class="groupActive(group) ? 'nav-dot-on' : 'nav-dot-off'">
              <span class="sr-only">{{ groupActive(group) ? t('common.active') : t('common.inactive') }}</span>
            </span>
            <span aria-hidden="true" class="side-nav-chevron" />
          </button>
        </h3>
        <div
          v-show="!isCollapsed(group.id)"
          :id="'side-nav-' + group.id"
          class="grid gap-0.5"
          :class="{ 'side-nav-children': group.label }"
        >
          <button
            v-for="tab in tabsFor(group)"
            :key="tab.id"
            class="side-nav-item flex items-center justify-between gap-2 rounded-[0.65rem] px-3 py-1.5 text-left text-sm font-medium transition"
            :class="activeTab === tab.id ? 'side-nav-item-active' : ''"
            type="button"
            :data-tab="tab.id"
            :title="group.label ? t('app.sidebar.moduleTitle', { subtitle: tab.subtitle, status: tab.status }) : tab.subtitle"
            :aria-current="activeTab === tab.id ? 'page' : undefined"
            @click="$emit('select', tab.id)"
          >
            <span>{{ tab.label }}</span>
            <span v-if="tab.id === 'core' && tab.statusWarning" class="nav-dot nav-dot-warn" :title="t('app.sidebar.suiteOff')" />
            <span v-else-if="tab.enhancementActive !== undefined" class="nav-dot" :class="tab.enhancementActive ? 'nav-dot-on' : 'nav-dot-off'" :title="tab.status" />
          </button>
        </div>
      </div>
    </section>

    <div class="mt-auto shrink-0 px-2">
      <p class="text-[0.68rem] leading-4 text-soft">{{ t('app.sidebar.saveHint') }}</p>

      <div class="mt-4 flex items-center gap-2.5 border-t pt-3" style="border-color: var(--app-border)">
        <img :src="logoUrl" :alt="t('app.sidebar.logoAlt')" class="side-nav-mark h-10 w-10 rounded-[0.6rem] object-contain" />
        <div>
          <p class="text-sm font-semibold tracking-tight">VectorXR</p>
          <p class="text-[0.68rem] text-soft">{{ t('app.sidebar.version', { version }) }}</p>
        </div>
        <button
          v-if="updateAvailable"
          class="update-arrow ml-auto inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-full"
          type="button"
          :title="updateTooltip || t('app.sidebar.updateTooltip')"
          :aria-label="t('app.sidebar.updateAria')"
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
