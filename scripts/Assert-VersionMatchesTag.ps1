[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TagName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
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

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = Get-Content -LiteralPath $Path -Raw
    return $content | ConvertFrom-Json
}

function Read-CargoVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = Get-Content -LiteralPath $Path -Raw
    $match = [regex]::Match($content, '(?m)^version\s*=\s*"([^"]+)"')

    if (-not $match.Success) {
        throw "Could not find package version in '$Path'."
    }

    return $match.Groups[1].Value
}

function Read-LatestPatchVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $content = Get-Content -LiteralPath $Path -Raw
    $match = [regex]::Match($content, "version:\s*'([^']+)'")

    if (-not $match.Success) {
        throw "Could not find latest patch note version in '$Path'."
    }

    return $match.Groups[1].Value
}

$versions = @(
    @{
        Name = "app/package.json"
        Version = (Read-JsonFile -Path (Join-Path $repoRoot "app/package.json")).version
    },
    @{
        Name = "app/src-tauri/tauri.conf.json"
        Version = (Read-JsonFile -Path (Join-Path $repoRoot "app/src-tauri/tauri.conf.json")).version
    },
    @{
        Name = "app/src-tauri/Cargo.toml"
        Version = Read-CargoVersion -Path (Join-Path $repoRoot "app/src-tauri/Cargo.toml")
    },
    @{
        Name = "app/src/lib/patchNotes.ts"
        Version = Read-LatestPatchVersion -Path (Join-Path $repoRoot "app/src/lib/patchNotes.ts")
    }
)

$mismatches = @(
    $versions | Where-Object { $_.Version -ne $expectedVersion }
)

if ($mismatches.Count -gt 0) {
    $details = $mismatches | ForEach-Object { "- $($_.Name): $($_.Version), expected $expectedVersion" }
    throw "Version mismatch for tag '$TagName':`n$($details -join "`n")"
}

Write-Host "Version check passed for $TagName."
