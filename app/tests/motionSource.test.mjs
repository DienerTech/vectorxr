import assert from 'node:assert/strict'
import test from 'node:test'
import { motionSourceState } from '../src/lib/motionSource.ts'

const integrated = { name: 'AMD Radeon(TM) Graphics', vendorId: 0x1002, deviceId: 0x13c0, opticalFlow: false, detail: 'not an NVIDIA GPU' }
const rtx = { name: 'NVIDIA GeForce RTX 5090', vendorId: 0x10de, deviceId: 0x2b85, opticalFlow: true, detail: '' }
const pascal = { name: 'NVIDIA GeForce GTX 1080', vendorId: 0x10de, deviceId: 0x1b80, opticalFlow: false, detail: 'the NVIDIA driver offers no optical flow hardware on it' }
const cards = (...adapters) => ({ probed: true, adapters, opticalFlow: adapters.some((adapter) => adapter.opticalFlow), error: '' })

test('an integrated GPU listed first does not hide an RTX card listed after it', () => {
  const state = motionSourceState({ synthesisMode: 'extrapolate', motionEstimation: 'hardware' }, cards(integrated, rtx))
  assert.equal(state.effective, 'hardware')
  assert.equal(state.hardwareUnavailable, null)
  assert.equal(state.locked, false)
  assert.equal(state.note, null)
})

test('with no card that has the hardware, NVIDIA hardware is unavailable and every card says why', () => {
  const settings = { synthesisMode: 'extrapolate', motionEstimation: 'hardware' }
  const state = motionSourceState(settings, cards(integrated, pascal))
  assert.equal(state.effective, 'shader')
  assert.match(state.hardwareUnavailable, /AMD Radeon\(TM\) Graphics: not an NVIDIA GPU/)
  assert.match(state.hardwareUnavailable, /GTX 1080: the NVIDIA driver offers no optical flow hardware on it/)
  assert.match(state.note, /Shader measures motion/)
  assert.equal(settings.motionEstimation, 'hardware', 'the saved choice is kept')
})

test('Interpolate in Dynamic locks the motion source to Shader whatever is chosen, and keeps the choice', () => {
  for (const gpus of [cards(rtx), cards(integrated), null]) {
    const settings = { synthesisMode: 'interpolate', motionEstimation: 'hardware', frameMultiplier: 'dynamic' }
    const state = motionSourceState(settings, gpus)
    assert.equal(state.locked, true)
    assert.equal(state.effective, 'shader')
    assert.match(state.note, /In Dynamic, Interpolate measures motion with Shader/)
    assert.equal(settings.motionEstimation, 'hardware')
  }
})

test('Interpolate at a fixed or Auto rate can use the NVIDIA hardware (experimental)', () => {
  for (const frameMultiplier of ['auto', '2x', '3x', '4x']) {
    const state = motionSourceState({ synthesisMode: 'interpolate', motionEstimation: 'hardware', frameMultiplier }, cards(rtx))
    assert.equal(state.locked, false)
    assert.equal(state.effective, 'hardware')
    assert.match(state.note, /Experimental/)
    const shader = motionSourceState({ synthesisMode: 'interpolate', motionEstimation: 'shader', frameMultiplier }, cards(rtx))
    assert.equal(shader.locked, false)
    assert.equal(shader.effective, 'shader')
    assert.equal(shader.note, null)
  }
  const missing = motionSourceState({ synthesisMode: 'interpolate', motionEstimation: 'hardware', frameMultiplier: '2x' }, cards(integrated))
  assert.equal(missing.effective, 'shader')
  assert.match(missing.note, /no graphics card here has it/)
})

test('cards not known yet, or not checkable, leave the choice as it is', () => {
  for (const gpus of [null, { probed: false, adapters: [], opticalFlow: false, error: 'not running in the VectorXR app' }]) {
    const state = motionSourceState({ synthesisMode: 'extrapolate', motionEstimation: 'hardware' }, gpus)
    assert.equal(state.effective, 'hardware')
    assert.equal(state.hardwareUnavailable, null)
  }
})

test('Shader chosen on a PC without the hardware needs no note', () => {
  const state = motionSourceState({ synthesisMode: 'extrapolate', motionEstimation: 'shader' }, cards(integrated))
  assert.equal(state.effective, 'shader')
  assert.notEqual(state.hardwareUnavailable, null)
  assert.equal(state.note, null)
})
