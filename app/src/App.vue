<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppHeader from './components/AppHeader.vue'
import ImportPreviewModal from './components/ImportPreviewModal.vue'
import LogViewerModal from './components/LogViewerModal.vue'
import PatchNotesModal from './components/PatchNotesModal.vue'
import StickySaveBar from './components/StickySaveBar.vue'
import TopNavTabs from './components/TopNavTabs.vue'
import CoreTab from './components/tabs/CoreTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import { loadLogSnapshot, type LogSnapshot } from './lib/commands'
import { normalizeConfig, type VectorXRConfig } from './lib/model'
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
const importFileInput = ref<HTMLInputElement | null>(null)
const importPreviewOpen = ref(false)
const importedConfig = ref<VectorXRConfig | null>(null)
const importErrors = ref<string[]>([])

const latestPatch = patchNotes[0]

const activeSummary = computed(() => {
  if (store.state.activeTab === 'core') {
    return {
      eyebrow: 'Home',
          title: 'VectorXR Utility Suite',
    body: 'Modern, open-source configuration utility for VectorXR and its modules.',
      panelTitle: 'Home focus',
      panelBody: 'Suite-wide controls, log access, and the product summary live here.',
    }
  }

  if (store.state.activeTab === 'depthxr') {
    return {
      eyebrow: 'Depth',
    title: 'VectorXR Utility Suite',
    body: 'Modern, open-source configuration utility for VectorXR and its modules.',
      panelTitle: 'Depth focus',
      panelBody: 'Defaults and profile overrides stay centered on depth tuning and quick comparison.',
    }
  }

  return {
    eyebrow: 'Pivot',
    title: 'VectorXR Utility Suite',
    body: 'Modern, open-source configuration utility for VectorXR and its modules.',
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


function triggerImport() {
  importFileInput.value?.click()
}

function handleImportFile(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) {
    return
  }

  const reader = new FileReader()
  reader.onload = (e) => {
    const content = e.target?.result as string
    try {
      const parsed: unknown = JSON.parse(content)
      const config = normalizeConfig(parsed)
      importedConfig.value = config
      importErrors.value = validateConfig(config)
      importPreviewOpen.value = true
    } catch {
      store.state.status = 'Import failed: file is not valid JSON'
    }
    input.value = ''
  }
  reader.readAsText(file)
}

function applyImport() {
  if (importedConfig.value) {
    store.importConfig(importedConfig.value)
  }
  importPreviewOpen.value = false
  importedConfig.value = null
  importErrors.value = []
}

function cancelImport() {
  importPreviewOpen.value = false
  importedConfig.value = null
  importErrors.value = []
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
        <PivotXrTab
          v-else
          :config="store.state.config"
          :applications="store.state.config.applications"
          @add-pivot-profile="store.addPivotProfile"
          @remove-pivot-profile="store.removePivotProfile"
          @sync-pivot-profile-name="store.syncPivotProfileName"
        />
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
        @import="triggerImport"
      />

      <input ref="importFileInput" type="file" accept=".json" class="hidden" @change="handleImportFile" />
      <ImportPreviewModal :open="importPreviewOpen" :config="importedConfig" :errors="importErrors" @apply="applyImport" @cancel="cancelImport" />
      <LogViewerModal :open="logViewerOpen" :loading="logViewerLoading" :snapshot="logSnapshot" @close="logViewerOpen = false" />
      <PatchNotesModal :open="patchNotesOpen" :entries="patchNotes" @close="patchNotesOpen = false" />
    </section>
  </main>
</template>
