[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$SkipTests,
    [switch]$SkipFresh,
    [switch]$CleanScratch,
    # Tests CTest runs side by side; a test that touches a graphics device still runs alone (RUN_SERIAL in
    # layer/CMakeLists.txt). The default, three quarters of the logical processors (up to 8), leaves the presenter
    # tests' wall-clock pacing room: 3 on a 4-processor CI runner (on four of this desktop's processors 3 passed
    # and 4 made the presenter tests late). 1 runs them one after another.
    [ValidateRange(1, 64)]
    [int]$TestJobs = [Math]::Max(1, [Math]::Min([Math]::Floor([Environment]::ProcessorCount * 3 / 4), 8))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-CMakeExecutable {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCommand) {
        return $cmakeCommand.Source
    }

    $knownLocations = @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\CMake\bin\cmake.exe"
    )

    foreach ($candidate in $knownLocations) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw "CMake was not found. Install CMake or add it to PATH."
}

function Resolve-CtestExecutable {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CmakeExecutable
    )

    $ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
    if ($ctestCommand) {
        return $ctestCommand.Source
    }

    $cmakeBinDir = Split-Path -Parent $CmakeExecutable
    $ctestPath = Join-Path $cmakeBinDir "ctest.exe"
    if (Test-Path $ctestPath) {
        return $ctestPath
    }

    throw "ctest.exe was not found alongside CMake."
}

function Get-VisualStudioGenerator {
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe was not found. Install Visual Studio Build Tools or Visual Studio Community with Desktop C++ support."
    }

    $vsInfoJson = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
    if (-not $vsInfoJson) {
        throw "No Visual Studio instance with MSVC C++ tools was found."
    }

    $vsInfo = $vsInfoJson | ConvertFrom-Json
    if ($vsInfo -is [array]) {
        $vsInfo = $vsInfo[0]
    }

    $productLineVersion = [int]$vsInfo.catalog.productLineVersion
    $majorVersion = [int]$vsInfo.installationVersion.Split(".")[0]
    $yearByMajor = @{
        18 = "2026"
        17 = "2022"
        16 = "2019"
        15 = "2017"
    }

    if (-not $yearByMajor.ContainsKey($majorVersion)) {
        if ($productLineVersion -in @(2026, 2022, 2019, 2017)) {
            return "Visual Studio $majorVersion $productLineVersion"
        }

        throw "Unsupported Visual Studio major version '$majorVersion' with product line '$productLineVersion'."
    }

    return "Visual Studio $majorVersion $($yearByMajor[$majorVersion])"
}

function Remove-ScratchBuildDirectories {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    $scratchDirectories = @(
        "build-cmake-check",
        "build-vendored",
        "build-wincheck"
    )

    foreach ($directory in $scratchDirectories) {
        $path = Join-Path $RepoRoot $directory
        if (Test-Path $path) {
            Remove-Item -LiteralPath $path -Recurse -Force
            Write-Host "Removed scratch build directory $directory"
        }
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$buildPath = Join-Path $repoRoot $BuildDir
$cmake = Resolve-CMakeExecutable
$ctest = Resolve-CtestExecutable -CmakeExecutable $cmake
$generator = Get-VisualStudioGenerator

if ($CleanScratch) {
    Remove-ScratchBuildDirectories -RepoRoot $repoRoot
}

$configureArgs = @(
    "-S", $repoRoot,
    "-B", $buildPath,
    "-G", $generator,
    "-A", "x64",
    "-DDEPTHXR_BUILD_LAYER=ON",
    "-DDEPTHXR_BUILD_TESTS=ON"
)

$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) {
    $vcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
}

if ($vcpkgRoot) {
    $vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $vcpkgToolchain) {
        $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain"
        Write-Host "Using vcpkg toolchain: $vcpkgToolchain"
    }
}

if (-not $SkipFresh) {
    $configureArgs += "--fresh"
}

Write-Host "Using CMake: $cmake"
Write-Host "Using generator: $generator"
Write-Host "Build directory: $buildPath"

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed (exit code $LASTEXITCODE)."
}

& $cmake --build $buildPath --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed (exit code $LASTEXITCODE)."
}

if (-not $SkipTests) {
    & $ctest --test-dir $buildPath -C $Configuration --output-on-failure --parallel $TestJobs
    if ($LASTEXITCODE -ne 0) {
        throw "Layer tests failed (exit code $LASTEXITCODE)."
    }
}
