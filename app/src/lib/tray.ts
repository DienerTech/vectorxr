import { invoke } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'

import type { HealthSummary } from './health'
import type { VectorXRConfig } from './model'

// The notification-area icon. Like the theme, the choice is stored on this PC
// only: it belongs to the app, not to the settings the layer reads.
export type TrayMode = 'off' | 'on' | 'keepRunning'

export const trayModeOptions: { value: TrayMode; label: string; description: string }[] = [
  { value: 'off', label: 'Off', description: 'No tray icon.' },
  { value: 'on', label: 'While open', description: 'A tray icon while VectorXR is open. Closing the window quits.' },
  {
    value: 'keepRunning',
    label: 'Keep running',
    description: 'Closing the window keeps VectorXR in the tray. Quit from the tray icon.',
  },
]

const storageKey = 'vectorxr-tray-mode'

function isTrayMode(value: string | null): value is TrayMode {
  return value === 'off' || value === 'on' || value === 'keepRunning'
}

export function loadTrayMode(): TrayMode {
  try {
    const stored = typeof window === 'undefined' ? null : window.localStorage.getItem(storageKey)
    return isTrayMode(stored) ? stored : 'on'
  } catch {
    return 'on'
  }
}

export function storeTrayMode(mode: TrayMode) {
  try {
    window.localStorage.setItem(storageKey, mode)
  } catch {
    // Not remembered; the tray still follows the choice until the app closes.
  }
}

// What the tray shows: the saved settings (what the layer uses), not unsaved
// edits on the page.
export interface TrayStatus {
  available: boolean
  healthLabel: string
  enabled: boolean
  logLevel: 'info' | 'debug'
}

export function trayStatus(saved: VectorXRConfig, health: HealthSummary, available: boolean): TrayStatus {
  return {
    available,
    healthLabel: health.label,
    enabled: saved.core.enabled,
    logLevel: saved.core.logLevel === 'debug' ? 'debug' : 'info',
  }
}

export type TrayAction =
  | { action: 'setEnabled'; enabled: boolean }
  | { action: 'setLogLevel'; level: 'info' | 'debug' }
  | { action: 'exportDebug' }
  | { action: 'showHealth' }
  | { action: 'refresh' }

function tauriAvailable(): boolean {
  return typeof window !== 'undefined' && '__TAURI_INTERNALS__' in window
}

export async function configureTray(mode: TrayMode): Promise<void> {
  if (!tauriAvailable()) return
  await invoke('configure_tray', { mode })
}

export async function updateTrayStatus(status: TrayStatus): Promise<void> {
  if (!tauriAvailable()) return
  await invoke('update_tray_status', { status })
}

export async function onTrayAction(handler: (action: TrayAction) => void): Promise<UnlistenFn> {
  if (!tauriAvailable()) return () => {}
  return listen<TrayAction>('tray-action', (event) => handler(event.payload))
}
