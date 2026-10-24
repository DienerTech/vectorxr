import { t } from '../i18n/index.ts'
import type { FrameGenMotionEstimation, FrameGenSettings } from './model'

// What the app's graphics card check found (load_gpu_capabilities): every
// card, each asked of NVIDIA's driver, not just the first one listed.
export interface GpuAdapterInfo {
  name: string
  vendorId: number
  deviceId: number
  opticalFlow: boolean
  detail: string // why it has no optical flow hardware (empty when it has it)
}

export interface GpuCapabilities {
  probed: boolean // false: nothing is known about the cards
  adapters: GpuAdapterInfo[]
  opticalFlow: boolean // some card has NVIDIA's optical flow hardware
  error: string
}

export interface MotionSourceState {
  // What Frame Generation measures motion with for these settings on this PC.
  effective: FrameGenMotionEstimation
  // The choice stays available in Dynamic; hardware may fall back while running.
  locked: boolean
  // Why NVIDIA hardware cannot be chosen (null: it can be, or it is not known).
  hardwareUnavailable: string | null
  note: string | null
}

// The motion source as the layer resolves it. The saved choice is never
// rewritten here: switching to Extrapolate, or to a PC with the hardware,
// brings it back.
export function motionSourceState(
  settings: Pick<FrameGenSettings, 'synthesisMode' | 'motionEstimation' | 'frameMultiplier'> & Partial<Pick<FrameGenSettings, 'interpolateFlow'>>,
  gpus: GpuCapabilities | null,
): MotionSourceState {
  const interpolate = settings.synthesisMode === 'interpolate'
  const locked = false
  let hardwareUnavailable: string | null = null
  if (gpus?.probed && !gpus.opticalFlow) {
    // adapter.detail comes from the app backend and stays as it reads there.
    const cards = gpus.adapters.map((adapter) => t('pivot.motionSource.cardDetail', { name: adapter.name, detail: adapter.detail })).join('; ')
    hardwareUnavailable = cards
      ? t('pivot.motionSource.hardwareUnavailableCards', { cards })
      : t('pivot.motionSource.hardwareUnavailable')
  }
  const effective: FrameGenMotionEstimation = hardwareUnavailable || (interpolate && settings.interpolateFlow === 'shader') ? 'shader' : settings.motionEstimation
  let note: string | null = null
  if (hardwareUnavailable && settings.motionEstimation === 'hardware') {
    note = t('pivot.motionSource.hardwareMissingNote')
  } else if (interpolate && effective === 'hardware') {
    note = t('framegen.settings.interpolateFlow.description')
  }
  return { effective, locked, hardwareUnavailable, note }
}
