// Builds the sample configuration shown in documentation screenshots, using
// the desktop app's own model factories so it always matches the schema.
import {
  createPivotProfile,
  createPivotQuickView,
  createProfile,
  createQuadViewsProfile,
  createTurboProfile,
  defaultConfig,
  normalizeConfig,
  toolkitInspiredTurboExperimental,
  type RegisteredApplication,
} from '../../app/src/lib/model.ts'

export function sampleConfig() {
  const config = defaultConfig()
  const applications: RegisteredApplication[] = [
    { id: 'dcs-world', name: 'DCS World', enabled: true, match: { exe: 'DCS.exe' } },
    { id: 'msfs-2024', name: 'Microsoft Flight Simulator 2024', enabled: true, match: { exe: 'FlightSimulator2024.exe' } },
    { id: 'iracing', name: 'iRacing', enabled: true, match: { exe: 'iRacingSim64DX11.exe' } },
  ]
  config.applications = applications

  const { modules } = config
  modules.quadviews.enabled = true
  const quadviews = createQuadViewsProfile(modules.quadviews.defaults, ['dcs-world'])
  quadviews.name = 'DCS World'
  modules.quadviews.profiles = [quadviews]

  modules.turbo.enabled = false
  const dcsTurbo = createTurboProfile(['dcs-world'])
  dcsTurbo.name = 'DCS World'
  const msfsTurbo = createTurboProfile(['msfs-2024'])
  msfsTurbo.name = 'MSFS 2024'
  msfsTurbo.experimental = { ...toolkitInspiredTurboExperimental(), applicationIds: [] }
  modules.turbo.profiles = [dcsTurbo, msfsTurbo]

  modules.pivotxr.enabled = true
  modules.pivotxr.behavior = 'snapViews'
  modules.pivotxr.viewControls.quickViews = [
    { name: 'Look Left', yawDegrees: -90, pitchDegrees: 0 },
    { name: 'Look Right', yawDegrees: 90, pitchDegrees: 0 },
    { name: 'High 12', yawDegrees: 0, pitchDegrees: 35 },
    { name: 'Check Six', yawDegrees: 180, pitchDegrees: 0 },
  ].map((preset) => ({ ...createPivotQuickView(preset.name), ...preset }))
  const pivot = createPivotProfile(modules.pivotxr.defaults, ['msfs-2024'])
  pivot.name = 'MSFS Enhanced Motion'
  pivot.alwaysActive = true
  modules.pivotxr.profiles = [pivot]

  modules.depthxr.enabled = true
  const depth = createProfile(modules.depthxr.defaults, ['iracing'])
  depth.name = 'iRacing'
  modules.depthxr.profiles = [depth]

  config.core.osd.enabled = true
  return normalizeConfig(config)
}

if (process.argv[1]?.endsWith('seed-config.ts')) {
  process.stdout.write(JSON.stringify(sampleConfig()))
}
