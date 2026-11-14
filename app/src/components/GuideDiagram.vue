<script setup lang="ts">
// Small explainer pictures for the Guides page, drawn in code in the app's
// palette (the same approach as the release videos' explainers). Every label
// comes from the guides catalog, so the pictures translate with the text.
import { t } from '../i18n/index.ts'
import type { GuideId } from '../lib/guides'

defineProps<{ id: GuideId }>()

// Generated frames between game frames: G = game frame, F = generated.
const frameLane = ['G', 'F', 'G', 'F', 'G', 'F', 'G', 'F']

// Squeezes a label into its box when a translation runs long: an estimate of
// the text's width (CJK glyphs are about square), and a textLength only when
// it would overflow.
function fit(label: string, max: number, fontSize: number): number | undefined {
  let width = 0
  for (const char of label) width += /[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/u.test(char) ? fontSize : fontSize * 0.56
  return width > max ? max : undefined
}
</script>

<template>
  <figure class="guide-diagram rounded-[1rem] border p-3 surface-panel-soft" style="border-color: var(--app-border)">
    <!-- What is VectorXR: the frame path through the layer. -->
    <svg v-if="id === 'start' || id === 'anticheat'" viewBox="0 0 640 170" role="img" :aria-label="t('guides.diagram.layer')">
      <g v-for="(box, index) in [
        { x: 8, label: t('guides.diagram.game'), accent: false },
        { x: 168, label: t('guides.diagram.layer'), accent: true },
        { x: 328, label: t('guides.diagram.runtime'), accent: false },
        { x: 488, label: t('guides.diagram.headset'), accent: false },
      ]" :key="index">
        <rect :x="box.x" y="20" width="144" height="56" rx="12" :class="box.accent ? 'gd-accent' : 'gd-box'" />
        <text :x="box.x + 72" y="53" text-anchor="middle" class="gd-label" :class="box.accent ? 'gd-label-strong' : ''" :textLength="fit(box.label, 128, 15)" lengthAdjust="spacingAndGlyphs">{{ box.label }}</text>
        <path v-if="index < 3" :d="`M ${box.x + 148} 48 h 12`" class="gd-arrow" marker-end="url(#gd-head)" />
      </g>
      <rect x="176" y="104" width="200" height="44" rx="10" class="gd-box gd-dashed" />
      <text x="276" y="131" text-anchor="middle" class="gd-small" :textLength="fit(t('guides.diagram.settingsFile'), 184, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.settingsFile') }}</text>
      <path d="M 240 102 V 82" class="gd-arrow" marker-end="url(#gd-head)" />
      <rect x="8" y="104" width="150" height="44" rx="10" class="gd-box gd-dashed" />
      <text x="83" y="131" text-anchor="middle" class="gd-small" :textLength="fit(t('guides.diagram.app'), 136, 13)" lengthAdjust="spacingAndGlyphs">{{ t('guides.diagram.app') }}</text>
      <path d="M 160 126 h 10" class="gd-arrow" marker-end="url(#gd-head)" />
      <defs>
        <marker id="gd-head" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
          <path d="M 0 0 L 10 5 L 0 10 z" class="gd-head" />
        </marker>
      </defs>
    </svg>

    <!-- Quadviews: a sharp inset inside the lower-detail view. -->
    <svg v-else-if="id === 'quadviews'" viewBox="0 0 640 170" role="img" :aria-label="t('guides.diagram.focus')">
      <rect x="40" y="14" width="280" height="140" rx="14" class="gd-box gd-periphery" />
      <rect x="140" y="50" width="100" height="70" rx="10" class="gd-accent" />
      <circle cx="190" cy="85" r="5" class="gd-dot" />
      <text x="360" y="72" class="gd-label gd-label-strong">{{ t('guides.diagram.focus') }}</text>
      <text x="360" y="110" class="gd-label">{{ t('guides.diagram.periphery') }}</text>
      <path d="M 352 67 H 246" class="gd-arrow gd-thin" />
      <path d="M 352 105 H 324" class="gd-arrow gd-thin" />
    </svg>

    <!-- Turbo: less time waiting, sooner rendering. -->
    <svg v-else-if="id === 'turbo'" viewBox="0 0 640 150" role="img" :aria-label="t('guides.diagram.wait')">
      <text x="8" y="40" class="gd-small">{{ t('guides.diagram.before') }}</text>
      <g v-for="n in 3" :key="`b${n}`">
        <rect :x="96 + (n - 1) * 176" y="24" width="72" height="24" rx="6" class="gd-wait" />
        <rect :x="172 + (n - 1) * 176" y="24" width="96" height="24" rx="6" class="gd-accent" />
      </g>
      <text x="8" y="96" class="gd-small">{{ t('guides.diagram.after') }}</text>
      <g v-for="n in 4" :key="`a${n}`">
        <rect :x="96 + (n - 1) * 132" y="80" width="20" height="24" rx="6" class="gd-wait" />
        <rect :x="120 + (n - 1) * 132" y="80" width="104" height="24" rx="6" class="gd-accent" />
      </g>
      <rect x="96" y="124" width="14" height="12" rx="3" class="gd-wait" />
      <text x="116" y="134" class="gd-small">{{ t('guides.diagram.wait') }}</text>
      <rect x="320" y="124" width="14" height="12" rx="3" class="gd-accent" />
      <text x="340" y="134" class="gd-small">{{ t('guides.diagram.render') }}</text>
    </svg>

    <!-- Frame Generation: generated frames fill the refreshes between game frames. -->
    <svg v-else-if="id === 'framegen'" viewBox="0 0 640 150" role="img" :aria-label="t('guides.diagram.generatedFrame')">
      <text x="8" y="22" class="gd-small">{{ t('guides.diagram.refresh') }}</text>
      <g v-for="(kind, index) in frameLane" :key="index">
        <path :d="`M ${40 + index * 72} 34 v 10`" class="gd-arrow gd-thin" />
        <rect :x="16 + index * 72" y="52" width="48" height="48" rx="8" :class="kind === 'G' ? 'gd-box' : 'gd-accent gd-dashed'" />
      </g>
      <rect x="16" y="120" width="14" height="14" rx="3" class="gd-box" />
      <text x="36" y="132" class="gd-small">{{ t('guides.diagram.realFrame') }}</text>
      <rect x="240" y="120" width="14" height="14" rx="3" class="gd-accent gd-dashed" />
      <text x="260" y="132" class="gd-small">{{ t('guides.diagram.generatedFrame') }}</text>
    </svg>

    <!-- Pivot: a small head turn gives a larger view turn. -->
    <svg v-else-if="id === 'pivot'" viewBox="0 0 640 180" role="img" :aria-label="t('guides.diagram.viewTurns')">
      <circle cx="200" cy="150" r="16" class="gd-box" />
      <path d="M 200 150 L 270 100" class="gd-arrow" marker-end="url(#gd-head-p)" />
      <path d="M 200 150 L 330 150" class="gd-arrow gd-accent-stroke" marker-end="url(#gd-head-p)" />
      <path d="M 200 70 A 80 80 0 0 1 256 93" class="gd-arc" />
      <path d="M 200 40 A 110 110 0 0 1 310 150" class="gd-arc gd-accent-stroke" />
      <text x="290" y="92" class="gd-small">{{ t('guides.diagram.headTurn') }}</text>
      <text x="340" y="154" class="gd-label gd-label-strong">{{ t('guides.diagram.viewTurns') }}</text>
      <defs>
        <marker id="gd-head-p" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
          <path d="M 0 0 L 10 5 L 0 10 z" class="gd-head" />
        </marker>
      </defs>
    </svg>

    <!-- Depth: two eyes converge on the depth plane. -->
    <svg v-else-if="id === 'depth'" viewBox="0 0 640 170" role="img" :aria-label="t('guides.diagram.depthPlane')">
      <circle cx="270" cy="150" r="9" class="gd-box" />
      <circle cx="370" cy="150" r="9" class="gd-box" />
      <path d="M 270 142 L 320 40 M 370 142 L 320 40" class="gd-arrow gd-thin" />
      <path d="M 160 40 H 480" class="gd-arrow gd-accent-stroke gd-dashed-line" />
      <text x="490" y="44" class="gd-label gd-label-strong">{{ t('guides.diagram.depthPlane') }}</text>
      <text x="200" y="154" text-anchor="end" class="gd-small">{{ t('guides.diagram.leftEye') }}</text>
      <text x="390" y="154" class="gd-small">{{ t('guides.diagram.rightEye') }}</text>
    </svg>

    <!-- On-Screen Display: a panel in the view. -->
    <svg v-else-if="id === 'osd'" viewBox="0 0 640 170" role="img" :aria-label="t('guides.diagram.osdPanel')">
      <rect x="140" y="10" width="360" height="150" rx="16" class="gd-box gd-periphery" />
      <rect x="170" y="30" width="120" height="60" rx="8" class="gd-accent" />
      <path d="M 182 52 h 60 M 182 70 h 90" class="gd-arrow gd-accent-stroke" />
      <text x="320" y="64" class="gd-label gd-label-strong">{{ t('guides.diagram.osdPanel') }}</text>
    </svg>
  </figure>
