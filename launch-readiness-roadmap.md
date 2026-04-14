From a 1.0-readiness angle, I’d treat the next sprint less like “add a few features” and more like “make VectorXR installable, understandable, recoverable, and diagnosable.” The core runtime work is already far enough along that packaging, safety rails, and user trust now matter a lot.

**My Recommended Next Sprint Themes**

1. **Define The 1.0 Contract**

   Decide what 1.0 means in plain terms:

   - Which modules are officially supported at launch: likely `Depth` and `Pivot`.
   - Which settings are stable and supported.
   - Which settings are “advanced / experimental.”
   - Which VR runtimes you intend to support first: SteamVR, Meta/Oculus, Varjo, etc.
   - Which games/apps you will test against before release.
   - What “disable VectorXR completely” means and how reliable that path must be.

   This matters because OpenXR layers sit in a sensitive part of the stack. A good 1.0 should feel boringly reversible: install it, configure it, disable it, uninstall it, and recover from weirdness.

2. **Application Registry Plus “Apps Seen”**

   Yes, I think the XR “apps seen” capability belongs in the next major slice.

   I’d split it into two related systems:

   - **Registered apps:** user-managed app entries used by profiles.
   - **Seen apps:** executables the layer has observed while active.

   The layer can record lightweight observations like:

   - executable name
   - first seen time
   - last seen time
   - number of launches
   - maybe runtime name if we can detect it safely
   - whether it matches a registered app

   Then the app can offer “Add to registry” from the seen list. That would make profile setup feel much less mysterious. Instead of asking users to know that DCS is `DCS.exe`, VectorXR can say: “I saw `DCS.exe`; do you want to create a profile target for it?”

   Important design details:

   - Keep this data local.
   - Add a “clear seen apps” action.
   - Do not make seen apps automatically become registered apps.
   - Store observations separately from the core runtime settings, or under a clearly app-owned/diagnostic part of config.
   - Avoid tracking paths at first unless there is a real need. Executable names are less invasive and already match your current registry model.

3. **Installer And Uninstaller**

   You already have dev scripts for registering and unregistering the layer. For 1.0, those need to become product-grade installer behavior.

   Since this is a Tauri app, the natural path is to use Tauri’s Windows bundling. Current Tauri docs say Windows apps are distributed as either `.msi` installers via WiX or `-setup.exe` installers via NSIS, and `tauri build` produces the bundle. For VectorXR, I’d lean **NSIS first** because Tauri exposes installer hooks that are well-suited for custom install/uninstall steps like OpenXR layer registration.

   The installer should handle:

   - installing the Tauri app
   - installing the OpenXR API layer DLL
   - installing the OpenXR layer manifest JSON
   - registering the manifest path in the OpenXR registry location
   - creating default config if missing
   - preserving user config on upgrade
   - removing layer registry entries on uninstall
   - optionally asking whether to remove user config/logs

   I would strongly consider **per-user install first** if it works for your layer registration path. Khronos documents both `HKLM` and `HKCU` OpenXR API layer registry locations on Windows. Using `HKCU` avoids requiring admin rights just to install a user-level OpenXR layer. If you need machine-wide registration, then expect elevation.

4. **Dependencies: What To Bundle**

   You generally do **not** ship Rust with the app. Rust is a build-time dependency. Users should receive compiled binaries.

   Same for Node/npm/Vite: those are build-time only.

   What you may need to package or account for:

   - `VectorXR.exe`, produced by Tauri
   - your OpenXR API layer DLL
   - your OpenXR layer manifest JSON
   - any C/C++ runtime dependency, unless you statically link the layer runtime
   - WebView2 runtime behavior for the Tauri app
   - default config/schema/example files, if useful
   - license notices for bundled dependencies

   Tauri’s Windows installer docs say WebView2 can be handled several ways: default bootstrapper download, embedded bootstrapper, offline installer, fixed runtime, or skip. For most modern Windows 10/11 users, WebView2 is often already present, but for a public installer I’d explicitly choose a mode instead of leaving it as a fuzzy accident. The offline/fixed options make the installer much larger but more self-contained.

   For the C++ layer, decide whether to:

   - statically link the MSVC runtime where appropriate, or
   - include/check/install the Visual C++ Redistributable.

   Tauri’s NSIS hooks can install dependencies like Visual C++ Redistributables, which is another reason NSIS looks attractive for VectorXR.

