<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from "vue";

import {
  loadRuntimeStatus,
  setRuntimeQuadViewsDiagnosticVisualization,
  type RuntimeStatusSession,
} from "../../lib/commands";

import ModuleBindingPage from "../ModuleBindingPage.vue";
import ModuleBindingPanel from "../ModuleBindingPanel.vue";
import ProfileShell from "../ProfileShell.vue";
import QuadViewsOverlayGuide from "../QuadViewsOverlayGuide.vue";
import QuadViewsSettingsFields from "../QuadViewsSettingsFields.vue";
import {
  savedBindingConflictWarnings,
  type QuadViewsSettings,
  type RegisteredApplication,
  type VectorXRConfig,
} from "../../lib/model";

const varjoCompatibilityInfoOpen = ref(false);
const overlayGuideOpen = ref(false);
const bindingSubPageOpen = ref(false);
let savedScrollTop = 0;
const runtimeSessions = ref<RuntimeStatusSession[]>([]);
const runtimeStatusError = ref("");
const runtimeCommandPending = ref(false);
let runtimeStatusPoll: number | undefined;

const props = defineProps<{
  config: VectorXRConfig;
  applications: RegisteredApplication[];
}>();

defineEmits<{
  addQuadViewsProfile: [];
  removeQuadViewsProfile: [index: number];
  syncQuadViewsProfileName: [index: number];
}>();

const diagnosticBindingWarnings = computed(() => savedBindingConflictWarnings(props.config, [
  props.config.modules.quadviews.diagnosticVisualizationBinding,
]));

function pageScroller(): Element | null {
  return document.querySelector('main section.overflow-y-auto');
}

function openDiagnosticBinding() {
  savedScrollTop = pageScroller()?.scrollTop ?? 0;
  bindingSubPageOpen.value = true;
  void nextTick(() => pageScroller()?.scrollTo({ top: 0 }));
}

function closeDiagnosticBinding() {
  bindingSubPageOpen.value = false;
  void nextTick(() => pageScroller()?.scrollTo({ top: savedScrollTop }));
}

const diagnosticRuntime = computed(() =>
  runtimeSessions.value.find((session) => session.capabilities.quadviewsDiagnosticVisualization),
);

const diagnosticRuntimeSummary = computed(() => {
  const session = diagnosticRuntime.value;
  if (session) {
    return `${session.state.quadviewsDiagnosticVisualization ? "Shown" : "Hidden"} in ${session.application}`;
  }
  if (runtimeSessions.value.length > 0) {
    return "Unavailable in the active OpenXR session";
  }
  return "No active synthesized Quadviews session";
});

async function refreshRuntimeStatus() {
  try {
    runtimeSessions.value = (await loadRuntimeStatus()).sessions;
    runtimeStatusError.value = "";
  } catch (error) {
    runtimeStatusError.value = error instanceof Error ? error.message : "Unable to read runtime status";
  }
}

async function setDiagnosticVisualization() {
  const session = diagnosticRuntime.value;
  if (!session || runtimeCommandPending.value) return;

  const desired = !session.state.quadviewsDiagnosticVisualization;
  runtimeCommandPending.value = true;
  runtimeStatusError.value = "";
  try {
    const revision = await setRuntimeQuadViewsDiagnosticVisualization(session.sessionId, desired);
    for (let attempt = 0; attempt < 12; attempt += 1) {
      await new Promise<void>((resolve) => window.setTimeout(resolve, 150));
      await refreshRuntimeStatus();
      const updated = runtimeSessions.value.find((candidate) => candidate.sessionId === session.sessionId);
      if (updated && updated.acknowledgedRevision >= revision &&
          updated.state.quadviewsDiagnosticVisualization === desired) {
        return;
      }
    }
    throw new Error("The OpenXR session did not acknowledge the control request");
  } catch (error) {
    runtimeStatusError.value = error instanceof Error ? error.message : "Unable to control the visualization";
  } finally {
    runtimeCommandPending.value = false;
  }
}

onMounted(() => {
  void refreshRuntimeStatus();
  runtimeStatusPoll = window.setInterval(() => void refreshRuntimeStatus(), 1_000);
});

