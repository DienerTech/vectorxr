// Generates THIRD-PARTY-NOTICES.txt in the repository root: the copyright
// notices and license texts for the third-party code in the files that the
// VectorXR installer ships. Re-run with
// `node scripts/generate-third-party-notices.mjs` after changing
// app/src-tauri/Cargo.lock, app/package-lock.json, the app's build setup, the
// vendored NVIDIA headers or the pinned Khronos headers. With `--check` it
// only reports whether the file in the repository is current, and exits with
// 1 when it is not.
//
// Everything is read locally; nothing is downloaded. The app's crate sources
// must already be in Cargo's registry (any build of the app puts them there),
// and app/node_modules must be installed (`npm ci`).
//
// Some entries are maintained by hand below: the WebView2 loader (WEBVIEW2),
// the development dependencies whose code the build puts into the interface
// (BUNDLED_DEV_PACKAGES), license texts that a package requires but does not
// ship (ADDED_NOTICES) and copyright notices for packages that ship no
// license file (COPYRIGHT_OVERRIDES). The generator warns, or stops with an
// error, when one of them no longer matches the packages it describes.
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
const WRAP_WIDTH = 78

// Files in a package that carry license terms or copyright notices. SPDX
// metadata documents (LICENSE.spdx) and source files are not notices.
const NOTICE_FILE = /^(licen[cs]e|copying|copyright|notice|unlicense|third[-_ ]?party)/i
const NOT_A_NOTICE = new Set(['.spdx', '.rs', '.js', '.cjs', '.mjs', '.ts', '.json', '.html', '.toml'])

const APACHE_START = /Apache License\s+Version 2\.0, January 2004/
const APACHE_END = 'END OF TERMS AND CONDITIONS'
const MPL_START = /Mozilla Public License Version 2\.0/

// Standard texts, used for a package that ships no license file and offers
// no license whose text this file already contains, and for ADDED_NOTICES.
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

// Recognizes the full text of a license in a notice file (with its
// whitespace collapsed), so the generator can check that every license a
// package's declared expression requires has its text in the output. A file
// that only names a license does not count.
const MIT_GRANT = /Permission is hereby granted, free of charge, to any person obtaining a copy of this software/i
const MIT_CONDITION = /shall be included in all copies or substantial portions/i
const ISC_GRANT = /Permission to use, copy, modify, and(\/or)? distribute this software for any purpose with or without fee is hereby granted/i
const BSD_GRANT = /Redistribution and use in source and binary forms, with or without modification, are permitted/i
const BSD_ENDORSEMENT = /endorse or promote products derived from this software/i
const LICENSE_TEXTS = {
  '0BSD': (text) => ISC_GRANT.test(text) && !/appear in all copies/i.test(text),
  'Apache-2.0': (text) => APACHE_START.test(text),
  'BSD-2-Clause': (text) => BSD_GRANT.test(text) && !BSD_ENDORSEMENT.test(text),
  'BSD-3-Clause': (text) => BSD_GRANT.test(text) && BSD_ENDORSEMENT.test(text),
  'BlueOak-1.0.0': (text) => /Blue Oak Model License/i.test(text),
  'CC0-1.0': (text) => /CC0 1\.0 Universal/.test(text),
  ISC: (text) => ISC_GRANT.test(text) && /appear in all copies/i.test(text),
  MIT: (text) => MIT_GRANT.test(text) && MIT_CONDITION.test(text),
  'MIT-0': (text) => MIT_GRANT.test(text) && !MIT_CONDITION.test(text),
  'MPL-2.0': (text) => MPL_START.test(text),
  'Unicode-3.0': (text) => /UNICODE LICENSE V3/.test(text),
  Unlicense: (text) => /This is free and unencumbered software released into the public domain/i.test(text),
  Zlib: (text) =>
    /Permission is granted to anyone to use this software for any purpose/i.test(text) &&
    /The origin of this software must not be misrepresented/i.test(text),
}

// The WebView2 loader. webview2-com-sys ships WebView2LoaderStatic.lib from
// Microsoft's WebView2 SDK (the Microsoft.Web.WebView2 NuGet package) and
// links it into vectorxr-app.exe statically. Part 2.1 describes this SDK
// version; the generator stops when the linked crate brings another one. The
// SDK's license text is read from licensePath: save the LICENSE.txt of the
// NuGet package there. Until then the output has a DRAFT placeholder.
const WEBVIEW2 = {
  crate: 'webview2-com-sys',
  sdkVersion: '1.0.3650.58',
  licensePath: join(repoRoot, 'third_party', 'microsoft-webview2', 'LICENSE.txt'),
}

