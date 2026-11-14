<script setup lang="ts">
// A stepped, animated walkthrough: one scene drawn in code, a caption per
// step, Back / Play / Next, step dots, and optional background music scored
// to the steps, with mute and volume. The clock is the music while it plays
// (so picture and music stay together) and the page's frame clock otherwise.
import { computed, onBeforeUnmount, ref, watch } from 'vue'

import { t, type MessageKey } from '../i18n/index.ts'
import { scenes } from './walkthroughs/scenes'
import './walkthroughs/walkthrough.css'
import type { GuideId } from '../lib/guides'
import { defaultMusicVolume, loadMusicPreference, nextRiff, secondsPerBeat, stepAt, storeMusicPreference, walkthroughSteps, walkthroughs } from '../lib/walkthroughs'

const props = defineProps<{ id: GuideId }>()

const walkthrough = computed(() => walkthroughs[props.id]!)
const steps = computed(() => walkthroughSteps(walkthrough.value))
const spb = computed(() => secondsPerBeat(walkthrough.value))
const totalEnd = computed(() => steps.value[steps.value.length - 1].end)

const time = ref(0)
const playing = ref(false)
const preference = loadMusicPreference()
const muted = ref(preference.muted)
const volume = ref(preference.volume)
// Which riff on the theme plays: the next one each time the walkthrough opens
// or replays.
const riff = ref(nextRiff(props.id, walkthrough.value.music.length))
const audio = new Audio()
audio.preload = 'auto'
audio.volume = volume.value

const step = computed(() => stepAt(steps.value, time.value))
// Seconds and beats into the current step.
const local = computed(() => time.value - steps.value[step.value].start)
const beat = computed(() => local.value / spb.value)
const finished = computed(() => time.value >= totalEnd.value)

const key = (path: string) => path as MessageKey
const caption = computed(() => t(key(`guides.walkthroughs.${props.id}.steps.${step.value + 1}`)))

let frame = 0
let lastTick = 0
// Set when a step was picked while paused: the scene shows that step settled,
// and Play starts it from its beginning (on its downbeat in the music).
let settled = false

function audioLive() {
  return !muted.value && !audio.paused
}

function tick() {
  if (!playing.value) return
  const now = performance.now()
  if (audioLive()) {
    time.value = audio.currentTime
  } else {
    time.value += (now - lastTick) / 1000
  }
  lastTick = now
  if (time.value >= totalEnd.value) {
    time.value = totalEnd.value
    playing.value = false // the music rings out on its own
    return
  }
  frame = requestAnimationFrame(tick)
}

async function startAudio() {
  if (muted.value || volume.value === 0) return
  try {
    const src = walkthrough.value.music[riff.value]
    if (!src) return
    if (audio.src !== new URL(src, window.location.href).href) audio.src = src
    audio.volume = volume.value
    audio.currentTime = time.value
    await audio.play()
  } catch {
    // No audio device or playback refused: the walkthrough runs silent.
  }
}

function play() {
  if (finished.value) {
    time.value = 0
    riff.value = nextRiff(props.id, walkthrough.value.music.length)
  } else if (settled) {
    time.value = steps.value[step.value].start
  }
  settled = false
  playing.value = true
  lastTick = performance.now()
  void startAudio()
  cancelAnimationFrame(frame)
  frame = requestAnimationFrame(tick)
}

function pause() {
  playing.value = false
  cancelAnimationFrame(frame)
  audio.pause()
}

function seek(index: number) {
  const clamped = Math.max(0, Math.min(steps.value.length - 1, index))
  time.value = steps.value[clamped].start
  lastTick = performance.now()
  if (playing.value && audioLive()) {
    audio.currentTime = time.value
  }
  if (!playing.value) {
    // Show the step's settled state when stepping by hand.
    time.value = Math.min(steps.value[clamped].end - 0.01, steps.value[clamped].start + spb.value * 10)
    settled = true
  }
}

function remember() {
  storeMusicPreference({ muted: muted.value, volume: volume.value })
}

function toggleMute() {
  muted.value = !muted.value
  // Unmuting at zero volume would stay silent: come back at the default.
  if (!muted.value && volume.value === 0) volume.value = defaultMusicVolume
  remember()
  if (muted.value) audio.pause()
  else if (playing.value) void startAudio()
}

function setVolume(event: Event) {
  volume.value = Number((event.target as HTMLInputElement).value) / 100
  audio.volume = volume.value
  // Moving the slider is a request to hear the music.
  if (muted.value && volume.value > 0) {
    muted.value = false
    if (playing.value) void startAudio()
  }
  if (volume.value === 0) audio.pause()
  else if (playing.value && audio.paused && !muted.value) void startAudio()
  remember()
}

const volumePercent = computed(() => Math.round(volume.value * 100))
const silent = computed(() => muted.value || volume.value === 0)

watch(() => props.id, () => {
  pause()
  time.value = 0
  riff.value = nextRiff(props.id, walkthrough.value.music.length)
})

onBeforeUnmount(() => {
  pause()
  audio.removeAttribute('src')
  audio.load()
})
</script>

