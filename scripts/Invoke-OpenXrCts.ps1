[CmdletBinding()]
param(
    [ValidateSet("Pivot", "QuadViews", "Composition", "Automated")]
    [string]$Suite = "Pivot",

    [ValidateSet("D3D11", "D3D12", "Vulkan", "Vulkan2", "OpenGL")]
    [string]$GraphicsPlugin = "D3D11",

    [string]$RunLabel = "manual",
    [string]$CtsCacheDir,
    [string]$ResultsDir,
    [string]$CompareTo,
    [string[]]$AdditionalArguments = @(),
    [switch]$DownloadOnly,
    [switch]$Plan
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ($env:OS -ne "Windows_NT" -or -not [Environment]::Is64BitOperatingSystem) {
    throw "The VectorXR CTS harness requires 64-bit Windows."
}

# Use an approved Khronos release, not the moving development branch. Updating
# these values is an intentional review point because CTS behavior can change.
$ctsVersion = "1.1.61.0"
$ctsReleaseTag = "openxr-cts-$ctsVersion"
$ctsArchiveName = "openxr-cts-$ctsVersion-x64.zip"
$ctsArchiveSha256 = "b3afcab662f6396a7e0e5f3fe476f4622c603a108ef5bd66d1d4ac14519fc1d8"
$ctsDownloadUrl = "https://github.com/KhronosGroup/OpenXR-CTS/releases/download/$ctsReleaseTag/$ctsArchiveName"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $CtsCacheDir) {
    $CtsCacheDir = Join-Path $repoRoot "build-openxr-cts"
}
if (-not $ResultsDir) {
    $ResultsDir = Join-Path $repoRoot "logs\openxr-cts"
}
$CtsCacheDir = [System.IO.Path]::GetFullPath($CtsCacheDir)
$ResultsDir = [System.IO.Path]::GetFullPath($ResultsDir)

$suiteSpecs = @{
    # These exercise the contract between xrLocateViews, xrLocateSpace, spaces
    # attached to compositor layers, and poses submitted through xrEndFrame.
    Pivot = "xrLocateSpace_xrLocateViews,XrCompositionLayerQuad,QuadPoses,QuadHands,QuadProjectionQuad,ProjectionQuadProjection,SpaceOffsets"

    # The extension form and the OpenXR 1.1 promoted names are both selected.
    QuadViews = "XR_VARJO_quad_views*,StereoWithFoveatedInset*"

    # Official CTS tag selection for all human-evaluated composition tests.
    Composition = "[composition][interactive]"

    # Official non-interactive selection. This is deliberately broad because
    # API layers can affect behavior outside the feature they intend to change.
    Automated = "exclude:[interactive]"
}

function Assert-PathWithinDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Directory
    )

    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $resolvedDirectory = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    if (-not $resolvedPath.StartsWith($resolvedDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify '$resolvedPath' because it is outside '$resolvedDirectory'."
    }
}

