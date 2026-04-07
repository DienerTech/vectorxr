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

Write-Host "Removing OpenXR layer manifest registration for $ManifestPath"

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
  & {
    if (Test-Path '$RegistryPath') {
      Remove-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -Force -ErrorAction SilentlyContinue | Out-Null
    }
  }
"@
