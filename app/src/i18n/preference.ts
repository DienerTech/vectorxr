import { isAppLocale, resolveSystemLocale, type AppLocale } from './locales.ts'

export type LanguagePreference = 'system' | AppLocale

// The language picked in Settings, stored on this PC like the theme. Before
// the first pick, the language chosen in the installer (firstRun.ts).
const storageKey = 'vectorxr-language'

export function loadLanguagePreference(): LanguagePreference | null {
  try {
    const stored = typeof window === 'undefined' ? null : window.localStorage.getItem(storageKey)
    if (stored === 'system' || isAppLocale(stored)) return stored
  } catch {
    // Storage unavailable: follow the system.
  }
  return null
}

export function storeLanguagePreference(preference: LanguagePreference) {
  try {
    window.localStorage.setItem(storageKey, preference)
  } catch {
    // Not remembered; the language still applies until the app closes.
  }
}

export function systemLanguages(): readonly string[] {
  if (typeof navigator === 'undefined') return []
  return navigator.languages?.length ? navigator.languages : [navigator.language]
}

export function resolveLanguagePreference(preference: LanguagePreference): AppLocale {
  return preference === 'system' ? resolveSystemLocale(systemLanguages()) : preference
}
