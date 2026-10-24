import { computed, reactive } from 'vue'

import {
  clearRuntimePacingObservation,
  clearSeenApps,
  clearTurboMetrics,
  loadConfigEnvelope,
  loadRuntimePacing,
  loadSeenApps,
  loadTurboMetrics,
  resetStoredData,
  saveConfigEnvelope,
  type ActiveRuntimeInfo,
  type SeenApplication,
} from '../lib/commands'
import { errorText } from '../lib/errors'
import { t } from '../i18n/index.ts'
import { createTaskQueue } from '../lib/taskQueue'
import { cloneConfig, createApplication, createFrameGenProfile, createProfile, createPivotProfile, createQuadViewsProfile, createTurboProfile, defaultConfig, defaultProfileName, isDefaultProfileName } from '../lib/model'
import type { AppTab, ModuleId, RuntimePacingObservation, TurboMetricsSession, VectorXRConfig } from '../lib/model'

interface StoreState {
  loading: boolean
  saving: boolean
  // Why the saved config could not be read. While set, the editor holds
  // defaults rather than the user's settings, so Save stays off until a load,
  // reset or import succeeds; otherwise the first Save would overwrite the
  // file with defaults.
  loadError: string
  path: string
  seenAppsPath: string
  seenApps: SeenApplication[]
  seenAppsLoading: boolean
  runtimePacingPath: string
  runtimePacing: RuntimePacingObservation[]
  activeRuntime: ActiveRuntimeInfo | null
  runtimePacingLoading: boolean
  turboMetricsPath: string
  turboMetrics: TurboMetricsSession[]
  config: VectorXRConfig
  originalConfig: VectorXRConfig
  activeTab: AppTab
  status: string
}

const state = reactive<StoreState>({
  loading: false,
  saving: false,
  loadError: '',
  path: '',
  seenAppsPath: '',
  seenApps: [],
  seenAppsLoading: false,
  runtimePacingPath: '',
  runtimePacing: [],
  activeRuntime: null,
  runtimePacingLoading: false,
  turboMetricsPath: '',
  turboMetrics: [],
  config: defaultConfig(),
  originalConfig: defaultConfig(),
  activeTab: 'home',
  status: '',
})

const isDirty = computed(() => JSON.stringify(state.config) !== JSON.stringify(state.originalConfig))

// Saves run one at a time, so a tray change and a Save on the page cannot
// write the file out of order.
const queueSave = createTaskQueue()

// The settings file as last read back (load, after each save, reset), in the
// same normalized form a later read gives: reloadIfChanged compares with it,
// so only a change made elsewhere counts.
let lastDisk = ''
async function readBack() {
  try {
    lastDisk = JSON.stringify((await loadConfigEnvelope()).config)
  } catch {
    lastDisk = ''
  }
}

function loadBlockedStatus() {
  return t('validation.store.loadBlocked', { error: state.loadError.replace(/\.\s*$/, '') })
}

