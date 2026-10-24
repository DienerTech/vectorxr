<script setup lang="ts">
import { computed, ref } from 'vue'

import AppPicker from './AppPicker.vue'
import { t } from '../i18n/index.ts'
import type { RegisteredApplication } from '../lib/model'

interface ProfileLike {
  name: string
  enabled: boolean
  applicationIds: string[]
}

const props = defineProps<{
  index: number
  profile: ProfileLike
  applications: RegisteredApplication[]
  moduleLabel: string
  warningTitle?: string
  warnings?: string[]
  enabledChangeNote?: string
  // When set, shows priority reorder controls. Array order is priority order:
  // earlier profiles win contested bindings for a shared application.
  reorderable?: boolean
  profileCount?: number
}>()

defineEmits<{
  remove: []
  syncName: []
  move: [direction: -1 | 1]
}>()

const editingName = ref(false)
const hasNoAssignedApplications = computed(
  () => props.profile.enabled && props.profile.applicationIds.length === 0,
)
</script>

<template>
  <article
    class="rounded-[1rem] border p-4 shadow-panel transition"
    :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft opacity-60'"
    :style="hasNoAssignedApplications ? { borderColor: 'var(--app-warning-text)' } : undefined"
  >
    <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
      <div class="flex flex-wrap items-center gap-2">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">{{ t('app.profile.number', { number: index + 1 }) }}</p>
        <h3 v-if="!editingName" class="text-lg font-semibold tracking-tight">{{ profile.name }}</h3>
        <input
          v-else
          v-model="profile.name"
          class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-1.5 text-lg font-semibold tracking-tight"
          placeholder="DCS"
          type="text"
          @blur="editingName = false"
          @keydown.enter="editingName = false"
        />
        <button
          v-if="!editingName"
          class="button-secondary inline-flex h-7 w-7 items-center justify-center rounded-[0.5rem]"
          type="button"
          :aria-label="t('app.profile.editName')"
          @click="editingName = true"
        >
          <svg aria-hidden="true" class="h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
          </svg>
        </button>
        <span
          v-if="hasNoAssignedApplications"
          class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em] chip-warning"
        >
          {{ t('app.profile.noApplications') }}
        </span>
        <slot name="badges" />
      </div>

      <div class="flex flex-wrap items-center gap-2">
        <div v-if="reorderable" class="flex items-center gap-1" :title="t('app.profile.priority')">
          <button
            class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem] disabled:cursor-not-allowed disabled:opacity-35"
            type="button"
            :aria-label="t('app.profile.moveUp')"
            :disabled="index === 0"
            @click="$emit('move', -1)"
          >
            <svg aria-hidden="true" class="h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
              <path fill-rule="evenodd" d="M5.2 12.8a1 1 0 0 1 0-1.4l4.1-4.1a1 1 0 0 1 1.4 0l4.1 4.1a1 1 0 1 1-1.4 1.4L10 9.4l-3.4 3.4a1 1 0 0 1-1.4 0Z" clip-rule="evenodd" />
            </svg>
          </button>
          <button
            class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem] disabled:cursor-not-allowed disabled:opacity-35"
            type="button"
            :aria-label="t('app.profile.moveDown')"
            :disabled="profileCount !== undefined && index >= profileCount - 1"
            @click="$emit('move', 1)"
          >
            <svg aria-hidden="true" class="h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
              <path fill-rule="evenodd" d="M14.8 7.2a1 1 0 0 1 0 1.4l-4.1 4.1a1 1 0 0 1-1.4 0L5.2 8.6a1 1 0 1 1 1.4-1.4L10 10.6l3.4-3.4a1 1 0 0 1 1.4 0Z" clip-rule="evenodd" />
            </svg>
          </button>
        </div>
        <label
          class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
          :title="enabledChangeNote ?? (profile.enabled ? t('app.profile.enabledTitle', { module: moduleLabel }) : t('app.profile.disabledTitle'))"
        >
          <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ profile.enabled ? t('app.profile.on') : t('app.profile.off') }}
          <span v-if="enabledChangeNote" class="restart-required-mark" :title="enabledChangeNote">&#8635;</span>
        </label>
        <button class="button-secondary rounded-[0.75rem] px-3.5 py-2 text-sm font-medium" type="button" @click="$emit('remove')">
          {{ t('common.remove') }}
        </button>
      </div>
    </div>

    <div
      v-if="warnings && warnings.length > 0"
      class="mb-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
    >
      <p class="font-medium">{{ warningTitle ?? t('app.profile.conflict') }}</p>
      <ul class="mt-2 space-y-1">
        <li v-for="warning in warnings" :key="warning">{{ warning }}</li>
      </ul>
    </div>

    <label class="block">
      <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
        {{ t('app.profile.applications') }}
        <span
          :title="t('app.profile.applicationsHelp')"
          class="cursor-help select-none text-xs text-muted"
          >ⓘ</span
        >
      </span>
      <AppPicker
        :applications="applications"
        :model-value="profile.applicationIds"
        @update:model-value="profile.applicationIds = $event"
        @change="$emit('syncName')"
      />
    </label>

    <div
      v-if="hasNoAssignedApplications"
      class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
      role="note"
    >
      <p class="font-medium">{{ t('app.profile.inactiveTitle') }}</p>
      <p class="mt-1">
        {{ t('app.profile.inactiveBody') }}
      </p>
    </div>

    <div v-if="!profile.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
      {{ t('app.profile.offNote', { module: moduleLabel }) }}
    </div>
    <div v-else class="mt-3">
      <slot />
    </div>
  </article>
</template>
