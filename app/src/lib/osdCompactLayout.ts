import type { OsdSettings } from './model'

// The FPS value's column, and with Frame Generation's badge beside it.
export const compactFpsWidth = 210
export const compactFpsBadgeWidth = 260

// Keep dimensions in sync with CompactOsdLayout in the layer rasterizer.
// `framegenSession`: the session started with Frame Generation on (the
// preview shows one), so the FPS value makes room for its badge.
export function osdCompactLayout(settings: OsdSettings, framegenSession = false) {
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
  const framegenBadge = fps && framegenSession && settings.showFramegenFps
  const fpsWidth = framegenBadge ? compactFpsBadgeWidth : compactFpsWidth
  const frameTimeX = fps ? 28 + fpsWidth + 24 : 28
  const status = settings.compactShowTurbo || settings.compactShowFramegen || settings.compactShowPivot
  const empty = !fields.length && !fps && !frameTime && !status
  const width = Math.max(240, fields.length ? x + 12 : 0,
    fps || frameTime ? 56 + (fps ? fpsWidth : 0) + (frameTime ? 300 : 0) + (fps && frameTime ? 24 : 0) : 0,
    status ? 520 : 0)
  let y = 18
  const row = (show: boolean, height: number) => { if (!show) return -1; const top = y; y += height + 12; return top }
  const headerY = row(!!fields.length, 32), metricsY = row(fps || frameTime, 56)
  // Status rows stack Turbo, Frame Gen, then Pivot. The "FRAME GEN" label is
  // wider than "TURBO"/"PIVOT", so every status value starts further right
  // while the Frame Gen row is shown, keeping the values aligned.
  const turboY = row(settings.compactShowTurbo, 36), framegenY = row(settings.compactShowFramegen, 36), pivotY = row(settings.compactShowPivot, 36)
  const statusY = [turboY, framegenY, pivotY].find(top => top >= 0) ?? -1
  const statusValueX = settings.compactShowFramegen ? 150 : 128
  return { width, height: empty ? 80 : y + 6, fields, fps, frameTime, frameTimeX, framegenBadge, headerY, metricsY, statusY, turboY, framegenY, pivotY, statusValueX, empty }
}
