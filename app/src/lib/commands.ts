import { invoke } from '@tauri-apps/api/core'
import { defaultConfig, normalizeConfig, type ConfigEnvelope, type VectorXRConfig } from './model'

const localKey = 'vectorxr-config'

export interface LogFileEntry {
  name: string
  path: string
  modifiedUnixSeconds: number
  content: string
}

export interface LogSnapshot {
  directory: string
  activePath: string
  files: LogFileEntry[]
}

function tauriAvailable(): boolean {
  return typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window
}

function localEnvelope(): ConfigEnvelope {
  const stored = window.localStorage.getItem(localKey)
  if (!stored) {
    return {
      path: './config/vectorxr.settings.json',
      config: defaultConfig(),
    }
  }

  return {
    path: './config/vectorxr.settings.json',
    config: normalizeConfig(JSON.parse(stored) as unknown),
  }
}

export async function loadConfigEnvelope(): Promise<ConfigEnvelope> {
  if (!tauriAvailable()) {
    return localEnvelope()
  }

  return invoke<ConfigEnvelope>('load_config')
}

export async function saveConfigEnvelope(config: VectorXRConfig): Promise<string> {
  if (!tauriAvailable()) {
    window.localStorage.setItem(localKey, JSON.stringify(config))
    return './config/vectorxr.settings.json'
  }

  return invoke<string>('save_config', { config })
}

export async function loadLogSnapshot(): Promise<LogSnapshot> {
  if (!tauriAvailable()) {
    return {
      directory: './logs',
      activePath: '',
      files: [],
    }
  }

  return invoke<LogSnapshot>('load_log_snapshot')
}
