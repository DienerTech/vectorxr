<#
.SYNOPSIS
Captures a few real frames from a running Frame Generation session for offline replay.

.DESCRIPTION
After -DelaySeconds, asks the VectorXR layer to write the next five real frames it generates
from (each eye's image and linear depth, with poses) to %LOCALAPPDATA%\VectorXR\captures.
Each capture takes about 2 GB at Pimax Crystal Super resolution, and the game pauses for a
second or two while it is written. Replay a capture with:

    depthxr_framegen_tests replay <capture directory> [output directory]

.EXAMPLE
.\Capture-FrameGen.ps1 -DelaySeconds 45 -Count 2 -IntervalSeconds 30
Waits 45 seconds (time to put the headset on and settle into the cockpit), captures, then
captures again 30 seconds later.
#>
[CmdletBinding()]
param(
    [int]$DelaySeconds = 45,
    [int]$Count = 1,
    [int]$IntervalSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Join-Path $env:LOCALAPPDATA "VectorXR"
$request = Join-Path $root "framegen-capture.request"
$captures = Join-Path $root "captures"

for ($i = 1; $i -le $Count; $i++) {
    $wait = if ($i -eq 1) { $DelaySeconds } else { $IntervalSeconds }
    Write-Host "Capture $i of $Count in $wait seconds: move your head around the cockpit while it runs."
    Start-Sleep -Seconds $wait
    [System.IO.File]::WriteAllText($request, "capture")
    Write-Host "Requested capture $i."
    $deadline = (Get-Date).AddSeconds(10)
    while ((Test-Path $request) -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }
    if (Test-Path $request) {
        Remove-Item $request -Force
        Write-Warning "Frame Generation did not pick up the request: is it generating frames right now?"
    }
}
Write-Host "Captures are written under $captures (the VectorXR log names each one)."