// Development dependencies whose own code the build puts into the interface
// (app/dist), with the notice files that cover that code. Check the built
// app/dist after changing the build setup or these packages. `end` cuts a
// file before the given line.
const BUNDLED_DEV_PACKAGES = [
  {
    name: '@vitejs/plugin-vue',
    code: 'its export helper, which assembles each compiled single-file component',
    files: [{ path: 'LICENSE' }],
  },
  {
    name: 'tailwindcss',
    code: "Preflight, Tailwind CSS's base stylesheet, and the CSS it generates for the classes the interface uses",
    files: [{ path: 'LICENSE' }, { path: 'lib/css/LICENSE' }],
  },
  {
    name: 'vite',
    code: 'its module preload polyfill and the helper that preloads dynamically imported modules',
    // The rest of LICENSE.md lists the licenses of the code bundled into
    // Vite's own build tool, which does not reach app/dist.
    files: [{ path: 'LICENSE.md', end: '# Licenses of bundled dependencies', note: "Vite's own license" }],
  },
]

// License texts that a package's declared license requires but that the
// package does not ship. Each is added after the package's own files.
const ADDED_NOTICES = {
  brotli: [
    {
      license: 'BSD-3-Clause',
      copyright: 'Copyright (c) 2016 Dropbox, Inc.',
      source: 'the LICENSE of brotli-decompressor, whose authors are the same',
    },
  ],
}

// Copyright notices for packages that ship no license file. `file` names a
// file in the package with the notice in a comment near its top; `lines`
// gives a notice taken from elsewhere, and `source` says where. `draft` marks
// a package whose copyright holder could not be determined from local files.
// webview2-com and webview2-com-sys ship no license file; their repository's
// LICENSE (unchanged since it was added in 2021) names the holder.
const WEBVIEW2_RS_COPYRIGHT = {
  lines: ['Copyright (c) 2021 Bill Avery'],
  source: 'the LICENSE of their repository, https://github.com/wravery/webview2-rs',
}
const COPYRIGHT_OVERRIDES = {
  '@tauri-apps/plugin-dialog': { file: 'dist-js/index.js' },
  'alloc-stdlib': {
    lines: ['Copyright (c) 2016 Dropbox, Inc.'],
    source: 'the LICENSE of alloc-no-stdlib, which comes from the same repository',
  },
  'unic-char-property': { file: 'src/lib.rs' },
  'unic-char-range': { file: 'src/lib.rs' },
  'unic-common': { file: 'src/lib.rs' },
  'unic-ucd-ident': { file: 'src/lib.rs' },
  'unic-ucd-version': { file: 'src/lib.rs' },
  'webview2-com': WEBVIEW2_RS_COPYRIGHT,
  'webview2-com-sys': WEBVIEW2_RS_COPYRIGHT,
}

const warnings = []
const usedOverrides = new Set()

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
const compareLower = (a, b) => compareText(a.toLowerCase(), b.toLowerCase())
const comparePackages = (a, b) =>
  compareLower(a.name, b.name) || a.version.localeCompare(b.version, 'en', { numeric: true })

function heading(text, underline) {
  return [text, underline.repeat(text.length)].join('\n')
}

// wrap() keeps text passed through unbroken() on one line.
const NO_BREAK = String.fromCharCode(0xa0)
const unbroken = (text) => text.replace(/ /g, NO_BREAK)

