import { t } from '../i18n/index.ts'
import type { TurboMetricsSession } from './model'

// Prefer a running capture, then the newest usable A/B run. A quick relaunch
// should not hide a longer comparison already collected for the user.
export function preferredTurboSession(sessions: TurboMetricsSession[], selectedId = ''): TurboMetricsSession | null {
  return sessions.find((session) => session.sessionId === selectedId)
    ?? sessions.find((session) => session.live)
    ?? sessions.find((session) => session.buckets.some((bucket) => bucket.state === 'off' && bucket.seconds >= 30)
      && session.buckets.some((bucket) => (bucket.state === 'async' || bucket.state === 'sequenced') && bucket.seconds >= 30))
    ?? sessions[0] ?? null
}

// Friendly strategy label for a capture session, derived from the layer's
// timingConfiguration string and the Turbo states it recorded, so flights from
// different launches can be compared by strategy.
export function turboSessionStrategy(session: Pick<TurboMetricsSession, 'timingConfiguration' | 'buckets'>): string {
  const timing = session.timingConfiguration ?? ''
  if (timing.startsWith('Experimental')) {
    const value = (key: string) => new RegExp(`${key}=([^;]+)`).exec(timing)?.[1]?.trim()
    const toolkit = value('submit') === 'wait' && value('sample') === 'entry' && value('prediction') === '100%' && value('cap') === '0'
    return toolkit ? t('turbo.strategy.toolkit') : t('turbo.strategy.custom')
  }
  const modes = session.buckets.map(bucket => bucket.state).filter(state => state !== 'off')
  // Unrecognized states are layer data and stay as written.
  const label = [...new Set(modes)].map(mode => mode === 'async' ? t('turbo.strategy.async') : mode === 'sequenced' ? t('turbo.strategy.sequenced') : mode).join(' + ')
  if (!timing && !label) return t('turbo.strategy.unknown')
  return label ? t('turbo.strategy.vectorxrWithModes', { modes: label }) : t('turbo.strategy.vectorxr')
}
