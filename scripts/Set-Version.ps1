<#
.SYNOPSIS
    Prepares a VectorXR version and can publish its complete GitHub release.

.DESCRIPTION
    Updates every app version and lock file, inserts the in-app patch note, writes
    the matching Markdown GitHub release notes, and validates the result. With
    -Publish, it also commits release-managed files, creates an annotated tag,
    atomically pushes the current branch and tag, and waits for the GitHub Actions
    release workflow to finish.

.EXAMPLE
    ./scripts/Set-Version.ps1 -Version 0.11.2 `
        -Title "Layer startup diagnostics" `
        -Summary "Adds detailed startup logging across the OpenXR layer chain." `
        -Items "First change.","Second change." `
        -Publish

.EXAMPLE
    ./scripts/Set-Version.ps1 -Version 0.11.2 `
        -Title "Layer startup diagnostics" `
        -Summary "Adds detailed startup logging across the OpenXR layer chain." `
        -Items "First change.","Second change." -Publish -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "Medium")]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Summary,
    [Parameter(Mandatory = $true)][string[]]$Items,
    [string]$Date,
    [switch]$SkipLocks,
    [switch]$Publish,
    [switch]$NoWait,
    [string]$Remote = "origin",
    [string]$CommitMessage,
    [ValidateRange(15, 600)][int]$WorkflowDiscoveryTimeoutSeconds = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "_VersionFiles.ps1")

if ($Version.StartsWith("v")) { $Version = $Version.Substring(1) }
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version '$Version' must use MAJOR.MINOR.PATCH format, such as 0.11.1."
}
if (-not $Date) { $Date = (Get-Date -Format 'yyyy-MM-dd') }
if ($Date -notmatch '^\d{4}-\d{2}-\d{2}$') {
    throw "Date '$Date' must use YYYY-MM-DD format."
}

# --- IMPORTANT: quote-escaping gotcha -----------------------------------------
# patchNotes.ts uses SINGLE-QUOTED TypeScript string literals. Any apostrophe in a
# title / summary / item (e.g. "application's") would terminate the string literal
# and break the build if inserted raw. Escape backslashes first (so we don't double
# escape), then escape single quotes as \'. Do NOT remove this -- it is the reason
# patch-note entries with apostrophes can be passed safely on the command line.
# ------------------------------------------------------------------------------
function ConvertTo-TsSingleQuoted {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $escaped = $Text.Replace('\', '\\').Replace("'", "\'")
    return "'$escaped'"
}

function Add-PatchNoteEntry {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Date,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string[]]$Items
    )

    $path = Get-PatchNotesPath
    $content = Read-FileRaw -Path $path

    if ($content -match ("version:\s*'" + [regex]::Escape($Version) + "'")) {
        throw "patchNotes.ts already contains an entry for version $Version."
    }

    # Preserve the file's existing newline style so the insertion produces a clean diff.
    $nl = if ($content -match "`r`n") { "`r`n" } else { "`n" }

    $anchor = "export const patchNotes: PatchNoteEntry[] = ["
    $anchorIndex = $content.IndexOf($anchor)
    if ($anchorIndex -lt 0) {
        throw "Could not find the patchNotes array opener in patchNotes.ts."
    }
    $insertPos = $content.IndexOf($nl, $anchorIndex) + $nl.Length

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("  {")
    $lines.Add("    version: $(ConvertTo-TsSingleQuoted $Version),")
    $lines.Add("    date: $(ConvertTo-TsSingleQuoted $Date),")
    $lines.Add("    title: $(ConvertTo-TsSingleQuoted $Title),")
    $lines.Add("    summary: $(ConvertTo-TsSingleQuoted $Summary),")
    $lines.Add("    items: [")
    foreach ($item in $Items) {
        $lines.Add("      $(ConvertTo-TsSingleQuoted $item),")
    }
    $lines.Add("    ],")
    $lines.Add("  },")

    $block = ($lines -join $nl) + $nl
    $updated = $content.Substring(0, $insertPos) + $block + $content.Substring($insertPos)
    Write-FileNoBom -Path $path -Content $updated
}