export function useConfigStore() {
  async function load() {
    state.loading = true
    state.status = ''

    try {
      const envelope = await loadConfigEnvelope()
      state.path = envelope.path
      state.config = cloneConfig(envelope.config)
      state.originalConfig = cloneConfig(envelope.config)
      lastDisk = JSON.stringify(envelope.config)
      state.loadError = ''
      state.status = ''
      await Promise.all([refreshSeenApps(), refreshRuntimePacing(), refreshTurboMetrics()])
    } catch (error) {
      state.loadError = errorText(error, t('validation.store.loadConfigFailed'))
      state.status = loadBlockedStatus()
    } finally {
      state.loading = false
    }
  }

  async function refreshSeenApps() {
    state.seenAppsLoading = true

    try {
      const envelope = await loadSeenApps()
      state.seenAppsPath = envelope.path
      state.seenApps = envelope.observations
    } catch (error) {
      state.status = errorText(error, t('validation.store.loadSeenAppsFailed'))
    } finally {
      state.seenAppsLoading = false
    }
  }

  async function refreshRuntimePacing() {
    state.runtimePacingLoading = true

    try {
      const envelope = await loadRuntimePacing()
      state.runtimePacingPath = envelope.path
      // Polled while the Turbo tab is visible — only touch reactive state on
      // real changes so the table doesn't rebuild every tick.
      if (JSON.stringify(state.runtimePacing) !== JSON.stringify(envelope.observations)) {
        state.runtimePacing = envelope.observations
      }
      if (JSON.stringify(state.activeRuntime) !== JSON.stringify(envelope.activeRuntime)) {
        state.activeRuntime = envelope.activeRuntime
      }
    } catch (error) {
      state.status = errorText(error, t('validation.store.loadRuntimePacingFailed'))
    } finally {
      state.runtimePacingLoading = false
    }
  }

  async function refreshTurboMetrics() {
    try {
      const envelope = await loadTurboMetrics()
      state.turboMetricsPath = envelope.path
      // Polled while the Turbo tab is visible — only touch reactive state on
      // real changes so the metrics card doesn't rebuild every tick.
      if (JSON.stringify(state.turboMetrics) !== JSON.stringify(envelope.sessions)) {
        state.turboMetrics = envelope.sessions
      }
    } catch (error) {
      state.status = errorText(error, t('validation.store.loadTurboMetricsFailed'))
    }
  }

  async function clearTurboMetricsSessions() {
    try {
      const envelope = await clearTurboMetrics()
      state.turboMetricsPath = envelope.path
      state.turboMetrics = envelope.sessions
      state.status = t('validation.store.turboMetricsCleared')
    } catch (error) {
      state.status = errorText(error, t('validation.store.clearTurboMetricsFailed'))
    }
  }

  async function rediscoverRuntimePacing(runtimeName: string) {
    try {
      const envelope = await clearRuntimePacingObservation(runtimeName)
      state.runtimePacingPath = envelope.path
      state.runtimePacing = envelope.observations
      state.activeRuntime = envelope.activeRuntime
      state.status = t('validation.store.pacingVerdictCleared', { runtime: runtimeName })
    } catch (error) {
      state.status = errorText(error, t('validation.store.clearPacingVerdictFailed'))
    }
  }

  function save() {
    return queueSave(async () => {
      if (state.loadError) {
        state.status = loadBlockedStatus()
        return
      }

      state.saving = true
      state.status = ''

      try {
        const saved = cloneConfig(state.config)
        state.path = await saveConfigEnvelope(saved)
        state.originalConfig = saved
        await readBack()
        state.status = t('validation.store.configSaved')
      } catch (error) {
        state.status = errorText(error, t('validation.store.saveConfigFailed'))
      } finally {
        state.saving = false
      }
    })
  }

  // A change made outside the editor (the tray): saved at once on top of the
  // settings on disk, then made in the editor too, so the editor's other
  // unsaved edits stay unsaved. The disk copy, not the one loaded at start:
  // an app kept running in the tray for hours must not undo a hand edit or
  // another copy's save.
  function saveSetting(change: (config: VectorXRConfig) => void, description: string): Promise<boolean> {
    return queueSave(async () => {
      if (state.loading) {
        return false
      }
      if (state.loadError) {
        state.status = loadBlockedStatus()
        return false
      }

      state.saving = true
      try {
        const saved = cloneConfig((await loadConfigEnvelope()).config)
        change(saved)
        state.path = await saveConfigEnvelope(saved)
        await readBack()
        const edited = isDirty.value
        state.originalConfig = saved
        if (edited) {
          change(state.config)
        } else {
          state.config = cloneConfig(saved)
        }
        state.status = description
        return true
      } catch (error) {
        state.status = errorText(error, t('validation.store.saveConfigFailed'))
        return false
      } finally {
        state.saving = false
      }
    })
  }

  // The file may have changed while the window was hidden (the tray keeps
  // the app running): pick that up when nothing is edited here.
  function reloadIfChanged() {
    return queueSave(async () => {
      if (state.loading || state.saving || state.loadError || isDirty.value) {
        return
      }
      try {
        const disk = (await loadConfigEnvelope()).config
        const read = JSON.stringify(disk)
        if (!lastDisk || read === lastDisk || isDirty.value) {
          lastDisk ||= read
          return
        }
        lastDisk = read
        state.config = cloneConfig(disk)
        state.originalConfig = cloneConfig(disk)
        state.status = t('validation.store.reloadedFromDisk')
      } catch {
        // Reported by the next load or save.
      }
    })
  }

  async function resetToDefaults() {
    state.saving = true
    state.status = ''

    try {
      const envelope = await resetStoredData()
      state.path = envelope.configPath
      state.seenAppsPath = envelope.seenAppsPath
      state.config = cloneConfig(envelope.config)
      state.originalConfig = cloneConfig(envelope.config)
      state.loadError = ''
      state.seenApps = []
      await readBack()
      state.status = t('validation.store.configReset')
    } catch (error) {
      state.status = errorText(error, t('validation.store.resetConfigFailed'))
    } finally {
      state.saving = false
    }
  }

  function discardChanges() {
    state.config = cloneConfig(state.originalConfig)
    state.status = t('validation.store.changesDiscarded')
  }

  function setActiveTab(tab: AppTab) {
    state.activeTab = tab
  }

  function addProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.depthxr.profiles.push(createProfile(state.config.modules.depthxr.defaults, defaultApplicationId ? [defaultApplicationId] : []))
  }

  function removeProfile(index: number) {
    state.config.modules.depthxr.profiles.splice(index, 1)
  }

  function syncProfileName(index: number) {
    const profile = state.config.modules.depthxr.profiles[index]
    if (!profile) {
      return
    }

    if (!profile.name.trim() || isDefaultProfileName(profile.name)) {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || defaultProfileName()
    }
  }

  function addPivotProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.pivotxr.profiles.push(
      createPivotProfile(
        state.config.modules.pivotxr.defaults,
        defaultApplicationId ? [defaultApplicationId] : [],
        state.config.modules.pivotxr.alwaysActive,
        state.config.modules.pivotxr.activationBindings,
        state.config.modules.pivotxr.viewControls,
        state.config.modules.pivotxr.behavior,
        state.config.modules.pivotxr.nudgeSetId,
      ),
    )
  }

  function removePivotProfile(index: number) {
    state.config.modules.pivotxr.profiles.splice(index, 1)
  }

  // Array order is runtime priority order: when several profiles target the
  // same application, an earlier profile owns any binding it shares with a
  // later one.
  function movePivotProfile(index: number, direction: -1 | 1) {
    const profiles = state.config.modules.pivotxr.profiles
    const target = index + direction
    if (index < 0 || index >= profiles.length || target < 0 || target >= profiles.length) {
      return
    }

    const [profile] = profiles.splice(index, 1)
    profiles.splice(target, 0, profile)
  }

  function syncPivotProfileName(index: number) {
    const profile = state.config.modules.pivotxr.profiles[index]
    if (!profile) {
      return
    }

    if (!profile.name.trim() || isDefaultProfileName(profile.name)) {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || defaultProfileName()
    }
  }

  function addQuadViewsProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.quadviews.profiles.push(
      createQuadViewsProfile(state.config.modules.quadviews.defaults, defaultApplicationId ? [defaultApplicationId] : []),
    )
  }

  function removeQuadViewsProfile(index: number) {
    state.config.modules.quadviews.profiles.splice(index, 1)
  }

  function syncQuadViewsProfileName(index: number) {
    const profile = state.config.modules.quadviews.profiles[index]
    if (!profile) {
      return
    }

    if (!profile.name.trim() || isDefaultProfileName(profile.name)) {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || defaultProfileName()
    }
  }

  function addTurboProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.turbo.profiles.push(createTurboProfile(defaultApplicationId ? [defaultApplicationId] : []))
  }

  function removeTurboProfile(index: number) {
    state.config.modules.turbo.profiles.splice(index, 1)
  }

  function syncTurboProfileName(index: number) {
    const profile = state.config.modules.turbo.profiles[index]
    if (!profile) {
      return
    }

    if (!profile.name.trim() || isDefaultProfileName(profile.name)) {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || defaultProfileName()
    }
  }

  function addFrameGenProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.framegen.profiles.push(
      createFrameGenProfile(state.config.modules.framegen.defaults, defaultApplicationId ? [defaultApplicationId] : []),
    )
  }

  function removeFrameGenProfile(index: number) {
    state.config.modules.framegen.profiles.splice(index, 1)
  }

  function syncFrameGenProfileName(index: number) {
    const profile = state.config.modules.framegen.profiles[index]
    if (!profile) {
      return
    }

    if (!profile.name.trim() || isDefaultProfileName(profile.name)) {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || defaultProfileName()
    }
  }

  // Registry shortcut: create a profile pre-assigned to one application and jump to the module tab.
  function addModuleProfileForApplication(moduleId: ModuleId, applicationId: string) {
    const application = state.config.applications.find((candidate) => candidate.id === applicationId)
    const applicationIds = application ? [application.id] : []

    if (moduleId === 'depthxr') {
      state.config.modules.depthxr.profiles.push(createProfile(state.config.modules.depthxr.defaults, applicationIds))
      syncProfileName(state.config.modules.depthxr.profiles.length - 1)
    } else if (moduleId === 'pivotxr') {
      state.config.modules.pivotxr.profiles.push(
        createPivotProfile(
          state.config.modules.pivotxr.defaults,
          applicationIds,
          state.config.modules.pivotxr.alwaysActive,
          state.config.modules.pivotxr.activationBindings,
          state.config.modules.pivotxr.viewControls,
          state.config.modules.pivotxr.behavior,
          state.config.modules.pivotxr.nudgeSetId,
        ),
      )
      syncPivotProfileName(state.config.modules.pivotxr.profiles.length - 1)
    } else if (moduleId === 'turbo') {
      state.config.modules.turbo.profiles.push(createTurboProfile(applicationIds))
      syncTurboProfileName(state.config.modules.turbo.profiles.length - 1)
    } else if (moduleId === 'framegen') {
      state.config.modules.framegen.profiles.push(createFrameGenProfile(state.config.modules.framegen.defaults, applicationIds))
      syncFrameGenProfileName(state.config.modules.framegen.profiles.length - 1)
    } else {
      state.config.modules.quadviews.profiles.push(createQuadViewsProfile(state.config.modules.quadviews.defaults, applicationIds))
      syncQuadViewsProfileName(state.config.modules.quadviews.profiles.length - 1)
    }

    state.activeTab = moduleId
  }

  // Importing is an explicit choice to replace the saved config, so it also
  // lifts the unreadable-file block (the backend keeps a copy of that file).
  function importConfig(config: VectorXRConfig) {
    state.config = cloneConfig(config)
    state.loadError = ''
    state.status = t('validation.store.configImported')
  }

  function addApplication() {
    state.config.applications.push(createApplication('Game.exe', state.config.applications))
  }

  function addSeenApplication(exe: string) {
    state.config.applications.push(createApplication(exe, state.config.applications))
    state.status = t('validation.store.applicationAdded', { exe })
  }

  async function clearSeenApplications() {
    try {
      state.seenAppsPath = await clearSeenApps()
      state.seenApps = []
      state.status = t('validation.store.seenAppsCleared')
    } catch (error) {
      state.status = errorText(error, t('validation.store.clearSeenAppsFailed'))
    }
  }

  function removeApplication(index: number) {
    const application = state.config.applications[index]
    if (!application) {
      return
    }

    state.config.applications.splice(index, 1)
    for (const profile of state.config.modules.depthxr.profiles) {
      profile.applicationIds = profile.applicationIds.filter((id) => id !== application.id)
    }
    for (const profile of state.config.modules.pivotxr.profiles) {
      profile.applicationIds = profile.applicationIds.filter((id) => id !== application.id)
    }
    for (const profile of state.config.modules.quadviews.profiles) {
      profile.applicationIds = profile.applicationIds.filter((id) => id !== application.id)
    }
    for (const profile of state.config.modules.turbo.profiles) {
      profile.applicationIds = profile.applicationIds.filter((id) => id !== application.id)
    }
    for (const profile of state.config.modules.framegen.profiles) {
      profile.applicationIds = profile.applicationIds.filter((id) => id !== application.id)
    }
  }

  return {
    state,
    isDirty,
    load,
    save,
    saveSetting,
    reloadIfChanged,
    resetToDefaults,
    discardChanges,
    importConfig,
    refreshSeenApps,
    refreshRuntimePacing,
    rediscoverRuntimePacing,
    refreshTurboMetrics,
    clearTurboMetricsSessions,
    setActiveTab,
    addProfile,
    removeProfile,
    syncProfileName,
    addPivotProfile,
    removePivotProfile,
    movePivotProfile,
    syncPivotProfileName,
    addQuadViewsProfile,
    removeQuadViewsProfile,
    syncQuadViewsProfileName,
    addTurboProfile,
    removeTurboProfile,
    syncTurboProfileName,
    addFrameGenProfile,
    removeFrameGenProfile,
    syncFrameGenProfileName,
    addModuleProfileForApplication,
    addApplication,
    addSeenApplication,
    clearSeenApplications,
    removeApplication,
  }
}
