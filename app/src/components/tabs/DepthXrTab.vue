<script setup lang="ts">
import { computed, ref } from 'vue'

import DepthAnchorField from '../DepthAnchorField.vue'
import EffectField from '../EffectField.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import { convergenceBadge, fromConvergenceDisplay, fromStereoBoostDisplay, stereoBoostBadge, toConvergenceDisplay, toStereoBoostDisplay } from '../../lib/display'
import type { RegisteredApplication, VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addProfile: []
  removeProfile: [index: number]
  syncProfileName: [index: number]
}>()

const depthInfoOpen = ref(false)
const bindingSubPageOpen = ref(false)
const extendedConvergenceRange = ref(false)

function convergenceDisplayLimit(value: number): number {
  return extendedConvergenceRange.value || Math.abs(toConvergenceDisplay(value)) > 5
    ? 25
    : 5
}

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  props.config.modules.depthxr.profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId)
      if (firstIndex === undefined) {
        firstProfileByApplication.set(applicationId, index)
        continue
      }

      const appName = applicationNameById.get(applicationId) ?? applicationId
      const message = `${appName} is already targeted by Profile ${firstIndex + 1}. The first active profile wins.`
      warnings.set(index, [...(warnings.get(index) ?? []), message])
    }
  })

  return warnings
})
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    module-label="Depth"
    :binding="config.modules.depthxr.bindings.toggleEnabled"
    label="Depth Toggle Binding"
    description="Toggle Depth on or off at runtime for quick A/B testing."
    @update:binding="config.modules.depthxr.bindings.toggleEnabled = $event"
    @close="bindingSubPageOpen = false"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <!-- Module header -->
      <div class="mb-4 flex flex-wrap items-start justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Depth</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Shape binocular scale and depth placement as one system, then use per-application profiles where a title needs its own tuning. Start at neutral, change one control at a time, and use the runtime binding for immediate A/B comparisons.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="depthInfoOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          Depth Troubleshooting
        </button>
      </div>

      <div class="mb-5 grid gap-3 lg:grid-cols-3" role="note" aria-label="Depth tuning guide">
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">Step 1: Set scale</p>
          <p class="mt-2 text-sm font-semibold">Stereo Depth</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            Left makes the world feel larger and flatter. Right strengthens stereo parallax and can make the world feel smaller. Nearby geometry changes most.
          </p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">Step 2: Place depth</p>
          <p class="mt-2 text-sm font-semibold">Convergence Plane</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            Leave this at zero while setting scale. Then use tiny steps: left moves the plane farther, right moves it nearer. Either extreme can make fusion uncomfortable.
          </p>
        </div>
        <div class="rounded-[1rem] border p-4 surface-panel-strong">
          <p class="eyebrow text-[10px] font-semibold uppercase tracking-[0.18em]">Step 3: Compare</p>
          <p class="mt-2 text-sm font-semibold">Tune as a pair</p>
          <p class="mt-1 text-[13px] leading-5 text-muted">
            Find scale first, then use Convergence only to settle the scene's depth placement. Stop at the first sign of eye strain, double vision, or a hard-to-fuse horizon.
          </p>
        </div>
      </div>
      <label class="mb-5 inline-flex cursor-pointer items-center gap-3 rounded-full border px-4 py-2 text-xs font-medium surface-panel-strong">
        <input v-model="extendedConvergenceRange" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
        Extended Convergence range (&plusmn;25)
        <span class="text-muted">Normal tuning is limited to &plusmn;5.</span>
      </label>
      <div
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        <p class="font-medium">In-game IPD settings can override Stereo Depth</p>
        <p class="mt-1">Disable any Force IPD, virtual IPD, stereo-separation, or world-scale override before testing Depth.</p>
        <p class="mt-1">Example: in DCS, uncheck <strong>Force IPD Distance</strong> under Options &gt; VR.</p>
      </div>

      <!-- Module-level binding — applies regardless of which profile is active -->
      <ModuleBindingPanel
        class="mb-5"
        heading="Depth Toggle Binding"
        :binding="config.modules.depthxr.bindings.toggleEnabled"
        hint="Toggle Depth on or off at runtime for quick A/B testing."
        @edit="bindingSubPageOpen = true"
      />

      <!-- Default Profile -->
      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>
        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.depthxr.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.depthxr.enabled ? 'On' : 'Off' }}
        </label>
        <div v-if="!config.modules.depthxr.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          The default profile is off and has no effect — applications without an enabled custom profile get no Depth adjustment. Enabled custom profiles below still apply to their assigned applications.
        </div>
        <div v-else class="mt-3 space-y-3">
          <div class="grid gap-3 lg:grid-cols-2">
            <EffectField
              v-model:value="config.modules.depthxr.defaults.stereoBoost"
              title="Stereo Depth / World Scale"
              subtitle="Changes virtual eye separation while keeping your physical headset IPD untouched. Move right for stronger near-field stereo and a smaller-feeling world; move left for a larger-feeling, flatter world. Start around ±5–15%."
              :min="0"
              :max="2"
              :step="0.01"
              :display-min="-100"
              :display-max="100"
              :display-step="1"
              :display-value="toStereoBoostDisplay"
              :parse-display-value="fromStereoBoostDisplay"
              :display-badge="stereoBoostBadge"
              left-label="- Larger world"
              center-label="0 Native scale"
              right-label="+ 3D stereo effect"
            />
            <EffectField
              v-model:value="config.modules.depthxr.defaults.convergence"
              title="Convergence / Depth Plane"
              subtitle="Applies an equal-and-opposite projection shift to place the zero-parallax plane. Keep 0 while tuning Stereo Depth, then adjust in 0.1–0.5 steps. Negative moves the plane farther; positive moves it nearer."
              :min="-convergenceDisplayLimit(config.modules.depthxr.defaults.convergence) / 100"
              :max="convergenceDisplayLimit(config.modules.depthxr.defaults.convergence) / 100"
              :step="0.001"
              :display-min="-convergenceDisplayLimit(config.modules.depthxr.defaults.convergence)"
              :display-max="convergenceDisplayLimit(config.modules.depthxr.defaults.convergence)"
              :display-step="0.1"
              :display-value="toConvergenceDisplay"
              :parse-display-value="fromConvergenceDisplay"
              :display-badge="convergenceBadge"
              left-label="- Farther plane"
              center-label="0 App default"
              right-label="+ Nearer plane"
            />
          </div>
          <DepthAnchorField v-model:value="config.modules.depthxr.defaults.depthAnchor" />
        </div>
      </details>
    </article>

    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Override Depth per application. The first enabled matching profile wins.</p>
        </div>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('addProfile')">
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.depthxr.profiles"
        :key="`depth-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Depth"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeProfile', index)"
        @sync-name="$emit('syncProfileName', index)"
      >
        <div class="space-y-3">
          <div class="grid gap-3 lg:grid-cols-2">
            <EffectField
              v-model:value="profile.settings.stereoBoost"
              :muted="!profile.enabled"
              title="Stereo Depth / World Scale"
              subtitle="Changes virtual eye separation while keeping your physical headset IPD untouched. Move right for stronger near-field stereo and a smaller-feeling world; move left for a larger-feeling, flatter world. Start around ±5–15%."
              :min="0"
              :max="2"
              :step="0.01"
              :display-min="-100"
              :display-max="100"
              :display-step="1"
              :display-value="toStereoBoostDisplay"
              :parse-display-value="fromStereoBoostDisplay"
              :display-badge="stereoBoostBadge"
              left-label="- Larger world"
              center-label="0 Native scale"
              right-label="+ 3D stereo effect"
            />
            <EffectField
              v-model:value="profile.settings.convergence"
              :muted="!profile.enabled"
              title="Convergence / Depth Plane"
              subtitle="Applies an equal-and-opposite projection shift to place the zero-parallax plane. Keep 0 while tuning Stereo Depth, then adjust in 0.1–0.5 steps. Negative moves the plane farther; positive moves it nearer."
              :min="-convergenceDisplayLimit(profile.settings.convergence) / 100"
              :max="convergenceDisplayLimit(profile.settings.convergence) / 100"
              :step="0.001"
              :display-min="-convergenceDisplayLimit(profile.settings.convergence)"
              :display-max="convergenceDisplayLimit(profile.settings.convergence)"
              :display-step="0.1"
              :display-value="toConvergenceDisplay"
              :parse-display-value="fromConvergenceDisplay"
              :display-badge="convergenceBadge"
              left-label="- Farther plane"
              center-label="0 App default"
              right-label="+ Nearer plane"
            />
          </div>
          <DepthAnchorField v-model:value="profile.settings.depthAnchor" :muted="!profile.enabled" />
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.depthxr.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to override depth values for a specific application.
      </div>
    </section>

    <div v-if="depthInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Depth Troubleshooting</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">When Depth Behaves Unexpectedly</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="depthInfoOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Games with a Force IPD, virtual IPD, stereo-separation, or world-scale setting may replace the eye separation reported by OpenXR. That override takes precedence over VectorXR Stereo Depth.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>DCS:</strong> uncheck <strong>Force IPD Distance</strong> under Options &gt; VR, then fully restart DCS. A forced value fixes DCS's own virtual camera separation and can make Stereo Depth appear to do nothing.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>Expected result:</strong> Stereo Depth changes binocular scale most noticeably on nearby geometry, so a cockpit can look larger or smaller while the distant terrain and horizon appear nearly unchanged. That distance-dependent response is normal.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>Convergence is subtler:</strong> it shifts the zero-parallax plane rather than scaling the scene. Compare a nearby cockpit edge against distant scenery while moving it in small steps. If Stereo Depth still has no effect after disabling game-level overrides, enable debug logging and export the session logs.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>Depth Anchor changes submission geometry:</strong> the game still renders the tuned stereo image, but the runtime receives headset-native eye poses and FOV. Treat it as a per-game experiment: compare it both on and off at the same slider values, and leave it off if reprojection, edge stability, or comfort becomes worse.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
