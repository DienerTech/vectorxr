import assert from 'node:assert/strict'
import test from 'node:test'
import { readdirSync, readFileSync } from 'node:fs'

// Public docs and release notes must be valid UTF-8: a stray Windows code-page
// byte (such as a bare 0xB0 degree sign) renders as U+FFFD on GitHub.
for (const folder of ['../../docs/', '../../release/notes/']) {
  const directory = new URL(folder, import.meta.url)
  for (const name of readdirSync(directory).filter((file) => file.endsWith('.md'))) {
    test(`${folder.replace('../../', '')}${name} is valid UTF-8`, () => {
      const bytes = readFileSync(new URL(name, directory))
      assert.doesNotThrow(() => new TextDecoder('utf-8', { fatal: true }).decode(bytes))
    })
  }
}
