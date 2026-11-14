<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'

import GuideDiagram from '../GuideDiagram.vue'
import WalkthroughPlayer from '../WalkthroughPlayer.vue'
import { t, type MessageKey } from '../../i18n/index.ts'
import { en } from '../../i18n/locales/en/index.ts'
import { walkthroughs } from '../../lib/walkthroughs'
import { emphasisRuns, guideEntry, guideGroups, guideOrigin, guides, openGuideId, showGuide, type GuideId } from '../../lib/guides'
import type { AppTab } from '../../lib/model'

// Short user guides: a list of cards, and one guide at a time. The sections,
// steps and points of a guide are whatever its English catalog entry holds,
// in that order; translations mirror the keys.
const emit = defineEmits<{ navigate: [tab: AppTab] }>()

type SectionCatalog = { heading?: string; body?: string; steps?: Record<string, string>; points?: Record<string, string> }
const standardHeadings = new Set(Object.keys(en.guides.headings))

const key = (path: string) => path as MessageKey

const current = computed(() => (openGuideId.value ? guideEntry(openGuideId.value) : undefined))

const sections = computed(() => {
  const id = openGuideId.value
  if (!id) return []
  const catalog = en.guides.items[id].sections as Record<string, SectionCatalog>
  const base = `guides.items.${id}.sections`
  return Object.entries(catalog).map(([name, section]) => ({
    name,
    heading: section.heading ? t(key(`${base}.${name}.heading`)) : standardHeadings.has(name) ? t(key(`guides.headings.${name}`)) : '',
    body: section.body ? t(key(`${base}.${name}.body`)) : '',
    steps: Object.keys(section.steps ?? {}).map((step) => t(key(`${base}.${name}.steps.${step}`))),
    points: Object.keys(section.points ?? {}).map((point) => t(key(`${base}.${name}.points.${point}`))),
  }))
})

const groups = computed(() =>
  guideGroups.map((group) => ({
    group,
    label: t(key(`guides.groups.${group}`)),
    guides: guides
      .filter((guide) => guide.group === group)
      .map((guide) => ({ ...guide, title: t(key(`guides.items.${guide.id}.title`)), summary: t(key(`guides.items.${guide.id}.summary`)) })),
  })),
)

function tabLabel(tab: AppTab) {
  return t(key(`app.tabs.${tab}.label`))
}

function badgeClass(badge: string) {
  return badge === 'new' ? 'chip-accent' : badge === 'experimental' ? 'chip-warning' : 'chip-idle'
}

const top = ref<HTMLElement | null>(null)
watch(openGuideId, async () => {
  await nextTick()
  top.value?.scrollIntoView({ block: 'start' })
})

function open(id: GuideId) {
  showGuide(id)
}

// Back to the page whose "Read the guide" link opened this guide.
function back(tab: AppTab) {
  emit('navigate', tab)
}

// Leaving the page closes the guide, so the sidebar's Guides always opens
// the list; a "Read the guide" link opens its guide as it switches pages.
onBeforeUnmount(() => {
  showGuide(null)
})
</script>

