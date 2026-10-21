// Checks a translated catalog against English: the same files and keys, the
// plural forms its language needs, the same {placeholders}, the glossary's
// strict terms kept, and metadata saying how it was made.
//
// CLI: node tests/i18nCatalogs.mjs [locale ...]   (every locale without one)
import { existsSync, readdirSync, readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const localesDir = fileURLToPath(new URL('../src/i18n/locales/', import.meta.url))
const glossary = JSON.parse(readFileSync(new URL('../src/i18n/glossary.json', import.meta.url), 'utf8'))
const strictTerms = glossary.terms.filter((entry) => entry.strict).map((entry) => entry.term)
const pluralSuffix = /_(zero|one|two|few|many|other)$/

function readJson(path) {
  const text = readFileSync(path, 'utf8')
  if (text.charCodeAt(0) === 0xfeff) throw new Error(`${path} starts with a byte order mark`)
  return JSON.parse(text)
}

export function flatten(messages, prefix = '', out = new Map()) {
  for (const [key, value] of Object.entries(messages)) {
    const id = prefix ? `${prefix}.${key}` : key
    if (typeof value === 'string') out.set(id, value)
    else if (value && typeof value === 'object') flatten(value, id, out)
    else out.set(id, value)
  }
  return out
}

export function catalogLocales() {
  return readdirSync(localesDir, { withFileTypes: true })
    .filter((entry) => entry.isDirectory() && entry.name !== 'en')
    .map((entry) => entry.name)
}

export function namespaces(locale) {
  return readdirSync(localesDir + locale).filter((name) => name.endsWith('.json') && !name.startsWith('_')).sort()
}

// The categories whole numbers reach in the language: the forms a count
// message needs. (French "many" is for millions, written "1 000 000 de"; its
// message falls back to "other".)
export function requiredPluralCategories(locale) {
  const rules = new Intl.PluralRules(locale)

  const reached = new Set(['other'])
  for (let n = 0; n <= 1000; n += 1) reached.add(rules.select(n))
  return [...reached]
}

function placeholders(message) {
  return new Set([...message.matchAll(/\{(\w+)\}/g)].map((match) => match[1]))
}

function sameSet(a, b) {
  return a.size === b.size && [...a].every((item) => b.has(item))
}

export function checkLocale(locale) {
  const problems = []
  const englishFiles = namespaces('en')
  const files = existsSync(localesDir + locale) ? namespaces(locale) : []
  for (const file of englishFiles) if (!files.includes(file)) problems.push(`${locale}: missing ${file}`)
  for (const file of files) if (!englishFiles.includes(file)) problems.push(`${locale}: ${file} has no English source`)

  const metaPath = `${localesDir}${locale}/_meta.json`
  if (!existsSync(metaPath)) problems.push(`${locale}: missing _meta.json`)
  else {
    const meta = readJson(metaPath)
    if (meta.locale !== locale) problems.push(`${locale}: _meta.json locale is ${meta.locale}`)
    if (typeof meta.machineTranslated !== 'boolean') problems.push(`${locale}: _meta.json must say machineTranslated: true or false`)
  }

  const required = requiredPluralCategories(locale)
  const allowed = new Set(new Intl.PluralRules(locale).resolvedOptions().pluralCategories)
  for (const file of englishFiles.filter((name) => files.includes(name))) {
    const namespace = file.slice(0, -'.json'.length)
    let translated
    try {
      translated = flatten(readJson(`${localesDir}${locale}/${file}`))
    } catch (error) {
      problems.push(`${locale}/${file}: ${error.message}`)
      continue
    }
    const english = flatten(readJson(`${localesDir}en/${file}`))
    // Plain keys and plural bases.
    const base = (key) => key.replace(pluralSuffix, '')
    const englishPlurals = new Set([...english.keys()].filter((key) => pluralSuffix.test(key)).map(base))
    for (const [key, message] of english) {
      const id = `${locale}/${namespace}.${key}`
      if (pluralSuffix.test(key)) continue
      if (!translated.has(key)) problems.push(`${id}: missing`)
      else check(id, message, translated.get(key))
    }
    for (const plural of englishPlurals) {
      const source = english.get(`${plural}_other`) ?? ''
      for (const category of required) {
        const key = `${plural}_${category}`
        if (!translated.has(key)) problems.push(`${locale}/${namespace}.${key}: missing plural form`)
      }
      for (const key of translated.keys()) {
        if (base(key) !== plural || !pluralSuffix.test(key)) continue
        const category = key.match(pluralSuffix)[1]
        if (!allowed.has(category)) problems.push(`${locale}/${namespace}.${key}: ${locale} has no "${category}" form`)
        else check(`${locale}/${namespace}.${key}`, source, translated.get(key), true)
      }
    }
    for (const key of translated.keys()) {
      const known = english.has(key) || (pluralSuffix.test(key) && englishPlurals.has(base(key)))
      if (!known) problems.push(`${locale}/${namespace}.${key}: not in English`)
    }
  }
  return problems

  function check(id, source, message, plural = false) {
    if (typeof message !== 'string' || !message.trim()) {
      problems.push(`${id}: empty or not a string`)
      return
    }
    const want = placeholders(source)
    const got = placeholders(message)
    // A plural form may leave out {count} ("one file").
    if (plural) want.delete('count'), got.delete('count')
    if (!sameSet(want, got)) problems.push(`${id}: placeholders {${[...got].join('}, {')}} instead of {${[...want].join('}, {')}}`)
    for (const term of strictTerms) {
      if (source.includes(term) && !message.includes(term)) problems.push(`${id}: glossary term "${term}" not kept`)
    }
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const locales = process.argv.length > 2 ? process.argv.slice(2) : catalogLocales()
  let total = 0
  for (const locale of locales) {
    const problems = checkLocale(locale)
    total += problems.length
    for (const problem of problems.slice(0, 200)) console.log(problem)
    if (problems.length > 200) console.log(`… and ${problems.length - 200} more`)
    console.log(`${locale}: ${problems.length} problem(s)`)
  }
  process.exitCode = total ? 1 : 0
}
