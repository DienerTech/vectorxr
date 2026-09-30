import type { OsdSettings } from './model'

// Keep dimensions in sync with CompactOsdLayout in the layer rasterizer.
export function osdCompactLayout(settings: OsdSettings) {
  const header = [
    { key: 'brand', show: settings.compactShowBrand, width: 155 },
    { key: 'app', show: settings.compactShowApp, width: 260 },
    { key: 'runtime', show: settings.compactShowRuntime, width: 297 },
    { key: 'clock', show: settings.compactShowClock, width: 142 },
  ].filter(item => item.show)
  let x = 28
  const fields = header.map(item => { const field = { ...item, x }; x += item.width + 16; return field })
  const fps = settings.compactMetrics === 'all' || settings.compactMetrics === 'fps'
  const frameTime = settings.compactMetrics === 'all' || settings.compactMetrics === 'frameTime'
  const status = settings.compactShowTurbo || settings.compactShowPivot
  const empty = !fields.length && !fps && !frameTime && !status
  const width = Math.max(240, fields.length ? x + 12 : 0,
    fps || frameTime ? 56 + (fps ? 210 : 0) + (frameTime ? 300 : 0) + (fps && frameTime ? 24 : 0) : 0,
    status ? 520 : 0)
  let y = 18
  const row = (show: boolean, height: number) => { if (!show) return -1; const top = y; y += height + 12; return top }
  const headerY = row(!!fields.length, 32), metricsY = row(fps || frameTime, 56)
  const turboY = row(settings.compactShowTurbo, 36), pivotY = row(settings.compactShowPivot, 36)
  const statusY = turboY >= 0 ? turboY : pivotY
  return { width, height: empty ? 80 : y + 6, fields, fps, frameTime, headerY, metricsY, statusY, turboY, pivotY, empty }
}
