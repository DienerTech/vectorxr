import { invoke, isTauri } from '@tauri-apps/api/core'

import { storeLanguagePreference, systemLanguages, type LanguagePreference } from './preference.ts'
import { isLocaleCode, resolveSystemLocale, type LocaleCode } from './locales.ts'

// The language before the first pick in Settings: the one chosen in the
// installer, which it saves next to the app (windows/nsis-hooks.nsh). A pick
// that matches Windows' display language stays "System (auto)", so the app
// follows a later change of the display language; another language is kept as
// an explicit choice. Either way it is stored, so the installer's file is read
// once.
export function preferenceForInstallerLanguage(code: string | null, system: readonly string[]): LanguagePreference {
  if (!code || !isLocaleCode(code)) return 'system'
  return code === resolveSystemLocale(system) ? 'system' : (code as LocaleCode)
}

export async function firstRunLanguagePreference(): Promise<LanguagePreference> {
  let installer: string | null = null
  if (isTauri()) {
    try {
      installer = await invoke<string | null>('installer_language')
    } catch {
      // No installer choice: follow the system.
    }
  }
  const preference = preferenceForInstallerLanguage(installer, systemLanguages())
  storeLanguagePreference(preference)
  return preference
}
