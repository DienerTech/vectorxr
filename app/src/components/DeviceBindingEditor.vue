<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'

import { captureDeviceBinding, listInputDevices, type InputDeviceInfo } from '../lib/commands'
import type { DeviceBinding } from '../lib/model'

const props = defineProps<{
  modelValue: DeviceBinding
}>()

const emit = defineEmits<{
  'update:modelValue': [value: DeviceBinding]
}>()

const devices = ref<InputDeviceInfo[]>([])
const loading = ref(false)
const capturing = ref(false)
const status = ref('')
const metadataOpen = ref(false)
const hasCompletedInitialScan = ref(false)
const lastRefreshFailed = ref(false)

const DEVICE_REFRESH_INTERVAL_MS = 2_000

let refreshIntervalId: ReturnType<typeof window.setInterval> | null = null
let refreshInFlight = false

const selectedDevice = computed(() => devices.value.find((device) => device.deviceGuid === props.modelValue.deviceGuid) ?? null)
const hasBoundDevice = computed(() => props.modelValue.deviceGuid.trim().length > 0)
const isBoundDeviceDisconnected = computed(() => (
  hasCompletedInitialScan.value &&
  !lastRefreshFailed.value &&
  hasBoundDevice.value &&
  !selectedDevice.value
))
const disconnectedDeviceName = computed(() => props.modelValue.deviceName?.trim() || '<device disconnected>')
const deviceName = computed(() => {
  if (selectedDevice.value) {
    return selectedDevice.value.deviceName
  }

  if (isBoundDeviceDisconnected.value) {
    return disconnectedDeviceName.value
  }

  return props.modelValue.deviceGuid || 'No device selected'
})
const inputLabel = computed(() => props.modelValue.inputLabel?.trim() || labelForInputPath(props.modelValue.inputPath))

onMounted(() => {
  void refreshDevices()
  startRefreshPolling()
})

onBeforeUnmount(() => {
  stopRefreshPolling()
})

function labelForInputPath(inputPath: string) {
  const normalized = inputPath.trim()
  const button = /^button-(\d+)$/.exec(normalized)
  if (button) {
    return `Button ${button[1]}`
  }

  const hat = /^hat-(\d+)-(up|up-right|right|down-right|down|down-left|left|up-left)$/.exec(normalized)
  if (hat) {
    const direction = hat[2]
      .split('-')
      .map((part) => part[0].toUpperCase() + part.slice(1))
      .join(' ')
    return `HAT ${hat[1]} ${direction}`
  }

  return normalized || 'No input captured'
}

function deviceCapabilityLabel(device: InputDeviceInfo) {
  const buttons = `${device.buttonCount} ${device.buttonCount === 1 ? 'button' : 'buttons'}`
  const hats = `${device.hatCount} ${device.hatCount === 1 ? 'HAT' : 'HATs'}`
  return `${buttons}, ${hats}`
}

function startRefreshPolling() {
  if (refreshIntervalId !== null) {
    return
  }

  refreshIntervalId = window.setInterval(() => {
    if (capturing.value) {
      return
    }

    void refreshDevices({ silent: true })
  }, DEVICE_REFRESH_INTERVAL_MS)
}

function stopRefreshPolling() {
  if (refreshIntervalId === null) {
    return
  }

  window.clearInterval(refreshIntervalId)
  refreshIntervalId = null
}

async function refreshDevices(options: { silent?: boolean } = {}) {
  if (refreshInFlight) {
    return
  }

  refreshInFlight = true
  const shouldShowLoading = !options.silent
  if (shouldShowLoading) {
    loading.value = true
  }
  if (!options.silent) {
    status.value = ''
  }

  try {
    devices.value = await listInputDevices()
    lastRefreshFailed.value = false
    if (devices.value.length === 0 && !options.silent) {
      status.value = 'No joystick devices found.'
    }
  } catch (error) {
    lastRefreshFailed.value = true
    if (!options.silent) {
      status.value = error instanceof Error ? error.message : 'Failed to list joystick devices.'
    }
  } finally {
    hasCompletedInitialScan.value = true
    if (shouldShowLoading) {
      loading.value = false
    }
    refreshInFlight = false
  }
}

async function captureBinding() {
  capturing.value = true
  status.value = 'Press a joystick button or move a HAT switch now.'

  try {
    const binding = await captureDeviceBinding()
    if (!binding) {
      status.value = 'Capture timed out.'
      return
    }

    emit('update:modelValue', {
      type: 'device',
      deviceGuid: binding.deviceGuid,
      productGuid: binding.productGuid,
      deviceName: binding.deviceName,
      inputPath: binding.inputPath,
      inputLabel: binding.inputLabel,
    })
    status.value = `Captured ${binding.deviceName} / ${binding.inputLabel}.`
    void refreshDevices()
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Failed to capture joystick input.'
  } finally {
    capturing.value = false
  }
}

