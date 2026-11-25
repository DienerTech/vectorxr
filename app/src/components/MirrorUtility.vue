<script setup lang="ts">
import GuideLink from './GuideLink.vue'
import { computed, onMounted, onUnmounted, ref } from 'vue'

import BindingConflictWarnings from './BindingConflictWarnings.vue'
import BindingListEditor from './BindingListEditor.vue'
import DefaultProfileExclusions from './DefaultProfileExclusions.vue'
import I18nText from './I18nText.vue'
import MirrorSettingsFields from './MirrorSettingsFields.vue'
import ProfileShell from './ProfileShell.vue'
import SetupGuide from './SetupGuide.vue'
import { t, type MessageKey } from '../i18n/index.ts'
import { errorText } from '../lib/errors'
import { loadRuntimeStatus, setRuntimeMirrorVisible, type RuntimeStatusSession } from '../lib/commands'
import { savedBindingConflictWarnings, type MirrorSettings, type RegisteredApplication, type VectorXRConfig } from '../lib/model'
import { mirrorWarnings } from '../lib/validation'

// The stabilized mirror window: a capture tool that is set
// per game like an enhancement, with a Default Profile and custom profiles.
const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  addMirrorProfile: []
  removeMirrorProfile: [index: number]
  syncMirrorProfileName: [index: number]
}>()

const mirror = computed(() => props.config.modules.mirror)
const warnings = computed(() => savedBindingConflictWarnings(props.config, mirror.value.bindings))
// Fill a monitor on with no monitor chosen: advice, never blocks Save.
const fillWarnings = computed(() => mirrorWarnings(props.config))
const notes: Array<{ label: MessageKey; body: MessageKey }> = [
  { label: 'utilities.mirror.notes.obs.label', body: 'utilities.mirror.notes.obs.body' },
  { label: 'utilities.mirror.notes.hdr.label', body: 'utilities.mirror.notes.hdr.body' },
  { label: 'utilities.mirror.notes.separate.label', body: 'utilities.mirror.notes.separate.body' },
  { label: 'utilities.mirror.notes.support.label', body: 'utilities.mirror.notes.support.body' },
  { label: 'utilities.mirror.notes.smoothing.label', body: 'utilities.mirror.notes.smoothing.body' },
  { label: 'utilities.mirror.notes.framegen.label', body: 'utilities.mirror.notes.framegen.body' },
  { label: 'utilities.mirror.notes.window.label', body: 'utilities.mirror.notes.window.body' },
  { label: 'utilities.mirror.notes.fill.label', body: 'utilities.mirror.notes.fill.body' },
  { label: 'utilities.mirror.notes.cost.label', body: 'utilities.mirror.notes.cost.body' },
]

// Live session status relayed by the layer.
const sessions = ref<RuntimeStatusSession[]>([])
const statusError = ref(false)
const pending = ref<{ sessionId: string; visible: boolean; revision?: number; deadline: number }>()
const controlError = ref('')
let disposed = false
let poll: ReturnType<typeof setTimeout> | undefined
async function refresh() {
  try {
    const status = await loadRuntimeStatus()
    if (!disposed) { sessions.value = status.sessions; statusError.value = false }
  } catch { if (!disposed) statusError.value = true }
  if (disposed) return
  const request = pending.value
  if (request?.revision !== undefined) {
    const session = sessions.value.find(item => item.sessionId === request.sessionId)
    if (!statusError.value && session && session.acknowledgedRevision >= request.revision &&
        session.state.mirrorVisible === request.visible) {
      pending.value = undefined
    } else if (Date.now() >= request.deadline) {
      pending.value = undefined
      controlError.value = t('quadviews.tab.diagnostics.notAcknowledged')
    }
  }
  poll = setTimeout(refresh, pending.value ? 250 : 1500)
}
async function toggleWindow(session: RuntimeStatusSession) {
  if (pending.value || session.state.mirrorVisible === undefined) return
  const visible = !session.state.mirrorVisible
  controlError.value = ''
  pending.value = { sessionId: session.sessionId, visible, deadline: Date.now() + 5000 }
  try {
    const revision = await setRuntimeMirrorVisible(session.sessionId, visible)
    if (!disposed && pending.value) pending.value.revision = revision
  } catch (error) {
    if (!disposed) {
      pending.value = undefined
      controlError.value = errorText(error, t('utilities.mirror.status.unavailable'))
    }
  }
}

