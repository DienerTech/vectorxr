import { shallowRef } from 'vue'

import { en } from './locales/en/index.ts'
import { isAppLocale, pseudoLocale, resolveSystemLocale, sourceLocale, type AppLocale, type LocaleCode } from './locales.ts'
import { pseudoMessage } from './pseudo.ts'

// A small typed t() rather than vue-i18n: the catalogs are plain nested JSON,
// keys are checked against the English catalog by vue-tsc, plurals use
// Intl.PluralRules, and lib code (health, validation, tray) translates the
// same way components do, also under the node test runner.
//
// Messages: "{name}" is replaced by params.name. A key with variants named
// key_one, key_few, key_other (CLDR plural categories) is a plural: t(key,
// { count }) picks the variant for the language, key_other when the language
// has no variant for that category. A placeholder with no param is left in
// place; I18nText fills those with slots (links, bold text).

export type Messages = typeof en

type PluralCategory = 'zero' | 'one' | 'two' | 'few' | 'many' | 'other'
type Leaves<T, Prefix extends string = ''> = {
  [K in keyof T & string]: T[K] extends string ? `${Prefix}${K}` : Leaves<T[K], `${Prefix}${K}.`>
}[keyof T & string]
type WithoutPlural<K> = K extends `${infer Base}_${PluralCategory}` ? Base : K

export type MessageKey = WithoutPlural<Leaves<Messages>>
export type MessageParams = Record<string, string | number>

const pluralCategories: readonly string[] = ['zero', 'one', 'two', 'few', 'many', 'other']

// Nested catalog -> "a.b.c" keys. Files named _meta (catalog metadata) are not
// messages.
export function flattenMessages(messages: unknown, prefix = '', out = new Map<string, string>()): Map<string, string> {
  if (typeof messages === 'string') {
    out.set(prefix, messages)
  } else if (messages && typeof messages === 'object') {
    for (const [key, value] of Object.entries(messages)) {
      if (key.startsWith('_')) continue
      flattenMessages(value, prefix ? `${prefix}.${key}` : key, out)
    }
  }
  return out
}

const catalogs = new Map<string, Map<string, string>>([[sourceLocale, flattenMessages(en)]])
// Each catalog's _meta.json: who made it.
const machineTranslated = new Map<string, boolean>([[sourceLocale, false]])

// The language messages are shown in. Reactive: anything rendering t() follows
// a change of language.
export const activeLocale = shallowRef<AppLocale>(sourceLocale)
// Counts catalogs added, so text shown in a language other than the app's
// (tIn) updates once that language's catalog arrives.
const catalogsAdded = shallowRef(0)

export function addMessages(locale: AppLocale, messages: unknown) {
  catalogs.set(locale, flattenMessages(messages))
  catalogsAdded.value++
  const meta = (messages as { _meta?: { machineTranslated?: unknown } } | null)?._meta
  machineTranslated.set(locale, meta?.machineTranslated !== false)
}

// Whether the language's catalog says it was made by machine (and not yet
// reviewed), so the app can say so.
export function isMachineTranslated(locale: AppLocale): boolean {
  return machineTranslated.get(locale) ?? false
}

export function hasMessages(locale: AppLocale): boolean {
  return locale === pseudoLocale || catalogs.has(locale)
}

// Shows `locale` from now on. Its catalog must have been added; a language
// with no catalog falls back to English.
export function setActiveLocale(locale: AppLocale) {
  activeLocale.value = isAppLocale(locale) && hasMessages(locale) ? locale : sourceLocale
  if (typeof document !== 'undefined') {
    // The page language also picks the CJK glyph forms (ja vs zh vs ko).
    document.documentElement.lang = activeLocale.value === pseudoLocale ? 'en' : activeLocale.value
  }
}

const pluralRules = new Map<string, Intl.PluralRules>()
function pluralCategory(locale: AppLocale, count: number): string {
  const tag = locale === pseudoLocale ? sourceLocale : locale
  let rules = pluralRules.get(tag)
  if (!rules) {
    rules = new Intl.PluralRules(tag)
    pluralRules.set(tag, rules)
  }
  return rules.select(count)
}

function lookup(locale: AppLocale, key: string, count: number | undefined): string | undefined {
  const catalog = catalogs.get(locale === pseudoLocale ? sourceLocale : locale)
  if (!catalog) return undefined
  if (count !== undefined) {
    const variant = catalog.get(`${key}_${pluralCategory(locale, count)}`) ?? catalog.get(`${key}_other`)
    if (variant !== undefined) return variant
  }
  return catalog.get(key)
}

export function interpolate(message: string, params?: MessageParams): string {
  if (!params) return message
  return message.replace(/\{(\w+)\}/g, (placeholder, name: string) =>
    Object.prototype.hasOwnProperty.call(params, name) ? String(params[name]) : placeholder,
  )
}

export function t(key: MessageKey, params?: MessageParams): string {
  return tIn(activeLocale.value, key, params)
}

// t() in `locale` rather than the app's language, for text that shows in its
// own language (the OSD preview in the OSD's). English until that catalog has
// been added (loadLanguage).
export function tIn(locale: AppLocale, key: MessageKey, params?: MessageParams): string {
  void catalogsAdded.value
  const count = typeof params?.count === 'number' ? params.count : undefined
  let message = lookup(locale, key, count)
  if (message === undefined && locale !== sourceLocale) message = lookup(sourceLocale, key, count)
  if (message === undefined) return key
  if (locale === pseudoLocale) message = pseudoMessage(message)
  return interpolate(message, params)
}

// Whether `key` names a message (or a plural) in English. For keys built from
// data, such as a module id, where a missing message has a fallback.
export function hasMessage(key: string): key is MessageKey {
  const catalog = catalogs.get(sourceLocale)!
  return catalog.has(key) || pluralCategories.some((category) => catalog.has(`${key}_${category}`))
}

// Runs `build` with messages in `locale`, for text that must stay in one
// language whatever the app shows (the debug package is English).
export function withLocale<T>(locale: AppLocale, build: () => T): T {
  const previous = activeLocale.value
  activeLocale.value = locale
  try {
    return build()
  } finally {
    activeLocale.value = previous
  }
}

// The app's language as the layer reads it (core.osd.appLanguage): the
// pseudo-locale is the app's test, and the OSD draws English for it.
export function layerLanguage(): LocaleCode {
  return activeLocale.value === pseudoLocale ? sourceLocale : activeLocale.value
}

// A list joined the way the language joins one ("A, B and C").
export function formatList(items: readonly string[], type: 'conjunction' | 'disjunction' = 'conjunction'): string {
  const tag = activeLocale.value === pseudoLocale ? sourceLocale : activeLocale.value
  return new Intl.ListFormat(tag, { style: 'long', type }).format(items)
}

export { resolveSystemLocale, type AppLocale, type LocaleCode }
