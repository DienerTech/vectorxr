import { ref, type Ref } from 'vue'

import { loadGpuCapabilities } from './commands'
import type { GpuCapabilities } from './motionSource'

// One check of the graphics cards for the whole app (the backend runs it once
// and keeps it); null until it answers.
const capabilities = ref<GpuCapabilities | null>(null)
let requested = false

export function useGpuCapabilities(): Ref<GpuCapabilities | null> {
  if (!requested) {
    requested = true
    loadGpuCapabilities()
      .then((result) => { capabilities.value = result })
      .catch((error: unknown) => {
        capabilities.value = { probed: false, adapters: [], opticalFlow: false, error: String(error) }
      })
  }
  return capabilities
}
