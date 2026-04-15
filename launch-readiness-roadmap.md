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

COMPLETED

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

OUT OF SCOPE

6. **Recovery And Safety**

COMPLETED

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

COMPLETED

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