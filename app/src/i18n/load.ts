import { addMessages, hasMessages, setActiveLocale } from './index.ts'
import { pseudoLocale, sourceLocale, type AppLocale } from './locales.ts'
import { resolveLanguagePreference, type LanguagePreference } from './preference.ts'

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
