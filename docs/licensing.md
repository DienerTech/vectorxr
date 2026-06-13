# Licensing — distribution checklist

VectorXR is MIT-licensed (`LICENSE` at the repo root, "Copyright (c) 2026 DienerTech LLC").

Copyright is automatic, so the in-app UI does **not** need to display a formal copyright
notice. As of June 2026 the About tab was retired and its content folded into Home; Home only
shows "Developed independently by DienerTech LLC." That is fine for the UI — the licensing
obligations below live at the **distribution** level, not in the Vue front end.

## TODO before launch

- [ ] **Ship our own `LICENSE` in the Tauri bundle.** The MIT permission notice must be included
      with distributed copies. Right now `app/src-tauri/tauri.conf.json` `bundle.resources` only
      ships the layer DLL/JSON — the root `LICENSE` is **not** bundled. Either add `LICENSE` to
      `bundle.resources`, or set the NSIS/WiX installer `licenseFile` so the license is shipped
      and shown during install.

- [ ] **Add a third-party notices file.** Bundled dependencies carry their own attribution
      requirements — Apache-2.0 (the OpenXR loader) and BSD in particular require reproducing
      their notices in distributions. Aggregate the npm/Vue/Tauri and Rust crate licenses into a
      `THIRD-PARTY-NOTICES.txt` shipped alongside `LICENSE`. This is the most commonly-missed
      obligation. (Tooling: `cargo about` for crates, `license-checker` / `licenses` for npm.)

## Optional

- A muted `© 2026 DienerTech LLC · MIT` line could be restored on the Home tab for polish/trust,
  but it is not legally required.

> Not legal advice — engineering/compliance common practice. Have counsel confirm before release
> if in doubt.
