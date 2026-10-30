// Finds user-facing English that skipped the catalog: text and static
// attributes in .vue templates, string literals in template expressions, and
// UI-shaped literals in scripts (label: 'Foo', errorText(e, 'Failed ...'),
// x ? 'Some sentence' : ...). A heuristic, so two escapes exist: an
// <!-- i18n-ignore --> comment skips the next element, and a line ending in
// // i18n-ignore is skipped in scripts. Logs (console.*) stay English.
//
// CLI: node tests/i18nHardcoded.mjs src/components/Foo.vue ...
import { readFileSync } from 'node:fs'
import { parse } from 'vue/compiler-sfc'

const translatableAttributes = new Set([
  'label', 'title', 'placeholder', 'aria-label', 'alt', 'description', 'legend', 'subtitle', 'hint', 'heading',
  'tooltip', 'summary', 'text', 'caption', 'empty-text', 'help', 'unit', 'suffix', 'prefix', 'badge', 'detail',
  'note', 'confirm-label', 'cancel-label', 'action-label',
])

// Text that needs no translation: numbers, units, symbols, and the names the
// glossary keeps in English.
const glossary = JSON.parse(readFileSync(new URL('../src/i18n/glossary.json', import.meta.url), 'utf8'))
const keptTerms = glossary.terms.map((entry) => entry.term)
const untranslatable = new RegExp(
  `^(?:[\\s\\d.,:;/+\\-−–—×x*%°'"()\\[\\]<>=!?#&|·•…~^_@$]|ms|Hz|fps|px|m|cm|mm|s|GB|MB|KB|dB|deg|${keptTerms
    .map((term) => term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'))
    .join('|')})*$`,
  'i',
)

