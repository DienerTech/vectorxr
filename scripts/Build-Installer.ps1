[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$SkipTests,
    [switch]$SkipFresh,
    [switch]$CleanScratch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

$buildLayerArgs = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
}

if ($SkipTests) {
    $buildLayerArgs.SkipTests = $true
}

if ($SkipFresh) {
    $buildLayerArgs.SkipFresh = $true
}

if ($CleanScratch) {
    $buildLayerArgs.CleanScratch = $true
}

& (Join-Path $PSScriptRoot "Build-Layer.ps1") @buildLayerArgs
& (Join-Path $PSScriptRoot "Stage-ReleasePayload.ps1") -BuildDir $BuildDir -Configuration $Configuration

Push-Location (Join-Path $repoRoot "app")
try {
    npm run tauri:build
}
finally {
    Pop-Location
}
