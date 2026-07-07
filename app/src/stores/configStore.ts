import { computed, reactive } from 'vue'

import {
  clearRuntimePacingObservation,
  clearSeenApps,
  loadConfigEnvelope,
  loadRuntimePacing,
  loadSeenApps,
  resetStoredData,
  saveConfigEnvelope,
  type ActiveRuntimeInfo,
  type SeenApplication,
} from '../lib/commands'
import { cloneConfig, createApplication, createProfile, createPivotProfile, createQuadViewsProfile, createTurboProfile, defaultConfig } from '../lib/model'
import type { AppTab, ModuleId, RuntimePacingObservation, VectorXRConfig } from '../lib/model'

interface StoreState {
  loading: boolean
  saving: boolean
  path: string
  seenAppsPath: string
  seenApps: SeenApplication[]
  seenAppsLoading: boolean
  runtimePacingPath: string
  runtimePacing: RuntimePacingObservation[]
  activeRuntime: ActiveRuntimeInfo | null
  runtimePacingLoading: boolean
  config: VectorXRConfig
  originalConfig: VectorXRConfig
  activeTab: AppTab
  status: string
}

const state = reactive<StoreState>({
  loading: false,
  saving: false,
  path: '',
  seenAppsPath: '',
  seenApps: [],
  seenAppsLoading: false,
  runtimePacingPath: '',
  runtimePacing: [],
  activeRuntime: null,
  runtimePacingLoading: false,
  config: defaultConfig(),
  originalConfig: defaultConfig(),
  activeTab: 'home',
  status: '',
})

const isDirty = computed(() => JSON.stringify(state.config) !== JSON.stringify(state.originalConfig))

export function useConfigStore() {
  async function load() {
    state.loading = true
    state.status = ''

    try {
      const envelope = await loadConfigEnvelope()
      state.path = envelope.path
      state.config = cloneConfig(envelope.config)
      state.originalConfig = cloneConfig(envelope.config)
      state.status = ''
      await Promise.all([refreshSeenApps(), refreshRuntimePacing()])
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to load config'
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
      state.status = error instanceof Error ? error.message : 'Failed to load seen apps'
    } finally {
      state.seenAppsLoading = false
    }
  }

  async function refreshRuntimePacing() {
    state.runtimePacingLoading = true

    try {
      const envelope = await loadRuntimePacing()
      state.runtimePacingPath = envelope.path
      state.runtimePacing = envelope.observations
      state.activeRuntime = envelope.activeRuntime
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to load runtime pacing state'
    } finally {
      state.runtimePacingLoading = false
    }
  }

  async function rediscoverRuntimePacing(runtimeName: string) {
    try {
      const envelope = await clearRuntimePacingObservation(runtimeName)
      state.runtimePacingPath = envelope.path
      state.runtimePacing = envelope.observations
      state.activeRuntime = envelope.activeRuntime
      state.status = `Pacing verdict cleared for ${runtimeName}; Auto re-discovers at the next session`
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to clear pacing verdict'
    }
  }

  async function save() {
    state.saving = true
    state.status = ''

    try {
      state.path = await saveConfigEnvelope(state.config)
      state.originalConfig = cloneConfig(state.config)
      state.status = 'Config saved'
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to save config'
    } finally {
      state.saving = false
    }
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
      state.seenApps = []
      state.status = 'Config reset to defaults'
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to reset config'
    } finally {
      state.saving = false
    }
  }

  function discardChanges() {
    state.config = cloneConfig(state.originalConfig)
    state.status = 'Unsaved changes discarded'
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

    if (!profile.name.trim() || profile.name === 'New Profile') {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || 'New Profile'
    }
  }

  function addPivotProfile() {
    const defaultApplicationId = state.config.applications[0]?.id
    state.config.modules.pivotxr.profiles.push(
      createPivotProfile(
        state.config.modules.pivotxr.defaults,
        defaultApplicationId ? [defaultApplicationId] : [],
        state.config.modules.pivotxr.activationMode,
        state.config.modules.pivotxr.activationBinding,
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

    if (!profile.name.trim() || profile.name === 'New Profile') {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || 'New Profile'
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

    if (!profile.name.trim() || profile.name === 'New Profile') {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || 'New Profile'
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

    if (!profile.name.trim() || profile.name === 'New Profile') {
      const firstApplication = state.config.applications.find((application) => application.id === profile.applicationIds[0])
      profile.name = firstApplication?.name || 'New Profile'
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
          state.config.modules.pivotxr.activationMode,
          state.config.modules.pivotxr.activationBinding,
        ),
      )
      syncPivotProfileName(state.config.modules.pivotxr.profiles.length - 1)
    } else if (moduleId === 'turbo') {
      state.config.modules.turbo.profiles.push(createTurboProfile(applicationIds))
      syncTurboProfileName(state.config.modules.turbo.profiles.length - 1)
    } else {
      state.config.modules.quadviews.profiles.push(createQuadViewsProfile(state.config.modules.quadviews.defaults, applicationIds))
      syncQuadViewsProfileName(state.config.modules.quadviews.profiles.length - 1)
    }

    state.activeTab = moduleId
  }

  function importConfig(config: VectorXRConfig) {
    state.config = cloneConfig(config)
    state.status = 'Config imported — save to write to disk'
  }

  function addApplication() {
    state.config.applications.push(createApplication('Game.exe', state.config.applications))
  }

  function addSeenApplication(exe: string) {
    state.config.applications.push(createApplication(exe, state.config.applications))
    state.status = `${exe} added to the application registry`
  }

  async function clearSeenApplications() {
    try {
      state.seenAppsPath = await clearSeenApps()
      state.seenApps = []
      state.status = 'Seen apps cleared'
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to clear seen apps'
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
  }

  return {
    state,
    isDirty,
    load,
    save,
    resetToDefaults,
    discardChanges,
    importConfig,
    refreshSeenApps,
    refreshRuntimePacing,
    rediscoverRuntimePacing,
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
    addModuleProfileForApplication,
    addApplication,
    addSeenApplication,
    clearSeenApplications,
    removeApplication,
  }
}
