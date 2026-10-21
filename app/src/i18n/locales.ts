// The languages VectorXR ships. English is the source; the others are
// machine-translated catalogs the community improves over time. The OSD in
// the layer and the installer use the same codes (osd_strings.cpp,
// windows/nsis-hooks.nsh), so add a language in all three places.
export const sourceLocale = 'en'

export const supportedLocales = [
  { code: 'en', nativeName: 'English', englishName: 'English' },
  { code: 'zh-CN', nativeName: '简体中文', englishName: 'Chinese (Simplified)' },
  { code: 'ru', nativeName: 'Русский', englishName: 'Russian' },
  { code: 'es-ES', nativeName: 'Español (España)', englishName: 'Spanish (Spain)' },
  { code: 'pt-BR', nativeName: 'Português (Brasil)', englishName: 'Portuguese (Brazil)' },
  { code: 'de', nativeName: 'Deutsch', englishName: 'German' },
  { code: 'ja', nativeName: '日本語', englishName: 'Japanese' },
  { code: 'fr', nativeName: 'Français', englishName: 'French' },
  { code: 'pl', nativeName: 'Polski', englishName: 'Polish' },
  { code: 'ko', nativeName: '한국어', englishName: 'Korean' },
  { code: 'zh-TW', nativeName: '繁體中文', englishName: 'Chinese (Traditional)' },
] as const

export type LocaleCode = (typeof supportedLocales)[number]['code']

// A test locale built from English at run time: accented and about 30%
// longer, so a string that skipped the catalog or a label that truncates
// stands out. Offered in the picker of development builds only.
export const pseudoLocale = 'en-XA'

export type AppLocale = LocaleCode | typeof pseudoLocale

export function isLocaleCode(value: unknown): value is LocaleCode {
  return supportedLocales.some((locale) => locale.code === value)
}

export function isAppLocale(value: unknown): value is AppLocale {
  return value === pseudoLocale || isLocaleCode(value)
}

// The closest shipped language for a BCP 47 tag such as navigator.language.
// Chinese picks its script: Traditional for zh-Hant, zh-TW, zh-HK and zh-MO,
// Simplified otherwise. Every Spanish and Portuguese region gets the one
// catalog there is. Null when nothing matches.
export function matchLocale(tag: string): LocaleCode | null {
  const normalized = tag.trim().replace(/_/g, '-').toLowerCase()
  if (!normalized) return null
  const [language, ...rest] = normalized.split('-')
  if (language === 'zh') {
    return rest.some((part) => ['hant', 'tw', 'hk', 'mo'].includes(part)) ? 'zh-TW' : 'zh-CN'
  }
  const exact = supportedLocales.find((locale) => locale.code.toLowerCase() === normalized)
  if (exact) return exact.code
  const byLanguage = supportedLocales.find((locale) => locale.code.split('-')[0].toLowerCase() === language)
  return byLanguage ? byLanguage.code : null
}

// The language "System (auto)" means: the first of the user's preferred
// languages that VectorXR ships, else English. In the app the list is
// navigator.languages, which WebView2 takes from the Windows display language.
export function resolveSystemLocale(preferred: readonly string[]): LocaleCode {
  for (const tag of preferred) {
    const match = matchLocale(tag)
    if (match) return match
  }
  return sourceLocale
}
