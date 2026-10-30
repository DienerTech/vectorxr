<script setup lang="ts">
import { computed, ref } from "vue";

import I18nText from "../I18nText.vue";
import ThemeToggle from "../ThemeToggle.vue";
import { activeLocale, isMachineTranslated, t } from "../../i18n/index.ts";
import { resolveLanguagePreference, type LanguagePreference } from "../../i18n/preference.ts";
import { menuLocales, pseudoLocale, sourceLocale, supportedLocales } from "../../i18n/locales.ts";
import { playTestSound, requestInputRescan } from "../../lib/commands";
import { errorText } from "../../lib/errors";
import type { VectorXRConfig } from "../../lib/model";
import type { ThemePreference } from "../../lib/theme";

const props = defineProps<{
  config: VectorXRConfig;
  path: string;
  logPath?: string;
  themePreference: ThemePreference;
  languagePreference: LanguagePreference;
  trayEnabled: boolean;
  settingsActionsDisabled: boolean;
}>();

defineEmits<{
  viewLogs: [];
  exportDebug: [];
  importConfig: [];
  exportConfig: [];
  resetConfig: [];
  "update:themePreference": [value: ThemePreference];
  "update:languagePreference": [value: LanguagePreference];
  "update:trayEnabled": [value: boolean];
}>();

// Rescan inputs: running games look again for bound devices that were not
// connected (a hot-plugged controller Windows did not report).
const rescanning = ref(false);
const rescanResult = ref("");
async function rescanInputs() {
  rescanning.value = true;
  rescanResult.value = "";
  try {
    const games = await requestInputRescan();
    rescanResult.value =
      games === 0
        ? t("settings.core.general.inputDevices.noGame")
        : t("settings.core.general.inputDevices.asked", { count: games });
  } catch (error) {
    rescanResult.value = errorText(error, t("settings.core.general.inputDevices.rescanFailed"));
  } finally {
    rescanning.value = false;
  }
}

// One select: System (auto), then every language by its own name. The
// pseudo-locale is for development builds.
const showPseudoLocale = import.meta.env.DEV;
const systemLanguageName = computed(() => {
  const code = resolveLanguagePreference("system");
  return supportedLocales.find((locale) => locale.code === code)?.nativeName ?? code;
});
const machineTranslated = computed(
  () => activeLocale.value !== sourceLocale && activeLocale.value !== pseudoLocale && isMachineTranslated(activeLocale.value),
);

const logPathShort = computed(() => {
  if (!props.logPath) return null;
  const sep = props.logPath.includes("\\") ? "\\" : "/";
  return props.logPath.split(sep).pop() ?? props.logPath;
});

const configDirectory = computed(() => {
  if (!props.path) return t("settings.core.configFiles.directoryPending");
  const directory = props.path.replace(/[\\/][^\\/]*$/, "");
  return directory || props.path;
});

const soundPreviewError = ref("");

