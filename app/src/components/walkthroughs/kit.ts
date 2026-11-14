// Small helpers the walkthrough scenes share.
export { fitText as fit } from '../../lib/walkthroughs'

export interface SceneProps {
  step: number // 0-based
  beat: number // beats into the step
  time: number // seconds into the walkthrough
  spb: number // seconds per beat
  caption: string
}

// 0 before beat `from`, 1 after beat `to`, eased in between (only in step `at`;
// 1 in later steps, 0 in earlier ones).
export function progress(props: SceneProps, at: number, from: number, to: number): number {
  if (props.step < at) return 0
  if (props.step > at) return 1
  const x = Math.max(0, Math.min(1, (props.beat - from) / Math.max(0.001, to - from)))
  return x * x * (3 - 2 * x)
}

// Whether step `at` has reached beat `from` (true in later steps).
export function reached(props: SceneProps, at: number, from = 0): boolean {
  return props.step > at || (props.step === at && props.beat >= from)
}

export function lerp(a: number, b: number, x: number): number {
  return a + (b - a) * x
}
