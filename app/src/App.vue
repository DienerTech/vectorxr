<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppRegistryEditor from './components/AppRegistryEditor.vue'
import ImportPreviewModal from './components/ImportPreviewModal.vue'
import LogViewerModal from './components/LogViewerModal.vue'
import PatchNotesModal from './components/PatchNotesModal.vue'
import StickySaveBar from './components/StickySaveBar.vue'
import TopNavTabs from './components/TopNavTabs.vue'
import AboutTab from './components/tabs/AboutTab.vue'
import CoreTab from './components/tabs/CoreTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import OpenXrLayersTab from './components/tabs/OpenXrLayersTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import { exportConfigFile, loadLogSnapshot, type LogSnapshot } from './lib/commands'
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

const tabs = computed(() => [
  {
    id: 'core' as const,
    label: 'Home',
    subtitle: 'App settings and logs',
    status: store.state.config.core.enabled ? 'Suite on' : 'Suite off',
  },
  {
    id: 'registry' as const,
    label: 'Application Registry',
    subtitle: 'Register applications for profile assignment',
    status: `${store.state.config.applications.length} app${store.state.config.applications.length === 1 ? '' : 's'}`,
  },
  {
    id: 'layers' as const,
    label: 'OpenXR Layers',
    subtitle: 'Inspect and manage implicit layer order',
    status: 'System',
  },
  {
    id: 'about' as const,
    label: 'About',
    subtitle: 'Project information and patch notes',
    status: latestPatch.version,
  },
  {
    id: 'depthxr' as const,
    label: 'Depth',
    subtitle: 'Stereo depth tuning - see the world in a new way!',
    status: store.state.config.modules.depthxr.enabled ? 'Enabled' : 'Disabled',
  },
  {
    id: 'pivotxr' as const,
    label: 'Pivot',
    subtitle: 'Rotation tuning - watch your six!',
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

async function exportConfig() {
  if (errors.value.length > 0) {
    store.state.status = 'Fix validation errors before exporting'
    return
  }

  try {
    const exported = await exportConfigFile(store.state.config)
    store.state.status = exported ? 'Config exported' : 'Export canceled'
  } catch (error) {
    store.state.status = error instanceof Error ? error.message : 'Failed to export config'
  }
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

async function applyImport() {
  if (importedConfig.value) {
    const shouldSave = importErrors.value.length === 0
    store.importConfig(importedConfig.value)
    if (shouldSave) {
      await store.save()
    }
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

async function confirmResetConfig() {
  const confirmed = window.confirm(
    'Reset VectorXR to defaults?\n\nThis will erase all saved settings, application registry entries, profiles, and OpenXR application discovery data. This cannot be undone.',
  )

  if (!confirmed) {
    return
  }

  await store.resetToDefaults()
}
</script>

<template>
  <main class="app-shell-bg h-screen overflow-hidden px-4 py-4 md:px-6 xl:px-8">
    <section class="mx-auto flex h-full max-w-[1500px] flex-col">
      <div class="shrink-0 pb-4 pt-1">
        <TopNavTabs :active-tab="store.state.activeTab" :tabs="tabs" @select="store.setActiveTab" />
      </div>

      <section class="min-h-0 flex-1 overflow-y-auto pb-4">
        <CoreTab
          v-if="store.state.activeTab === 'core'"
          :config="store.state.config"
          :path="store.state.path"
          :log-path="logSnapshot?.activePath"
          :theme-preference="themePreference"
          :settings-actions-disabled="store.state.loading || store.state.saving"
          @view-logs="openLogs"
          @import-config="triggerImport"
          @export-config="exportConfig"
          @reset-config="confirmResetConfig"
          @update:theme-preference="themePreference = $event"
        />
        <AppRegistryEditor
          v-else-if="store.state.activeTab === 'registry'"
          :config="store.state.config"
          :applications="store.state.config.applications"
          :seen-apps="store.state.seenApps"
          :seen-apps-loading="store.state.seenAppsLoading"
          @add="store.addApplication"
          @add-seen="store.addSeenApplication"
          @clear-seen="store.clearSeenApplications"
          @refresh-seen="store.refreshSeenApps"
          @remove="store.removeApplication"
        />
        <AboutTab v-else-if="store.state.activeTab === 'about'" :latest-patch="latestPatch" @open-patch-notes="patchNotesOpen = true" />
        <OpenXrLayersTab v-else-if="store.state.activeTab === 'layers'" />
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
      />

      <input ref="importFileInput" type="file" accept=".json" class="hidden" @change="handleImportFile" />
      <ImportPreviewModal :open="importPreviewOpen" :config="importedConfig" :errors="importErrors" @apply="applyImport" @cancel="cancelImport" />
      <LogViewerModal :open="logViewerOpen" :loading="logViewerLoading" :snapshot="logSnapshot" @close="logViewerOpen = false" />
      <PatchNotesModal :open="patchNotesOpen" :entries="patchNotes" @close="patchNotesOpen = false" />
    </section>
  </main>
</template>