function selectDevice(deviceGuid: string) {
  const device = devices.value.find((entry) => entry.deviceGuid === deviceGuid)
  emit('update:modelValue', {
    ...props.modelValue,
    deviceGuid,
    productGuid: device?.productGuid ?? props.modelValue.productGuid ?? '',
    deviceName: device?.deviceName ?? props.modelValue.deviceName ?? '',
  })
}
</script>

<template>
  <div class="mt-4 space-y-3">
    <div class="grid gap-3 lg:grid-cols-[minmax(260px,480px)_minmax(180px,240px)]">
      <label class="block min-w-0">
        <span class="mb-1.5 block text-sm font-medium">Joystick Device</span>
        <select
          class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5 text-sm"
          :disabled="loading || capturing"
          :value="modelValue.deviceGuid"
          @change="selectDevice(($event.target as HTMLSelectElement).value)"
        >
          <option value="">{{ loading ? 'Scanning...' : 'Select a device' }}</option>
          <option v-if="isBoundDeviceDisconnected" :value="modelValue.deviceGuid">
            {{ disconnectedDeviceName }} (disconnected)
          </option>
          <option v-for="device in devices" :key="device.deviceGuid" :value="device.deviceGuid">
            {{ device.deviceName }} ({{ deviceCapabilityLabel(device) }})
          </option>
        </select>
      </label>

      <div>
        <span class="mb-1.5 block text-sm font-medium">Assigned Input</span>
        <div class="app-readonly-field flex h-11 items-center rounded-[0.75rem] px-4 py-2.5 text-sm" aria-readonly="true">
          {{ inputLabel }}
        </div>
      </div>
    </div>

    <div
      v-if="isBoundDeviceDisconnected"
      class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
    >
      <div class="flex items-start gap-3">
        <svg aria-hidden="true" class="mt-0.5 h-4 w-4 shrink-0" viewBox="0 0 20 20" fill="currentColor">
          <path fill-rule="evenodd" d="M10 2.5c.44 0 .85.23 1.08.62l6.63 11.3A1.25 1.25 0 0 1 16.63 16H3.37a1.25 1.25 0 0 1-1.08-1.88l6.63-11.3A1.25 1.25 0 0 1 10 2.5Zm0 4a.75.75 0 0 0-.75.75v3.5a.75.75 0 0 0 1.5 0v-3.5A.75.75 0 0 0 10 6.5Zm0 7.25a1 1 0 1 0 0-2 1 1 0 0 0 0 2Z" clip-rule="evenodd" />
        </svg>
        <div>
          <p class="font-medium">Bound device not currently connected</p>
          <p class="mt-1">
            This binding is still saved as {{ deviceName }} / {{ inputLabel }}, but Windows is not currently reporting that joystick.
          </p>
        </div>
      </div>
    </div>

    <div class="flex flex-wrap items-center gap-3">
      <button
        class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
        :disabled="capturing"
        type="button"
        @click="captureBinding"
      >
        {{ capturing ? 'Listening...' : 'Capture Input' }}
      </button>
      <button
        class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
        :disabled="loading || capturing"
        type="button"
        @click="void refreshDevices()"
      >
        Refresh Devices
      </button>
      <button
        class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition"
        type="button"
        @click="metadataOpen = true"
      >
        Metadata
      </button>
    </div>

    <p v-if="status" class="text-sm text-muted">{{ status }}</p>

    <div v-if="metadataOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[680px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Joystick Metadata</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ deviceName }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="metadataOpen = false">
            Close
          </button>
        </div>

        <dl class="mt-5 grid gap-3 text-sm sm:grid-cols-2">
          <div>
            <dt class="text-muted">Device GUID</dt>
            <dd class="mt-1 break-all font-mono text-xs">{{ modelValue.deviceGuid || 'Unassigned' }}</dd>
          </div>
          <div>
            <dt class="text-muted">Connection Status</dt>
            <dd class="mt-1 text-xs">{{ isBoundDeviceDisconnected ? 'Disconnected' : (modelValue.deviceGuid ? 'Connected' : 'Unassigned') }}</dd>
          </div>
          <div>
            <dt class="text-muted">Product GUID</dt>
            <dd class="mt-1 break-all font-mono text-xs">{{ modelValue.productGuid || selectedDevice?.productGuid || 'Unavailable' }}</dd>
          </div>
          <div>
            <dt class="text-muted">Input Path</dt>
            <dd class="mt-1 break-all font-mono text-xs">{{ modelValue.inputPath || 'Unassigned' }}</dd>
          </div>
          <div>
            <dt class="text-muted">Input Label</dt>
            <dd class="mt-1 break-all font-mono text-xs">{{ inputLabel }}</dd>
          </div>
        </dl>
      </div>
    </div>
  </div>
</template>