<template>
  <figure class="walkthrough rounded-[1rem] border surface-panel-soft" style="border-color: var(--app-border)" :data-walkthrough="id" :data-step="step + 1">
    <div class="flex flex-wrap items-center justify-between gap-2 border-b px-4 py-2.5" style="border-color: var(--app-border)">
      <p class="text-sm font-semibold tracking-tight" style="color: var(--app-text)">{{ t(key(`guides.walkthroughs.${id}.title`)) }}</p>
      <div class="flex items-center gap-2" data-walkthrough-music>
        <button
          class="wt-music inline-flex h-7 w-7 items-center justify-center rounded-full"
          :class="silent ? '' : 'wt-music-on'"
          type="button"
          :aria-pressed="!silent"
          :title="silent ? t('guides.walkthrough.unmute') : t('guides.walkthrough.mute')"
          :aria-label="silent ? t('guides.walkthrough.unmute') : t('guides.walkthrough.mute')"
          data-walkthrough-mute
          @click="toggleMute"
        >
          <svg aria-hidden="true" viewBox="0 0 20 20" fill="currentColor" class="h-4 w-4">
            <path d="M9.4 3.2a.75.75 0 0 1 1.1.66v12.28a.75.75 0 0 1-1.1.66L5.6 13.5H3.5A1.5 1.5 0 0 1 2 12V8a1.5 1.5 0 0 1 1.5-1.5h2.1l3.8-3.3Z" />
            <path v-if="!silent" d="M13.2 6.3a.75.75 0 0 1 1.06 0 5.25 5.25 0 0 1 0 7.4.75.75 0 1 1-1.06-1.06 3.75 3.75 0 0 0 0-5.28.75.75 0 0 1 0-1.06Zm2.12-2.12a.75.75 0 0 1 1.06 0 8.25 8.25 0 0 1 0 11.64.75.75 0 0 1-1.06-1.06 6.75 6.75 0 0 0 0-9.52.75.75 0 0 1 0-1.06Z" />
            <path v-else d="M13.47 7.47a.75.75 0 0 1 1.06 0L16 8.94l1.47-1.47a.75.75 0 1 1 1.06 1.06L17.06 10l1.47 1.47a.75.75 0 1 1-1.06 1.06L16 11.06l-1.47 1.47a.75.75 0 1 1-1.06-1.06L14.94 10l-1.47-1.47a.75.75 0 0 1 0-1.06Z" />
          </svg>
        </button>
        <input
          class="wt-volume w-24"
          :class="silent ? 'wt-volume-off' : ''"
          type="range"
          min="0"
          max="100"
          step="5"
          :value="silent ? 0 : volumePercent"
          :aria-label="t('guides.walkthrough.volume')"
          :title="t('guides.walkthrough.volume')"
          data-walkthrough-volume
          @input="setVolume"
        />
      </div>
    </div>

    <component :is="scenes[id]" class="wt-scene" :step="step" :beat="beat" :time="time" :spb="spb" :caption="caption" />

    <p class="wt-caption min-h-[3.5rem] px-4 pt-2 text-sm leading-6" style="color: var(--app-text)" aria-live="polite">{{ caption }}</p>

    <div class="flex flex-wrap items-center justify-between gap-3 px-4 pb-3 pt-1">
      <div class="flex items-center gap-1.5" role="tablist">
        <button
          v-for="(_, index) in steps"
          :key="index"
          class="wt-dot h-2.5 rounded-full transition-all"
          :class="index === step ? 'wt-dot-on w-6' : 'w-2.5'"
          type="button"
          role="tab"
          :aria-selected="index === step"
          :aria-label="t('guides.walkthrough.stepOf', { current: index + 1, total: steps.length })"
          @click="seek(index)"
        />
        <span class="ml-2 text-xs text-muted">{{ t('guides.walkthrough.stepOf', { current: step + 1, total: steps.length }) }}</span>
      </div>
      <div class="flex items-center gap-2">
        <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium" type="button" :disabled="step === 0" @click="seek(step - 1)">
          {{ t('guides.walkthrough.back') }}
        </button>
        <button class="wt-play rounded-[0.75rem] px-4 py-1.5 text-sm font-semibold" type="button" data-walkthrough-play @click="playing ? pause() : play()">
          {{ playing ? t('guides.walkthrough.pause') : finished ? t('guides.walkthrough.replay') : t('guides.walkthrough.play') }}
        </button>
        <button class="button-secondary rounded-[0.75rem] px-3 py-1.5 text-sm font-medium" type="button" :disabled="step === steps.length - 1" @click="seek(step + 1)">
          {{ t('guides.walkthrough.next') }}
        </button>
      </div>
    </div>
  </figure>
</template>

<style scoped>
.wt-scene {
  display: block;
  width: 100%;
  max-height: 17rem;
  padding: 0.75rem 1rem 0;
}
.wt-dot {
  background: var(--app-border-strong);
}
.wt-dot-on {
  background: var(--app-accent);
}
.wt-play {
  background: var(--app-accent);
  color: #fff;
}
.wt-play:hover {
  background: var(--app-accent-strong);
}
.wt-music {
  color: var(--app-text-muted);
  border: 1px solid var(--app-border);
}
.wt-volume {
  accent-color: var(--app-accent);
}
.wt-volume-off {
  opacity: 0.55;
}
.wt-music-on {
  color: var(--app-accent-soft-text);
  border-color: var(--app-accent);
  background: var(--app-accent-soft);
}
</style>
