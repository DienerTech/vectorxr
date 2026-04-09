import { reactive } from 'vue'

import { loadConfigEnvelope, saveConfigEnvelope } from '../lib/commands'
import { createProfile, defaultConfig } from '../lib/model'
import { sanitizeProfileName } from '../lib/model'
import type { VectorXRConfig } from '../lib/model'

interface StoreState {
  loading: boolean
  saving: boolean
  path: string
  config: VectorXRConfig
  status: string
}

const state = reactive<StoreState>({
  loading: false,
  saving: false,
  path: '',
  config: defaultConfig(),
  status: '',
})

export function useConfigStore() {
  async function load() {
    state.loading = true
    state.status = ''

    try {
      const envelope = await loadConfigEnvelope()
      state.path = envelope.path
      state.config = envelope.config
      state.status = 'Config loaded'
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
      state.status = 'Config saved'
    } catch (error) {
      state.status = error instanceof Error ? error.message : 'Failed to save config'
    } finally {
      state.saving = false
    }
  }

  function addProfile() {
    state.config.modules.depthxr.profiles.push(createProfile(state.config.modules.depthxr.defaults))
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
      profile.name = sanitizeProfileName(profile.match.exe)
    }
  }

  return {
    state,
    load,
    save,
    addProfile,
    removeProfile,
    syncProfileName,
  }
}
