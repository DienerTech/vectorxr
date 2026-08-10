[CmdletBinding()]
param(
    [ValidateSet("Pivot", "PivotInteractive", "QuadViews", "Composition", "Automated")]
    [string]$Suite = "Pivot",

    [ValidateSet("D3D11", "D3D12", "Vulkan", "Vulkan2", "OpenGL")]
    [string]$GraphicsPlugin = "D3D11",

    [string]$RunLabel = "manual",
    [ValidateSet("Auto", "Any", "Enabled", "Disabled")]
    [string]$ExpectedLayerState = "Auto",
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
    # The first three are fully automatic assertions. The composition scenes
    # are rendered for a short period and then auto-skipped: this exercises the
    # relevant API paths without pretending software can judge the image.
    Pivot = "xrLocateSpace,xrLocateSpace_xrLocateViews,XrCompositionLayerQuad,QuadPoses,QuadHands,QuadProjectionQuad,ProjectionQuadProjection"

    # Full human-evaluated form, including the no-auto SpaceOffsets scenario.
    PivotInteractive = "xrLocateSpace,xrLocateSpace_xrLocateViews,XrCompositionLayerQuad,QuadPoses,QuadHands,QuadProjectionQuad,ProjectionQuadProjection,SpaceOffsets"

    # The extension form and the OpenXR 1.1 promoted names are both selected.
    QuadViews = "XR_VARJO_quad_views*,StereoWithFoveatedInset*"

    # Official CTS tag selection for all human-evaluated composition tests.
    Composition = "[composition][interactive]"

    # Official non-interactive selection. This is deliberately broad because
    # API layers can affect behavior outside the feature they intend to change.
    Automated = "exclude:[interactive]"
}

$suiteDefaultArguments = @{
    Pivot = @("--autoSkipTimeout", "3000")
    PivotInteractive = @()
    QuadViews = @()
    Composition = @()
    Automated = @()
}

# Keep the focused Pivot cases in separate processes. Some runtimes do not
# recover cleanly when an interactive composition session is auto-skipped, and
# a native crash in one CTS case would otherwise prevent every later case from
# producing a baseline result.
$pivotPhases = @(
    [ordered]@{ name = "xrLocateSpace"; spec = "xrLocateSpace"; arguments = @() },
    [ordered]@{ name = "xrLocateSpace_xrLocateViews"; spec = "xrLocateSpace_xrLocateViews"; arguments = @() },
    [ordered]@{ name = "XrCompositionLayerQuad"; spec = "XrCompositionLayerQuad"; arguments = @() },
    [ordered]@{ name = "QuadPoses"; spec = "QuadPoses"; arguments = @("--autoSkipTimeout", "3000") },
    [ordered]@{ name = "QuadHands"; spec = "QuadHands"; arguments = @("--autoSkipTimeout", "3000") },
    [ordered]@{ name = "QuadProjectionQuad"; spec = "QuadProjectionQuad"; arguments = @("--autoSkipTimeout", "3000") },
    [ordered]@{ name = "ProjectionQuadProjection"; spec = "ProjectionQuadProjection"; arguments = @("--autoSkipTimeout", "3000") }
)

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
    param([Parameter(Mandatory = $true)][string[]]$XmlPath)

    $outcomes = @{}
    foreach ($resultFile in $XmlPath) {
        [xml]$document = Get-Content -LiteralPath $resultFile -Raw
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
            else {
                $warning = $testCase.SelectSingleNode(".//*[local-name()='warning']")
                if ($warning -and $warning.InnerText -match "User-specified timeout reached") {
                    $outcome = "auto-skipped"
                }
                elseif ($warning) {
                    $outcome = "warning"
                }
            }
            $outcomes[$key] = $outcome
        }
    }
    return $outcomes
}

function Resolve-ResultXmlPaths {
    param([Parameter(Mandatory = $true)][string]$Path)

    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        return $resolved
    }
    if (Test-Path -LiteralPath $resolved -PathType Container) {
        $candidates = @(Get-ChildItem -LiteralPath $resolved -Filter "results*.xml" -File | Sort-Object Name)
        if ($candidates.Count -gt 0) {
            return @($candidates.FullName)
        }
    }
    throw "Could not find a CTS results XML file at '$Path'."
}

