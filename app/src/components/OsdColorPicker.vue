<script setup lang="ts">
import { computed } from 'vue'
const props = defineProps<{ modelValue: string }>()
const emit = defineEmits<{ 'update:modelValue': [string] }>()
const hsv = computed(() => {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(props.modelValue.slice(i, i + 2), 16) / 255)
  const max = Math.max(r!, g!, b!), min = Math.min(r!, g!, b!), d = max - min
  let h = d === 0 ? 0 : max === r ? ((g! - b!) / d) % 6 : max === g ? (b! - r!) / d + 2 : (r! - g!) / d + 4
  return { h: (h * 60 + 360) % 360, s: max === 0 ? 0 : d / max, v: max }
})
function setColor(h: number, s: number, v: number) {
  const c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c
  const rgb = h < 60 ? [c,x,0] : h < 120 ? [x,c,0] : h < 180 ? [0,c,x] : h < 240 ? [0,x,c] : h < 300 ? [x,0,c] : [c,0,x]
  emit('update:modelValue', '#' + rgb.map(n => Math.round((n + m) * 255).toString(16).padStart(2, '0')).join(''))
}
function wheel(event: PointerEvent) {
  const element = event.currentTarget as HTMLElement
  if (event.type === 'pointerdown') element.setPointerCapture(event.pointerId)
  else if (!element.hasPointerCapture(event.pointerId)) return
  const rect = element.getBoundingClientRect(), x = (event.clientX - rect.left) / rect.width * 2 - 1, y = (event.clientY - rect.top) / rect.height * 2 - 1
  setColor((Math.atan2(y,x) * 180 / Math.PI + 450) % 360, Math.min(1, Math.hypot(x,y)), hsv.value.v || 1)
}
const marker = computed(() => ({ left: `${50 + Math.sin(hsv.value.h * Math.PI / 180) * hsv.value.s * 50}%`, top: `${50 - Math.cos(hsv.value.h * Math.PI / 180) * hsv.value.s * 50}%` }))
function hexInput(event: Event) {
  const value = (event.target as HTMLInputElement).value
  if (/^#[0-9a-f]{6}$/i.test(value)) emit('update:modelValue', value)
}
</script>

<template>
  <div class="flex flex-wrap items-center gap-5">
    <div class="color-wheel" aria-label="Custom accent hue and saturation wheel" @pointerdown="wheel" @pointermove="wheel">
      <i :style="marker" />
    </div>
    <div class="min-w-0 flex-1 space-y-3">
      <label class="block text-xs">Brightness<input :value="hsv.v * 100" aria-label="Custom accent brightness" type="range" min="0" max="100" class="mt-2 h-2 w-full cursor-pointer accent-depthxr-copper" @input="setColor(hsv.h, hsv.s, Number(($event.target as HTMLInputElement).value) / 100)" /></label>
      <label class="block text-xs">Custom accent<input :value="modelValue" type="color" aria-label="Custom accent color" class="app-input manual-input-select mt-2 h-9 w-full rounded-lg" @input="emit('update:modelValue', ($event.target as HTMLInputElement).value)" /></label>
      <label class="block text-xs">Hex color<input :value="modelValue" maxlength="7" class="app-input mt-1 w-full rounded-lg px-2 py-1 font-mono" @input="hexInput" @blur="($event.target as HTMLInputElement).value = modelValue" /></label>
    </div>
  </div>
</template>

<style scoped>
.color-wheel { position: relative; width: 140px; height: 140px; border-radius: 50%; background: radial-gradient(closest-side, white, transparent), conic-gradient(red, yellow, lime, cyan, blue, magenta, red); cursor: crosshair; touch-action: none; flex-shrink: 0; }
.color-wheel i { position: absolute; width: 12px; height: 12px; transform: translate(-50%, -50%); border: 2px solid white; border-radius: 50%; box-shadow: 0 0 0 1px #000; pointer-events: none; }
</style>
