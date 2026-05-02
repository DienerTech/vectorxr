export type UpdateCheckStatus = 'idle' | 'checking' | 'upToDate' | 'available' | 'unavailable' | 'error'

export interface GitHubReleaseAsset {
  name: string
  browserDownloadUrl: string
}

export interface GitHubReleaseInfo {
  version: string
  tagName: string
  name: string
  htmlUrl: string
  publishedAt: string
  body: string
  assets: GitHubReleaseAsset[]
}

export interface UpdateCheckResult {
  currentVersion: string
  latestRelease: GitHubReleaseInfo | null
  updateAvailable: boolean
}

interface GitHubReleaseApiAsset {
  name?: unknown
  browser_download_url?: unknown
}

interface GitHubReleaseApiResponse {
  tag_name?: unknown
  name?: unknown
  html_url?: unknown
  published_at?: unknown
  body?: unknown
  assets?: unknown
}

const latestReleaseUrl = 'https://api.github.com/repos/DienerTech/vectorxr/releases/latest'

function normalizeVersion(version: string): string {
  return version.trim().replace(/^v/i, '')
}

function parseVersionParts(version: string): number[] {
  const normalized = normalizeVersion(version)
  const core = normalized.split(/[+-]/, 1)[0]
  return core.split('.').map((part) => {
    const parsed = Number.parseInt(part, 10)
    return Number.isFinite(parsed) ? parsed : 0
  })
}

export function compareVersions(left: string, right: string): number {
  const leftParts = parseVersionParts(left)
  const rightParts = parseVersionParts(right)
  const length = Math.max(leftParts.length, rightParts.length)

  for (let index = 0; index < length; index += 1) {
    const leftPart = leftParts[index] ?? 0
    const rightPart = rightParts[index] ?? 0

    if (leftPart > rightPart) {
      return 1
    }

    if (leftPart < rightPart) {
      return -1
    }
  }

  return 0
}

function asString(value: unknown): string {
  return typeof value === 'string' ? value : ''
}

function parseRelease(data: GitHubReleaseApiResponse): GitHubReleaseInfo {
  const tagName = asString(data.tag_name)
  const htmlUrl = asString(data.html_url)

  if (!tagName || !htmlUrl) {
    throw new Error('GitHub did not return a valid release')
  }

  const assets = Array.isArray(data.assets)
    ? data.assets
        .map((asset): GitHubReleaseAsset | null => {
          const apiAsset = asset as GitHubReleaseApiAsset
          const name = asString(apiAsset.name)
          const browserDownloadUrl = asString(apiAsset.browser_download_url)

          if (!name || !browserDownloadUrl) {
            return null
          }

          return { name, browserDownloadUrl }
        })
        .filter((asset): asset is GitHubReleaseAsset => asset !== null)
    : []

  return {
    version: normalizeVersion(tagName),
    tagName,
    name: asString(data.name) || tagName,
    htmlUrl,
    publishedAt: asString(data.published_at),
    body: asString(data.body),
    assets,
  }
}

export async function checkForUpdates(currentVersion: string): Promise<UpdateCheckResult> {
  const response = await fetch(latestReleaseUrl, {
    headers: {
      Accept: 'application/vnd.github+json',
    },
  })

  if (response.status === 404) {
    return {
      currentVersion,
      latestRelease: null,
      updateAvailable: false,
    }
  }

  if (!response.ok) {
    throw new Error(`GitHub update check failed with HTTP ${response.status}`)
  }

  const release = parseRelease((await response.json()) as GitHubReleaseApiResponse)

  return {
    currentVersion,
    latestRelease: release,
    updateAvailable: compareVersions(release.version, currentVersion) > 0,
  }
}
