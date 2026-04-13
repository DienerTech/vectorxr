<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'

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

const selectedDevice = computed(() => devices.value.find((device) => device.deviceGuid === props.modelValue.deviceGuid) ?? null)
const deviceName = computed(() => props.modelValue.deviceName?.trim() || selectedDevice.value?.deviceName || props.modelValue.deviceGuid || 'No device selected')
const inputLabel = computed(() => props.modelValue.inputLabel?.trim() || labelForInputPath(props.modelValue.inputPath))

onMounted(() => {
  void refreshDevices()
})

function labelForInputPath(inputPath: string) {
  const match = /^button-(\d+)$/.exec(inputPath.trim())
  return match ? `Button ${match[1]}` : inputPath || 'No button captured'
}

async function refreshDevices() {
  loading.value = true
  status.value = ''

  try {
    devices.value = await listInputDevices()
    if (devices.value.length === 0) {
      status.value = 'No joystick devices found.'
    }
  } catch (error) {
    status.value = error instanceof Error ? error.message : 'Failed to list joystick devices.'
  } finally {
    loading.value = false
  }
}

async function captureBinding() {
  capturing.value = true
  status.value = 'Press a joystick button now.'

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
    status.value = error instanceof Error ? error.message : 'Failed to capture joystick button.'
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
          class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
          :disabled="loading || capturing"
          :value="modelValue.deviceGuid"
          @change="selectDevice(($event.target as HTMLSelectElement).value)"
        >
          <option value="">{{ loading ? 'Scanning...' : 'Select a device' }}</option>
          <option v-for="device in devices" :key="device.deviceGuid" :value="device.deviceGuid">
            {{ device.deviceName }} ({{ device.buttonCount }} buttons)
          </option>
        </select>
      </label>

      <div>
        <span class="mb-1.5 block text-sm font-medium">Assigned Button</span>
        <div class="app-input flex min-h-11 items-center rounded-[0.75rem] px-4 py-2.5 text-sm">
          {{ inputLabel }}
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
        {{ capturing ? 'Listening...' : 'Capture Button' }}
      </button>
      <button
        class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
        :disabled="loading || capturing"
        type="button"
        @click="refreshDevices"
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
