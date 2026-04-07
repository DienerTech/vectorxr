$ManifestPath = Join-Path $PSScriptRoot "..\\layer\\manifest\\XR_APILAYER_NOVENDOR_depthxr.json"
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$RegistryPath = "HKLM:\\Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit"

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
  & {
    if (-not (Test-Path '$RegistryPath')) {
      New-Item -Path '$RegistryPath' -Force | Out-Null
    }

    New-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -PropertyType DWord -Value 0 -Force | Out-Null
  }
"@
