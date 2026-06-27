[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TagName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Canonical version-location list is single-sourced in _VersionFiles.ps1 and shared
# with Set-Version.ps1, so this check and the bump script can never disagree.
. (Join-Path $PSScriptRoot "_VersionFiles.ps1")

$expectedVersion = $TagName.Trim()

if ($expectedVersion.StartsWith("refs/tags/")) {
    $expectedVersion = $expectedVersion.Substring("refs/tags/".Length)
}

if ($expectedVersion.StartsWith("v")) {
    $expectedVersion = $expectedVersion.Substring(1)
}

if ($expectedVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "Tag '$TagName' must use the vMAJOR.MINOR.PATCH format, such as v0.7.0."
}

$versions = @()
foreach ($target in Get-VersionTargets) {
    $versions += [pscustomobject]@{
        Name    = $target.Name
        Version = (Read-TargetVersion -Target $target)
    }
}
$versions += [pscustomobject]@{
    Name    = "app/src/lib/patchNotes.ts"
    Version = (Read-LatestPatchVersion)
}

$mismatches = @(
    $versions | Where-Object { $_.Version -ne $expectedVersion }
)

if ($mismatches.Count -gt 0) {
    $details = $mismatches | ForEach-Object { "- $($_.Name): $($_.Version), expected $expectedVersion" }
    throw "Version mismatch for tag '$TagName':`n$($details -join "`n")"
}

Write-Host "Version check passed for $TagName."
