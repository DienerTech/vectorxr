// Generates the two bundled default feedback WAVs (activate / deactivate).
// Re-run with `node scripts/generate-default-sounds.mjs` if the assets need to
// be regenerated. Output: short 16-bit mono PCM tones, well under the layer's
// 5-second playback cap.
import { mkdirSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const SAMPLE_RATE = 44100
const BITS_PER_SAMPLE = 16
const AMPLITUDE = 0.35

// One pair of notes per cue. Rising = activate (engaging), falling = deactivate.
// The origin cues are shorter and higher — a mechanical "latch" click distinct
// from the melodic engage/disengage pair.
const cues = {
  activate: [
    { freq: 587.33, ms: 70 }, // D5
    { freq: 880.0, ms: 110 }, // A5
  ],
  deactivate: [
    { freq: 880.0, ms: 70 },
    { freq: 587.33, ms: 110 },
  ],
  'origin-set': [
    { freq: 1046.5, ms: 40 }, // C6
    { freq: 1568.0, ms: 55 }, // G6
  ],
  'origin-release': [
    { freq: 1568.0, ms: 40 },
    { freq: 1046.5, ms: 55 },
  ],
  // Turbo: a three-note "spool up" arpeggio, clearly distinct from the
  // two-note pivot cues; reversed for disengage.
  'turbo-on': [
    { freq: 523.25, ms: 45 }, // C5
    { freq: 659.25, ms: 45 }, // E5
    { freq: 783.99, ms: 80 }, // G5
  ],
  'turbo-off': [
    { freq: 783.99, ms: 45 },
    { freq: 659.25, ms: 45 },
    { freq: 523.25, ms: 80 },
  ],
  // Depth Lock: a compact octave snap, distinct from Depth's normal two-note
  // transition and Turbo's three-note arpeggio.
  'depth-lock-on': [
    { freq: 523.25, ms: 45 }, // C5
    { freq: 1046.5, ms: 75 }, // C6
  ],
  'depth-lock-off': [
    { freq: 1046.5, ms: 45 },
    { freq: 523.25, ms: 75 },
  ],
}

function renderTones(tones) {
  const samples = []
  for (const tone of tones) {
    const count = Math.round((tone.ms / 1000) * SAMPLE_RATE)
    // 6 ms attack/release fade so each note starts and ends without a click.
    const fade = Math.min(Math.round(0.006 * SAMPLE_RATE), Math.floor(count / 2))
    for (let i = 0; i < count; i += 1) {
      let envelope = 1
      if (i < fade) envelope = i / fade
      else if (i > count - fade) envelope = (count - i) / fade
      samples.push(Math.sin((2 * Math.PI * tone.freq * i) / SAMPLE_RATE) * AMPLITUDE * envelope)
    }
  }
  return samples
}

function encodeWav(samples) {
  const blockAlign = BITS_PER_SAMPLE / 8
  const dataSize = samples.length * blockAlign
  const buffer = Buffer.alloc(44 + dataSize)

  buffer.write('RIFF', 0)
  buffer.writeUInt32LE(36 + dataSize, 4)
  buffer.write('WAVE', 8)
  buffer.write('fmt ', 12)
  buffer.writeUInt32LE(16, 16) // fmt chunk size
  buffer.writeUInt16LE(1, 20) // PCM
  buffer.writeUInt16LE(1, 22) // mono
  buffer.writeUInt32LE(SAMPLE_RATE, 24)
  buffer.writeUInt32LE(SAMPLE_RATE * blockAlign, 28) // byte rate
  buffer.writeUInt16LE(blockAlign, 32)
  buffer.writeUInt16LE(BITS_PER_SAMPLE, 34)
  buffer.write('data', 36)
  buffer.writeUInt32LE(dataSize, 40)

  let offset = 44
  for (const sample of samples) {
    const clamped = Math.max(-1, Math.min(1, sample))
    buffer.writeInt16LE(Math.round(clamped * 32767), offset)
    offset += blockAlign
  }
  return buffer
}

const here = dirname(fileURLToPath(import.meta.url))
const repoRoot = join(here, '..')
// Both consumers get the same assets: the layer bundles them next to the DLL,
// the app bundles them for the in-UI Test button.
const targets = [
  join(repoRoot, 'layer', 'assets', 'sounds'),
  join(repoRoot, 'app', 'src-tauri', 'resources', 'sounds'),
]

for (const [name, tones] of Object.entries(cues)) {
  const wav = encodeWav(renderTones(tones))
  for (const dir of targets) {
    mkdirSync(dir, { recursive: true })
    const path = join(dir, `${name}.wav`)
    writeFileSync(path, wav)
    console.log(`Wrote ${path} (${wav.length} bytes)`)
  }
}
