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

## Patreon

The `patreon` entry drives a Patreon button beside the Ko-fi buttons on Home and
About. It shows only when `enabled` is `true` and `url` is an HTTPS page on
`patreon.com` or `www.patreon.com` (not the bare site root). Anything else,
including a missing entry or a failed request, hides it.

```json
"patreon": {
  "enabled": true,
  "url": "https://www.patreon.com/<page>",
  "message": "Optional text shown above the button."
}
```

`message` is optional plain text, at most 400 characters. It can be one string,
or one per app language code, with English as the fallback:
`{ "en": "Early access builds", "de": "Frühzugang zu neuen Builds" }`. The button
label itself is translated in the app.

### Preview before publishing

To see a change before it is published, put a `community.override.json` file beside
the app's settings, in `%LOCALAPPDATA%\VectorXR\config\`. Its top-level fields
replace the published file's (for example, only `patreon`), and it still applies
when the published file can't be fetched. Restart the app to pick it up, and delete
the file to go back to the published config. In a browser preview of the app
(`npm run dev`), the same JSON is read from `localStorage` under
`vectorxr-community-override`.

## Supporter acknowledgements

The **Supporters** page reads a public list of credited Patreon members from the `supporters` branch of
this repository (`supporters.json`; the address is set by `feed` in
[config/supporter-credits.json](../config/supporter-credits.json) and read when the app is built).
DienerTech's Patreon sync writes it; it holds only section headings and the display names members
agreed to publish, in this shape:

```json
{ "version": 1, "tiers": [{ "label": "Sponsors", "names": ["A name"] }] }
```

The app fetches it when **Supporters** is opened or **Refresh** is selected, reuses a successful fetch
for an hour, and can show the last list for up to seven days offline. An empty list replaces the
previous credits at once. While no file is published (HTTP 404 or 410, including before the
`supporters` branch exists), the page says the Patreon is coming soon and clears any saved list;
only a failed request says the list is unavailable. Names are shown as
text only, up to 80 characters, with control and direction-override characters refused.
