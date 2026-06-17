<script setup lang="ts">
import { computed, ref, watch } from 'vue'

import ProfileShell from '../ProfileShell.vue'
import type { PerformanceSessionSummary, PerformanceSessionsSnapshot } from '../../lib/commands'
import type { PerformanceMonitorProfileConfig, RegisteredApplication, VectorXRConfig } from '../../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
  sessionsSnapshot: PerformanceSessionsSnapshot | null
  sessionsLoading: boolean
}>()

const emit = defineEmits<{
  addPerformanceProfile: []
  removePerformanceProfile: [index: number]
  syncPerformanceProfileName: [index: number]
  refreshSessions: []
  openLogDirectory: [path: string]
}>()

const selectedProfileIndex = ref(0)
const selectedSessionId = ref<string | null>(null)

const profileWarnings = computed(() => {
  const warnings = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  props.config.modules.performance.profiles.forEach((profile, index) => {
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

const sessions = computed(() => props.sessionsSnapshot?.sessions ?? [])
const enabledProfileCount = computed(() => props.config.modules.performance.profiles.filter((profile) => profile.enabled).length)
const totalSessionCount = computed(() => sessions.value.length)
const selectedProfile = computed(() => props.config.modules.performance.profiles[selectedProfileIndex.value] ?? null)

function sessionsForProfile(profile: PerformanceMonitorProfileConfig): PerformanceSessionSummary[] {
  const ids = new Set(profile.applicationIds)
  return sessions.value.filter((session) => ids.has(session.applicationId))
}

const selectedProfileSessions = computed(() => {
  if (!selectedProfile.value) {
    return []
  }

  return sessionsForProfile(selectedProfile.value)
})

const selectedSession = computed(() => {
  if (selectedProfileSessions.value.length === 0) {
    return null
  }

  return selectedProfileSessions.value.find((session) => session.id === selectedSessionId.value) ?? selectedProfileSessions.value[0]
})

const chartMetrics = computed(() => {
  const session = selectedSession.value
  if (!session) {
    return []
  }

  return [
    { label: 'Avg', value: session.frameAvgMs },
    { label: 'P95', value: session.frameP95Ms },
    { label: 'P99', value: session.frameP99Ms },
    { label: 'Worst', value: session.frameMaxMs },
    { label: 'CPU', value: session.cpuSpanAvgMs },
  ]
})

const chartMax = computed(() => {
  const values = chartMetrics.value.map((metric) => metric.value)
  const target = selectedSession.value?.targetFrameMs ?? 0
  return Math.max(1, target, ...values) * 1.1
})

watch(
  () => props.config.modules.performance.profiles.length,
  (length) => {
    if (selectedProfileIndex.value >= length) {
      selectedProfileIndex.value = Math.max(0, length - 1)
    }
  },
)

watch(selectedProfileSessions, (profileSessions) => {
  if (profileSessions.length === 0) {
    selectedSessionId.value = null
    return
  }

  if (!profileSessions.some((session) => session.id === selectedSessionId.value)) {
    selectedSessionId.value = profileSessions[0].id
  }
})

function profileSessionCount(profile: PerformanceMonitorProfileConfig) {
  return sessionsForProfile(profile).length
}

function latestSession(profile: PerformanceMonitorProfileConfig) {
  return sessionsForProfile(profile)[0] ?? null
}

function openProfileSessions(index: number) {
  selectedProfileIndex.value = index
  selectedSessionId.value = latestSession(props.config.modules.performance.profiles[index])?.id ?? null
}

function openSessionLog(session: PerformanceSessionSummary | null) {
  if (!session) {
    return
  }

  emit('openLogDirectory', session.sourceLogPath)
}

function formatNumber(value: number, digits = 1) {
  if (!Number.isFinite(value) || value <= 0) {
    return '-'
  }

  return value.toFixed(digits)
}

function formatInteger(value: number) {
  return Number.isFinite(value) ? Math.round(value).toLocaleString() : '0'
}

function formatDate(seconds: number) {
  if (!seconds) {
    return 'Unknown time'
  }

  return new Date(seconds * 1000).toLocaleString()
}

function targetLabel(session: PerformanceSessionSummary | null) {
  if (!session || session.targetFrameMs <= 0) {
    return 'Target unknown'
  }

  return `${formatNumber(session.targetFrameMs)} ms @ ${formatNumber(session.targetHz, 0)} Hz`
}

function excludedSampleCount(session: PerformanceSessionSummary | null) {
  if (!session) {
    return 0
  }

  return session.excludedFrameSamples + session.excludedCpuSpanSamples
}

function formatSecondsFromMs(value: number) {
  if (!Number.isFinite(value) || value <= 0) {
    return '0s'
  }

  return `${(value / 1000).toFixed(value >= 10000 ? 0 : 1)}s`
}

function sessionTone(session: PerformanceSessionSummary | null) {
  if (!session || session.frameSamples === 0) {
    return 'chip-idle'
  }

  if (session.overBudgetPercent >= 10 || session.shouldRenderFalse > 0) {
    return 'chip-warning'
  }

  return 'chip-success'
}

function sessionLabel(session: PerformanceSessionSummary | null) {
  if (!session || session.frameSamples === 0) {
    if (excludedSampleCount(session) > 0) {
      return 'Loading only'
    }

    return 'No samples'
  }

  if (session.overBudgetPercent >= 10 || session.shouldRenderFalse > 0) {
    return 'Needs review'
  }

  return 'Stable'
}

function metricWidth(value: number) {
  return `${Math.max(0, Math.min(100, (value / chartMax.value) * 100))}%`
}
</script>

<template>
  <div class="space-y-4">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Runtime Stats</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Performance Monitoring</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            Enable frame collection per registered game, then review recorded session summaries from VectorXR runtime logs.
          </p>
        </div>
        <div class="flex flex-wrap items-center gap-2">
          <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">
            {{ enabledProfileCount }} enabled
          </span>
          <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-idle">
            {{ totalSessionCount }} sessions
          </span>
          <button
            class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
            type="button"
            :disabled="sessionsLoading"
            @click="$emit('refreshSessions')"
          >
            {{ sessionsLoading ? 'Refreshing' : 'Refresh Sessions' }}
          </button>
        </div>
      </div>
    </article>

    <section class="grid gap-4 xl:grid-cols-[minmax(0,1.05fr)_minmax(360px,0.95fr)]">
      <div class="space-y-3">
        <div class="flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 surface-panel">
          <div>
            <h2 class="text-lg font-semibold tracking-tight">Collection Profiles</h2>
            <p class="text-sm text-muted">Performance monitoring is only active for games assigned to an enabled profile.</p>
          </div>
          <button
            class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
            type="button"
            @click="$emit('addPerformanceProfile')"
          >
            Add Profile
          </button>
        </div>

        <ProfileShell
          v-for="(profile, index) in config.modules.performance.profiles"
          :key="`performance-profile-${index}`"
          :index="index"
          :profile="profile"
          :applications="applications"
          module-label="Performance Monitoring"
          :warnings="profileWarnings.get(index)"
          @remove="$emit('removePerformanceProfile', index)"
          @sync-name="$emit('syncPerformanceProfileName', index)"
        >
          <template #badges>
            <span
              v-if="profile.enabled"
              class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent"
            >
              {{ profile.collectionMode }}
            </span>
            <span
              v-if="profile.enabled"
              class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-idle"
            >
              {{ profileSessionCount(profile) }} sessions
            </span>
          </template>

          <div class="grid gap-3 lg:grid-cols-2">
            <label class="block rounded-[1rem] border p-4 surface-panel-soft">
              <span class="mb-1.5 block text-sm font-medium">Collection Mode</span>
              <select v-model="profile.collectionMode" class="app-input w-full rounded-[0.75rem] px-4 py-2.5">
                <option value="summary">Summary</option>
                <option value="diagnostic">Diagnostic</option>
              </select>
            </label>
            <label class="block rounded-[1rem] border p-4 surface-panel-soft">
              <span class="mb-1.5 block text-sm font-medium">Retained Sessions</span>
              <input
                v-model.number="profile.retentionSessions"
                class="app-input w-full rounded-[0.75rem] px-4 py-2.5"
                min="1"
                max="200"
                step="1"
                type="number"
              />
            </label>
          </div>

          <div class="mt-3 flex flex-wrap items-center justify-between gap-3 rounded-[0.9rem] border px-4 py-3 surface-panel-strong">
            <div>
              <p class="text-sm font-semibold tracking-tight">Recorded Sessions</p>
              <p class="text-xs text-muted">
                {{ profileSessionCount(profile) }} matching {{ profileSessionCount(profile) === 1 ? 'session' : 'sessions' }}
              </p>
            </div>
            <div class="flex flex-wrap items-center gap-2">
              <button
                class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium"
                type="button"
                :disabled="profileSessionCount(profile) === 0"
                @click="openProfileSessions(index)"
              >
                Open Sessions
              </button>
              <button
                class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium"
                type="button"
                :disabled="!latestSession(profile)"
                @click="openSessionLog(latestSession(profile))"
              >
                Open Log
              </button>
            </div>
          </div>
        </ProfileShell>

        <div
          v-if="config.modules.performance.profiles.length === 0"
          class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
        >
          No performance profiles yet. Add a profile to collect frame stats for a registered game.
        </div>
      </div>

      <aside class="space-y-3">
        <div class="rounded-[1rem] border px-4 py-3 surface-panel">
          <div class="flex flex-wrap items-center justify-between gap-3">
            <div>
              <p class="eyebrow text-xs uppercase tracking-[0.2em]">Session Review</p>
              <h2 class="mt-1 text-lg font-semibold tracking-tight">
                {{ selectedProfile?.name ?? 'No profile selected' }}
              </h2>
            </div>
            <select
              v-if="selectedProfileSessions.length > 0"
              v-model="selectedSessionId"
              class="app-input rounded-[0.75rem] px-3 py-2 text-sm"
            >
              <option v-for="session in selectedProfileSessions" :key="session.id" :value="session.id">
                {{ formatDate(session.recordedUnixSeconds) }}
              </option>
            </select>
          </div>
        </div>

        <div v-if="!selectedProfile" class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
          Select or create a collection profile to review performance sessions.
        </div>

        <div v-else-if="!selectedSession" class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
          No recorded sessions found for the selected profile.
        </div>

        <div v-else class="space-y-3">
          <article class="rounded-[1rem] border p-4 surface-panel">
            <div class="flex flex-wrap items-start justify-between gap-3">
              <div>
                <p class="text-sm font-semibold tracking-tight">{{ formatDate(selectedSession.recordedUnixSeconds) }}</p>
                <p class="mt-1 text-xs text-muted">{{ selectedSession.sourceLogName }}:{{ selectedSession.sourceLineNumber }}</p>
              </div>
              <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="sessionTone(selectedSession)">
                {{ sessionLabel(selectedSession) }}
              </span>
            </div>

            <div class="mt-4 grid gap-2 sm:grid-cols-2">
              <div class="rounded-[0.85rem] border px-3 py-3 surface-panel-soft">
                <p class="text-xs text-muted">Target</p>
                <p class="mt-1 text-lg font-semibold">{{ targetLabel(selectedSession) }}</p>
              </div>
              <div class="rounded-[0.85rem] border px-3 py-3 surface-panel-soft">
                <p class="text-xs text-muted">Over Budget</p>
                <p class="mt-1 text-lg font-semibold">{{ formatNumber(selectedSession.overBudgetPercent) }}%</p>
              </div>
              <div class="rounded-[0.85rem] border px-3 py-3 surface-panel-soft">
                <p class="text-xs text-muted">Samples</p>
                <p class="mt-1 text-lg font-semibold">{{ formatInteger(selectedSession.frameSamples) }}</p>
              </div>
              <div class="rounded-[0.85rem] border px-3 py-3 surface-panel-soft">
                <p class="text-xs text-muted">Excluded</p>
                <p class="mt-1 text-lg font-semibold">{{ formatInteger(excludedSampleCount(selectedSession)) }}</p>
              </div>
              <div class="rounded-[0.85rem] border px-3 py-3 surface-panel-soft">
                <p class="text-xs text-muted">Duration</p>
                <p class="mt-1 text-lg font-semibold">{{ formatNumber(selectedSession.durationSeconds, 0) }}s</p>
              </div>
            </div>
          </article>

          <article class="rounded-[1rem] border p-4 surface-panel">
            <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
              <h3 class="text-base font-semibold tracking-tight">Frame Timing</h3>
              <span class="text-xs text-muted">Target {{ formatNumber(selectedSession.targetFrameMs) }} ms</span>
            </div>

            <div class="space-y-3">
              <div v-for="metric in chartMetrics" :key="metric.label" class="grid grid-cols-[3.5rem_minmax(0,1fr)_4.5rem] items-center gap-3">
                <span class="text-xs font-semibold uppercase tracking-[0.14em] text-soft">{{ metric.label }}</span>
                <div class="h-3 overflow-hidden rounded-full surface-panel-soft">
                  <div class="h-full rounded-full" style="background: var(--app-accent)" :style="{ width: metricWidth(metric.value) }" />
                </div>
                <span class="text-right text-sm tabular-nums">{{ formatNumber(metric.value) }} ms</span>
              </div>
            </div>
          </article>

          <article class="rounded-[1rem] border p-4 surface-panel">
            <div class="mb-3 flex flex-wrap items-center justify-between gap-2">
              <h3 class="text-base font-semibold tracking-tight">Session Log</h3>
              <button
                class="button-secondary rounded-[0.65rem] px-3 py-1.5 text-xs font-medium"
                type="button"
                @click="openSessionLog(selectedSession)"
              >
                Open Log
              </button>
            </div>
            <div class="grid gap-2 text-sm">
              <div class="flex justify-between gap-3">
                <span class="text-muted">Reason</span>
                <span class="font-medium">{{ selectedSession.reason || '-' }}</span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">Should render false</span>
                <span class="font-medium">{{ formatInteger(selectedSession.shouldRenderFalse) }}</span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">Loading threshold</span>
                <span class="font-medium">{{ formatNumber(selectedSession.loadingGapThresholdMs) }} ms</span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">Excluded frame gaps</span>
                <span class="font-medium">
                  {{ formatInteger(selectedSession.excludedFrameSamples) }}
                  / {{ formatSecondsFromMs(selectedSession.excludedFrameDurationMs) }}
                </span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">Excluded CPU spans</span>
                <span class="font-medium">
                  {{ formatInteger(selectedSession.excludedCpuSpanSamples) }}
                  / {{ formatSecondsFromMs(selectedSession.excludedCpuSpanDurationMs) }}
                </span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">CPU span avg</span>
                <span class="font-medium">{{ formatNumber(selectedSession.cpuSpanAvgMs) }} ms</span>
              </div>
              <div class="flex justify-between gap-3">
                <span class="text-muted">Worst frame</span>
                <span class="font-medium">{{ formatNumber(selectedSession.frameMaxMs) }} ms</span>
              </div>
            </div>
          </article>
        </div>
      </aside>
    </section>
  </div>
</template>
