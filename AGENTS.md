# VectorXR development

## Internal artifacts

- Keep all R&D artifacts, research, planning, internal code reviews, and validation/launch working notes in the ignored `notes/` directory. Do not commit or publish them under `docs/` or elsewhere in the Git tree.
- Reserve tracked `docs/` for public product and contributor documentation. Public release notes belong in `release/notes/`.

## Build and install for testing

- Always build and install/register the latest **Release** layer DLL when working on VectorXR, before handing changes back for testing. A Debug-only build is insufficient: headset testing must run the latest changes.
- Use `scripts/Build-And-Install-Layer.ps1 -Configuration Release` for the build and installation workflow, and run the relevant checks.
- Verify that the enabled OpenXR manifest resolves to the newly built Release DLL. If it already points to the workspace Release output, rebuilding that DLL updates the active installation; verify the registration instead of unnecessarily reinstalling it.
- Remind the user to relaunch the VR application to load the rebuilt DLL. If building or installing is blocked, report that explicitly; do not imply the changes are ready for headset testing.
