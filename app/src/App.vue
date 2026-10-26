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
import OsdTab from './components/tabs/OsdTab.vue'
import DepthXrTab from './components/tabs/DepthXrTab.vue'
import FrameGenTab from './components/tabs/FrameGenTab.vue'
import HomeTab from './components/tabs/HomeTab.vue'
import OpenXrLayersTab from './components/tabs/OpenXrLayersTab.vue'
import PivotXrTab from './components/tabs/PivotXrTab.vue'
import QuadViewsTab from './components/tabs/QuadViewsTab.vue'
import TurboTab from './components/tabs/TurboTab.vue'
import { exportConfigFile, loadDebugSources, loadLogSnapshot, loadOpenXrLayers, loadRuntimeStatus, loadSeenApps, loadRuntimePacing, loadTurboMetrics, type LogSnapshot, type OpenXrLayerSnapshot } from './lib/commands'
import { createDebugPackage, saveDebugPackage } from './lib/debugPackage'
import { errorText } from './lib/errors'
import { buildHealthSummary, type HealthSummary } from './lib/health'
import { t, withLocale } from './i18n/index.ts'
import { applyLanguagePreference } from './i18n/load.ts'
import { loadLanguagePreference, storeLanguagePreference, type LanguagePreference } from './i18n/preference.ts'
import { sourceLocale } from './i18n/locales.ts'
import { moduleStateForApplication, normalizeConfig, type ModuleId, type VectorXRConfig } from './lib/model'
import { patchNotes } from './lib/patchNotes'
import { applyThemePreference, loadThemePreference, observeSystemThemeChanges, type ThemePreference } from './lib/theme'
import { configureTray, loadTrayEnabled, onTrayAction, storeTrayEnabled, trayStatus, updateTrayStatus, type TrayAction } from './lib/tray'
import { validateConfig } from './lib/validation'
import { useConfigStore } from './stores/configStore'
import { useUpdateStore } from './stores/updateStore'

const store = useConfigStore()
const updateStore = useUpdateStore()
const errors = computed(() => validateConfig(store.state.config))
const turboInUse = computed(
  () =>
    store.state.config.modules.turbo.enabled ||
    store.state.config.modules.turbo.profiles.some((profile) => profile.enabled),
)
const quadviewsInUse = computed(
  () =>
    store.state.config.modules.quadviews.enabled ||
    store.state.config.modules.quadviews.profiles.some((profile) => profile.enabled),
)
const dirty = computed(() => store.isDirty.value)
const logViewerOpen = ref(false)
const healthCheckOpen = ref(false)
const debugExportOpen = ref(false)
const debugExporting = ref(false)
const patchNotesOpen = ref(false)
const logViewerLoading = ref(false)
const logSnapshot = ref<LogSnapshot | null>(null)
const themePreference = ref<ThemePreference>(loadThemePreference())
// main.ts applied it (or the installer's language) before the first render.
const languagePreference = ref<LanguagePreference>(loadLanguagePreference() ?? 'system')
const trayEnabled = ref<boolean>(loadTrayEnabled())
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
    ? t('app.updates.available', { version: updateStore.state.latestRelease.version })
    : '',
)
function buildEditorHealthSummary(): HealthSummary {
  return buildHealthSummary({
    config: store.state.config,
    configPath: store.state.path,
    logSnapshot: logSnapshot.value,
    openXrLayerSnapshot: openXrLayerSnapshot.value,
    openXrLayersLoading: openXrLayersLoading.value,
    seenApps: store.state.seenApps,
  })
}
const healthSummary = computed(buildEditorHealthSummary)

// The debug package stays English whatever language the app shows, so the
// person reading a tester's report can follow it. t() is synchronous, so
// switching the locale around the build is never visible on screen.
function englishHealthSummary(): HealthSummary {
  return withLocale(sourceLocale, buildEditorHealthSummary)
}

// The tray shows what is saved, which is what the layer uses.
const trayHealthSummary = computed(() => buildHealthSummary({
  config: store.state.originalConfig,
  configPath: store.state.path,
  logSnapshot: logSnapshot.value,
  openXrLayerSnapshot: openXrLayerSnapshot.value,
  openXrLayersLoading: openXrLayersLoading.value,
  seenApps: store.state.seenApps,
}))
const currentTrayStatus = computed(() => trayStatus(
  store.state.originalConfig,
  trayHealthSummary.value,
  !store.state.loading && !store.state.loadError && !!store.state.path,
))

function enabledProfileCount(moduleId: ModuleId) {
  return store.state.config.modules[moduleId].profiles.filter((profile) => profile.enabled).length
}

function enhancementActive(moduleId: ModuleId) {
  return store.state.config.modules[moduleId].enabled || enabledProfileCount(moduleId) > 0
}

