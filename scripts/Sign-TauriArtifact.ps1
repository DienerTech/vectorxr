[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$FilePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$logPath = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    Join-Path ([System.IO.Path]::GetTempPath()) "vectorxr-tauri-signing.log"
} else {
    Join-Path $env:RUNNER_TEMP "vectorxr-tauri-signing.log"
}

function Write-SigningLog {
    param([string]$Message)

    $timestamp = [DateTimeOffset]::UtcNow.ToString("o")
    "$timestamp $Message" | Add-Content -LiteralPath $logPath -Encoding utf8
}

trap {
    Write-SigningLog "FAILED: $($_.Exception.ToString())"
    throw
}

$requiredVariables = @(
    "AZURE_SIGNING_ENDPOINT",
    "AZURE_SIGNING_ACCOUNT",
    "AZURE_CERTIFICATE_PROFILE"
)

foreach ($name in $requiredVariables) {
    if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
        throw "Required signing variable '$name' is not configured."
    }
}

$resolvedPath = (Resolve-Path -LiteralPath $FilePath -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
    throw "Tauri signing target is not a file: $resolvedPath"
}

Write-SigningLog "Signing target: $resolvedPath"
Write-SigningLog "PowerShell: $($PSVersionTable.PSVersion)"
Import-Module ArtifactSigning -RequiredVersion "0.1.8" -ErrorAction Stop
Write-SigningLog "Imported ArtifactSigning 0.1.8."

$signingParameters = @{
    Endpoint                            = $env:AZURE_SIGNING_ENDPOINT
    CodeSigningAccountName             = $env:AZURE_SIGNING_ACCOUNT
    CertificateProfileName             = $env:AZURE_CERTIFICATE_PROFILE
    Files                               = $resolvedPath
    FileDigest                          = "SHA256"
    TimestampRfc3161                    = "http://timestamp.acs.microsoft.com"
    TimestampDigest                     = "SHA256"
    Description                         = "VectorXR"
    DescriptionUrl                      = "https://github.com/DienerTech/vectorxr"
    ExcludeEnvironmentCredential        = $true
    ExcludeWorkloadIdentityCredential   = $true
    ExcludeManagedIdentityCredential    = $true
    ExcludeSharedTokenCacheCredential   = $true
    ExcludeVisualStudioCredential       = $true
    ExcludeVisualStudioCodeCredential   = $true
    ExcludeAzureCliCredential           = $false
    ExcludeAzurePowerShellCredential    = $true
    ExcludeAzureDeveloperCliCredential  = $true
    ExcludeInteractiveBrowserCredential = $true
}

Write-Host "Signing Tauri artifact: $resolvedPath"
Invoke-ArtifactSigning @signingParameters
Write-SigningLog "Artifact Signing command completed."

$signature = Get-AuthenticodeSignature -LiteralPath $resolvedPath
Write-Host "$resolvedPath : $($signature.Status) : $($signature.SignerCertificate.Subject)"

if ($signature.Status -ne "Valid") {
    throw "Invalid Authenticode signature for '$resolvedPath': $($signature.Status)."
}

if ($signature.SignerCertificate.Subject -notmatch '(^|,\s*)CN=DienerTech LLC(,|$)' -or
    $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=DienerTech LLC(,|$)') {
    throw "Unexpected Authenticode publisher for '$resolvedPath': $($signature.SignerCertificate.Subject)."
}

if (-not $signature.TimeStamperCertificate) {
    throw "The Authenticode signature for '$resolvedPath' does not contain a trusted timestamp."
}

Write-SigningLog "Verified DienerTech publisher and trusted timestamp."
