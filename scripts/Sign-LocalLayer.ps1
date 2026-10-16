<#
.SYNOPSIS
Signs a locally built VectorXR layer for testing in games with anti-cheat.

.DESCRIPTION
Opt-in, for local testing only; releases are signed by the release workflow.

Copies the built layer (DLL, manifest and sounds) into
local-signing\builds\<UTC time>-<commit>\vectorxr-layer in the main checkout, which git
ignores. There it signs XR_APILAYER_DIENERTECH_VECTORXR.dll, and nothing else, with Azure
Artifact Signing through your Azure CLI login. It verifies the DienerTech LLC publisher and
the timestamp, and appends the signing to local-signing\ledger.csv. -Install then copies the
signed layer into Program Files and registers it; Windows asks for an administrator.

Guards:
- Refuses a checkout with uncommitted changes to tracked files; -AllowDirty overrides, and
  the ledger records it.
- Refuses a DLL older than any of the layer's source files; rebuild first.
- The signing client is a pinned NuGet package, checked against its SHA256 when it is
  downloaded and against Microsoft's signature before every use.
- Only the Azure CLI credential is offered to Azure, and only the file's digest is sent.

Settings: AZURE_SIGNING_ENDPOINT, AZURE_SIGNING_ACCOUNT and AZURE_CERTIFICATE_PROFILE, as the
release workflow names them, else local-signing\signing.json:
    { "endpoint": "https://<region>.codesigning.azure.net/", "account": "<account>",
      "profile": "<certificate profile>" }

A signed build is attributable to DienerTech LLC and cannot be recalled: keep it on this
machine. Never sign on a release day: a revocation covers everything signed that day.

.EXAMPLE
.\scripts\Build-Layer.ps1 -Configuration Release -SkipTests
.\scripts\Sign-LocalLayer.ps1 -Install -Note "Star Citizen anti-cheat test"
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$Install,
    [string]$InstallDir = "C:\Program Files\VectorXR-Local\vectorxr-layer",
    [switch]$AllowDirty,
    [string]$Note = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$LayerDll = "XR_APILAYER_DIENERTECH_VECTORXR.dll"
$LayerManifest = "XR_APILAYER_DIENERTECH_VECTORXR.json"
$ClientPackage = "Microsoft.ArtifactSigning.Client"
$ClientVersion = "1.0.128"
$ClientSha256 = "74BD7D27E6CE1051409C38D9B46BC8DF0400ECD643D51FFBF2AC00869061E40B"
$TimestampUrl = "http://timestamp.acs.microsoft.com"
# The paths whose changes change the layer DLL (tauri.conf.json carries its version).
$LayerSources = @("layer", "CMakeLists.txt", "cmake", "third_party", "app/src-tauri/tauri.conf.json")

function Invoke-Git {
    param([string]$Root, [string[]]$Arguments)
    $output = & git -C $Root @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed in $Root."
    }
    return $output
}

function Assert-MicrosoftSigned {
    param([string]$Path)
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $subject = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { "no signer" }
    # Once Windows catalogs the same VC runtime DLLs, they verify through the catalog: CN=Microsoft Windows.
    if ($signature.Status -ne "Valid" -or $subject -notmatch '(^|,\s*)CN=Microsoft (Corporation|Windows)(,|$)' -or
        $subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw "The signing client's '$Path' is not validly signed by Microsoft ($($signature.Status), $subject); delete the tools folder to fetch it again."
    }
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# --- The checkout: its commit, and whether the build can be traced to it.
$commit = Invoke-Git $repoRoot @("rev-parse", "HEAD")
$shortCommit = $commit.Substring(0, 7)
$branch = Invoke-Git $repoRoot @("rev-parse", "--abbrev-ref", "HEAD")
$changes = @(Invoke-Git $repoRoot @("status", "--porcelain", "--untracked-files=no") | Where-Object { $_ })
$dirty = $changes.Count -gt 0
if ($dirty -and -not $AllowDirty) {
    throw "The checkout has uncommitted changes to tracked files, so the signed DLL would match no commit:`n" +
          ($changes -join "`n") + "`nCommit them, or pass -AllowDirty (the ledger records it)."
}

# --- One local-signing folder for every worktree: the main checkout's.
$commonDir = Invoke-Git $repoRoot @("rev-parse", "--path-format=absolute", "--git-common-dir")
$mainRoot = Split-Path -Parent ([System.IO.Path]::GetFullPath($commonDir))
$signingRoot = Join-Path $mainRoot "local-signing"
New-Item -ItemType Directory -Force $signingRoot | Out-Null
# Ignored on every branch, whatever its .gitignore says.
$selfIgnore = Join-Path $signingRoot ".gitignore"
if (-not (Test-Path -LiteralPath $selfIgnore)) {
    Set-Content -LiteralPath $selfIgnore -Value "*" -Encoding ascii
}
$ledger = Join-Path $signingRoot "ledger.csv"

function Write-Ledger {
    param([string]$Result, [hashtable]$Fields)
    $row = [ordered]@{
        utc            = [DateTimeOffset]::UtcNow.ToString("o")
        result         = $Result
        commit         = $commit
        branch         = $branch
        dirty          = $dirty
        source         = $Fields.source
        unsignedSha256 = $Fields.unsigned
        signedSha256   = $Fields.signed
        staged         = $Fields.staged
        installed      = $Fields.installed
        azureUser      = $Fields.azure
        windowsUser    = "$env:USERDOMAIN\$env:USERNAME"
        note           = $Note
    }
    [pscustomobject]$row | Export-Csv -LiteralPath $ledger -Append -NoTypeInformation -Encoding utf8
}

# --- The built layer.
$buildOutput = Join-Path $repoRoot (Join-Path $BuildDir (Join-Path "layer" $Configuration))
$sourceDll = Join-Path $buildOutput $LayerDll
$sourceManifest = Join-Path $buildOutput $LayerManifest
foreach ($path in @($sourceDll, $sourceManifest)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "'$path' was not found. Build the layer first (scripts\Build-Layer.ps1 -Configuration $Configuration)."
    }
}
# Built from these sources: none of the layer's tracked files is newer than the
# DLL (with the checkout clean, they are the commit's). Commit times would not
# do: building, testing, then committing leaves the DLL older than its commit,
# and a rebuild of unchanged sources does not relink it.
$built = (Get-Item -LiteralPath $sourceDll).LastWriteTimeUtc
$newest = Invoke-Git $repoRoot (@("ls-files", "--") + $LayerSources) | Where-Object { $_ } |
    ForEach-Object { Get-Item -LiteralPath (Join-Path $repoRoot $_) -ErrorAction SilentlyContinue } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($newest -and $newest.LastWriteTimeUtc -gt $built) {
    throw "'$sourceDll' was built at $($built.ToString('u')), before '$($newest.FullName)' last changed " +
          "($($newest.LastWriteTimeUtc.ToString('u'))): rebuild it."
}

