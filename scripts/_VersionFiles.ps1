# Shared, single-source definition of where the VectorXR app version string lives.
#
# Dot-sourced by both Set-Version.ps1 (the writer) and Assert-VersionMatchesTag.ps1
# (the reader) so the canonical list of version locations is defined exactly ONCE.
# If a new version-bearing file is added, add it here and both scripts pick it up.
#
# Scope note: this lists the "gated" version files only -- the three source-of-truth
# files plus patchNotes.ts, which the release tag must match. The lock files
# (app/package-lock.json, app/src-tauri/Cargo.lock) are derived artifacts that are
# NOT part of the tag gate; Set-Version.ps1 keeps them in sync separately.
# CMakeLists.txt's project(VERSION ...) is the C++ project version and does not
# track the app version, so it is intentionally not managed here.

Set-StrictMode -Version Latest

function Get-RepoRoot {
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

# Reads a file as UTF-8 (BOM-aware). Windows PowerShell 5.1's `Get-Content -Raw`
# without -Encoding decodes using the ANSI codepage, which mojibakes non-ASCII
# characters (e.g. a typographic apostrophe in an existing patch note). .NET's
# ReadAllText defaults to UTF-8 and handles both BOM and no-BOM files, so read and
# write stay symmetric and round-trip safe.
function Read-FileRaw {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.File]::ReadAllText($Path)
}

# Writes UTF-8 WITHOUT a BOM. Windows PowerShell 5.1's `Set-Content -Encoding utf8`
# emits a BOM, which would corrupt JSON tooling and create noisy diffs, so we go
# through .NET directly.
function Write-FileNoBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )
    [System.IO.File]::WriteAllText($Path, $Content, (New-Object System.Text.UTF8Encoding($false)))
}

# The version-bearing source files asserted against the release tag. Each target
# carries a Kind that selects the regex used to read/write its version string:
#   json  -> top-level   "version": "x.y.z"
#   cargo -> [package]    version = "x.y.z"
function Get-VersionTargets {
    $root = Get-RepoRoot
    return @(
        [pscustomobject]@{ Name = "app/package.json";              Path = (Join-Path $root "app/package.json");              Kind = "json" }
        [pscustomobject]@{ Name = "app/src-tauri/tauri.conf.json"; Path = (Join-Path $root "app/src-tauri/tauri.conf.json"); Kind = "json" }
        [pscustomobject]@{ Name = "app/src-tauri/Cargo.toml";      Path = (Join-Path $root "app/src-tauri/Cargo.toml");      Kind = "cargo" }
    )
}

function Get-PatchNotesPath {
    return (Join-Path (Get-RepoRoot) "app/src/lib/patchNotes.ts")
}

# Three capture groups: (1) prefix up to and including the opening quote,
# (2) the version value, (3) the closing quote. Only the FIRST match is ever used,
# so nested / dependency versions are never touched.
function Get-VersionRegex {
    param([Parameter(Mandatory = $true)][string]$Kind)
    switch ($Kind) {
        "json"  { return '("version"\s*:\s*")([^"]+)(")' }
        "cargo" { return '(?m)(^version\s*=\s*")([^"]+)(")' }
        default { throw "Unknown version target kind '$Kind'." }
    }
}

function Read-TargetVersion {
    param([Parameter(Mandatory = $true)][object]$Target)
    $content = Read-FileRaw -Path $Target.Path
    $match = [regex]::Match($content, (Get-VersionRegex -Kind $Target.Kind))
    if (-not $match.Success) {
        throw "Could not find a version string in '$($Target.Name)'."
    }
    return $match.Groups[2].Value
}

function Set-TargetVersion {
    param(
        [Parameter(Mandatory = $true)][object]$Target,
        [Parameter(Mandatory = $true)][string]$NewVersion
    )
    $content = Read-FileRaw -Path $Target.Path
    $regex = [regex]::new((Get-VersionRegex -Kind $Target.Kind))
    if (-not $regex.IsMatch($content)) {
        throw "Could not find a version string to update in '$($Target.Name)'."
    }
    # Replace only the first match so nested / dependency versions are left alone.
    $updated = $regex.Replace($content, "`${1}$NewVersion`${3}", 1)
    Write-FileNoBom -Path $Target.Path -Content $updated
}

function Read-LatestPatchVersion {
    $content = Read-FileRaw -Path (Get-PatchNotesPath)
    $match = [regex]::Match($content, "version:\s*'([^']+)'")
    if (-not $match.Success) {
        throw "Could not find latest patch note version in patchNotes.ts."
    }
    return $match.Groups[1].Value
}
