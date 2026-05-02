<script setup lang="ts">
import { healthChipClass, type HealthCheckItem, type HealthSummary } from '../lib/health'

defineProps<{
  open: boolean
  summary: HealthSummary
}>()

defineEmits<{
  close: []
  exportDebug: []
}>()

function checkClass(check: HealthCheckItem): string {
  switch (check.state) {
    case 'pass':
      return 'chip-success'
    case 'warn':
      return 'chip-warning'
    case 'fail':
      return 'chip-danger'
    default:
      return 'chip-idle'
  }
}
</script>

<template>
  <div v-if="open" class="fixed inset-0 z-50 flex items-center justify-center bg-black/45 px-4 py-6 backdrop-blur-sm">
    <div class="flex max-h-[85vh] w-full max-w-[860px] flex-col overflow-hidden rounded-[1.5rem] border surface-panel-strong">
      <header class="flex shrink-0 flex-wrap items-start justify-between gap-4 border-b p-5" style="border-color: var(--app-border)">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">System Health</p>
          <div class="mt-2 flex flex-wrap items-center gap-3">
            <h2 class="text-2xl font-semibold tracking-tight">Health Check</h2>
            <span class="rounded-full px-3 py-1 text-xs font-semibold uppercase tracking-[0.16em]" :class="healthChipClass(summary.overall)">
              {{ summary.label }}
            </span>
          </div>
          <p class="mt-2 max-w-2xl text-sm leading-6 text-muted">{{ summary.description }}</p>
        </div>
        <button class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium" type="button" @click="$emit('close')">
          Close
        </button>
      </header>

      <section class="min-h-0 overflow-y-auto p-5">
        <div class="grid gap-3 md:grid-cols-2">
          <article v-for="check in summary.checks" :key="check.id" class="rounded-[0.9rem] border p-3 surface-panel">
            <div class="flex items-start justify-between gap-3">
              <div>
                <h3 class="text-sm font-semibold tracking-tight">{{ check.label }}</h3>
                <p class="mt-1 text-xs leading-5 text-muted">{{ check.detail }}</p>
              </div>
              <span class="shrink-0 rounded-full px-2.5 py-1 text-xs font-semibold uppercase tracking-[0.14em]" :class="checkClass(check)">
                {{ check.state }}
              </span>
            </div>
          </article>
        </div>

        <div class="mt-4 grid gap-3 md:grid-cols-2">
          <article class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">VectorXR Layer</p>
            <p class="mt-2 break-all font-mono text-xs">{{ summary.vectorXrLayer?.manifestPath || 'Not found' }}</p>
          </article>
          <article class="rounded-[0.9rem] border p-3 surface-panel-soft">
            <p class="text-xs font-semibold uppercase tracking-[0.16em] text-soft">Last Seen App</p>
            <p class="mt-2 font-mono text-xs">{{ summary.lastSeenApp?.exe || 'No OpenXR app observed yet' }}</p>
          </article>
        </div>
      </section>

      <footer class="flex shrink-0 flex-wrap items-center justify-between gap-3 border-t p-5" style="border-color: var(--app-border)">
        <p class="text-sm leading-6 text-muted">Export a debug package when sharing troubleshooting details.</p>
        <button class="button-accent rounded-[0.75rem] px-5 py-2.5 text-sm font-medium" type="button" @click="$emit('exportDebug')">
          Export Debug Information
        </button>
      </footer>
    </div>
  </div>
</template>
