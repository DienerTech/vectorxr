export function toStereoBoostDisplay(value: number): number {
  return Number(((value - 1) * 100).toFixed(1))
}

export function fromStereoBoostDisplay(value: number): number {
  return Number((1 + value / 100).toFixed(4))
}

export function stereoBoostBadge(value: number): string {
  const display = toStereoBoostDisplay(value)
  if (Math.abs(display) < 0.05) {
    return 'Native IPD'
  }
  return `Stereo ${display > 0 ? '+' : ''}${display.toFixed(1)}%`
}

export function toConvergenceDisplay(value: number): number {
  return Number((value * 100).toFixed(1))
}

export function fromConvergenceDisplay(value: number): number {
  return Number((value / 100).toFixed(4))
}

export function convergenceBadge(value: number): string {
  const display = toConvergenceDisplay(value)
  if (Math.abs(display) < 0.05) {
    return 'Plane: app default'
  }
  if (display > 0) {
    return `Plane: nearer +${display.toFixed(1)}`
  }
  return `Plane: farther ${display.toFixed(1)}`
}
