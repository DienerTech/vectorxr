<script setup lang="ts">
import { openExternalUrl } from "../../lib/commands";
import type { PatchNoteEntry } from "../../lib/patchNotes";

const props = defineProps<{
  latestPatch: PatchNoteEntry;
}>();

defineEmits<{
  openPatchNotes: [];
}>();

const dienerTechUrl = "https://diener.tech";
const githubUrl = "https://github.com/DienerTech/vectorxr";

async function openLink(url: string) {
  await openExternalUrl(url);
}
</script>

<template>
  <div class="space-y-6">
    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <h2 class="text-2xl font-semibold tracking-tight">About VectorXR</h2>
      <p class="mt-3 max-w-3xl text-sm leading-6 text-muted">
        VectorXR is an OpenXR tweak platform with per-game profiles. Adjust stereo depth, configure neck-saver rotation, and manage your OpenXR layers from one place.
Developed by DienerTech. Copyright DienerTech LLC.
      </p>
    </article> 

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-5">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Support</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">Support independent development</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">
            VectorXR is built independently. If it improves your setup, tips help keep development moving.
          </p>
        </div>

        <button
          class="button-accent cursor-not-allowed rounded-[0.75rem] px-4 py-2 text-sm font-medium opacity-60"
          disabled
          type="button"
          title="Ko-fi link coming soon"
        >
          Support on Ko-fi
        </button>
      </div>

      <div class="mt-5 flex flex-wrap gap-3">
        <button
          class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="openLink(dienerTechUrl)"
        >
          DienerTech
        </button>
        <button
          class="button-secondary rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="openLink(githubUrl)"
        >
          VectorXR GitHub
        </button>
      </div>
    </article>

    <article class="rounded-[1.25rem] border p-5 shadow-panel backdrop-blur surface-panel">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div>
          <p class="eyebrow text-xs uppercase tracking-[0.24em]">Latest Patch Notes</p>
          <h2 class="mt-2 text-2xl font-semibold tracking-tight">{{ props.latestPatch.title }}</h2>
          <p class="mt-2 max-w-3xl text-sm leading-6 text-muted">{{ props.latestPatch.summary }}</p>
        </div>

        <span class="chip-accent rounded-full px-3 py-1 text-xs font-medium uppercase tracking-[0.18em]">
          {{ props.latestPatch.version }}
        </span>
      </div>

      <div class="mt-5 flex flex-wrap items-center justify-between gap-3 text-sm">
        <span class="text-muted">{{ props.latestPatch.date }}</span>
        <button
          class="button-accent rounded-[0.75rem] px-4 py-2 text-sm font-medium"
          type="button"
          @click="$emit('openPatchNotes')"
        >
          Read full patch notes
        </button>
      </div>
    </article>
  </div>
</template>
