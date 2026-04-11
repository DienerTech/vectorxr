<script setup lang="ts">
defineProps<{
  dirty: boolean
  saving: boolean
  loading: boolean
  status: string
  disabled: boolean
}>()

defineEmits<{
  save: []
  discard: []
  reload: []
  import: []
}>()
</script>

<template>
  <div>
    <div class="mx-auto max-w-[1500px] rounded-[1.25rem] border p-4 backdrop-blur surface-panel-strong">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <p class="text-sm font-semibold tracking-tight">
            {{ dirty ? 'Unsaved changes are ready to apply.' : 'All changes are saved.' }}
          </p>
          <p class="mt-1 text-sm text-muted">{{ status || (dirty ? 'Save or discard the current working copy.' : 'The working copy matches the last saved config.') }}</p>
        </div>

        <div class="flex flex-wrap gap-3">
          <button
            class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-accent disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="disabled || !dirty"
            type="button"
            @click="$emit('save')"
          >
            {{ saving ? 'Saving...' : 'Save Changes' }}
          </button>
          <button
            class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-secondary disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving || !dirty"
            type="button"
            @click="$emit('discard')"
          >
            Discard
          </button>
          <button
            class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-secondary disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving"
            type="button"
            @click="$emit('reload')"
          >
            Reload From Disk
          </button>
          <button
            class="rounded-[0.75rem] px-5 py-2.5 text-sm font-medium transition button-secondary disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving"
            type="button"
            @click="$emit('import')"
          >
            Import Config
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