onUnmounted(() => {
  if (runtimeStatusPoll !== undefined) window.clearInterval(runtimeStatusPoll);
});

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>();
  const firstProfileByApplication = new Map<string, number>();
  const applicationNameById = new Map(
    props.applications.map((application) => [application.id, application.name]),
  );

  props.config.modules.quadviews.profiles.forEach((profile, index) => {
    if (!profile.enabled) {
      return;
    }

    for (const applicationId of profile.applicationIds) {
      const firstIndex = firstProfileByApplication.get(applicationId);
      if (firstIndex === undefined) {
        firstProfileByApplication.set(applicationId, index);
        continue;
      }

      const appName = applicationNameById.get(applicationId) ?? applicationId;
      warnings.set(index, [
        ...(warnings.get(index) ?? []),
        `${appName} is already targeted by Profile ${firstIndex + 1}. The first active profile wins.`,
      ]);
    }
  });

  return warnings;
});

function estimatedPixelBudget(settings: QuadViewsSettings) {
  const focusWidthScale =
    (settings.focusScale *
      Math.min(Math.max(settings.focusHorizontalSizePercent, 1), 100)) /
    100;
  const focusHeightScale =
    (settings.focusScale *
      Math.min(Math.max(settings.focusVerticalSizePercent, 1), 100)) /
    100;
  const budget =
    settings.peripheralScale * settings.peripheralScale +
    focusWidthScale * focusHeightScale;

  return Math.max(0, budget * 100);
}

function budgetLabel(settings: QuadViewsSettings) {
  return `${estimatedPixelBudget(settings).toFixed(1)}% estimated app render pixels`;
}

type BudgetTone = "light" | "moderate" | "heavy" | "detrimental";

function budgetToneKey(settings: QuadViewsSettings): BudgetTone {
  const budget = estimatedPixelBudget(settings);

  if (budget > 100) {
    return "detrimental";
  }
  if (budget <= 45) {
    return "light";
  }
  if (budget <= 85) {
    return "moderate";
  }
  return "heavy";
}

function budgetTone(settings: QuadViewsSettings) {
  const labels: Record<BudgetTone, string> = {
    light: "Light",
    moderate: "Moderate",
    heavy: "Heavy",
    detrimental: "⚠ Detrimental",
  };
  return labels[budgetToneKey(settings)];
}

function budgetChipClass(settings: QuadViewsSettings) {
  const classes: Record<BudgetTone, string> = {
    light: "chip-success",
    moderate: "chip-warning",
    heavy: "chip-danger",
    detrimental: "chip-danger quadviews-budget-alert",
  };
  return classes[budgetToneKey(settings)];
}
</script>

