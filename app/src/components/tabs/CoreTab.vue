<script setup lang="ts">
import { computed } from "vue";

import ThemeToggle from "../ThemeToggle.vue";
import type { OpenXrLayerEntry, OpenXrLayerSnapshot } from "../../lib/commands";
import type { HealthSummary } from "../../lib/health";
import type { VectorXRConfig } from "../../lib/model";
import type { ThemePreference } from "../../lib/theme";

const props = defineProps<{
  config: VectorXRConfig;
  path: string;
  logPath?: string;
  themePreference: ThemePreference;
  settingsActionsDisabled: boolean;
  openXrLayerSnapshot: OpenXrLayerSnapshot | null;
  openXrLayersLoading: boolean;
  healthSummary: HealthSummary;
}>();

defineEmits<{
  viewLogs: [];
  viewHealth: [];
  exportDebug: [];
  importConfig: [];
  exportConfig: [];
  resetConfig: [];
  "update:themePreference": [value: ThemePreference];
}>();

const logPathShort = computed(() => {
  if (!props.logPath) return null;
  const sep = props.logPath.includes("\\") ? "\\" : "/";
  return props.logPath.split(sep).pop() ?? props.logPath;
});

const configDirectory = computed(() => {
  if (!props.path) return "Available after config loads";
  const directory = props.path.replace(/[\\/][^\\/]*$/, "");
  return directory || props.path;
});

const vectorXrLayer = computed<OpenXrLayerEntry | null>(() => {
  return props.openXrLayerSnapshot?.slices
    .flatMap((slice) => slice.layers)
    .find((layer) => layer.isVectorXr) ?? null;
});

const vectorXrLayerStatusLabel = computed(() => {
  if (props.openXrLayersLoading) return "Checking";
  if (!vectorXrLayer.value) return "Not found";
  return vectorXrLayer.value.enabled ? "Layer enabled" : "Layer disabled";
});

const vectorXrLayerStatusClass = computed(() => {
  if (props.openXrLayersLoading || !vectorXrLayer.value) return "chip-idle";
  return vectorXrLayer.value.enabled ? "chip-success" : "chip-warning";
});

const vectorXrLayerStatusDescription = computed(() => {
  if (props.openXrLayersLoading) {
    return "Checking the registered VectorXR OpenXR API layer.";
  }

  if (!vectorXrLayer.value) {
    return "VectorXR's OpenXR API layer registration was not found. OpenXR Tweaks will not apply until the layer is registered and enabled.";
  }

  if (vectorXrLayer.value.enabled) {
    return "The VectorXR OpenXR API layer is enabled, so enabled OpenXR Tweaks can apply at runtime.";
  }

  return "The VectorXR OpenXR API layer is disabled. None of the VectorXR OpenXR Tweaks will apply, regardless of suite or tweak enabled state.";
});

const runtimeStatusClass = computed(() => props.config.core.enabled ? "chip-success" : "chip-warning");
const runtimeStatusLabel = computed(() => props.config.core.enabled ? "Runtime enabled" : "Runtime disabled");
const systemHealthDescription = computed(() => {
  if (!props.config.core.enabled) {
    return "Runtime effects are disabled from the suite switch. Layer status is still available for troubleshooting.";
  }

  if (props.openXrLayersLoading) {
    return "Checking VectorXR runtime and OpenXR layer readiness.";
  }

  if (!vectorXrLayer.value) {
    return "VectorXR is enabled, but the OpenXR API layer registration was not found.";
  }

  if (!vectorXrLayer.value.enabled) {
    return "VectorXR is enabled, but the OpenXR API layer is disabled.";
  }

  return "VectorXR runtime and OpenXR layer status look ready for future OpenXR app launches.";
});
</script>