# --- Settings: the release workflow's variables, else the local file.
$settingsFile = Join-Path $signingRoot "signing.json"
$fileSettings = $null
if (Test-Path -LiteralPath $settingsFile) {
    $fileSettings = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
}
function Get-Setting {
    param([string]$Variable, [string]$Field)
    $value = [Environment]::GetEnvironmentVariable($Variable)
    if ([string]::IsNullOrWhiteSpace($value) -and $fileSettings -and ($fileSettings.PSObject.Properties.Name -contains $Field)) {
        $value = $fileSettings.$Field
    }
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "No signing ${Field}: set $Variable, or '$Field' in $settingsFile."
    }
    return $value
}
$endpoint = Get-Setting "AZURE_SIGNING_ENDPOINT" "endpoint"
$account = Get-Setting "AZURE_SIGNING_ACCOUNT" "account"
$certificateProfile = Get-Setting "AZURE_CERTIFICATE_PROFILE" "profile"

# --- Azure CLI login (the only credential offered).
# Windows PowerShell turns a native command's redirected stderr into an error under Stop.
$ErrorActionPreference = "Continue"
$azureUser = & az account show --query user.name -o tsv 2>$null
$azureExit = $LASTEXITCODE
$ErrorActionPreference = "Stop"
if ($azureExit -ne 0 -or [string]::IsNullOrWhiteSpace($azureUser)) {
    throw "The Azure CLI is not logged in: run 'az login' with the account that may sign."
}

# --- SignTool from the newest Windows SDK (Artifact Signing needs 10.0.22621 or later).
$signTool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Where-Object { [version]$_.Directory.Parent.Name -ge [version]"10.0.22621.0" } |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending | Select-Object -First 1
if (-not $signTool) {
    throw "SignTool from Windows SDK 10.0.22621 or later was not found: install the Windows SDK."
}

# --- The signing client, pinned.
$clientDir = Join-Path $signingRoot "tools\$ClientPackage.$ClientVersion"
$dlib = Join-Path $clientDir "bin\x64\Azure.CodeSigning.Dlib.dll"
if (-not (Test-Path -LiteralPath $dlib)) {
    $download = Join-Path $signingRoot "tools\download-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force $download | Out-Null
    try {
        $package = Join-Path $download "package.zip"
        Write-Host "Downloading $ClientPackage $ClientVersion from nuget.org."
        $ProgressPreference = "SilentlyContinue"
        Invoke-WebRequest "https://www.nuget.org/api/v2/package/$ClientPackage/$ClientVersion" -OutFile $package -UseBasicParsing
        $hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
        if ($hash -ne $ClientSha256) {
            throw "$ClientPackage $ClientVersion has SHA256 $hash, not the pinned ${ClientSha256}: not used."
        }
        if (Test-Path -LiteralPath $clientDir) {
            Remove-Item -LiteralPath $clientDir -Recurse -Force
        }
        Expand-Archive -LiteralPath $package -DestinationPath $clientDir
    } finally {
        Remove-Item -LiteralPath $download -Recurse -Force -ErrorAction SilentlyContinue
    }
}
# Everything SignTool loads from the client's folder, before every use.
Get-ChildItem -LiteralPath (Split-Path -Parent $dlib) -File -Filter *.dll | ForEach-Object { Assert-MicrosoftSigned $_.FullName }