function enhancementStatus(moduleId: ModuleId) {
  return enhancementActive(moduleId) ? t('common.active') : t('common.inactive')
}

const tabs = computed(() => [
  {
    id: 'home' as const,
    label: t('app.tabs.home.label'),
    subtitle: t('app.tabs.home.subtitle'),
    status: latestPatch.version,
  },
  {
    id: 'core' as const,
    label: t('app.tabs.core.label'),
    subtitle: t('app.tabs.core.subtitle'),
    status: store.state.config.core.enabled ? t('app.tabs.core.suiteOn') : t('app.tabs.core.suiteOff'),
    statusWarning: !store.state.config.core.enabled,
  },
  {
    id: 'osd' as const,
    label: t('app.tabs.osd.label'),
    subtitle: t('app.tabs.osd.subtitle'),
    status: store.state.config.core.osd.enabled ? t('common.enabled') : t('common.disabled'),
    enhancementActive: store.state.config.core.osd.enabled,
  },
  {
    id: 'registry' as const,
    label: t('app.tabs.registry.label'),
    subtitle: t('app.tabs.registry.subtitle'),
    status: t('app.tabs.registry.status', { count: store.state.config.applications.length }),
  },
  {
    id: 'layers' as const,
    label: t('app.tabs.layers.label'),
    subtitle: t('app.tabs.layers.subtitle'),
    status: t('app.tabs.layers.status'),
  },
  {
    id: 'about' as const,
    label: t('app.tabs.about.label'),
    subtitle: t('app.tabs.about.subtitle'),
    status: latestPatch.version,
  },
  {
    id: 'quadviews' as const,
    label: t('app.tabs.quadviews.label'),
    subtitle: t('app.tabs.quadviews.subtitle'),
    status: enhancementStatus('quadviews'),
    enhancementActive: enhancementActive('quadviews'),
  },
  {
    id: 'turbo' as const,
    label: t('app.tabs.turbo.label'),
    subtitle: t('app.tabs.turbo.subtitle'),
    status: enhancementStatus('turbo'),
    enhancementActive: enhancementActive('turbo'),
  },
  {
    id: 'framegen' as const,
    label: t('app.tabs.framegen.label'),
    subtitle: t('app.tabs.framegen.subtitle'),
    status: enhancementStatus('framegen'),
    enhancementActive: enhancementActive('framegen'),
  },
  {
    id: 'pivotxr' as const,
    label: t('app.tabs.pivotxr.label'),
    subtitle: t('app.tabs.pivotxr.subtitle'),
    status: enhancementStatus('pivotxr'),
    enhancementActive: enhancementActive('pivotxr'),
  },
  {
    id: 'depthxr' as const,
    label: t('app.tabs.depthxr.label'),
    subtitle: t('app.tabs.depthxr.subtitle'),
    status: enhancementStatus('depthxr'),
    enhancementActive: enhancementActive('depthxr'),
  },
])

// A new language shows at once. The OSD follows the app's unless it names
// its own: the saved settings carry the app's language (saveConfigEnvelope),
// so save them again for the layer to pick it up.
watch(languagePreference, async (preference) => {
  storeLanguagePreference(preference)
  await applyLanguagePreference(preference)
  if (store.state.originalConfig.core.osd.language === 'app' && !store.state.loading && !store.state.loadError && store.state.path) {
    await store.saveSetting(() => {}, t('validation.store.configSaved'))
  }
})

watch(
  themePreference,
  (preference) => {
    applyThemePreference(preference)
  },
  { immediate: true },
)

// The layer writes pacing verdicts during play; re-read them whenever the
// user lands on the Turbo tab so discoveries from the last session show up.
// The layer writes pacing verdicts mid-session (e.g. while the user watches
// the Turbo tab with the game running); poll gently while the tab is visible
// and refresh on window focus so the runtimes table updates without needing
// to navigate away and back.
let runtimePacingRefreshTimer: number | undefined

function stopRuntimePacingRefresh() {
  if (runtimePacingRefreshTimer !== undefined) {
    window.clearInterval(runtimePacingRefreshTimer)
    runtimePacingRefreshTimer = undefined
  }
}

function onWindowFocus() {
  void store.reloadIfChanged()
  if (store.state.activeTab === 'turbo') {
    void store.refreshRuntimePacing()
    void store.refreshTurboMetrics()
  }
}

watch(
  () => store.state.activeTab,
  (tab) => {
    stopRuntimePacingRefresh()
    if (tab === 'turbo') {
      void store.refreshRuntimePacing()
      void store.refreshTurboMetrics()
      runtimePacingRefreshTimer = window.setInterval(() => {
        void store.refreshRuntimePacing()
        void store.refreshTurboMetrics()
      }, 5000)
    }
  },
)

