<#
.SYNOPSIS
    Bumps the VectorXR app version everywhere it lives and prepends a patch-notes entry.

.DESCRIPTION
    Updates the source-of-truth version files (package.json, tauri.conf.json, Cargo.toml)
    via the shared list in _VersionFiles.ps1, inserts a new entry at the top of
    patchNotes.ts, syncs the derived lock files, and finally runs
    Assert-VersionMatchesTag.ps1 as a self-check.

.EXAMPLE
    ./scripts/Set-Version.ps1 -Version 0.11.2 `
        -Title "Layer startup diagnostics" `
        -Summary "Adds detailed startup logging across the OpenXR layer chain." `
        -Items "First change.","Second change."
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Summary,
    [Parameter(Mandatory = $true)][string[]]$Items,
    [string]$Date,
    [switch]$SkipLocks
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "_VersionFiles.ps1")

if ($Version.StartsWith("v")) { $Version = $Version.Substring(1) }
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version '$Version' must use MAJOR.MINOR.PATCH format, such as 0.11.1."
}
if (-not $Date) { $Date = (Get-Date -Format 'yyyy-MM-dd') }
if ($Date -notmatch '^\d{4}-\d{2}-\d{2}$') {
    throw "Date '$Date' must use YYYY-MM-DD format."
}

# --- IMPORTANT: quote-escaping gotcha -----------------------------------------
# patchNotes.ts uses SINGLE-QUOTED TypeScript string literals. Any apostrophe in a
# title / summary / item (e.g. "application's") would terminate the string literal
# and break the build if inserted raw. Escape backslashes first (so we don't double
# escape), then escape single quotes as \'. Do NOT remove this -- it is the reason
# patch-note entries with apostrophes can be passed safely on the command line.
# ------------------------------------------------------------------------------
function ConvertTo-TsSingleQuoted {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $escaped = $Text.Replace('\', '\\').Replace("'", "\'")
    return "'$escaped'"
}

function Add-PatchNoteEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Date,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string[]]$Items
    )

    $path = Get-PatchNotesPath
    $content = Read-FileRaw -Path $path

    if ($content -match ("version:\s*'" + [regex]::Escape($Version) + "'")) {
        throw "patchNotes.ts already contains an entry for version $Version."
    }

    # Preserve the file's existing newline style so the insertion produces a clean diff.
    $nl = if ($content -match "`r`n") { "`r`n" } else { "`n" }

    $anchor = "export const patchNotes: PatchNoteEntry[] = ["
    $anchorIndex = $content.IndexOf($anchor)
    if ($anchorIndex -lt 0) {
        throw "Could not find the patchNotes array opener in patchNotes.ts."
    }
    $insertPos = $content.IndexOf($nl, $anchorIndex) + $nl.Length

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("  {")
    $lines.Add("    version: $(ConvertTo-TsSingleQuoted $Version),")
    $lines.Add("    date: $(ConvertTo-TsSingleQuoted $Date),")
    $lines.Add("    title: $(ConvertTo-TsSingleQuoted $Title),")
    $lines.Add("    summary: $(ConvertTo-TsSingleQuoted $Summary),")
    $lines.Add("    items: [")
    foreach ($item in $Items) {
        $lines.Add("      $(ConvertTo-TsSingleQuoted $item),")
    }
    $lines.Add("    ],")
    $lines.Add("  },")

    $block = ($lines -join $nl) + $nl
    $updated = $content.Substring(0, $insertPos) + $block + $content.Substring($insertPos)
    Write-FileNoBom -Path $path -Content $updated
}

# Edits ONLY the vectorxr-app entry in a derived lock file, anchored on its package
# name so dependency versions are never touched. We deliberately edit rather than
# regenerate (npm install / cargo update), which would churn unrelated dependencies
# and produce a noisy release diff. Lock files are not part of the tag gate.
function Update-LockVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$NewVersion
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Warning "  skipped $Name (not found)"
        return
    }
    $content = Read-FileRaw -Path $Path
    $regex = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $regex.IsMatch($content)) {
        throw "Could not find the vectorxr-app version entry in '$Name'."
    }
    $updated = $regex.Replace($content, "`${1}$NewVersion`${2}", 1)
    Write-FileNoBom -Path $Path -Content $updated
    Write-Host "  $Name"
}

$repoRoot = Get-RepoRoot

Write-Host "Updating source version files to $Version ..."
foreach ($target in Get-VersionTargets) {
    $old = Read-TargetVersion -Target $target
    Set-TargetVersion -Target $target -NewVersion $Version
    Write-Host "  $($target.Name): $old -> $Version"
}

Write-Host "Inserting patch-notes entry for $Version ($Date) ..."
Add-PatchNoteEntry -Version $Version -Date $Date -Title $Title -Summary $Summary -Items $Items

if ($SkipLocks) {
    Write-Host "Skipping lock files (-SkipLocks)."
}
else {
    Write-Host "Syncing lock files ..."
    # Each pattern has two capture groups: (1) prefix through the opening quote,
    # (2) closing quote. The version value between them is replaced.
    Update-LockVersion -Name "app/src-tauri/Cargo.lock" `
        -Path (Join-Path $repoRoot "app/src-tauri/Cargo.lock") `
        -Pattern '(name = "vectorxr-app"\s+version = ")[^"]+(")' `
        -NewVersion $Version
    Update-LockVersion -Name "app/package-lock.json (root)" `
        -Path (Join-Path $repoRoot "app/package-lock.json") `
        -Pattern '(\A\{\s*"name":\s*"vectorxr-app",\s*"version":\s*")[^"]+(")' `
        -NewVersion $Version
    Update-LockVersion -Name "app/package-lock.json (packages root)" `
        -Path (Join-Path $repoRoot "app/package-lock.json") `
        -Pattern '("":\s*\{\s*"name":\s*"vectorxr-app",\s*"version":\s*")[^"]+(")' `
        -NewVersion $Version
}

Write-Host "Verifying ..."
& (Join-Path $PSScriptRoot "Assert-VersionMatchesTag.ps1") -TagName "v$Version"

Write-Host ""
Write-Host "Done. Review the diff, then commit and tag v$Version."
