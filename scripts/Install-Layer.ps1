[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [string]$ManifestPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repoRoot $BuildDir
    $ManifestPath = Join-Path $ManifestPath "layer"
    $ManifestPath = Join-Path $ManifestPath $Configuration
    $ManifestPath = Join-Path $ManifestPath "XR_APILAYER_NOVENDOR_depthxr.json"
}

$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$RegistryPath = "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"

if (-not (Test-Path $ManifestPath)) {
    throw "Manifest not found at '$ManifestPath'. Build the layer first or pass -ManifestPath explicitly."
}

Write-Host "Installing OpenXR layer manifest from $ManifestPath"

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
  & {
    if (-not (Test-Path '$RegistryPath')) {
      New-Item -Path '$RegistryPath' -Force | Out-Null
    }

    New-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -PropertyType DWord -Value 0 -Force | Out-Null
  }
"@