let stopSystemThemeObservation = () => {}
let stopTrayActions = () => {}

onMounted(() => {
  stopSystemThemeObservation = observeSystemThemeChanges(() => {
    if (themePreference.value === 'system') {
      applyThemePreference(themePreference.value)
    }
  })

  window.addEventListener('focus', onWindowFocus)
  void onTrayAction(handleTrayAction).then((unlisten) => { stopTrayActions = unlisten })

  void store.load()
  void refreshLogs()
  void refreshOpenXrLayers()
  // Best-effort and fully non-blocking: detached from startup, failures are swallowed
  // inside the store, so the indicator simply stays hidden and nothing else is affected.
  void updateStore.check(latestPatch.version)
})

onUnmounted(() => {
  stopSystemThemeObservation()
  stopRuntimePacingRefresh()
  stopTrayActions()
  window.removeEventListener('focus', onWindowFocus)
})

watch(
  trayEnabled,
  (enabled) => {
    storeTrayEnabled(enabled)
    configureTray(enabled)
      .then(() => updateTrayStatus(currentTrayStatus.value))
      .catch((error) => { store.state.status = errorText(error, t('app.status.trayFailed')) })
  },
  { immediate: true },
)

watch(currentTrayStatus, (status) => { void updateTrayStatus(status).catch(() => {}) }, { deep: true })

// The health can go stale while the window is hidden in the tray; re-read it
// when the pointer reaches the icon, at most every 10 s.
let trayRefreshedAt = 0
function refreshForTray() {
  const now = Date.now()
  if (now - trayRefreshedAt < 10_000) return
  trayRefreshedAt = now
  void refreshLogs()
  void refreshOpenXrLayers()
}

async function handleTrayAction(action: TrayAction) {
  switch (action.action) {
    case 'setEnabled':
      await store.saveSetting(
        (config) => { config.core.enabled = action.enabled },
        action.enabled ? t('app.status.enabledFromTray') : t('app.status.disabledFromTray'),
      )
      break
    case 'setLogLevel':
      await store.saveSetting(
        (config) => { config.core.logLevel = action.level },
        action.level === 'debug' ? t('app.status.logLevelDebugFromTray') : t('app.status.logLevelInfoFromTray'),
      )
      break
    case 'exportDebug':
      debugExportOpen.value = true
      break
    case 'showHealth':
      healthCheckOpen.value = true
      break
    case 'refresh':
      refreshForTray()
      break
  }
  // A refused or failed change leaves the tray as it was.
  void updateTrayStatus(currentTrayStatus.value).catch(() => {})
}

async function saveConfig() {
  if (errors.value.length > 0) {
    store.state.status = t('app.status.fixBeforeSaving')
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
    store.state.status = errorText(error, t('app.status.loadLogsFailed'))
  } finally {
    logViewerLoading.value = false
  }
}

let layerRefreshRevision = 0
async function refreshOpenXrLayers(includeSignatures = false) {
  const revision = ++layerRefreshRevision
  openXrLayersLoading.value = true

  try {
    if (!includeSignatures) {
      const initial = await loadOpenXrLayers(false)
      if (revision !== layerRefreshRevision) return
      openXrLayerSnapshot.value = initial
    }
    const complete = await loadOpenXrLayers(true)
    if (revision === layerRefreshRevision) openXrLayerSnapshot.value = complete
  } catch (error) {
    if (revision === layerRefreshRevision) store.state.status = errorText(error, t('app.status.loadLayersFailed'))
  } finally {
    if (revision === layerRefreshRevision) openXrLayersLoading.value = false
  }
}

async function openLogs() {
  logViewerOpen.value = true
  await refreshLogs()
}

async function exportConfig() {
  if (errors.value.length > 0) {
    store.state.status = t('app.status.fixBeforeExporting')
    return
  }

  try {
    const exported = await exportConfigFile(store.state.config)
    store.state.status = exported ? t('app.status.configExported') : t('app.status.exportCanceled')
  } catch (error) {
    store.state.status = errorText(error, t('app.status.exportFailed'))
  }
}

