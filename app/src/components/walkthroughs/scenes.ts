import type { Component } from 'vue'

import type { GuideId } from '../../lib/guides'
import SceneDepth from './SceneDepth.vue'
import SceneFramegen from './SceneFramegen.vue'
import SceneOsd from './SceneOsd.vue'
import ScenePivot from './ScenePivot.vue'
import SceneQuadviews from './SceneQuadviews.vue'
import SceneStart from './SceneStart.vue'
import SceneTurbo from './SceneTurbo.vue'

// The animated scene of each walkthrough. Each gets the step (0-based), the
// beat within it, the time and the seconds per beat, and draws itself. The
// last step of each enhancement's walkthrough shows its page in the app.
export const scenes: Partial<Record<GuideId, Component>> = {
  start: SceneStart,
  quadviews: SceneQuadviews,
  turbo: SceneTurbo,
  framegen: SceneFramegen,
  pivot: ScenePivot,
  depth: SceneDepth,
  osd: SceneOsd,
}