function Install-PinnedCts {
    $archivePath = Join-Path $CtsCacheDir $ctsArchiveName
    $extractRoot = Join-Path $CtsCacheDir $ctsVersion
    $expectedCli = Join-Path $extractRoot "openxr-cts-x64\conformance_cli.exe"
    $installMarker = Join-Path $extractRoot ".vectorxr-cts.json"

    if ((Test-Path -LiteralPath $expectedCli -PathType Leaf) -and
        (Test-Path -LiteralPath $installMarker -PathType Leaf)) {
        try {
            $marker = Get-Content -LiteralPath $installMarker -Raw | ConvertFrom-Json
            if ($marker.version -eq $ctsVersion -and $marker.archiveSha256 -eq $ctsArchiveSha256) {
                return $expectedCli
            }
        }
        catch {
            # Reinstall from the verified archive below.
        }
    }

    New-Item -ItemType Directory -Force -Path $CtsCacheDir | Out-Null

    $archiveValid = $false
    if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
        $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        $archiveValid = $actualHash -eq $ctsArchiveSha256
        if (-not $archiveValid) {
            Assert-PathWithinDirectory -Path $archivePath -Directory $CtsCacheDir
            Remove-Item -LiteralPath $archivePath -Force
        }
    }

    if (-not $archiveValid) {
        Write-Host "Downloading Khronos OpenXR CTS $ctsVersion (x64)..."
        Invoke-WebRequest -Headers @{ "User-Agent" = "VectorXR-CTS-Harness" } -Uri $ctsDownloadUrl -OutFile $archivePath
        $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $ctsArchiveSha256) {
            Assert-PathWithinDirectory -Path $archivePath -Directory $CtsCacheDir
            Remove-Item -LiteralPath $archivePath -Force
            throw "CTS archive hash mismatch. Expected $ctsArchiveSha256, received $actualHash."
        }
    }

    if (Test-Path -LiteralPath $extractRoot) {
        Assert-PathWithinDirectory -Path $extractRoot -Directory $CtsCacheDir
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }

    Write-Host "Extracting CTS to $extractRoot"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot -Force
    if (-not (Test-Path -LiteralPath $expectedCli -PathType Leaf)) {
        throw "The verified CTS archive did not contain the expected executable: $expectedCli"
    }

    [ordered]@{
        version = $ctsVersion
        releaseTag = $ctsReleaseTag
        archiveSha256 = $ctsArchiveSha256
        installedAt = (Get-Date).ToString("o")
    } | ConvertTo-Json | Set-Content -LiteralPath $installMarker -Encoding UTF8

    return $expectedCli
}

function Get-ActiveRuntimeMetadata {
    $registryCandidates = @(
        "Registry::HKEY_CURRENT_USER\SOFTWARE\Khronos\OpenXR\1",
        "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1"
    )

    foreach ($registryPath in $registryCandidates) {
        try {
            $runtimePath = (Get-ItemProperty -LiteralPath $registryPath -Name "ActiveRuntime" -ErrorAction Stop).ActiveRuntime
            if ($runtimePath) {
                $runtimeJson = $null
                if (Test-Path -LiteralPath $runtimePath -PathType Leaf) {
                    try {
                        $runtimeJson = Get-Content -LiteralPath $runtimePath -Raw | ConvertFrom-Json
                    }
                    catch {
                        $runtimeJson = $null
                    }
                }
                return [ordered]@{
                    registryPath = $registryPath
                    manifestPath = $runtimePath
                    manifest = $runtimeJson
                }
            }
        }
        catch {
            # Try the next standard registry location.
        }
    }

    return [ordered]@{
        registryPath = $null
        manifestPath = $null
        manifest = $null
    }
}

function Get-VectorXrLayerRegistrations {
    $registrations = @()
    $registryCandidates = @(
        "Registry::HKEY_CURRENT_USER\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit",
        "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
    )

    foreach ($registryPath in $registryCandidates) {
        if (-not (Test-Path -LiteralPath $registryPath)) {
            continue
        }

        $item = Get-Item -LiteralPath $registryPath
        foreach ($valueName in $item.GetValueNames()) {
            if ($valueName -notmatch "VECTORXR|DIENERTECH") {
                continue
            }
            $registrations += [ordered]@{
                registryPath = $registryPath
                manifestPath = $valueName
                registryValue = $item.GetValue($valueName)
                enabled = $item.GetValue($valueName) -eq 0
            }
        }
    }

    return @($registrations)
}

function Get-GitMetadata {
    $commit = $null
    $branch = $null
    try {
        $commit = (& git -C $repoRoot rev-parse HEAD 2>$null).Trim()
        $branch = (& git -C $repoRoot branch --show-current 2>$null).Trim()
    }
    catch {
        # Results remain useful when invoked from a source archive without Git.
    }
    return [ordered]@{ commit = $commit; branch = $branch }
}

