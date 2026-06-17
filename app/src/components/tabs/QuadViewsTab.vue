<script setup lang="ts">
import { computed } from "vue";

import ProfileShell from "../ProfileShell.vue";
import QuadViewsSettingsFields from "../QuadViewsSettingsFields.vue";
import type {
  QuadViewsSettings,
  RegisteredApplication,
  VectorXRConfig,
} from "../../lib/model";

const props = defineProps<{
  config: VectorXRConfig;
  applications: RegisteredApplication[];
}>();

defineEmits<{
  addQuadViewsProfile: [];
  removeQuadViewsProfile: [index: number];
  syncQuadViewsProfileName: [index: number];
}>();

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

function trackingModeLabel(settings: QuadViewsSettings) {
  return settings.trackingMode === "eye" ? "Eye tracked" : "Head tracked";
}

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
  return `${estimatedPixelBudget(settings).toFixed(1)}% of stereo pixels`;
}

function budgetTone(settings: QuadViewsSettings) {
  const budget = estimatedPixelBudget(settings);

  if (budget <= 45) {
    return "Light";
  }

  if (budget <= 85) {
    return "Moderate";
  }

  return "Heavy";
}
</script>

<template>
  <div class="space-y-4">
    <article
      class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel"
    >
      <div class="mb-4 flex flex-wrap items-center justify-between gap-3">
        <div>
          <div class="flex flex-wrap items-center gap-3">
            <h2 class="text-2xl font-semibold tracking-tight">Quadviews</h2>
            <span
              class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-warning"
            >
              ⚠ Experimental
            </span>
          </div>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Manage native quadview defaults and per-application profiles for
            foveal and peripheral rendering.
          </p>
          <p class="mt-2 max-w-3xl text-sm font-bold leading-6 text-muted">
          Experimental - DX11 OpenXR applications only!
          </p>
        </div>
        <label
          class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
        >
          <input
            v-model="config.modules.quadviews.enabled"
            class="h-4 w-4 accent-depthxr-copper"
            type="checkbox"
          />
          Default Profile {{ config.modules.quadviews.enabled ? "On" : "Off" }}
          <span
            title="Turns the default Quadviews profile on or off for apps without an enabled custom profile."
            class="cursor-help select-none text-xs text-muted"
            >ⓘ</span
          >
        </label>
      </div>

      <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
        <summary class="flex flex-wrap items-center gap-2">
          <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
          </svg>
          <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Default Profile</span>
          <span class="text-xs text-muted">Applies to applications without an enabled custom profile</span>
          <span class="ml-auto flex items-center gap-2">
            <span class="rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-accent">
              {{ budgetTone(config.modules.quadviews.defaults) }}
            </span>
            <span class="text-xs text-muted">{{ budgetLabel(config.modules.quadviews.defaults) }}</span>
          </span>
        </summary>
        <QuadViewsSettingsFields class="mt-3" :settings="config.modules.quadviews.defaults" />
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
        :warnings="profileWarnings.get(index)"
        @remove="$emit('removeQuadViewsProfile', index)"
        @sync-name="$emit('syncQuadViewsProfileName', index)"
      >
        <template #badges>
          <span
            v-if="profile.enabled"
            class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent"
            >{{ trackingModeLabel(profile.settings) }}</span
          >
          <span
            v-if="profile.enabled"
            class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent"
            >{{ budgetLabel(profile.settings) }}</span
          >
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
  </div>
</template>
