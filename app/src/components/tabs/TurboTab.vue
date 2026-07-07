<script setup lang="ts">
import { computed, ref } from 'vue'

import BindingEditor from '../BindingEditor.vue'
import ProfileShell from '../ProfileShell.vue'
import type { ActiveRuntimeInfo, OpenXrLayerSnapshot } from '../../lib/commands'
import type {
  RegisteredApplication,
  RuntimePacingObservation,
  TurboPacingMode,
  VectorXRConfig,
} from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
  runtimePacing: RuntimePacingObservation[]
  activeRuntime: ActiveRuntimeInfo | null
  layerSnapshot: OpenXrLayerSnapshot | null
}>()

const emit = defineEmits<{
  addTurboProfile: []
  removeTurboProfile: [index: number]
  syncTurboProfileName: [index: number]
  rediscoverRuntime: [runtimeName: string]
}>()

const howItWorksOpen = ref(false)

const pacingForced = computed(() => props.config.modules.turbo.pacingMode !== 'auto')

// Turbo counts as "in use" when the default profile is on or any enabled
// custom profile exists — the advisory should fire for either.
const turboInUse = computed(
  () => props.config.modules.turbo.enabled || props.config.modules.turbo.profiles.some((profile) => profile.enabled),
)

// Two frame-pacing layers fight each other: warn when OpenXR Toolkit is
// enabled anywhere while Turbo applies. (We cannot see whether OXRTK's own
// turbo is switched on, so this stays an advisory, not an error.)
const toolkitConflict = computed(() => {
  if (!turboInUse.value) {
    return false
  }
  return (props.layerSnapshot?.slices ?? []).some((slice) =>
    slice.layers.some((layer) => layer.enabled && layer.layerName.toLowerCase().includes('mbucchia_toolkit')),
  )
})

interface PacingRow {
  runtimeName: string
  runtimeVersion: string
  mode: RuntimePacingObservation['mode'] | null
  source: RuntimePacingObservation['source'] | null
  lastUsedUnixSeconds: number
  isActive: boolean
}

// Generic tokens that appear in nearly every OpenXR manifest path and would
// make the active-runtime match meaningless.
const genericTokens = new Set(['openxr', 'runtime', 'windows', 'program', 'files'])

function matchesActiveRuntime(runtimeName: string): boolean {
  const active = props.activeRuntime
  if (!active) {
    return false
  }
  const name = runtimeName.toLowerCase()
  if (active.name) {
    const activeName = active.name.toLowerCase()
    if (activeName.includes(name) || name.includes(activeName)) {
      return true
    }
  }
  const normalizedPath = active.manifestPath.toLowerCase().replace(/[^a-z0-9]+/g, '')
  return runtimeName
    .toLowerCase()
    .split(/[^a-z0-9]+/)
    .filter((token) => token.length >= 4 && !genericTokens.has(token))
    .some((token) => normalizedPath.includes(token))
}

const pacingRows = computed<PacingRow[]>(() => {
  const rows: PacingRow[] = props.runtimePacing.map((observation) => ({
    runtimeName: observation.runtimeName,
    runtimeVersion: observation.runtimeVersion,
    mode: observation.mode,
    source: observation.source,
    lastUsedUnixSeconds: observation.lastUsedUnixSeconds,
    isActive: matchesActiveRuntime(observation.runtimeName),
  }))
  // Pins for runtimes without an observation still deserve a row.
  for (const runtimeName of Object.keys(props.config.modules.turbo.runtimePins)) {
    if (!rows.some((row) => row.runtimeName === runtimeName)) {
      rows.push({
        runtimeName,
        runtimeVersion: '',
        mode: null,
        source: null,
        lastUsedUnixSeconds: 0,
        isActive: matchesActiveRuntime(runtimeName),
      })
    }
  }
  return rows.sort((lhs, rhs) => rhs.lastUsedUnixSeconds - lhs.lastUsedUnixSeconds)
})

const activeRuntimeLabel = computed(() => {
  const active = props.activeRuntime
  if (!active) {
    return ''
  }
  if (active.name) {
    return active.name
  }
  const file = active.manifestPath.split(/[\\/]/).pop() ?? ''
  return file.replace(/\.json$/i, '')
})

const activeRuntimeHasRow = computed(() => pacingRows.value.some((row) => row.isActive))

function pinValue(runtimeName: string): TurboPacingMode | 'auto' {
  return props.config.modules.turbo.runtimePins[runtimeName] ?? 'auto'
}

function setPin(runtimeName: string, value: string) {
  const pins = props.config.modules.turbo.runtimePins
  if (value === 'async' || value === 'sequenced') {
    pins[runtimeName] = value
  } else {
    delete pins[runtimeName]
  }
}

function rowModeLabel(row: PacingRow): string {
  const pinned = props.config.modules.turbo.runtimePins[row.runtimeName]
  if (pinned) {
    return pinned === 'async' ? 'Async' : 'Sequenced'
  }
  if (row.mode === 'async') {
    return 'Async'
  }
  if (row.mode === 'sequenced') {
    return 'Sequenced'
  }
  if (row.mode === 'unsupported') {
    return 'Not supported'
  }
  return 'Undiscovered'
}

