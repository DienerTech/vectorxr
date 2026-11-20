<script setup lang="ts">
import { ref } from 'vue'

import { hasMessage, t } from '../i18n/index.ts'
import { moduleLabels, moduleStateForApplication, type ModuleId, type RegisteredApplication, type VectorXRConfig } from '../lib/model'

const props = defineProps<{
  config: VectorXRConfig
  applications: RegisteredApplication[]
}>()

defineEmits<{
  add: []
  remove: [index: number]
  openModule: [moduleId: ModuleId, applicationId: string]
}>()

const moduleIds: ModuleId[] = ['quadviews', 'turbo', 'framegen', 'pivotxr', 'depthxr']

function moduleChip(applicationId: string, moduleId: ModuleId) {
  const moduleState = moduleStateForApplication(props.config, moduleId, applicationId)
  const labelKey = `model.modules.${moduleId}`
  const module = hasMessage(labelKey) ? t(labelKey) : moduleLabels[moduleId]

  switch (moduleState.kind) {
    case 'default-off':
      return {
        text: t('registry.applications.chip.defaultOff', { module }),
        chipClass: 'chip-idle',
        title: t('registry.applications.chip.defaultOffTitle', { module }),
      }
    case 'custom': {
      const profile = moduleState.profileName ?? ''
      return {
        text: t('registry.applications.chip.custom', { module, profile }),
        chipClass: 'chip-success',
        title: t('registry.applications.chip.customTitle', { module, profile }),
      }
    }
    default:
      return {
        text: t('registry.applications.chip.default', { module }),
        chipClass: 'chip-idle',
        title: t('registry.applications.chip.defaultTitle', { module }),
      }
  }
}

const editingApplicationId = ref<string | null>(null)

function finishNameEdit() {
  editingApplicationId.value = null
}
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div>
        <h2 class="text-2xl font-semibold tracking-tight">{{ t('registry.applications.title') }}</h2>
        <p class="mt-2 text-sm leading-6 text-muted">
          {{ t('registry.applications.intro') }}
        </p>
      </div>

      <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('add')">
        {{ t('registry.applications.add') }}
      </button>
    </div>

    <div v-if="applications.length > 0" class="mt-4 grid gap-3 xl:grid-cols-2">
      <div v-for="(application, index) in applications" :key="application.id" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div class="flex flex-wrap items-center gap-2">
            <p v-if="editingApplicationId !== application.id" class="text-base font-semibold tracking-tight">{{ application.name || t('registry.applications.unnamed') }}</p>
            <input
              v-else
              v-model="application.name"
              class="app-input w-full max-w-sm rounded-[0.75rem] px-3 py-2 text-base font-semibold tracking-tight"
              placeholder="DCS"
              type="text"
              @blur="finishNameEdit"
              @keydown.enter="finishNameEdit"
            />
            <button
              v-if="editingApplicationId !== application.id"
              class="button-secondary inline-flex h-8 w-8 items-center justify-center rounded-[0.5rem]"
              type="button"
              :aria-label="t('registry.applications.editName')"
              @click="editingApplicationId = application.id"
            >
              <svg aria-hidden="true" class="h-4 w-4" viewBox="0 0 20 20" fill="currentColor">
                <path d="M13.92 2.87a2.2 2.2 0 0 1 3.11 3.11l-.72.72-3.11-3.11.72-.72Zm-1.7 1.7 3.11 3.11-8.7 8.7-3.44.33.33-3.44 8.7-8.7Z" />
              </svg>
            </button>
          </div>

          <div class="flex flex-wrap items-center gap-3">
            <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('remove', index)">
              {{ t('common.remove') }}
            </button>
          </div>
        </div>

        <label class="block">
          <span class="mb-1.5 flex items-center gap-1.5 text-sm font-medium">
            {{ t('registry.applications.executable') }}
            <span
              :title="t('registry.applications.executableHelp')"
              class="cursor-help select-none text-xs text-muted"
              >ⓘ</span
            >
          </span>
          <!-- i18n-ignore: an example file name, not text -->
          <input v-model="application.match.exe" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" placeholder="DCS.exe" type="text" />
        </label>

        <div class="mt-3 flex flex-wrap items-center gap-2">
          <button
            v-for="moduleId in moduleIds"
            :key="moduleId"
            class="rounded-full px-3 py-1 text-xs font-medium transition hover:brightness-110"
            :class="moduleChip(application.id, moduleId).chipClass"
            type="button"
            :title="moduleChip(application.id, moduleId).title"
            @click="$emit('openModule', moduleId, application.id)"
          >
            {{ moduleChip(application.id, moduleId).text }}
          </button>
        </div>
      </div>
    </div>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      {{ t('registry.applications.empty') }}
    </div>
  </article>
</template>
