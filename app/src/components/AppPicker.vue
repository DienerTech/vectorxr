<script setup lang="ts">
import type { RegisteredApplication } from '../lib/model'

const props = defineProps<{
  applications: RegisteredApplication[]
  modelValue: string[]
}>()

const emit = defineEmits<{
  'update:modelValue': [ids: string[]]
  change: []
}>()

function toggle(id: string) {
  const next = props.modelValue.includes(id) ? props.modelValue.filter((value) => value !== id) : [...props.modelValue, id]
  emit('update:modelValue', next)
  emit('change')
}
</script>

<template>
  <div v-if="applications.length > 0" class="flex flex-wrap gap-2">
    <button
      v-for="application in applications"
      :key="application.id"
      class="app-pill inline-flex items-center gap-1.5 px-3 py-1.5 text-xs font-semibold transition"
      :class="modelValue.includes(application.id) ? 'app-pill-active' : ''"
      type="button"
      :title="application.match.exe"
      @click="toggle(application.id)"
    >
      <svg v-if="modelValue.includes(application.id)" aria-hidden="true" class="h-3 w-3" viewBox="0 0 20 20" fill="currentColor">
        <path fill-rule="evenodd" d="M16.7 5.3a1 1 0 0 1 0 1.4l-7.5 7.5a1 1 0 0 1-1.4 0l-4-4a1 1 0 1 1 1.4-1.4l3.3 3.29 6.8-6.79a1 1 0 0 1 1.4 0Z" clip-rule="evenodd" />
      </svg>
      {{ application.name }}
    </button>
  </div>
  <p v-else class="text-sm text-muted">No applications registered yet. Add one on the Application Registry tab first.</p>
</template>
