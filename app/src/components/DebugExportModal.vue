<script setup lang="ts">
defineProps<{
  open: boolean
  exporting: boolean
}>()

defineEmits<{
  cancel: []
  confirm: []
}>()
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="w-full max-w-[640px] rounded-[1.5rem] border p-5 surface-panel-strong">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Diagnostics</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Export Debug Information</h2>
          <p class="mt-2 max-w-2xl text-sm leading-6 text-muted">
            VectorXR will package troubleshooting details into a ZIP file you can share during testing.
          </p>
        </div>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('cancel')">
          Close
        </button>
      </div>

      <section class="mt-5 rounded-[1rem] border p-4 surface-panel-soft">
        <p class="text-sm font-semibold tracking-tight">Included</p>
        <div class="mt-3 grid gap-2 text-sm leading-6 md:grid-cols-2">
          <span>Current settings JSON</span>
          <span>Seen OpenXR apps</span>
          <span>Recent VectorXR logs</span>
          <span>OpenXR layer snapshot</span>
          <span>Health check summary</span>
          <span>App version and relevant paths</span>
        </div>
      </section>

      <p class="mt-4 text-xs leading-5 text-muted">
        The package may include local file paths and game executable names. It does not include system environment variables or unrelated files.
      </p>

      <div class="mt-5 flex flex-wrap justify-end gap-3">
        <button class="button-secondary rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('cancel')">
          Cancel
        </button>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" :disabled="exporting" @click="$emit('confirm')">
          {{ exporting ? 'Exporting...' : 'Save ZIP' }}
        </button>
      </div>
    </div>
  </div>
</template>
