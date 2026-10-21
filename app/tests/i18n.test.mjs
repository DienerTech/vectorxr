import assert from 'node:assert/strict'
import { readdirSync, readFileSync } from 'node:fs'
import test from 'node:test'

import { addMessages, activeLocale, formatList, hasMessage, interpolate, layerLanguage, setActiveLocale, t, withLocale } from '../src/i18n/index.ts'
import { preferenceForInstallerLanguage } from '../src/i18n/firstRun.ts'
import { matchLocale, pseudoLocale, resolveSystemLocale, supportedLocales } from '../src/i18n/locales.ts'
import { pseudoMessage } from '../src/i18n/pseudo.ts'
import { errorText } from '../src/lib/errors.ts'
import { catalogLocales, checkLocale, flatten, namespaces } from './i18nCatalogs.mjs'
import { findHardcodedText } from './i18nHardcoded.mjs'

const englishMessages = () => new Map(namespaces('en').flatMap((file) =>
  [...flatten(JSON.parse(readFileSync(new URL(`../src/i18n/locales/en/${file}`, import.meta.url), 'utf8')))]
    .map(([key, message]) => [`${file.slice(0, -5)}.${key}`, message])))

function sourceFiles(directory) {
  return readdirSync(new URL(directory, import.meta.url), { withFileTypes: true, recursive: true })
    .filter((entry) => entry.isFile() && /\.(vue|ts)$/.test(entry.name) && !entry.parentPath.replace(/\\/g, '/').includes('/src/i18n'))
    .map((entry) => `${entry.parentPath}/${entry.name}`)
}

test('every shipped language has a catalog in step with English', () => {
  const shipped = supportedLocales.map((locale) => locale.code).filter((code) => code !== 'en').sort()
  assert.deepEqual(catalogLocales().sort(), shipped)
  for (const locale of shipped) assert.deepEqual(checkLocale(locale), [], locale)
})

test('English messages are complete: no empty text, plurals with one and other', () => {
  const messages = englishMessages()
  assert.ok(messages.size > 1500)
  for (const [key, message] of messages) {
    assert.equal(typeof message, 'string', key)
    assert.ok(message.trim(), `${key} is empty`)
    const plural = key.match(/^(.*)_(one|other)$/)
    if (plural) for (const form of ['one', 'other']) assert.ok(messages.has(`${plural[1]}_${form}`), `${plural[1]}_${form}`)
  }
})

test('user-facing text goes through the catalog', () => {
  const findings = [...sourceFiles('../src/'), ...[]].flatMap((path) => findHardcodedText(path))
  assert.deepEqual(findings.map((finding) => `${finding.path}:${finding.line} ${finding.text}`), [])
})

test('the system language picks the closest shipped catalog', () => {
  assert.equal(matchLocale('de-AT'), 'de')
  assert.equal(matchLocale('pt-PT'), 'pt-BR')
  assert.equal(matchLocale('es-MX'), 'es-ES')
  assert.equal(matchLocale('zh-Hant-HK'), 'zh-TW')
  assert.equal(matchLocale('zh-HK'), 'zh-TW')
  assert.equal(matchLocale('zh-SG'), 'zh-CN')
  assert.equal(matchLocale('ja_JP'), 'ja')
  assert.equal(matchLocale('nl-NL'), null)
  assert.equal(resolveSystemLocale(['nl-NL', 'fr-CA', 'en-US']), 'fr')
  assert.equal(resolveSystemLocale([]), 'en')
})

test('the installer language becomes the first-run choice unless it is the system language', () => {
  assert.equal(preferenceForInstallerLanguage('de', ['en-US']), 'de')
  assert.equal(preferenceForInstallerLanguage('de', ['de-DE']), 'system')
  assert.equal(preferenceForInstallerLanguage(null, ['de-DE']), 'system')
  assert.equal(preferenceForInstallerLanguage('xx', ['de-DE']), 'system')
})