function rowBadge(row: PacingRow): string {
  if (props.config.modules.turbo.runtimePins[row.runtimeName]) {
    return 'Pinned'
  }
  if (row.mode === 'unsupported') {
    return 'Suspended'
  }
  if (row.source === 'preset') {
    return 'Preset'
  }
  if (row.source === 'discovered') {
    return 'Discovered'
  }
  return ''
}

function formatDate(unixSeconds: number): string {
  if (!unixSeconds) {
    return '—'
  }
  return new Date(unixSeconds * 1000).toLocaleDateString()
}
</script>

<template>
  <div class="space-y-4">
    <!-- Module header + defaults -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <h2 class="text-2xl font-semibold tracking-tight">Turbo</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Frees your game from the headset runtime's frame pacing so the GPU sets the pace. On some systems the runtime throttles frames at an unfortunate moment, capping fps well below what the hardware can deliver — Turbo removes that wait. If your fps is already stable, Turbo changes nothing worth having; it is a targeted fix, not a general boost.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="howItWorksOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          How Turbo Works
        </button>
      </div>

      <div
        v-if="toolkitConflict"
        class="mb-5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
        style="border-color: var(--app-border)"
      >
        OpenXR Toolkit is enabled alongside VectorXR Turbo. Two frame-pacing layers fight each other — if OXRTK's own Turbo Mode is switched on, disable it there or here, never both.
      </div>

      <!-- Module-level binding — applies regardless of which profile enabled turbo -->
      <BindingEditor
        :model-value="config.modules.turbo.toggleBinding"
        class="mb-5"
        label="Turbo Toggle Binding"
        description="Flip Turbo on and off while in-game to compare fps and frame feel directly. Turbo starts enabled whenever it applies to the running application. Note: switching Turbo on mid-session can cause a brief hitch while the frame pipeline re-synchronizes."
        none-text="No binding assigned. Turbo stays on for applications it applies to."
        default-activate-sound="turbo-on.wav"
        default-deactivate-sound="turbo-off.wav"
        @update:model-value="config.modules.turbo.toggleBinding = $event"
      />

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
        </summary>

        <label class="pill-toggle mt-3 inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="config.modules.turbo.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          Default Profile {{ config.modules.turbo.enabled ? 'On' : 'Off' }}
        </label>

        <div v-if="!config.modules.turbo.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          The default profile is off — Turbo does not apply to applications without an enabled custom profile. Enabled custom profiles below still turn Turbo on for their assigned applications.
        </div>
      </details>
    </article>

    <!-- Frame pacing strategy -->
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="mb-3">
        <h2 class="text-lg font-semibold tracking-tight">Frame Pacing</h2>
        <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
          How Turbo sequences the runtime's frame wait. Auto picks the right strategy per runtime and remembers what works — leave it on Auto unless you are debugging.
        </p>
      </div>

      <label class="flex flex-wrap items-center gap-3 text-sm font-medium">
        Pacing mode
        <select
          v-model="config.modules.turbo.pacingMode"
          class="rounded-[0.6rem] border px-3 py-1.5 text-sm surface-panel-strong"
          style="border-color: var(--app-border)"
        >
          <option value="auto">Auto (recommended)</option>
          <option value="async">Async — always</option>
          <option value="sequenced">Sequenced — always</option>
        </select>
      </label>
      <p class="mt-2 text-xs leading-5 text-muted">
        <template v-if="!pacingForced">
          Async overlaps the runtime wait with the next frame's work; Sequenced performs it right after each submit for runtimes that interlock the wait with submission (Oculus, Varjo, Pimax). Auto probes async first and falls back automatically.
        </template>
        <template v-else>
          Forced mode: this strategy is used on every runtime. Per-runtime discovery and pins below are paused, and Turbo suspends itself (instead of adapting) if the runtime cannot keep pace.
        </template>
      </p>

      <div class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-2 flex flex-wrap items-center justify-between gap-2">
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Runtimes</span>
          <span v-if="activeRuntimeLabel" class="text-xs text-muted">
            Active runtime: <span class="font-medium">{{ activeRuntimeLabel }}</span>
          </span>
        </div>

        <div v-if="pacingRows.length === 0" class="rounded-[0.9rem] border border-dashed px-4 py-4 text-sm text-muted surface-panel-soft">
          No runtimes observed yet — discoveries appear after Turbo's first session on each runtime.
        </div>

        <div v-else class="overflow-x-auto" :class="pacingForced ? 'opacity-50 pointer-events-none' : ''">
          <table class="w-full text-sm">
            <thead>
              <tr class="text-left text-xs uppercase tracking-wide text-muted">
                <th class="py-2 pr-3 font-medium">Runtime</th>
                <th class="py-2 pr-3 font-medium">Mode</th>
                <th class="py-2 pr-3 font-medium">Source</th>
                <th class="py-2 pr-3 font-medium">Last used</th>
                <th class="py-2 pr-3 font-medium">Override</th>
                <th class="py-2 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              <tr
                v-for="row in pacingRows"
                :key="row.runtimeName"
                class="border-t"
                style="border-color: var(--app-border)"
              >
                <td class="py-2.5 pr-3">
                  <span :class="row.isActive ? 'font-semibold' : ''">{{ row.runtimeName }}</span>
                  <span v-if="row.runtimeVersion" class="ml-1 text-xs text-muted">{{ row.runtimeVersion }}</span>
                  <span
                    v-if="row.isActive"
                    class="ml-2 rounded-full border px-2 py-0.5 text-[0.65rem] uppercase tracking-wide"
                    style="border-color: var(--app-border)"
                  >Active</span>
                </td>
                <td class="py-2.5 pr-3">{{ rowModeLabel(row) }}</td>
                <td class="py-2.5 pr-3">
                  <span
                    v-if="rowBadge(row)"
                    class="rounded-full border px-2 py-0.5 text-xs"
                    style="border-color: var(--app-border)"
                  >{{ rowBadge(row) }}</span>
                </td>
                <td class="py-2.5 pr-3 text-muted">{{ formatDate(row.lastUsedUnixSeconds) }}</td>
                <td class="py-2.5 pr-3">
                  <select
                    :value="pinValue(row.runtimeName)"
                    class="rounded-[0.5rem] border px-2 py-1 text-xs surface-panel-strong"
                    style="border-color: var(--app-border)"
                    @change="setPin(row.runtimeName, ($event.target as HTMLSelectElement).value)"
                  >
                    <option value="auto">Auto</option>
                    <option value="async">Pin Async</option>
                    <option value="sequenced">Pin Sequenced</option>
                  </select>
                </td>
                <td class="py-2.5 text-right">
                  <button
                    v-if="row.source"
                    class="button-secondary rounded-[0.5rem] px-2.5 py-1 text-xs"
                    type="button"
                    title="Clear this runtime's recorded verdict so Auto probes it again at the next session"
                    @click="emit('rediscoverRuntime', row.runtimeName)"
                  >
                    Re-discover
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>

        <p v-if="pacingForced && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          Forced mode in effect for all runtimes — pins and discovery are paused. Switch back to Auto to use them.
        </p>
        <p v-else-if="activeRuntimeLabel && !activeRuntimeHasRow && pacingRows.length > 0" class="mt-2 text-xs text-muted">
          No verdict yet for {{ activeRuntimeLabel }} — Auto will discover it on the next Turbo session.
        </p>
      </div>
    </article>

    <!-- Custom Profiles -->
    <section class="space-y-3">
      <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Enable Turbo for specific applications while leaving the default off.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addTurboProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.turbo.profiles"
        :key="profile.id"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Turbo"
        @remove="$emit('removeTurboProfile', index)"
        @sync-name="$emit('syncTurboProfileName', index)"
      >
        <div class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          Turbo is on for this profile's applications. There is nothing further to tune — pacing is either overridden or it isn't.
        </div>
      </ProfileShell>

      <div
        v-if="config.modules.turbo.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to enable Turbo for a specific application.
      </div>
    </section>

    <div v-if="howItWorksOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Turbo Mode</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">How Turbo Works</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="howItWorksOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Normally, an OpenXR game must wait for the headset runtime to say "start the next frame now." Some runtime and GPU combinations time that signal badly, forcing the game to idle and capping fps below what the hardware can do.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            With Turbo, VectorXR performs that wait out of the game's way and lets it begin its next frame immediately after submitting the previous one — one frame of pipelining. The headset runtime still receives a fully correct frame sequence; only the game's pacing is decoupled.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Runtimes disagree about where that wait may happen. <strong>Async</strong> pacing runs it in the background, overlapping it with the game's next frame — the most effective form, proven on SteamVR. <strong>Sequenced</strong> pacing performs it right after each frame submit, which is what runtimes like Oculus, Varjo, and Pimax require. <strong>Auto</strong> starts from what is known about your runtime, verifies it against live frame timing, adapts if needed, and remembers the verdict per runtime — so the right strategy is applied from the first frame of your next session.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Trade-offs: frame timing prediction becomes slightly less accurate, which can show up as marginally increased latency or judder in some titles. Turning Turbo on mid-session can occasionally cause a one-second hitch while the frame pipeline re-synchronizes.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Turbo works alongside all of VectorXR's other enhancements, including the Varjo quadviews compatibility path — VectorXR sequences its frame handling so the two never conflict.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 chip-warning" style="border-color: var(--app-border)">
            Safety net: if frame pacing stalls even after Auto has adapted, VectorXR suspends Turbo for the session (you'll hear the turbo-off cue) and records the runtime as unsupported so the stutter never replays on launch. Press the toggle binding to retry — a clean run overwrites the verdict.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            Tip: assign the optional Turbo Toggle binding and flip it in-game while watching an fps counter. If Turbo does not clearly help a title, leave it off there.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
