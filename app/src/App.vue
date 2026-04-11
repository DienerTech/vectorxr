<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppHeader from './components/AppHeader.vue'
import LogViewerModal from './components/LogViewerModal.vue'
import PatchNotesModal from './components/PatchNotesModal.vue'
import StickySaveBar from './components/StickySaveBar.vue'
import TopNavTabs from './components/TopNavTabs.vue'
import CoreTab from './components/tabs/CoreTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import { loadLogSnapshot, type LogSnapshot } from './lib/commands'
import { patchNotes } from './lib/patchNotes'
import { applyThemePreference, loadThemePreference, observeSystemThemeChanges, type ThemePreference } from './lib/theme'
import { validateConfig } from './lib/validation'
import { useConfigStore } from './stores/configStore'

const store = useConfigStore()
const errors = computed(() => validateConfig(store.state.config))
const dirty = computed(() => store.isDirty.value)
const logViewerOpen = ref(false)
const patchNotesOpen = ref(false)
const logViewerLoading = ref(false)
const logSnapshot = ref<LogSnapshot | null>(null)
const themePreference = ref<ThemePreference>(loadThemePreference())

const latestPatch = patchNotes[0]

const activeSummary = computed(() => {
  if (store.state.activeTab === 'core') {
    return {
      eyebrow: 'Home',
      title: 'Shared controls, release notes, and save state stay close at hand.',
      body: 'Use the Home tab to keep an eye on suite-wide settings, logs, and the shape of the working copy while Depth and Pivot stay focused on their own tuning.',
      panelTitle: 'Home focus',
      panelBody: 'Suite-wide controls, log access, and the product summary live here.',
    }
  }

  if (store.state.activeTab === 'depthxr') {
    return {
      eyebrow: 'Depth',
      title: 'Depth tuning stays lean, with defaults and per-game overrides in one place.',
      body: 'Stereo boost and convergence stay easy to adjust without mixing them back into unrelated shell or logging concerns.',
      panelTitle: 'Depth focus',
      panelBody: 'Defaults and profile overrides stay centered on depth tuning and quick comparison.',
    }
  }

  return {
    eyebrow: 'Pivot',
    title: 'Rotation tuning stays separate, so yaw and pitch changes read clearly.',
    body: 'Activation, smoothing, deadzones, and extra range now live in a cleaner workspace built for iteration instead of placeholders.',
    panelTitle: 'Pivot focus',
    panelBody: 'Activation behavior, rotation limits, and tuning controls stay grouped together here.',
  }
})

const tabs = computed(() => [
  {
    id: 'core' as const,
    label: 'Home',
    subtitle: 'Suite settings and logs',
    status: store.state.config.core.enabled ? 'Suite on' : 'Suite off',
  },
  {
    id: 'depthxr' as const,
    label: 'Depth',
    subtitle: 'Stereo depth tuning',
    status: store.state.config.modules.depthxr.enabled ? 'Enabled' : 'Disabled',
  },
  {
    id: 'pivotxr' as const,
    label: 'Pivot',
    subtitle: 'Rotation tuning',
    status: store.state.config.modules.pivotxr.enabled ? 'Enabled' : 'Disabled',
  },
])

watch(
  themePreference,
  (preference) => {
    applyThemePreference(preference)
  },
  { immediate: true },
)

let stopSystemThemeObservation = () => {}

onMounted(() => {
  stopSystemThemeObservation = observeSystemThemeChanges(() => {
    if (themePreference.value === 'system') {
      applyThemePreference(themePreference.value)
    }
  })

  void store.load()
  void refreshLogs()
})

onUnmounted(() => {
  stopSystemThemeObservation()
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
  <main class="app-shell-bg h-screen overflow-hidden px-4 py-4 md:px-6 xl:px-8">
    <section class="mx-auto flex h-full max-w-[1500px] flex-col">
      <div class="shrink-0 pb-4 pt-1">
        <AppHeader
          :eyebrow="activeSummary.eyebrow"
          :title="activeSummary.title"
          :body="activeSummary.body"
          :status="store.state.status || 'Ready.'"
          :dirty="dirty"
          :latest-patch="latestPatch"
          @open-patch-notes="patchNotesOpen = true"
        />

        <div class="mt-4">
          <TopNavTabs :active-tab="store.state.activeTab" :tabs="tabs" @select="store.setActiveTab" />
        </div>
      </div>

      <section class="min-h-0 flex-1 overflow-y-auto pb-4">
        <div class="grid gap-6 xl:grid-cols-[minmax(0,1.28fr)_320px]">
          <div class="min-w-0">
            <CoreTab
              v-if="store.state.activeTab === 'core'"
              :config="store.state.config"
              :path="store.state.path"
            :log-path="logSnapshot?.activePath"
            :theme-preference="themePreference"
            @view-logs="openLogs"
            @add-application="store.addApplication"
            @remove-application="store.removeApplication"
            @update:theme-preference="themePreference = $event"
          />
            <DepthXrTab
            v-else-if="store.state.activeTab === 'depthxr'"
            :config="store.state.config"
            :applications="store.state.config.applications"
            @add-profile="store.addProfile"
            @remove-profile="store.removeProfile"
            @sync-profile-name="store.syncProfileName"
            />
            <PivotXrTab v-else :config="store.state.config" />
          </div>

          <aside class="space-y-4">
            <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel">
              <p class="eyebrow text-xs uppercase tracking-[0.24em]">Validation</p>
              <h2 class="mt-2 text-xl font-semibold tracking-tight">Config health</h2>

              <ul v-if="errors.length > 0" class="mt-3 space-y-2.5">
                <li v-for="error in errors" :key="error" class="chip-warning rounded-[0.9rem] border px-4 py-3 text-sm" style="border-color: var(--app-border)">
                  {{ error }}
                </li>
              </ul>

              <div v-else class="chip-success mt-3 rounded-[0.9rem] border px-4 py-4 text-sm" style="border-color: var(--app-border)">
                VectorXR config structure and value ranges look valid.
              </div>
            </article>

            <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel">
              <p class="eyebrow text-xs uppercase tracking-[0.24em]">Working Copy</p>
              <h2 class="mt-2 text-xl font-semibold tracking-tight">Save status</h2>
              <div class="mt-3 rounded-[0.9rem] border px-4 py-4 text-sm leading-6 surface-panel-soft">
                <p><strong>State:</strong> {{ dirty ? 'Unsaved changes' : 'Synced with disk' }}</p>
                <p class="mt-2"><strong>Path:</strong></p>
                <p class="mt-1 break-all font-mono text-xs md:text-sm">{{ store.state.path || 'Resolving...' }}</p>
              </div>
            </article>

            <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel-inverse">
              <p class="text-xs uppercase tracking-[0.24em] text-inverse-muted">{{ activeSummary.panelTitle }}</p>
              <p class="mt-3 text-sm leading-6 text-inverse-muted">{{ activeSummary.panelBody }}</p>

              <div class="mt-4 rounded-[0.9rem] border border-white/10 bg-white/5 px-4 py-4 text-sm leading-6 text-inverse-muted">
                <p><strong class="text-white">Theme:</strong> {{ themePreference }}</p>
                <p class="mt-2"><strong class="text-white">Latest patch:</strong> {{ latestPatch.version }}</p>
              </div>
            </article>
          </aside>
        </div>
      </section>

      <StickySaveBar
        class="shrink-0 pt-4"
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
      <PatchNotesModal :open="patchNotesOpen" :entries="patchNotes" @close="patchNotesOpen = false" />
    </section>
  </main>
</template>