function needsTranslation(text) {
  const plain = text.replace(/&[a-z]+;|&#\d+;/gi, ' ').trim()
  // One letter alone (an icon glyph such as "i") is not text.
  return /[A-Za-z].*[A-Za-z]/s.test(plain) && !untranslatable.test(plain)
}

// A string literal inside a template or script expression that reads as UI
// text: a capitalised word followed by more words, or several plain words.
// CSS class lists (lowercase, hyphens, digits) and identifiers are not.
function looksLikeUiText(literal) {
  if (!needsTranslation(literal)) return false
  if (/^(?:https?:|mailto:|file:)/.test(literal.trim())) return false
  if (/^[MmLlHhVvCcSsQqTtAaZz][\d\s.,MmLlHhVvCcSsQqTtAaZz-]*$/.test(literal.trim())) return false // SVG path data
  if (/^[a-z0-9\-_:/[\].\s%#()!@&=>+,]*$/.test(literal)) {
    // Lowercase only: UI text only if it reads as a sentence fragment.
    return /\b[a-z]{2,} [a-z]{2,} [a-z]{2,}\b/.test(literal) && !/[-:[\]/]/.test(literal)
  }
  if (/^[A-Za-z][\w.-]*$/.test(literal) && !/^[A-Z][a-z]+$/.test(literal)) return false // identifier, key or code
  return /[A-Z][a-z]/.test(literal) || /\s/.test(literal.trim())
}

// Literals that are code, not text: message keys, emitted event names,
// comparisons with a value ('Escape', a layer state) and switch cases.
const literal = /(['"`])(?:\\.|(?!\1)[^\\])*\1/.source
const codeLiterals = [
  new RegExp(/\b(?:t|hasMessage|\$?emit)\(\s*/.source + literal, 'g'),
  new RegExp(/[!=]==?\s*/.source + literal, 'g'),
  new RegExp(literal + /\s*[!=]==?/.source, 'g'),
  new RegExp(/\.(?:startsWith|endsWith|includes|indexOf|split)\(\s*/.source + literal, 'g'),
  new RegExp(/\bcase\s+/.source + literal, 'g'),
]

function withoutCodeLiterals(code) {
  return codeLiterals.reduce((text, pattern) => text.replace(pattern, ' '), code)
}

function stringLiterals(expression) {
  expression = withoutCodeLiterals(expression)
  const found = []
  const pattern = /(['"`])((?:\\.|(?!\1)[^\\])*)\1/g
  let match
  while ((match = pattern.exec(expression))) {
    // Template literals: check the static text around ${...}.
    const value = match[1] === '`' ? match[2].replace(/\$\{[^}]*\}/g, ' ') : match[2]
    found.push(value)
  }
  return found
}

function walk(nodes, visit, state = { ignoreNext: false }) {
  for (const node of nodes) {
    if (node.type === 3) {
      if (/i18n-ignore/.test(node.content)) state.ignoreNext = true
      continue
    }
    if (state.ignoreNext && node.type === 1) {
      state.ignoreNext = false
      continue
    }
    visit(node)
    if (node.children) walk(node.children, visit, { ignoreNext: false })
    if (node.branches) walk(node.branches, visit, { ignoreNext: false })
  }
}

function checkTemplate(path, template, findings) {
  const report = (node, text, kind) => findings.push({ path, line: node.loc.start.line, kind, text: text.trim().slice(0, 80) })
  walk(template.ast.children, (node) => {
    if (node.type === 2 && needsTranslation(node.content)) report(node, node.content, 'text')
    if (node.type === 5 && node.content?.content) {
      for (const literal of stringLiterals(node.content.content)) if (looksLikeUiText(literal)) report(node, literal, 'expression')
    }
    if (node.type === 1) {
      if (['code', 'kbd', 'pre', 'style', 'script'].includes(node.tag)) {
        node.children = []
        return
      }
      for (const prop of node.props ?? []) {
        if (prop.type === 6 && translatableAttributes.has(prop.name) && prop.value && needsTranslation(prop.value.content)) {
          report(prop, prop.value.content, `attribute ${prop.name}`)
        }
        if (prop.type === 7 && prop.exp?.content && prop.name !== 'model') {
          // :class / :style literals are class names, not text.
          const target = prop.arg?.content
          if (['class', 'style', 'key', 'id', 'to', 'd', 'points', 'transform', 'viewBox', 'keypath'].includes(target)) continue
          for (const literal of stringLiterals(prop.exp.content)) if (looksLikeUiText(literal)) report(prop, literal, `binding ${target ?? prop.name}`)
        }
      }
    }
  })
}

const scriptPatterns = [
  // label: 'Foo', title: "Bar baz", description: `...`
  /\b(?:label|title|subtitle|description|status|hint|tooltip|placeholder|summary|detail|reason|message|heading|legend|caption|badge|text|note|action|unit)\s*:\s*(['"`])((?:\\.|(?!\1)[^\\])*)\1/g,
  // errorText(error, 'Failed to ...')
  /errorText\([^,()]+,\s*(['"`])((?:\\.|(?!\1)[^\\])*)\1/g,
  // = 'Some text', ? 'Some text' : 'Other', return 'Some text', push(`${x} is bad`)
  /(?:=|\?|:|\breturn|\(|,|\[|\|\||\?\?)\s*(['"`])((?:\\.|(?!\1)[^\\])*)\1/g,
]

function checkScript(path, source, startLine, findings) {
  const lines = source.split('\n')
  lines.forEach((source, index) => {
    let line = source
    const trimmed = line.trim()
    if (/i18n-ignore/.test(line) || trimmed.startsWith('//') || trimmed.startsWith('*') || trimmed.startsWith('/*')) return
    if (/\bconsole\.|\bimport\b|\bthrow new Error\b|\bnew Error\(/.test(line)) return
    const seen = new Set()
    line = withoutCodeLiterals(line)
    for (const pattern of scriptPatterns) {
      pattern.lastIndex = 0
      let match
      while ((match = pattern.exec(line))) {
        const raw = match[2]
        const value = match[1] === '`' ? raw.replace(/\$\{[^}]*\}/g, ' ') : raw
        if (seen.has(value)) continue
        seen.add(value)
        if (looksLikeUiText(value)) findings.push({ path, line: startLine + index, kind: 'script', text: value.trim().slice(0, 80) })
      }
    }
  })
}

export function findHardcodedText(path, source = readFileSync(path, 'utf8')) {
  const findings = []
  if (path.endsWith('.vue')) {
    const { descriptor } = parse(source, { filename: path })
    if (descriptor.template?.ast) checkTemplate(path, descriptor.template, findings)
    for (const block of [descriptor.script, descriptor.scriptSetup]) {
      if (block) checkScript(path, block.content, block.loc.start.line, findings)
    }
  } else {
    checkScript(path, source, 1, findings)
  }
  return findings
}

if (process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, '/').split('/').pop())) {
  let total = 0
  for (const path of process.argv.slice(2)) {
    for (const finding of findHardcodedText(path)) {
      total += 1
      console.log(`${finding.path}:${finding.line} [${finding.kind}] ${finding.text}`)
    }
  }
  console.log(`${total} finding(s)`)
  process.exitCode = total ? 1 : 0
}
