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

$buildScript = Join-Path $PSScriptRoot "Build-Layer.ps1"
$installScript = Join-Path $PSScriptRoot "Install-Layer.ps1"

$buildArgs = @{
    BuildDir = $BuildDir
    Configuration = $Configuration
}

if ($SkipTests) {
    $buildArgs.SkipTests = $true
}

if ($SkipFresh) {
    $buildArgs.SkipFresh = $true
}

if ($CleanScratch) {
    $buildArgs.CleanScratch = $true
}

& $buildScript @buildArgs
& $installScript -BuildDir $BuildDir -Configuration $Configuration
