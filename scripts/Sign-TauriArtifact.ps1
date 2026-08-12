[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$FilePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

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

Import-Module ArtifactSigning -RequiredVersion "0.1.8" -ErrorAction Stop

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