# Edits ONLY the vectorxr-app entry in a derived lock file, anchored on its package
# name so dependency versions are never touched. We deliberately edit rather than
# regenerate (npm install / cargo update), which would churn unrelated dependencies
# and produce a noisy release diff. Lock files are not part of the tag gate.
function Update-LockVersion {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$NewVersion
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Warning "  skipped $Name (not found)"
        return
    }
    $content = Read-FileRaw -Path $Path
    $regex = [regex]::new($Pattern, [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $regex.IsMatch($content)) {
        throw "Could not find the vectorxr-app version entry in '$Name'."
    }
    $updated = $regex.Replace($content, "`${1}$NewVersion`${2}", 1)
    Write-FileNoBom -Path $Path -Content $updated
    Write-Host "  $Name"
}

function Assert-ExistingPatchNoteMatches {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Date,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string[]]$Items
    )

    $content = Read-FileRaw -Path (Get-PatchNotesPath)
    $nl = if ($content -match "`r`n") { "`r`n" } else { "`n" }
    $lines = @(
        "  {"
        "    version: $(ConvertTo-TsSingleQuoted $Version),"
        "    date: $(ConvertTo-TsSingleQuoted $Date),"
        "    title: $(ConvertTo-TsSingleQuoted $Title),"
        "    summary: $(ConvertTo-TsSingleQuoted $Summary),"
        "    items: ["
    )
    foreach ($item in $Items) {
        $lines += "      $(ConvertTo-TsSingleQuoted $item),"
    }
    $lines += "    ],"
    $lines += "  },"

    $expectedBlock = ($lines -join $nl) + $nl
    if (-not $content.Contains($expectedBlock)) {
        throw "The existing patch-notes entry for $Version does not match -Date, -Title, -Summary, and -Items."
    }
}

function Write-ReleaseNotes {
    param(
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Title,
        [Parameter(Mandatory = $true)][string]$Summary,
        [Parameter(Mandatory = $true)][string[]]$Items
    )

    $path = Get-ReleaseNotesPath -Version $Version
    $directory = Split-Path -Parent $path
    New-Item -ItemType Directory -Path $directory -Force | Out-Null

    $lines = @(
        "## $Title"
        ""
        $Summary
        ""
        "### Changes"
        ""
    )
    foreach ($item in $Items) {
        $lines += "- $item"
    }

    Write-FileNoBom -Path $path -Content (($lines -join "`n") + "`n")
    Write-Host "  release/notes/v$Version.md"
}

function Invoke-CapturedNativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    $output = @(& $FilePath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $text = (($output | ForEach-Object { "$_" }) -join "`n").Trim()
    if ($exitCode -ne 0) {
        if ($text) {
            throw "$FailureMessage (exit code $exitCode):`n$text"
        }
        throw "$FailureMessage (exit code $exitCode)."
    }
    return $text
}

function Invoke-CheckedNativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit code $LASTEXITCODE)."
    }
}

