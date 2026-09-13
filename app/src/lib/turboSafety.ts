import type { TurboRecoveryRecord } from './commands'
import type { VectorXRConfig } from './model'

export function turboSafetyBlocks(records: TurboRecoveryRecord[]): TurboRecoveryRecord[] {
  const latest = new Map<string, TurboRecoveryRecord>()
  for (const record of records) {
    if (!latest.has(record.fingerprint) || latest.get(record.fingerprint)!.updatedAtUnixMilliseconds < record.updatedAtUnixMilliseconds) {
      latest.set(record.fingerprint, record)
    }
  }
  return [...latest.values()].sort((a, b) => b.updatedAtUnixMilliseconds - a.updatedAtUnixMilliseconds)
}

export function turboSafetyApplies(config: VectorXRConfig, application: string): boolean {
  if (!config.modules.turbo.interruptedSessionRecovery) return false
  const app = config.applications.find(app => app.enabled && app.match.exe.toLowerCase() === application.toLowerCase())
  const profile = app && config.modules.turbo.profiles.find(profile => profile.enabled && profile.applicationIds.includes(app.id))
  return !profile?.disableSafety
}

export function turboFaultReason(record: TurboRecoveryRecord): string {
  return record.reason || 'The session ended without a clean shutdown. The exact cause and crash time are unknown.'
}