onMounted(refresh)
onUnmounted(() => { disposed = true; if (poll) clearTimeout(poll) })

// The layer's state, with its English reason shown as it reads.
function sessionStatus(session: RuntimeStatusSession): { label: string; chip: string } {
  const reason = session.state.mirrorReason?.trim()
  const withReason = (status: string) => reason ? t('utilities.mirror.session.withReason', { status, reason }) : status
  // An empty state is a layer that does not report this feature, like an absent one.
  switch (session.state.mirrorState || undefined) {
    case 'active': return { label: withReason(t('utilities.mirror.session.active')), chip: 'chip-success' }
    case 'pending': return { label: withReason(t('utilities.mirror.session.pending')), chip: 'chip-idle' }
    case 'hidden': return { label: withReason(t('utilities.mirror.session.hidden')), chip: 'chip-accent' }
    case 'unsupported': return { label: withReason(t('utilities.mirror.session.unsupported')), chip: 'chip-warning' }
    case 'failed': return { label: withReason(t('utilities.mirror.session.failed')), chip: 'chip-warning' }
    case 'off': return { label: withReason(t('utilities.mirror.session.off')), chip: 'chip-idle' }
    case undefined: return { label: t('utilities.mirror.session.needsLayer'), chip: 'chip-idle' }
    default: return { label: withReason(t('utilities.mirror.session.unknown')), chip: 'chip-idle' }
  }
}

const profileWarnings = computed(() => {
  const found = new Map<number, string[]>()
  const firstProfileByApplication = new Map<string, number>()
  const applicationNameById = new Map(props.applications.map((application) => [application.id, application.name]))

  mirror.value.profiles.forEach((profile, index) => {
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
      found.set(index, [
        ...(found.get(index) ?? []),
        t('utilities.mirror.customProfiles.duplicateApplication', { application: appName, profile: firstIndex + 1 }),
      ])
    }
  })

  return found
})