# --- Stage a copy (the build output is rebuilt over; the copy keeps its signature).
$stamp = [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss")
$buildRoot = Join-Path $signingRoot "builds\$stamp-$shortCommit"
$stage = Join-Path $buildRoot "vectorxr-layer"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -LiteralPath $sourceDll, $sourceManifest -Destination $stage
$sounds = Join-Path $buildOutput "sounds"
if (Test-Path -LiteralPath $sounds) {
    Copy-Item -LiteralPath $sounds -Destination $stage -Recurse
}
$stagedDll = Join-Path $stage $LayerDll
$unsigned = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
$fields = @{ source = $sourceDll; unsigned = $unsigned; signed = ""; staged = $stagedDll; installed = ""; azure = $azureUser }

$metadata = Join-Path $buildRoot "metadata.json"
[ordered]@{
    Endpoint               = $endpoint
    CodeSigningAccountName = $account
    CertificateProfileName = $certificateProfile
    CorrelationId          = "vectorxr-local-$shortCommit"
    ExcludeCredentials     = @("EnvironmentCredential", "WorkloadIdentityCredential", "ManagedIdentityCredential",
                               "SharedTokenCacheCredential", "VisualStudioCredential", "VisualStudioCodeCredential",
                               "AzurePowerShellCredential", "AzureDeveloperCliCredential", "InteractiveBrowserCredential")
} | ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding ascii

# --- Sign the staged DLL only, then verify it as the release workflow does.
try {
    Write-Host "Signing $stagedDll (commit $shortCommit on $branch$(if ($dirty) { ', uncommitted changes' }))."
    & $signTool.FullName sign /fd SHA256 /tr $TimestampUrl /td SHA256 /d "VectorXR local test build" `
        /dlib $dlib /dmdf $metadata $stagedDll
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool failed (exit code $LASTEXITCODE)."
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $stagedDll
    if ($signature.Status -ne "Valid") {
        throw "The signed DLL's signature is $($signature.Status)."
    }
    if ($signature.SignerCertificate.Subject -notmatch '(^|,\s*)CN=DienerTech LLC(,|$)' -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=DienerTech LLC(,|$)') {
        throw "Unexpected publisher: $($signature.SignerCertificate.Subject)."
    }
    if (-not $signature.TimeStamperCertificate) {
        throw "The signature has no trusted timestamp."
    }
    $fields.signed = (Get-FileHash -LiteralPath $stagedDll -Algorithm SHA256).Hash
} catch {
    Write-Ledger "failed: $($_.Exception.Message)" $fields
    throw
}
Write-Ledger "signed" $fields
Write-Host "Signed: $stagedDll"
Write-Host "  SHA256 $($fields.signed) (unsigned $unsigned), publisher DienerTech LLC, timestamped."

if (-not $Install) {
    Write-Host "Not installed. To install it: scripts\Sign-LocalLayer.ps1 -Install, or copy $stage to Program Files and run scripts\Install-Layer.ps1 -ManifestPath on its manifest."
    return
}

# --- Install: copy into Program Files and register it (one administrator prompt).
$installScript = Join-Path $PSScriptRoot "Install-Layer.ps1"
$quote = { param([string]$Text) "'" + $Text.Replace("'", "''") + "'" }
$command = "`$ErrorActionPreference = 'Stop'; " +
           "New-Item -ItemType Directory -Force $(& $quote $InstallDir) | Out-Null; " +
           "Copy-Item -Path $(& $quote (Join-Path $stage '*')) -Destination $(& $quote $InstallDir) -Recurse -Force; " +
           "& $(& $quote $installScript) -ManifestPath $(& $quote (Join-Path $InstallDir $LayerManifest))"
# Encoded: Start-Process quotes no arguments, and the paths have spaces.
$encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($command))
$process = Start-Process -FilePath powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
    -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand", $encoded)
$installedDll = Join-Path $InstallDir $LayerDll
$installedHash = if (Test-Path -LiteralPath $installedDll) { (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash } else { "" }
$registered = (Get-ItemProperty -Path "HKLM:\Software\Khronos\OpenXR\1\ApiLayers\Implicit" -ErrorAction SilentlyContinue).PSObject.Properties |
    Where-Object { $_.Name -eq (Join-Path $InstallDir $LayerManifest) -and $_.Value -eq 0 }
$fields.installed = $installedDll
if ($process.ExitCode -ne 0 -or $installedHash -ne $fields.signed -or -not $registered) {
    Write-Ledger "install failed (exit $($process.ExitCode))" $fields
    throw "Installing into '$InstallDir' failed: exit code $($process.ExitCode), installed SHA256 '$installedHash', registered: $([bool]$registered)."
}
Write-Ledger "installed" $fields
Write-Host "Installed and registered: $installedDll. Relaunch the VR application to load it."
