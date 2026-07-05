[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$sourceDir = Join-Path $repoRoot $BuildDir
$sourceDir = Join-Path $sourceDir "layer"
$sourceDir = Join-Path $sourceDir $Configuration

$layerDll = Join-Path $sourceDir "XR_APILAYER_DIENERTECH_VECTORXR.dll"
$layerManifest = Join-Path $sourceDir "XR_APILAYER_DIENERTECH_VECTORXR.json"
$layerSounds = Join-Path $sourceDir "sounds"

if (-not (Test-Path $layerDll)) {
    throw "Layer DLL not found at '$layerDll'. Run scripts\Build-Layer.ps1 first."
}

if (-not (Test-Path $layerManifest)) {
    throw "Layer manifest not found at '$layerManifest'. Run scripts\Build-Layer.ps1 first."
}

if (-not (Test-Path $layerSounds)) {
    throw "Layer default sounds not found at '$layerSounds'. Run scripts\Build-Layer.ps1 first."
}

$payloadDir = Join-Path $repoRoot "app"
$payloadDir = Join-Path $payloadDir "src-tauri"
$payloadDir = Join-Path $payloadDir "resources"
$payloadDir = Join-Path $payloadDir "vectorxr-layer"

New-Item -ItemType Directory -Path $payloadDir -Force | Out-Null

Copy-Item -LiteralPath $layerDll -Destination (Join-Path $payloadDir "XR_APILAYER_DIENERTECH_VECTORXR.dll") -Force
Copy-Item -LiteralPath $layerManifest -Destination (Join-Path $payloadDir "XR_APILAYER_DIENERTECH_VECTORXR.json") -Force

$payloadSoundsDir = Join-Path $payloadDir "sounds"
New-Item -ItemType Directory -Path $payloadSoundsDir -Force | Out-Null
foreach ($soundFile in @("activate.wav", "deactivate.wav", "origin-set.wav", "origin-release.wav")) {
    Copy-Item -LiteralPath (Join-Path $layerSounds $soundFile) -Destination (Join-Path $payloadSoundsDir $soundFile) -Force
}

Write-Host "Staged VectorXR layer payload in $payloadDir"
