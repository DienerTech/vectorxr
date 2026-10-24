# Translating VectorXR

The app's text lives in `locales/<language>/<area>.json`, one file per area of the app. English (`locales/en/`) is the source; every other language mirrors its files and keys. All ten translations started as machine translations (`_meta.json` says `"machineTranslated": true`), so corrections from native speakers are very welcome.

## Editing a translation

- Change only the values, never the keys. Keep every `{placeholder}` exactly as written; move it wherever your grammar needs it.
- Keys ending in `_one`, `_few`, `_many` or `_other` are plural forms. Give the forms your language uses ([CLDR plural rules](https://www.unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html)); `_other` is always required.
- Keep the names in [`glossary.json`](glossary.json) in English (VectorXR, OpenXR, Turbo, Pivot, Quadviews, Frame Generation and the others marked `strict`).
- Never translate file names, settings keys, units (ms, Hz, fps) or key names (Ctrl, F10).
- When a reviewed language no longer needs the machine-translation note, set `"machineTranslated": false` in its `_meta.json`.

## Checking your change

From `app/`:

```bash
node tests/i18nCatalogs.mjs de
```

It reports missing or extra keys, plural forms, placeholders and glossary terms. `npm test` runs the same check for every language.

To look at the app in a language, pick it in **Settings → Language**. Development builds (`npm run tauri:dev`) also offer a pseudo-locale that marks text that is not translated yet.

## Other places with translated text

- The headset OSD: `layer/src/osd_strings.cpp` (compiled into the layer).
- The installer: `src-tauri/windows/install-notice.<language>.txt`, and `src-tauri/windows/languages/` for installer strings Tauri does not ship.
- Adding a language means adding it in `locales.ts`, `osd_strings.cpp` and the installer (`tauri.conf.json`, `windows/nsis-hooks.nsh`); `tests/i18n.test.mjs` checks the three agree.
