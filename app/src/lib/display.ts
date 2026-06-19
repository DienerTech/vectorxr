export function toStereoBoostDisplay(value: number): number {
  return Number(((value - 1) * 100).toFixed(1))
}

export function fromStereoBoostDisplay(value: number): number {
  return Number((1 + value / 100).toFixed(4))
}

export function stereoBoostBadge(value: number): string {
  const display = toStereoBoostDisplay(value)
  return `IPD ${display >= 0 ? '+' : ''}${display.toFixed(1)}%`
}

export function toConvergenceDisplay(value: number): number {
  return Number((value * 100).toFixed(1))
}

export function fromConvergenceDisplay(value: number): number {
  return Number((value / 100).toFixed(4))
}

export function convergenceBadge(value: number): string {
  const display = toConvergenceDisplay(value)
  return `Conv ${display >= 0 ? '+' : ''}${display.toFixed(1)}`
}
