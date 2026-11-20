import assert from 'node:assert/strict'
import test from 'node:test'
import { clearNetworkPaths, isNetworkPath } from '../src/lib/importPaths.ts'
import { defaultConfig, normalizeConfig } from '../src/lib/model.ts'
import { validateConfig } from '../src/lib/validation.ts'

test('network paths are told apart from drive paths', () => {
  for (const path of ['\\\\server\\share\\Shots', '//server/share/a.png', '\\\\?\\UNC\\server\\share', '\\\\.\\pipe\\x', '  \\\\10.0.0.5\\c$\\a.wav', '\\\\server']) {
    assert.equal(isNetworkPath(path), true, path)
  }
  for (const path of ['D:\\Shots', 'D:/Shots', '\\\\?\\D:\\Shots', '', 'Shots', '\\Shots', undefined, 5]) {
    assert.equal(isNetworkPath(path), false, String(path))
  }
})

test('an imported config loses its network paths and names them', () => {
  const config = defaultConfig()
  const sound = (activateSound, deactivateSound = '') => ({ enabled: true, activateSound, deactivateSound })
  config.core.screenshot = {
    bindings: [{ type: 'keyboard', chord: ['Ctrl', 'F9'], sound: sound('\\\\evil\\s\\on.wav', 'D:\\Sounds\\off.wav') }],
    eyes: 'both',
    folder: '\\\\evil\\share\\Shots',
  }
  const imported = normalizeConfig(JSON.parse(JSON.stringify(config)))
  const before = JSON.stringify(imported)

  const { config: cleaned, cleared } = clearNetworkPaths(imported)
  assert.equal(JSON.stringify(imported), before, 'the input is not changed')
  assert.deepEqual(cleared.map((entry) => entry.field).sort(), [
    'core.screenshot.bindings[0].sound.activateSound',
    'core.screenshot.folder',
  ])
  assert.equal(cleared.find((entry) => entry.field === 'core.screenshot.folder').path, '\\\\evil\\share\\Shots')
  assert.ok(!('folder' in cleaned.core.screenshot))
  assert.deepEqual(cleaned.core.screenshot.bindings[0].sound, sound('', 'D:\\Sounds\\off.wav'))
  assert.ok(!JSON.stringify(cleaned).includes('evil'))
  // What is left is a config the app can save.
  assert.deepEqual(validateConfig(cleaned), [])
  assert.deepEqual(clearNetworkPaths(cleaned).cleared, [])
})

test('local paths are kept on import', () => {
  const config = defaultConfig()
  config.core.screenshot.folder = 'D:\\Shots'
  const { config: cleaned, cleared } = clearNetworkPaths(config)
  assert.deepEqual(cleared, [])
  assert.deepEqual(cleaned, JSON.parse(JSON.stringify(config)))
})