<template>
  <ModuleBindingPage
    v-if="bindingSubPageOpen"
    module-label="Quadviews"
    :binding="config.modules.quadviews.diagnosticVisualizationBinding"
    label="Diagnostic Visualization Toggle"
    description="Show or hide the Quadviews calibration view while in-game. It starts hidden each session and is available only when VectorXR synthesizes Quadviews through its D3D11 compositor."
    none-text="No in-headset shortcut assigned. You can still control the visualization from the Quadviews page."
    :warnings="diagnosticBindingWarnings"
    @update:binding="config.modules.quadviews.diagnosticVisualizationBinding = $event"
    @close="closeDiagnosticBinding"
  />
  <div v-else class="space-y-4">
    <article
      class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel"
    >
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <div class="flex flex-wrap items-center gap-3">
            <h2 class="text-2xl font-semibold tracking-tight">Quadviews</h2>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Configure foveal and peripheral rendering for games that already
            request OpenXR quad views. Enabling this module does not add
            quad-view support to ordinary stereo games.
          </p>
          <p class="mt-2 max-w-3xl text-sm font-bold leading-6 text-muted">
            DCS is the primary tested title. Likely, but unconfirmed, D3D11 candidates include Pavlov VR, VAIL VR, The 7th Guest VR, and Kayak VR: Mirage. Each title must request quad views itself.
          </p>
          <p class="mt-1 max-w-3xl text-xs leading-5 text-muted">
            Pixel estimates compare application view rendering only; the runtime and VectorXR composite have additional GPU cost.
          </p>
        </div>
        <button
          class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="varjoCompatibilityInfoOpen = true"
        >
          <span class="inline-flex h-5 w-5 items-center justify-center rounded-full border text-xs" style="border-color: var(--app-border)">
            i
          </span>
          Varjo Compatibility
        </button>
      </div>

      <div class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
        <strong>Recommended DCS setup</strong>
        <ul class="mt-1.5 list-disc space-y-1 pl-5">
          <li><strong>Game:</strong> turn on <strong>Use Quad View</strong> and, for gaze tracking, <strong>Use Eye Tracking</strong>.</li>
          <li><strong>Provider:</strong> enable the VectorXR layer and Quadviews profile, then disable <code>XR_APILAYER_MBUCCHIA_quad_views_foveated</code>. On Pimax, also turn Native Pimax Quad Views off. Native Varjo Quadviews remains runtime-driven.</li>
          <li><strong>Apply:</strong> save the profile and restart DCS after changing runtime, layer, or in-game VR settings.</li>
        </ul>
      </div>

      <div class="mb-4 flex items-start gap-2.5 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong" role="note">
        <span class="restart-required-mark mt-0.5" aria-hidden="true">&#8635;</span>
        <p><strong>Restart to apply:</strong> marked fields and Quadviews on/off take full effect after the current OpenXR application exits. Unmarked controls update during play.</p>
      </div>

      <div class="mb-4 space-y-3 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.18em]">In-headset diagnostics</p>
            <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
              See render zones, focus alignment, gaze filtering, and tracking availability while you tune Quadviews.
            </p>
          </div>
          <button class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-3.5 py-2 text-sm font-semibold" type="button" @click="overlayGuideOpen = true">
            <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.8">
              <path d="M10 2.5a7.5 7.5 0 1 0 0 15 7.5 7.5 0 0 0 0-15Zm0 4v4.25m0 2.75v.1" stroke-linecap="round" />
            </svg>
            How to read the overlay
          </button>
        </div>
        <div class="grid gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(19rem,0.8fr)]">
          <div class="rounded-[0.9rem] border p-4 surface-panel-strong">
            <div class="flex flex-wrap items-center justify-between gap-3">
              <div>
                <p class="text-sm font-semibold">Session control</p>
                <p class="mt-1 text-xs leading-5 text-muted">{{ diagnosticRuntimeSummary }}</p>
              </div>
              <button
                class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
                type="button"
                :disabled="!diagnosticRuntime || runtimeCommandPending"
                @click="setDiagnosticVisualization"
              >
                {{ runtimeCommandPending
                  ? "Applying..."
                  : diagnosticRuntime?.state.quadviewsDiagnosticVisualization
                    ? "Hide visualization"
                    : "Show visualization" }}
              </button>
            </div>
            <p v-if="runtimeStatusError" class="mt-2 text-xs chip-danger">{{ runtimeStatusError }}</p>
          </div>
          <ModuleBindingPanel
            heading="In-headset shortcut"
            :binding="config.modules.quadviews.diagnosticVisualizationBinding"
            hint="Optional. The visualization starts hidden each OpenXR session."
            @edit="openDiagnosticBinding"
          />
        </div>
        <p class="text-xs leading-5 text-muted">New to the visualization? The interactive guide explains each shape and maps common symptoms to the setting that controls them.</p>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
          <span class="ml-auto flex items-center gap-2">
            <span
              class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="budgetChipClass(config.modules.quadviews.defaults)"
            >
              {{ budgetTone(config.modules.quadviews.defaults) }}
            </span>
            <span class="text-xs font-semibold">{{ budgetLabel(config.modules.quadviews.defaults) }}</span>
          </span>
        </summary>
        <div class="mt-3 flex flex-wrap items-center gap-2">
          <label
            class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
            title="Saved immediately, but a running OpenXR application keeps its launch-time Quadviews state until it exits."
          >
            <input v-model="config.modules.quadviews.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            Default Profile {{ config.modules.quadviews.enabled ? "On" : "Off" }}
            <span
              class="restart-required-mark"
              title="Quadviews enable/disable changes apply after the running OpenXR application exits."
            >&#8635;</span>
          </label>
        </div>
        <div v-if="!config.modules.quadviews.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
          The default profile is off and has no effect — applications without an enabled custom profile get no Quadviews. Enabled custom profiles below still apply to their assigned applications.
        </div>
        <QuadViewsSettingsFields v-else class="mt-3" :settings="config.modules.quadviews.defaults" />
      </details>
    </article>

    <section class="space-y-3">
      <div
        class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel"
      >
        <div>
          <h2 class="text-lg font-semibold tracking-tight">Custom Profiles</h2>
          <p class="text-sm text-muted">Override Quadviews per application. The first enabled matching profile wins.</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addQuadViewsProfile')"
        >
          Add Profile
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.quadviews.profiles"
        :key="`quadviews-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Quadviews"
        enabled-change-note="Saved immediately, but a running OpenXR application keeps its launch-time Quadviews state until it exits."
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeQuadViewsProfile', index)"
        @sync-name="$emit('syncQuadViewsProfileName', index)"
      >
        <template #badges>
          <span
            v-if="profile.enabled"
            class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
            :class="budgetChipClass(profile.settings)"
            >{{ budgetTone(profile.settings) }}</span
          >
          <span v-if="profile.enabled" class="text-xs font-semibold">{{ budgetLabel(profile.settings) }}</span>
        </template>

        <QuadViewsSettingsFields :settings="profile.settings" />
      </ProfileShell>

      <div
        v-if="config.modules.quadviews.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        No custom profiles yet. Add a profile to override quadview values for a
        specific application.
      </div>
    </section>

    <QuadViewsOverlayGuide :open="overlayGuideOpen" @close="overlayGuideOpen = false" />

    <div v-if="varjoCompatibilityInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">Varjo Compatibility</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">Varjo Compatible Quadviews</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="varjoCompatibilityInfoOpen = false">
            Close
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-accent" style="border-color: var(--app-border)">
            Varjo headsets are bi-panel: a wide, lower-density context display plus a small, high-density focus display per eye. VectorXR's Varjo compatible quadviews keeps the four view layers your headset expects (two peripheral, two focus) so the physical focus panels are driven directly. As a consequence, the Varjo runtime takes over the view geometry, and some Quadviews settings are ignored.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            VectorXR Quadviews &amp; Varjo runtime compatibility notes:
            <div class="mt-3 grid gap-3 sm:grid-cols-2">
              <div class="rounded-[0.9rem] border p-3" style="border-color: var(--app-border)">
                <div class="text-xs font-semibold uppercase tracking-[0.16em] text-muted">VectorXR Still applies</div>
                <ul class="mt-1.5 space-y-1">
                  <li>Focus Scale </li>
                  <li>Peripheral Scale</li>
                  <li>Focus Sharpness</li>
                </ul>
              </div>
              <div class="rounded-[0.9rem] border p-3" style="border-color: var(--app-border)">
                <div class="text-xs font-semibold uppercase tracking-[0.16em] text-muted">Handled by the Varjo runtime</div>
                <ul class="mt-1.5 space-y-1">
                  <li>Focus Size &amp; Transition Thickness</li>
                  <li>Horizontal &amp; Vertical Offset</li>
                  <li>Gaze Smoothing &amp; Deadzone</li>
                  <li>Diagnostic Visualization</li>
                </ul>
              </div>
            </div>
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            On headsets without physical quad-view support through bi-panel displays (e.g. Quest Pro, Pimax Crystal), VectorXR Quadviews uses the standard emulation mode and all settings apply. These compatibility notes apply only to Varjo runtimes.
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel" role="note">
            <strong>Diagnostic visualization is not currently supported on Varjo headsets.</strong> Varjo's runtime owns the native quad-view composition, so VectorXR's synthesized D3D11 compositor does not run there.
          </div>
        </div>
      </div>
    </div>
  </div>
</template>
