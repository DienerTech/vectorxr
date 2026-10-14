import type { VectorXRConfig } from './model'

// Diagnostic modes a tester may leave on, named in a tag after the OSD's
// brand as the layer does (OpenXrLayer::BuildOsdSnapshot): debug logging, and
// a Frame Generation diagnostic view where Frame Generation is on.
const viewLabels = { flow: 'FLOW VIEW', confidence: 'CONFIDENCE VIEW', tint: 'TINT VIEW' } as const

export function osdDiagnostics(config: VectorXRConfig): string {
  const tags: string[] = []
  if (config.core.logLevel === 'debug') tags.push('DEBUG LOG')
  const framegen = config.modules.framegen
  const framegenOn = framegen.enabled || framegen.profiles.some(profile => profile.enabled)
  if (framegenOn && framegen.diagnosticView !== 'off') tags.push(viewLabels[framegen.diagnosticView])
  return tags.join(' · ')
}

// The tag's width in the OSD's pixels: about what the layer measures for its
// 17-pixel semibold text, plus 10 pixels either side.
export function osdDiagnosticsTagWidth(text: string): number {
  return text ? Math.round(text.length * 10.5) + 20 : 0
}
