import type { ActiveRuntimeInfo, LogSnapshot, OpenXrLayerSnapshot, SeenApplication } from './commands'
import type { HealthSummary } from './health'
import type { RuntimePacingObservation, VectorXRConfig } from './model'

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
  seenApps: SeenApplication[]
  runtimePacingPath: string
  runtimePacing: RuntimePacingObservation[]
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
  const output: number[] = []
  const centralDirectory: number[] = []
  const timestamp = dosDateTime(new Date())

  for (const entry of entries) {
    const nameBytes = textEncoder.encode(entry.path)
    const localHeaderOffset = output.length

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
    pushBytes(output, entry.bytes)

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

  const centralDirectoryOffset = output.length
  pushBytes(output, new Uint8Array(centralDirectory))
  writeUint32(output, 0x06054b50)
  writeUint16(output, 0)
  writeUint16(output, 0)
  writeUint16(output, entries.length)
  writeUint16(output, entries.length)
  writeUint32(output, centralDirectory.length)
  writeUint32(output, centralDirectoryOffset)
  writeUint16(output, 0)

  return new Blob([new Uint8Array(output)], { type: 'application/zip' })
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
  ]

  return `${lines.join('\n')}\n`
}

function safeLogName(name: string, index: number): string {
  const normalized = name.replace(/[^\w.-]+/g, '-').replace(/^-+|-+$/g, '')
  return normalized || `log-${index + 1}.txt`
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
    stringEntry('settings.json', input.config),
    stringEntry('seen-apps.json', {
      path: input.seenAppsPath,
      observations: input.seenApps,
    }),
    stringEntry('runtime-pacing.json', {
      path: input.runtimePacingPath,
      activeRuntime: input.activeRuntime,
      observations: input.runtimePacing,
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

  input.logSnapshot?.files.forEach((file, index) => {
    entries.push(stringEntry(`logs/${safeLogName(file.name, index)}`, file.content))
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
