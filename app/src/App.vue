<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'

import AppRegistryEditor from './components/AppRegistryEditor.vue'
import DebugExportModal from './components/DebugExportModal.vue'
import HealthCheckModal from './components/HealthCheckModal.vue'
import ImportPreviewModal from './components/ImportPreviewModal.vue'
import LogViewerModal from './components/LogViewerModal.vue'
import PatchNotesModal from './components/PatchNotesModal.vue'
import SidebarNav from './components/SidebarNav.vue'
import StickySaveBar from './components/StickySaveBar.vue'
import AboutTab from './components/tabs/AboutTab.vue'
import CoreTab from './components/tabs/CoreTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import HomeTab from './components/tabs/HomeTab.vue'
import OpenXrLayersTab from './components/tabs/OpenXrLayersTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import QuadViewsTab from './components/tabs/QuadViewsTab.vue'
import { exportConfigFile, loadLogSnapshot, loadOpenXrLayers, type LogSnapshot, type OpenXrLayerSnapshot } from './lib/commands'
import { createDebugPackage, saveDebugPackage } from './lib/debugPackage'
import { buildHealthSummary } from './lib/health'
import { moduleStateForApplication, normalizeConfig, type ModuleId, type VectorXRConfig } from './lib/model'
import { patchNotes } from './lib/patchNotes'
import { applyThemePreference, loadThemePreference, observeSystemThemeChanges, type ThemePreference } from './lib/theme'
import { validateConfig } from './lib/validation'
import { useConfigStore } from './stores/configStore'
import { useUpdateStore } from './stores/updateStore'

const store = useConfigStore()
const updateStore = useUpdateStore()
const errors = computed(() => validateConfig(store.state.config))
const dirty = computed(() => store.isDirty.value)
const logViewerOpen = ref(false)
const healthCheckOpen = ref(false)
const debugExportOpen = ref(false)
const debugExporting = ref(false)
const patchNotesOpen = ref(false)
const logViewerLoading = ref(false)
const logSnapshot = ref<LogSnapshot | null>(null)
const themePreference = ref<ThemePreference>(loadThemePreference())
const importFileInput = ref<HTMLInputElement | null>(null)
const importPreviewOpen = ref(false)
const importedConfig = ref<VectorXRConfig | null>(null)
const importErrors = ref<string[]>([])
const openXrLayerSnapshot = ref<OpenXrLayerSnapshot | null>(null)
const openXrLayersLoading = ref(false)
const openXrMachineWritesUnlocked = ref(false)

const latestPatch = patchNotes[0]

const updateAvailable = updateStore.updateAvailable
const updateTooltip = computed(() =>
  updateStore.state.latestRelease
    ? `Version ${updateStore.state.latestRelease.version} is available. Click for details.`
    : '',
)
const healthSummary = computed(() => buildHealthSummary({
  config: store.state.config,
  configPath: store.state.path,
  logSnapshot: logSnapshot.value,
  openXrLayerSnapshot: openXrLayerSnapshot.value,
  openXrLayersLoading: openXrLayersLoading.value,
  seenApps: store.state.seenApps,
}))

function enabledProfileCount(moduleId: ModuleId) {
  return store.state.config.modules[moduleId].profiles.filter((profile) => profile.enabled).length
}

function enhancementActive(moduleId: ModuleId) {
  return store.state.config.modules[moduleId].enabled || enabledProfileCount(moduleId) > 0
}

