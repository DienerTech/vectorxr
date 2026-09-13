import type { ActiveRuntimeInfo, DebugSourceSnapshot, LogSnapshot, OpenXrLayerSnapshot, SeenApplication, RuntimeStatusEnvelope } from './commands'
import type { HealthSummary } from './health'
import type { RuntimePacingObservation, TurboMetricsSession, VectorXRConfig } from './model'

interface SaveFilePickerHandle {
  createWritable(): Promise<{
    write(data: BlobPart): Promise<void>
    close(): Promise<void>
  }>
}

type SaveFilePickerWindow = Window & {
  showSaveFilePicker?: (options: {
    suggestedName?: string
    types?: Array<{
      description: string
      accept: Record<string, string[]>
    }>
  }) => Promise<SaveFilePickerHandle>
}

interface DebugPackageInput {
  appVersion: string
  configPath: string
  seenAppsPath: string
  config: VectorXRConfig
  unsavedChanges: boolean
  debugSources: DebugSourceSnapshot
  collectionErrors: string[]
  seenApps: SeenApplication[]
  runtimePacingPath: string
  runtimePacing: RuntimePacingObservation[]
  turboMetricsPath: string
  turboMetrics: TurboMetricsSession[]
  runtimeStatus: RuntimeStatusEnvelope | { error: string }
  activeRuntime: ActiveRuntimeInfo | null
  logSnapshot: LogSnapshot | null
  openXrLayerSnapshot: OpenXrLayerSnapshot | null
  healthSummary: HealthSummary
}

interface ZipEntry {
  path: string
  bytes: Uint8Array
  crc32: number
}

const textEncoder = new TextEncoder()
let crcTable: Uint32Array | null = null

function crc32(bytes: Uint8Array): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256)
    for (let i = 0; i < 256; i += 1) {
      let value = i
      for (let bit = 0; bit < 8; bit += 1) {
        value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1
      }
      crcTable[i] = value >>> 0
    }
  }

  let crc = 0xffffffff
  for (const byte of bytes) {
    crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8)
  }
  return (crc ^ 0xffffffff) >>> 0
}

function dosDateTime(date: Date): { date: number; time: number } {
  const year = Math.max(1980, date.getFullYear())
  return {
    date: ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate(),
    time: (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2),
  }
}

function writeUint16(bytes: number[], value: number) {
  bytes.push(value & 0xff, (value >>> 8) & 0xff)
}

function writeUint32(bytes: number[], value: number) {
  bytes.push(value & 0xff, (value >>> 8) & 0xff, (value >>> 16) & 0xff, (value >>> 24) & 0xff)
}

function pushBytes(target: number[], bytes: Uint8Array) {
  for (const byte of bytes) {
    target.push(byte)
  }
}

function stringEntry(path: string, content: unknown): ZipEntry {
  const text = typeof content === 'string' ? content : JSON.stringify(content, null, 2)
  const bytes = textEncoder.encode(text)
  return {
    path,
    bytes,
    crc32: crc32(bytes),
  }
}

function createZip(entries: ZipEntry[]): Blob {
  const parts: BlobPart[] = []
  let offset = 0
  const centralDirectory: number[] = []
  const timestamp = dosDateTime(new Date())

  for (const entry of entries) {
    const output: number[] = []
    const nameBytes = textEncoder.encode(entry.path)
    const localHeaderOffset = offset

    writeUint32(output, 0x04034b50)
    writeUint16(output, 20)
    writeUint16(output, 0x0800)
    writeUint16(output, 0)
    writeUint16(output, timestamp.time)
    writeUint16(output, timestamp.date)
    writeUint32(output, entry.crc32)
    writeUint32(output, entry.bytes.length)
    writeUint32(output, entry.bytes.length)
    writeUint16(output, nameBytes.length)
    writeUint16(output, 0)
    pushBytes(output, nameBytes)
    parts.push(new Uint8Array(output), new Uint8Array(entry.bytes))
    offset += output.length + entry.bytes.length

    writeUint32(centralDirectory, 0x02014b50)
    writeUint16(centralDirectory, 20)
    writeUint16(centralDirectory, 20)
    writeUint16(centralDirectory, 0x0800)
    writeUint16(centralDirectory, 0)
    writeUint16(centralDirectory, timestamp.time)
    writeUint16(centralDirectory, timestamp.date)
    writeUint32(centralDirectory, entry.crc32)
    writeUint32(centralDirectory, entry.bytes.length)
    writeUint32(centralDirectory, entry.bytes.length)
    writeUint16(centralDirectory, nameBytes.length)
    writeUint16(centralDirectory, 0)
    writeUint16(centralDirectory, 0)
    writeUint16(centralDirectory, 0)
    writeUint16(centralDirectory, 0)
    writeUint32(centralDirectory, 0)
    writeUint32(centralDirectory, localHeaderOffset)
    pushBytes(centralDirectory, nameBytes)
  }

  const centralDirectoryOffset = offset
  const output: number[] = []
  pushBytes(output, new Uint8Array(centralDirectory))
  writeUint32(output, 0x06054b50)
  writeUint16(output, 0)
  writeUint16(output, 0)
  writeUint16(output, entries.length)
  writeUint16(output, entries.length)
  writeUint32(output, centralDirectory.length)
  writeUint32(output, centralDirectoryOffset)
  writeUint16(output, 0)

  parts.push(new Uint8Array(output))
  return new Blob(parts, { type: 'application/zip' })
}

