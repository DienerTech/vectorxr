import { invoke } from '@tauri-apps/api/core'
import { defaultConfig, type ConfigEnvelope, type DepthXRConfig } from './model'

const localKey = 'depthxr-config'

function tauriAvailable(): boolean {
  return typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window
}

function localEnvelope(): ConfigEnvelope {
  const stored = window.localStorage.getItem(localKey)
  if (!stored) {
    return {
      path: './config/depthxr.settings.json',
      config: defaultConfig(),
    }
  }

  return {
    path: './config/depthxr.settings.json',
    config: JSON.parse(stored) as DepthXRConfig,
  }
}

export async function loadConfigEnvelope(): Promise<ConfigEnvelope> {
  if (!tauriAvailable()) {
    return localEnvelope()
  }

  return invoke<ConfigEnvelope>('load_config')
}

export async function saveConfigEnvelope(config: DepthXRConfig): Promise<string> {
  if (!tauriAvailable()) {
    window.localStorage.setItem(localKey, JSON.stringify(config))
    return './config/depthxr.settings.json'
  }

  return invoke<string>('save_config', { config })
}