async function previewSoundVolume() {
  soundPreviewError.value = "";
  try {
    await playTestSound("", true, props.config.core.sound.volume);
  } catch (error) {
    soundPreviewError.value = errorText(error, t("settings.core.sound.previewFailed"));
  }
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <h2 class="text-2xl font-semibold tracking-tight">{{ t("settings.core.title") }}</h2>
      <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
        {{ t("settings.core.intro") }}
      </p>

      <section class="mt-4 border-t pt-4" style="border-color: var(--app-border)">
        <p class="eyebrow mb-3 text-xs font-semibold uppercase tracking-[0.24em]">{{ t("settings.core.general.heading") }}</p>
        <div class="space-y-3">
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.enabled.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">{{ t("settings.core.general.enabled.description") }}</p>
            </div>
            <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
              <input v-model="config.core.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ config.core.enabled ? t("common.enabled") : t("common.disabled") }}
            </label>
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.theme.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">{{ t("settings.core.general.theme.description") }}</p>
            </div>
            <ThemeToggle
              :model-value="themePreference"
              @update:model-value="$emit('update:themePreference', $event)"
            />
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0 max-w-2xl">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.language.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">{{ t("settings.core.general.language.description") }}</p>
              <p v-if="machineTranslated" class="mt-1 text-xs leading-5 text-muted">
                {{ t("settings.core.general.language.machineTranslated") }}
              </p>
            </div>
            <select
              :value="languagePreference"
              :aria-label="t('settings.core.general.language.title')"
              class="app-input manual-input-select h-10 min-w-[14rem] rounded-[0.75rem] px-3 py-2 text-sm"
              @change="$emit('update:languagePreference', ($event.target as HTMLSelectElement).value as LanguagePreference)"
            >
              <option value="system">{{ t("settings.core.general.language.systemResolved", { language: systemLanguageName }) }}</option>
              <option v-for="locale in menuLocales" :key="locale.code" :value="locale.code" :lang="locale.code">
                {{ locale.nativeName }}
              </option>
              <option v-if="showPseudoLocale" :value="pseudoLocale">{{ t("settings.core.general.language.pseudo") }}</option>
            </select>
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0 max-w-2xl">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.tray.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">
                {{ t("settings.core.general.tray.description") }}
              </p>
              <p class="mt-1 text-xs leading-5 text-muted">
                {{ t("settings.core.general.tray.helperNote") }}
              </p>
            </div>
            <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
              <input
                :checked="trayEnabled"
                class="h-4 w-4 accent-depthxr-copper"
                type="checkbox"
                @change="$emit('update:trayEnabled', ($event.target as HTMLInputElement).checked)"
              />
              {{ trayEnabled ? t("common.on") : t("common.off") }}
            </label>
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0 max-w-2xl">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.inputDevices.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">
                {{ t("settings.core.general.inputDevices.description") }}
              </p>
              <p v-if="rescanResult" class="mt-1 text-xs leading-5" style="color: var(--app-text)">{{ rescanResult }}</p>
            </div>
            <button
              class="rounded-full border px-4 py-2 text-sm font-medium transition surface-panel-strong"
              style="border-color: var(--app-border)"
              type="button"
              :disabled="rescanning"
              @click="rescanInputs"
            >
              {{ rescanning ? t("settings.core.general.inputDevices.rescanning") : t("settings.core.general.inputDevices.rescan") }}
            </button>
          </div>
          <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
            <div class="min-w-0 max-w-2xl">
              <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.general.trackSeenApps.title") }}</p>
              <p class="mt-0.5 text-xs leading-5 text-muted">
                {{ t("settings.core.general.trackSeenApps.description") }}
              </p>
            </div>
            <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
              <input v-model="config.core.trackSeenApps" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              {{ config.core.trackSeenApps ? t("common.on") : t("common.off") }}
            </label>
          </div>
        </div>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <p class="eyebrow mb-3 text-xs font-semibold uppercase tracking-[0.24em]">{{ t("settings.core.logging.heading") }}</p>
        <div class="flex flex-wrap items-end gap-4">
          <label class="block w-48">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t("settings.core.logging.logLevel") }}
              <span
                :title="t('settings.core.logging.logLevelHelp')"
                class="cursor-help select-none text-xs text-muted"
                >ⓘ</span
              >
            </span>
            <select v-model="config.core.logLevel" class="app-input manual-input-select h-10 w-full rounded-[0.75rem] px-3 py-2 text-sm">
              <option value="info">{{ t("settings.core.logging.levelInfo") }}</option>
              <option value="debug">{{ t("settings.core.logging.levelDebug") }}</option>
            </select>
          </label>
          <label class="block w-48">
            <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
              {{ t("settings.core.logging.retention") }}
              <span
                :title="t('settings.core.logging.retentionHelp')"
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
            {{ t("settings.core.logging.viewLogs") }}
          </button>
          <button
            class="button-secondary h-10 rounded-[0.75rem] px-4 text-sm font-medium"
            type="button"
            :title="t('settings.core.logging.exportDebugHelp')"
            @click="$emit('exportDebug')"
          >
            {{ t("settings.core.logging.exportDebug") }}
          </button>
        </div>
        <I18nText keypath="settings.core.logging.latestLog" tag="p" class="mt-3 text-xs text-muted">
          <template #path><span class="mono-text font-mono">{{ logPathShort || t("settings.core.logging.latestLogPending") }}</span></template>
        </I18nText>
      </section>

      <section class="mt-5 border-t pt-4" style="border-color: var(--app-border)">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div>
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t("settings.core.sound.heading") }}</p>
            <p class="mt-1 text-xs leading-5 text-muted">{{ t("settings.core.sound.description") }}</p>
          </div>
          <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-xs font-medium" type="button" @click="previewSoundVolume">
            ▶ {{ t("settings.core.sound.test") }}
          </button>
        </div>
        <div class="flex max-w-xl items-center gap-3">
          <input
            v-model.number="config.core.sound.volume"
            :aria-label="t('settings.core.sound.volume')"
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
            <p class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t("settings.core.configFiles.heading") }}</p>
            <p class="mt-1 text-xs leading-5 text-muted">{{ t("settings.core.configFiles.description") }}</p>
          </div>
          <div class="flex flex-wrap gap-2">
            <button
              class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('importConfig')"
            >
              {{ t("settings.core.configFiles.import") }}
            </button>
            <button
              class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
              :disabled="settingsActionsDisabled"
              type="button"
              @click="$emit('exportConfig')"
            >
              {{ t("settings.core.configFiles.export") }}
            </button>
          </div>
        </div>

        <label class="mt-3 block">
          <span class="mb-1.5 block text-xs text-muted">{{ t("settings.core.configFiles.directory") }}</span>
          <input
            class="app-input w-full rounded-[0.75rem] px-3 py-2 font-mono text-xs"
            :value="configDirectory"
            readonly
            type="text"
          />
        </label>

        <div class="mt-4 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border p-4 surface-panel-soft">
          <div class="min-w-0">
            <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t("settings.core.configFiles.reset.title") }}</p>
            <p class="mt-0.5 text-xs leading-5 text-muted">{{ t("settings.core.configFiles.reset.description") }}</p>
          </div>
          <button
            class="button-danger rounded-[0.75rem] px-4 py-2 text-sm font-medium transition disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="settingsActionsDisabled"
            type="button"
            @click="$emit('resetConfig')"
          >
            {{ t("settings.core.configFiles.reset.button") }}
          </button>
        </div>
      </section>
    </article>
  </div>
</template>
