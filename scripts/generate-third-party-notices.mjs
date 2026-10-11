// Generates THIRD-PARTY-NOTICES.txt in the repository root: the copyright
// notices and license texts for the third-party code in the files that the
// VectorXR installer ships. Re-run with
// `node scripts/generate-third-party-notices.mjs` after changing
// app/src-tauri/Cargo.lock, app/package-lock.json, the vendored NVIDIA headers
// or the pinned Khronos headers. With `--check` it only reports whether the
// file in the repository is current, and exits with 1 when it is not.
//
// Everything is read locally; nothing is downloaded. The app's crate sources
// must already be in Cargo's registry (any build of the app puts them there),
// and app/node_modules must be installed (`npm ci`).
import { execFileSync } from 'node:child_process'
import { existsSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { dirname, extname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..')
const appDir = join(repoRoot, 'app')
const tauriDir = join(appDir, 'src-tauri')
const outputPath = join(repoRoot, 'THIRD-PARTY-NOTICES.txt')
const APP_CRATE = 'vectorxr-app'
const TARGET = 'x86_64-pc-windows-msvc'

const RULE = '='.repeat(80)
const THIN_RULE = '-'.repeat(80)
const FILE_RULE = '- '.repeat(40).trimEnd()

// Files in a package that carry license terms or copyright notices. SPDX
// metadata documents (LICENSE.spdx) and source files are not notices.
const NOTICE_FILE = /^(licen[cs]e|copying|copyright|notice|unlicense|third[-_ ]?party)/i
const NOT_A_NOTICE = new Set(['.spdx', '.rs', '.js', '.cjs', '.mjs', '.ts', '.json', '.html', '.toml'])

const APACHE_START = /Apache License\s+Version 2\.0, January 2004/
const APACHE_END = 'END OF TERMS AND CONDITIONS'
const MPL_START = /Mozilla Public License Version 2\.0/

// Standard texts, used only for a package that ships no license file and
// offers no license whose text this file already contains.
const STANDARD_TEXTS = {
  MIT: `Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.`,
  'BSD-3-Clause': `Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.`,
}

const warnings = []

function readText(path) {
  const text = readFileSync(path, 'utf8')
  if (text.includes('\uFFFD')) warnings.push(`${path} is not valid UTF-8; check its text in the output`)
  return text
    .replace(/^\uFEFF/, '')
    .replace(/\r\n?/g, '\n')
    .split('\n')
    .map((line) => line.trimEnd())
    .join('\n')
    .replace(/^\n+|\n+$/g, '')
}

const collapse = (text) => text.replace(/\s+/g, ' ').trim()
const compareText = (a, b) => (a < b ? -1 : a > b ? 1 : 0)
const comparePackages = (a, b) =>
  compareText(a.name.toLowerCase(), b.name.toLowerCase()) ||
  a.version.localeCompare(b.version, 'en', { numeric: true })

function heading(text, underline) {
  return [text, underline.repeat(text.length)].join('\n')
}

function noticeFiles(dir) {
  const files = []
  for (const name of readdirSync(dir).sort(compareText)) {
    if (!NOTICE_FILE.test(name) || NOT_A_NOTICE.has(extname(name).toLowerCase())) continue
    const path = join(dir, name)
    if (statSync(path).isDirectory()) {
      for (const inner of readdirSync(path).sort(compareText)) {
        const innerPath = join(path, inner)
        if (statSync(innerPath).isFile()) files.push({ name: `${name}/${inner}`, text: readText(innerPath) })
      }
    } else {
      files.push({ name, text: readText(path) })
    }
  }
  return files
}

// Splits a file around a copy of the Apache License 2.0 terms, so the terms
// can be printed once for every package while each file's own notices stay.
// Some copies omit the closing "END OF TERMS AND CONDITIONS" line.
function splitApache(text) {
  const match = APACHE_START.exec(text)
  if (!match) return null
  const endAt = text.indexOf(APACHE_END, match.index)
  const lineStart = text.lastIndexOf('\n', match.index) + 1
  const end = endAt < 0 ? text.length : endAt + APACHE_END.length
  return {
    before: text.slice(0, lineStart).trim(),
    body: text.slice(lineStart, end),
    after: text.slice(end).trim(),
  }
}

// Copies of the terms that differ only in layout, in the scheme of the
// apache.org address or in the closing line count as the same text.
function apacheKey(body) {
  return collapse(body)
    .replace(/https:\/\//g, 'http://')
    .replace(new RegExp(` ?${APACHE_END}$`), '')
}

// The unfilled "How to apply" appendix is instructions, not a notice.
function isApacheAppendixTemplate(text) {
  const flat = collapse(text)
  return (
    flat === '' ||
    (flat.startsWith('APPENDIX: How to apply the Apache License to your work') &&
      /[[{]yyyy[\]}]/.test(flat) &&
      flat.endsWith('limitations under the License.'))
  )
}

function splitOrAlternatives(expression) {
  const normalized = expression.trim().replace(/\s*\/\s*/g, ' OR ').replace(/\s+/g, ' ')
  if (/[()]|\bAND\b|\bWITH\b/.test(normalized)) return null
  return normalized.split(' OR ')
}

// Groups "MIT/Apache-2.0", "MIT OR Apache-2.0" and "Apache-2.0 OR MIT" alike.
function normalizeExpression(expression) {
  const alternatives = splitOrAlternatives(expression)
  if (!alternatives) return expression.trim().replace(/\s*\/\s*/g, ' OR ').replace(/\s+/g, ' ')
  return [...new Set(alternatives)].sort((a, b) => compareText(a.toLowerCase(), b.toLowerCase())).join(' OR ')
}

// Names only: drops the e-mail and web addresses that author fields carry.
function authorNames(authors) {
  return authors
    .map((author) => author.replace(/\s*<[^>]*>/g, '').replace(/\s*\([^)]*\)/g, '').trim())
    .filter(Boolean)
}

function loadCrates() {
  const cargo = (args) =>
    execFileSync('cargo', args, { cwd: tauriDir, encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 })
  const metadata = JSON.parse(
    cargo(['metadata', '--format-version', '1', '--locked', '--offline', '--filter-platform', TARGET]),
  )
  const byId = new Map(metadata.packages.map((pkg) => [pkg.id, pkg]))
  const nodes = new Map(metadata.resolve.nodes.map((node) => [node.id, node]))
  const isProcMacro = (pkg) => pkg.targets.some((target) => target.kind.includes('proc-macro'))
  const root = metadata.packages.find((pkg) => pkg.name === APP_CRATE)

  // Normal (non-dev, non-build) dependencies. Procedural macros run inside
  // the compiler, so neither they nor the crates only they use are linked.
  const linked = new Set()
  const pending = [root.id]
  while (pending.length > 0) {
    const id = pending.pop()
    if (linked.has(id)) continue
    linked.add(id)
    for (const dep of nodes.get(id).deps) {
      const normal = dep.dep_kinds.some((kind) => kind.kind === null)
      if (normal && !isProcMacro(byId.get(dep.pkg))) pending.push(dep.pkg)
    }
  }

  // cargo metadata merges the features that build scripts and procedural
  // macros enable into the normal graph. cargo tree resolves features the way
  // the release build does, which drops crates only build-time features use.
  const built = new Set()
  const tree = cargo(['tree', '--locked', '--offline', '--target', TARGET, '-e', 'normal,no-proc-macro',
    '--prefix', 'none', '--format', '{p}'])
  for (const line of tree.split(/\r?\n/)) {
    const match = /^(\S+) v(\S+)/.exec(line)
    if (match) built.add(`${match[1]} ${match[2]}`)
  }

  const crates = []
  for (const id of linked) {
    const pkg = byId.get(id)
    const key = `${pkg.name} ${pkg.version}`
    if (pkg.id === root.id || !built.has(key)) continue
    built.delete(key)
    if (!pkg.source || !pkg.source.startsWith('registry+')) {
      throw new Error(`${key} does not come from crates.io; add its notice by hand.`)
    }
    const dir = dirname(pkg.manifest_path)
    const files = noticeFiles(dir)
    if (pkg.license_file && !files.some((file) => file.name === pkg.license_file)) {
      files.push({ name: pkg.license_file, text: readText(join(dir, pkg.license_file)) })
    }
    crates.push({
      name: pkg.name,
      version: pkg.version,
      license: pkg.license ?? (pkg.license_file ? `see ${pkg.license_file}` : 'not declared'),
      authors: authorNames(pkg.authors ?? []),
      repository: pkg.repository ?? pkg.homepage ?? null,
      files,
    })
  }
  built.delete(`${root.name} ${root.version}`)
  if (built.size > 0) {
    throw new Error(`cargo tree lists crates outside the normal dependency graph: ${[...built].join(', ')}`)
  }
  return crates.sort(comparePackages)
}

function loadNpmPackages() {
  const lock = JSON.parse(readFileSync(join(appDir, 'package-lock.json'), 'utf8'))
  if (lock.lockfileVersion !== 3) {
    throw new Error(`Expected package-lock.json version 3, found ${lock.lockfileVersion}.`)
  }
  const packages = []
  for (const [path, entry] of Object.entries(lock.packages)) {
    if (path === '' || entry.dev) continue
    const dir = join(appDir, path)
    if (!existsSync(dir)) throw new Error(`${path} is not installed; run npm ci in app/ first.`)
    const manifest = JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8'))
    const author = typeof manifest.author === 'string' ? manifest.author : manifest.author?.name
    const repository = typeof manifest.repository === 'string' ? manifest.repository : manifest.repository?.url
    packages.push({
      name: entry.name ?? path.slice(path.lastIndexOf('node_modules/') + 'node_modules/'.length),
      version: entry.version,
      license: entry.license ?? manifest.license ?? 'not declared',
      authors: authorNames(author ? [author] : []),
      repository: repository ?? manifest.homepage ?? null,
      files: noticeFiles(dir),
    })
  }
  return packages.sort(comparePackages)
}

function nvidiaNotice(path) {
  const match = /^\/\*\n([\s\S]*?)\n\*\//.exec(readText(path))
  if (!match) throw new Error(`${path} does not start with its license comment.`)
  return match[1]
    .split('\n')
    .map((line) => line.replace(/^\* ?/, ''))
    .join('\n')
}

// The most common copy of the Apache License 2.0 terms among the packages is
// the one printed in part 4.1; the others point to it.
function chooseApacheText(packages) {
  const mostCommon = (counts) =>
    [...counts.entries()].sort((a, b) => b[1].count - a[1].count || compareText(a[0], b[0]))[0]
  const keys = new Map()
  for (const pkg of packages) {
    for (const file of pkg.files) {
      const apache = splitApache(file.text)
      if (!apache) continue
      const key = apacheKey(apache.body)
      const entry = keys.get(key) ?? { count: 0, copies: new Map() }
      const copy = entry.copies.get(collapse(apache.body)) ?? { count: 0, body: apache.body }
      entry.count += 1
      copy.count += 1
      entry.copies.set(collapse(apache.body), copy)
      keys.set(key, entry)
    }
  }
  if (keys.size === 0) throw new Error('No package ships the Apache License 2.0 text.')
  const [key, entry] = mostCommon(keys)
  return { key, body: mostCommon(entry.copies)[1].body }
}

function renderFile(file, texts) {
  const apache = splitApache(file.text)
  if (apache && apacheKey(apache.body) === texts.apacheKey) {
    const parts = []
    if (apache.before) parts.push(apache.before)
    parts.push('[The Apache License 2.0; the text is in part 4.1.]')
    if (!isApacheAppendixTemplate(apache.after)) parts.push(apache.after)
    return parts.join('\n\n')
  }
  const mplAt = file.text.search(MPL_START)
  if (mplAt >= 0 && collapse(file.text.slice(mplAt)) === texts.mplKey) {
    const before = file.text.slice(0, mplAt).trim()
    return [before, '[The Mozilla Public License 2.0; see part 4.2.]'].filter(Boolean).join('\n\n')
  }
  return file.text
}

function renderMissingFiles(pkg) {
  const lines = [`No license file is included. Declared license: ${pkg.license}.`]
  const alternatives = splitOrAlternatives(pkg.license) ?? []
  if (alternatives.includes('Apache-2.0')) {
    lines.push('', '[The Apache License 2.0; the text is in part 4.1.]')
  } else if (alternatives.length === 1 && STANDARD_TEXTS[alternatives[0]]) {
    if (pkg.authors.length > 0) lines.push(`Authors: ${pkg.authors.join(', ')}.`)
    if (pkg.repository) lines.push(`Repository: ${pkg.repository}`)
    lines.push('', `The standard ${alternatives[0]} license text:`, '', STANDARD_TEXTS[alternatives[0]])
    warnings.push(`${pkg.name} ${pkg.version}: no license file; printed the standard ${alternatives[0]} text`)
  } else {
    lines.push('', `[See https://spdx.org/licenses/ for the text of ${pkg.license}.]`)
    warnings.push(`${pkg.name} ${pkg.version}: no license file and no standard text for "${pkg.license}"`)
  }
  return lines.join('\n')
}

// One block per distinct set of notice files, listing every package that
// ships exactly that set, inside one section per declared license.
function renderPackageGroups(packages, texts, kind, number) {
  const groups = new Map()
  for (const pkg of packages) {
    const license = normalizeExpression(pkg.license)
    if (!groups.has(license)) groups.set(license, [])
    groups.get(license).push(pkg)
  }
  const sections = []
  let index = 0
  for (const license of [...groups.keys()].sort((a, b) => compareText(a.toLowerCase(), b.toLowerCase()))) {
    index += 1
    const members = groups.get(license)
    const blocks = new Map()
    for (const pkg of members) {
      const body =
        pkg.files.length === 0
          ? renderMissingFiles(pkg)
          : pkg.files.map((file) => `${file.name}:\n\n${renderFile(file, texts)}`).join(`\n\n${FILE_RULE}\n\n`)
      if (!blocks.has(body)) blocks.set(body, [])
      blocks.get(body).push(pkg)
    }
    const noun = members.length === 1 ? kind : `${kind}s`
    const lines = [heading(`${number}.${index}  ${license} (${members.length} ${noun})`, '-')]
    if (/\bMPL-2\.0\b/.test(license)) {
      // MPL-2.0 section 3.2(a): say where the source code is available.
      lines.push('', members.length === 1
        ? `VectorXR uses this ${noun} unmodified. Its source code is available at:`
        : `VectorXR uses these ${noun} unmodified. Their source code is available at:`, '')
      for (const pkg of members) lines.push(`  ${sourceUrl(pkg, kind)}`)
    }
    for (const [body, owners] of blocks) {
      lines.push('', THIN_RULE, ...owners.map((pkg) => `${pkg.name} ${pkg.version}`), THIN_RULE, '', body)
    }
    sections.push(lines.join('\n'))
  }
  return sections.join('\n\n\n')
}

function sourceUrl(pkg, kind) {
  return kind === 'crate'
    ? `https://crates.io/crates/${pkg.name}/${pkg.version}`
    : `https://www.npmjs.com/package/${pkg.name}/v/${pkg.version}`
}

function generate() {
  const crates = loadCrates()
  const npmPackages = loadNpmPackages()
  const license = readText(join(repoRoot, 'LICENSE'))
  const mplAt = license.search(MPL_START)
  if (mplAt < 0) throw new Error('LICENSE does not contain the Mozilla Public License 2.0 text.')
  const apache = chooseApacheText([...crates, ...npmPackages])
  const texts = { apacheKey: apache.key, mplKey: collapse(license.slice(mplAt)) }
  const nvidiaDir = join(repoRoot, 'third_party', 'nvidia-optical-flow')
  const nvidiaHeaders = ['nvOpticalFlowCommon.h', 'nvOpticalFlowD3D12.h']

  // Parts 1.2 to 1.4 describe headers that are not in the source tree (the
  // OpenXR.Loader package and the pins in layer/CMakeLists.txt and the
  // workflows). Update their text by hand when those pins change.
  const parts = [
    `${heading('VectorXR third-party notices', '=')}

DRAFT FOR REVIEW. DienerTech LLC has not yet reviewed this file. It is not
yet a final statement of the licenses of VectorXR's third-party components.

VectorXR is Copyright (c) 2026 DienerTech LLC and is licensed under the
Mozilla Public License 2.0: see LICENSE.txt next to this file (LICENSE in the
source repository).

VectorXR contains third-party software. This file lists it with the copyright
notices and license texts that its licenses ask to be passed on.
scripts/generate-third-party-notices.mjs generates the file from the source
tree, app/src-tauri/Cargo.lock and app/package-lock.json; regenerate it
rather than editing it by hand.


${heading('What this file covers', '-')}

1. The OpenXR API layer, XR_APILAYER_DIENERTECH_VECTORXR.dll. It compiles in
   declarations (types, constants and inline functions) from these headers,
   and links no other third-party code:
   1.1  the NVIDIA Optical Flow SDK 5.0.7 interface headers;
   1.2  the Khronos OpenXR SDK headers;
   1.3  the OpenXR loader interface, which the layer re-declares;
   1.4  the Khronos Vulkan-Headers.
2. The VectorXR app, vectorxr-app.exe: the Rust standard library and the
   ${crates.length} Rust crates compiled into it.
3. The app's user interface: the ${npmPackages.length} npm packages it is built from.
4. The license texts that many of the entries above share.


${heading('What this file does not cover', '-')}

- Software that VectorXR uses but does not install, which comes with its own
  terms: the Microsoft Visual C++ runtime (the layer and the app link it
  dynamically), D3DCompiler_47.dll and the other Windows system libraries, the
  Microsoft Edge WebView2 Runtime that shows the app's window, NVIDIA's
  optical flow library nvofapi64.dll (installed with the NVIDIA driver and
  loaded at run time), and the OpenXR loader (each game loads the layer
  through its own loader; VectorXR neither links nor installs one).
- Tools that only run while VectorXR is built and whose own code is not in
  the installed files: the DirectX Shader Compiler (DXC) and fxc, which
  compile VectorXR's own shaders into the layer; the C++ and Rust compilers;
  Rust build-script dependencies, procedural macros and the crates only they
  use; and npm development dependencies such as Vite and Tailwind CSS.
- C++ standard library code that the compiler builds into the layer.
  Microsoft's STL is licensed under Apache-2.0 WITH LLVM-exception, which
  waives the notice conditions for compiled code.
- The installer program itself, which the Tauri bundler builds with NSIS.`,

    `${RULE}
1. OpenXR API layer
${RULE}

${heading('1.1  NVIDIA Optical Flow SDK 5.0.7 interface headers', '-')}

Files: nvOpticalFlowCommon.h and nvOpticalFlowD3D12.h, unchanged, in
third_party/nvidia-optical-flow/ of the source repository.
Source: https://developer.nvidia.com/optical-flow-sdk

Each header carries its own MIT license notice, which applies to that header
only. The notices follow verbatim, without the comment markers.

${nvidiaHeaders.map((name) => `${name}:\n\n${nvidiaNotice(join(nvidiaDir, name))}`).join('\n\n')}


${heading('1.2  Khronos OpenXR SDK headers', '-')}

Files: openxr.h, openxr_platform.h and openxr_platform_defines.h from the
OpenXR SDK. Release builds take them from the openxr-loader port of the vcpkg
registry that the release workflow pins; local builds can take them from the
OpenXR.Loader 1.1.58 NuGet package, whose headers carry the notice below.
Source: https://github.com/KhronosGroup/OpenXR-SDK

  Copyright 2017-2026 The Khronos Group Inc.
  SPDX-License-Identifier: Apache-2.0 OR MIT

VectorXR uses these headers under the Apache License 2.0 (part 4.1).


${heading('1.3  OpenXR loader interface', '-')}

layer/include/depthxr/openxr_loader_api_layer.h re-declares the loader
negotiation structures and constants from src/common/loader_interfaces.h of
the OpenXR SDK, which carries this notice:

  Copyright (c) 2017-2023, The Khronos Group Inc.
  Copyright (c) 2017 Valve Corporation
  Copyright (c) 2017 LunarG, Inc.
  SPDX-License-Identifier: Apache-2.0 OR MIT

VectorXR uses these declarations under the Apache License 2.0 (part 4.1).


${heading('1.4  Khronos Vulkan-Headers', '-')}

Files: vulkan/vulkan.h, vulkan/vulkan_core.h, vulkan/vulkan_win32.h,
vulkan/vk_platform.h and the vk_video headers that vulkan_core.h includes,
from Vulkan-Headers commit b379292b2ab6df5771ba9870d53cf8b2c9295daf (Vulkan
1.3.290).
Source: https://github.com/KhronosGroup/Vulkan-Headers

  vk_platform.h:      Copyright 2014-2024 The Khronos Group Inc.
  The other headers:  Copyright 2015-2024 The Khronos Group Inc.
  SPDX-License-Identifier: Apache-2.0

The repository as a whole is licensed under Apache-2.0 OR MIT, but these C
headers are Apache-2.0 only. The license text is in part 4.1.`,

    `${RULE}
2. VectorXR app: Rust crates
${RULE}

vectorxr-app.exe statically links the Rust standard library, which is
Copyright The Rust Project Developers and licensed under Apache-2.0 OR MIT
(the Apache License 2.0 text is in part 4.1). The Rust toolchain lists the
standard library's full notices in share/doc/rust/COPYRIGHT-library.html.

It also links the ${crates.length} crates below: the normal dependencies of
${APP_CRATE} for ${TARGET}, with features resolved as in the
release build (cargo tree -e normal,no-proc-macro). Each crate's source code
is at https://crates.io/crates/<name>/<version>.

The crates are grouped by the license that each one declares. Each entry
reproduces the license and notice files that the crate's package ships. Where
a crate offers a choice of licenses (OR), the files for every offered license
are reproduced as shipped. The Apache License 2.0 and Mozilla Public License
2.0 terms, which many crates share, are printed once, in part 4.


${renderPackageGroups(crates, texts, 'crate', 2)}`,

    `${RULE}
3. VectorXR app: npm packages
${RULE}

The app's user interface is built from these ${npmPackages.length} packages: every
package in app/package-lock.json that is not marked as a development
dependency. Some of them, such as the Vue compiler packages and TypeScript,
only run while the interface is built. Each package's source code is at
https://www.npmjs.com/package/<name>/v/<version>.

The packages are grouped and reproduced the same way as the crates in part 2.


${renderPackageGroups(npmPackages, texts, 'package', 3)}`,

    `${RULE}
4. Shared license texts
${RULE}

${heading('4.1  Apache License 2.0', '-')}

${apache.body}


${heading('4.2  Mozilla Public License 2.0', '-')}

The Mozilla Public License 2.0 is VectorXR's own license. Its full text is in
LICENSE.txt next to this file (LICENSE in the source repository) and at
https://mozilla.org/MPL/2.0/.`,
  ]

  return { text: `${parts.join('\n\n\n')}\n`, crates, npmPackages }
}

const { text, crates, npmPackages } = generate()
for (const warning of warnings) console.warn(`warning: ${warning}`)

if (process.argv.includes('--check')) {
  const current = existsSync(outputPath) ? readFileSync(outputPath, 'utf8').replace(/\r\n/g, '\n') : ''
  if (current !== text) {
    console.error('THIRD-PARTY-NOTICES.txt is out of date. Run: node scripts/generate-third-party-notices.mjs')
    process.exit(1)
  }
  console.log('THIRD-PARTY-NOTICES.txt is current.')
} else {
  writeFileSync(outputPath, text)
  console.log(`Wrote ${outputPath}: ${crates.length} Rust crates, ${npmPackages.length} npm packages.`)
}
