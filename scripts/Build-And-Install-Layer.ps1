[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$SkipTests,
    [switch]$SkipFresh,
    [switch]$CleanScratch,
    # Tests run side by side (Build-Layer.ps1's -TestJobs; 0: its default).
    [ValidateRange(0, 64)]
    [int]$TestJobs = 0,
    # Signs the built layer locally and installs that signed copy instead
    # (Sign-LocalLayer.ps1): for testing in games with anti-cheat only.
    [switch]$Sign,
    [string]$SignNote = ""
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

if ($TestJobs -gt 0) {
    $buildArgs.TestJobs = $TestJobs
}

& $buildScript @buildArgs
if ($Sign) {
    & (Join-Path $PSScriptRoot "Sign-LocalLayer.ps1") -BuildDir $BuildDir -Configuration $Configuration -Install -Note $SignNote
} else {
    & $installScript -BuildDir $BuildDir -Configuration $Configuration
}
