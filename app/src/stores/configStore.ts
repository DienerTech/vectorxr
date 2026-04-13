import { computed, reactive } from 'vue'

import { loadConfigEnvelope, saveConfigEnvelope } from '../lib/commands'
import { cloneConfig, createApplication, createProfile, createPivotProfile, defaultConfig } from '../lib/model'
import type { AppTab, VectorXRConfig } from '../lib/model'

interface StoreState {
  loading: boolean
  saving: boolean
  path: string
  config: VectorXRConfig
  originalConfig: VectorXRConfig
  activeTab: AppTab
  status: string
}

const state = reactive<StoreState>({
  loading: false,
  saving: false,
  path: '',
  config: defaultConfig(),
  originalConfig: defaultConfig(),
  activeTab: 'core',
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
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to load config'
    } finally {
      state.loading = false
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

  function importConfig(config: VectorXRConfig) {
    state.config = cloneConfig(config)
    state.status = 'Config imported — save to write to disk'
  }

  function addApplication() {
    state.config.applications.push(createApplication('Game.exe', state.config.applications))
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
  }

  return {
    state,
    isDirty,
    load,
    save,
    discardChanges,
    importConfig,
    setActiveTab,
    addProfile,
    removeProfile,
    syncProfileName,
    addPivotProfile,
    removePivotProfile,
    syncPivotProfileName,
    addApplication,
    removeApplication,
  }
}
