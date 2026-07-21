# Shared, single-source definition of where the VectorXR app version string lives.
#
# Dot-sourced by both Set-Version.ps1 (the writer) and Assert-VersionMatchesTag.ps1
# (the reader) so the canonical list of version locations is defined exactly ONCE.
# If a new version-bearing file is added, add it here and both scripts pick it up.
#
# Scope note: this lists the "gated" version files only -- the three source-of-truth
# files plus patchNotes.json, which the release tag must match. The lock files
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
    return (Join-Path (Get-RepoRoot) "app/src/lib/patchNotes.json")
}

function Get-ReleaseNotesPath {
    param([Parameter(Mandatory = $true)][string]$Version)
    return (Join-Path (Get-RepoRoot) "release/notes/v$Version.md")
}

function Get-ReleaseManagedPaths {
    param([Parameter(Mandatory = $true)][string]$Version)

    $root = Get-RepoRoot
    $paths = @((Get-VersionTargets | ForEach-Object { $_.Path }))
    $paths += Get-PatchNotesPath
    $paths += Join-Path $root "app/package-lock.json"
    $paths += Join-Path $root "app/src-tauri/Cargo.lock"
    $paths += Get-ReleaseNotesPath -Version $Version
    return $paths
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

function Assert-PatchNoteItems {
    param(
        [Parameter(Mandatory = $true)][object[]]$Items,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if ($Items.Count -eq 0) {
        throw "$Path must contain at least one item."
    }
    for ($index = 0; $index -lt $Items.Count; $index++) {
        $item = $Items[$index]
        $itemPath = "$Path[$index]"
        if ($item -is [string]) {
            if ([string]::IsNullOrWhiteSpace([string]$item)) {
                throw "$itemPath must not be empty."
            }
            continue
        }
        if ($null -eq $item) {
            throw "$itemPath must be a string or nested list item."
        }
        $propertyNames = @($item.PSObject.Properties.Name)
        if ($propertyNames -notcontains "html" -or
            [string]::IsNullOrWhiteSpace([string]$item.html)) {
            throw "$itemPath.html must be a non-empty string."
        }
        if ($propertyNames -contains "items") {
            Assert-PatchNoteItems -Items @($item.items) -Path "$itemPath.items"
        }
    }
}

function Read-PatchNotes {
    $path = Get-PatchNotesPath
    $content = Read-FileRaw -Path $path
    if (-not $content.TrimStart().StartsWith("[")) {
        throw "Patch notes must be a JSON array: $path"
    }
    try {
        $parsed = $content | ConvertFrom-Json
        $entries = @($parsed | ForEach-Object { $_ })
    }
    catch {
        throw "Could not parse patch notes JSON at '$path': $($_.Exception.Message)"
    }
    if ($entries.Count -eq 0) {
        throw "Patch notes JSON must contain at least one entry."
    }
    $seenVersions = @{}
    for ($index = 0; $index -lt $entries.Count; $index++) {
        $entry = $entries[$index]
        if (-not $entry.version -or -not $entry.date -or -not $entry.title -or
            -not $entry.summary -or @($entry.items).Count -eq 0) {
            throw "Each patch-notes entry must define version, date, title, summary, and items."
        }
        if (([string]$entry.date) -notmatch '^\d{4}-\d{2}-\d{2}$') {
            throw "Patch-notes entry $index must use a YYYY-MM-DD date."
        }
        if ($seenVersions.ContainsKey([string]$entry.version)) {
            throw "Patch notes contain duplicate version $($entry.version)."
        }
        $seenVersions[[string]$entry.version] = $true
        Assert-PatchNoteItems -Items @($entry.items) -Path "entries[$index].items"
    }
    return $entries
}

function Read-LatestPatchVersion {
    $entries = @(Read-PatchNotes)
    return [string]$entries[0].version
}
