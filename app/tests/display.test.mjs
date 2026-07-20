import assert from 'node:assert/strict'
import test from 'node:test'

import {
  fromConvergenceDisplay,
  toConvergenceDisplay,
} from '../src/lib/display.ts'

test('convergence display preserves one decimal place of input precision', () => {
  assert.equal(fromConvergenceDisplay(1.5), 0.015)
  assert.equal(fromConvergenceDisplay(1.6), 0.016)
  assert.equal(toConvergenceDisplay(0.015), 1.5)
  assert.equal(toConvergenceDisplay(0.016), 1.6)
})
