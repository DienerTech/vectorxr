<script setup lang="ts">
import type { PivotBindingWarning } from '../lib/model'

defineProps<{
  warnings: PivotBindingWarning[]
}>()
</script>

<template>
  <div
    v-for="warning in warnings"
    :key="`${warning.title}:${warning.message}`"
    class="rounded-[0.9rem] border px-4 py-3 text-sm leading-6 chip-warning"
    style="border-color: var(--app-border)"
    role="note"
  >
    <p class="font-medium">{{ warning.title }}</p>
    <p class="mt-1">{{ warning.message }}</p>
  </div>
</template>
