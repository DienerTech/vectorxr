<script setup lang="ts">
import type { RegisteredApplication } from '../lib/model'

defineProps<{
  applications: RegisteredApplication[]
}>()

defineEmits<{
  add: []
  remove: [index: number]
}>()
</script>

<template>
  <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
    <div class="flex flex-wrap items-center justify-between gap-3">
      <div>
        <p class="eyebrow text-xs uppercase tracking-[0.24em]">Applications</p>
        <h2 class="mt-2 text-2xl font-semibold tracking-tight">Application registry</h2>
        <p class="mt-2 text-sm leading-6 text-muted">
          Register titles once, then reuse them across module profiles.
        </p>
      </div>

      <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('add')">
        Add Application
      </button>
    </div>

    <div v-if="applications.length > 0" class="mt-4 space-y-3">
      <div v-for="(application, index) in applications" :key="application.id" class="rounded-[1rem] border p-4 surface-panel-soft">
        <div class="mb-3 flex flex-wrap items-center justify-between gap-3">
          <div>
            <p class="text-base font-semibold tracking-tight">{{ application.name || 'Unnamed application' }}</p>
            <p class="mt-1 font-mono text-xs text-muted">{{ application.id }}</p>
          </div>

          <div class="flex flex-wrap items-center gap-3">
            <label class="pill-toggle inline-flex items-center gap-2 rounded-full px-3 py-1.5 text-xs font-medium">
              <input v-model="application.enabled" class="h-4 w-4 accent-depthxr-copper" type="checkbox" />
              Enabled
            </label>
            <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('remove', index)">
              Remove
            </button>
          </div>
        </div>

        <div class="grid gap-3 md:grid-cols-2">
          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Application Name</span>
            <input v-model="application.name" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" placeholder="DCS" type="text" />
          </label>

          <label class="block">
            <span class="mb-1.5 block text-sm font-medium">Executable</span>
            <input v-model="application.match.exe" class="app-input w-full rounded-[0.75rem] px-4 py-2.5" placeholder="DCS.exe" type="text" />
          </label>
        </div>
      </div>
    </div>

    <div v-else class="mt-4 rounded-[1rem] border border-dashed px-6 py-7 text-center text-sm surface-panel-soft">
      No applications registered yet. Add one before creating module profiles.
    </div>
  </article>
</template>
