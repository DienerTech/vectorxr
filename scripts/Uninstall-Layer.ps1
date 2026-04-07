$ManifestPath = Join-Path $PSScriptRoot "..\\layer\\manifest\\XR_APILAYER_NOVENDOR_depthxr.json"
$ManifestPath = [System.IO.Path]::GetFullPath($ManifestPath)
$RegistryPath = "HKLM:\\Software\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit"

Start-Process -FilePath powershell.exe -Verb RunAs -Wait -ArgumentList @"
  & {
    if (Test-Path '$RegistryPath') {
      Remove-ItemProperty -Path '$RegistryPath' -Name '$ManifestPath' -Force -ErrorAction SilentlyContinue | Out-Null
    }
  }
"@
