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
}>()
</script>

<template>
  <div class="sticky bottom-0 z-30 pt-4">
    <div class="mx-auto max-w-[1500px] rounded-[1.8rem] border border-black/10 bg-depthxr-ink/96 p-4 text-white shadow-[0_25px_90px_rgba(17,19,22,0.34)] backdrop-blur">
      <div class="flex flex-col gap-4 lg:flex-row lg:items-center lg:justify-between">
        <div>
          <p class="text-sm font-semibold tracking-tight">
            {{ dirty ? 'Unsaved changes are ready to apply.' : 'All changes are saved.' }}
          </p>
          <p class="mt-1 text-sm text-white/70">{{ status || (dirty ? 'Save or discard the current working copy.' : 'The working copy matches the last saved config.') }}</p>
        </div>

        <div class="flex flex-wrap gap-3">
          <button
            class="rounded-full bg-depthxr-copper px-5 py-2.5 text-sm font-medium text-white transition hover:brightness-110 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="disabled || !dirty"
            type="button"
            @click="$emit('save')"
          >
            {{ saving ? 'Saving...' : 'Save Changes' }}
          </button>
          <button
            class="rounded-full border border-white/15 px-5 py-2.5 text-sm font-medium text-white transition hover:border-white/35 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving || !dirty"
            type="button"
            @click="$emit('discard')"
          >
            Discard
          </button>
          <button
            class="rounded-full border border-white/15 px-5 py-2.5 text-sm font-medium text-white transition hover:border-white/35 disabled:cursor-not-allowed disabled:opacity-50"
            :disabled="loading || saving"
            type="button"
            @click="$emit('reload')"
          >
            Reload From Disk
          </button>
        </div>
      </div>
    </div>
  </div>
</template>