5. **OpenXR Layer Ordering**

   Short version: **we can help, but we probably cannot guarantee it globally.**

   Khronos’ OpenXR loader docs say implicit API layers are discovered automatically from the registry on Windows, and implicit layers intercept before explicit layers. Explicit layer order can be controlled by the app’s `enabledApiLayerNames` or by `XR_ENABLE_API_LAYERS`, where order matters. But for normal implicit layer registration, there is no clean product-level “put me last” priority field in your manifest.

   So the installer can guarantee:

   - VectorXR is registered.
   - VectorXR can be disabled by its `disable_environment` variable.
   - VectorXR’s manifest path points at the installed DLL.
   - VectorXR unregisters cleanly.

   The installer probably cannot guarantee:

   - VectorXR is bottom-most / closest to the runtime in every OpenXR call chain.
   - VectorXR appears after every other implicit layer.
   - Other tools will not reorder, disable, or conflict with it.
   - App-provided explicit layers will not sit below it.

   For 1.0, I’d make this a **diagnostics and guidance problem**, not an installer magic problem.

   Good launch-ready behavior would be:

   - Add an “OpenXR Layer Status” screen.
   - Show whether VectorXR is registered.
   - Show the manifest path and DLL path.
   - Show whether the DLL exists.
   - Show other discovered implicit layers from `HKCU` / `HKLM`.
   - Warn: “Layer ordering may affect behavior.”
   - Link or mention external layer-management tools where appropriate.
   - Provide a one-click “disable VectorXR layer” and “enable VectorXR layer” if registry permissions allow it.
   - Document that users may need a layer manager/OpenXR toolkit if they have multiple OpenXR tools installed.

   If you eventually want stronger control, one possible advanced path is a launcher mode that sets `XR_ENABLE_API_LAYERS` for a launched game. But that only helps for apps launched through VectorXR, and implicit layers still have their own behavior. I would not make that a 1.0 requirement.

6. **Recovery And Safety**

   This is big for public release. Users should never feel trapped.

   Add or polish:

   - “Disable suite” toggle that the layer reliably honors.
   - “Unregister OpenXR layer” action in the app.
   - “Open logs folder” action.
   - “Open config folder” action.
   - “Reset config to defaults.”
   - Clear error states when config is malformed.
   - Safe fallback if config cannot be parsed.
   - Startup self-check: config path, layer registration, manifest validity, DLL path, version mismatch.

   The app should be able to answer: “Is VectorXR actually installed and active?” That will save you a mountain of support pain.

7. **Packaging Pipeline**

   Before polishing more UI, I’d want a repeatable release process:

   - `npm ci`
   - frontend build
   - Tauri release build
   - native layer release build
   - copy layer DLL + manifest into Tauri resources or installer payload
   - produce installer
   - install on a clean machine/VM
   - launch app
   - run a known OpenXR title
   - uninstall
   - confirm registry cleanup

   Eventually:

   - code signing certificate
   - signed EXE/DLL/installer
   - GitHub release artifacts
   - checksums
   - changelog
   - version stamping across app, layer DLL, manifest, and config schema

8. **UI Changes Worth Prioritizing**

   I’d make UI polish serve 1.0 readiness, not just aesthetics.

   Highest-value UI work:

   - Home page becomes a status dashboard.
   - App registry editor lands.
   - “Seen apps” list lands.
   - OpenXR layer status lands.
   - Save/reload/import flows become hard to misuse.
   - Logs/config buttons are easy to find.
   - Module names become product-facing: `Depth`, `Pivot`.
   - Warnings become plain-language and actionable.

   The UI should answer four questions fast:

   - Is VectorXR enabled?
   - Is the OpenXR layer installed?
   - Which apps will it affect?
   - What do I do if something breaks?

**My Suggested Sprint Order**

1. App registry plus “apps seen” foundation.
2. OpenXR layer install/status diagnostics in the app.
3. Installer/uninstaller spike using Tauri NSIS.
4. Dependency strategy: WebView2 mode, VC runtime/static linking, bundled layer files.
5. Clean recovery actions: enable/disable/unregister/reset/open logs.
6. UI polish around Home, app registry, warnings, and save flow.
7. Clean-machine install/uninstall QA pass.

**Bottom Line**

For 1.0, I would not chase perfect OpenXR layer ordering. I’d make VectorXR install cleanly, uninstall cleanly, explain its layer status clearly, and give users good tools when other layers are present. The “apps seen” feature is absolutely worth doing because it turns profile setup from “know internal executable names” into “pick the VR app VectorXR already noticed,” which is much more launch-ready.

Sources I checked for the platform bits: Khronos’ [OpenXR Loader Design and Operation](https://registry.khronos.org/OpenXR/specs/1.1/loader.html) and Tauri’s [Windows installer documentation](https://v2.tauri.app/distribute/windows-installer/).