test('messages interpolate, pick plural forms per language and fall back to English', () => {
  assert.equal(interpolate('{a} and {b} and {c}', { a: 1, b: 'x' }), '1 and x and {c}')
  addMessages('ru', { settings: { core: { general: { inputDevices: {
    asked_one: 'одна {count}', asked_few: 'несколько {count}', asked_many: 'много {count}', asked_other: 'дробь {count}' } } } } })
  try {
    setActiveLocale('ru')
    assert.equal(t('settings.core.general.inputDevices.asked', { count: 1 }), 'одна 1')
    assert.equal(t('settings.core.general.inputDevices.asked', { count: 3 }), 'несколько 3')
    assert.equal(t('settings.core.general.inputDevices.asked', { count: 5 }), 'много 5')
    assert.equal(t('settings.core.general.inputDevices.asked', { count: 1.5 }), 'дробь 1.5')
    // Not in this catalog: English.
    assert.equal(t('common.save'), 'Save')
    assert.equal(withLocale('en', () => t('settings.core.general.inputDevices.asked', { count: 3 })), 'Asked 3 running games to look again.')
    assert.equal(activeLocale.value, 'ru')
    assert.equal(layerLanguage(), 'ru')
    assert.equal(formatList(['A', 'B', 'C']), 'A, B и C')
  } finally {
    setActiveLocale('en')
  }
  assert.ok(hasMessage('settings.core.general.inputDevices.asked'))
  assert.ok(!hasMessage('settings.core.general.inputDevices'))
})

test('the pseudo-locale accents and lengthens text but keeps placeholders', () => {
  const message = 'Asked {count} running games to look again.'
  const pseudo = pseudoMessage(message)
  assert.match(pseudo, /^\[.*\]$/)
  assert.ok(pseudo.includes('{count}'))
  assert.ok(!/[A-Za-z]{3,}/.test(pseudo.replace('{count}', '')), pseudo)
  assert.ok(pseudo.length >= message.length * 1.25, pseudo)
  setActiveLocale(pseudoLocale)
  try {
    assert.equal(t('common.save'), '[Šáṽé ~]')
    assert.equal(layerLanguage(), 'en')
  } finally {
    setActiveLocale('en')
  }
})

test('coded backend errors show in the app language; others pass through without the code', () => {
  const coded = (code, params, message) => `\u0001vxr:${JSON.stringify({ code, params, message })}\u0001${message}`
  const parse = coded('configParse', { path: 'C:\\settings.json', detail: 'expected u32' }, 'Unable to parse C:\\settings.json: expected u32')
  assert.equal(errorText(parse, 'fallback'), 'Unable to parse C:\\settings.json: expected u32')
  addMessages('de', { errors: { configParse: '{path} ist ungültig: {detail}', layerConflict: '{slice}: {path}' }, layers: { slices: { hklm64: 'Maschine' } } })
  try {
    setActiveLocale('de')
    assert.equal(errorText(parse, 'fallback'), 'C:\\settings.json ist ungültig: expected u32')
    assert.equal(errorText(coded('layerConflict', { slice: 'hklm64', path: 'x.json' }, 'English'), 'fallback'), 'Maschine: x.json')
    // An error built around a coded one, or a code this app does not know: English.
    assert.equal(errorText(`${parse}. The previous layer order was kept.`, 'fallback'),
      'Unable to parse C:\\settings.json: expected u32. The previous layer order was kept.')
    assert.equal(errorText(coded('newerCode', {}, 'Something new'), 'fallback'), 'Something new')
    assert.equal(errorText('Access is denied.', 'fallback'), 'Access is denied.')
  } finally {
    setActiveLocale('en')
  }
})

test('the layer and the installer ship the same languages as the app', () => {
  const codes = supportedLocales.map((locale) => locale.code)
  const layer = readFileSync(new URL('../../layer/src/osd_strings.cpp', import.meta.url), 'utf8')
  const layerCodes = layer.match(/kCodes\{([^}]*)\}/)[1].match(/"([^"]+)"/g).map((code) => code.slice(1, -1))
  assert.deepEqual(layerCodes, codes)
  const hooks = readFileSync(new URL('../src-tauri/windows/nsis-hooks.nsh', import.meta.url), 'utf8')
  for (const code of codes) assert.ok(hooks.includes(`"${code}"`), `installer hook maps ${code}`)
})
