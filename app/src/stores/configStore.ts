import { reactive } from 'vue'

import { loadConfigEnvelope, saveConfigEnvelope } from '../lib/commands'
import { createProfile, defaultConfig } from '../lib/model'
import type { DepthXRConfig } from '../lib/model'

interface StoreState {
  loading: boolean
  saving: boolean
  path: string
  config: DepthXRConfig
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
    state.config.profiles.push(createProfile(state.config.global))
  }

  function removeProfile(index: number) {
    state.config.profiles.splice(index, 1)
  }

  return {
    state,
    load,
    save,
    addProfile,
    removeProfile,
  }
}
