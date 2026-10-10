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

const restartNote = 'Turning Frame Generation on takes effect the next time the game starts; turning it off applies at once.'

const diagnosticViews: { value: FrameGenDiagnosticView; label: string; description: string }[] = [
  { value: 'off', label: 'Off', description: 'Generated frames are shown as they are.' },
  { value: 'flow', label: 'Motion flow', description: 'Color-codes the motion Frame Generation measured on generated frames.' },
  { value: 'confidence', label: 'Confidence', description: 'Shows where generated frames fell back to the real image; useful while tuning the Advanced settings.' },
  { value: 'tint', label: 'Generated-frame tint', description: 'Tints generated frames faintly, so you can see they are shown.' },
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

// The layer's state and a short reason, in the words the OSD uses.
function sessionStatus(session: RuntimeStatusSession): { label: string; chip: string } {
  const reason = session.state.framegenReason?.trim()
  const withReason = (label: string, separator = ' · ') => reason ? `${label}${separator}${reason}` : label
  switch (session.state.framegenState) {
    case 'active': return { label: withReason('On'), chip: 'chip-success' }
    case 'standby': return { label: withReason('Standby'), chip: 'chip-idle' }
    case 'waiting': return { label: withReason('Starting · waiting for steady frames'), chip: 'chip-idle' }
    case 'paused': return { label: withReason('Paused'), chip: 'chip-warning' }
    case 'unavailable': return { label: withReason('Unavailable', ': '), chip: 'chip-warning' }
    case 'off': return { label: withReason('Off'), chip: 'chip-idle' }
    case undefined: return { label: 'Needs the updated VectorXR layer', chip: 'chip-idle' }
    default: return { label: withReason('Status unavailable'), chip: 'chip-idle' }
  }
}

// The game's depth helps generated frames keep near objects steady as the
// head moves. Shown only while it is in use or when the player can act on it.
function depthStatus(session: RuntimeStatusSession): { label: string; title: string; chip: string } | null {
  switch (session.state.framegenDepth) {
    case 'in-use': return { label: 'Using game depth', title: 'The game shares its depth, so near objects stay steady as you move your head.', chip: 'chip-success' }
    case 'empty': return { label: 'Game depth is empty', title: 'The game sends depth without filling it. If the game has a depth-based reprojection option, turning it on lets Frame Generation use it.', chip: 'chip-warning' }
    case 'unusable': return { label: 'Game depth not used', title: 'The depth the game sends does not line up between the eyes, so Frame Generation works without it.', chip: 'chip-idle' }
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
  const rate = settings.frameMultiplier === 'dynamic' ? 'Dynamic' : settings.frameMultiplier === 'auto' ? 'Fixed Auto' : `Fixed ${settings.frameMultiplier}`
  const motion = settings.motionEstimation === 'hardware' ? 'NVIDIA hardware' : 'Shader'
  return `${mode} · ${rate} · ${motion}`
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    module-label="Frame Generation"
    :binding="framegen.toggleBinding"
    label="Frame Generation A/B Toggle"
    description="Switches generated frames off and on in the headset, to compare motion in the same scene. Works in games that started with Frame Generation on."
    none-text="No binding assigned. Frame Generation runs wherever it is on when the game starts."
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
            Adds generated frames between the game's frames, so motion stays smooth at your headset's full refresh rate even when the game can't keep up. Try it per game and compare with the A/B toggle.
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

      <SetupGuide class="mb-3" title="Setup guide" summary="Turn it on for one game, then compare in the headset" :open="setupGuideOpen">
        <ol class="list-decimal space-y-1 pl-5 text-sm leading-6">
          <li><strong>Turn off runtime smoothing</strong> such as SteamVR Motion Smoothing, Pimax Smart Smoothing, Virtual Desktop SSW or Meta ASW. It fights Frame Generation.</li>
          <li><strong>Keep the headset at its full refresh rate.</strong> Don't use a runtime's half-rate or fixed 45 fps mode; Frame Generation sets the game's rate itself.</li>
          <li><strong>Turn it on for one game:</strong> leave the Default Profile off, add a custom profile below and assign the game. The default settings suit most games.</li>
          <li><strong>Save and restart the game.</strong> Turning Frame Generation on takes effect at the next launch.</li>
          <li><strong>Check it in the headset:</strong> the <button class="underline underline-offset-2" type="button" @click="$emit('navigate', 'osd')">On-Screen Display</button> shows FG ON next to its FPS, or pick the Generated-frame tint diagnostic view.</li>
          <li><strong>Compare:</strong> assign the A/B toggle and switch generated frames off and on in the same scene.</li>
        </ol>
        <p class="mt-3 text-xs leading-5 text-muted">
          Turbo stays off in games where Frame Generation runs.
        </p>
      </SetupGuide>

      <p class="mb-4 flex items-start gap-2 text-xs leading-5 text-muted" role="note">
        <span class="restart-required-mark" aria-hidden="true">&#8635;</span>
        <span><strong class="font-semibold">Restart to apply:</strong> turning Frame Generation on for a game takes effect the next time the game starts. Turning it off, and every other setting on this page, applies during play.</span>
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
              <span v-if="depthStatus(session)" class="rounded-full px-2.5 py-1 text-xs" :class="depthStatus(session)!.chip" :title="depthStatus(session)!.title">
                {{ depthStatus(session)!.label }}
              </span>
            </template>
          </template>
        </div>
        <div class="grid gap-3 lg:grid-cols-2">
          <ModuleBindingPanel
            heading="A/B toggle"
            :binding="framegen.toggleBinding"
            hint="Optional. Switches generated frames off and on in games that started with Frame Generation on."
            @edit="openToggleBinding"
          />
          <div class="rounded-[1rem] border p-4 surface-panel-soft">
            <label class="flex flex-wrap items-center justify-between gap-3">
              <span class="text-sm font-semibold tracking-tight">Diagnostic view</span>
              <select v-model="framegen.diagnosticView" class="app-input manual-input-select rounded-[0.75rem] px-3 py-1.5 text-sm">
                <option v-for="view in diagnosticViews" :key="view.value" :value="view.value">{{ view.label }}</option>
              </select>
            </label>
            <p class="mt-2 text-xs leading-5 text-muted">{{ diagnosticDescription }} Applies to every game and updates during play.</p>
          </div>
        </div>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to games without an enabled custom profile</span>
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
          Off for games without a custom profile. Custom profiles below still turn it on for their games.
        </p>
        <template v-else>
          <div class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning" style="border-color: var(--app-border)">
            <strong>On for every game without a custom profile,</strong> including ones you haven't tried it with. The safer setup is to leave this off and add a profile for each game you've checked.
          </div>
          <FrameGenSettingsFields class="mt-3" :settings="framegen.defaults" />
        </template>
      </details>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Turn Frame Generation on per game. The first enabled matching profile wins.</p>
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
        No custom profiles yet. Add a profile to turn Frame Generation on for a game.
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
              Frame Generation measures how the image moves between the game's frames and creates new frames from that motion, so the headset gets a fresh image every refresh even when the game renders fewer. Your head rotation is taken out first, because the headset's own reprojection already handles it.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Mode</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Interpolate</strong> (default) blends between the last two game frames. It looks smoothest and has the fewest artifacts, but shows each game frame a little later.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Extrapolate</strong> projects the newest game frame forward along its motion, so it adds no latency. Fast motion and newly revealed areas show more artifacts; it looks best with NVIDIA hardware as the motion source.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Frame rate</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Dynamic</strong> (default) lets the game run as fast as it can and fills the refreshes between its frames with generated ones. When the game keeps up on its own, Frame Generation stands by and generates nothing.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Fixed</strong> holds the game at a set fraction of the refresh rate for even pacing. On a 90 Hz headset, <strong>2x</strong> runs the game at 45 fps, <strong>3x</strong> at 30 fps and <strong>4x</strong> at 22.5 fps; <strong>Auto</strong> picks among them as the game needs. Each generated frame moves the image further, so higher rates show more artifacts.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Before you start</p>
            <div class="rounded-[1rem] border px-4 py-3 chip-warning" style="border-color: var(--app-border)">
              <strong>Turn off runtime smoothing</strong> such as SteamVR Motion Smoothing, Pimax Smart Smoothing, Virtual Desktop SSW and Meta ASW, and keep the headset at its full refresh rate. Frame Generation sets the game's rate itself.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              <strong>Turbo stays off while Frame Generation runs.</strong> The two pace frames in opposite ways, so Turbo yields in those games and reads Paused for Frame Generation. Your Turbo profiles are not changed.
            </div>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Works with Direct3D 11, Direct3D 12 and Vulkan games, including with Quadviews.
            </div>
          </section>

          <section class="space-y-3">
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Known limits</p>
            <div class="rounded-[1rem] border px-4 py-3 surface-panel">
              Motion is measured from the images the game shows, so text, HUDs, transparency and repeating patterns can shimmer or warp on generated frames. If it is distracting, raise Artifact suppression or lower Motion limit under Advanced.
            </div>
          </section>
        </div>
      </div>
    </div>
  </div>
</template>
