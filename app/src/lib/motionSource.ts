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
  // The choice does nothing: Interpolate measures with Shader whatever is chosen.
  locked: boolean
  // Why NVIDIA hardware cannot be chosen (null: it can be, or it is not known).
  hardwareUnavailable: string | null
  note: string | null
}

// The motion source as the layer resolves it. The saved choice is never
// rewritten here: switching to Extrapolate, or to a PC with the hardware,
// brings it back.
export function motionSourceState(
  settings: Pick<FrameGenSettings, 'synthesisMode' | 'motionEstimation'>,
  gpus: GpuCapabilities | null,
): MotionSourceState {
  const locked = settings.synthesisMode === 'interpolate'
  let hardwareUnavailable: string | null = null
  if (gpus?.probed && !gpus.opticalFlow) {
    const cards = gpus.adapters.map((adapter) => `${adapter.name}: ${adapter.detail}`).join('; ')
    hardwareUnavailable = cards
      ? `No graphics card in this PC has NVIDIA's optical flow hardware (${cards}).`
      : "No graphics card in this PC has NVIDIA's optical flow hardware."
  }
  const effective: FrameGenMotionEstimation = locked || hardwareUnavailable ? 'shader' : settings.motionEstimation
  let note: string | null = null
  if (locked) {
    note = 'Interpolate always measures motion with Shader: its frames between real frames need motion sooner than the NVIDIA hardware delivers it beside a busy game. The choice applies to Extrapolate.'
  } else if (hardwareUnavailable && settings.motionEstimation === 'hardware') {
    note = 'NVIDIA hardware is chosen, but no graphics card here has it, so Shader measures motion.'
  }
  return { effective, locked, hardwareUnavailable, note }
}
