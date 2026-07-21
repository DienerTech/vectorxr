import assert from 'node:assert/strict'
import test from 'node:test'

import {
  fromConvergenceDisplay,
  fromStereoBoostDisplay,
  toConvergenceDisplay,
  toStereoBoostDisplay,
} from '../src/lib/display.ts'

test('convergence display preserves one decimal place of input precision', () => {
  assert.equal(fromConvergenceDisplay(1.5), 0.015)
  assert.equal(fromConvergenceDisplay(1.6), 0.016)
  assert.equal(toConvergenceDisplay(0.015), 1.5)
  assert.equal(toConvergenceDisplay(0.016), 1.6)
})

test('Stereo Depth display preserves normal and extended range precision', () => {
  assert.equal(fromStereoBoostDisplay(25), 1.25)
  assert.equal(fromStereoBoostDisplay(73.4), 1.734)
  assert.equal(toStereoBoostDisplay(1.25), 25)
  assert.equal(toStereoBoostDisplay(1.734), 73.4)
})