function settingsSummary(settings: MirrorSettings) {
  return settings.smoothing > 0 ? t('utilities.mirror.summary.smoothing', { value: settings.smoothing }) : t('utilities.mirror.summary.plain')
}
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel" data-utility="mirror">
    <div class="mb-4">
      <h2 class="text-2xl font-semibold tracking-tight">{{ t('utilities.mirror.heading') }}</h2>
      <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ t('utilities.mirror.intro') }}</p>
      <p class="mt-1 max-w-3xl text-xs leading-5 text-muted" role="note">{{ t('utilities.mirror.liveNote') }}</p>
    </div>

    <GuideLink guide="mirror" class="mb-3" />
    <SetupGuide class="mb-3" :title="t('utilities.mirror.notes.title')" :summary="t('utilities.mirror.notes.summary')">
      <ul class="list-disc space-y-1 pl-5 text-sm leading-6">
        <I18nText v-for="note in notes" :key="note.body" :keypath="note.body" tag="li">
          <template #label><strong>{{ t(note.label) }}</strong></template>
        </I18nText>
      </ul>
    </SetupGuide>

    <div
      v-for="warning in fillWarnings"
      :key="warning"
      class="mb-4 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
      role="note"
      data-mirror-warning
    >
      {{ warning }}
    </div>

    <div class="mb-4 flex flex-wrap items-center gap-2 rounded-[1rem] border p-4 surface-panel-soft" data-mirror-status>
      <p class="eyebrow mr-1 text-xs uppercase tracking-[0.18em]">{{ t('utilities.mirror.status.heading') }}</p>
      <span v-if="statusError" class="text-xs text-muted">{{ t('utilities.mirror.status.unavailable') }}</span>
      <span v-else-if="!sessions.length" class="chip-idle rounded-full px-2.5 py-1 text-xs">{{ t('utilities.mirror.status.noSession') }}</span>
      <template v-else>
        <div v-for="session in sessions" :key="session.sessionId" class="flex w-full flex-wrap items-center justify-between gap-2">
          <span class="rounded-full px-2.5 py-1 text-xs" :class="sessionStatus(session).chip">
            <strong class="font-semibold">{{ session.application }}</strong> · {{ sessionStatus(session).label }}
          </span>
          <button class="button-secondary rounded-[0.75rem] px-3.5 py-2 text-sm font-semibold" type="button"
            :disabled="session.state.mirrorVisible === undefined || !!pending" @click="toggleWindow(session)">
            {{ pending?.sessionId === session.sessionId ? t('quadviews.tab.diagnostics.applying')
              : session.state.mirrorVisible ? t('utilities.mirror.status.hide') : t('utilities.mirror.status.show') }}
          </button>
        </div>
      </template>
      <button v-if="statusError || !sessions.length" class="button-secondary ml-auto rounded-[0.75rem] px-3.5 py-2 text-sm font-semibold" type="button" disabled>
        {{ t('utilities.mirror.status.show') }}
      </button>
      <p v-if="controlError" class="w-full text-sm text-muted" role="alert">{{ controlError }}</p>
    </div>

    <div class="mb-4 space-y-4">
      <BindingConflictWarnings :warnings="warnings" />

      <BindingListEditor
        v-model="mirror.bindings"
        :label="t('utilities.mirror.binding.label')"
        :description="t('utilities.mirror.binding.description')"
        :none-text="t('utilities.mirror.binding.none')"
      />
    </div>

    <details class="section-disclosure border-t pt-4" style="border-color: var(--app-border)" open>
      <summary class="flex flex-wrap items-center gap-2">
        <svg aria-hidden="true" class="section-chevron h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
          <path fill-rule="evenodd" d="M7.2 14.8a1 1 0 0 1 0-1.4L10.6 10 7.2 6.6a1 1 0 1 1 1.4-1.4l4.1 4.1a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
        </svg>
        <span class="eyebrow text-xs font-semibold uppercase tracking-[0.24em]">{{ t('utilities.mirror.defaultProfile.heading') }}</span>
        <span class="text-xs text-muted">{{ t('utilities.mirror.defaultProfile.appliesTo') }}</span>
        <span v-if="mirror.enabled" class="ml-auto rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em] chip-accent">
          {{ settingsSummary(mirror.defaults) }}
        </span>
      </summary>
      <div class="mt-3 flex flex-wrap items-center gap-2">
        <label class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium">
          <input v-model="mirror.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ mirror.enabled ? t('utilities.mirror.defaultProfile.toggleOn') : t('utilities.mirror.defaultProfile.toggleOff') }}
        </label>
        <DefaultProfileExclusions :applications="applications" :profiles="mirror.profiles" />
      </div>
      <p v-if="!mirror.enabled" class="mt-2 text-xs text-muted">
        {{ t('utilities.mirror.defaultProfile.offNote') }}
      </p>
      <MirrorSettingsFields v-else class="mt-3" :settings="mirror.defaults" />
    </details>
  </article>

  <section class="mt-4 space-y-3" data-utility="mirror-profiles">
    <div class="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-3 rounded-[1rem] border px-4 py-3 shadow-panel backdrop-blur surface-panel-strong">
      <div class="min-w-0 flex-1">
        <h2 class="text-lg font-semibold tracking-tight">{{ t('utilities.mirror.customProfiles.heading') }}</h2>
        <p class="text-sm text-muted">{{ t('utilities.mirror.customProfiles.description') }}</p>
      </div>
      <button
        class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium"
        type="button"
        @click="$emit('addMirrorProfile')"
      >
        {{ t('utilities.mirror.customProfiles.add') }}
      </button>
    </div>

    <ProfileShell
      v-for="(profile, index) in mirror.profiles"
      :key="`mirror-profile-${index}`"
      :index="index"
      :profile="profile"
      :applications="applications"
      :module-label="t('utilities.mirror.heading')"
      :warnings="profileWarnings.get(index)"
      @remove="$emit('removeMirrorProfile', index)"
      @sync-name="$emit('syncMirrorProfileName', index)"
    >
      <template #badges>
        <span v-if="profile.enabled" class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em] chip-accent">{{ settingsSummary(profile.settings) }}</span>
      </template>

      <MirrorSettingsFields :settings="profile.settings" />
    </ProfileShell>

    <div
      v-if="mirror.profiles.length === 0"
      class="rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft"
    >
      {{ t('utilities.mirror.customProfiles.empty') }}
    </div>
  </section>
</template>
