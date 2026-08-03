import { readonly, ref } from 'vue'

import { listInputDevices, type InputDeviceInfo } from './commands'

const DEVICE_REFRESH_INTERVAL_MS = 5_000

const devices = ref<InputDeviceInfo[]>([])
const loading = ref(false)
const hasCompletedInitialScan = ref(false)
const lastRefreshFailed = ref(false)
const lastRefreshError = ref('')

let subscriberCount = 0
let activeCaptureCount = 0
let refreshIntervalId: ReturnType<typeof window.setInterval> | null = null
let refreshInFlight: Promise<void> | null = null

export const inputDeviceDiscovery = {
  devices: readonly(devices),
  loading: readonly(loading),
  hasCompletedInitialScan: readonly(hasCompletedInitialScan),
  lastRefreshFailed: readonly(lastRefreshFailed),
  lastRefreshError: readonly(lastRefreshError),
}

export function refreshInputDevices(options: { showLoading?: boolean } = {}): Promise<void> {
  if (refreshInFlight) return refreshInFlight

  const showLoading = options.showLoading === true
  if (showLoading) loading.value = true

  refreshInFlight = listInputDevices()
    .then((result) => {
      devices.value = result
      lastRefreshFailed.value = false
      lastRefreshError.value = ''
    })
    .catch((error: unknown) => {
      lastRefreshFailed.value = true
      lastRefreshError.value = error instanceof Error ? error.message : 'Failed to list joystick devices.'
    })
    .finally(() => {
      hasCompletedInitialScan.value = true
      if (showLoading) loading.value = false
      refreshInFlight = null
    })

  return refreshInFlight
}

export function startInputDeviceDiscovery(): Promise<void> {
  subscriberCount += 1
  if (subscriberCount > 1) return refreshInFlight ?? Promise.resolve()

  const initialRefresh = refreshInputDevices({ showLoading: !hasCompletedInitialScan.value })
  refreshIntervalId = window.setInterval(() => {
    if (activeCaptureCount === 0) void refreshInputDevices()
  }, DEVICE_REFRESH_INTERVAL_MS)
  return initialRefresh
}

export function stopInputDeviceDiscovery() {
  subscriberCount = Math.max(0, subscriberCount - 1)
  if (subscriberCount > 0 || refreshIntervalId === null) return

  window.clearInterval(refreshIntervalId)
  refreshIntervalId = null
}

export function pauseInputDeviceDiscovery() {
  activeCaptureCount += 1
}

export function resumeInputDeviceDiscovery() {
  activeCaptureCount = Math.max(0, activeCaptureCount - 1)
}