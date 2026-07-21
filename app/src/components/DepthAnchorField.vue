<script setup lang="ts">
defineProps<{
  value: boolean
  muted?: boolean
}>()

defineEmits<{
  'update:value': [value: boolean]
}>()
</script>

<template>
  <label
    class="flex cursor-pointer items-start gap-3 rounded-[1rem] border p-4 shadow-panel backdrop-blur transition"
    :class="muted ? 'surface-panel-soft opacity-70' : 'surface-panel'"
  >
    <input
      :checked="value"
      class="mt-1 h-4 w-4 shrink-0 accent-depthxr-copper"
      type="checkbox"
      @change="$emit('update:value', ($event.target as HTMLInputElement).checked)"
    />
    <span class="min-w-0">
      <span class="flex flex-wrap items-center gap-2">
        <span class="text-base font-semibold tracking-tight">Depth Anchor</span>
        <span class="chip-warning rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em]">
          Experimental
        </span>
        <span
          class="rounded-full px-2.5 py-1 text-[10px] font-semibold uppercase tracking-[0.14em]"
          :class="value ? 'chip-accent' : 'surface-panel-strong text-muted'"
        >
          {{ value ? 'On' : 'Off' }}
        </span>
      </span>
      <span class="mt-1 block text-[13px] leading-5 text-muted">
        Keep the tuned stereo geometry in the rendered image, then restore headset-native eye poses and FOV for runtime submission. This is the experimental "restore Depth geometry at submission" path; test it per game for comfort and reprojection stability.
      </span>
    </span>
  </label>
</template>
