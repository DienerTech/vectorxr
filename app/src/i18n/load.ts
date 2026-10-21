import { addMessages, hasMessages, setActiveLocale } from './index.ts'
import { isAppLocale, pseudoLocale, resolveSystemLocale, sourceLocale, type AppLocale } from './locales.ts'

// Every catalog but English loads when it is first shown, one chunk per
// language. Bundled with the app: nothing is fetched from the network.
const catalogFiles = import.meta.glob<{ default: Record<string, unknown> }>('./locales/*/*.json')

async function loadCatalog(locale: AppLocale) {
  if (hasMessages(locale)) return
  const prefix = `./locales/${locale}/`
  const files = Object.entries(catalogFiles).filter(([path]) => path.startsWith(prefix))
  const messages: Record<string, unknown> = {}
  await Promise.all(files.map(async ([path, load]) => {
    const name = path.slice(prefix.length, -'.json'.length)
    messages[name] = (await load()).default
  }))
  addMessages(locale, messages)
}

export type LanguagePreference = 'system' | AppLocale

// The language picked in Settings, stored on this PC like the theme. Before
// the first pick, the language chosen in the installer (if any).
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

// Shows `preference` once its catalog has loaded. A catalog that fails to load
// leaves the language as it was.
export async function applyLanguagePreference(preference: LanguagePreference): Promise<AppLocale> {
  const locale = resolveLanguagePreference(preference)
  if (locale !== sourceLocale && locale !== pseudoLocale) {
    try {
      await loadCatalog(locale)
    } catch (error) {
      console.warn(`Failed to load the ${locale} catalog`, error)
      return locale
    }
  }
  setActiveLocale(locale)
  return locale
}
