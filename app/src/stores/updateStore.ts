import { computed, reactive } from 'vue'

import { checkForUpdates, type GitHubReleaseInfo, type UpdateCheckStatus } from '../lib/updates'

interface UpdateState {
  status: UpdateCheckStatus
  error: string
  latestRelease: GitHubReleaseInfo | null
}

// Single source of truth for the GitHub release check, shared between the sidebar
// indicator and the About page so they can never disagree or show stale results.
const state = reactive<UpdateState>({
  status: 'idle',
  error: '',
  latestRelease: null,
})

let inFlight: Promise<void> | null = null

const updateAvailable = computed(() => state.status === 'available' && state.latestRelease !== null)

export function useUpdateStore() {
  // Runs the release check and stores the result app-wide. Concurrent calls share the
  // same in-flight request. `force` lets the About page's manual "Check" button re-run
  // after a prior result; without it, a check is skipped once one has completed, so the
  // startup auto-check and the About page's mount check never fire duplicate requests.
  async function check(currentVersion: string, force = false): Promise<void> {
    if (inFlight) {
      return inFlight
    }
    if (!force && state.status !== 'idle') {
      return
    }

    state.status = 'checking'
    state.error = ''

    inFlight = (async () => {
      try {
        const result = await checkForUpdates(currentVersion)
        state.latestRelease = result.latestRelease

        if (!result.latestRelease) {
          state.status = 'unavailable'
        } else if (result.updateAvailable) {
          state.status = 'available'
        } else {
          state.status = 'upToDate'
        }
      } catch (error) {
        state.status = 'error'
        state.error = error instanceof Error ? error.message : 'Unable to check for updates right now.'
      } finally {
        inFlight = null
      }
    })()

    return inFlight
  }

  return {
    state,
    updateAvailable,
    check,
  }
}