<template>
  <div class="space-y-6">
    <article
      class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel"
    >
      <div class="mb-4">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Home</p>
        <h2 class="text-2xl font-semibold tracking-tight">Runtime settings</h2>
        <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
          Control the app-wide runtime switch, logging behavior, local theme preference, and import or export settings.
        </p>
      </div>

      <div
        class="mb-5 flex items-center justify-between rounded-[0.9rem] border px-4 py-3 surface-panel-strong"
      >
        <div>
          <p class="text-sm font-semibold">VectorXR Enabled</p>
          <p class="mt-0.5 text-xs text-muted">
            Enables or disables all VectorXR OpenXR Tweaks effects at runtime.
          </p>
        </div>
        <label
          class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
        >
          <input
            v-model="config.core.enabled"
            class="h-4 w-4 accent-depthxr-copper"
            type="checkbox"
          />
          {{ config.core.enabled ? "Enabled" : "Disabled" }}
        </label>
      </div>

      <div
        class="mb-5 rounded-[0.9rem] border px-4 py-3 surface-panel-strong"
      >
        <div class="grid gap-3 lg:grid-cols-[minmax(0,1fr)_auto] lg:items-start">
          <div class="min-w-0">
            <p class="text-sm font-semibold">System Health</p>
            <p class="mt-0.5 text-xs text-muted">
              {{ systemHealthDescription }}
            </p>
          </div>
          <div class="flex flex-wrap gap-2 lg:justify-end">
            <span
              class="inline-flex min-w-[9.75rem] cursor-default items-center justify-center rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
              :class="runtimeStatusClass"
              title="Suite-level runtime switch state."
            >
              {{ runtimeStatusLabel }}
            </span>
            <span
              class="inline-flex min-w-[9.75rem] cursor-default items-center justify-center rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]"
              :class="vectorXrLayerStatusClass"
              :title="vectorXrLayerStatusDescription"
            >
              {{ vectorXrLayerStatusLabel }}
            </span>
          </div>
        </div>
        <div class="mt-3 flex flex-wrap gap-2 border-t pt-3" style="border-color: var(--app-border)">
          <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('viewHealth')">
            View Health
          </button>
          <button class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium" type="button" @click="$emit('exportDebug')">
            Export Debug
          </button>
        </div>
      </div>

      <div class="mt-5 mb-5">
        <span class="mb-1.5 block text-sm font-medium">Theme</span>
        <ThemeToggle
          :model-value="themePreference"
          @update:model-value="$emit('update:themePreference', $event)"
        />
      </div>

      <div
        class="grid gap-5 sm:grid-cols-2 lg:grid-cols-[minmax(0,200px)_minmax(0,200px)] mb-0"  
      >
        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Log Level
            <span
              title="Info writes normal operational messages and errors. Debug adds verbose diagnostics."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <select
            v-model="config.core.logLevel"
            class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5"
          >
            <option value="info">info</option>
            <option value="debug">debug</option>
          </select>
        </label>

        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            Log Retention
            <span
              title="Number of rotated log files to keep. Older files are removed after the retention count is exceeded."
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <input
            v-model.number="config.core.logRetentionFiles"
            class="app-input h-11 w-full rounded-[0.75rem] px-4 py-2.5"
            min="1"
            max="50"
            step="1"
            type="number"
          />
        </label>
      </div>

      <div class="mt-4 flex flex-wrap items-center gap-3">
        <button
          class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
          type="button"
          @click="$emit('viewLogs')"
        >
          View Logs
        </button>
        <div class="rounded-full border px-4 py-2 text-sm surface-panel-strong">
          Latest Log:
          <span class="font-mono text-xs">{{
            logPathShort || "Available after first session"
          }}</span>
        </div>
      </div>

      <section class="mt-6 border-t pt-5">
        <div class="flex flex-wrap items-start justify-between gap-4">
          <div>
            <p class="eyebrow text-xs uppercase tracking-[0.24em]">
              Manage Settings
            </p>
            <h3 class="mt-1 text-lg font-semibold tracking-tight">
              Config files
            </h3>
            <p class="mt-2 max-w-2xl text-sm leading-6 text-muted">
              Import another settings file, export the current settings, or
              confirm where VectorXR keeps its local config.
            </p>
          </div>

          <div class="flex flex-wrap gap-3">
            <button
              class="button-secondary rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('importConfig')"
            >
              Import Config
            </button>
            <button
              class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('exportConfig')"
            >
              Export Config
            </button>
          </div>
        </div>

        <label class="mt-4 block">
          <span class="mb-1.5 block text-sm font-medium">
            Config Directory
          </span>
          <input
            class="app-input w-full rounded-[0.75rem] px-4 py-2.5 font-mono text-xs"
            :value="configDirectory"
            readonly
            type="text"
          />
        </label>

        <div class="mt-5 border-t pt-5">
          <div class="flex flex-wrap items-center justify-between gap-4">
            <div>
              <h4 class="text-sm font-semibold tracking-tight">
                Reset to defaults
              </h4>
              <p class="mt-1 max-w-2xl text-sm leading-6 text-muted">
                Rebuild settings.json with default values and clear local OpenXR application discovery data.
              </p>
            </div>
            <button
              class="button-danger rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('resetConfig')"
            >
              Reset to Default
            </button>
          </div>
        </div>
      </section>
    </article>
  </div>
</template>
