<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref } from "vue";

import {
  loadRuntimeStatus,
  setRuntimeQuadViewsDiagnosticVisualization,
  type RuntimeStatusSession,
} from "../../lib/commands";
import { errorText } from "../../lib/errors";
import { t } from "../../i18n/index.ts";

import DefaultProfileExclusions from "../DefaultProfileExclusions.vue";
import I18nText from "../I18nText.vue";
import ModuleBindingPage from "../ModuleBindingPage.vue";
import ModuleBindingPanel from "../ModuleBindingPanel.vue";
import ProfileShell from "../ProfileShell.vue";
import SetupGuide from "../SetupGuide.vue";
import QuadViewsOverlayGuide from "../QuadViewsOverlayGuide.vue";
import QuadViewsSettingsFields from "../QuadViewsSettingsFields.vue";
import QuadViewsDimensions from "../QuadViewsDimensions.vue";
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

// Open the setup guide for first-time setup; returning users with an enabled profile start with it closed.
const setupGuideOpen = !(props.config.modules.quadviews.enabled || props.config.modules.quadviews.profiles.some((profile) => profile.enabled));
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

function dimensionSessions(profileIndex: number) {
  return runtimeSessions.value.filter((session) => {
    if (session.quadviewsDimensions?.length !== 4) return false;
    const application = props.applications.find((app) => app.enabled && app.match.exe.toLowerCase() === session.application.toLowerCase());
    const index = application ? props.config.modules.quadviews.profiles.findIndex((profile) => profile.enabled && profile.applicationIds.includes(application.id)) : -1;
    return index === profileIndex;
  });
}

const diagnosticRuntimeSummary = computed(() => {
  const session = diagnosticRuntime.value;
  if (session) {
    return session.state.quadviewsDiagnosticVisualization
      ? t("quadviews.tab.diagnostics.shownIn", { application: session.application })
      : t("quadviews.tab.diagnostics.hiddenIn", { application: session.application });
  }
  if (runtimeSessions.value.length > 0) {
    return t("quadviews.tab.diagnostics.unavailable");
  }
  return t("quadviews.tab.diagnostics.noSession");
});

