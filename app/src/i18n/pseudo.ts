// The pseudo-locale (en-XA): every English message with its letters accented
// and padded about 30%, inside brackets. Text still plain English is text that
// skipped the catalog; a cut-off bracket is a label that truncates. {name}
// placeholders and {slot} markers are kept as they are.
const accents: Record<string, string> = {
  a: 'á', b: 'ƀ', c: 'ç', d: 'ď', e: 'é', f: 'ƒ', g: 'ĝ', h: 'ĥ', i: 'í', j: 'ĵ', k: 'ķ', l: 'ľ', m: 'ɱ',
  n: 'ñ', o: 'ö', p: 'þ', q: 'ǫ', r: 'ŕ', s: 'š', t: 'ť', u: 'ü', v: 'ṽ', w: 'ŵ', x: 'ẋ', y: 'ý', z: 'ž',
  A: 'Á', B: 'Ɓ', C: 'Ç', D: 'Ď', E: 'É', F: 'Ƒ', G: 'Ĝ', H: 'Ĥ', I: 'Í', J: 'Ĵ', K: 'Ķ', L: 'Ľ', M: 'Ṁ',
  N: 'Ñ', O: 'Ö', P: 'Þ', Q: 'Ǫ', R: 'Ŕ', S: 'Š', T: 'Ť', U: 'Ü', V: 'Ṽ', W: 'Ŵ', X: 'Ẋ', Y: 'Ý', Z: 'Ž',
}

const padding = '~ﾟ·'

export function pseudoMessage(message: string): string {
  let letters = 0
  const accented = message
    .split(/(\{[^{}]+\})/)
    .map((part) => {
      if (part.startsWith('{') && part.endsWith('}')) return part
      return part.replace(/[A-Za-z]/g, (letter) => {
        letters += 1
        return accents[letter] ?? letter
      })
    })
    .join('')
  if (!message.trim()) return message
  const extra = Math.max(1, Math.round(letters * 0.3))
  let pad = ''
  for (let index = 0; index < extra; index += 1) pad += padding[index % padding.length]
  return `[${accented} ${pad}]`
}