const tabs = computed(() => [
  {
    id: 'home' as const,
    label: 'Home',
    subtitle: 'Status, updates, and support',
    status: latestPatch.version,
  },
  {
    id: 'core' as const,
    label: 'Settings',
    subtitle: 'Runtime, logging, theme, and config',
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
    subtitle: 'Project info, patch notes, and support',
    status: latestPatch.version,
  },
  {
    id: 'quadviews' as const,
    label: 'Quadviews',
    subtitle: 'Dynamic foveated rendering control',
    status: enhancementActive('quadviews') ? 'Active' : 'Inactive',
    enhancementActive: enhancementActive('quadviews'),
  },
  {
    id: 'pivotxr' as const,
    label: 'Pivot',
    subtitle: 'Rotation tuning - watch your six!',
    status: enhancementActive('pivotxr') ? 'Active' : 'Inactive',
    enhancementActive: enhancementActive('pivotxr'),
  },
  {
    id: 'depthxr' as const,
    label: 'Depth',
    subtitle: 'Stereo depth tuning - see the world in a new way!',
    status: enhancementActive('depthxr') ? 'Active' : 'Inactive',
    enhancementActive: enhancementActive('depthxr'),
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
  void refreshOpenXrLayers()
  // Best-effort and fully non-blocking: detached from startup, failures are swallowed
  // inside the store, so the indicator simply stays hidden and nothing else is affected.
  void updateStore.check(latestPatch.version)
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

function showUpdateDetails() {
  store.setActiveTab('about')
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

async function refreshOpenXrLayers() {
  openXrLayersLoading.value = true

  try {
    openXrLayerSnapshot.value = await loadOpenXrLayers()
  } catch (error) {
    store.state.status = error instanceof Error ? error.message : 'Failed to load OpenXR layer status'
  } finally {
    openXrLayersLoading.value = false
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

async function exportDebugInformation() {
  debugExporting.value = true

  try {
    await Promise.all([refreshLogs(), refreshOpenXrLayers(), store.refreshSeenApps()])
    const packageBlob = createDebugPackage({
      appVersion: latestPatch.version,
      configPath: store.state.path,
      seenAppsPath: store.state.seenAppsPath,
      config: store.state.config,
      seenApps: store.state.seenApps,
      logSnapshot: logSnapshot.value,
      openXrLayerSnapshot: openXrLayerSnapshot.value,
      healthSummary: healthSummary.value,
    })
    const exported = await saveDebugPackage(packageBlob)
    store.state.status = exported ? 'Debug information exported' : 'Debug export canceled'
    if (exported) {
      debugExportOpen.value = false
    }
  } catch (error) {
    store.state.status = error instanceof Error ? error.message : 'Failed to export debug information'
  } finally {
    debugExporting.value = false
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

// Registry module chips: jump to an existing profile's module tab, or create a
// profile for the app first when only the defaults currently apply.
function handleOpenModule(moduleId: ModuleId, applicationId: string) {
  const stateForApp = moduleStateForApplication(store.state.config, moduleId, applicationId)
  if (stateForApp.kind === 'default' || stateForApp.kind === 'default-off') {
    store.addModuleProfileForApplication(moduleId, applicationId)
  } else {
    store.setActiveTab(moduleId)
  }
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
  <main class="app-shell-bg h-screen overflow-hidden">
    <section class="mx-auto flex h-full max-w-[1700px]">
      <SidebarNav
        :active-tab="store.state.activeTab"
        :tabs="tabs"
        :version="latestPatch.version"
        :update-available="updateAvailable"
        :update-tooltip="updateTooltip"
        @select="store.setActiveTab"
        @show-updates="showUpdateDetails"
      />

      <div class="flex min-w-0 flex-1 flex-col px-4 py-3 md:px-6">
      <section class="min-h-0 flex-1 overflow-y-auto pb-1">
        <HomeTab
          v-if="store.state.activeTab === 'home'"
          :config="store.state.config"
          :open-xr-layer-snapshot="openXrLayerSnapshot"
          :open-xr-layers-loading="openXrLayersLoading"
          @view-health="healthCheckOpen = true"
          @export-debug="debugExportOpen = true"
          @navigate="store.setActiveTab"
        />
        <CoreTab
          v-else-if="store.state.activeTab === 'core'"
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
          @open-module="handleOpenModule"
        />
        <OpenXrLayersTab
          v-else-if="store.state.activeTab === 'layers'"
          :snapshot="openXrLayerSnapshot"
          :loading="openXrLayersLoading"
          :machine-writes-unlocked="openXrMachineWritesUnlocked"
          @refresh="refreshOpenXrLayers"
          @machine-writes-unlocked="openXrMachineWritesUnlocked = $event"
          @snapshot-updated="openXrLayerSnapshot = $event"
          @status="store.state.status = $event"
        />
        <AboutTab
          v-else-if="store.state.activeTab === 'about'"
          :latest-patch="latestPatch"
          @open-patch-notes="patchNotesOpen = true"
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
          v-else-if="store.state.activeTab === 'pivotxr'"
          :config="store.state.config"
          :applications="store.state.config.applications"
          @add-pivot-profile="store.addPivotProfile"
          @remove-pivot-profile="store.removePivotProfile"
          @sync-pivot-profile-name="store.syncPivotProfileName"
        />
        <QuadViewsTab
          v-else
          :config="store.state.config"
          :applications="store.state.config.applications"
          @add-quad-views-profile="store.addQuadViewsProfile"
          @remove-quad-views-profile="store.removeQuadViewsProfile"
          @sync-quad-views-profile-name="store.syncQuadViewsProfileName"
        />
      </section>

      <StickySaveBar
        class="shrink-0 pt-3"
        :dirty="dirty"
        :saving="store.state.saving"
        :loading="store.state.loading"
        :status="store.state.status"
        :disabled="store.state.loading || store.state.saving || errors.length > 0"
        @save="saveConfig"
        @discard="store.discardChanges"
      />
      </div>

      <input ref="importFileInput" type="file" accept=".json" class="hidden" @change="handleImportFile" />
      <HealthCheckModal :open="healthCheckOpen" :summary="healthSummary" @close="healthCheckOpen = false" @export-debug="debugExportOpen = true" />
      <DebugExportModal :open="debugExportOpen" :exporting="debugExporting" @cancel="debugExportOpen = false" @confirm="exportDebugInformation" />
      <ImportPreviewModal :open="importPreviewOpen" :config="importedConfig" :errors="importErrors" @apply="applyImport" @cancel="cancelImport" />
      <LogViewerModal :open="logViewerOpen" :loading="logViewerLoading" :snapshot="logSnapshot" @close="logViewerOpen = false" />
      <PatchNotesModal :open="patchNotesOpen" :entries="patchNotes" @close="patchNotesOpen = false" />
    </section>
  </main>
</template>
