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
$LegacyManifestPath = Join-Path $repoRoot $BuildDir
$LegacyManifestPath = Join-Path $LegacyManifestPath "layer"
$LegacyManifestPath = Join-Path $LegacyManifestPath $Configuration
$LegacyManifestPath = Join-Path $LegacyManifestPath "XR_APILAYER_NOVENDOR_depthxr.json"
$LegacyManifestPath = [System.IO.Path]::GetFullPath($LegacyManifestPath)

if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repoRoot $BuildDir
    $ManifestPath = Join-Path $ManifestPath "layer"
    $ManifestPath = Join-Path $ManifestPath $Configuration
    $ManifestPath = Join-Path $ManifestPath "XR_APILAYER_DIENERTECH_VECTORXR.json"
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

    # Only one manifest for a given API layer name may be enabled. A normal
    # VectorXR installation can coexist on disk with this development build,
    # but loading both DLLs into the same OpenXR process crashes during the
    # instance dispatch-chain setup.
    `$existingRegistrations = (Get-Item -Path '$RegistryPath').Property
    foreach (`$registration in `$existingRegistrations) {
      if (`$registration -ne '$ManifestPath' -and
          [System.IO.Path]::GetFileName(`$registration) -ieq 'XR_APILAYER_DIENERTECH_VECTORXR.json') {
        New-ItemProperty -Path '$RegistryPath' -Name `$registration -PropertyType DWord -Value 1 -Force | Out-Null
      }
    }

    New-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -PropertyType DWord -Value 0 -Force | Out-Null
    Remove-ItemProperty -Path '$RegistryPath' -Name '$LegacyManifestPath' -Force -ErrorAction SilentlyContinue | Out-Null
  }
"@