<template>
  <div ref="top" class="space-y-4" data-guides>
    <article v-if="!current" class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <h2 class="text-2xl font-semibold tracking-tight">{{ t('guides.title') }}</h2>
      <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t('guides.intro') }}</p>

      <section v-for="group in groups" :key="group.group" class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <p class="eyebrow mb-3 text-xs font-semibold uppercase tracking-[0.24em]">{{ group.label }}</p>
        <div class="grid gap-3 md:grid-cols-2 xl:grid-cols-3">
          <button
            v-for="guide in group.guides"
            :key="guide.id"
            class="guide-card flex flex-col items-start gap-1.5 rounded-[1rem] border p-4 text-left transition surface-panel-soft"
            style="border-color: var(--app-border)"
            type="button"
            :data-guide="guide.id"
            @click="open(guide.id)"
          >
            <span class="flex flex-wrap items-center gap-2">
              <span class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ guide.title }}</span>
              <span
                v-for="badge in guide.badges"
                :key="badge"
                class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.14em]"
                :class="badgeClass(badge)"
              >{{ t(key(`guides.badges.${badge}`)) }}</span>
            </span>
            <span class="text-xs leading-5 text-muted">{{ guide.summary }}</span>
          </button>
        </div>
      </section>
    </article>

    <article v-else class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel" :data-guide-open="current.id">
      <div v-if="guideOrigin" class="flex flex-wrap items-center justify-between gap-3">
        <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium" type="button" data-guide-back @click="back(guideOrigin)">
          ← {{ t('guides.backTo', { page: tabLabel(guideOrigin) }) }}
        </button>
        <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium" type="button" @click="showGuide(null)">
          {{ t('guides.back') }}
        </button>
      </div>
      <div v-else class="flex flex-wrap items-center justify-between gap-3">
        <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium" type="button" @click="showGuide(null)">
          ← {{ t('guides.back') }}
        </button>
        <button
          v-if="current.tab"
          class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium"
          type="button"
          @click="emit('navigate', current.tab)"
        >
          {{ t('guides.openTab', { page: tabLabel(current.tab) }) }}
        </button>
      </div>

      <h2 class="mt-4 flex flex-wrap items-center gap-2 text-2xl font-semibold tracking-tight">
        {{ t(key(`guides.items.${current.id}.title`)) }}
        <span
          v-for="badge in current.badges"
          :key="badge"
          class="rounded-full px-2 py-0.5 text-[10px] font-semibold uppercase tracking-[0.14em]"
          :class="badgeClass(badge)"
        >{{ t(key(`guides.badges.${badge}`)) }}</span>
      </h2>
      <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t(key(`guides.items.${current.id}.summary`)) }}</p>

      <WalkthroughPlayer v-if="walkthroughs[current.id]" :id="current.id" class="mt-4 max-w-3xl" />
      <GuideDiagram v-else :id="current.id" class="mt-4 max-w-3xl" />

      <section v-for="section in sections" :key="section.name" class="mt-5 max-w-3xl" :data-guide-section="section.name">
        <h3 v-if="section.heading" class="text-base font-semibold tracking-tight">{{ section.heading }}</h3>
        <p v-if="section.body" class="mt-1.5 text-sm leading-6" style="color: var(--app-text)">
          <template v-for="(run, index) in emphasisRuns(section.body)" :key="index"><strong v-if="run.strong">{{ run.text }}</strong><template v-else>{{ run.text }}</template></template>
        </p>
        <ol v-if="section.steps.length" class="guide-steps mt-2 space-y-2 text-sm leading-6" style="color: var(--app-text)">
          <li v-for="(step, index) in section.steps" :key="index" class="flex gap-3">
            <span class="guide-step-number mt-0.5 inline-flex h-5 w-5 shrink-0 items-center justify-center rounded-full text-[11px] font-semibold">{{ index + 1 }}</span>
            <span><template v-for="(run, runIndex) in emphasisRuns(step)" :key="runIndex"><strong v-if="run.strong">{{ run.text }}</strong><template v-else>{{ run.text }}</template></template></span>
          </li>
        </ol>
        <ul v-if="section.points.length" class="mt-2 list-disc space-y-1.5 pl-5 text-sm leading-6" style="color: var(--app-text)">
          <li v-for="(point, index) in section.points" :key="index">
            <template v-for="(run, runIndex) in emphasisRuns(point)" :key="runIndex"><strong v-if="run.strong">{{ run.text }}</strong><template v-else>{{ run.text }}</template></template>
          </li>
        </ul>
      </section>
    </article>
  </div>
</template>

<style scoped>
.guide-card:hover {
  border-color: var(--app-accent) !important;
  transform: translateY(-1px);
}
.guide-step-number {
  background: var(--app-accent-soft);
  color: var(--app-accent-soft-text);
}
</style>