function Write-Comparison {
    param(
        [Parameter(Mandatory = $true)][string]$BaselinePath,
        [Parameter(Mandatory = $true)][string[]]$CurrentPath,
        [Parameter(Mandatory = $true)][string]$OutputDirectory
    )

    $baselineXml = @(Resolve-ResultXmlPaths -Path $BaselinePath)
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
        if (($before -in @("passed", "warning", "skipped", "auto-skipped")) -and ($after -in @("failed", "error", "missing"))) {
            $regressions += $entry
        }
        elseif (($before -in @("failed", "error", "missing")) -and ($after -in @("passed", "warning", "skipped", "auto-skipped"))) {
            $improvements += $entry
        }
    }

    $comparison = [ordered]@{
        baseline = @($baselineXml)
        current = @($CurrentPath)
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
$metadataPath = Join-Path $runDirectory "metadata.json"

if ($Suite -eq "Pivot") {
    $phases = @($pivotPhases)
}
else {
    $phases = @([ordered]@{
        name = $Suite
        spec = $testSpec
        arguments = @($suiteDefaultArguments[$Suite])
    })
}

$phasePlans = @()
for ($phaseIndex = 0; $phaseIndex -lt $phases.Count; $phaseIndex++) {
    $phase = $phases[$phaseIndex]
    $phaseNumber = $phaseIndex + 1
    $safePhaseName = ($phase.name -replace '[^A-Za-z0-9._-]', '-').ToLowerInvariant()
    $resultsXml = Join-Path $runDirectory ("results-{0:D2}-{1}.xml" -f $phaseNumber, $safePhaseName)
    $prematureExitGuard = Join-Path $runDirectory ("premature-exit-{0:D2}-{1}.guard" -f $phaseNumber, $safePhaseName)
    $ctsArguments = @(
        $phase.spec,
        "-G", $GraphicsPlugin,
        # Meta Link and some simulator-backed runtimes can report that no HMD
        # system is available while the headset/session is still waking up.
        "--pollGetSystem",
        "--reporter", "ctsxml::out=$resultsXml",
        "--reporter", "console",
        "--premature-exit-guard-file", $prematureExitGuard
    )
    $ctsArguments += @($phase.arguments)
    $ctsArguments += $AdditionalArguments
    $phasePlans += [ordered]@{
        name = $phase.name
        spec = $phase.spec
        resultsXml = $resultsXml
        prematureExitGuard = $prematureExitGuard
        arguments = @($ctsArguments)
        command = @($conformanceCli) + $ctsArguments
    }
}

$metadata = [ordered]@{
    schemaVersion = 2
    runLabel = $RunLabel
    suite = $Suite
    testSpec = $testSpec
    graphicsPlugin = $GraphicsPlugin
    ctsVersion = $ctsVersion
    ctsReleaseTag = $ctsReleaseTag
    ctsArchiveSha256 = $ctsArchiveSha256
    commands = @($phasePlans | ForEach-Object {
        [ordered]@{
            phase = $_.name
            executable = $conformanceCli
            arguments = @($_.arguments)
        }
    })
    activeRuntime = Get-ActiveRuntimeMetadata
    vectorXrLayerRegistrations = @(Get-VectorXrLayerRegistrations)
    expectedLayerState = $ExpectedLayerState
    vectorXrSource = Get-GitMetadata
    startedAt = (Get-Date).ToString("o")
    completedAt = $null
    invocations = @()
}

Write-Host "Suite: $Suite"
Write-Host "Test selection: $testSpec"
Write-Host "Graphics plugin: $GraphicsPlugin"
Write-Host "Run label: $RunLabel"
Write-Host "Results: $runDirectory"
if ($Suite -eq "Pivot") {
    Write-Host "Mode: hands-off; interactive composition scenes auto-skip after 3 seconds."
}

$runtimeName = $null
if ($metadata.activeRuntime.manifest -and $metadata.activeRuntime.manifest.runtime) {
    $runtimeName = $metadata.activeRuntime.manifest.runtime.name
}
if (-not $runtimeName) {
    $runtimeName = "Unknown runtime"
}
Write-Host "Active runtime: $runtimeName ($($metadata.activeRuntime.manifestPath))"

$enabledLayerRegistrations = @($metadata.vectorXrLayerRegistrations | Where-Object { $_.enabled })
$layerEnabled = $enabledLayerRegistrations.Count -gt 0
$layerStateLabel = if ($layerEnabled) { "enabled" } else { "disabled or not registered" }
Write-Host "VectorXR implicit layer: $layerStateLabel"

$effectiveExpectedLayerState = $ExpectedLayerState
if ($effectiveExpectedLayerState -eq "Auto") {
    if ($RunLabel -match "(?i)baseline") {
        $effectiveExpectedLayerState = "Disabled"
    }
    else {
        $effectiveExpectedLayerState = "Any"
    }
}
$metadata.effectiveExpectedLayerState = $effectiveExpectedLayerState

$layerStateError = $null
if ($effectiveExpectedLayerState -eq "Disabled" -and $layerEnabled) {
    $registeredPaths = @($enabledLayerRegistrations | ForEach-Object { $_.manifestPath }) -join ", "
    $layerStateError = "This run expects VectorXR to be disabled, but its implicit layer is enabled: $registeredPaths"
}
elseif ($effectiveExpectedLayerState -eq "Enabled" -and -not $layerEnabled) {
    $layerStateError = "This run expects VectorXR to be enabled, but no enabled implicit-layer registration was found."
}

if ($Plan) {
    if ($layerStateError) {
        Write-Warning $layerStateError
    }
    foreach ($phasePlan in $phasePlans) {
        Write-Host "Command [$($phasePlan.name)]: $conformanceCli $($phasePlan.arguments -join ' ')"
    }
    Write-Host "Plan only; CTS was not launched."
    return
}

if ($layerStateError) {
    throw $layerStateError
}

if ($Suite -eq "Pivot") {
    Write-Host "Launching hands-off CTS run. Keep the headset active; no controller pass/fail input is required."
}
else {
    Write-Host "Launching CTS. Put on the headset and follow its instructions."
    Write-Host "Interactive cases wait for controller pass/fail input; press Ctrl+C to cancel if the headset shows no CTS content."
}

New-Item -ItemType Directory -Force -Path $runDirectory | Out-Null
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

$phaseResults = @()
$savedErrorActionPreference = $ErrorActionPreference
Push-Location (Split-Path -Parent $conformanceCli)
try {
    # Keep the native process attached directly to the console. Piping through
    # Tee-Object makes the CTS C++ streams block-buffered, hiding its startup
    # and interactive progress until the process exits.
    # CTS and its validation layer also write conformance diagnostics to
    # stderr. They are test evidence, not PowerShell exceptions; keep them
    # visible and use the native exit code/result XML to judge the run.
    $ErrorActionPreference = "Continue"
    foreach ($phasePlan in $phasePlans) {
        Write-Host ""
        Write-Host "--- CTS phase: $($phasePlan.name) ---"
        $phaseStartedAt = (Get-Date).ToString("o")
        & $conformanceCli @($phasePlan.arguments)
        $processExitCode = $LASTEXITCODE
        $prematureExit = Test-Path -LiteralPath $phasePlan.prematureExitGuard
        $producedResults = Test-Path -LiteralPath $phasePlan.resultsXml -PathType Leaf
        $phaseResults += [ordered]@{
            name = $phasePlan.name
            spec = $phasePlan.spec
            resultsXml = $phasePlan.resultsXml
            startedAt = $phaseStartedAt
            completedAt = (Get-Date).ToString("o")
            processExitCode = $processExitCode
            prematureExit = $prematureExit
            producedResults = $producedResults
        }
        if ($processExitCode -ne 0 -or $prematureExit) {
            Write-Warning "CTS phase '$($phasePlan.name)' exited abnormally (exit code $processExitCode). Continuing with the remaining isolated phases."
        }
    }
}
finally {
    $ErrorActionPreference = $savedErrorActionPreference
    Pop-Location
}

$metadata.completedAt = (Get-Date).ToString("o")
$metadata.invocations = @($phaseResults)
$metadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

$resultXmlPaths = @($phaseResults | Where-Object { $_.producedResults } | ForEach-Object { $_.resultsXml })
$missingResultPhases = @($phaseResults | Where-Object { -not $_.producedResults })
if ($resultXmlPaths.Count -eq 0) {
    throw "CTS did not produce any result XML. Verify that $runtimeName exposes an awake HMD and review the terminal output above."
}

$outcomes = Get-CtsOutcomes -XmlPath $resultXmlPaths
$failedCount = @($outcomes.Values | Where-Object { $_ -in @("failed", "error") }).Count
$warningCount = @($outcomes.Values | Where-Object { $_ -eq "warning" }).Count
$skippedCount = @($outcomes.Values | Where-Object { $_ -eq "skipped" }).Count
$autoSkippedCount = @($outcomes.Values | Where-Object { $_ -eq "auto-skipped" }).Count
$summary = [ordered]@{
    total = $outcomes.Count
    failedOrErrored = $failedCount
    warnings = $warningCount
    skipped = $skippedCount
    autoSkipped = $autoSkippedCount
    abnormalProcessExits = @($phaseResults | Where-Object { $_.processExitCode -ne 0 -or $_.prematureExit }).Count
    missingResultPhases = $missingResultPhases.Count
    passed = @($outcomes.Values | Where-Object { $_ -eq "passed" }).Count
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory "summary.json") -Encoding UTF8

Write-Host "CTS result summary: $($summary.passed) passed, $failedCount failed/errored, $autoSkippedCount auto-skipped, $warningCount warning, $skippedCount skipped."
Write-Host "Result XML files: $($resultXmlPaths -join ', ')"

$comparison = $null
if ($CompareTo) {
    $comparison = Write-Comparison -BaselinePath $CompareTo -CurrentPath $resultXmlPaths -OutputDirectory $runDirectory
}

if ($comparison -and $comparison.regressionCount -gt 0) {
    throw "CTS comparison found $($comparison.regressionCount) regression(s). See $($runDirectory)\comparison.json"
}
if ($failedCount -gt 0 -and -not $CompareTo) {
    Write-Warning "CTS reported $failedCount failure(s). Establish a runtime baseline and use -CompareTo before attributing them to VectorXR."
}
if ($missingResultPhases.Count -gt 0) {
    $missingNames = @($missingResultPhases | ForEach-Object { $_.name }) -join ", "
    throw "CTS produced no result XML for $($missingResultPhases.Count) phase(s): $missingNames"
}