function ConvertTo-RepoRelativeGitPath {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $rootWithSeparator = $RepoRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($rootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Release-managed path is outside the repository: $fullPath"
    }
    return $fullPath.Substring($rootWithSeparator.Length).Replace('\', '/')
}

function Assert-PublishPreconditions {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Remote,
        [Parameter(Mandatory = $true)][string]$TagName,
        [Parameter(Mandatory = $true)][bool]$WaitForWorkflow
    )

    Push-Location $RepoRoot
    try {
        $insideWorkTree = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "rev-parse", "--is-inside-work-tree"
        ) -FailureMessage "This command must run inside the VectorXR Git repository"
        if ($insideWorkTree -ne "true") {
            throw "This command must run inside the VectorXR Git repository."
        }

        $status = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "status", "--porcelain=v1", "--untracked-files=all"
        ) -FailureMessage "Could not inspect the Git worktree"
        if ($status) {
            throw "Publishing requires a clean worktree. Commit or stash these changes first:`n$status"
        }

        $branch = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "branch", "--show-current"
        ) -FailureMessage "Could not determine the current Git branch"
        if (-not $branch) {
            throw "Publishing from a detached HEAD is not supported. Check out the release branch first."
        }

        [void](Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "remote", "get-url", $Remote
        ) -FailureMessage "Git remote '$Remote' is not configured")

        if ($WaitForWorkflow) {
            if (-not (Get-Command "gh" -ErrorAction SilentlyContinue)) {
                throw "GitHub CLI is required to follow the release. Install gh or use -NoWait."
            }
            & gh auth status --hostname github.com *> $null
            if ($LASTEXITCODE -ne 0) {
                throw "GitHub CLI is not authenticated. Run 'gh auth login --hostname github.com', or use -NoWait."
            }
        }

        [void](Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "fetch", "--tags", $Remote
        ) -FailureMessage "Could not fetch tags from '$Remote'")

        & git show-ref --verify --quiet "refs/tags/$TagName"
        $showRefExitCode = $LASTEXITCODE
        if ($showRefExitCode -notin @(0, 1)) {
            throw "Could not inspect local tag '$TagName' (exit code $showRefExitCode)."
        }
        $localTagExists = $showRefExitCode -eq 0

        $remoteTag = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "ls-remote", "--tags", $Remote, "refs/tags/$TagName"
        ) -FailureMessage "Could not inspect tag '$TagName' on '$Remote'"
        $remoteTagExists = [bool]$remoteTag

        $headSha = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "rev-parse", "HEAD"
        ) -FailureMessage "Could not resolve HEAD"

        if ($localTagExists -or $remoteTagExists) {
            $tagCommit = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
                "rev-list", "-n", "1", $TagName
            ) -FailureMessage "Could not resolve existing tag '$TagName'"
            if ($tagCommit -ne $headSha) {
                throw "Tag '$TagName' already exists at $tagCommit, but HEAD is $headSha."
            }
        }

        return [pscustomobject]@{
            Branch          = $branch
            HeadSha         = $headSha
            LocalTagExists  = $localTagExists
            RemoteTagExists = $remoteTagExists
        }
    }
    finally {
        Pop-Location
    }
}

function Wait-ForReleaseWorkflow {
    param(
        [Parameter(Mandatory = $true)][string]$TagName,
        [Parameter(Mandatory = $true)][string]$TaggedCommit,
        [Parameter(Mandatory = $true)][int]$DiscoveryTimeoutSeconds
    )

    Write-Host "Waiting for the GitHub Actions release workflow ..."
    $deadline = (Get-Date).AddSeconds($DiscoveryTimeoutSeconds)
    $run = $null

    do {
        $json = Invoke-CapturedNativeCommand -FilePath "gh" -Arguments @(
            "run", "list",
            "--workflow", "release.yml",
            "--event", "push",
            "--limit", "20",
            "--json", "databaseId,headBranch,headSha,status,conclusion,url"
        ) -FailureMessage "Could not query GitHub Actions runs"

        if ($json) {
            # Windows PowerShell 5.1 emits a top-level JSON array as one pipeline
            # object. Explicitly pipe the parsed value again so its elements are
            # enumerated; otherwise property access below returns all run IDs and
            # gh run watch receives them as one malformed, space-delimited ID.
            $parsedRuns = $json | ConvertFrom-Json
            $runs = @($parsedRuns | ForEach-Object { $_ })
            $run = $runs |
                Where-Object { $_.headBranch -eq $TagName -and $_.headSha -eq $TaggedCommit } |
                Sort-Object -Property databaseId -Descending |
                Select-Object -First 1
        }

        if (-not $run) {
            Start-Sleep -Seconds 3
        }
    } while (-not $run -and (Get-Date) -lt $deadline)

    if (-not $run) {
        throw "Tag '$TagName' was pushed, but its release workflow was not discovered within $DiscoveryTimeoutSeconds seconds. Check GitHub Actions; rerunning this command is safe."
    }

    Write-Host "Following $($run.url)"
    Invoke-CheckedNativeCommand -FilePath "gh" -Arguments @(
        "run", "watch", "$($run.databaseId)", "--exit-status", "--interval", "10"
    ) -FailureMessage "The GitHub Actions release workflow failed"

    $releaseUrl = Invoke-CapturedNativeCommand -FilePath "gh" -Arguments @(
        "release", "view", $TagName, "--json", "url", "--jq", ".url"
    ) -FailureMessage "The workflow completed, but the GitHub Release could not be found"

    Write-Host ""
    Write-Host "Release published: $releaseUrl"
}

