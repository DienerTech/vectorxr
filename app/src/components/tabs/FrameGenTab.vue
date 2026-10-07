<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from 'vue'

import { loadRuntimeStatus, type RuntimeStatusSession } from '../../lib/commands'
import DefaultProfileExclusions from '../DefaultProfileExclusions.vue'
import FrameGenSettingsFields from '../FrameGenSettingsFields.vue'
import ModuleBindingPage from '../ModuleBindingPage.vue'
import ModuleBindingPanel from '../ModuleBindingPanel.vue'
import ProfileShell from '../ProfileShell.vue'
import SetupGuide from '../SetupGuide.vue'
import {
  savedBindingConflictWarnings,
  type AppTab,
  type FrameGenDiagnosticView,
  type FrameGenSettings,
  type RegisteredApplication,
  type VectorXRConfig,
} from '../../lib/model'
import { configWarnings } from '../../lib/validation'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addFrameGenProfile: []
  removeFrameGenProfile: [index: number]
  syncFrameGenProfileName: [index: number]
  navigate: [tab: AppTab]
}>()

const framegen = computed(() => props.config.modules.framegen)
const howItWorksOpen = ref(false)
const bindingSubPageOpen = ref(false)
let savedScrollTop = 0

// Open the setup guide for first-time setup; returning users with an enabled profile start with it closed.
const setupGuideOpen = !(props.config.modules.framegen.enabled || props.config.modules.framegen.profiles.some((profile) => profile.enabled))
const toggleBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [framegen.value.toggleBinding]))
const turboWarnings = computed(() => configWarnings(props.config))

const restartNote = 'Turning Frame Generation off applies to a running game at once; turning it on takes effect the next time the game starts.'

const diagnosticViews: { value: FrameGenDiagnosticView; label: string; description: string }[] = [
  { value: 'off', label: 'Off', description: 'Generated frames are shown without an overlay.' },
  { value: 'flow', label: 'Motion flow', description: 'Overlays color-coded motion on generated frames, so you can see what the flow is tracking.' },
  { value: 'confidence', label: 'Confidence', description: 'Heat map of where generation fell back to the real image. Use it while tuning Artifact suppression and Motion limit.' },
  { value: 'tint', label: 'Generated-frame tint', description: 'Adds a faint tint to generated frames so you can confirm they are being shown.' },
]
const diagnosticDescription = computed(() => diagnosticViews.find((view) => view.value === framegen.value.diagnosticView)?.description ?? '')

// Live session status relayed by the layer.
const sessions = ref<RuntimeStatusSession[]>([])
const statusError = ref('')
let disposed = false
let poll: ReturnType<typeof setTimeout> | undefined
async function refresh() {
  try {
    const status = await loadRuntimeStatus()
    if (!disposed) { sessions.value = status.sessions; statusError.value = '' }
  } catch { if (!disposed) statusError.value = 'Runtime status is temporarily unavailable.' }
  if (!disposed) poll = setTimeout(refresh, 2000)
}
onMounted(refresh)
onUnmounted(() => { disposed = true; if (poll) clearTimeout(poll) })

function sessionStatus(session: RuntimeStatusSession): { label: string; chip: string } {
  const reason = session.state.framegenReason?.trim()
  const withReason = (label: string, separator = ' · ') => reason ? `${label}${separator}${reason}` : label
  switch (session.state.framegenState) {
    case 'active': return { label: withReason('Active'), chip: 'chip-success' }
    case 'waiting': return { label: withReason('Waiting for steady frames'), chip: 'chip-idle' }
    case 'paused': return { label: withReason('Paused'), chip: 'chip-idle' }
    case 'unavailable': return { label: withReason('Unavailable', ': '), chip: 'chip-warning' }
    case 'off': return { label: withReason('Off'), chip: 'chip-idle' }
    case undefined: return { label: 'Needs the updated VectorXR layer', chip: 'chip-idle' }
    default: return { label: withReason('Status unavailable'), chip: 'chip-idle' }
  }
}

