<script setup lang="ts">
import { computed } from 'vue'
import AppPicker from './AppPicker.vue'
import { defaultTurboExperimental, toolkitInspiredTurboExperimental, type RegisteredApplication, type VectorXRConfig } from '../lib/model'

const props = defineProps<{ config: VectorXRConfig; applications: RegisteredApplication[] }>()
defineEmits<{ close: [] }>()
const settings = computed(() => props.config.modules.turbo.experimental)
const selectedApplications = computed(() => props.applications.filter(application =>
  application.enabled && settings.value.applicationIds.includes(application.id)))
const startingPoint = computed(() => !settings.value.enabled ? 'Normal timing' :
  settings.value.waitForSubmit && settings.value.sampleAtEntry && settings.value.predictionPercent === 100 && settings.value.frameLimit === 0
    ? 'Toolkit-inspired starting point' : 'Custom experiment')
function applyToolkitStartingPoint() {
  props.config.modules.turbo.experimental = toolkitInspiredTurboExperimental(settings.value.applicationIds)
}
function resetBehavior() {
  props.config.modules.turbo.experimental = {
    ...defaultTurboExperimental(), applicationIds: [...settings.value.applicationIds],
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
          <p class="mt-2 text-sm text-muted">Save and relaunch the VR application to apply timing changes.</p>
        </div>
      </div>

      <section class="mt-4 rounded-xl border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
            <input v-model="settings.enabled" type="checkbox" class="h-4 w-4 accent-depthxr-copper" />
            Timing experiments {{ settings.enabled ? 'On' : 'Off' }}
          </label>
          <button type="button" class="button-secondary rounded-xl px-4 py-2 text-sm" @click="resetBehavior">Restore normal timing</button>
        </div>
        <div class="mt-3 flex flex-wrap items-center gap-3">
          <button type="button" class="button-secondary rounded-xl px-4 py-2 text-sm" @click="applyToolkitStartingPoint">Use Toolkit-inspired starting point</button>
          <p class="text-sm text-muted" role="status">{{ startingPoint }}</p>
        </div>
        <p class="mt-2 text-sm text-muted">The starting point enables submission waiting and entry sampling, with prediction at 100% and the frame cap off.</p>
      </section>

      <section class="mt-4 rounded-xl border p-4 surface-panel-soft">
        <h3 class="font-semibold">Applications to test</h3>
        <p class="mb-3 mt-1 text-sm text-muted">Experiments apply only to the selected registered applications. Selecting an application does not enable Turbo for it.</p>
        <AppPicker v-model="settings.applicationIds" :applications="applications" />
        <p v-if="!selectedApplications.length" class="mt-3 text-sm" role="status">Choose an enabled application to use these settings. You can configure the starting point now; it takes effect only for selected applications with Turbo enabled.</p>
      </section>

      <section class="mt-4 rounded-xl border p-4 surface-panel-soft">
        <h3 class="font-semibold">Timing controls</h3>
        <p class="mt-2 text-sm text-muted">Experiments use Async pacing for selected applications and leave remembered Auto decisions unchanged.</p>
        <fieldset :disabled="!settings.enabled" class="mt-4 space-y-4 disabled:opacity-50">
          <label class="block">
            <span class="flex items-center gap-3 font-medium"><input v-model="settings.waitForSubmit" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Wait for an overlapping submission</span>
            <span class="mt-1 block text-sm text-muted">Wait for the current submission before predicting the next frame, for up to 50 ms.</span>
          </label>
          <label class="block">
            <span class="flex items-center gap-3 font-medium"><input v-model="settings.sampleAtEntry" type="checkbox" class="h-4 w-4 accent-depthxr-copper" /> Sample prediction timing at wait entry</span>
            <span class="mt-1 block text-sm text-muted">Sample time before internal waits, as Toolkit did, instead of when returning the prediction.</span>
          </label>
          <div class="grid gap-4 sm:grid-cols-2">
            <label class="block">
              <span class="block font-medium">Prediction dampening (%)</span>
              <input v-model.number="settings.predictionPercent" type="number" min="50" max="100" step="1" class="mt-2 w-28 rounded-lg border px-3 py-2 surface-panel" />
              <span class="mt-1 block text-sm text-muted">100 is unchanged; lower values shorten prediction. Requires runtime clock support.</span>
            </label>
            <label class="block">
              <span class="block font-medium">Application frame cap (FPS)</span>
              <input v-model.number="settings.frameLimit" type="number" min="0" max="240" step="1" class="mt-2 w-28 rounded-lg border px-3 py-2 surface-panel" />
              <span class="mt-1 block text-sm text-muted">0 is off; 20–240 caps the app while Turbo is active. Does not enable or lock ASW.</span>
            </label>
          </div>
        </fieldset>
      </section>

      <div class="mt-4 space-y-3 text-sm leading-6 text-muted">
        <details>
          <summary class="cursor-pointer font-medium">How to compare and tune</summary>
          <ol class="mt-3 list-decimal space-y-2 pl-5">
            <li><strong>Record normal timing first.</strong> Use the same scene, resolution, refresh rate, reprojection setting, and log level for every run.</li>
            <li><strong>Try the starting point.</strong> Save, relaunch, and compare cockpit and terrain motion while turning and moving your head. Export each run separately. If it is worse, restore normal timing.</li>
            <li><strong>Find which change helped.</strong> Turn submission waiting off for one run, restore it, then turn entry sampling off for another. Keep the combination that feels best.</li>
            <li><strong>Optional tuning.</strong> For head-motion wobble, try prediction at 90%, then 80%; return toward 100% if tracking feels delayed or worsens. For a known reprojection target, try that FPS cap (45 only for a 45 FPS target); return to 0 if pacing worsens. These are exploratory tests, not confirmed fixes.</li>
          </ol>
        </details>
        <details>
          <summary class="cursor-pointer font-medium">What did Toolkit do, and how close is this?</summary>
          <p class="mt-2">Toolkit used a shared frame lock and sampled time before waiting on that lock. Its prediction changed as the background runtime wait finished; it also waited on a repeated app poll. That was dynamic frame handling, not an automatic search for the smoothest settings.</p>
          <p class="mt-2">This preset approximates the lock and sampling effects. VectorXR keeps its 50 ms submission-wait limit, persistent worker, runtime visibility state, and existing safety handling. Toolkit's optional frame limiter used different scheduling, and its dampening also affected ordinary runtime waits; ours affects fabricated Turbo predictions. Matching settings cannot guarantee identical presentation.</p>
          <p class="mt-2">Based on <a class="underline" href="https://github.com/mbucchia/OpenXR-Toolkit/blob/6b9ecb69a4b2dc714b14a86407868af315d02531/XR_APILAYER_MBUCCHIA_toolkit/layer.cpp#L1876" target="_blank" rel="noreferrer">Toolkit source revision 6b9ecb6</a>; your installed version or settings may differ.</p>
        </details>
      </div>
    </article>
  </div>
</template>