// Fills words into lines of at most WRAP_WIDTH characters. `indent` starts
// the first line and `hanging` the others.
function wrap(text, indent = '', hanging = indent) {
  const lines = []
  let prefix = indent
  let line = ''
  for (const word of text.split(/[ \t\n]+/).filter(Boolean)) {
    if (line && prefix.length + line.length + 1 + word.length > WRAP_WIDTH) {
      lines.push(prefix + line)
      prefix = hanging
      line = word
    } else {
      line = line ? `${line} ${word}` : word
    }
  }
  lines.push(prefix + line)
  return lines.join('\n').replaceAll(NO_BREAK, ' ')
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

// Parses an SPDX license expression into { id, exception }, { and: [...] }
// and { or: [...] } nodes; AND binds more tightly than OR, and "/" is the old
// crates.io spelling of OR. Returns null for text that is not an expression.
function parseExpression(expression) {
  const tokens = expression.replace(/\//g, ' OR ').match(/[()]|[^\s()]+/g) ?? []
  const operators = new Set(['AND', 'OR', 'WITH', '(', ')'])
  let at = 0
  const parseAtom = () => {
    const token = tokens[at++]
    if (token === '(') {
      const node = parseOr()
      return node && tokens[at++] === ')' ? node : null
    }
    if (token === undefined || operators.has(token)) return null
    if (tokens[at] !== 'WITH') return { id: token }
    const exception = tokens[at + 1]
    at += 2
    return exception === undefined || operators.has(exception) ? null : { id: token, exception }
  }
  const parseList = (operator, parseItem) => {
    const items = [parseItem()]
    while (items.at(-1) && tokens[at] === operator) {
      at += 1
      items.push(parseItem())
    }
    if (items.some((item) => !item)) return null
    return items.length === 1 ? items[0] : { [operator.toLowerCase()]: items }
  }
  const parseAnd = () => parseList('AND', parseAtom)
  const parseOr = () => parseList('OR', parseAnd)
  const node = parseOr()
  return node && at === tokens.length ? node : null
}

// Sorts and deduplicates the operands, so "MIT/Apache-2.0", "MIT OR
// Apache-2.0" and "Apache-2.0 OR MIT" read alike.
function formatExpression(node, nested = false) {
  if (node.id) return node.exception ? `${node.id} WITH ${node.exception}` : node.id
  const operator = node.and ? 'AND' : 'OR'
  const operands = [...new Set((node.and ?? node.or).map((item) => formatExpression(item, true)))]
  const text = operands.sort(compareLower).join(` ${operator} `)
  return nested && operands.length > 1 ? `(${text})` : text
}

function normalizeExpression(expression) {
  const node = parseExpression(expression)
  return node ? formatExpression(node) : expression.trim().replace(/\s+/g, ' ')
}

// The parts of an expression that all apply (AND), each of which may offer a
// choice (OR).
const conjuncts = (node) => node.and ?? [node]

// The plain licenses (without exceptions) that a part offers.
function alternatives(node) {
  const items = node.or ?? [node]
  return [...new Set(items.filter((item) => item.id && !item.exception).map((item) => item.id))].sort(compareLower)
}

function satisfies(node, available) {
  if (node.and) return node.and.every((item) => satisfies(item, available))
  if (node.or) return node.or.some((item) => satisfies(item, available))
  return available.has(node.id)
}

function licenseTexts(files) {
  const found = new Set()
  for (const file of files) {
    const flat = collapse(file.text)
    for (const [id, test] of Object.entries(LICENSE_TEXTS)) if (test(flat)) found.add(id)
  }
  return found
}

// Names only: drops the e-mail and web addresses that author fields carry.
function authorNames(authors) {
  return authors
    .map((author) => author.replace(/\s*<[^>]*>/g, '').replace(/\s*\([^)]*\)/g, '').trim())
    .filter(Boolean)
}

// The license texts in ADDED_NOTICES for a package, after its own files.
function addedNotices(pkg, files) {
  const added = ADDED_NOTICES[pkg.name]
  if (!added) return []
  usedOverrides.add(`ADDED_NOTICES.${pkg.name}`)
  const shipped = licenseTexts(files)
  const notices = []
  for (const notice of added) {
    if (shipped.has(notice.license)) {
      warnings.push(`${pkg.name} ${pkg.version} now ships the ${notice.license} text; remove it from ADDED_NOTICES`)
      continue
    }
    const note = `[The package declares ${pkg.license} but does not ship the ${notice.license} text. This is ` +
      `the standard ${notice.license} text, with the copyright notice from ${notice.source}.]`
    notices.push({
      name: `${notice.license} (added by VectorXR)`,
      text: [wrap(note), notice.copyright, STANDARD_TEXTS[notice.license]].join('\n\n'),
    })
  }
  return notices
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
    const crate = {
      name: pkg.name,
      version: pkg.version,
      license: pkg.license ?? (pkg.license_file ? `see ${pkg.license_file}` : 'not declared'),
      authors: authorNames(pkg.authors ?? []),
      repository: pkg.repository ?? pkg.homepage ?? null,
      dir,
      files,
    }
    files.push(...addedNotices(crate, files))
    crates.push(crate)
  }
  built.delete(`${root.name} ${root.version}`)
  if (built.size > 0) {
    throw new Error(`cargo tree lists crates outside the normal dependency graph: ${[...built].join(', ')}`)
  }
  return crates.sort(comparePackages)
}