// What the game's depth does for generated frames (Direct3D 12 reads it).
function depthStatus(session: RuntimeStatusSession): { label: string; chip: string } | null {
  switch (session.state.framegenDepth) {
    case 'in-use': return { label: 'Depth in use: cockpit detail is compensated exactly', chip: 'chip-success' }
    case 'checking': return { label: "Checking the game's depth against the other eye", chip: 'chip-idle' }
    case 'not-sent': return { label: 'No depth from this game: near cockpit detail may jitter, Interpolate fits best', chip: 'chip-idle' }
    case 'not-readable': return { label: 'Depth is only read in Direct3D 12 games for now', chip: 'chip-idle' }
    case 'empty': return { label: "The game's depth is empty: turn on its depth reprojection (MSFS: Motion reprojection = Depth)", chip: 'chip-warning' }
    case 'unusable': return { label: "The game's depth does not match the other eye, so it is not used", chip: 'chip-warning' }
    default: return null
  }
}

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto')
}

function openToggleBinding() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0
  bindingSubPageOpen.value = true
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }))
}

function closeToggleBinding() {
  bindingSubPageOpen.value = false
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }))
}

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  framegen.value.profiles.forEach((profile, index) => {
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
      warnings.set(index, [
        ...(warnings.get(index) ?? []),
        `${appName} is already targeted by Profile ${firstIndex + 1}. The first active profile wins.`,
      ])
    }
  })

  return warnings
})

