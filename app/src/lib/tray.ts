import { invoke } from '@tauri-apps/api/core'
import { listen, type UnlistenFn } from '@tauri-apps/api/event'

import type { HealthSummary } from './health'
import type { VectorXRConfig } from './model'

// The notification-area icon, on or off. On, closing the window keeps the
// app running in the tray (one window: opening VectorXR again shows it); off,
// closing the window quits. Like the theme, the choice is stored on this PC
// only: it belongs to the app, not to the settings the layer reads.
const storageKey = 'vectorxr-tray-mode'

// Stored as 'on' or 'off'. Before 0.19.3 there were three modes: 'on' (a
// tray icon while the window was open) and 'keepRunning' are now on.
export function loadTrayEnabled(): boolean {
  try {
    const stored = typeof window === 'undefined' ? null : window.localStorage.getItem(storageKey)
    return stored !== 'off'
  } catch {
    return true
  }
}

export function storeTrayEnabled(enabled: boolean) {
  try {
    window.localStorage.setItem(storageKey, enabled ? 'on' : 'off')
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

export async function configureTray(enabled: boolean): Promise<void> {
  if (!tauriAvailable()) return
  await invoke('configure_tray', { mode: enabled ? 'on' : 'off' })
}

export async function updateTrayStatus(status: TrayStatus): Promise<void> {
  if (!tauriAvailable()) return
  await invoke('update_tray_status', { status })
}

export async function onTrayAction(handler: (action: TrayAction) => void): Promise<UnlistenFn> {
  if (!tauriAvailable()) return () => {}
  return listen<TrayAction>('tray-action', (event) => handler(event.payload))
}
