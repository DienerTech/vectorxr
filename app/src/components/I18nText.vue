<script setup lang="ts">
import { computed } from 'vue'

import { t, type MessageKey, type MessageParams } from '../i18n/index.ts'

// A message with markup inside it: each {name} placeholder that is not in
// `params` is filled by the slot of that name, so a sentence with a link or a
// <strong> stays one message translators can reorder.
//   <I18nText keypath="about.support" tag="p"><template #link><a ...>{{ t('about.discord') }}</a></template></I18nText>
const props = defineProps<{ keypath: MessageKey; params?: MessageParams; tag?: string }>()

const parts = computed(() =>
  t(props.keypath, props.params)
    .split(/(\{\w+\})/)
    .filter((part) => part !== '')
    .map((part) => (/^\{\w+\}$/.test(part) ? { slot: part.slice(1, -1), text: '' } : { slot: '', text: part })),
)
</script>

<template>
  <component :is="tag ?? 'span'">
    <template v-for="(part, index) in parts" :key="index">
      <slot v-if="part.slot" :name="part.slot" />
      <template v-else>{{ part.text }}</template>
    </template>
  </component>
</template>