function Get-CtsOutcomes {
    param([Parameter(Mandatory = $true)][string]$XmlPath)

    [xml]$document = Get-Content -LiteralPath $XmlPath -Raw
    $outcomes = @{}
    $testCases = $document.SelectNodes("//*[local-name()='testcase']")
    foreach ($testCase in $testCases) {
        $key = "$($testCase.classname)::$($testCase.name)"
        $outcome = "passed"
        if ($testCase.SelectSingleNode(".//*[local-name()='error']")) {
            $outcome = "error"
        }
        elseif ($testCase.SelectSingleNode(".//*[local-name()='failure']")) {
            $outcome = "failed"
        }
        elseif ($testCase.SelectSingleNode(".//*[local-name()='skipped']")) {
            $outcome = "skipped"
        }
        elseif ($testCase.SelectSingleNode(".//*[local-name()='warning']")) {
            $outcome = "warning"
        }
        $outcomes[$key] = $outcome
    }
    return $outcomes
}

function Resolve-ResultXmlPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        return $resolved
    }
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        $candidate = Get-ChildItem -LiteralPath $resolved -Filter "results.xml" -File | Select-Object -First 1
        if ($candidate) {
            return $candidate.FullName
        }
    }
    throw "Could not find a CTS results XML file at '$Path'."
}

function Write-Comparison {
    param(
        [Parameter(Mandatory = $true)][string]$BaselinePath,
        [Parameter(Mandatory = $true)][string]$CurrentPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory
    )

    $baselineXml = Resolve-ResultXmlPath -Path $BaselinePath
    $baseline = Get-CtsOutcomes -XmlPath $baselineXml
    $current = Get-CtsOutcomes -XmlPath $CurrentPath
    $regressions = @()
    $improvements = @()
    $changed = @()

    $allKeys = @($baseline.Keys) + @($current.Keys)
    foreach ($key in ($allKeys | Sort-Object -Unique)) {
        $before = if ($baseline.ContainsKey($key)) { $baseline[$key] } else { "missing" }
        $after = if ($current.ContainsKey($key)) { $current[$key] } else { "missing" }
        if ($before -eq $after) {
            continue
        }
        $entry = [ordered]@{ test = $key; baseline = $before; current = $after }
        $changed += $entry
        if (($before -in @("passed", "warning", "skipped")) -and ($after -in @("failed", "error", "missing"))) {
            $regressions += $entry
        }
        elseif (($before -in @("failed", "error", "missing")) -and ($after -in @("passed", "warning", "skipped"))) {
            $improvements += $entry
        }
    }

    $comparison = [ordered]@{
        baseline = $baselineXml
        current = $CurrentPath
        regressionCount = $regressions.Count
        improvementCount = $improvements.Count
        changedCount = $changed.Count
        regressions = $regressions
        improvements = $improvements
        changes = $changed
    }
    $comparisonPath = Join-Path $OutputDirectory "comparison.json"
    $comparison | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $comparisonPath -Encoding UTF8

    Write-Host "Comparison: $($regressions.Count) regression(s), $($improvements.Count) improvement(s), $($changed.Count) changed outcome(s)."
    Write-Host "Comparison report: $comparisonPath"
    return $comparison
}

$conformanceCli = Install-PinnedCts
Write-Host "CTS ready: $conformanceCli"
if ($DownloadOnly) {
    return
}

$testSpec = $suiteSpecs[$Suite]
$safeLabel = ($RunLabel -replace '[^A-Za-z0-9._-]', '-')
if ([string]::IsNullOrWhiteSpace($safeLabel)) {
    $safeLabel = "manual"
}
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runDirectory = Join-Path $ResultsDir "$timestamp-$safeLabel-$($Suite.ToLowerInvariant())-$($GraphicsPlugin.ToLowerInvariant())"
$resultsXml = Join-Path $runDirectory "results.xml"
$consoleLog = Join-Path $runDirectory "console.log"
$metadataPath = Join-Path $runDirectory "metadata.json"
$prematureExitGuard = Join-Path $runDirectory "premature-exit.guard"

