export function toStereoBoostDisplay(value: number): number {
  return Number(((value - 1) * 100).toFixed(1))
}

export function fromStereoBoostDisplay(value: number): number {
  return Number((1 + value / 100).toFixed(4))
}

export function stereoBoostBadge(value: number): string {
  const display = toStereoBoostDisplay(value)
  return `SB ${display >= 0 ? '+' : ''}${display.toFixed(1)}`
}

export function toConvergenceDisplay(value: number): number {
  return Number((value * 1000).toFixed(1))
}

export function fromConvergenceDisplay(value: number): number {
  return Number((value / 1000).toFixed(4))
}

export function convergenceBadge(value: number): string {
  return `Scale ${toConvergenceDisplay(value).toFixed(1)}`
}