async function exportDebugInformation() {
  debugExporting.value = true

  try {
    // Collection errors go into the debug package, which stays English.
    const collectionErrors: string[] = []
    async function capture<T>(label: string, request: Promise<T>, fallback: T): Promise<T> {
      try { return await request } catch (error) {
        collectionErrors.push(`${label}: ${String(error)}`)
        return fallback
      }
    }
    const [seenApps, pacing, metrics, runtimeStatus, layers, logs, debugSources] = await Promise.all([
      capture('Seen apps', loadSeenApps(), { path: '', observations: [] }), // i18n-ignore
      capture('Runtime pacing', loadRuntimePacing(), { path: '', observations: [], activeRuntime: null }), // i18n-ignore
      capture('Turbo metrics', loadTurboMetrics(), { path: '', sessions: [] }), // i18n-ignore
      loadRuntimeStatus().catch((error) => {
        const message = `Runtime status: ${String(error)}` // i18n-ignore
        collectionErrors.push(message)
        return { error: message }
      }),
      capture('OpenXR layers', loadOpenXrLayers(true), null), // i18n-ignore
      capture('Log preview', loadLogSnapshot(), null), // i18n-ignore
      capture('Raw diagnostic files', loadDebugSources(), { files: [], warnings: [] }), // i18n-ignore
    ])
    openXrLayerSnapshot.value = layers
    logSnapshot.value = logs
    // The health summary reads the store's seen apps, which were loaded when
    // the app started; use the list just read so health-summary.json and the
    // README agree with seen-apps.json in the same bundle.
    if (seenApps.path) {
      store.state.seenAppsPath = seenApps.path
      store.state.seenApps = seenApps.observations
    }
    const packageBlob = await createDebugPackage({
      runtimeStatus,
      debugSources,
      collectionErrors,
      unsavedChanges: dirty.value,
      appVersion: latestPatch.version,
      configPath: store.state.path,
      seenAppsPath: seenApps.path,
      config: store.state.config,
      seenApps: seenApps.observations,
      runtimePacingPath: pacing.path,
      runtimePacing: pacing.observations,
      turboMetricsPath: metrics.path,
      turboMetrics: metrics.sessions,
      activeRuntime: pacing.activeRuntime,
      logSnapshot: logSnapshot.value,
      openXrLayerSnapshot: openXrLayerSnapshot.value,
      healthSummary: englishHealthSummary(),
    })
    const exported = await saveDebugPackage(packageBlob)
    store.state.status = exported ? t('app.status.debugExported') : t('app.status.debugExportCanceled')
    if (exported) {
      debugExportOpen.value = false
    }
  } catch (error) {
    store.state.status = errorText(error, t('app.status.debugExportFailed'))
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
      store.state.status = t('app.status.importInvalidJson')
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
  const confirmed = window.confirm(t('app.resetConfirm'))

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
          :language-preference="languagePreference"
          :tray-enabled="trayEnabled"
          :settings-actions-disabled="store.state.loading || store.state.saving"
          @view-logs="openLogs"
          @export-debug="debugExportOpen = true"
          @import-config="triggerImport"
          @export-config="exportConfig"
          @reset-config="confirmResetConfig"
          @update:theme-preference="themePreference = $event"
          @update:language-preference="languagePreference = $event"
          @update:tray-enabled="trayEnabled = $event"
        />
        <OsdTab v-else-if="store.state.activeTab === 'osd'" :config="store.state.config" :saved-config="store.state.originalConfig" />
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
          :turbo-in-use="turboInUse"
          :quadviews-in-use="quadviewsInUse"
          @refresh="refreshOpenXrLayers(true)"
          @machine-writes-unlocked="openXrMachineWritesUnlocked = $event"
          @snapshot-updated="openXrLayerSnapshot = $event; refreshOpenXrLayers(true)"
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
        <TurboTab
          v-else-if="store.state.activeTab === 'turbo'"
          :config="store.state.config"
          :saved-config="store.state.originalConfig"
          :applications="store.state.config.applications"
          :runtime-pacing="store.state.runtimePacing"
          :active-runtime="store.state.activeRuntime"
          :layer-snapshot="openXrLayerSnapshot"
          :turbo-metrics="store.state.turboMetrics"
          @add-turbo-profile="store.addTurboProfile"
          @remove-turbo-profile="store.removeTurboProfile"
          @sync-turbo-profile-name="store.syncTurboProfileName"
          @rediscover-runtime="store.rediscoverRuntimePacing"
          @clear-turbo-metrics="store.clearTurboMetricsSessions"
        />
        <PivotXrTab
          v-else-if="store.state.activeTab === 'pivotxr'"
          :config="store.state.config"
          :applications="store.state.config.applications"
          @add-pivot-profile="store.addPivotProfile"
          @remove-pivot-profile="store.removePivotProfile"
          @move-pivot-profile="store.movePivotProfile"
          @sync-pivot-profile-name="store.syncPivotProfileName"
        />
        <FrameGenTab
          v-else-if="store.state.activeTab === 'framegen'"
          :config="store.state.config"
          :applications="store.state.config.applications"
          @add-frame-gen-profile="store.addFrameGenProfile"
          @remove-frame-gen-profile="store.removeFrameGenProfile"
          @sync-frame-gen-profile-name="store.syncFrameGenProfileName"
          @navigate="store.setActiveTab"
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
        :disabled="store.state.loading || store.state.saving || !!store.state.loadError || errors.length > 0"
        :errors="errors"
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