function reportText(input: DebugPackageInput): string {
  const lines = [
    'VectorXR Debug Information',
    `Generated: ${new Date().toISOString()}`,
    `App version: ${input.appVersion}`,
    '',
    `Health: ${input.healthSummary.label}`,
    input.healthSummary.description,
    '',
    'Checks:',
    ...input.healthSummary.checks.map((check) => `- ${check.label}: ${check.state.toUpperCase()} - ${check.detail}`),
    '',
    `Config path: ${input.configPath || 'unknown'}`,
    `Seen apps path: ${input.seenAppsPath || 'unknown'}`,
    `Log directory: ${input.logSnapshot?.directory || 'unknown'}`,
    `Active log: ${input.logSnapshot?.activePath || 'unknown'}`,
    '',
    'settings.json contains the current UI settings; raw/settings.json contains the saved file used by the layer.',
    `Unsaved UI changes: ${input.unsavedChanges ? 'yes' : 'no'}`,
    'runtime-status.json includes live sessions, safety blocks, and recorded Turbo faults.',
    'runtime-pacing.json and turbo-metrics.json contain interpreted pacing and metric data.',
    'raw/ includes unfiltered saved data, runtime status/control files (including stale sessions), recovery markers, fault archives, and pacing reset markers.',
    'logs/ includes all retained VectorXR log files, subject to capture limits.',
    'Raw capture limits: 8 MiB per file and 64 MiB total. Oversized logs keep their tail; oversized JSON files are omitted intact.',
    'diagnostic-sources.json lists source paths, original sizes, truncation, missing/unreadable files, and collection errors.',
    'Files are captured during export; live runtime writers may update them between reads. Cleared logs and disabled metric captures cannot be recovered.',
    ...input.collectionErrors.map((error) => `Collection error: ${error}`),
  ]

  return `${lines.join('\n')}\n`
}

function timestampName(): string {
  const date = new Date()
  const pad = (value: number) => value.toString().padStart(2, '0')
  return [
    date.getFullYear(),
    pad(date.getMonth() + 1),
    pad(date.getDate()),
    '-',
    pad(date.getHours()),
    pad(date.getMinutes()),
    pad(date.getSeconds()),
  ].join('')
}

export function defaultDebugPackageName(): string {
  return `VectorXR-debug-report-${timestampName()}.zip`
}

export function createDebugPackage(input: DebugPackageInput): Blob {
  const entries: ZipEntry[] = [
    stringEntry('README.txt', reportText(input)),
    stringEntry('health-summary.json', input.healthSummary),
    stringEntry('runtime-status.json', input.runtimeStatus),
    stringEntry('settings.json', input.config),
    stringEntry('diagnostic-sources.json', {
      files: input.debugSources.files.map(({ content, ...metadata }) => ({ ...metadata, included: content !== null })),
      warnings: input.debugSources.warnings,
      collectionErrors: input.collectionErrors,
      unsavedChanges: input.unsavedChanges,
    }),
    stringEntry('seen-apps.json', {
      path: input.seenAppsPath,
      observations: input.seenApps,
    }),
    stringEntry('runtime-pacing.json', {
      path: input.runtimePacingPath,
      activeRuntime: input.activeRuntime,
      observations: input.runtimePacing,
    }),
    stringEntry('turbo-metrics.json', {
      path: input.turboMetricsPath,
      sessions: input.turboMetrics,
    }),
    stringEntry('openxr-layer-snapshot.json', input.openXrLayerSnapshot ?? { slices: [] }),
    stringEntry('build-info.json', {
      generatedAt: new Date().toISOString(),
      appVersion: input.appVersion,
      configPath: input.configPath,
      seenAppsPath: input.seenAppsPath,
      logDirectory: input.logSnapshot?.directory ?? '',
      activeLogPath: input.logSnapshot?.activePath ?? '',
    }),
  ]

  input.debugSources.files.forEach((file) => {
    if (file.content !== null) entries.push(stringEntry(file.archivePath, file.content))
  })

  return createZip(entries)
}

export async function saveDebugPackage(blob: Blob, suggestedName = defaultDebugPackageName()): Promise<boolean> {
  const pickerWindow = window as SaveFilePickerWindow

  if (pickerWindow.showSaveFilePicker) {
    try {
      const handle = await pickerWindow.showSaveFilePicker({
        suggestedName,
        types: [
          {
            description: 'VectorXR debug package',
            accept: { 'application/zip': ['.zip'] },
          },
        ],
      })
      const writable = await handle.createWritable()
      await writable.write(blob)
      await writable.close()
      return true
    } catch (error) {
      if (error instanceof DOMException && error.name === 'AbortError') {
        return false
      }
      throw error
    }
  }

  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = suggestedName
  document.body.appendChild(link)
  link.click()
  link.remove()
  window.setTimeout(() => URL.revokeObjectURL(url), 0)
  return true
}