function npmPackage(path, entry, files) {
  const dir = join(appDir, path)
  if (!existsSync(dir)) throw new Error(`${path} is not installed; run npm ci in app/ first.`)
  const manifest = JSON.parse(readFileSync(join(dir, 'package.json'), 'utf8'))
  const author = typeof manifest.author === 'string' ? manifest.author : manifest.author?.name
  const repository = typeof manifest.repository === 'string' ? manifest.repository : manifest.repository?.url
  const pkg = {
    name: entry.name ?? path.slice(path.lastIndexOf('node_modules/') + 'node_modules/'.length),
    version: entry.version,
    license: entry.license ?? manifest.license ?? 'not declared',
    authors: authorNames(author ? [author] : []),
    repository: repository ?? manifest.homepage ?? null,
    dir,
    files: files ? files(dir) : noticeFiles(dir),
  }
  pkg.files.push(...addedNotices(pkg, pkg.files))
  return pkg
}

function bundledDevFiles(spec) {
  return (dir) =>
    spec.files.map((file) => {
      const path = join(dir, file.path)
      if (!existsSync(path)) throw new Error(`${spec.name} no longer ships ${file.path}; update BUNDLED_DEV_PACKAGES.`)
      let text = readText(path)
      if (file.end) {
        const endAt = text.indexOf(file.end)
        if (endAt < 0) throw new Error(`${spec.name}'s ${file.path} no longer contains "${file.end}".`)
        text = text.slice(0, endAt).trim()
      }
      return { name: file.note ? `${file.path} (${file.note})` : file.path, text }
    })
}

function loadNpmPackages() {
  const lock = JSON.parse(readFileSync(join(appDir, 'package-lock.json'), 'utf8'))
  if (lock.lockfileVersion !== 3) {
    throw new Error(`Expected package-lock.json version 3, found ${lock.lockfileVersion}.`)
  }
  const packages = []
  for (const [path, entry] of Object.entries(lock.packages)) {
    if (path === '' || entry.dev) continue
    packages.push(npmPackage(path, entry))
  }
  const bundledDev = []
  for (const spec of BUNDLED_DEV_PACKAGES) {
    const path = `node_modules/${spec.name}`
    const entry = lock.packages[path]
    if (!entry) throw new Error(`${spec.name} is not in app/package-lock.json; update BUNDLED_DEV_PACKAGES.`)
    if (!entry.dev) {
      warnings.push(`${spec.name} is no longer a development dependency; remove it from BUNDLED_DEV_PACKAGES`)
      continue
    }
    const pkg = npmPackage(path, entry, bundledDevFiles(spec))
    packages.push(pkg)
    bundledDev.push({ ...spec, version: pkg.version })
  }
  return { npmPackages: packages.sort(comparePackages), bundledDev }
}

function loadWebView2Loader(crates) {
  const crate = crates.find((pkg) => pkg.name === WEBVIEW2.crate)
  if (!crate) throw new Error(`The app no longer links ${WEBVIEW2.crate}; update part 2.1 and WEBVIEW2.`)
  const lib = readFileSync(join(crate.dir, 'src', 'lib.rs'), 'utf8')
  if (!/link\(name = "WebView2LoaderStatic", kind = "static"\)/.test(lib)) {
    throw new Error(`${crate.name} ${crate.version} no longer links WebView2LoaderStatic.lib statically; update part 2.1.`)
  }
  // Each release's CHANGELOG entry, or an older one below it, names the SDK
  // version that the crate's libraries come from.
  const changelog = readText(join(crate.dir, 'CHANGELOG.md'))
  const entryAt = changelog.indexOf(`## [${crate.version}]`)
  const sdk = entryAt < 0 ? null : /WebView2 SDK to (\d+(?:\.\d+){3})/.exec(changelog.slice(entryAt))
  if (!sdk) throw new Error(`Cannot find the WebView2 SDK version of ${crate.name} ${crate.version} in its CHANGELOG.md.`)
  if (sdk[1] !== WEBVIEW2.sdkVersion) {
    throw new Error(`${crate.name} ${crate.version} brings WebView2 SDK ${sdk[1]}, but part 2.1 describes ` +
      `${WEBVIEW2.sdkVersion}. Check the new SDK's license, then update WEBVIEW2.`)
  }
  let license = null
  if (existsSync(WEBVIEW2.licensePath)) {
    license = readText(WEBVIEW2.licensePath)
  } else {
    warnings.push(`the WebView2 SDK license is missing: save the LICENSE.txt of the Microsoft.Web.WebView2 ` +
      `${WEBVIEW2.sdkVersion} NuGet package as third_party/microsoft-webview2/LICENSE.txt and regenerate`)
  }
  return { crate, license }
}

