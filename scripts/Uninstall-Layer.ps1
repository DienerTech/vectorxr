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

Write-Host "Removing OpenXR layer manifest registration for $ManifestPath"

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
  & {
    if (Test-Path '$RegistryPath') {
      Remove-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -Force -ErrorAction SilentlyContinue | Out-Null
      Remove-ItemProperty -Path '$RegistryPath' -Name '$LegacyManifestPath' -Force -ErrorAction SilentlyContinue | Out-Null

      # Install-Layer.ps1 disables every other VectorXR registration (two
      # enabled copies crash OpenXR apps). Re-enable one that still exists so
      # removing a dev build does not leave VectorXR off entirely.
      `$key = Get-Item -Path '$RegistryPath'
      `$remaining = @(`$key.Property | Where-Object {
        [System.IO.Path]::GetFileName(`$_) -ieq 'XR_APILAYER_DIENERTECH_VECTORXR.json' -and
        (Test-Path -LiteralPath `$_)
      })
      `$enabled = @(`$remaining | Where-Object { `$key.GetValue(`$_, `$null) -eq 0 })
      if (`$remaining.Count -gt 0 -and `$enabled.Count -eq 0) {
        New-ItemProperty -Path '$RegistryPath' -Name `$remaining[0] -PropertyType DWord -Value 0 -Force | Out-Null
      }
    }
  }
"@
