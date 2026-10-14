// Tauri commands reject with the backend's error string, not an Error, so
// reading only `Error.message` would replace the real cause (a path, a JSON
// line and column, a registry error) with the generic fallback.
export function errorText(error: unknown, fallback: string): string {
  if (error instanceof Error && error.message.trim()) {
    return error.message
  }
  if (typeof error === 'string' && error.trim()) {
    return error
  }
  return fallback
}
