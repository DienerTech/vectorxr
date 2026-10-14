import assert from 'node:assert/strict'
import test from 'node:test'
import { errorText } from '../src/lib/errors.ts'

test('backend rejections keep their text instead of the generic fallback', () => {
  // Tauri rejects a command's Err(String) with the bare string.
  assert.equal(
    errorText('Unable to parse C:\\settings.json: invalid type: floating point `90.0`, expected u32 at line 40 column 22', 'Failed to load config'),
    'Unable to parse C:\\settings.json: invalid type: floating point `90.0`, expected u32 at line 40 column 22',
  )
  assert.equal(errorText(new Error('Access is denied.'), 'Failed to save config'), 'Access is denied.')
  assert.equal(errorText('  ', 'Failed to save config'), 'Failed to save config')
  assert.equal(errorText(new Error(''), 'Failed to save config'), 'Failed to save config')
  assert.equal(errorText(undefined, 'Failed to save config'), 'Failed to save config')
  assert.equal(errorText({ code: 5 }, 'Failed to save config'), 'Failed to save config')
})
