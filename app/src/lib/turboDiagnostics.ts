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