</template>

<style scoped>
.guide-diagram svg {
  display: block;
  width: 100%;
  max-height: 11rem;
}
.gd-box {
  fill: var(--app-surface-strong);
  stroke: var(--app-border-strong);
  stroke-width: 1.5;
}
.gd-accent {
  fill: var(--app-accent-soft);
  stroke: var(--app-accent);
  stroke-width: 1.5;
}
.gd-wait {
  fill: var(--app-text-soft);
  fill-opacity: 0.28;
  stroke: var(--app-text-soft);
  stroke-width: 1;
  stroke-dasharray: 3 3;
}
.gd-periphery {
  fill: var(--app-idle-soft);
}
.gd-dashed {
  stroke-dasharray: 5 4;
}
.gd-arrow {
  fill: none;
  stroke: var(--app-text-muted);
  stroke-width: 2;
  stroke-linecap: round;
}
.gd-thin {
  stroke-width: 1.25;
}
.gd-dashed-line {
  stroke-dasharray: 8 6;
}
.gd-accent-stroke {
  stroke: var(--app-accent);
}
.gd-arc {
  fill: none;
  stroke: var(--app-text-muted);
  stroke-width: 1.5;
  stroke-dasharray: 3 4;
}
.gd-head {
  fill: var(--app-text-muted);
}
.gd-dot {
  fill: var(--app-accent);
}
.gd-label {
  fill: var(--app-text);
  font-size: 15px;
}
.gd-label-strong {
  font-weight: 600;
}
.gd-small {
  fill: var(--app-text-muted);
  font-size: 13px;
}
</style>
