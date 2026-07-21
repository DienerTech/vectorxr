import assert from 'node:assert/strict'
import test from 'node:test'

import {
  formatPatchNoteInlineHtml,
  patchNotes,
} from '../src/lib/patchNotes.ts'

function assertItem(item, path, depth = 0) {
  if (typeof item === 'string') {
    assert.ok(item.trim(), `${path} must not be empty`)
    return depth
  }

  assert.equal(typeof item.html, 'string', `${path}.html must be a string`)
  assert.ok(item.html.trim(), `${path}.html must not be empty`)
  if (!item.items) {
    return depth
  }
  assert.ok(item.items.length > 0, `${path}.items must not be empty`)
  return Math.max(
    depth,
    ...item.items.map((child, index) => assertItem(child, `${path}.items[${index}]`, depth + 1)),
  )
}

test('patch notes load from structured JSON with unique versions', () => {
  assert.ok(patchNotes.length > 0)
  assert.equal(new Set(patchNotes.map((entry) => entry.version)).size, patchNotes.length)

  for (const [entryIndex, entry] of patchNotes.entries()) {
    assert.match(entry.date, /^\d{4}-\d{2}-\d{2}$/)
    assert.ok(entry.title.trim())
    assert.ok(entry.summary.trim())
    assert.ok(entry.items.length > 0)
    entry.items.forEach((item, itemIndex) =>
      assertItem(item, `entries[${entryIndex}].items[${itemIndex}]`),
    )
  }
})

test('0.14 patch notes exercise nested lists', () => {
  assert.equal(patchNotes[0].version, '0.14.0')
  const maxDepth = Math.max(
    ...patchNotes[0].items.map((item, index) => assertItem(item, `latest.items[${index}]`)),
  )
  assert.ok(maxDepth >= 1)
})

test('inline formatting allows only exact attribute-free tags', () => {
  assert.equal(
    formatPatchNoteInlineHtml('<strong>Depth</strong> & <em>comfort</em><br><code>xrEndFrame</code>'),
    '<strong>Depth</strong> &amp; <em>comfort</em><br><code>xrEndFrame</code>',
  )
  assert.equal(
    formatPatchNoteInlineHtml('<script>alert(1)</script><img src=x onerror=alert(1)>'),
    '&lt;script&gt;alert(1)&lt;/script&gt;&lt;img src=x onerror=alert(1)&gt;',
  )
})
