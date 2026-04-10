<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

import LogViewerModal from './components/LogViewerModal.vue'
import StickySaveBar from './components/StickySaveBar.vue'
import TopNavTabs from './components/TopNavTabs.vue'
import CoreTab from './components/tabs/CoreTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import { loadLogSnapshot, type LogSnapshot } from './lib/commands'
import { validateConfig } from './lib/validation'
import { useConfigStore } from './stores/configStore'

const store = useConfigStore()
const errors = computed(() => validateConfig(store.state.config))
const dirty = computed(() => store.isDirty.value)
const logViewerOpen = ref(false)
const logViewerLoading = ref(false)
const logSnapshot = ref<LogSnapshot | null>(null)

const activeSummary = computed(() => {
  if (store.state.activeTab === 'core') {
    return {
      eyebrow: 'Suite Shell',
      title: 'VectorXR now has a real home tab.',
      body: 'Global controls, module visibility, and shared save behavior are separated from feature editing so the suite can grow cleanly.',
    }
  }

  if (store.state.activeTab === 'depthxr') {
    return {
      eyebrow: 'DepthXR',
      title: 'Depth tuning lives in its own feature workspace.',
      body: 'Defaults and profiles stay focused on stereo boost and convergence while the runtime remains unchanged underneath.',
    }
  }

  return {
    eyebrow: 'PivotXR',
    title: 'The future rotation feature already has config ownership.',
    body: 'This keeps the app shell, validation, and persistence aligned before the runtime spike activates the feature path.',
  }
})

onMounted(() => {
  void store.load()
  void refreshLogs()
})

async function saveConfig() {
  if (errors.value.length > 0) {
    store.state.status = 'Fix validation errors before saving'
    return
  }

  await store.save()
}

async function refreshLogs() {
  logViewerLoading.value = true

  try {
    logSnapshot.value = await loadLogSnapshot()
  } catch (error) {
    store.state.status = error instanceof Error ? error.message : 'Failed to load logs'
  } finally {
    logViewerLoading.value = false
  }
}

async function openLogs() {
  logViewerOpen.value = true
  await refreshLogs()
}
</script>

<template>
  <main class="min-h-screen bg-[radial-gradient(circle_at_top_left,_rgba(185,111,61,0.18),_transparent_28%),linear-gradient(135deg,_#eff2eb_0%,_#f8f3e8_45%,_#d7c7a6_100%)] px-4 py-5 text-depthxr-ink md:px-6 xl:px-8">
    <section class="mx-auto max-w-[1500px] pb-6">
      <header class="mb-6 overflow-hidden rounded-[2rem] border border-black/10 bg-depthxr-ink text-white shadow-panel">
        <div class="grid gap-6 p-6 xl:grid-cols-[1.45fr_0.95fr]">
          <div>
            <p class="text-xs uppercase tracking-[0.32em] text-depthxr-sand">VectorXR</p>
            <h1 class="mt-2 text-4xl font-semibold tracking-tight xl:text-[2.8rem]">Phase 2 shell refactor for a modular XR utility suite.</h1>
            <p class="mt-3 max-w-3xl text-sm leading-6 text-white/72 md:text-[15px]">
              Milestone 2 separates the suite shell from feature editing, with dedicated tabs for VectorXR core controls, DepthXR tuning, and the
              upcoming PivotXR feature space.
            </p>
          </div>

          <div class="rounded-[1.6rem] border border-white/10 bg-white/5 p-4">
            <p class="text-xs uppercase tracking-[0.22em] text-white/56">{{ activeSummary.eyebrow }}</p>
            <p class="mt-2 text-2xl font-semibold tracking-tight">{{ activeSummary.title }}</p>
            <p class="mt-3 text-sm leading-6 text-white/72">{{ activeSummary.body }}</p>
            <p class="mt-4 text-sm text-white/72">{{ store.state.status }}</p>
          </div>
        </div>
      </header>

      <TopNavTabs :active-tab="store.state.activeTab" @select="store.setActiveTab" />

      <section class="mt-6 grid gap-6 xl:grid-cols-[minmax(0,1.28fr)_320px]">
        <div class="min-w-0">
          <CoreTab
            v-if="store.state.activeTab === 'core'"
            :config="store.state.config"
            :path="store.state.path"
            :log-path="logSnapshot?.activePath"
            @view-logs="openLogs"
          />
          <DepthXrTab
            v-else-if="store.state.activeTab === 'depthxr'"
            :config="store.state.config"
            @add-profile="store.addProfile"
            @remove-profile="store.removeProfile"
            @sync-profile-name="store.syncProfileName"
          />
          <PivotXrTab v-else :config="store.state.config" />
        </div>

        <aside class="space-y-4">
          <article class="rounded-[2rem] border border-black/10 bg-white/85 p-5 shadow-panel">
            <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Validation</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight text-depthxr-pine">Config Health</h2>

            <ul v-if="errors.length > 0" class="mt-3 space-y-2.5">
              <li
                v-for="error in errors"
                :key="error"
                class="rounded-2xl border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-700"
              >
                {{ error }}
              </li>
            </ul>

            <div v-else class="mt-3 rounded-2xl border border-emerald-200 bg-emerald-50 px-4 py-4 text-sm text-emerald-700">
              VectorXR config structure and value ranges look valid.
            </div>
          </article>

          <article class="rounded-[2rem] border border-black/10 bg-white/85 p-5 shadow-panel">
            <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Working Copy</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight text-depthxr-pine">Save Status</h2>
            <div class="mt-3 rounded-2xl border border-black/10 bg-[#fbf7ef] px-4 py-4 text-sm leading-6 text-depthxr-steel">
              <p><strong class="text-depthxr-pine">State:</strong> {{ dirty ? 'Unsaved changes' : 'Synced with disk' }}</p>
              <p class="mt-2"><strong class="text-depthxr-pine">Path:</strong></p>
              <p class="mt-1 break-all font-mono text-xs md:text-sm">{{ store.state.path || 'Resolving...' }}</p>
            </div>
          </article>

          <article class="rounded-[2rem] border border-black/10 bg-[#24322d] p-5 text-white shadow-panel">
            <p class="text-xs uppercase tracking-[0.24em] text-depthxr-sand">Milestone 2</p>
            <ul class="mt-3 space-y-2.5 text-sm leading-6 text-white/78">
              <li>The app shell now treats VectorXR, DepthXR, and PivotXR as separate product spaces.</li>
              <li>Dirty-state tracking keeps edits in a working copy until you explicitly save.</li>
              <li>The sticky save bar stays visible as you move between tabs, so save behavior remains consistent across the suite.</li>
            </ul>
          </article>
        </aside>
      </section>

      <StickySaveBar
        class="mt-6"
        :dirty="dirty"
        :saving="store.state.saving"
        :loading="store.state.loading"
        :status="store.state.status"
        :disabled="store.state.loading || store.state.saving || errors.length > 0"
        @save="saveConfig"
        @discard="store.discardChanges"
        @reload="store.load"
      />

      <LogViewerModal :open="logViewerOpen" :loading="logViewerLoading" :snapshot="logSnapshot" @close="logViewerOpen = false" />
    </section>
  </main>
</template>
