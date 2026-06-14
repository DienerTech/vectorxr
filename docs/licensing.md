# Licensing — distribution checklist

VectorXR is MIT-licensed (`LICENSE` at the repo root, "Copyright (c) 2026 DienerTech LLC").

Copyright is automatic, so the in-app UI does **not** need to display a formal copyright
notice. The About tab shows "Developed independently by DienerTech LLC." and
"(c) 2026 DienerTech LLC. Released under the MIT License." That is a courtesy, not a requirement —
the licensing obligations below live at the **distribution** level, not in the Vue front end.

## Inspiration repo source audit

Audited on 2026-06-13 against fresh shallow clones of:

- `mbucchia/Quad-Views-Foveated`
- `NobiWan/xrnecksafer`
- `fredemmott/OpenXR-API-Layers-GUI`

Checks performed:

- exact file SHA-256 matches across source/text files
- exact long-line matches after filtering generated files, lockfiles, and license boilerplate
- exact token-window matches for copied/reformatted code blocks
- comment-line matches
- function/class/struct definition-name overlap
- project-specific phrase searches for upstream names and identifiers

Result: no wholesale copied source files or copied code blocks from those projects were found.
The only implementation-level overlaps were expected generic OpenXR/Windows items:

- `xrNegotiateLoaderApiLayerInterface` in OpenXR layer manifests
- the PowerShell `Start-Process -Verb RunAs` elevation pattern in install/uninstall helpers
- `DllMain`, the standard Windows DLL entry point

The references to those projects in `README.md`, docs, and UI copy are attribution and
compatibility guidance, not vendored source. Their MIT license texts do not appear to need to be
carried forward solely because of those references.

One separate Khronos notice item remains. `layer/include/depthxr/openxr_loader_api_layer.h` does
not vendor `openxr_loader_negotiation.h` verbatim, but it manually redeclares the same OpenXR
loader/API-layer ABI structs, constants, field order, and member names from the Khronos loader
negotiation header. The local `OpenXR.Loader.1.1.58.nupkg` also includes Khronos headers and
declares `Apache-2.0 OR MIT`. Include the Khronos OpenXR SDK/loader notice in the third-party
notices overhaul.

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