function renderWebView2Loader({ crate, license }) {
  const copyright = 'Copyright (C) Microsoft Corporation'
  const lines = [
    heading('2.1  Microsoft WebView2 loader', '-'),
    '',
    wrap(`vectorxr-app.exe statically links WebView2LoaderStatic.lib, the WebView2 loader from Microsoft's ` +
      `WebView2 SDK, which is the Microsoft.Web.WebView2 ${WEBVIEW2.sdkVersion} NuGet package. The ` +
      `${crate.name} ${crate.version} crate, listed below, ships the library and links it. The loader finds ` +
      'the Microsoft Edge WebView2 Runtime installed on the computer and starts it; the Runtime is not part ' +
      'of VectorXR.'),
    `Source: https://www.nuget.org/packages/Microsoft.Web.WebView2/${WEBVIEW2.sdkVersion}`,
    '',
  ]
  if (!license || !license.includes(copyright)) lines.push(`  ${copyright}`, '')
  lines.push(
    'The license of the Microsoft.Web.WebView2 package (its LICENSE.txt):',
    '',
    license ??
      `[DRAFT: paste the LICENSE.txt of the Microsoft.Web.WebView2 ${WEBVIEW2.sdkVersion} NuGet package here]`,
  )
  return lines.join('\n')
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

// The copyright notice from COPYRIGHT_OVERRIDES for a package that ships no
// license file.
function overrideCopyright(pkg) {
  const override = COPYRIGHT_OVERRIDES[pkg.name]
  if (!override) {
    warnings.push(`${pkg.name} ${pkg.version}: no license file and no copyright notice; add one to COPYRIGHT_OVERRIDES`)
    return []
  }
  usedOverrides.add(`COPYRIGHT_OVERRIDES.${pkg.name}`)
  if (override.draft) {
    warnings.push(`${pkg.name} ${pkg.version}: copyright holder not determined; the output has a DRAFT marker`)
    return [wrap(`[DRAFT: copyright holder not determined: ${override.draft}.]`)]
  }
  if (override.lines) {
    return [...override.lines, wrap(`[Copyright notice from ${override.source}.]`)]
  }
  // The comment lines near the top of the file that state a copyright.
  const lines = readText(join(pkg.dir, override.file))
    .split('\n')
    .slice(0, 20)
    .map((line) => /^\s*(?:\/\/+|\/\*+|\*+)\s*(Copyright\b.*?)\s*(?:\*\/)?$/i.exec(line)?.[1])
    .filter(Boolean)
  if (lines.length === 0) throw new Error(`${pkg.name}'s ${override.file} has no copyright comment near its top.`)
  return [...lines, `[Copyright notice from a comment in the package's ${override.file}.]`]
}

// The start of the entry for a package that ships no license file, with a
// copyright notice from COPYRIGHT_OVERRIDES.
function renderMissingFiles(pkg) {
  const lines = [`No license file is included. Declared license: ${pkg.license}.`]
  if (pkg.authors.length > 0) lines.push(`Authors: ${pkg.authors.join(', ')}.`)
  if (pkg.repository) lines.push(`Repository: ${pkg.repository}`)
  const copyright = overrideCopyright(pkg)
  if (copyright.length > 0) lines.push('', ...copyright)
  return lines.join('\n')
}

// A package's entry: its notice files, then, for each license that its
// declared expression requires (AND) but whose text none of the files has, a
// pointer to part 4.1 when the package offers Apache-2.0. A package that
// ships no files gets a standard text instead where there is one. Warns
// about each license whose text is still missing.
function renderPackage(pkg, texts) {
  const shipsFiles = pkg.files.length > 0
  const parts = shipsFiles
    ? [pkg.files.map((file) => `${file.name}:\n\n${renderFile(file, texts)}`).join(`\n\n${FILE_RULE}\n\n`)]
    : [renderMissingFiles(pkg)]
  const available = licenseTexts(pkg.files)
  const expression = parseExpression(pkg.license)
  for (const part of expression ? conjuncts(expression) : []) {
    if (satisfies(part, available)) continue
    const offered = alternatives(part)
    const standard = offered.find((id) => STANDARD_TEXTS[id])
    if (offered.includes('Apache-2.0')) {
      parts.push(shipsFiles
        ? wrap(`[The package ships no text of ${offered.join(' or ')}. The Apache License 2.0 text is in part 4.1.]`)
        : '[The Apache License 2.0; the text is in part 4.1.]')
      available.add('Apache-2.0')
    } else if (standard && !shipsFiles) {
      parts.push(`The standard ${standard} license text:\n\n${STANDARD_TEXTS[standard]}`)
      available.add(standard)
    } else if (!shipsFiles) {
      parts.push(`[See https://spdx.org/licenses/ for the text of ${formatExpression(part)}.]`)
    }
  }
  if (!expression) {
    warnings.push(`${pkg.name} ${pkg.version}: cannot read the declared license "${pkg.license}"`)
    if (!shipsFiles) parts.push(`[See https://spdx.org/licenses/ for the text of ${pkg.license}.]`)
  }
  for (const part of expression ? conjuncts(expression) : []) {
    if (!satisfies(part, available)) {
      warnings.push(`${pkg.name} ${pkg.version}: declares ${formatExpression(expression)}, but no notice file ` +
        `has the text of ${formatExpression(part)}`)
    }
  }
  return parts.join(shipsFiles ? `\n\n${FILE_RULE}\n\n` : '\n\n')
}

// One block per distinct set of notice files, listing every package that
// ships exactly that set, inside one section per declared license.
function renderPackageGroups(packages, texts, kind, number, firstIndex = 1) {
  const groups = new Map()
  for (const pkg of packages) {
    const license = normalizeExpression(pkg.license)
    if (!groups.has(license)) groups.set(license, [])
    groups.get(license).push(pkg)
  }
  const sections = []
  let index = firstIndex - 1
  for (const license of [...groups.keys()].sort(compareLower)) {
    index += 1
    const members = groups.get(license)
    const blocks = new Map()
    for (const pkg of members) {
      const body = renderPackage(pkg, texts)
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
  return { text: sections.join('\n\n\n'), lastIndex: index }
}

function sourceUrl(pkg, kind) {
  return kind === 'crate'
    ? `https://crates.io/crates/${pkg.name}/${pkg.version}`
    : `https://www.npmjs.com/package/${pkg.name}/v/${pkg.version}`
}

function generate() {
  const crates = loadCrates()
  const { npmPackages, bundledDev } = loadNpmPackages()
  const webView2 = loadWebView2Loader(crates)
  const license = readText(join(repoRoot, 'LICENSE'))
  const mplAt = license.search(MPL_START)
  if (mplAt < 0) throw new Error('LICENSE does not contain the Mozilla Public License 2.0 text.')
  const apache = chooseApacheText([...crates, ...npmPackages])
  const texts = { apacheKey: apache.key, mplKey: collapse(license.slice(mplAt)) }
  const nvidiaDir = join(repoRoot, 'third_party', 'nvidia-optical-flow')
  const nvidiaHeaders = ['nvOpticalFlowCommon.h', 'nvOpticalFlowD3D12.h']
  const crateGroups = renderPackageGroups(crates, texts, 'crate', 2, 2)
  const npmGroups = renderPackageGroups(npmPackages, texts, 'package', 3)
  const bundledDevList = bundledDev
    .map((spec) => wrap(`${spec.name} ${spec.version}: ${spec.code}.`, '  - ', '    '))
    .join('\n')
  const devCount = bundledDev.length === 1 ? 'development dependency' : `${bundledDev.length} development dependencies`

  for (const name of Object.keys(ADDED_NOTICES)) {
    if (!usedOverrides.has(`ADDED_NOTICES.${name}`)) warnings.push(`ADDED_NOTICES lists ${name}, which is not shipped`)
  }
  for (const name of Object.keys(COPYRIGHT_OVERRIDES)) {
    if (!usedOverrides.has(`COPYRIGHT_OVERRIDES.${name}`)) {
      warnings.push(`COPYRIGHT_OVERRIDES lists ${name}, which is not shipped or now ships a license file`)
    }
  }

  // Parts 1.2 to 1.4 describe headers that are not in the source tree (the
  // OpenXR.Loader package and the pins in layer/CMakeLists.txt and the
  // workflows). Update their text by hand when those pins change.
  const parts = [
    `${heading('VectorXR third-party notices', '=')}

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
2. The VectorXR app, vectorxr-app.exe: the Rust standard library, the
   WebView2 loader from Microsoft's WebView2 SDK (part 2.1) and the
   ${crates.length} Rust crates compiled into it.
3. The app's user interface: the ${npmPackages.length} npm packages it is built from.
4. The license texts that many of the entries above share.


${heading('What this file does not cover', '-')}

- Software that VectorXR uses but does not install, which comes with its own
  terms: the Windows Universal C Runtime (the app links it
  dynamically), D3DCompiler_47.dll and the other Windows system libraries, the
  Microsoft Edge WebView2 Runtime that shows the app's window (part 2.1
  covers the WebView2 loader in the app, which finds and starts the Runtime),
  NVIDIA's optical flow library nvofapi64.dll (installed with the NVIDIA
  driver and loaded at run time), and the OpenXR loader (each game loads the
  layer through its own loader; VectorXR neither links nor installs one).
- Tools that only run while VectorXR is built and whose own code is not in
  the installed files: the DirectX Shader Compiler (DXC) and fxc, which
  compile VectorXR's own shaders into the layer; the C++ and Rust compilers;
  Rust build-script dependencies, procedural macros and the crates only they
  use; and the npm development dependencies that only process the
  interface's code, such as vue-tsc, Autoprefixer and the Tauri CLI. The
  development dependencies that put some of their own code into the
  interface, such as Vite and Tailwind CSS, are listed in part 3.
- The Microsoft C and C++ runtime code that the compiler builds into the
  layer (it links the Visual C++ runtime statically) and into the app (its
  vcruntime part). Microsoft's STL is licensed under Apache-2.0 WITH
  LLVM-exception, which waives the notice conditions for compiled code.
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
2. VectorXR app: Rust crates and the WebView2 loader
${RULE}

vectorxr-app.exe statically links the Rust standard library, which is
Copyright The Rust Project Developers and licensed under Apache-2.0 OR MIT
(the Apache License 2.0 text is in part 4.1). The Rust toolchain lists the
standard library's full notices in share/doc/rust/COPYRIGHT-library.html.

${wrap(`It also links Microsoft's WebView2 loader (part 2.1) and the ${crates.length} crates in parts 2.2 to ` +
  `2.${crateGroups.lastIndex}: the normal dependencies of ${APP_CRATE} for ${TARGET}, with features resolved as ` +
  `in the release build (${unbroken('cargo tree -e normal,no-proc-macro')}). Each crate's source code is at ` +
  'https://crates.io/crates/<name>/<version>.')}

The crates are grouped by the license that each one declares. Each entry
reproduces the license and notice files that the crate's package ships. Where
a crate offers a choice of licenses (OR), the files for every offered license
are reproduced as shipped. Where a package ships no license file, or not the
text of every license it requires, its entry says so, adds the missing text
and says where any copyright notice it adds comes from. The Apache License
2.0 and Mozilla Public License 2.0 terms, which many crates share, are
printed once, in part 4.


${renderWebView2Loader(webView2)}


${crateGroups.text}`,

    `${RULE}
3. VectorXR app: npm packages
${RULE}

${wrap(`The app's user interface is built from these ${npmPackages.length} packages: every package in ` +
  'app/package-lock.json that is not marked as a development dependency, and the ' +
  `${devCount} whose own code the build puts into the interface:`)}
${bundledDevList}
Some of the other packages, such as the Vue compiler packages and TypeScript,
only run while the interface is built. Each package's source code is at
https://www.npmjs.com/package/<name>/v/<version>.

The packages are grouped and reproduced the same way as the crates in part 2.


${npmGroups.text}`,

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