function settingsSummary(settings: FrameGenSettings) {
  const mode = settings.synthesisMode === 'extrapolate' ? 'Extrapolate' : 'Interpolate'
  const quality = settings.flowQuality.charAt(0).toUpperCase() + settings.flowQuality.slice(1)
  const multiplier = settings.frameMultiplier === 'auto' ? 'Auto' : settings.frameMultiplier === 'dynamic' ? 'Dynamic' : settings.frameMultiplier
  return `${mode} · ${quality} · ${multiplier}`
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    module-label="Frame Generation"
    :binding="framegen.toggleBinding"
    label="Frame Generation A/B Toggle"
    description="Switch generated frames off and on while in-game to compare motion directly. It works only in sessions that started with Frame Generation on for the application."
    none-text="No binding assigned. Frame Generation runs wherever it is enabled when the game starts."
    :warnings="toggleBindingWarnings"
    @update:binding="framegen.toggleBinding = $event"
    @close="closeToggleBinding"
  />
  <div v-else class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-start justify-between gap-4">
        <div>
          <div class="flex flex-wrap items-center gap-2">
            <h2 class="text-2xl font-semibold tracking-tight">Frame Generation</h2>
            <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-warning">Experimental</span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Holds the game at half your headset's refresh rate or lower and adds optical-flow frames between real frames, so the headset still gets a full-rate image stream. Test it per game with the A/B toggle.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">i</span>
          How Frame Generation Works
        </button>
      </div>

      <div
        v-for="warning in turboWarnings"
        :key="warning"
        class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
        role="note"
      >
        {{ warning }}
      </div>

      <SetupGuide class="mb-3" title="Setup guide" summary="Enable it for one game, then compare in the headset" :open="setupGuideOpen">
        <ol class="list-decimal space-y-1 pl-5 text-sm leading-6">
          <li><strong>Turn off runtime smoothing:</strong> SteamVR Motion Smoothing, Pimax Smart Smoothing, Virtual Desktop SSW, or Meta ASW. They fight Frame Generation's pacing.</li>
          <li><strong>Keep the runtime at full refresh:</strong> don't halve the frame rate in Pimax Play or with a SteamVR fixed 45 fps setting. Frame Generation lowers the game's rate itself.</li>
          <li><strong>Enable it for one game:</strong> leave the Default Profile off, add a custom profile below, and assign the game. Frame multiplier Auto suits most games; for very heavy games, try Extrapolate with 3x, 4x, or Auto.</li>
          <li><strong>Save and restart the game.</strong> Turning Frame Generation on takes effect at the next launch.</li>
          <li><strong>Check it in the headset:</strong> show the Frame Generation state row in the <button class="underline underline-offset-2" type="button" @click="$emit('navigate', 'osd')">On-Screen Display</button>, or pick the Generated-frame tint diagnostic view.</li>
          <li><strong>Compare:</strong> assign the A/B toggle and switch generated frames off and on in the same scene.</li>
        </ol>
        <p class="mt-3 text-xs leading-5 text-muted">
          Works with Direct3D 11, Direct3D 12, and Vulkan games, in stereo or Quadviews. Turbo stays off in sessions where Frame Generation runs.
        </p>
      </SetupGuide>

      <p class="mb-4 flex items-start gap-2 text-xs leading-5 text-muted" role="note">
        <span class="restart-required-mark" aria-hidden="true">&#8635;</span>
        <span><strong class="font-semibold">Restart to apply:</strong> turning Frame Generation on for an application, through the Default Profile or a custom profile and its applications, takes effect the next time that VR application starts. Turning it off, and every other setting on this page, updates during play.</span>
      </p>

      <div class="mb-4 space-y-3 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-center gap-2">
          <p class="eyebrow mr-1 text-xs uppercase tracking-[0.18em]">In-headset controls</p>
          <span v-if="statusError" class="text-xs text-muted">{{ statusError }}</span>
          <span v-else-if="!sessions.length" class="chip-idle rounded-full px-2.5 py-1 text-xs">No VR application running</span>
          <template v-else>
            <template v-for="session in sessions" :key="session.sessionId">
              <span class="rounded-full px-2.5 py-1 text-xs" :class="sessionStatus(session).chip">
                <strong class="font-semibold">{{ session.application }}</strong> · {{ sessionStatus(session).label }}
              </span>
              <span v-if="depthStatus(session)" class="rounded-full px-2.5 py-1 text-xs" :class="depthStatus(session)!.chip">
                {{ depthStatus(session)!.label }}
              </span>
            </template>
          </template>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <ModuleBindingPanel
            heading="A/B toggle"
            :binding="framegen.toggleBinding"
            hint="Optional. Switches generated frames off and on in sessions that started with Frame Generation on."
            @edit="openToggleBinding"
          />
          <div class="rounded-[1rem] border p-4 surface-panel-soft">
            <label class="flex flex-wrap items-center justify-between gap-3">
              <span class="text-sm font-semibold tracking-tight">Diagnostic view</span>
              <select v-model="framegen.diagnosticView" class="app-input manual-input-select rounded-[0.75rem] px-3 py-1.5 text-sm">
                <option v-for="view in diagnosticViews" :key="view.value" :value="view.value">{{ view.label }}</option>
              </select>
            </label>
            <p class="mt-2 text-xs leading-5 text-muted">{{ diagnosticDescription }} Applies to every Frame Generation session and updates during play.</p>
          </div>
        </div>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
          <span v-if="framegen.enabled" class="ml-auto rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-accent">
            {{ settingsSummary(framegen.defaults) }}
          </span>
        </summary>
        <div class="mt-3 flex flex-wrap items-center gap-2">
          <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium" :title="restartNote">
            <input v-model="framegen.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            Default Profile {{ framegen.enabled ? 'On' : 'Off' }}
            <span class="restart-required-mark" :title="restartNote">&#8635;</span>
          </label>
          <DefaultProfileExclusions :applications="applications" :profiles="framegen.profiles" />
        </div>
        <p v-if="!framegen.enabled" class="mt-2 text-xs text-muted">
          Off unless an enabled custom profile turns it on. Custom profiles below still apply to their applications.
        </p>
        <template v-else>
          <div class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
            <strong>Broad enablement is active.</strong> Frame Generation applies to every application without a custom profile, including ones you have not tested. The safer setup is Default Off with custom profiles for the games you have checked.
          </div>
          <FrameGenSettingsFields class="mt-3" :settings="framegen.defaults" />
        </template>
      </details>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Turn Frame Generation on per application. The first enabled matching profile wins.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addFrameGenProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in framegen.profiles"
        :key="`framegen-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Frame Generation"
        :enabled-change-note="restartNote"
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeFrameGenProfile', index)"
        @sync-name="$emit('syncFrameGenProfileName', index)"
      >
        <template #badges>
          <span v-if="profile.enabled" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">{{ settingsSummary(profile.settings) }}</span>
        </template>

        <FrameGenSettingsFields :settings="profile.settings" />
      </ProfileShell>

      <div
        v-if="framegen.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to turn Frame Generation on for a specific application.
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="max-h-[85vh] w-full max-w-[720px] overflow-y-auto rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Frame Generation</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">How Frame Generation Works</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">Close</button>
        </div>

        <div class="mt-5 space-y-5 text-sm leading-6">
          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">What it does</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Frame Generation holds the game at half your headset's refresh rate or lower, for example 45 fps on a 90 Hz headset, and inserts generated frames between real frames. The headset still receives a full-rate image stream, while the game only has to render a fraction of it.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Generated frames come from GPU optical flow measured on the final images the game submits. VectorXR removes your head rotation from that flow first, because the runtime's own reprojection already handles head rotation, so the flow follows world, vehicle, and object motion.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Interpolate or Extrapolate</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Interpolate</strong> (default) blends generated frames between the previous and the newest real frame. It shows the fewest artifacts and is the approach closest to OFXR-Bridge, but each real frame must be ready before the generated frames leading up to it are shown, so it needs more GPU headroom at any multiplier.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Extrapolate</strong> pushes the newest real frame forward along its motion, like ASW or Smart Smoothing. The game gets more GPU time per frame, but fast motion and newly revealed areas show more edge artifacts. It is the better choice for heavy GPU-bound games.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Frame multiplier</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              The multiplier sets how many headset frames each real game frame covers. On a 90 Hz headset, <strong>2x</strong> runs the game at 45 fps with one generated frame between real frames, <strong>3x</strong> at 30 fps with two, and <strong>4x</strong> at 22.5 fps with three.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Auto</strong> (default) starts at full rate: while the game holds the headset refresh, frames pass straight through and the status reads Full rate. When it cannot, Auto steps up to 2x, 3x or 4x, and steps back down when it can, checking now and then.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Higher multipliers rescue very heavy games, such as MSFS at very high resolution, at a cost.</strong> Each generated frame moves content further, so artifacts grow, most at 4x. Use Extrapolate with 3x, 4x, or Auto for these games. In Extrapolate, each generated frame also needs its own set of headset-resolution images, so memory use grows with the multiplier.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Before you start</p>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Turn off runtime smoothing.</strong> SteamVR Motion Smoothing, Pimax Smart Smoothing, Virtual Desktop SSW, and Meta ASW fight Frame Generation's pacing. Disable them for games that use Frame Generation.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Keep the runtime at full refresh.</strong> Don't halve the frame rate in Pimax Play or with a SteamVR fixed 45 fps setting. Frame Generation lowers the game's rate itself and needs the headset running at its full refresh rate.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Turbo stays off while Frame Generation runs.</strong> The two have opposite pacing goals, so in a session where Frame Generation runs, Turbo yields and its status reads Paused for Frame Generation. Your Turbo profiles are not changed.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Works with Direct3D 11, Direct3D 12, and Vulkan games. It also works with Quadviews, both VectorXR's synthesized quad views and Varjo-native quad views, because generation runs on the final images sent to the headset. Turning it on needs a game restart; turning it off and every other setting apply live.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Current limits</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Frame Generation does not use game motion vectors or depth yet; that is planned. Motion is estimated from color images only, so fine detail, transparency, and repeating patterns can confuse it.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Cockpit text and gauges can shimmer.</strong> HUD elements baked into the 3D image are not masked yet, so they may wobble on generated frames. If warping is distracting, raise Artifact suppression or lower Motion limit.
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
