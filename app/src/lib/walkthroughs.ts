import type { GuideId } from './guides'

// Stepped, animated walkthroughs on the Guides page. Each step is a whole
// number of bars at the walkthrough's tempo, because the background music is
// scored to the steps: every step change lands on a downbeat, and the player
// can seek the music to any step. The scores live in the generated-art repo
// (vxr-guides/music/score.py, WALKTHROUGHS); keep bpm and bars in step with it.
//
// music lists riffs on the walkthrough's theme: the same tempo, steps and cues
// with different harmony, grooves and tone. The player plays the next one each
// time, so the theme doesn't wear thin.
export interface Walkthrough {
  bpm: number
  bars: number[]
  music: string[]
}

// Every riff, by file name: guide-<walkthrough>-<riff>.opus.
const musicFiles = import.meta.glob<string>('../assets/guides/guide-*.opus', { eager: true, query: '?url', import: 'default' })
function riffs(id: GuideId): string[] {
  return ['a', 'b', 'c'].map((riff) => musicFiles[`../assets/guides/guide-${id}-${riff}.opus`]).filter((url): url is string => Boolean(url))
}

// The timings here and in score.py must match.
const timing: Partial<Record<GuideId, { bpm: number; bars: number[] }>> = {
  start: { bpm: 96, bars: [2, 2, 3, 3, 3, 3] },
  quadviews: { bpm: 92, bars: [2, 3, 3, 3, 3] },
  turbo: { bpm: 104, bars: [3, 3, 3, 3, 3] },
  framegen: { bpm: 100, bars: [3, 3, 3, 4, 3] },
  pivot: { bpm: 100, bars: [3, 3, 3, 3, 3] },
  depth: { bpm: 92, bars: [3, 3, 3, 3] },
  osd: { bpm: 104, bars: [2, 3, 3, 3, 3] },
}

export const walkthroughs: Partial<Record<GuideId, Walkthrough>> = Object.fromEntries(
  Object.entries(timing).map(([id, t]) => [id, { ...t, music: riffs(id as GuideId) }]),
)

export interface WalkthroughStep {
  start: number
  end: number
}

export function secondsPerBeat(walkthrough: Walkthrough): number {
  return 60 / walkthrough.bpm
}

export function walkthroughSteps(walkthrough: Walkthrough): WalkthroughStep[] {
  const bar = 4 * secondsPerBeat(walkthrough)
  const steps: WalkthroughStep[] = []
  let start = 0
  for (const bars of walkthrough.bars) {
    steps.push({ start, end: start + bars * bar })
    start += bars * bar
  }
  return steps
}

// The step at time t (the last step once the walkthrough has ended).
export function stepAt(steps: WalkthroughStep[], t: number): number {
  for (let index = steps.length - 1; index >= 0; index--) {
    if (t >= steps[index].start) return index
  }
  return 0
}

// Music volume and mute, remembered on this PC. Muted until the viewer turns
// the music on.
export interface MusicPreference {
  muted: boolean
  volume: number // 0 to 1
}

const musicKey = 'vectorxr-guide-music'
const volumeKey = 'vectorxr-guide-volume'
export const defaultMusicVolume = 0.6

function read(key: string): string | null {
  try {
    return window.localStorage.getItem(key)
  } catch {
    return null
  }
}

function write(key: string, value: string) {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    // Not remembered; the defaults apply next time.
  }
}

export function loadMusicPreference(): MusicPreference {
  const volume = Number(read(volumeKey))
  return {
    muted: read(musicKey) !== 'on',
    volume: read(volumeKey) !== null && Number.isFinite(volume) ? Math.max(0, Math.min(1, volume)) : defaultMusicVolume,
  }
}

export function storeMusicPreference(preference: MusicPreference) {
  write(musicKey, preference.muted ? 'off' : 'on')
  write(volumeKey, String(Math.round(preference.volume * 100) / 100))
}

// The riff to play next for a walkthrough, rotating through its music.
export function nextRiff(id: GuideId, count: number): number {
  const key = `vectorxr-guide-riff-${id}`
  const last = Number(read(key))
  const next = Number.isInteger(last) && read(key) !== null ? (last + 1) % Math.max(1, count) : 0
  write(key, String(next))
  return next
}

// Squeezes a label into its box when a translation runs long: an estimate of
// the text's width (CJK glyphs are about square), and a textLength only when
// it would overflow.
export function fitText(label: string, max: number, fontSize: number): number | undefined {
  let width = 0
  for (const char of label) width += /[\p{Script=Han}\p{Script=Hiragana}\p{Script=Katakana}\p{Script=Hangul}]/u.test(char) ? fontSize : fontSize * 0.56
  return width > max ? max : undefined
}
