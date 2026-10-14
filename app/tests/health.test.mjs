import assert from 'node:assert/strict'
import test from 'node:test'
import { buildHealthSummary, findVectorXrLayer } from '../src/lib/health.ts'
import { defaultConfig } from '../src/lib/model.ts'

function layer(slice, manifestPath, enabled, overrides = {}) {
  return {
    slice, manifestPath, layerName: 'XR_APILAYER_DIENERTECH_vectorxr', description: '', enabled, order: 1,
    signatureStatus: 'unknown', signatureStatusDescription: '', isVectorXr: true,
    manifestExists: true, libraryExists: true, ...overrides,
  }
}

function snapshot(layers) {
  return {
    processElevated: false,
    slices: ['hklm64', 'hkcu64'].map((id) => ({
      id, label: id, registryPath: '', recommended: id === 'hklm64', uncommon: id !== 'hklm64',
      requiresElevationForWrites: id === 'hklm64', layers: layers.filter((entry) => entry.slice === id),
    })),
  }
}

function summary(layers) {
  return buildHealthSummary({
    config: defaultConfig(), configPath: 'settings.json', logSnapshot: null,
    openXrLayerSnapshot: snapshot(layers), openXrLayersLoading: false, seenApps: [],
  })
}

test('Home and Health report the VectorXR registration OpenXR apps will load', () => {
  const stale = layer('hklm64', 'C:\\old\\XR_APILAYER_DIENERTECH_VECTORXR.json', false, { libraryExists: false })
  const active = layer('hklm64', 'C:\\Program Files\\VectorXR\\XR_APILAYER_DIENERTECH_VECTORXR.json', true)
  assert.equal(findVectorXrLayer(snapshot([stale, active])), active)
  const health = summary([stale, active])
  assert.equal(health.vectorXrLayer, active)
  assert.equal(health.checks.find((check) => check.id === 'layer-enabled').state, 'pass')
  assert.equal(health.checks.find((check) => check.id === 'layer-files').state, 'pass')
  assert.equal(health.checks.some((check) => check.id === 'layer-duplicates'), false)

  // With nothing enabled the first registration still explains why.
  assert.equal(findVectorXrLayer(snapshot([stale])), stale)
  assert.equal(findVectorXrLayer(snapshot([])), null)
})

test('Health fails when two VectorXR registrations are enabled', () => {
  const machine = layer('hklm64', 'C:\\Program Files\\VectorXR\\XR_APILAYER_DIENERTECH_VECTORXR.json', true)
  const perUser = layer('hkcu64', 'C:\\dev\\XR_APILAYER_DIENERTECH_VECTORXR.json', true)
  const health = summary([machine, perUser])
  const duplicate = health.checks.find((check) => check.id === 'layer-duplicates')
  assert.equal(duplicate?.state, 'fail')
  assert.match(duplicate.detail, /2 VectorXR registrations are enabled/)
  assert.equal(health.overall, 'inactive')
})
