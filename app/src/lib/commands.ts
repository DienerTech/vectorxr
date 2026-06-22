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

export interface SeenApplication {
  exe: string
  firstSeenUnixSeconds: number
  lastSeenUnixSeconds: number
  launchCount: number
}

export interface SeenAppsEnvelope {
  path: string
  observations: SeenApplication[]
}

export interface ResetStoredDataEnvelope {
  configPath: string
  seenAppsPath: string
  config: VectorXRConfig
}

export interface InputDeviceInfo {
  deviceGuid: string
  productGuid: string
  deviceName: string
  productName: string
  buttonCount: number
}

export interface CapturedDeviceBinding {
  deviceGuid: string
  productGuid: string
  deviceName: string
  inputPath: string
  inputLabel: string
  buttonIndex: number
}

export type OpenXrLayerRegistrySliceId = 'hklm64' | 'hkcu64' | 'hklm32' | 'hkcu32'
export type OpenXrLayerSignatureStatus = 'signed' | 'unsigned' | 'invalid' | 'unknown'
export type OpenXrLayerMoveDirection = 'up' | 'down'

export interface OpenXrLayerEntry {
  slice: OpenXrLayerRegistrySliceId
  manifestPath: string
  layerName: string
  description: string
  libraryPath?: string
  enabled: boolean
  order: number
  signatureStatus: OpenXrLayerSignatureStatus
  signatureStatusDescription: string
  signatureSignerSubject?: string
  signatureSignerIssuer?: string
  signatureSignerThumbprint?: string
  signatureSignerNotBefore?: string
  signatureSignerNotAfter?: string
  isVectorXr: boolean
  manifestExists: boolean
  libraryExists: boolean
  error?: string
}

export interface OpenXrLayerRegistrySlice {
  id: OpenXrLayerRegistrySliceId
  label: string
  registryPath: string
  recommended: boolean
  uncommon: boolean
  requiresElevationForWrites: boolean
  layers: OpenXrLayerEntry[]
}

export interface OpenXrLayerSnapshot {
  slices: OpenXrLayerRegistrySlice[]
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

export async function resetStoredData(): Promise<ResetStoredDataEnvelope> {
  if (!tauriAvailable()) {
    const config = defaultConfig()
    window.localStorage.setItem(localKey, JSON.stringify(config))
    return {
      configPath: './config/vectorxr.settings.json',
      seenAppsPath: './config/seen-apps.json',
      config,
    }
  }

  const envelope = await invoke<ResetStoredDataEnvelope>('reset_stored_data')
  return {
    ...envelope,
    config: normalizeConfig(envelope.config),
  }
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

export async function listInputDevices(): Promise<InputDeviceInfo[]> {
  if (!tauriAvailable()) {
    return []
  }

  return invoke<InputDeviceInfo[]>('list_input_devices')
}

export async function captureDeviceBinding(timeoutMs = 15_000): Promise<CapturedDeviceBinding | null> {
  if (!tauriAvailable()) {
    return null
  }

  return invoke<CapturedDeviceBinding | null>('capture_device_binding', { timeoutMs })
}

export async function pickSoundFile(): Promise<string | null> {
  if (!tauriAvailable()) {
    return null
  }

  const { open } = await import('@tauri-apps/plugin-dialog')
  const selected = await open({
    multiple: false,
    directory: false,
    filters: [{ name: 'WAV audio', extensions: ['wav'] }],
  })

  return typeof selected === 'string' ? selected : null
}

export async function playTestSound(path: string, activate: boolean, volume = 100): Promise<void> {
  if (!tauriAvailable()) {
    return
  }

  await invoke('play_test_sound', { path: path.trim() ? path : null, activate, volume })
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

export async function openFileDirectory(path: string): Promise<void> {
  if (!tauriAvailable() || !path) {
    return
  }

  await invoke('open_file_directory', { path })
}

export async function openExternalUrl(url: string): Promise<void> {
  if (!url) {
    return
  }

  if (!tauriAvailable()) {
    window.open(url, '_blank', 'noopener,noreferrer')
    return
  }

  await invoke('open_external_url', { url })
}

export async function loadSeenApps(): Promise<SeenAppsEnvelope> {
  if (!tauriAvailable()) {
    return {
      path: './config/seen-apps.json',
      observations: [],
    }
  }

  return invoke<SeenAppsEnvelope>('load_seen_apps')
}

export async function clearSeenApps(): Promise<string> {
  if (!tauriAvailable()) {
    return './config/seen-apps.json'
  }

  return invoke<string>('clear_seen_apps')
}

export async function loadOpenXrLayers(): Promise<OpenXrLayerSnapshot> {
  if (!tauriAvailable()) {
    return {
      slices: [
        {
          id: 'hklm64',
          label: 'Machine-wide 64-bit',
          registryPath: String.raw`HKLM\Software\Khronos\OpenXR\1\ApiLayers\Implicit`,
          recommended: true,
          uncommon: false,
          requiresElevationForWrites: true,
          layers: [],
        },
      ],
    }
  }

  return invoke<OpenXrLayerSnapshot>('load_openxr_layers')
}

export async function ensureOpenXrLayerElevation(): Promise<void> {
  if (!tauriAvailable()) {
    return
  }

  await invoke('ensure_openxr_layer_elevation')
}

export async function setOpenXrLayerEnabled(
  slice: OpenXrLayerRegistrySliceId,
  manifestPath: string,
  enabled: boolean,
): Promise<OpenXrLayerSnapshot> {
  return invoke<OpenXrLayerSnapshot>('set_openxr_layer_enabled', { slice, manifestPath, enabled })
}

export async function moveOpenXrLayer(
  slice: OpenXrLayerRegistrySliceId,
  manifestPath: string,
  direction: OpenXrLayerMoveDirection,
): Promise<OpenXrLayerSnapshot> {
  return invoke<OpenXrLayerSnapshot>('move_openxr_layer', { slice, manifestPath, direction })
}