async function refreshRuntimeStatus() {
  try {
    runtimeSessions.value = (await loadRuntimeStatus()).sessions;
    runtimeStatusError.value = "";
  } catch (error) {
    runtimeSessions.value = [];
    runtimeStatusError.value = errorText(error, t("quadviews.tab.diagnostics.readStatusFailed"));
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
    throw new Error(t("quadviews.tab.diagnostics.notAcknowledged"));
  } catch (error) {
    runtimeStatusError.value = errorText(error, t("quadviews.tab.diagnostics.controlFailed"));
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
        t("quadviews.tab.customProfiles.alreadyTargeted", { application: appName, number: firstIndex + 1 }),
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
  return t("quadviews.tab.budget.estimate", { percent: estimatedPixelBudget(settings).toFixed(1) });
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
    light: t("quadviews.tab.budget.light"),
    moderate: t("quadviews.tab.budget.moderate"),
    heavy: t("quadviews.tab.budget.heavy"),
    detrimental: t("quadviews.tab.budget.detrimental"),
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
    :label="t('quadviews.tab.binding.label')"
    :description="t('quadviews.tab.binding.description')"
    :none-text="t('quadviews.tab.binding.noneText')"
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
            {{ t('quadviews.tab.intro') }}
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
          {{ t('quadviews.tab.varjoCompatibility') }}
        </button>
      </div>

      <SetupGuide class="mb-3" :title="t('quadviews.tab.setupGuide.title')" :summary="t('quadviews.tab.setupGuide.summary')" :open="setupGuideOpen">
        <p class="text-sm font-semibold">{{ t('quadviews.tab.setupGuide.heading') }}</p>
        <ul class="mt-1.5 list-disc space-y-1 pl-5 text-sm leading-6">
          <I18nText keypath="quadviews.tab.setupGuide.game" tag="li">
            <template #label><strong>{{ t('quadviews.tab.setupGuide.gameLabel') }}</strong></template>
            <template #useQuadView><strong>{{ t('quadviews.tab.setupGuide.useQuadView') }}</strong></template>
            <template #useEyeTracking><strong>{{ t('quadviews.tab.setupGuide.useEyeTracking') }}</strong></template>
          </I18nText>
          <I18nText keypath="quadviews.tab.setupGuide.provider" tag="li">
            <template #label><strong>{{ t('quadviews.tab.setupGuide.providerLabel') }}</strong></template>
            <template #layerName><code>XR_APILAYER_MBUCCHIA_quad_views_foveated</code></template>
          </I18nText>
          <I18nText keypath="quadviews.tab.setupGuide.apply" tag="li">
            <template #label><strong>{{ t('quadviews.tab.setupGuide.applyLabel') }}</strong></template>
          </I18nText>
        </ul>
        <p class="mt-3 text-sm leading-6 text-muted">
          {{ t('quadviews.tab.setupGuide.titles') }}
        </p>
        <p class="mt-1 text-xs leading-5 text-muted">
          {{ t('quadviews.tab.setupGuide.pixelEstimates') }}
        </p>
      </SetupGuide>

      <p class="mb-4 flex items-start gap-2 text-xs leading-5 text-muted" role="note">
        <span class="restart-required-mark" aria-hidden="true">&#8635;</span>
        <I18nText keypath="quadviews.tab.restartNote">
          <template #label><strong class="font-semibold">{{ t('quadviews.tab.restartNoteLabel') }}</strong></template>
        </I18nText>
      </p>

      <div class="mb-4 space-y-3 rounded-[1rem] border p-4 surface-panel-soft">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.18em]">{{ t('quadviews.tab.diagnostics.eyebrow') }}</p>
            <p class="mt-1 max-w-3xl text-sm leading-6 text-muted">
              {{ t('quadviews.tab.diagnostics.intro') }}
            </p>
          </div>
          <button class="button-secondary inline-flex items-center gap-2 rounded-[0.75rem] px-3.5 py-2 text-sm font-semibold" type="button" @click="overlayGuideOpen = true">
            <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="1.8">
              <path d="M10 2.5a7.5 7.5 0 1 0 0 15 7.5 7.5 0 0 0 0-15Zm0 4v4.25m0 2.75v.1" stroke-linecap="round" />
            </svg>
            {{ t('quadviews.tab.diagnostics.howToRead') }}
          </button>
        </div>
        <div class="grid gap-3 lg:grid-cols-[minmax(0,1fr)_minmax(19rem,0.8fr)]">
          <div class="rounded-[0.9rem] border p-4 surface-panel-strong">
            <div class="flex flex-wrap items-center justify-between gap-3">
              <div>
                <p class="text-sm font-semibold">{{ t('quadviews.tab.diagnostics.sessionControl') }}</p>
                <p class="mt-1 text-xs leading-5 text-muted">{{ diagnosticRuntimeSummary }}</p>
              </div>
              <button
                class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
                type="button"
                :disabled="!diagnosticRuntime || runtimeCommandPending"
                @click="setDiagnosticVisualization"
              >
                {{ runtimeCommandPending
                  ? t('quadviews.tab.diagnostics.applying')
                  : diagnosticRuntime?.state.quadviewsDiagnosticVisualization
                    ? t('quadviews.tab.diagnostics.hide')
                    : t('quadviews.tab.diagnostics.show') }}
              </button>
            </div>
            <p v-if="runtimeStatusError" class="mt-2 text-xs chip-danger">{{ runtimeStatusError }}</p>
          </div>
          <ModuleBindingPanel
            :heading="t('quadviews.tab.diagnostics.shortcutHeading')"
            :binding="config.modules.quadviews.diagnosticVisualizationBinding"
            :hint="t('quadviews.tab.diagnostics.shortcutHint')"
            @edit="openDiagnosticBinding"
          />
        </div>
        <p class="text-xs leading-5 text-muted">{{ t('quadviews.tab.diagnostics.guideHint') }}</p>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('quadviews.tab.defaultProfile.heading') }}</span>
          <span class="text-xs text-muted">{{ t('quadviews.tab.defaultProfile.appliesTo') }}</span>
          <span class="ml-auto flex flex-wrap items-center gap-2">
            <span
              class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]"
              :class="budgetChipClass(config.modules.quadviews.defaults)"
            >
              {{ budgetTone(config.modules.quadviews.defaults) }}
            </span>
            <span class="text-xs font-semibold">{{ budgetLabel(config.modules.quadviews.defaults) }}</span>
            <QuadViewsDimensions :sessions="dimensionSessions(-1)" />
          </span>
        </summary>
        <div class="mt-3 flex flex-wrap items-center gap-2">
          <label
            class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
            :title="t('quadviews.tab.launchStateNote')"
          >
            <input v-model="config.modules.quadviews.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
            {{ config.modules.quadviews.enabled ? t('quadviews.tab.defaultProfile.toggleOn') : t('quadviews.tab.defaultProfile.toggleOff') }}
            <span
              class="restart-required-mark"
              :title="t('quadviews.tab.defaultProfile.restartTitle')"
            >&#8635;</span>
          </label>
          <DefaultProfileExclusions
            :applications="applications"
            :profiles="config.modules.quadviews.profiles"
          />
        </div>
        <p v-if="!config.modules.quadviews.enabled" class="mt-2 text-xs text-muted">
          {{ t('quadviews.tab.defaultProfile.offNote') }}
        </p>
        <QuadViewsSettingsFields v-else class="mt-3" :settings="config.modules.quadviews.defaults" />
      </details>
    </article>

    <section class="space-y-3">
      <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
        <div class="min-w-0 flex-1">
          <h2 class="text-lg font-semibold tracking-tight">{{ t('quadviews.tab.customProfiles.heading') }}</h2>
          <p class="text-sm text-muted">{{ t('quadviews.tab.customProfiles.intro') }}</p>
        </div>
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('addQuadViewsProfile')"
        >
          {{ t('quadviews.tab.customProfiles.add') }}
        </button>
      </div>

      <ProfileShell
        v-for="(profile, index) in config.modules.quadviews.profiles"
        :key="`quadviews-profile-${index}`"
        :index="index"
        :profile="profile"
        :applications="applications"
        module-label="Quadviews"
        :enabled-change-note="t('quadviews.tab.launchStateNote')"
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
          <QuadViewsDimensions v-if="profile.enabled" :sessions="dimensionSessions(index)" />
        </template>

        <QuadViewsSettingsFields :settings="profile.settings" />
      </ProfileShell>

      <div
        v-if="config.modules.quadviews.profiles.length === 0"
        class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
      >
        {{ t('quadviews.tab.customProfiles.empty') }}
      </div>
    </section>

    <QuadViewsOverlayGuide :open="overlayGuideOpen" @close="overlayGuideOpen = false" />

    <div v-if="varjoCompatibilityInfoOpen" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
      <div class="w-full max-w-[720px] rounded-[1.25rem] border p-5 surface-panel-strong">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('quadviews.tab.varjoCompatibility') }}</p>
            <h2 class="mt-2 text-xl font-semibold tracking-tight">{{ t('quadviews.tab.varjo.title') }}</h2>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="varjoCompatibilityInfoOpen = false">
            {{ t('common.close') }}
          </button>
        </div>

        <div class="mt-5 space-y-4 text-sm leading-6">
          <div class="rounded-[1rem] border px-4 py-4 chip-accent" style="border-color: var(--app-border)">
            {{ t('quadviews.tab.varjo.biPanel') }}
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('quadviews.tab.varjo.notesIntro') }}
            <div class="mt-3 grid gap-3 sm:grid-cols-2">
              <div class="rounded-[0.9rem] border p-3" style="border-color: var(--app-border)">
                <div class="text-xs font-semibold uppercase tracking-[0.16em] text-muted">{{ t('quadviews.tab.varjo.stillApplies') }}</div>
                <ul class="mt-1.5 space-y-1">
                  <li>{{ t('quadviews.tab.varjo.focusScale') }}</li>
                  <li>{{ t('quadviews.tab.varjo.peripheralScale') }}</li>
                  <li>{{ t('quadviews.tab.varjo.focusSharpness') }}</li>
                </ul>
              </div>
              <div class="rounded-[0.9rem] border p-3" style="border-color: var(--app-border)">
                <div class="text-xs font-semibold uppercase tracking-[0.16em] text-muted">{{ t('quadviews.tab.varjo.handledByRuntime') }}</div>
                <ul class="mt-1.5 space-y-1">
                  <li>{{ t('quadviews.tab.varjo.focusSizeTransition') }}</li>
                  <li>{{ t('quadviews.tab.varjo.offsets') }}</li>
                  <li>{{ t('quadviews.tab.varjo.smoothingDeadzone') }}</li>
                  <li>{{ t('quadviews.tab.varjo.diagnosticVisualization') }}</li>
                </ul>
              </div>
            </div>
          </div>

          <div class="rounded-[1rem] border px-4 py-4 surface-panel">
            {{ t('quadviews.tab.varjo.emulation') }}
          </div>

          <I18nText keypath="quadviews.tab.varjo.diagnosticUnsupported" tag="div" class="rounded-[1rem] border px-4 py-4 surface-panel" role="note">
            <template #lead><strong>{{ t('quadviews.tab.varjo.diagnosticUnsupportedLead') }}</strong></template>
          </I18nText>
        </div>
      </div>
    </div>
  </div>
</template>
