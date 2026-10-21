import { hasMessage, t, type MessageParams } from '../i18n/index.ts'

// An error the backend codes for translation (src-tauri/src/user_error.rs):
// U+0001 "vxr:" {code, params, message} U+0001, then the English message.
const codedError = /\u0001vxr:(\{.*?\})\u0001/gs

interface CodedError {
  code: string
  params: Record<string, string>
  message: string
}

function parseCoded(json: string): CodedError | null {
  try {
    const value = JSON.parse(json) as Partial<CodedError>
    return typeof value.code === 'string' && typeof value.message === 'string'
      ? { code: value.code, params: value.params ?? {}, message: value.message }
      : null
  } catch {
    return null
  }
}

// The error in the app's language when it is one coded error alone; else the
// backend's English with any codes removed.
function translateBackendError(text: string): string {
  const matches = [...text.matchAll(codedError)]
  if (!matches.length) return text
  if (matches.length === 1 && matches[0].index === 0) {
    const coded = parseCoded(matches[0][1])
    const key = coded && `errors.${coded.code}`
    if (coded && key && hasMessage(key) && text.slice(matches[0][0].length) === coded.message) {
      const params: MessageParams = { ...coded.params }
      // A registry slice is named by its id; its label is translated.
      if (typeof params.slice === 'string') {
        const slice = `layers.slices.${params.slice}`
        if (hasMessage(slice)) params.slice = t(slice)
      }
      return t(key, params)
    }
  }
  return text.replace(codedError, '')
}

// Tauri commands reject with the backend's error string, not an Error, so
// reading only `Error.message` would replace the real cause (a path, a JSON
// line and column, a registry error) with the generic fallback.
export function errorText(error: unknown, fallback: string): string {
  if (error instanceof Error && error.message.trim()) {
    return translateBackendError(error.message)
  }
  if (typeof error === 'string' && error.trim()) {
    return translateBackendError(error)
  }
  return fallback
}
