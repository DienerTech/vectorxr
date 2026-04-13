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
}>()
</script>

<template>
  <div>
    <div class="mx-auto max-w-[1500px] rounded-[1.25rem] border p-4 backdrop-blur surface-panel-strong">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <div class="flex items-center gap-2.5">
            <p class="text-sm font-semibold tracking-tight">
              {{ dirty ? 'Unsaved changes' : 'All changes saved' }}
            </p>
            <span v-if="dirty" class="chip-warning rounded-full px-2.5 py-1 text-xs font-medium">Pending</span>
          </div>
          <p class="mt-1 text-sm text-muted">{{ status || (dirty ? 'Save to apply or discard to revert.' : 'Config matches disk.') }}</p>
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
        </div>
      </div>
    </div>
  </div>
</template>
