<script setup lang="ts">
import { computed, onMounted } from 'vue'

import EffectField from './components/EffectField.vue'
import ProfileEditor from './components/ProfileEditor.vue'
import type { LogLevel } from './lib/model'
import { validateConfig } from './lib/validation'
import { useConfigStore } from './stores/configStore'

const store = useConfigStore()
const logLevels: LogLevel[] = ['off', 'error', 'info', 'debug']

const errors = computed(() => validateConfig(store.state.config))

onMounted(() => {
  void store.load()
})

async function saveConfig() {
  if (errors.value.length > 0) {
    store.state.status = 'Fix validation errors before saving'
    return
  }

  await store.save()
}
</script>

<template>
  <main class="min-h-screen bg-[radial-gradient(circle_at_top_left,_rgba(185,111,61,0.18),_transparent_28%),linear-gradient(135deg,_#eff2eb_0%,_#f8f3e8_45%,_#d7c7a6_100%)] px-5 py-8 text-depthxr-ink md:px-8 xl:px-12">
    <section class="mx-auto max-w-7xl">
      <header class="mb-8 overflow-hidden rounded-[2.5rem] border border-black/10 bg-depthxr-ink text-white shadow-panel">
        <div class="grid gap-8 p-8 lg:grid-cols-[1.35fr_0.9fr]">
          <div>
            <p class="text-xs uppercase tracking-[0.32em] text-depthxr-sand">DepthXR</p>
            <h1 class="mt-3 text-5xl font-semibold tracking-tight">Stereo depth tuning without the toolkit runtime.</h1>
            <p class="mt-4 max-w-2xl text-base leading-7 text-white/72">
              Edit the shared JSON config used by the DepthXR OpenXR layer. Profiles are matched by executable name and resolved at runtime inside
              <code class="rounded bg-white/10 px-2 py-1 text-sm">xrLocateViews</code>.
            </p>
          </div>

          <div class="rounded-[2rem] border border-white/10 bg-white/5 p-5">
            <p class="text-sm uppercase tracking-[0.22em] text-white/56">Shared Config Path</p>
            <p class="mt-3 break-all rounded-3xl bg-white/10 px-4 py-3 font-mono text-sm">
              {{ store.state.path || 'Resolving…' }}
            </p>

            <div class="mt-5 flex flex-wrap gap-3">
              <button
                class="rounded-full bg-depthxr-copper px-5 py-3 font-medium text-white transition hover:brightness-110 disabled:cursor-not-allowed disabled:opacity-60"
                :disabled="store.state.loading || store.state.saving"
                type="button"
                @click="saveConfig"
              >
                {{ store.state.saving ? 'Saving…' : 'Save Config' }}
              </button>
              <button
                class="rounded-full border border-white/20 px-5 py-3 font-medium text-white transition hover:border-white/40"
                :disabled="store.state.loading || store.state.saving"
                type="button"
                @click="store.load"
              >
                Reload
              </button>
            </div>

            <p class="mt-4 text-sm text-white/70">{{ store.state.status }}</p>
          </div>
        </div>
      </header>

      <section class="grid gap-8 xl:grid-cols-[1.15fr_0.85fr]">
        <div class="space-y-8">
          <article class="rounded-4xl border border-black/10 bg-white/80 p-6 shadow-panel backdrop-blur">
            <div class="mb-5 flex flex-wrap items-center justify-between gap-3">
              <div>
                <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Global</p>
                <h2 class="text-3xl font-semibold tracking-tight text-depthxr-pine">Default Runtime Settings</h2>
              </div>
              <label class="inline-flex items-center gap-3 rounded-full bg-depthxr-pine px-4 py-2 text-sm font-medium text-white">
                <input v-model="store.state.config.global.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
                Layer Enabled
              </label>
            </div>

            <div class="mb-5 grid gap-4 md:grid-cols-2">
              <label class="block">
                <span class="mb-2 block text-sm font-medium">Log Level</span>
                <select v-model="store.state.config.global.logLevel" class="w-full rounded-2xl border border-black/10 bg-white px-4 py-3">
                  <option v-for="level in logLevels" :key="level" :value="level">
                    {{ level }}
                  </option>
                </select>
              </label>
              <div class="rounded-2xl border border-dashed border-black/10 bg-[#f7f2e8] px-4 py-3 text-sm leading-6 text-depthxr-steel">
                Neutral values are <strong>1.0</strong>. The layer skips each effect when its value is neutral or its feature toggle is disabled.
              </div>
            </div>

            <div class="grid gap-4 xl:grid-cols-3">
              <EffectField
                v-model:enabled="store.state.config.global.stereoBoostEnabled"
                v-model:value="store.state.config.global.stereoBoost"
                title="Stereo Boost"
                subtitle="Expands the eye baseline around the midpoint."
                :min="0.5"
                :max="2"
                :step="0.01"
              />
              <EffectField
                v-model:enabled="store.state.config.global.worldScaleEnabled"
                v-model:value="store.state.config.global.worldScale"
                title="World Scale"
                subtitle="Scales view-space translations as a first-pass world scale experiment."
                :min="0.5"
                :max="2"
                :step="0.01"
              />
              <EffectField
                v-model:enabled="store.state.config.global.fovScaleEnabled"
                v-model:value="store.state.config.global.fovScale"
                title="FoV Scale"
                subtitle="Adjusts returned FoV angles in tangent space."
                :min="0.5"
                :max="1.5"
                :step="0.01"
              />
            </div>
          </article>

          <section class="space-y-5">
            <div class="flex flex-wrap items-center justify-between gap-3">
              <div>
                <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Profiles</p>
                <h2 class="text-3xl font-semibold tracking-tight text-depthxr-pine">Per-Game Overrides</h2>
              </div>
              <button
                class="rounded-full bg-depthxr-copper px-5 py-3 font-medium text-white transition hover:brightness-110"
                type="button"
                @click="store.addProfile"
              >
                Add Profile
              </button>
            </div>

            <ProfileEditor
              v-for="(profile, index) in store.state.config.profiles"
              :key="`${profile.match.exe}-${index}`"
              :index="index"
              :profile="profile"
              @remove="store.removeProfile(index)"
            />

            <div
              v-if="store.state.config.profiles.length === 0"
              class="rounded-4xl border border-dashed border-black/15 bg-white/50 px-6 py-8 text-center text-depthxr-steel"
            >
              No per-game overrides yet. Add a profile to bind custom values to a specific executable.
            </div>
          </section>
        </div>

        <aside class="space-y-6">
          <article class="rounded-4xl border border-black/10 bg-white/85 p-6 shadow-panel">
            <p class="text-xs uppercase tracking-[0.24em] text-depthxr-copper">Validation</p>
            <h2 class="mt-2 text-2xl font-semibold tracking-tight text-depthxr-pine">Config Health</h2>

            <ul v-if="errors.length > 0" class="mt-4 space-y-3">
              <li
                v-for="error in errors"
                :key="error"
                class="rounded-2xl border border-red-200 bg-red-50 px-4 py-3 text-sm text-red-700"
              >
                {{ error }}
              </li>
            </ul>

            <div v-else class="mt-4 rounded-2xl border border-emerald-200 bg-emerald-50 px-4 py-4 text-sm text-emerald-700">
              Config structure and value ranges look valid.
            </div>
          </article>

          <article class="rounded-4xl border border-black/10 bg-[#24322d] p-6 text-white shadow-panel">
            <p class="text-xs uppercase tracking-[0.24em] text-depthxr-sand">Runtime Notes</p>
            <ul class="mt-4 space-y-3 text-sm leading-6 text-white/78">
              <li>DepthXR is a standalone OpenXR API layer. It does not depend on OpenXR-Toolkit being installed or active.</li>
              <li>Per-profile matching currently uses executable file name only.</li>
              <li>World scale is intentionally labeled experimental because wider space interception may be needed later.</li>
            </ul>
          </article>
        </aside>
      </section>
    </section>
  </main>
</template>