function Publish-Release {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$TagName,
        [Parameter(Mandatory = $true)][string]$Remote,
        [Parameter(Mandatory = $true)][string]$CommitMessage,
        [Parameter(Mandatory = $true)][object]$PublishState,
        [Parameter(Mandatory = $true)][bool]$WaitForWorkflow,
        [Parameter(Mandatory = $true)][int]$DiscoveryTimeoutSeconds
    )

    Push-Location $RepoRoot
    try {
        $status = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "status", "--porcelain=v1", "--untracked-files=all"
        ) -FailureMessage "Could not inspect generated release changes"

        if (($PublishState.LocalTagExists -or $PublishState.RemoteTagExists) -and $status) {
            throw "Tag '$TagName' already exists at HEAD, but the generated release files differ:`n$status"
        }

        if (-not $PublishState.LocalTagExists) {
            $managedPaths = @(Get-ReleaseManagedPaths -Version $Version)
            $relativePaths = @(
                $managedPaths | ForEach-Object {
                    ConvertTo-RepoRelativeGitPath -RepoRoot $RepoRoot -Path $_
                }
            )

            Invoke-CheckedNativeCommand -FilePath "git" -Arguments (@(
                "add", "--"
            ) + $relativePaths) -FailureMessage "Could not stage release files"

            & git diff --cached --quiet
            $diffExitCode = $LASTEXITCODE
            if ($diffExitCode -eq 1) {
                Invoke-CheckedNativeCommand -FilePath "git" -Arguments @(
                    "commit", "-m", $CommitMessage
                ) -FailureMessage "Could not create the release commit"
            }
            elseif ($diffExitCode -ne 0) {
                throw "Could not inspect staged release changes (exit code $diffExitCode)."
            }
            else {
                Write-Host "Release files already match; tagging the current commit."
            }

            Invoke-CheckedNativeCommand -FilePath "git" -Arguments @(
                "tag", "--annotate", $TagName, "--message", "VectorXR $TagName"
            ) -FailureMessage "Could not create annotated tag '$TagName'"
        }
        else {
            Write-Host "Reusing existing local tag $TagName."
        }

        if (-not $PublishState.RemoteTagExists) {
            Invoke-CheckedNativeCommand -FilePath "git" -Arguments @(
                "push", "--atomic", $Remote,
                "HEAD:refs/heads/$($PublishState.Branch)",
                "refs/tags/$TagName"
            ) -FailureMessage "Could not atomically push branch '$($PublishState.Branch)' and tag '$TagName'"
        }
        else {
            Write-Host "Remote tag $TagName already exists; no push needed."
        }

        $taggedCommit = Invoke-CapturedNativeCommand -FilePath "git" -Arguments @(
            "rev-list", "-n", "1", $TagName
        ) -FailureMessage "Could not resolve tagged commit"

        if ($WaitForWorkflow) {
            Wait-ForReleaseWorkflow -TagName $TagName -TaggedCommit $taggedCommit -DiscoveryTimeoutSeconds $DiscoveryTimeoutSeconds
        }
        else {
            Write-Host ""
            Write-Host "Tag $TagName is on $Remote. GitHub Actions will build and publish the release."
        }
    }
    finally {
        Pop-Location
    }
}

