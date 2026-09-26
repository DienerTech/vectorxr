<script setup lang="ts">
import { computed, ref } from "vue";

import ThemeToggle from "../ThemeToggle.vue";
import { playTestSound } from "../../lib/commands";
import type { VectorXRConfig } from "../../lib/model";
import type { ThemePreference } from "../../lib/theme";

const props = defineProps<{
  config: VectorXRConfig;
  path: string;
  logPath?: string;
  themePreference: ThemePreference;
  settingsActionsDisabled: boolean;
}>();

defineEmits<{
  viewLogs: [];
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

const soundPreviewError = ref("");

async function previewSoundVolume() {
  soundPreviewError.value = "";
  try {
    await playTestSound("", true, props.config.core.sound.volume);
  } catch (error) {
    soundPreviewError.value = error instanceof Error ? error.message : "Failed to play sound.";
  }
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <h2 class="text-2xl font-semibold tracking-tight">Settings</h2>
      <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
        App-wide switches, appearance, logging, sound feedback, and config files.
      </p>

      <section class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <p class="eyebrow mb-3 text-xs font-semibold uppercase tracking-[0.24em]">General</p>
        <div class="space-y-3">
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">VectorXR Enabled</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">Enables or disables all VectorXR OpenXR Enhancements at runtime.</p>
            </div>
            <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
              <input v-model="config.core.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ config.core.enabled ? "Enabled" : "Disabled" }}
            </label>
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">Theme</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">Stored on this PC only.</p>
            </div>
            <ThemeToggle
              :model-value="themePreference"
              @update:model-value="$emit('update:themePreference', $event)"
            />
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0 max-w-2xl">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">Track discovered XR apps</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">
                Notes the executable name and first/last seen time of OpenXR apps you launch, so you can easily
                register them from the Application Registry. All data is kept locally on this PC.
              </p>
            </div>
            <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
              <input v-model="config.core.trackSeenApps" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ config.core.trackSeenApps ? "On" : "Off" }}
            </label>
          </div>
        </div>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <p class="eyebrow mb-3 text-xs font-semibold uppercase tracking-[0.24em]">Logging</p>
        <div class="flex flex-wrap items-end gap-4">
          <label class="block w-48">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              Log Level
              <span
                title="Info writes normal operational messages and errors. Debug adds verbose diagnostics, including bounded Turbo frame-timing traces. Logging changes apply live; launch the VR app with Debug enabled for runtime clock measurements."
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <select v-model="config.core.logLevel" class="app-input manual-input-select h-10 w-full rounded-[0.75rem] px-3 py-2 text-sm">
              <option value="info">Info</option>
              <option value="debug">Debug</option>
            </select>
          </label>
          <label class="block w-48">
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
              class="app-input h-10 w-full rounded-[0.75rem] px-3 py-2 text-sm"
              min="1"
              max="50"
              step="1"
              type="number"
            />
          </label>
          <button class="button-secondary h-10 rounded-[0.75rem] px-4 text-sm font-medium" type="button" @click="$emit('viewLogs')">
            View Logs
          </button>
        </div>
        <p class="mt-3 text-xs text-muted">
          Latest log: <span class="mono-text font-mono">{{ logPathShort || "Available after first session" }}</span>
        </p>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Sound Feedback</p>
            <p class="mt-1 text-xs leading-5 text-muted">Master volume for the activate / deactivate cues you can attach to bindings.</p>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" type="button" @click="previewSoundVolume">
            ▶ Test
          </button>
        </div>
        <div class="flex max-w-xl items-center gap-3">
          <input
            v-model.number="config.core.sound.volume"
            aria-label="Sound feedback volume"
            class="h-2 flex-1 cursor-pointer accent-depthxr-copper"
            type="range"
            min="0"
            max="100"
            step="1"
          />
          <span class="w-12 text-right font-mono text-sm">{{ config.core.sound.volume }}%</span>
        </div>
        <p v-if="soundPreviewError" class="mt-2 text-sm chip-warning">{{ soundPreviewError }}</p>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">Config Files</p>
            <p class="mt-1 text-xs leading-5 text-muted">Import another settings file, export the current settings, or confirm where VectorXR keeps its local config.</p>
          </div>
          <div class="flex flex-wrap gap-2">
            <button
              class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('importConfig')"
            >
              Import Config
            </button>
            <button
              class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('exportConfig')"
            >
              Export Config
            </button>
          </div>
        </div>

        <label class="mt-3 block">
          <span class="mb-1.5 block text-xs text-muted">Config directory</span>
          <input
            class="app-input w-full rounded-[0.75rem] px-3 py-2 font-mono text-xs"
            :value="configDirectory"
            readonly
            type="text"
          />
        </label>

        <div class="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
          <div class="min-w-0">
            <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">Reset to defaults</p>
            <p class="mt-0.5 text-xs leading-5 text-muted">Rebuild settings.json with default values and clear local OpenXR application discovery data.</p>
          </div>
          <button
            class="button-danger rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="settingsActionsDisabled"
            type="button"
            @click="$emit('resetConfig')"
          >
            Reset to Default
          </button>
        </div>
      </section>
    </article>
  </div>
</template>
