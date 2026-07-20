<script setup lang="ts">
import { computed, ref } from 'vue'

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
            Tune stereo boost and convergence defaults, then add per-application profiles when a title needs different depth behavior. Start with small value changes and use the runtime toggle binding for quick comparisons in headset.
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
      <div
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        <p class="font-medium">In-game IPD settings can override Stereo Boost</p>
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
        <div v-else class="mt-3 grid gap-3 lg:grid-cols-2">
          <EffectField
            v-model:value="config.modules.depthxr.defaults.stereoBoost"
            title="Stereo Boost"
            subtitle="Widens or narrows the gap between your two eye viewpoints (0% = your real IPD). Positive adds depth and 3D 'pop' and can make the world feel miniaturized; negative flattens depth and makes it feel larger. Start around +10–25%; high values exaggerate parallax and can strain your eyes."
            :min="0"
            :max="2"
            :step="0.01"
            :display-min="-100"
            :display-max="100"
            :display-step="1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:value="config.modules.depthxr.defaults.convergence"
            title="Convergence"
            subtitle="Shifts where your eyes' sightlines cross — the distance that sits at screen depth with no parallax. Positive pulls that plane toward you (near objects pop forward); negative pushes it away (scene settles back, can ease strain). 0 = app default. Use small amounts (±2–10); large values force your eyes to cross and break fusion."
            :min="-0.25"
            :max="0.25"
            :step="0.01"
            :display-min="-25"
            :display-max="25"
            :display-step="0.1"
            :display-value="toConvergenceDisplay"
            :parse-display-value="fromConvergenceDisplay"
            :display-badge="convergenceBadge"
          />
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
        <div class="grid gap-3 lg:grid-cols-2">
          <EffectField
            v-model:value="profile.settings.stereoBoost"
            :muted="!profile.enabled"
            title="Stereo Boost"
            subtitle="Widens or narrows the gap between your two eye viewpoints (0% = your real IPD). Positive adds depth and 3D 'pop' and can make the world feel miniaturized; negative flattens depth and makes it feel larger. Start around +10–25%; high values exaggerate parallax and can strain your eyes."
            :min="0"
            :max="2"
            :step="0.01"
            :display-min="-100"
            :display-max="100"
            :display-step="1"
            :display-value="toStereoBoostDisplay"
            :parse-display-value="fromStereoBoostDisplay"
            :display-badge="stereoBoostBadge"
          />
          <EffectField
            v-model:value="profile.settings.convergence"
            :muted="!profile.enabled"
            title="Convergence"
            subtitle="Shifts where your eyes' sightlines cross — the distance that sits at screen depth with no parallax. Positive pulls that plane toward you (near objects pop forward); negative pushes it away (scene settles back, can ease strain). 0 = app default. Use small amounts (±2–10); large values force your eyes to cross and break fusion."
            :min="-0.25"
            :max="0.25"
            :step="0.01"
            :display-min="-25"
            :display-max="25"
            :display-step="0.1"
            :display-value="toConvergenceDisplay"
            :parse-display-value="fromConvergenceDisplay"
            :display-badge="convergenceBadge"
          />
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
            <h2 class="mt-2 text-xl font-semibold tracking-tight">When Stereo Boost Appears Inactive</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="depthInfoOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Games with a Force IPD, virtual IPD, stereo-separation, or world-scale setting may replace the eye separation reported by OpenXR. That override takes precedence over VectorXR Stereo Boost.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>DCS:</strong> uncheck <strong>Force IPD Distance</strong> under Options &gt; VR, then fully restart DCS. A forced value fixes DCS's own virtual camera separation and can make Stereo Boost appear to do nothing.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>Expected result:</strong> Stereo Boost changes binocular scale most noticeably on nearby geometry, so a cockpit can look larger or smaller while the distant terrain and horizon appear nearly unchanged. That distance-dependent response is normal.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            <strong>Convergence is subtler:</strong> it shifts the zero-parallax plane rather than scaling the scene. Compare a nearby cockpit edge against distant scenery while moving it in small steps. If Stereo Boost still has no effect after disabling game-level overrides, enable debug logging and export the session logs.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
