import { invoke } from '@tauri-apps/api/core'
import { defaultConfig, normalizeConfig, type ConfigEnvelope, type VectorXRConfig } from './model'

const localKey = 'vectorxr-config'

interface SaveFilePickerHandle {
  createWritable(): Promise<{
    write(data: BlobPart): Promise<void>
    close(): Promise<void>
  }>
}

type SaveFilePickerWindow = Window & {
  showSaveFilePicker?: (options: {
    suggestedName?: string
    types?: Array<{
      description: string
      accept: Record<string, string[]>
    }>
  }) => Promise<SaveFilePickerHandle>
}

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

  const envelope = await invoke<ConfigEnvelope>('load_config')
  return {
    ...envelope,
    config: normalizeConfig(envelope.config),
  }
}

export async function saveConfigEnvelope(config: VectorXRConfig): Promise<string> {
  if (!tauriAvailable()) {
    window.localStorage.setItem(localKey, JSON.stringify(config))
    return './config/vectorxr.settings.json'
  }

  return invoke<string>('save_config', { config })
}

export async function exportConfigFile(config: VectorXRConfig): Promise<boolean> {
  const content = JSON.stringify(normalizeConfig(config), null, 2)
  const pickerWindow = window as SaveFilePickerWindow

  if (pickerWindow.showSaveFilePicker) {
    try {
      const handle = await pickerWindow.showSaveFilePicker({
        suggestedName: 'settings.json',
        types: [
          {
            description: 'JSON config',
            accept: { 'application/json': ['.json'] },
          },
        ],
      })
      const writable = await handle.createWritable()
      await writable.write(content)
      await writable.close()
      return true
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') {
        return false
      }
      throw error
    }
  }

  const blob = new Blob([content], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = 'settings.json'
  document.body.appendChild(link)
  link.click()
  link.remove()
  window.setTimeout(() => URL.revokeObjectURL(url), 0)
  return true
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