if (-not $Title.Trim()) {
    throw "Title cannot be empty."
}
if (-not $Summary.Trim()) {
    throw "Summary cannot be empty."
}
if ($Items.Count -eq 0 -or @($Items | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
    throw "Items must contain at least one non-empty release-note item."
}
$textValues = @($Title, $Summary) + @($Items)
if (@($textValues | Where-Object { $_ -match "[`r`n]" }).Count -gt 0) {
    throw "Title, Summary, and Items must each be single-line text."
}
if (-not $Remote -or $Remote.StartsWith("-")) {
    throw "Remote must be a configured Git remote name."
}

if ($Publish -and $SkipLocks) {
    throw "-SkipLocks cannot be used with -Publish."
}
if ($NoWait -and -not $Publish) {
    throw "-NoWait can only be used with -Publish."
}

$repoRoot = Get-RepoRoot
$tagName = "v$Version"
$waitForWorkflow = $Publish -and -not $NoWait
if (-not $CommitMessage) {
    $CommitMessage = "release: VectorXR $tagName"
}

$action = if ($Publish) {
    "prepare $tagName, commit release files, atomically push the branch and tag, and publish the GitHub Release"
}
else {
    "prepare $tagName and generate its in-app and GitHub release notes"
}

if (-not $PSCmdlet.ShouldProcess($repoRoot, $action)) {
    return
}

$publishState = $null
if ($Publish) {
    $publishState = Assert-PublishPreconditions -RepoRoot $repoRoot -Remote $Remote -TagName $tagName -WaitForWorkflow $waitForWorkflow
}

$patchNotesContent = Read-FileRaw -Path (Get-PatchNotesPath)
$latestPatchVersion = Read-LatestPatchVersion
$versionPattern = "version:\s*'" + [regex]::Escape($Version) + "'"
if ($latestPatchVersion -eq $Version) {
    Write-Host "Using existing patch-notes entry for $Version ($Date) ..."
    Assert-ExistingPatchNoteMatches -Version $Version -Date $Date -Title $Title -Summary $Summary -Items $Items
}
elseif ($patchNotesContent -match $versionPattern) {
    throw "patchNotes.ts contains version $Version, but it is not the latest entry."
}
else {
    Write-Host "Inserting patch-notes entry for $Version ($Date) ..."
    Add-PatchNoteEntry -Version $Version -Date $Date -Title $Title -Summary $Summary -Items $Items
}

Write-Host "Updating source version files to $Version ..."
foreach ($target in Get-VersionTargets) {
    $old = Read-TargetVersion -Target $target
    Set-TargetVersion -Target $target -NewVersion $Version
    Write-Host "  $($target.Name): $old -> $Version"
}

Write-Host "Writing GitHub release notes ..."
Write-ReleaseNotes -Version $Version -Title $Title -Summary $Summary -Items $Items

if ($SkipLocks) {
    Write-Host "Skipping lock files (-SkipLocks)."
}
else {
    Write-Host "Syncing lock files ..."
    # Each pattern has two capture groups: (1) prefix through the opening quote,
    # (2) closing quote. The version value between them is replaced.
    Update-LockVersion -Name "app/src-tauri/Cargo.lock" `
        -Path (Join-Path $repoRoot "app/src-tauri/Cargo.lock") `
        -Pattern '(name = "vectorxr-app"\s+version = ")[^"]+(")' `
        -NewVersion $Version
    Update-LockVersion -Name "app/package-lock.json (root)" `
        -Path (Join-Path $repoRoot "app/package-lock.json") `
        -Pattern '(\A\{\s*"name":\s*"vectorxr-app",\s*"version":\s*")[^"]+(")' `
        -NewVersion $Version
    Update-LockVersion -Name "app/package-lock.json (packages root)" `
        -Path (Join-Path $repoRoot "app/package-lock.json") `
        -Pattern '("":\s*\{\s*"name":\s*"vectorxr-app",\s*"version":\s*")[^"]+(")' `
        -NewVersion $Version
}

Write-Host "Verifying ..."
& (Join-Path $PSScriptRoot "Assert-VersionMatchesTag.ps1") -TagName $tagName

if ($Publish) {
    Publish-Release -RepoRoot $repoRoot -Version $Version -TagName $tagName -Remote $Remote -CommitMessage $CommitMessage -PublishState $publishState -WaitForWorkflow $waitForWorkflow -DiscoveryTimeoutSeconds $WorkflowDiscoveryTimeoutSeconds
}
else {
    Write-Host ""
    Write-Host "Done. Review the generated release changes; add -Publish to commit, tag, push, and publish."
}
