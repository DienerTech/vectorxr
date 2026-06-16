<script setup lang="ts">
import { ref } from 'vue'

import AppPicker from './AppPicker.vue'
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
}>()

defineEmits<{
  remove: []
  syncName: []
}>()

const editingName = ref(false)
</script>

<template>
  <article class="rounded-[1rem] border p-4 shadow-panel transition" :class="profile.enabled ? 'surface-panel' : 'surface-panel-soft'">
    <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
      <div class="flex flex-wrap items-center gap-2">
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Profile {{ index + 1 }}</p>
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
          aria-label="Edit profile name"
          @click="editingName = true"
        >
          <svg aria-hidden="true" class="h-3.5 w-3.5" viewBox="0 0 20 20" fill="currentColor">
            <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
          </svg>
        </button>
        <slot name="badges" />
      </div>

      <div class="flex flex-wrap items-center gap-2">
        <label
          class="pill-toggle inline-flex items-center gap-3 rounded-full px-4 py-2 text-sm font-medium"
          :title="profile.enabled ? `This profile applies custom ${moduleLabel} settings to its assigned applications.` : `This profile is ignored at runtime.`"
        >
          <input v-model="profile.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
          {{ profile.enabled ? 'Profile On' : 'Profile Off' }}
        </label>
        <button class="button-secondary rounded-[0.75rem] px-3.5 py-2 text-sm font-medium" type="button" @click="$emit('remove')">
          Remove
        </button>
      </div>
    </div>

    <div
      v-if="warnings && warnings.length > 0"
      class="mb-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
      style="border-color: var(--app-border)"
    >
      <p class="font-medium">{{ warningTitle ?? 'Profile conflict' }}</p>
      <ul class="mt-2 space-y-1">
        <li v-for="warning in warnings" :key="warning">{{ warning }}</li>
      </ul>
    </div>

    <label class="block">
      <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
        Applications
        <span
          :title="`Registered applications this profile applies to. The first active matching profile wins.`"
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

    <div v-if="!profile.enabled" class="mt-3 rounded-[0.9rem] border px-4 py-3 text-sm leading-6 surface-panel-strong">
      This profile is off and has no effect. Its applications fall back to the default {{ moduleLabel }} profile.
    </div>
    <div v-else class="mt-3">
      <slot />
    </div>
  </article>
</template>
