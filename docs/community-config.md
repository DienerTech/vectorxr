# Community links

Home and About load `community.json` from the public repository's `master` branch.
Publish this file to `master` before distributing an app build that depends on it.
The local file is not bundled as a fallback: a missing file or failed request leaves
joining disabled.

To rotate the invite, update `discordInvite` in that file to an HTTPS Discord invite
and publish the change. No application release is needed. Set it to `null` to pause
joining. A missing property, empty or invalid URL, or null document also disables
joining. Only `discord.gg/<code>` and `discord.com/invite/<code>` URLs are accepted.

The app fetches once when either Discord button first mounts, shares the result
between Home and About, and times out after eight seconds. Unavailable/error states
offer Retry. Requests bypass the browser cache; GitHub propagation can still take
time. A successful result stays in memory for that app session, so restart the app
to pick up changes. There is no persistent or hardcoded invite fallback. Revoke an
old invite in Discord if it must stop working immediately for already-running apps.
