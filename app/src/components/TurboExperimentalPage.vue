<script setup lang="ts">
import { computed } from 'vue'
import AppPicker from './AppPicker.vue'
import { defaultTurboExperimental, type RegisteredApplication, type VectorXRConfig } from '../lib/model'

const props = defineProps<{ config: VectorXRConfig; applications: RegisteredApplication[] }>()
defineEmits<{ close: [] }>()
const settings = computed(() => props.config.modules.turbo.experimental)
function resetBehavior() {
  props.config.modules.turbo.experimental = {
    ...defaultTurboExperimental(), applicationIds: [...settings.value.applicationIds], timingTrace: settings.value.timingTrace,
  }
}
</script>

<template>
  <div class="space-y-4">
    <button type="button" class="button-secondary rounded-xl px-4 py-2 text-sm" @click="$emit('close')">← Back to Turbo</button>
    <article class="rounded-[1.25rem] border p-5 shadow-panel surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div>
          <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Experimental</p>
          <h2 class="mt-1 text-2xl font-semibold">Turbo timing experiments</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">Compare one timing change at a time in the same scene. Save, then relaunch the VR application after each change. These settings do not switch a running session.</p>
        </div>
        <button type="button" class="button-secondary rounded-xl px-4 py-2 text-sm" @click="resetBehavior">Restore normal timing</button>
      </div>

      <section class="mt-5 rounded-xl border p-4 surface-panel-soft">
        <h3 class="font-semibold">Applications to test</h3>
        <p class="mb-3 mt-1 text-sm text-muted">Experiments and timing logs apply only to the selected registered applications. Selecting an application does not enable Turbo for it.</p>
        <AppPicker v-model="settings.applicationIds" :applications="applications" />
        <p v-if="!settings.applicationIds.length" class="mt-3 text-sm" role="status">No applications selected. Experiments and timing capture will remain inactive.</p>
      </section>

      <section class="mt-4 rounded-xl border p-4 surface-panel-soft">
        <label class="flex items-center gap-3 font-semibold">
          <input v-model="settings.timingTrace" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Capture detailed timing logs
        </label>
        <p class="mt-2 text-sm leading-6 text-muted">Works with normal timing or experiments. Records short bursts of frame waits, predictions, pose lookup times, and submissions in the standard logs included in Export Debug Report. Use Info or Debug logging. Capture is bounded to limit overhead and file size; it does not measure headset presentation or ASW activity.</p>
      </section>

      <section class="mt-4 rounded-xl border p-4 surface-panel-soft">
        <label class="flex items-center gap-3 font-semibold">
          <input v-model="settings.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Enable timing experiments
        </label>
        <p class="mt-2 text-sm leading-6 text-muted">Off uses normal VectorXR timing. When enabled, experiments use Async pacing for the selected applications and do not read or save Auto pacing decisions. Existing Turbo safety protection still applies. These controls are not an OpenXR Toolkit compatibility preset.</p>
        <fieldset :disabled="!settings.enabled" class="mt-4 space-y-4 disabled:opacity-50">
          <label class="block">
            <span class="flex items-center gap-3 font-medium"><input v-model="settings.waitForSubmit" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Wait for an overlapping submission</span>
            <span class="mt-1 block text-sm leading-6 text-muted">Delay the next app frame wait until the current submission finishes. After 50 ms, use the normal protected handoff so the experiment cannot wait indefinitely.</span>
          </label>
          <label class="block">
            <span class="flex items-center gap-3 font-medium"><input v-model="settings.sampleAtEntry" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Sample prediction timing at wait entry</span>
            <span class="mt-1 block text-sm leading-6 text-muted">Measure elapsed time before internal waits instead of when returning the prediction. Compare independently from the submission option.</span>
          </label>
          <label class="block">
            <span class="block font-medium">Prediction dampening (%)</span>
            <input v-model.number="settings.predictionPercent" type="number" min="50" max="100" step="1" class="mt-2 w-28 rounded-lg border px-3 py-2 surface-panel" />
            <span class="mt-1 block text-sm leading-6 text-muted">100 preserves the prediction. Lower values shorten the prediction horizon toward the current runtime time. Requires runtime clock conversion; unsupported runtimes retain normal prediction and report that in the log.</span>
          </label>
          <label class="block">
            <span class="block font-medium">Application frame cap (FPS)</span>
            <input v-model.number="settings.frameLimit" type="number" min="0" max="240" step="1" class="mt-2 w-28 rounded-lg border px-3 py-2 surface-panel" />
            <span class="mt-1 block text-sm leading-6 text-muted">0 disables the cap; use 20–240 to limit the app while Turbo is active. This does not enable or lock ASW.</span>
          </label>
        </fieldset>
      </section>
      <p class="mt-4 text-sm leading-6 text-muted">For an A/B comparison, capture normal timing first, change one control, then repeat after relaunch. Restore normal timing and relaunch to return to the existing behavior. Export each result separately; averages within a session include all captured conditions.</p>
    </article>
  </div>
</template>