$ctsArguments = @(
    $testSpec,
    "-G", $GraphicsPlugin,
    "--reporter", "ctsxml::out=$resultsXml",
    "--reporter", "console",
    "--premature-exit-guard-file", $prematureExitGuard
)
$ctsArguments += $AdditionalArguments

$metadata = [ordered]@{
    schemaVersion = 1
    runLabel = $RunLabel
    suite = $Suite
    testSpec = $testSpec
    graphicsPlugin = $GraphicsPlugin
    ctsVersion = $ctsVersion
    ctsReleaseTag = $ctsReleaseTag
    ctsArchiveSha256 = $ctsArchiveSha256
    command = @($conformanceCli) + $ctsArguments
    activeRuntime = Get-ActiveRuntimeMetadata
    vectorXrLayerRegistrations = @(Get-VectorXrLayerRegistrations)
    vectorXrSource = Get-GitMetadata
    startedAt = (Get-Date).ToString("o")
    completedAt = $null
    processExitCode = $null
    prematureExit = $null
}

Write-Host "Suite: $Suite"
Write-Host "Test selection: $testSpec"
Write-Host "Graphics plugin: $GraphicsPlugin"
Write-Host "Run label: $RunLabel"
Write-Host "Results: $runDirectory"

if ($Plan) {
    Write-Host "Plan only; CTS was not launched."
    return
}

New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

$processExitCode = $null
Push-Location (Split-Path -Parent $conformanceCli)
try {
    & $conformanceCli @ctsArguments 2>&1 | Tee-Object -FilePath $consoleLog
    $processExitCode = $LASTEXITCODE
}
finally {
    Pop-Location
}

$metadata.completedAt = (Get-Date).ToString("o")
$metadata.processExitCode = $processExitCode
$metadata.prematureExit = Test-Path -LiteralPath $prematureExitGuard
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

if (-not (Test-Path -LiteralPath $resultsXml -PathType Leaf)) {
    throw "CTS did not produce results.xml. See $consoleLog"
}

$outcomes = Get-CtsOutcomes -XmlPath $resultsXml
$failedCount = @($outcomes.Values | Where-Object { $_ -in @("failed", "error") }).Count
$warningCount = @($outcomes.Values | Where-Object { $_ -eq "warning" }).Count
$skippedCount = @($outcomes.Values | Where-Object { $_ -eq "skipped" }).Count
$summary = [ordered]@{
    total = $outcomes.Count
    failedOrErrored = $failedCount
    warnings = $warningCount
    skipped = $skippedCount
    passed = @($outcomes.Values | Where-Object { $_ -eq "passed" }).Count
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory "summary.json") -Encoding UTF8

Write-Host "CTS result summary: $($summary.passed) passed, $failedCount failed/errored, $warningCount warning, $skippedCount skipped."
Write-Host "Result XML: $resultsXml"

$comparison = $null
if ($CompareTo) {
    $comparison = Write-Comparison -BaselinePath $CompareTo -CurrentPath $resultsXml -OutputDirectory $runDirectory
}

if ($metadata.prematureExit) {
    throw "CTS did not exit cleanly; the premature-exit guard remains at $prematureExitGuard"
}
if ($processExitCode -ne 0) {
    throw "CTS process exited with code $processExitCode. See $consoleLog"
}
if ($comparison -and $comparison.regressionCount -gt 0) {
    throw "CTS comparison found $($comparison.regressionCount) regression(s). See $($runDirectory)\comparison.json"
}
if ($failedCount -gt 0 -and -not $CompareTo) {
    Write-Warning "CTS reported $failedCount failure(s). Establish a runtime baseline and use -CompareTo before attributing them to VectorXR."
}
