import assert from 'node:assert/strict'
import { existsSync, readdirSync, readFileSync } from 'node:fs'
import test from 'node:test'
import { guideEntry, guideGroups, guides } from '../src/lib/guides.ts'

const catalog = JSON.parse(readFileSync(new URL('../src/i18n/locales/en/guides.json', import.meta.url), 'utf8'))
const walkthroughsSource = readFileSync(new URL('../src/lib/walkthroughs.ts', import.meta.url), 'utf8')

test('every guide has its text, and every text its guide', () => {
  assert.deepEqual(guides.map((guide) => guide.id).sort(), Object.keys(catalog.items).sort())
  for (const guide of guides) {
    const item = catalog.items[guide.id]
    assert.ok(item.title.trim() && item.summary.trim(), guide.id)
    assert.ok(Object.keys(item.sections).length > 0, guide.id)
    assert.ok(guideGroups.includes(guide.group), guide.id)
  }
})

test('every badge a guide shows has its text in every language', () => {
  const locales = readdirSync(new URL('../src/i18n/locales/', import.meta.url), { withFileTypes: true })
    .filter((entry) => entry.isDirectory()).map((entry) => entry.name)
  for (const locale of locales) {
    const badges = JSON.parse(readFileSync(new URL(`../src/i18n/locales/${locale}/guides.json`, import.meta.url), 'utf8')).badges ?? {}
    for (const guide of guides) {
      for (const badge of guide.badges) assert.ok(badges[badge]?.trim(), `${locale}: guides.badges.${badge} (${guide.id})`)
    }
  }
})

// The timing table in walkthroughs.ts, read from its source (the module itself needs the bundler).
const timing = Object.fromEntries(
  [...walkthroughsSource.matchAll(/^\s*(\w+): \{ bpm: (\d+), bars: \[([\d, ]+)\] \},$/gm)].map((match) => [match[1], { bpm: Number(match[2]), bars: match[3].split(',').map(Number) }]),
)

test('every guide but the anti-cheat one has a walkthrough: a scene, a caption for each step, and three riffs', () => {
  const scenes = readFileSync(new URL('../src/components/walkthroughs/scenes.ts', import.meta.url), 'utf8')
  for (const guide of guides.filter((entry) => entry.id !== 'anticheat')) {
    const walkthrough = timing[guide.id]
    assert.ok(walkthrough, `${guide.id} has a walkthrough`)
    assert.ok(new RegExp(`^\\s*${guide.id}: Scene`, 'm').test(scenes), `${guide.id} has a scene`)
    const text = catalog.walkthroughs[guide.id]
    assert.ok(text.title.trim(), guide.id)
    assert.deepEqual(Object.keys(text.steps), walkthrough.bars.map((_, index) => String(index + 1)), `${guide.id} has a caption for each step`)
    for (const riff of ['a', 'b', 'c']) {
      assert.ok(existsSync(new URL(`../src/assets/guides/guide-${guide.id}-${riff}.opus`, import.meta.url)), `${guide.id} riff ${riff}`)
    }
  }
  assert.deepEqual(Object.keys(timing).sort(), guides.map((guide) => guide.id).filter((id) => id !== 'anticheat').sort())
})

test('the stabilized mirror window has a full guide on its own page, with its pictures in every language', () => {
  const mirror = guideEntry('mirror')
  assert.equal(mirror.tab, 'mirror')
  assert.equal(guideEntry('screenshots').tab, 'screenshots')
  assert.equal(mirror.group, 'tools')
  for (const section of ['what', 'when', 'setup', 'check', 'off', 'problems']) {
    assert.ok(catalog.items.mirror.sections[section], section)
  }
  assert.deepEqual(timing.mirror, { bpm: 94, bars: [3, 3, 3, 3, 3, 3] })
  for (const label of ['runtimeMirror', 'sameImage', 'mirrorWindow', 'smoothedCopy']) {
    assert.ok(catalog.diagram[label].trim(), label)
  }
  assert.match(readFileSync(new URL('../src/components/GuideDiagram.vue', import.meta.url), 'utf8'), /id === 'mirror'/)
  assert.match(readFileSync(new URL('../src/components/MirrorUtility.vue', import.meta.url), 'utf8'), /<GuideLink guide="mirror"/)
  // The last three steps show the mirror settings: one picture with the Default Profile switch ringed,
  // and one with a box for each group of settings.
  const shots = JSON.parse(readFileSync(new URL('../src/assets/guides/screens/manifest.json', import.meta.url), 'utf8')).boxes
  const languages = readdirSync(new URL('../src/i18n/locales/', import.meta.url))
  assert.equal(languages.length, 11)
  for (const language of languages) {
    for (const picture of ['mirror', 'mirror-settings']) {
      assert.ok(existsSync(new URL(`../src/assets/guides/screens/${language}/${picture}.jpg`, import.meta.url)), `${language} ${picture}`)
    }
    for (const box of ['mirror', 'mirror-stabilization', 'mirror-window']) {
      const [x, y, width, height] = shots[language][box]
      assert.ok(x >= 0 && y >= 0 && width > 0 && height > 0 && x + width <= 1 && y + height <= 1, `${language} ${box}`)
    }
  }
})
