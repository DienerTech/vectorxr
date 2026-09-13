import assert from 'node:assert/strict'
import test from 'node:test'
import { crc32 } from 'node:zlib'
import { createDebugPackage } from '../src/lib/debugPackage.ts'
import { defaultConfig } from '../src/lib/model.ts'

// Read using the ZIP central directory and check each member's CRC independently.
async function unzip(blob) {
  const bytes = Buffer.from(await blob.arrayBuffer())
  const end = bytes.length - 22
  assert.equal(bytes.readUInt32LE(end), 0x06054b50)
  const count = bytes.readUInt16LE(end + 10)
  let position = bytes.readUInt32LE(end + 16)
  const entries = new Map()
  for (let index = 0; index < count; index++) {
    assert.equal(bytes.readUInt32LE(position), 0x02014b50)
    assert.equal(bytes.readUInt16LE(position + 10), 0)
    const size = bytes.readUInt32LE(position + 24)
    const nameLength = bytes.readUInt16LE(position + 28)
    const name = bytes.subarray(position + 46, position + 46 + nameLength).toString('utf8')
    const local = bytes.readUInt32LE(position + 42)
    assert.equal(bytes.readUInt32LE(local), 0x04034b50)
    const start = local + 30 + bytes.readUInt16LE(local + 26) + bytes.readUInt16LE(local + 28)
    const data = bytes.subarray(start, start + size)
    assert.equal(crc32(data), bytes.readUInt32LE(position + 16), name)
    assert.ok(!entries.has(name), `Duplicate ZIP member: ${name}`)
    entries.set(name, data.toString('utf8'))
    position += 46 + nameLength + bytes.readUInt16LE(position + 30) + bytes.readUInt16LE(position + 32)
  }
  assert.equal(position, end)
  return entries
}

function fixture() {
  return {
    appVersion: 'test', configPath: 'settings.json', seenAppsPath: 'seen-apps.json',
    config: defaultConfig(), unsavedChanges: true, seenApps: [],
    runtimePacingPath: 'runtime-pacing.json', runtimePacing: [{ verdict: 'sequenced', reason: 'submission faults' }],
    turboMetricsPath: 'turbo-metrics.json', turboMetrics: [{ sessionId: 'session-1', mode: 'sequenced', fallbackReason: 'runtime failure' }],
    runtimeStatus: { sessions: [{ state: { turboState: 'suspended', turboReason: 'runtime failure' } }],
      recovery: [{ fingerprint: 'dcs:steamvr', state: 'failed' }], faults: [{ reason: 'submission fault' }] },
    activeRuntime: null, logSnapshot: null, openXrLayerSnapshot: null,
    healthSummary: { label: 'Ready', description: 'Test', checks: [] },
    collectionErrors: [], debugSources: { files: [], warnings: [] },
  }
}

test('debug ZIP preserves Turbo safety, pacing, metrics, raw files, and complete retained logs', async () => {
  const input = fixture()
  const log = 'Turbo frame diagnostics →\n'.repeat(8_000)
  for (const [archivePath, content] of [
    ['logs/vectorxr.log', log], ['raw/settings.json', '{"saved":true}'],
    ['raw/runtime/status/stale.json', '{"state":"suspended"}'],
    ['raw/runtime/turbo-recovery/live.json', '{"state":"armed"}'],
    ['raw/runtime/turbo-recovery/faults/failure.json', '{"reason":"runtime error"}'],
    ['raw/runtime-pacing-resets/runtime.txt', '42'],
  ]) input.debugSources.files.push({ archivePath, sourcePath: archivePath, content,
    originalBytes: Buffer.byteLength(content), truncated: false, error: null })
  const zip = await unzip(createDebugPackage(input))
  assert.deepEqual(JSON.parse(zip.get('runtime-status.json')), input.runtimeStatus)
  assert.deepEqual(JSON.parse(zip.get('runtime-pacing.json')).observations, input.runtimePacing)
  assert.deepEqual(JSON.parse(zip.get('turbo-metrics.json')).sessions, input.turboMetrics)
  for (const file of input.debugSources.files) assert.equal(zip.get(file.archivePath), file.content)
  const inventory = JSON.parse(zip.get('diagnostic-sources.json'))
  assert.equal(inventory.unsavedChanges, true)
  assert.ok(inventory.files.every(file => file.included && !('content' in file)))
  assert.match(zip.get('README.txt'), /raw\/settings.json contains the saved file/)
})

test('debug ZIP reports collection failures and omitted or truncated source files', async () => {
  const input = fixture()
  input.runtimeStatus = { error: 'unavailable' }
  input.collectionErrors = ['Runtime status: unavailable']
  input.debugSources = { warnings: ['Missing runtime directory'], files: [
    { archivePath: 'raw/big.json', sourcePath: 'big.json', content: null, originalBytes: 10_000_000, truncated: true, error: 'Size limit' },
    { archivePath: 'logs/large.log', sourcePath: 'large.log', content: 'tail', originalBytes: 10_000_000, truncated: true, error: null },
  ] }
  const zip = await unzip(createDebugPackage(input))
  assert.equal(zip.has('raw/big.json'), false)
  assert.equal(zip.get('logs/large.log'), 'tail')
  const inventory = JSON.parse(zip.get('diagnostic-sources.json'))
  assert.deepEqual(inventory.collectionErrors, input.collectionErrors)
  assert.deepEqual(inventory.warnings, input.debugSources.warnings)
  assert.equal(inventory.files[0].included, false)
  assert.equal(inventory.files[1].truncated, true)
  assert.match(zip.get('README.txt'), /Collection error: Runtime status: unavailable/)
})
